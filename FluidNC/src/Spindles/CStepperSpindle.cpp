// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "CStepperSpindle.h"

#include "CStepperSpindleLogic.h"
#include "GCode.h"
#include "Lathe.h"
#include "LatheEncoder.h"
#include "Machine/Axes.h"
#include "Machine/MachineConfig.h"
#include "Planner.h"
#include "Protocol.h"
#include "Stepping.h"
#include "System.h"

#include <algorithm>
#include <cmath>

namespace Spindles {
    void CStepper::init() {
        is_reversable = true;
        if (config == nullptr || config->_axes == nullptr || _axis < 0 || _axis >= Machine::Axes::_numberAxis) {
            log_config_error(name() << " C-stepper axis is unavailable");
            return;
        }

        auto axis = config->_axes->_axis[_axis];
        if (axis == nullptr) {
            log_config_error(name() << " C-stepper axis is not configured");
            return;
        }
        _stepsPerRevolution = CStepperLogic::steps_per_revolution(axis->_stepsPerMm);
        // C positioning is intentionally slow and precise. Continuous spindle
        // rotation has independent limits; deriving them from axes.c would
        // incorrectly cap the spindle at 2000 deg/min / 360 = 5.56 RPM.
        _accelerationMillihzPerSec =
            CStepperLogic::acceleration_millihz_per_sec(_accelerationRpmPerSec, _stepsPerRevolution);
        if (_stepsPerRevolution == 0 || _maximumRpm <= 0.0f || _accelerationMillihzPerSec == 0) {
            log_config_error(name() << " C-stepper scale/rate/acceleration is invalid");
            return;
        }

        _current_state = SpindleState::Disable;
        _current_speed = 0;
        _commandedRpm  = 0.0f;
        _lastOperatorHeartbeatMs = millis();
        init_atc();
        config_message();
    }

    bool CStepper::canSetState(SpindleState state, float rpm, const char*& reason) const {
        reason = nullptr;
        if ((state == SpindleState::Cw || state == SpindleState::Ccw) && rpm > 0.0f &&
            (_current_state == SpindleState::Cw || _current_state == SpindleState::Ccw) && state != _current_state) {
            reason = "C-stepper spindle must stop before reversing";
            return false;
        }
        return true;
    }

    void CStepper::setState(SpindleState state, SpindleSpeed speed) {
        setStateRpm(state, static_cast<float>(speed));
    }

    void CStepper::setStateRpm(SpindleState state, float rpm) {
        if (sys.abort() && state != SpindleState::Disable) {
            return;
        }

        if (state == SpindleState::Disable || rpm <= 0.0f) {
            stopStream(false);
            return;
        }

        const char* reason = nullptr;
        if (!canSetState(state, rpm, reason)) {
            log_error(reason);
            return;
        }
        if (Lathe::shared_chuck_mode() == Lathe::SharedChuckMode::CPositioning) {
            log_error("Shared chuck spindle start blocked until C-axis positioning is complete");
            return;
        }

        const float clampedRpm = std::min(std::max(rpm, 0.0f), _maximumRpm);
        const uint32_t rateMillihz = CStepperLogic::step_rate_millihz(clampedRpm, _stepsPerRevolution);
        if (rateMillihz == 0) {
            stopStream(false);
            return;
        }

        protocol_cancel_disable_steppers();
        Machine::Axes::set_disable(false, false);
        if (Machine::Stepping::continuousActive()) {
            Machine::Stepping::setContinuousRate(rateMillihz);
        } else {
            const bool positive = _cwPositive ? state == SpindleState::Cw : state == SpindleState::Ccw;
            if (!Machine::Stepping::startContinuous(
                    static_cast<axis_t>(_axis), positive, rateMillihz, _accelerationMillihzPerSec)) {
                log_error(name() << " could not start the C-axis I2S pulse stream");
                protocol_disable_steppers();
                return;
            }
        }

        _current_state = state;
        _current_speed = static_cast<SpindleSpeed>(std::lround(clampedRpm));
        _commandedRpm  = clampedRpm;
        sys.set_spindle_speed(_current_speed);
        _lastOperatorHeartbeatMs = millis();
        Lathe::note_shared_chuck_spindle_state(state);
    }

    void CStepper::stopStream(bool immediate) {
        if (Machine::Stepping::continuousActive()) {
            Machine::Stepping::stopContinuous(immediate);
            if (!immediate) {
                const uint32_t started = millis();
                while (Machine::Stepping::continuousActive() && (uint32_t)(millis() - started) < 3000U) {
                    delay_ms(5);
                }
                if (Machine::Stepping::continuousActive()) {
                    Machine::Stepping::stopContinuous(true);
                }
            }
        }
        _current_state = SpindleState::Disable;
        _current_speed = 0;
        _commandedRpm  = 0.0f;
        sys.set_spindle_speed(0);
        gc_state.modal.spindle      = SpindleState::Disable;
        gc_state.spindle_speed      = 0.0f;
        gc_state.lathe_commanded_rpm = 0.0f;
        Lathe::note_shared_chuck_spindle_state(SpindleState::Disable);

        // The auxiliary stream counted every physical C pulse.  Make parser
        // and planner positions agree with that dead-reckoned motor position
        // before C positioning can be selected again.
        if (!inMotionState() && plan_get_current_block() == nullptr) {
            plan_sync_position();
            gc_sync_position();
            _positionSyncPending = false;
        } else {
            _positionSyncPending = true;
        }
        protocol_disable_steppers();
    }

    void IRAM_ATTR CStepper::setSpeedfromISR(uint32_t dev_speed) {
        // Planner segment speed updates are unrelated to this background C
        // stream.  M3/M4/S updates are applied by setStateRpm().
    }

    void CStepper::service() {
        if (_positionSyncPending && !inMotionState() && plan_get_current_block() == nullptr) {
            plan_sync_position();
            gc_sync_position();
            _positionSyncPending = false;
        }
        if (_operatorWatchdogMs == 0 || !Machine::Stepping::continuousActive()) {
            return;
        }
        if ((uint32_t)(millis() - _lastOperatorHeartbeatMs) > _operatorWatchdogMs) {
            log_error(name() << " stopped: operator link heartbeat timed out");
            stopStream(true);
        }
    }

    void CStepper::operatorHeartbeat() {
        _lastOperatorHeartbeatMs = millis();
    }

    const Lathe::SpindleFeedback& CStepper::latheFeedback() const {
        Lathe::set_encoder_commanded_rpm(static_cast<SpindleSpeed>(std::lround(_commandedRpm)));
        return Lathe::configured_spindle_feedback();
    }

    float CStepper::openLoopRpm() const {
        if (_stepsPerRevolution == 0) {
            return 0.0f;
        }
        return static_cast<float>(Machine::Stepping::continuousRateMillihz()) * 60.0f /
               (static_cast<float>(_stepsPerRevolution) * 1000.0f);
    }

    void CStepper::config_message() {
        log_info(name() << " C-stepper axis:" << Machine::Axes::axisName(static_cast<axis_t>(_axis))
                        << " Steps/rev:" << _stepsPerRevolution << " Max RPM:" << _maximumRpm
                        << " Accel RPM/s:" << _accelerationRpmPerSec
                        << " CW positive:" << (_cwPositive ? "true" : "false") << atc_info());
    }

    namespace {
        SpindleFactory::InstanceBuilder<CStepper> registration("CStepper");
    }
}
