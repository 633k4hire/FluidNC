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
                    latchContinuousFault();
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
                } else {
                    latchContinuousFault();
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
    __atomic_store_n(&_continuousPlannerStartPending, false, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousPulseDue, false, __ATOMIC_RELEASE);
    _plannerSchedulerActive = false;
    _plannerTicksUntilEvent = 0;
    _plannerDeferredForContinuous = false;
    _plannerDeferredAdjustmentTicks = 0;
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
        latchContinuousFault();
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

    const uint32_t elapsed_ticks = _schedulerLastIntervalTicks ? _schedulerLastIntervalTicks : 1U;
    bool owner = __atomic_load_n(&_continuousOwner, __ATOMIC_ACQUIRE);
    if (owner && i2s_out_continuous_transport_faulted()) {
        latchContinuousFault();
        owner = false;
    }

    ContinuousEventScheduler::RateCommand command;
    uint32_t command_sequence = 0;
    if (readContinuousRateCommand(command, command_sequence) && command_sequence != _continuousAppliedSequence) {
        if (!ContinuousEventScheduler::apply_rate(_continuousInterval, command)) {
            latchContinuousFault();
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
        _plannerSchedulerActive = true;
        _plannerTicksUntilEvent = 0;
    } else if (_plannerSchedulerActive && !Stepper::is_awake()) {
        _plannerSchedulerActive = false;
        _plannerTicksUntilEvent = 0;
        _plannerDeferredForContinuous = false;
        _plannerDeferredAdjustmentTicks = 0;
    }

    if (_plannerSchedulerActive && _plannerTicksUntilEvent != 0) {
        ContinuousEventScheduler::elapse(_plannerTicksUntilEvent, elapsed_ticks);
    }
    if (owner && _continuousInterval.active && _continuousInterval.ticks_until_step != 0) {
        ContinuousEventScheduler::elapse(_continuousInterval.ticks_until_step, elapsed_ticks);
    }

    bool planner_due = _plannerSchedulerActive && _plannerTicksUntilEvent == 0;
    const bool continuous_due = owner && _continuousInterval.active &&
                                _continuousInterval.ticks_until_step == 0;
    int32_t planner_phase_adjustment = 0;
    const uint32_t merge_guard_ticks = std::max<uint32_t>(1U, _pulseUsecs * ticksPerMicrosecond);
    const auto coincidence = ContinuousEventScheduler::choose_coincidence(
        _plannerSchedulerActive,
        _plannerTicksUntilEvent,
        owner && _continuousInterval.active,
        _continuousInterval.ticks_until_step,
        merge_guard_ticks);

    if (coincidence == ContinuousEventScheduler::CoincidenceAction::DelayPlannerToContinuous) {
        const uint32_t delay_ticks = _continuousInterval.ticks_until_step;
        _plannerDeferredForContinuous = true;
        _plannerDeferredAdjustmentTicks = -static_cast<int32_t>(delay_ticks);
        _plannerTicksUntilEvent = delay_ticks;
        planner_due = false;
    } else if (coincidence == ContinuousEventScheduler::CoincidenceAction::AdvancePlannerToContinuous) {
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
                ContinuousEventScheduler::retire_step(_continuousInterval);
            } else {
                latchContinuousFault();
            }
        }
        _plannerSchedulerActive = planner_continues;
        _plannerTicksUntilEvent = planner_continues
                                      ? ContinuousEventScheduler::adjusted_planner_period(
                                            _plannerPeriodTicks, planner_phase_adjustment)
                                      : 0;
    } else if (continuous_due) {
        if (emitContinuousPulse()) {
            ContinuousEventScheduler::retire_step(_continuousInterval);
        } else {
            latchContinuousFault();
        }
    }

    owner = __atomic_load_n(&_continuousOwner, __ATOMIC_ACQUIRE);
    uint32_t next_ticks = std::numeric_limits<uint32_t>::max();
    if (_plannerSchedulerActive) {
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
    constexpr uint32_t service_ticks = 20000U;  // 1 ms at the fixed 20 MHz step timer.
    _schedulerLastIntervalTicks = service_ticks;
    __atomic_store_n(&_continuousOwner, true, __ATOMIC_RELEASE);
    __atomic_store_n(&_continuousSchedulerActive, true, __ATOMIC_RELEASE);
    step_engine->start_timer();
    return true;
}

bool Stepping::setContinuousRate(uint32_t rate_millihz) {
    if (!continuousActive() || rate_millihz == 0 ||
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

void IRAM_ATTR Stepping::latchContinuousFault() {
    // A shared scheduler fault invalidates both lanes immediately. Foreground
    // CStepper service then raises the machine alarm and resets planner state.
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
        latchContinuousFault();
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

bool Stepping::continuousFaulted() {
    return __atomic_load_n(&_continuousFaulted, __ATOMIC_ACQUIRE) || i2s_out_continuous_transport_faulted();
}

bool Stepping::takeContinuousFault() {
    const bool scheduler_fault = __atomic_exchange_n(&_continuousFaultPending, false, __ATOMIC_ACQ_REL);
    const bool transport_fault = i2s_out_continuous_transport_take_fault();
    if (transport_fault) {
        __atomic_store_n(&_continuousFaulted, true, __ATOMIC_RELEASE);
    }
    return scheduler_fault || transport_fault;
}
