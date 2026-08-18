// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace Machine::ContinuousEventScheduler {
#if defined(__GNUC__)
#define CONTINUOUS_SCHEDULER_INLINE inline __attribute__((always_inline))
#else
#define CONTINUOUS_SCHEDULER_INLINE inline
#endif
    struct RateCommand {
        uint32_t rate_millihz   = 0;
        uint32_t whole_ticks    = 0;
        uint32_t remainder      = 0;
        uint32_t denominator    = 1;
        bool     ramping        = false;
        bool     valid          = true;
    };

    // This division runs in foreground code. The ISR consumes only the
    // precomputed quotient/remainder and performs bounded integer addition.
    CONTINUOUS_SCHEDULER_INLINE RateCommand make_rate_command(uint32_t timer_hz, uint32_t rate_millihz, bool ramping = false) {
        RateCommand command;
        command.rate_millihz = rate_millihz;
        command.ramping      = ramping;
        if (rate_millihz == 0) {
            return command;
        }

        const uint64_t numerator = static_cast<uint64_t>(timer_hz) * 1000ULL;
        const uint64_t whole     = numerator / rate_millihz;
        if (whole == 0 || whole > std::numeric_limits<uint32_t>::max()) {
            command.valid = false;
            return command;
        }
        command.whole_ticks = static_cast<uint32_t>(whole);
        command.remainder   = static_cast<uint32_t>(numerator % rate_millihz);
        command.denominator = rate_millihz;
        return command;
    }

    struct IntervalState {
        RateCommand command;
        uint32_t    remainder_carry = 0;
        uint32_t    current_period_ticks = 0;
        uint32_t    ticks_until_step = 0;
        uint32_t    emitted_pulses = 0;
        bool        active = false;
    };

    CONTINUOUS_SCHEDULER_INLINE void reset(IntervalState& state, bool clear_pulse_count = true) {
        const uint32_t pulse_count = clear_pulse_count ? 0 : state.emitted_pulses;
        state                      = {};
        state.emitted_pulses       = pulse_count;
    }

    CONTINUOUS_SCHEDULER_INLINE uint32_t next_period(IntervalState& state) {
        uint32_t ticks = state.command.whole_ticks;
        const uint64_t carried = static_cast<uint64_t>(state.remainder_carry) + state.command.remainder;
        if (carried >= state.command.denominator) {
            ++ticks;
            state.remainder_carry = static_cast<uint32_t>(carried - state.command.denominator);
        } else {
            state.remainder_carry = static_cast<uint32_t>(carried);
        }
        return ticks ? ticks : 1;
    }

    // Apply a precomputed foreground command without division. Accelerating
    // may shorten the pending interval; decelerating may lengthen it. A step
    // already due is never moved by a rate update.
    CONTINUOUS_SCHEDULER_INLINE bool apply_rate(IntervalState& state, const RateCommand& command) {
        if (!command.valid) {
            return false;
        }
        if (command.rate_millihz == 0) {
            state.command              = command;
            state.remainder_carry      = 0;
            state.current_period_ticks = 0;
            state.ticks_until_step     = 0;
            state.active               = false;
            return true;
        }
        if (state.active && state.command.rate_millihz == command.rate_millihz) {
            state.command.ramping = command.ramping;
            return true;
        }

        const uint32_t old_rate      = state.command.rate_millihz;
        const uint32_t old_remaining = state.ticks_until_step;
        state.command                = command;
        state.remainder_carry        = 0;
        state.current_period_ticks   = next_period(state);
        state.active                 = true;

        if (old_rate == 0) {
            state.ticks_until_step = state.current_period_ticks;
        } else if (old_remaining == 0) {
            state.ticks_until_step = 0;
        } else if (command.rate_millihz > old_rate) {
            state.ticks_until_step = std::min(old_remaining, state.current_period_ticks);
        } else {
            state.ticks_until_step = std::max(old_remaining, state.current_period_ticks);
        }
        return true;
    }

    CONTINUOUS_SCHEDULER_INLINE uint32_t elapse(uint32_t& ticks_remaining, uint32_t elapsed_ticks) {
        if (ticks_remaining == 0) {
            return elapsed_ticks;
        }
        if (elapsed_ticks >= ticks_remaining) {
            const uint32_t overdue = elapsed_ticks - ticks_remaining;
            ticks_remaining        = 0;
            return overdue;
        }
        ticks_remaining -= elapsed_ticks;
        return 0;
    }

    // phase_adjustment_ticks is positive for a pulse merged early with a
    // planner event, and negative for a pulse deliberately delayed to one.
    CONTINUOUS_SCHEDULER_INLINE void retire_step(IntervalState& state, int32_t phase_adjustment_ticks = 0) {
        ++state.emitted_pulses;
        state.current_period_ticks = next_period(state);
        int64_t next = static_cast<int64_t>(state.current_period_ticks) + phase_adjustment_ticks;
        if (next < 1) {
            next = 1;
        } else if (next > std::numeric_limits<uint32_t>::max()) {
            next = std::numeric_limits<uint32_t>::max();
        }
        state.ticks_until_step = static_cast<uint32_t>(next);
    }

    CONTINUOUS_SCHEDULER_INLINE uint32_t merged_step_mask(uint32_t planner_mask, uint32_t continuous_mask, bool continuous_due) {
        return continuous_due ? planner_mask | continuous_mask : planner_mask;
    }

    CONTINUOUS_SCHEDULER_INLINE bool combined_rate_admissible(uint32_t continuous_rate_millihz,
                                                             uint32_t planner_peak_steps_per_second,
                                                             uint32_t engine_peak_steps_per_second) {
        const uint64_t combined_millihz = static_cast<uint64_t>(continuous_rate_millihz) +
                                          static_cast<uint64_t>(planner_peak_steps_per_second) * 1000ULL;
        return combined_millihz <= static_cast<uint64_t>(engine_peak_steps_per_second) * 1000ULL;
    }

#undef CONTINUOUS_SCHEDULER_INLINE
}
