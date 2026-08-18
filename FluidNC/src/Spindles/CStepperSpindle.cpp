// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "CStepperSpindle.h"

#include "CStepperSpindleLogic.h"
#include "ContinuousStepperLogic.h"
#include "GCode.h"
#include "Lathe.h"
#include "LatheEncoder.h"
#include "Machine/Axes.h"
#include "Machine/MachineConfig.h"
#include "MotionControl.h"
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
        _decelerationMillihzPerSec =
            CStepperLogic::acceleration_millihz_per_sec(_decelerationRpmPerSec, _stepsPerRevolution);
        if (_stepsPerRevolution == 0 || _minimumRpm <= 0.0f || _maximumRpm <= 0.0f ||
            _minimumRpm > _maximumRpm || _accelerationMillihzPerSec == 0 || _decelerationMillihzPerSec == 0) {
            log_config_error(name() << " C-stepper scale/rate/acceleration is invalid");
            return;
        }

        _current_state = SpindleState::Disable;
        _current_speed = 0;
        _commandedRpm  = 0.0f;
        _stopping      = false;
        _stopStartedMs = 0;
        _stopDeadlineMs = 0;
        _cReferenceValid = true;
        _lastOperatorHeartbeatMs = millis();
        init_atc();
        config_message();
    }

    bool CStepper::canSetState(SpindleState state, float rpm, const char*& reason) const {
        reason = nullptr;
        if (state == SpindleState::Cw || state == SpindleState::Ccw) {
            if (rpm == 0.0f) {
                return true;
            }
            if (!std::isfinite(rpm) || rpm < _minimumRpm) {
                reason = "C-stepper spindle RPM is below minimum_rpm";
                return false;
            }
            if (!CStepperLogic::rpm_is_commandable(rpm, _minimumRpm, _maximumRpm)) {
                reason = "C-stepper spindle RPM exceeds maximum_rpm";
                return false;
            }
            if (_stopping) {
                reason = "C-stepper spindle is still stopping";
                return false;
            }
        }
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
        _lastControlActionFailed = false;
        if (sys.abort() && state != SpindleState::Disable) {
            return;
        }

        if (state == SpindleState::Disable || rpm <= 0.0f) {
            stopStream(sys.abort());
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

        const uint32_t rateMillihz = CStepperLogic::step_rate_millihz(rpm, _stepsPerRevolution);
        if (rateMillihz == 0) {
            stopStream(false);
            return;
        }

        protocol_cancel_disable_steppers();
        Machine::Axes::set_disable(false, false);
        if (Machine::Stepping::continuousActive()) {
            if (!Machine::Stepping::setContinuousRate(rateMillihz)) {
                log_error(name() << " rejected the C-axis rate: combined planner/I2S capacity exceeded");
                _lastControlActionFailed = true;
                return;
            }
        } else {
            const bool positive = _cwPositive ? state == SpindleState::Cw : state == SpindleState::Ccw;
            if (!Machine::Stepping::startContinuous(
                    static_cast<axis_t>(_axis), positive, rateMillihz, _accelerationMillihzPerSec)) {
                log_error(name() << " could not start the planner-integrated C scheduler; planner must be idle and capacity available");
                _lastControlActionFailed = true;
                protocol_disable_steppers();
                return;
            }
        }

        _cReferenceValid = false;
        _current_state = state;
        _current_speed = static_cast<SpindleSpeed>(std::lround(rpm));
        _commandedRpm  = rpm;
        sys.set_spindle_speed(_current_speed);
        _lastOperatorHeartbeatMs = millis();
        Lathe::note_shared_chuck_spindle_state(state);
    }

    void CStepper::stopStream(bool immediate) {
        if (_stopping) {
            if (immediate) {
                Machine::Stepping::stopContinuous(true);
            }
            return;
        }

        bool stopTimedOut = false;
        bool stopFaulted  = false;
        if (Machine::Stepping::continuousActive()) {
            const uint32_t startingRate = Machine::Stepping::continuousRateMillihz();
            const uint32_t timeoutMs = Machine::ContinuousStepperLogic::stop_timeout_ms(
                startingRate, _decelerationMillihzPerSec);
            _stopping       = !immediate;
            _stopStartedMs  = millis();
            _stopDeadlineMs = timeoutMs;
            _commandedRpm   = 0.0f;
            sys.set_spindle_speed(0);
            Machine::Stepping::stopContinuous(immediate, _decelerationMillihzPerSec);
            if (!immediate) {
                log_info(name() << " stopping from " << openLoopRpm() << " RPM; deadline " << timeoutMs << " ms");
                while (Machine::Stepping::continuousActive() &&
                       (uint32_t)(millis() - _stopStartedMs) < timeoutMs) {
                    Machine::Stepping::serviceContinuous();
                    protocol_exec_rt_system();
                    stopFaulted = Machine::Stepping::takeContinuousFault();
                    const bool safetyState = state_is(State::SafetyDoor) || state_is(State::Alarm) ||
                                             state_is(State::ConfigAlarm) || state_is(State::Critical);
                    const bool watchdogExpired = _operatorWatchdogMs != 0 &&
                        (uint32_t)(millis() - _lastOperatorHeartbeatMs) > _operatorWatchdogMs;
                    if (sys.abort() || safetyState || watchdogExpired || stopFaulted) {
                        Machine::Stepping::stopContinuous(true);
                        break;
                    }
                    delay_ms(5);
                }
                if (Machine::Stepping::continuousActive()) {
                    stopTimedOut = true;
                    ++_stopTimeouts;
                    Machine::Stepping::stopContinuous(true);
                }
            }
        }
        _stopping       = false;
        _stopStartedMs  = 0;
        _stopDeadlineMs = 0;
        _current_state = SpindleState::Disable;
        _current_speed = 0;
        _commandedRpm  = 0.0f;
        sys.set_spindle_speed(0);
        gc_state.modal.spindle      = SpindleState::Disable;
        gc_state.spindle_speed      = 0.0f;
        gc_state.lathe_commanded_rpm = 0.0f;
        Lathe::note_shared_chuck_spindle_state(SpindleState::Disable);

        // Spindle rotation is deliberately outside the planner coordinate
        // frame. Once stopped, define that physical location as relative C0.
        establishRelativeCZero();
        protocol_disable_steppers();
        if (stopTimedOut) {
            _lastControlActionFailed = true;
            log_error(name() << " graceful stop exceeded its calculated deadline");
            mc_critical(ExecAlarm::SpindleControl);
        } else if (stopFaulted) {
            _lastControlActionFailed = true;
            log_error(name() << " stopped during deceleration: C scheduler or I2S transport fault");
            mc_critical(ExecAlarm::SpindleControl);
        }
    }

    void CStepper::establishRelativeCZero() {
        if (!inMotionState() && plan_get_current_block() == nullptr) {
            Machine::Stepping::setSteps(static_cast<axis_t>(_axis), 0);
            plan_sync_position();
            gc_sync_position();
            _positionSyncPending = false;
            _cReferenceValid     = true;
        } else {
            _positionSyncPending = true;
            _cReferenceValid     = false;
        }
    }

    void IRAM_ATTR CStepper::setSpeedfromISR(uint32_t dev_speed) {
        // Planner segment speed updates are unrelated to this background C
        // stream.  M3/M4/S updates are applied by setStateRpm().
    }

    void CStepper::service() {
        if (Machine::Stepping::takeContinuousFault()) {
            log_error(name() << " stopped: C scheduler or I2S transport fault");
            _lastControlActionFailed = true;
            stopStream(true);
            mc_critical(ExecAlarm::SpindleControl);
            return;
        }
        Machine::Stepping::serviceContinuous();
        if (_positionSyncPending && !inMotionState() && plan_get_current_block() == nullptr) {
            establishRelativeCZero();
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

    uint32_t CStepper::stopRemainingMs() const {
        if (!_stopping || _stopDeadlineMs == 0) {
            return 0;
        }
        const uint32_t elapsed = millis() - _stopStartedMs;
        return elapsed >= _stopDeadlineMs ? 0 : _stopDeadlineMs - elapsed;
    }

    const char* CStepper::cReferenceName() const {
        if (_current_state == SpindleState::Cw || _current_state == SpindleState::Ccw) {
            return "ROTATING";
        }
        if (_positionSyncPending) {
            return "PENDING_RELATIVE_ZERO";
        }
        return _cReferenceValid ? "RELATIVE_ZERO" : "INVALID";
    }

    void CStepper::config_message() {
        log_info(name() << " C-stepper axis:" << Machine::Axes::axisName(static_cast<axis_t>(_axis))
                        << " Steps/rev:" << _stepsPerRevolution << " RPM:" << _minimumRpm << "-" << _maximumRpm
                        << " Accel RPM/s:" << _accelerationRpmPerSec << " Decel RPM/s:" << _decelerationRpmPerSec
                        << " CW positive:" << (_cwPositive ? "true" : "false") << atc_info());
    }

    namespace {
        SpindleFactory::InstanceBuilder<CStepper> registration("CStepper");
    }
}
