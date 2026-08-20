// #include "Driver/i2s_out.h"
#include "EnumItem.h"
#include "Stepping.h"
#include "Machine/MachineConfig.h"  // config
#include "Driver/i2s_out.h"
#include "ContinuousStepperLogic.h"

#include <algorithm>
#include <cmath>
#include <limits>

step_engine_t* step_engines = NULL;  // Linked list of stepping engines

step_engine_t* find_engine(const char* name) {
    for (step_engine_t* p = step_engines; p; p = p->link) {
        // Initial substring match, handles different forms of I2S
        if (strncmp(name, p->name, strlen(p->name)) == 0) {
            return p;
        }
    }
    return NULL;
}

namespace Machine {

    // fStepperTimer should be an integer divisor of the bus speed, i.e. of fTimers
    const int ticksPerMicrosecond = Stepping::fStepperTimer / 1000000;

    uint32_t Stepping::_engine = DEFAULT_STEPPING_ENGINE;

    AxisMask Stepping::direction_mask = 0;

    bool    Stepping::_switchedStepper = false;
    int32_t Stepping::_segments        = 12;

    uint32_t Stepping::_idleMsecs           = 255;
    uint32_t Stepping::_pulseUsecs          = 4;
    uint32_t Stepping::_directionDelayUsecs = 0;
    uint32_t Stepping::_disableDelayUsecs   = 0;

    step_engine_t* Stepping::step_engine;

    const EnumItem stepTypes[] = { { Stepping::TIMED, "Timed" },
#if MAX_N_RMT
                                   { Stepping::RMT_ENGINE, "RMT" },
#endif
#if MAX_N_I2SO
                                   { Stepping::I2S_STATIC, "I2S_STATIC" },
                                   { Stepping::I2S_STREAM, "I2S_STREAM" },
#endif
                                   EnumItem(DEFAULT_STEPPING_ENGINE) };

    void Stepping::afterParse() {
        const char* name = stepTypes[_engine].name;
        step_engine      = find_engine(name);
        Assert(step_engine, "Cannot find stepping engine for %s", name);
#if MAX_N_I2SO
        Assert(strcmp("I2S", name) || config->_i2so, "I2SO bus must be configured for this stepping type");
#endif
    }

    void Stepping::init() {
        log_info("Stepping:" << stepTypes[_engine].name << " Pulse:" << _pulseUsecs << "us Dsbl Delay:" << _disableDelayUsecs
                             << "us Dir Delay:" << _directionDelayUsecs << "us Idle Delay:" << _idleMsecs << "ms");

        uint32_t actual = step_engine->init(_directionDelayUsecs, _pulseUsecs, fStepperTimer, continuousSchedulerPulse);
        if (actual != _pulseUsecs) {
            log_warn("stepping/pulse_us adjusted to " << actual);
        }

        // Register pulse_func with the I2S subsystem
        // This could be done via the linker.
        //        i2s_out_set_pulse_callback(Stepper::pulse_func);

        Stepper::init();
    }

}

Stepping::motor_pins_t* Stepping::axis_motors[MAX_N_AXIS][MAX_MOTORS_PER_AXIS] = { nullptr };

void Stepping::assignMotor(axis_t axis, motor_t motor, pinnum_t step_pin, bool step_invert, pinnum_t dir_pin, bool dir_invert) {
    step_pin = step_engine->init_step_pin(step_pin, step_invert);

    auto m                   = new motor_pins_t;
    axis_motors[axis][motor] = m;
    m->step_pin              = step_pin;
    m->step_invert           = step_invert;
    m->dir_pin               = dir_pin;
    m->dir_invert            = dir_invert;
    m->blocked               = false;
    m->limited               = false;

    if (motor == 0 && dir_invert) {
        set_bitnum(direction_mask, axis);
    }
}

steps_t Stepping::axis_steps[MAX_N_AXIS] = { 0 };
volatile axis_t Stepping::_continuousAxis = INVALID_AXIS;
AxisMask        Stepping::_previousDirectionMask = 65535;
uint32_t        Stepping::_continuousTargetRateMillihz = 0;
uint32_t        Stepping::_continuousCurrentRateMillihz = 0;
uint32_t        Stepping::_continuousAccelerationMillihzPerSec = 0;
uint32_t        Stepping::_continuousLastRampMs = 0;
uint32_t        Stepping::_continuousRampRemainder = 0;
uint32_t        Stepping::_continuousPublishedRateMillihz = 0;
uint32_t        Stepping::_continuousPlannerPeakPulsesPerSec = 0;
Stepping::continuous_rate_mailbox_t Stepping::_continuousRateMailbox = {};
ContinuousEventScheduler::IntervalState Stepping::_continuousInterval = {};
volatile bool     Stepping::_continuousOwner = false;
volatile bool     Stepping::_continuousSchedulerActive = false;
volatile bool     Stepping::_continuousPlannerStartPending = false;
volatile bool     Stepping::_continuousPulseDue = false;
volatile bool     Stepping::_continuousFaulted = false;
volatile bool     Stepping::_continuousFaultPending = false;
volatile bool     Stepping::_continuousStoppedAck = false;
volatile uint32_t Stepping::_continuousAppliedRateMillihz = 0;
volatile uint32_t Stepping::_continuousPulseCounter = 0;
uint32_t          Stepping::_continuousAppliedSequence = 0;
bool              Stepping::_plannerSchedulerActive = false;
uint32_t          Stepping::_plannerPeriodTicks = 100;
uint32_t          Stepping::_plannerTicksUntilEvent = 0;
uint32_t          Stepping::_schedulerLastIntervalTicks = 100;
bool              Stepping::_plannerDeferredForContinuous = false;
int32_t           Stepping::_plannerDeferredAdjustmentTicks = 0;
ThreadingStepScheduler::State Stepping::_threadingState = {};
uint32_t          Stepping::_threadingBlockToken = std::numeric_limits<uint32_t>::max();
volatile bool     Stepping::_threadingPassActive = false;
volatile bool     Stepping::_threadingInvalidated = false;
volatile bool     Stepping::_threadingInvalidatedPending = false;
volatile uint32_t Stepping::_continuousFaultReason = 0;
volatile uint32_t Stepping::_continuousFaultCount = 0;
volatile uint32_t Stepping::_continuousSchedulerCalls = 0;
volatile uint32_t Stepping::_continuousPlannerStarts = 0;
volatile uint32_t Stepping::_continuousPlannerResets = 0;
volatile uint32_t Stepping::_continuousMergedPulses = 0;
volatile uint32_t Stepping::_continuousStandalonePulses = 0;
volatile uint32_t Stepping::_continuousPlannerEndFallbackPulses = 0;
volatile uint32_t Stepping::_continuousPlannerDelays = 0;
volatile uint32_t Stepping::_continuousPlannerAdvances = 0;
volatile uint32_t Stepping::_continuousLastIntervalTicks = 0;
volatile uint32_t Stepping::_continuousMinIntervalTicks = 0;
volatile uint32_t Stepping::_continuousMaxIntervalTicks = 0;
volatile uint32_t Stepping::_continuousLastPhysicalIntervalFrames = 0;
volatile uint32_t Stepping::_continuousMinPhysicalIntervalFrames = 0;
volatile uint32_t Stepping::_continuousMaxPhysicalIntervalFrames = 0;
volatile uint32_t Stepping::_continuousPhysicalIntervalCount = 0;
volatile uint32_t Stepping::_continuousLastPulseTimelineFrames = 0;
volatile bool     Stepping::_continuousHasPulseTimeline = false;
volatile uint32_t Stepping::_continuousFaultPulseCount = 0;
volatile uint32_t Stepping::_continuousFaultAppliedRateMillihz = 0;
volatile uint32_t Stepping::_continuousFaultRateSequence = 0;
volatile uint32_t Stepping::_continuousFaultSchedulerIntervalTicks = 0;
volatile uint32_t Stepping::_continuousFaultPlannerPeriodTicks = 0;
volatile uint32_t Stepping::_continuousFaultPlannerTicksUntilEvent = 0;
volatile uint32_t Stepping::_continuousFaultTicksUntilStep = 0;
volatile uint32_t Stepping::_continuousFaultPlannerResetCount = 0;
volatile uint32_t Stepping::_continuousFaultFlags = 0;

bool* Stepping::limit_var(axis_t axis, motor_t motor) {
    auto m = axis_motors[axis][motor];
    return m ? &(m->limited) : nullptr;
}

void Stepping::block(axis_t axis, motor_t motor) {
    auto m = axis_motors[axis][motor];
    if (m) {
        m->blocked = true;
    }
}

void Stepping::unblock(axis_t axis, motor_t motor) {
    auto m = axis_motors[axis][motor];
    if (m) {
        m->blocked = false;
    }
}

void Stepping::limit(axis_t axis, motor_t motor) {
    auto m = axis_motors[axis][motor];
    if (m) {
        m->limited = true;
    }
}
void Stepping::unlimit(axis_t axis, motor_t motor) {
    auto m = axis_motors[axis][motor];
    if (m) {
        m->limited = false;
    }
}

void IRAM_ATTR Stepping::step(AxisMask step_mask, AxisMask dir_mask) {
    const axis_t continuous_axis = _continuousAxis;
    const bool continuous_owner = __atomic_load_n(&_continuousOwner, __ATOMIC_ACQUIRE);
    const bool continuous_due = continuous_owner && continuous_axis < MAX_N_AXIS &&
                                __atomic_load_n(&_continuousPulseDue, __ATOMIC_ACQUIRE);
    AxisMask continuous_mask = 0;
    if (continuous_axis < MAX_N_AXIS) {
        set_bitnum(continuous_mask, continuous_axis);
    }
    step_mask = static_cast<AxisMask>(
        ContinuousEventScheduler::merged_step_mask(step_mask, continuous_mask, continuous_due));
    dir_mask = static_cast<AxisMask>(ContinuousEventScheduler::preserve_owned_direction(
        dir_mask, _previousDirectionMask, continuous_mask, continuous_owner));
    // Set the direction pins, but optimize for the common
    // situation where the direction bits haven't changed.
    if (_previousDirectionMask == 65535) {
        // Set all the direction bits the first time
        _previousDirectionMask = ~dir_mask;
    }

    if (dir_mask != _previousDirectionMask) {
        for (axis_t axis = X_AXIS; axis < Axes::_numberAxis; axis++) {
            if (continuous_owner && axis == continuous_axis) {
                continue;
            }
            bool dir     = bitnum_is_true(dir_mask, axis);
            bool old_dir = bitnum_is_true(_previousDirectionMask, axis);
            if (dir != old_dir) {
                for (size_t motor = 0; motor < MAX_MOTORS_PER_AXIS; motor++) {
                    auto m = axis_motors[axis][motor];
                    if (m) {
                        step_engine->set_dir_pin(m->dir_pin, dir ^ m->dir_invert);
                    }
                }
            }
            // Some stepper drivers need time between changing direction and doing a pulse.
            step_engine->finish_dir();
        }
        // Preserve the continuously-owned C direction bit while accepting
        // planner direction changes for every other axis.
        if (continuous_owner && continuous_axis < MAX_N_AXIS) {
            const bool continuous_dir = bitnum_is_true(_previousDirectionMask, continuous_axis);
            _previousDirectionMask = dir_mask;
            if (continuous_dir) set_bitnum(_previousDirectionMask, continuous_axis);
            else clear_bitnum(_previousDirectionMask, continuous_axis);
        } else {
            _previousDirectionMask = dir_mask;
        }
    }

    step_engine->start_step();

    // Turn on step pulses for motors that are supposed to step now
    for (axis_t axis = X_AXIS; axis < Axes::_numberAxis; axis++) {
        if (bitnum_is_true(step_mask, axis)) {
            if (continuous_owner && axis == continuous_axis) {
                if (!continuous_due) {
                    latchContinuousFault(static_cast<uint32_t>(ContinuousFaultReason::UnexpectedPlannerC));
                    continue;
                }

                bool emitted = false;
                for (size_t motor = 0; motor < MAX_MOTORS_PER_AXIS; motor++) {
                    auto m = axis_motors[axis][motor];
                    if (m && !m->blocked && !m->limited) {
                        step_engine->set_step_pin(m->step_pin, !m->step_invert);
                        emitted = true;
                    }
                }
                if (emitted) {
                    const auto increment = bitnum_is_true(_previousDirectionMask, axis) ? -1 : 1;
                    axis_steps[axis] += increment;
                    __atomic_add_fetch(&_continuousPulseCounter, 1U, __ATOMIC_RELAXED);
                    recordContinuousPhysicalPulse();
                } else {
                    latchContinuousFault(static_cast<uint32_t>(ContinuousFaultReason::COutputUnavailable));
                }
                continue;
            }
            auto increment = bitnum_is_true(dir_mask, axis) ? -1 : 1;
            axis_steps[axis] += increment;
            for (size_t motor = 0; motor < MAX_MOTORS_PER_AXIS; motor++) {
                auto m = axis_motors[axis][motor];
                if (m && !m->blocked && !m->limited) {
                    step_engine->set_step_pin(m->step_pin, !m->step_invert);
                }
            }
        }
    }
    step_engine->finish_step();
}

// Turn all stepper pins off
void IRAM_ATTR Stepping::unstep() {
    if (step_engine->start_unstep()) {
        return;
    }
    for (axis_t axis = X_AXIS; axis < Axes::_numberAxis; axis++) {
        for (size_t motor = 0; motor < MAX_MOTORS_PER_AXIS; motor++) {
            auto m = axis_motors[axis][motor];
            if (m) {
                step_engine->set_step_pin(m->step_pin, m->step_invert);
            }
        }
    }
    step_engine->finish_unstep();
}

void Stepping::resetPlanner() {
    __atomic_add_fetch(&_continuousPlannerResets, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&_continuousPlannerStartPending, false, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousPulseDue, false, __ATOMIC_RELEASE);
    _plannerSchedulerActive = false;
    _plannerTicksUntilEvent = 0;
    _plannerDeferredForContinuous = false;
    _plannerDeferredAdjustmentTicks = 0;
    ThreadingStepScheduler::reset(_threadingState);
    _threadingBlockToken = std::numeric_limits<uint32_t>::max();
    __atomic_store_n(&_threadingPassActive, false, __ATOMIC_RELEASE);
    __atomic_store_n(&_threadingInvalidated, false, __ATOMIC_RELEASE);
    __atomic_store_n(&_threadingInvalidatedPending, false, __ATOMIC_RELEASE);
}

void Stepping::reset() {
    emergencyStop();
    resetPlanner();
}
void Stepping::beginLowLatency() {}
void Stepping::endLowLatency() {}

// Called only from Stepper::pulse_func when a new segment is loaded
// The argument is in units of ticks of the timer that generates ISRs
void IRAM_ATTR Stepping::setTimerPeriod(uint32_t ticks) {
    _plannerPeriodTicks = ticks ? ticks : 1;
    if (!__atomic_load_n(&_continuousSchedulerActive, __ATOMIC_ACQUIRE)) {
        step_engine->set_timer_ticks(_plannerPeriodTicks);
    }
}

// Called only from Stepper::wake_up which is not used in ISR context
void Stepping::startTimer() {
    if (__atomic_load_n(&_continuousOwner, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&_continuousSchedulerActive, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&_continuousPlannerStartPending, true, __ATOMIC_RELEASE);
    }
    step_engine->start_timer();
}

// Called only from Stepper::stop_stepping, used in both ISR and foreground contexts
void IRAM_ATTR Stepping::stopTimer() {
    step_engine->stop_timer();
}

void Stepping::group(Configuration::HandlerBase& handler) {
    handler.item("engine", _engine, stepTypes);
    handler.item("idle_ms", _idleMsecs, 0, 10000000);  // full range
    handler.item("pulse_us", _pulseUsecs, 0, 30);
    handler.item("dir_delay_us", _directionDelayUsecs, 0, 10);
    handler.item("disable_delay_us", _disableDelayUsecs, 0, 1000000);  // max 1 second
    handler.item("segments", _segments, 6, 20);
}

uint32_t Stepping::maxPulsesPerSec() {
    return step_engine->max_pulses_per_sec();
}

void Stepping::publishContinuousRate(uint32_t rate_millihz, bool ramping) {
    const auto command = ContinuousEventScheduler::make_rate_command(fStepperTimer, rate_millihz, ramping);
    if (!command.valid) {
        latchContinuousFault(static_cast<uint32_t>(ContinuousFaultReason::InvalidRateCommand));
        return;
    }

    uint32_t sequence = __atomic_load_n(&_continuousRateMailbox.sequence, __ATOMIC_RELAXED);
    if (sequence & 1U) {
        ++sequence;
    }
    __atomic_store_n(&_continuousRateMailbox.sequence, sequence + 1U, __ATOMIC_SEQ_CST);
    _continuousRateMailbox.rate_millihz = command.rate_millihz;
    _continuousRateMailbox.whole_ticks  = command.whole_ticks;
    _continuousRateMailbox.remainder    = command.remainder;
    _continuousRateMailbox.denominator  = command.denominator;
    _continuousRateMailbox.ramping      = command.ramping ? 1U : 0U;
    __atomic_store_n(&_continuousRateMailbox.sequence, sequence + 2U, __ATOMIC_SEQ_CST);
    _continuousPublishedRateMillihz = rate_millihz;
}

bool IRAM_ATTR Stepping::readContinuousRateCommand(ContinuousEventScheduler::RateCommand& command, uint32_t& sequence) {
    const uint32_t first = __atomic_load_n(&_continuousRateMailbox.sequence, __ATOMIC_ACQUIRE);
    if (first & 1U) {
        return false;
    }

    command.rate_millihz = _continuousRateMailbox.rate_millihz;
    command.whole_ticks  = _continuousRateMailbox.whole_ticks;
    command.remainder    = _continuousRateMailbox.remainder;
    command.denominator  = _continuousRateMailbox.denominator;
    command.ramping      = _continuousRateMailbox.ramping != 0;
    command.valid        = true;
    const uint32_t second = __atomic_load_n(&_continuousRateMailbox.sequence, __ATOMIC_ACQUIRE);
    if (first != second || (second & 1U)) {
        return false;
    }
    sequence = second;
    return true;
}

bool IRAM_ATTR Stepping::emitContinuousPulse() {
    if (_continuousAxis >= MAX_N_AXIS ||
        !__atomic_load_n(&_continuousOwner, __ATOMIC_ACQUIRE)) {
        return false;
    }

    AxisMask continuous_mask = 0;
    set_bitnum(continuous_mask, _continuousAxis);
    const uint32_t pulses_before =
        __atomic_load_n(&_continuousPulseCounter, __ATOMIC_ACQUIRE);
    __atomic_store_n(&_continuousPulseDue, true, __ATOMIC_RELEASE);
    step(continuous_mask, _previousDirectionMask == 65535 ? direction_mask : _previousDirectionMask);
    unstep();
    __atomic_store_n(&_continuousPulseDue, false, __ATOMIC_RELEASE);
    const uint32_t pulses_after =
        __atomic_load_n(&_continuousPulseCounter, __ATOMIC_ACQUIRE);
    return ContinuousEventScheduler::pulse_was_emitted(pulses_before, pulses_after);
}

bool IRAM_ATTR Stepping::continuousSchedulerPulse() {
    if (!__atomic_load_n(&_continuousSchedulerActive, __ATOMIC_ACQUIRE)) {
        return Stepper::pulse_func();
    }
    ++_continuousSchedulerCalls;

    const uint32_t elapsed_ticks = _schedulerLastIntervalTicks ? _schedulerLastIntervalTicks : 1U;
    bool owner = __atomic_load_n(&_continuousOwner, __ATOMIC_ACQUIRE);
    if (owner && i2s_out_continuous_transport_faulted()) {
        latchContinuousFault(static_cast<uint32_t>(ContinuousFaultReason::TransportFault));
        owner = false;
    }

    Stepper::ThreadingExecution threading_execution;
    bool threading_block = _plannerSchedulerActive && Stepper::threading_execution(threading_execution);

    ContinuousEventScheduler::RateCommand command;
    uint32_t command_sequence = 0;
    if (readContinuousRateCommand(command, command_sequence) && command_sequence != _continuousAppliedSequence) {
        if (__atomic_load_n(&_threadingPassActive, __ATOMIC_ACQUIRE) && threading_block &&
            command.rate_millihz != threading_execution.commanded_c_rate_millihz) {
            latchContinuousFault(static_cast<uint32_t>(ContinuousFaultReason::ThreadingRateChanged));
            owner = false;
        } else if (!ContinuousEventScheduler::apply_rate(_continuousInterval, command)) {
            latchContinuousFault(static_cast<uint32_t>(ContinuousFaultReason::InvalidAppliedRate));
            owner = false;
        } else {
            _continuousAppliedSequence = command_sequence;
            __atomic_store_n(&_continuousAppliedRateMillihz, command.rate_millihz, __ATOMIC_RELEASE);
            if (command.rate_millihz == 0) {
                __atomic_store_n(&_continuousStoppedAck, true, __ATOMIC_RELEASE);
            }
        }
    }

    if (__atomic_exchange_n(&_continuousPlannerStartPending, false, __ATOMIC_ACQ_REL)) {
        ++_continuousPlannerStarts;
        _plannerSchedulerActive = true;
        _plannerTicksUntilEvent = 0;
    } else if (_plannerSchedulerActive && !Stepper::is_awake()) {
        _plannerSchedulerActive = false;
        _plannerTicksUntilEvent = 0;
        _plannerDeferredForContinuous = false;
        _plannerDeferredAdjustmentTicks = 0;
    }

    threading_block = _plannerSchedulerActive && Stepper::threading_execution(threading_execution);
    const bool threading_invalidated = __atomic_load_n(&_threadingInvalidated, __ATOMIC_ACQUIRE);
    if (threading_block && ThreadingStepScheduler::active(_threadingState) &&
        threading_execution.block_token != _threadingBlockToken) {
        ThreadingStepScheduler::reset(_threadingState);
        __atomic_store_n(&_threadingPassActive, false, __ATOMIC_RELEASE);
    }
    if (threading_block && !threading_invalidated &&
        !ThreadingStepScheduler::active(_threadingState)) {
        ThreadingStepScheduler::Command threading_command;
        threading_command.c_steps_per_revolution = threading_execution.c_steps_per_revolution;
        threading_command.z_steps_per_revolution = threading_execution.z_steps_per_revolution;
        threading_command.valid = threading_command.c_steps_per_revolution != 0 &&
                                  threading_command.z_steps_per_revolution != 0 &&
                                  threading_command.z_steps_per_revolution <= threading_command.c_steps_per_revolution &&
                                  _continuousInterval.active &&
                                  _continuousInterval.command.rate_millihz == threading_execution.commanded_c_rate_millihz;
        if (!ThreadingStepScheduler::arm(
                _threadingState, threading_command,
                __atomic_load_n(&_continuousPulseCounter, __ATOMIC_ACQUIRE))) {
            latchContinuousFault(static_cast<uint32_t>(ContinuousFaultReason::InvalidThreadingCommand));
            owner = false;
        } else {
            _threadingBlockToken = threading_execution.block_token;
            __atomic_store_n(&_threadingPassActive, true, __ATOMIC_RELEASE);
        }
    } else if (!threading_block && ThreadingStepScheduler::active(_threadingState)) {
        ThreadingStepScheduler::reset(_threadingState);
        _threadingBlockToken = std::numeric_limits<uint32_t>::max();
        __atomic_store_n(&_threadingPassActive, false, __ATOMIC_RELEASE);
    }

    const bool synchronized_threading =
        threading_block && !threading_invalidated && ThreadingStepScheduler::active(_threadingState);

    if (_plannerSchedulerActive && !synchronized_threading && _plannerTicksUntilEvent != 0) {
        ContinuousEventScheduler::elapse(_plannerTicksUntilEvent, elapsed_ticks);
    }
    if (owner && _continuousInterval.active && _continuousInterval.ticks_until_step != 0) {
        ContinuousEventScheduler::elapse(_continuousInterval.ticks_until_step, elapsed_ticks);
    }

    bool planner_due = _plannerSchedulerActive && !synchronized_threading && _plannerTicksUntilEvent == 0;
    const bool continuous_due = owner && _continuousInterval.active &&
                                _continuousInterval.ticks_until_step == 0;
    int32_t planner_phase_adjustment = 0;
    if (synchronized_threading) {
        if (!owner) {
            invalidateThreading();
            latchContinuousFault(static_cast<uint32_t>(ContinuousFaultReason::ThreadingLostContinuousC));
        } else if (continuous_due) {
            const uint32_t continuous_pulses_before =
                __atomic_load_n(&_continuousPulseCounter, __ATOMIC_ACQUIRE);
            const bool z_due = ThreadingStepScheduler::z_due_on_next_c_pulse(
                _threadingState, continuous_pulses_before);
            bool planner_continues = true;
            bool emitted = false;

            if (z_due) {
                __atomic_store_n(&_continuousPulseDue, true, __ATOMIC_RELEASE);
                planner_continues = Stepper::pulse_func();
                __atomic_store_n(&_continuousPulseDue, false, __ATOMIC_RELEASE);
                emitted = ContinuousEventScheduler::pulse_was_emitted(
                    continuous_pulses_before,
                    __atomic_load_n(&_continuousPulseCounter, __ATOMIC_ACQUIRE));
                if (!emitted && !planner_continues) {
                    emitted = emitContinuousPulse();
                    if (emitted) {
                        ++_continuousPlannerEndFallbackPulses;
                    }
                } else if (emitted) {
                    ++_continuousMergedPulses;
                }
            } else {
                emitted = emitContinuousPulse();
                if (emitted) {
                    ++_continuousStandalonePulses;
                }
            }

            if (!emitted) {
                latchContinuousFault(static_cast<uint32_t>(ContinuousFaultReason::MissingMergedPulse));
            } else {
                const uint32_t continuous_pulses_after =
                    __atomic_load_n(&_continuousPulseCounter, __ATOMIC_ACQUIRE);
                const bool retired_z = ThreadingStepScheduler::retire_c_pulse(
                    _threadingState, continuous_pulses_after);
                if (planner_continues && retired_z != z_due) {
                    latchContinuousFault(static_cast<uint32_t>(ContinuousFaultReason::ThreadingDecisionMismatch));
                }
                recordContinuousInterval(_continuousInterval.current_period_ticks);
                ContinuousEventScheduler::retire_step(_continuousInterval);
            }

            _plannerSchedulerActive = planner_continues;
            Stepper::ThreadingExecution next_threading_execution;
            if (!planner_continues || !Stepper::threading_execution(next_threading_execution)) {
                ThreadingStepScheduler::reset(_threadingState);
                _threadingBlockToken = std::numeric_limits<uint32_t>::max();
                __atomic_store_n(&_threadingPassActive, false, __ATOMIC_RELEASE);
                _plannerTicksUntilEvent = planner_continues ? std::max<uint32_t>(1U, _plannerPeriodTicks) : 0U;
            }
        }
    } else {
        const uint32_t merge_guard_ticks = std::max<uint32_t>(1U, _pulseUsecs * ticksPerMicrosecond);
        const auto coincidence = ContinuousEventScheduler::choose_coincidence(
            _plannerSchedulerActive,
            _plannerTicksUntilEvent,
            owner && _continuousInterval.active,
            _continuousInterval.ticks_until_step,
            merge_guard_ticks);

        if (coincidence == ContinuousEventScheduler::CoincidenceAction::DelayPlannerToContinuous) {
            ++_continuousPlannerDelays;
            const uint32_t delay_ticks = _continuousInterval.ticks_until_step;
            _plannerDeferredForContinuous = true;
            _plannerDeferredAdjustmentTicks = -static_cast<int32_t>(delay_ticks);
            _plannerTicksUntilEvent = delay_ticks;
            planner_due = false;
        } else if (coincidence == ContinuousEventScheduler::CoincidenceAction::AdvancePlannerToContinuous) {
            ++_continuousPlannerAdvances;
            planner_phase_adjustment = static_cast<int32_t>(_plannerTicksUntilEvent);
            _plannerTicksUntilEvent = 0;
            planner_due = true;
        }
        if (planner_due && _plannerDeferredForContinuous) {
            planner_phase_adjustment = _plannerDeferredAdjustmentTicks;
            _plannerDeferredForContinuous = false;
            _plannerDeferredAdjustmentTicks = 0;
        }

        if (planner_due) {
            const uint32_t continuous_pulses_before =
                __atomic_load_n(&_continuousPulseCounter, __ATOMIC_ACQUIRE);
            __atomic_store_n(&_continuousPulseDue, continuous_due, __ATOMIC_RELEASE);
            const bool planner_continues = Stepper::pulse_func();
            __atomic_store_n(&_continuousPulseDue, false, __ATOMIC_RELEASE);
            if (continuous_due) {
                const uint32_t continuous_pulses_after =
                    __atomic_load_n(&_continuousPulseCounter, __ATOMIC_ACQUIRE);
                const bool emitted_with_planner = ContinuousEventScheduler::pulse_was_emitted(
                    continuous_pulses_before, continuous_pulses_after);
                const bool emitted_at_planner_end =
                    !emitted_with_planner &&
                    ContinuousEventScheduler::missing_due_pulse_action(planner_continues) ==
                        ContinuousEventScheduler::MissingDuePulseAction::EmitContinuousOnly &&
                    emitContinuousPulse();
                if (emitted_with_planner || emitted_at_planner_end) {
                    recordContinuousInterval(_continuousInterval.current_period_ticks);
                    if (emitted_with_planner) {
                        ++_continuousMergedPulses;
                    } else {
                        ++_continuousPlannerEndFallbackPulses;
                    }
                    ContinuousEventScheduler::retire_step(_continuousInterval);
                } else {
                    latchContinuousFault(static_cast<uint32_t>(ContinuousFaultReason::MissingMergedPulse));
                }
            }
            _plannerSchedulerActive = planner_continues;
            _plannerTicksUntilEvent = planner_continues
                                          ? ContinuousEventScheduler::adjusted_planner_period(
                                                _plannerPeriodTicks, planner_phase_adjustment)
                                          : 0;
        } else if (continuous_due) {
            if (emitContinuousPulse()) {
                recordContinuousInterval(_continuousInterval.current_period_ticks);
                ++_continuousStandalonePulses;
                ContinuousEventScheduler::retire_step(_continuousInterval);
            } else {
                latchContinuousFault(static_cast<uint32_t>(ContinuousFaultReason::MissingStandalonePulse));
            }
        }
    }

    owner = __atomic_load_n(&_continuousOwner, __ATOMIC_ACQUIRE);
    uint32_t next_ticks = std::numeric_limits<uint32_t>::max();
    if (_plannerSchedulerActive && !__atomic_load_n(&_threadingPassActive, __ATOMIC_ACQUIRE)) {
        next_ticks = std::max<uint32_t>(1U, _plannerTicksUntilEvent);
    }
    if (owner && _continuousInterval.active) {
        next_ticks = std::min(next_ticks, std::max<uint32_t>(1U, _continuousInterval.ticks_until_step));
    }
    if (owner && (!_continuousInterval.active || _continuousInterval.command.ramping)) {
        constexpr uint32_t service_ticks = 20000U;
        next_ticks = std::min(next_ticks, service_ticks);
    }

    if (next_ticks == std::numeric_limits<uint32_t>::max()) {
        __atomic_store_n(&_continuousSchedulerActive, false, __ATOMIC_RELEASE);
        _schedulerLastIntervalTicks = _plannerPeriodTicks ? _plannerPeriodTicks : 100U;
        step_engine->set_timer_ticks(_schedulerLastIntervalTicks);
        return false;
    }

    _schedulerLastIntervalTicks = next_ticks;
    step_engine->set_timer_ticks(next_ticks);
    return owner || _plannerSchedulerActive;
}

uint32_t Stepping::plannerPeakPulsesPerSecond(axis_t excluded_axis) {
    uint32_t peak = 0;
    if (config == nullptr || config->_axes == nullptr) {
        return maxPulsesPerSec();
    }
    for (axis_t axis = X_AXIS; axis < Axes::_numberAxis; ++axis) {
        if (axis == excluded_axis) {
            continue;
        }
        auto configured_axis = config->_axes->_axis[axis];
        if (configured_axis != nullptr) {
            peak = std::max(peak, static_cast<uint32_t>(std::ceil(
                                      configured_axis->_stepsPerMm * configured_axis->_maxRate / 60.0f)));
        }
    }
    return peak;
}

bool Stepping::startContinuous(axis_t axis, bool positive, uint32_t rate_millihz, uint32_t acceleration_millihz_per_sec) {
    if (step_engine == nullptr || strncmp(step_engine->name, "I2S", 3) != 0 || axis >= Axes::_numberAxis ||
        rate_millihz == 0 || Stepper::is_awake() || continuousActive()) {
        return false;
    }
    auto motor = axis_motors[axis][0];
    if (motor == nullptr || axis_motors[axis][1] != nullptr || motor->blocked || motor->limited) {
        return false;
    }

    _continuousPlannerPeakPulsesPerSec = plannerPeakPulsesPerSecond(axis);
    if (!ContinuousEventScheduler::combined_rate_admissible(
            rate_millihz, _continuousPlannerPeakPulsesPerSec, maxPulsesPerSec())) {
        return false;
    }
    const auto target_command = ContinuousEventScheduler::make_rate_command(fStepperTimer, rate_millihz, true);
    if (!target_command.valid) {
        return false;
    }

    // In the planner convention a clear direction bit increases position.
    const bool dir_bit   = !positive;
    const bool dir_level = dir_bit ^ motor->dir_invert;
    i2s_out_write(motor->step_pin, motor->step_invert ? 1 : 0);
    step_engine->set_dir_pin(motor->dir_pin, dir_level);
    step_engine->finish_dir();
    i2s_out_delay();
    if (!i2s_out_continuous_transport_start()) {
        return false;
    }

    _continuousAxis = axis;
    _continuousTargetRateMillihz = rate_millihz;
    _continuousCurrentRateMillihz = 0;
    _continuousPublishedRateMillihz = 0;
    _continuousAccelerationMillihzPerSec = acceleration_millihz_per_sec ? acceleration_millihz_per_sec : rate_millihz;
    _continuousLastRampMs = millis();
    _continuousRampRemainder = 0;
    _continuousAppliedSequence = 0;
    ContinuousEventScheduler::reset(_continuousInterval);
    __atomic_store_n(&_continuousAppliedRateMillihz, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousPulseCounter, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousFaulted, false, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousFaultPending, false, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousStoppedAck, false, __ATOMIC_RELEASE);
    resetContinuousDiagnostics();
    publishContinuousRate(0, true);

    if (_previousDirectionMask == 65535) {
        _previousDirectionMask = direction_mask;
    }
    if (dir_bit) set_bitnum(_previousDirectionMask, axis);
    else clear_bitnum(_previousDirectionMask, axis);

    _plannerSchedulerActive = false;
    _plannerTicksUntilEvent = 0;
    _plannerDeferredForContinuous = false;
    _plannerDeferredAdjustmentTicks = 0;
    ThreadingStepScheduler::reset(_threadingState);
    _threadingBlockToken = std::numeric_limits<uint32_t>::max();
    __atomic_store_n(&_threadingPassActive, false, __ATOMIC_RELEASE);
    __atomic_store_n(&_threadingInvalidated, false, __ATOMIC_RELEASE);
    __atomic_store_n(&_threadingInvalidatedPending, false, __ATOMIC_RELEASE);
    constexpr uint32_t service_ticks = 20000U;  // 1 ms at the fixed 20 MHz step timer.
    _schedulerLastIntervalTicks = service_ticks;
    __atomic_store_n(&_continuousOwner, true, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousSchedulerActive, true, __ATOMIC_RELEASE);
    step_engine->start_timer();
    return true;
}

bool Stepping::setContinuousRate(uint32_t rate_millihz) {
    if (threadingPassActive() || !continuousActive() || rate_millihz == 0 ||
        !ContinuousEventScheduler::combined_rate_admissible(
            rate_millihz, _continuousPlannerPeakPulsesPerSec, maxPulsesPerSec()) ||
        !ContinuousEventScheduler::make_rate_command(fStepperTimer, rate_millihz).valid) {
        return false;
    }
    _continuousTargetRateMillihz = rate_millihz;
    return true;
}

void Stepping::finishContinuousStop() {
    // The scheduler has retired the last high pulse. Drain the shallow FIFO so
    // the corresponding low/static word is physical before releasing C.
    i2s_out_delay();
    i2s_out_continuous_transport_stop();
    __atomic_store_n(&_continuousOwner, false, __ATOMIC_RELEASE);
    _continuousCurrentRateMillihz = 0;
    _continuousPublishedRateMillihz = 0;
    _continuousAccelerationMillihzPerSec = 0;
    _continuousRampRemainder = 0;
    _continuousAxis = INVALID_AXIS;
    __atomic_store_n(&_continuousStoppedAck, false, __ATOMIC_RELEASE);
}

void Stepping::stopContinuous(bool immediate, uint32_t deceleration_millihz_per_sec) {
    _continuousTargetRateMillihz = 0;
    if (immediate) {
        emergencyStop();
        publishContinuousRate(0, false);
        return;
    }
    if (deceleration_millihz_per_sec != 0) {
        _continuousAccelerationMillihzPerSec = deceleration_millihz_per_sec;
        _continuousRampRemainder = 0;
    }
    serviceContinuous();
}

void IRAM_ATTR Stepping::emergencyStop() {
    // This is the single immediate stop path for the continuous C lane. It is
    // deliberately independent of finite planner state so its callers can
    // decide whether X/Z must also be reset (alarm/reset) or kept intact.
    __atomic_store_n(&_continuousOwner, false, __ATOMIC_RELEASE);
    if (__atomic_exchange_n(&_threadingPassActive, false, __ATOMIC_ACQ_REL)) {
        __atomic_store_n(&_threadingInvalidated, true, __ATOMIC_RELEASE);
        __atomic_store_n(&_threadingInvalidatedPending, true, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&_continuousPulseDue, false, __ATOMIC_RELEASE);
    i2s_out_continuous_transport_stop();
    _continuousTargetRateMillihz = 0;
    _continuousCurrentRateMillihz = 0;
    _continuousPublishedRateMillihz = 0;
    _continuousAccelerationMillihzPerSec = 0;
    _continuousRampRemainder = 0;
    _plannerDeferredForContinuous = false;
    _plannerDeferredAdjustmentTicks = 0;
    _continuousInterval.active = false;
    _continuousInterval.ticks_until_step = 0;
    _continuousAxis = INVALID_AXIS;
    __atomic_store_n(&_continuousAppliedRateMillihz, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousStoppedAck, true, __ATOMIC_RELEASE);
}

void Stepping::resetContinuousDiagnostics() {
    __atomic_store_n(&_continuousFaultReason, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousFaultCount, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousSchedulerCalls, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousPlannerStarts, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousPlannerResets, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousMergedPulses, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousStandalonePulses, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousPlannerEndFallbackPulses, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousPlannerDelays, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousPlannerAdvances, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousLastIntervalTicks, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousMinIntervalTicks, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousMaxIntervalTicks, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousLastPhysicalIntervalFrames, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousMinPhysicalIntervalFrames, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousMaxPhysicalIntervalFrames, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousPhysicalIntervalCount, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousLastPulseTimelineFrames, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousHasPulseTimeline, false, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousFaultPulseCount, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousFaultAppliedRateMillihz, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousFaultRateSequence, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousFaultSchedulerIntervalTicks, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousFaultPlannerPeriodTicks, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousFaultPlannerTicksUntilEvent, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousFaultTicksUntilStep, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousFaultPlannerResetCount, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousFaultFlags, 0U, __ATOMIC_RELEASE);
}

void IRAM_ATTR Stepping::recordContinuousInterval(uint32_t interval_ticks) {
    _continuousLastIntervalTicks = interval_ticks;
    const uint32_t minimum = _continuousMinIntervalTicks;
    if (minimum == 0 || interval_ticks < minimum) {
        _continuousMinIntervalTicks = interval_ticks;
    }
    const uint32_t maximum = _continuousMaxIntervalTicks;
    if (interval_ticks > maximum) {
        _continuousMaxIntervalTicks = interval_ticks;
    }
}

void IRAM_ATTR Stepping::recordContinuousPhysicalPulse() {
    const uint32_t timeline = i2s_out_timeline_frames();
    const bool has_prior = _continuousHasPulseTimeline;
    const uint32_t prior = _continuousLastPulseTimelineFrames;
    _continuousLastPulseTimelineFrames = timeline;
    if (!has_prior) {
        _continuousHasPulseTimeline = true;
        return;
    }

    const uint32_t interval_frames = timeline - prior;
    _continuousLastPhysicalIntervalFrames = interval_frames;
    const uint32_t minimum = _continuousMinPhysicalIntervalFrames;
    if (minimum == 0 || interval_frames < minimum) {
        _continuousMinPhysicalIntervalFrames = interval_frames;
    }
    const uint32_t maximum = _continuousMaxPhysicalIntervalFrames;
    if (interval_frames > maximum) {
        _continuousMaxPhysicalIntervalFrames = interval_frames;
    }
    ++_continuousPhysicalIntervalCount;
}

void IRAM_ATTR Stepping::captureContinuousFault(uint32_t reason) {
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(
            &_continuousFaultReason, &expected, reason, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return;
    }

    const axis_t axis = _continuousAxis;
    motor_pins_t* motor = axis < MAX_N_AXIS ? axis_motors[axis][0] : nullptr;
    uint32_t flags = 0;
    if (__atomic_load_n(&_continuousOwner, __ATOMIC_ACQUIRE)) flags |= 1U << 0;
    if (__atomic_load_n(&_continuousSchedulerActive, __ATOMIC_ACQUIRE)) flags |= 1U << 1;
    if (_plannerSchedulerActive) flags |= 1U << 2;
    if (_plannerSchedulerActive && _plannerTicksUntilEvent == 0) flags |= 1U << 3;
    if (_continuousInterval.active && _continuousInterval.ticks_until_step == 0) flags |= 1U << 4;
    if (Stepper::is_awake()) flags |= 1U << 5;
    if (motor != nullptr) flags |= 1U << 6;
    if (motor != nullptr && motor->blocked) flags |= 1U << 7;
    if (motor != nullptr && motor->limited) flags |= 1U << 8;

    __atomic_store_n(&_continuousFaultPulseCount, __atomic_load_n(&_continuousPulseCounter, __ATOMIC_ACQUIRE), __ATOMIC_RELAXED);
    __atomic_store_n(&_continuousFaultAppliedRateMillihz, __atomic_load_n(&_continuousAppliedRateMillihz, __ATOMIC_ACQUIRE), __ATOMIC_RELAXED);
    __atomic_store_n(&_continuousFaultRateSequence, _continuousAppliedSequence, __ATOMIC_RELAXED);
    __atomic_store_n(&_continuousFaultSchedulerIntervalTicks, _schedulerLastIntervalTicks, __ATOMIC_RELAXED);
    __atomic_store_n(&_continuousFaultPlannerPeriodTicks, _plannerPeriodTicks, __ATOMIC_RELAXED);
    __atomic_store_n(&_continuousFaultPlannerTicksUntilEvent, _plannerTicksUntilEvent, __ATOMIC_RELAXED);
    __atomic_store_n(&_continuousFaultTicksUntilStep, _continuousInterval.ticks_until_step, __ATOMIC_RELAXED);
    __atomic_store_n(&_continuousFaultPlannerResetCount, __atomic_load_n(&_continuousPlannerResets, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    __atomic_store_n(&_continuousFaultFlags, flags, __ATOMIC_RELEASE);
}

void IRAM_ATTR Stepping::latchContinuousFault(uint32_t reason) {
    // A shared scheduler fault invalidates both lanes immediately. Foreground
    // CStepper service then raises the machine alarm and resets planner state.
    __atomic_add_fetch(&_continuousFaultCount, 1U, __ATOMIC_RELAXED);
    captureContinuousFault(reason);
    emergencyStop();
    __atomic_store_n(&_continuousPlannerStartPending, false, __ATOMIC_RELEASE);
    _plannerSchedulerActive = false;
    _plannerTicksUntilEvent = 0;
    __atomic_store_n(&_continuousFaulted, true, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousFaultPending, true, __ATOMIC_RELEASE);
}

void Stepping::serviceContinuous() {
    if (_continuousAxis >= MAX_N_AXIS || !__atomic_load_n(&_continuousOwner, __ATOMIC_ACQUIRE)) {
        return;
    }
    if (i2s_out_continuous_transport_faulted()) {
        latchContinuousFault(static_cast<uint32_t>(ContinuousFaultReason::TransportFault));
        return;
    }

    const uint32_t now = millis();
    const uint32_t elapsed_ms = now - _continuousLastRampMs;
    if (elapsed_ms != 0) {
        _continuousLastRampMs = now;
        const uint32_t next_rate = ContinuousStepperLogic::ramp_rate(
            _continuousCurrentRateMillihz,
            _continuousTargetRateMillihz,
            _continuousAccelerationMillihzPerSec,
            elapsed_ms,
            _continuousRampRemainder);
        _continuousCurrentRateMillihz = next_rate;
        const bool ramping = next_rate != _continuousTargetRateMillihz;
        if (next_rate != _continuousPublishedRateMillihz ||
            (next_rate == 0 && !__atomic_load_n(&_continuousStoppedAck, __ATOMIC_ACQUIRE))) {
            if (next_rate == 0) {
                __atomic_store_n(&_continuousStoppedAck, false, __ATOMIC_RELEASE);
            }
            publishContinuousRate(next_rate, ramping);
        }
    }

    if (_continuousTargetRateMillihz == 0 && _continuousCurrentRateMillihz == 0 &&
        __atomic_load_n(&_continuousStoppedAck, __ATOMIC_ACQUIRE)) {
        finishContinuousStop();
    }
}

bool Stepping::continuousActive() {
    return __atomic_load_n(&_continuousOwner, __ATOMIC_ACQUIRE);
}

uint32_t Stepping::continuousRateMillihz() {
    return __atomic_load_n(&_continuousAppliedRateMillihz, __ATOMIC_ACQUIRE);
}

uint32_t Stepping::continuousTargetRateMillihz() {
    return _continuousTargetRateMillihz;
}

uint32_t Stepping::continuousPulseCount() {
    return __atomic_load_n(&_continuousPulseCounter, __ATOMIC_ACQUIRE);
}

bool Stepping::threadingPassActive() {
    return __atomic_load_n(&_threadingPassActive, __ATOMIC_ACQUIRE);
}

void IRAM_ATTR Stepping::invalidateThreading() {
    if (__atomic_exchange_n(&_threadingPassActive, false, __ATOMIC_ACQ_REL)) {
        __atomic_store_n(&_threadingInvalidated, true, __ATOMIC_RELEASE);
        __atomic_store_n(&_threadingInvalidatedPending, true, __ATOMIC_RELEASE);
    }
}

bool Stepping::takeThreadingInvalidated() {
    return __atomic_exchange_n(&_threadingInvalidatedPending, false, __ATOMIC_ACQ_REL);
}

bool Stepping::continuousFaulted() {
    return __atomic_load_n(&_continuousFaulted, __ATOMIC_ACQUIRE) || i2s_out_continuous_transport_faulted();
}

bool Stepping::takeContinuousFault() {
    const bool scheduler_fault = __atomic_exchange_n(&_continuousFaultPending, false, __ATOMIC_ACQ_REL);
    const bool transport_fault = i2s_out_continuous_transport_take_fault();
    if (transport_fault) {
        captureContinuousFault(static_cast<uint32_t>(ContinuousFaultReason::TransportFault));
        __atomic_store_n(&_continuousFaulted, true, __ATOMIC_RELEASE);
    }
    return scheduler_fault || transport_fault;
}

Stepping::ContinuousDiagnostics Stepping::continuousDiagnostics() {
    ContinuousDiagnostics result;
    result.faultReason = static_cast<ContinuousFaultReason>(__atomic_load_n(&_continuousFaultReason, __ATOMIC_ACQUIRE));
    result.faultCount = __atomic_load_n(&_continuousFaultCount, __ATOMIC_ACQUIRE);
    result.schedulerCalls = __atomic_load_n(&_continuousSchedulerCalls, __ATOMIC_ACQUIRE);
    result.plannerStarts = __atomic_load_n(&_continuousPlannerStarts, __ATOMIC_ACQUIRE);
    result.plannerResets = __atomic_load_n(&_continuousPlannerResets, __ATOMIC_ACQUIRE);
    result.mergedPulses = __atomic_load_n(&_continuousMergedPulses, __ATOMIC_ACQUIRE);
    result.standalonePulses = __atomic_load_n(&_continuousStandalonePulses, __ATOMIC_ACQUIRE);
    result.plannerEndFallbackPulses = __atomic_load_n(&_continuousPlannerEndFallbackPulses, __ATOMIC_ACQUIRE);
    result.plannerDelays = __atomic_load_n(&_continuousPlannerDelays, __ATOMIC_ACQUIRE);
    result.plannerAdvances = __atomic_load_n(&_continuousPlannerAdvances, __ATOMIC_ACQUIRE);
    result.lastIntervalTicks = __atomic_load_n(&_continuousLastIntervalTicks, __ATOMIC_ACQUIRE);
    result.minIntervalTicks = __atomic_load_n(&_continuousMinIntervalTicks, __ATOMIC_ACQUIRE);
    result.maxIntervalTicks = __atomic_load_n(&_continuousMaxIntervalTicks, __ATOMIC_ACQUIRE);
    result.lastPhysicalIntervalFrames = __atomic_load_n(&_continuousLastPhysicalIntervalFrames, __ATOMIC_ACQUIRE);
    result.minPhysicalIntervalFrames = __atomic_load_n(&_continuousMinPhysicalIntervalFrames, __ATOMIC_ACQUIRE);
    result.maxPhysicalIntervalFrames = __atomic_load_n(&_continuousMaxPhysicalIntervalFrames, __ATOMIC_ACQUIRE);
    result.physicalIntervalCount = __atomic_load_n(&_continuousPhysicalIntervalCount, __ATOMIC_ACQUIRE);
    result.faultPulseCount = __atomic_load_n(&_continuousFaultPulseCount, __ATOMIC_ACQUIRE);
    result.faultAppliedRateMillihz = __atomic_load_n(&_continuousFaultAppliedRateMillihz, __ATOMIC_ACQUIRE);
    result.faultRateSequence = __atomic_load_n(&_continuousFaultRateSequence, __ATOMIC_ACQUIRE);
    result.faultSchedulerIntervalTicks = __atomic_load_n(&_continuousFaultSchedulerIntervalTicks, __ATOMIC_ACQUIRE);
    result.faultPlannerPeriodTicks = __atomic_load_n(&_continuousFaultPlannerPeriodTicks, __ATOMIC_ACQUIRE);
    result.faultPlannerTicksUntilEvent = __atomic_load_n(&_continuousFaultPlannerTicksUntilEvent, __ATOMIC_ACQUIRE);
    result.faultTicksUntilStep = __atomic_load_n(&_continuousFaultTicksUntilStep, __ATOMIC_ACQUIRE);
    result.faultPlannerResetCount = __atomic_load_n(&_continuousFaultPlannerResetCount, __ATOMIC_ACQUIRE);
    const uint32_t flags = __atomic_load_n(&_continuousFaultFlags, __ATOMIC_ACQUIRE);
    result.faultOwner = (flags & (1U << 0)) != 0;
    result.faultSchedulerActive = (flags & (1U << 1)) != 0;
    result.faultPlannerActive = (flags & (1U << 2)) != 0;
    result.faultPlannerDue = (flags & (1U << 3)) != 0;
    result.faultContinuousDue = (flags & (1U << 4)) != 0;
    result.faultStepperAwake = (flags & (1U << 5)) != 0;
    result.faultMotorPresent = (flags & (1U << 6)) != 0;
    result.faultMotorBlocked = (flags & (1U << 7)) != 0;
    result.faultMotorLimited = (flags & (1U << 8)) != 0;
    return result;
}

const char* Stepping::continuousFaultReasonName(ContinuousFaultReason reason) {
    switch (reason) {
        case ContinuousFaultReason::None: return "none";
        case ContinuousFaultReason::UnexpectedPlannerC: return "unexpected_planner_c";
        case ContinuousFaultReason::COutputUnavailable: return "c_output_unavailable";
        case ContinuousFaultReason::InvalidRateCommand: return "invalid_rate_command";
        case ContinuousFaultReason::TransportFault: return "transport_fault";
        case ContinuousFaultReason::InvalidAppliedRate: return "invalid_applied_rate";
        case ContinuousFaultReason::MissingMergedPulse: return "missing_merged_pulse";
        case ContinuousFaultReason::MissingStandalonePulse: return "missing_standalone_pulse";
        case ContinuousFaultReason::InvalidThreadingCommand: return "invalid_threading_command";
        case ContinuousFaultReason::ThreadingRateChanged: return "threading_rate_changed";
        case ContinuousFaultReason::ThreadingDecisionMismatch: return "threading_decision_mismatch";
        case ContinuousFaultReason::ThreadingLostContinuousC: return "threading_lost_continuous_c";
    }
    return "unknown";
}
