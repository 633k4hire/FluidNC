// Copyright (c) 2021 Stefan de Bruijn
// Copyright (c) 2021 Mitch Bradley
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include "Configuration/Configurable.h"
#include "ContinuousEventScheduler.h"
#include "ThreadingStepScheduler.h"
#include "Driver/step_engine.h"
#include "System.h"

namespace Machine {
    class Stepping : public Configuration::Configurable {
    public:
        // fStepperTimer should be an integer divisor of the bus speed, i.e. of fTimers
        static const uint32_t fStepperTimer = 20000000;  // frequency of step pulse timer
    private:
        static bool    _switchedStepper;
        static int32_t _stepPulseEndTime;
        static int32_t _i2sPulseCounts;

        static const int MAX_MOTORS_PER_AXIS = 2;
        struct motor_pins_t {
            pinnum_t step_pin;
            pinnum_t dir_pin;
            bool     step_invert;
            bool     dir_invert;
            bool     blocked;
            bool     limited;
        };
        static motor_pins_t* axis_motors[MAX_N_AXIS][MAX_MOTORS_PER_AXIS];
        static axis_t        _n_active_axes;

        static void    startPulseTimer();
        static void    waitDirection();  // Wait for direction delay
        static steps_t axis_steps[MAX_N_AXIS];

        static volatile axis_t _continuousAxis;
        static AxisMask        _previousDirectionMask;
        static uint32_t        _continuousTargetRateMillihz;
        static uint32_t        _continuousCurrentRateMillihz;
        static uint32_t        _continuousAccelerationMillihzPerSec;
        static uint32_t        _continuousLastRampMs;
        static uint32_t        _continuousRampRemainder;
        static uint32_t        _continuousPublishedRateMillihz;
        static uint32_t        _continuousPlannerPeakPulsesPerSec;

        struct continuous_rate_mailbox_t {
            volatile uint32_t sequence;
            volatile uint32_t rate_millihz;
            volatile uint32_t whole_ticks;
            volatile uint32_t remainder;
            volatile uint32_t denominator;
            volatile uint32_t ramping;
        };

        static continuous_rate_mailbox_t             _continuousRateMailbox;
        static ContinuousEventScheduler::IntervalState _continuousInterval;
        static volatile bool                         _continuousOwner;
        static volatile bool                         _continuousSchedulerActive;
        static volatile bool                         _continuousPlannerStartPending;
        static volatile bool                         _continuousPulseDue;
        static volatile bool                         _continuousFaulted;
        static volatile bool                         _continuousFaultPending;
        static volatile bool                         _continuousStoppedAck;
        static volatile uint32_t                     _continuousAppliedRateMillihz;
        static volatile uint32_t                     _continuousPulseCounter;
        static uint32_t                              _continuousAppliedSequence;
        static bool                                  _plannerSchedulerActive;
        static uint32_t                              _plannerPeriodTicks;
        static uint32_t                              _plannerTicksUntilEvent;
        static uint32_t                              _schedulerLastIntervalTicks;
        static bool                                  _plannerDeferredForContinuous;
        static int32_t                               _plannerDeferredAdjustmentTicks;
        static ThreadingStepScheduler::State          _threadingState;
        static uint32_t                               _threadingBlockToken;
        static volatile bool                          _threadingPassActive;
        static volatile bool                          _threadingInvalidated;
        static volatile bool                          _threadingInvalidatedPending;

        static volatile uint32_t _continuousFaultReason;
        static volatile uint32_t _continuousFaultCount;
        static volatile uint32_t _continuousSchedulerCalls;
        static volatile uint32_t _continuousPlannerStarts;
        static volatile uint32_t _continuousPlannerResets;
        static volatile uint32_t _continuousMergedPulses;
        static volatile uint32_t _continuousStandalonePulses;
        static volatile uint32_t _continuousPlannerEndFallbackPulses;
        static volatile uint32_t _continuousPlannerDelays;
        static volatile uint32_t _continuousPlannerAdvances;
        static volatile uint32_t _continuousLastIntervalTicks;
        static volatile uint32_t _continuousMinIntervalTicks;
        static volatile uint32_t _continuousMaxIntervalTicks;
        static volatile uint32_t _continuousLastPhysicalIntervalFrames;
        static volatile uint32_t _continuousMinPhysicalIntervalFrames;
        static volatile uint32_t _continuousMaxPhysicalIntervalFrames;
        static volatile uint32_t _continuousPhysicalIntervalCount;
        static volatile uint32_t _continuousLastPulseTimelineFrames;
        static volatile bool     _continuousHasPulseTimeline;
        static volatile uint32_t _continuousFaultPulseCount;
        static volatile uint32_t _continuousFaultAppliedRateMillihz;
        static volatile uint32_t _continuousFaultRateSequence;
        static volatile uint32_t _continuousFaultSchedulerIntervalTicks;
        static volatile uint32_t _continuousFaultPlannerPeriodTicks;
        static volatile uint32_t _continuousFaultPlannerTicksUntilEvent;
        static volatile uint32_t _continuousFaultTicksUntilStep;
        static volatile uint32_t _continuousFaultPlannerResetCount;
        static volatile uint32_t _continuousFaultFlags;

        static bool     continuousSchedulerPulse();
        static bool     emitContinuousPulse();
        static void     publishContinuousRate(uint32_t rate_millihz, bool ramping);
        static bool     readContinuousRateCommand(ContinuousEventScheduler::RateCommand& command, uint32_t& sequence);
        static void     finishContinuousStop();
        static void     resetContinuousDiagnostics();
        static void     recordContinuousInterval(uint32_t interval_ticks);
        static void     recordContinuousPhysicalPulse();
        static void     captureContinuousFault(uint32_t reason);
        static void     latchContinuousFault(uint32_t reason);
        static uint32_t plannerPeakPulsesPerSecond(axis_t excluded_axis);

        static step_engine_t* step_engine;

    public:
        enum class ContinuousFaultReason : uint32_t {
            None = 0,
            UnexpectedPlannerC,
            COutputUnavailable,
            InvalidRateCommand,
            TransportFault,
            InvalidAppliedRate,
            MissingMergedPulse,
            MissingStandalonePulse,
            InvalidThreadingCommand,
            ThreadingRateChanged,
            ThreadingDecisionMismatch,
            ThreadingLostContinuousC,
        };

        struct ContinuousDiagnostics {
            ContinuousFaultReason faultReason = ContinuousFaultReason::None;
            uint32_t faultCount = 0;
            uint32_t schedulerCalls = 0;
            uint32_t plannerStarts = 0;
            uint32_t plannerResets = 0;
            uint32_t mergedPulses = 0;
            uint32_t standalonePulses = 0;
            uint32_t plannerEndFallbackPulses = 0;
            uint32_t plannerDelays = 0;
            uint32_t plannerAdvances = 0;
            uint32_t lastIntervalTicks = 0;
            uint32_t minIntervalTicks = 0;
            uint32_t maxIntervalTicks = 0;
            uint32_t lastPhysicalIntervalFrames = 0;
            uint32_t minPhysicalIntervalFrames = 0;
            uint32_t maxPhysicalIntervalFrames = 0;
            uint32_t physicalIntervalCount = 0;
            uint32_t faultPulseCount = 0;
            uint32_t faultAppliedRateMillihz = 0;
            uint32_t faultRateSequence = 0;
            uint32_t faultSchedulerIntervalTicks = 0;
            uint32_t faultPlannerPeriodTicks = 0;
            uint32_t faultPlannerTicksUntilEvent = 0;
            uint32_t faultTicksUntilStep = 0;
            uint32_t faultPlannerResetCount = 0;
            bool faultOwner = false;
            bool faultSchedulerActive = false;
            bool faultPlannerActive = false;
            bool faultPlannerDue = false;
            bool faultContinuousDue = false;
            bool faultStepperAwake = false;
            bool faultMotorPresent = false;
            bool faultMotorBlocked = false;
            bool faultMotorLimited = false;
        };

        enum stepper_id_t {
            TIMED = 0,
            RMT_ENGINE,
            I2S_STATIC,
            I2S_STREAM,
        };

        Stepping() = default;

        // _segments is the number of entries in the step segment buffer between the step execution algorithm
        // and the planner blocks. Each segment is set of steps executed at a constant velocity over a
        // fixed time defined by ACCELERATION_TICKS_PER_SECOND. They are computed such that the planner
        // block velocity profile is traced exactly. The size of this buffer governs how much step
        // execution lead time there is for other processes to run.  The latency for a feedhold or other
        // override is roughly 10 ms times _segments.

        static int32_t _segments;

        static uint32_t _idleMsecs;
        static uint32_t _pulseUsecs;
        static uint32_t _directionDelayUsecs;
        static uint32_t _disableDelayUsecs;

        static uint32_t _engine;

        // Interfaces to stepping engine
        static void init();

        static steps_t getSteps(axis_t axis) { return axis_steps[axis]; }
        static void    setSteps(axis_t axis, steps_t steps) { axis_steps[axis] = steps; }

        static void assignMotor(axis_t axis, motor_t motor, pinnum_t step_pin, bool step_invert, pinnum_t dir_pin, bool dir_invert);

        static void reset();         // Reset planner state and stop continuous C immediately
        static void resetPlanner();  // Reset finite planner state without disturbing continuous C
        static void emergencyStop(); // Immediately disarm only the continuous C lane
        static void beginLowLatency();
        static void endLowLatency();

        static void step(AxisMask step_mask, AxisMask dir_mask);
        static void unstep();

        // Used to stop a motor quickly when a limit switch is hit
        static bool* limit_var(axis_t axis, motor_t motor);
        static void  limit(axis_t axis, motor_t motor);
        static void  unlimit(axis_t axis, motor_t motor);

        // Used to stop a motor during ganged homint
        static void block(axis_t axis, motor_t motor);
        static void unblock(axis_t axis, motor_t motor);

        static uint32_t maxPulsesPerSec();

        // Starts a continuous pulse lane on one configured I2S axis. The lane
        // shares the proven planner callback and Stepping::step() output word;
        // it does not use a second timer or an I2S-frame overlay.
        static bool startContinuous(axis_t axis, bool positive, uint32_t rate_millihz, uint32_t acceleration_millihz_per_sec);
        static bool setContinuousRate(uint32_t rate_millihz);
        static void stopContinuous(bool immediate, uint32_t deceleration_millihz_per_sec = 0);
        static void serviceContinuous();
        static bool continuousActive();
        static uint32_t continuousRateMillihz();
        static uint32_t continuousTargetRateMillihz();
        static uint32_t continuousPulseCount();
        static bool threadingPassActive();
        static void IRAM_ATTR invalidateThreading();
        static bool takeThreadingInvalidated();
        static bool continuousFaulted();
        static bool takeContinuousFault();
        static ContinuousDiagnostics continuousDiagnostics();
        static const char* continuousFaultReasonName(ContinuousFaultReason reason);

        static AxisMask direction_mask;

        // Timers
        static void setTimerPeriod(uint32_t timerTicks);
        static void startTimer();
        static void stopTimer();

        // Configuration system helpers:
        void group(Configuration::HandlerBase& handler) override;
        void afterParse() override;
    };
}
extern const EnumItem stepTypes[];
