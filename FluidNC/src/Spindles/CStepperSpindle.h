// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include "Spindle.h"

namespace Spindles {
    class CStepper : public Spindle {
    public:
        CStepper(const char* name) : Spindle(name) {}

        void init() override;
        void setState(SpindleState state, SpindleSpeed speed) override;
        void setStateRpm(SpindleState state, float rpm) override;
        bool canSetState(SpindleState state, float rpm, const char*& reason) const override;
        void setSpeedfromISR(uint32_t dev_speed) override;
        void config_message() override;
        void service() override;
        void operatorHeartbeat() override;
        const char* driveType() const override { return "C_STEPPER"; }
        float commandedRpm() const override { return _commandedRpm; }
        float openLoopRpm() const override;
        float minimumRpm() const override { return _minimumRpm; }
        float maximumRpm() override { return _maximumRpm; }
        float accelerationRpmPerSec() const override { return _accelerationRpmPerSec; }
        float decelerationRpmPerSec() const override { return _decelerationRpmPerSec; }
        bool isStopping() const override { return _stopping; }
        uint32_t stopRemainingMs() const override;
        uint32_t stopTimeouts() const override { return _stopTimeouts; }
        bool lastControlActionFailed() const override { return _lastControlActionFailed; }
        uint32_t stepsPerRevolution() const override { return _stepsPerRevolution; }
        bool positionIsDeadReckoned() const override { return true; }
        const char* cReferenceName() const override;
        bool use_delay_settings() const override { return false; }
        const Lathe::SpindleFeedback& latheFeedback() const override;

        void group(Configuration::HandlerBase& handler) override {
            handler.item("axis", _axis, 0, MAX_N_AXIS - 1);
            handler.item("cw_positive", _cwPositive);
            handler.item("minimum_rpm", _minimumRpm, 0.1f, 10000.0f);
            handler.item("maximum_rpm", _maximumRpm, 0.1f, 10000.0f);
            handler.item("acceleration_rpm_per_sec", _accelerationRpmPerSec, 0.1f, 10000.0f);
            handler.item("deceleration_rpm_per_sec", _decelerationRpmPerSec, 0.1f, 10000.0f);
            handler.item("operator_watchdog_ms", _operatorWatchdogMs, 0, 60000);
            Spindle::group(handler);
        }

    private:
        int32_t  _axis               = C_AXIS;
        bool     _cwPositive         = true;
        uint32_t _operatorWatchdogMs = 12000;
        uint32_t _stepsPerRevolution = 0;
        uint32_t _accelerationMillihzPerSec = 0;
        uint32_t _decelerationMillihzPerSec = 0;
        float    _minimumRpm         = 50.0f;
        float    _maximumRpm         = 675.0f;
        float    _accelerationRpmPerSec = 1500.0f;
        float    _decelerationRpmPerSec = 100.0f;
        float    _commandedRpm       = 0.0f;
        uint32_t _lastOperatorHeartbeatMs = 0;
        bool     _positionSyncPending = false;
        bool     _cReferenceValid     = true;
        bool     _stopping            = false;
        uint32_t _stopStartedMs       = 0;
        uint32_t _stopDeadlineMs      = 0;
        uint32_t _stopTimeouts        = 0;
        bool     _lastControlActionFailed = false;

        void stopStream(bool immediate);
        void establishStoppedCReference();
    };
}
