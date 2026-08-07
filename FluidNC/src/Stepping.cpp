// #include "Driver/i2s_out.h"
#include "EnumItem.h"
#include "Stepping.h"
#include "Machine/MachineConfig.h"  // config
#include "Driver/i2s_out.h"
#include "ContinuousStepperLogic.h"

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

        uint32_t actual = step_engine->init(_directionDelayUsecs, _pulseUsecs, fStepperTimer, Stepper::pulse_func);
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
    // Set the direction pins, but optimize for the common
    // situation where the direction bits haven't changed.
    if (_previousDirectionMask == 65535) {
        // Set all the direction bits the first time
        _previousDirectionMask = ~dir_mask;
    }

    if (dir_mask != _previousDirectionMask) {
        for (axis_t axis = X_AXIS; axis < Axes::_numberAxis; axis++) {
            if (axis == _continuousAxis) {
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
        if (_continuousAxis < MAX_N_AXIS) {
            const bool continuous_dir = bitnum_is_true(_previousDirectionMask, _continuousAxis);
            _previousDirectionMask = dir_mask;
            if (continuous_dir) set_bitnum(_previousDirectionMask, _continuousAxis);
            else clear_bitnum(_previousDirectionMask, _continuousAxis);
        } else {
            _previousDirectionMask = dir_mask;
        }
    }

    step_engine->start_step();

    // Turn on step pulses for motors that are supposed to step now
    for (axis_t axis = X_AXIS; axis < Axes::_numberAxis; axis++) {
        if (bitnum_is_true(step_mask, axis)) {
            if (axis == _continuousAxis) {
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

void Stepping::reset() {}
void Stepping::beginLowLatency() {}
void Stepping::endLowLatency() {}

// Called only from Stepper::pulse_func when a new segment is loaded
// The argument is in units of ticks of the timer that generates ISRs
void IRAM_ATTR Stepping::setTimerPeriod(uint32_t ticks) {
    step_engine->set_timer_ticks((uint32_t)ticks);
}

// Called only from Stepper::wake_up which is not used in ISR context
void Stepping::startTimer() {
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

bool Stepping::startContinuous(axis_t axis, bool positive, uint32_t rate_millihz, uint32_t acceleration_millihz_per_sec) {
    if (step_engine == nullptr || strncmp(step_engine->name, "I2S", 3) != 0 || axis >= Axes::_numberAxis || rate_millihz == 0) {
        return false;
    }
    auto motor = axis_motors[axis][0];
    if (motor == nullptr || axis_motors[axis][1] != nullptr) {
        return false;
    }

    // In the planner convention a clear direction bit increases position.
    const bool dir_bit   = !positive;
    const bool dir_level = dir_bit ^ motor->dir_invert;
    _continuousAxis      = axis;
    _continuousTargetRateMillihz       = rate_millihz;
    _continuousCurrentRateMillihz      = 0;
    _continuousAccelerationMillihzPerSec = acceleration_millihz_per_sec ? acceleration_millihz_per_sec : rate_millihz;
    _continuousLastRampMs              = millis();
    _continuousRampRemainder           = 0;
    if (!i2s_out_aux_step_start(motor->step_pin,
                                motor->step_invert,
                                motor->dir_pin,
                                dir_level,
                                0)) {
        _continuousAxis = INVALID_AXIS;
        _continuousTargetRateMillihz = 0;
        _continuousAccelerationMillihzPerSec = 0;
        return false;
    }
    if (_previousDirectionMask == 65535) {
        _previousDirectionMask = direction_mask;
    }
    if (dir_bit) set_bitnum(_previousDirectionMask, axis);
    else clear_bitnum(_previousDirectionMask, axis);
    return true;
}

void Stepping::setContinuousRate(uint32_t rate_millihz) {
    _continuousTargetRateMillihz = rate_millihz;
}

void Stepping::stopContinuous(bool immediate, uint32_t deceleration_millihz_per_sec) {
    _continuousTargetRateMillihz = 0;
    if (immediate) {
        i2s_out_aux_step_set_rate(0);
        i2s_out_aux_step_stop(true);
        _continuousCurrentRateMillihz = 0;
        _continuousAccelerationMillihzPerSec = 0;
        _continuousRampRemainder = 0;
        _continuousAxis = INVALID_AXIS;
        return;
    }
    if (deceleration_millihz_per_sec != 0) {
        _continuousAccelerationMillihzPerSec = deceleration_millihz_per_sec;
        _continuousRampRemainder             = 0;
    }
    serviceContinuous();
}

void Stepping::serviceContinuous() {
    if (_continuousAxis >= MAX_N_AXIS) {
        return;
    }

    if (!i2s_out_aux_step_active()) {
        _continuousTargetRateMillihz = 0;
        _continuousCurrentRateMillihz = 0;
        _continuousAccelerationMillihzPerSec = 0;
        _continuousRampRemainder = 0;
        _continuousAxis = INVALID_AXIS;
        return;
    }

    const uint32_t now        = millis();
    const uint32_t elapsed_ms = now - _continuousLastRampMs;
    if (elapsed_ms == 0) {
        return;
    }
    _continuousLastRampMs = now;

    const uint32_t next_rate = ContinuousStepperLogic::ramp_rate(
        _continuousCurrentRateMillihz,
        _continuousTargetRateMillihz,
        _continuousAccelerationMillihzPerSec,
        elapsed_ms,
        _continuousRampRemainder);
    if (next_rate != _continuousCurrentRateMillihz) {
        _continuousCurrentRateMillihz = next_rate;
        i2s_out_aux_step_set_rate(next_rate);
    }

    if (_continuousTargetRateMillihz == 0 && _continuousCurrentRateMillihz == 0) {
        i2s_out_aux_step_stop(true);
        _continuousAccelerationMillihzPerSec = 0;
        _continuousRampRemainder = 0;
        _continuousAxis = INVALID_AXIS;
    }
}

bool Stepping::continuousActive() {
    const bool active = i2s_out_aux_step_active();
    if (!active) {
        _continuousTargetRateMillihz = 0;
        _continuousCurrentRateMillihz = 0;
        _continuousAccelerationMillihzPerSec = 0;
        _continuousRampRemainder = 0;
        _continuousAxis = INVALID_AXIS;
    }
    return active;
}

uint32_t Stepping::continuousRateMillihz() {
    return _continuousCurrentRateMillihz;
}

bool Stepping::takeContinuousFault() {
    return i2s_out_aux_step_take_fault();
}
