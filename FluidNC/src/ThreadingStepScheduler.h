// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace Machine::ThreadingStepScheduler {
#if defined(__GNUC__)
#    define THREADING_SCHEDULER_INLINE inline __attribute__((always_inline))
#else
#    define THREADING_SCHEDULER_INLINE inline
#endif

    enum class Phase : uint8_t {
        Idle = 0,
        Armed,
        Running,
        Invalidated,
    };

    struct Command {
        uint32_t c_steps_per_revolution = 0;
        uint32_t z_steps_per_revolution = 0;
        bool     valid                  = false;
    };

    struct State {
        Command  command;
        uint32_t start_c_pulse = 0;
        uint32_t accumulator   = 0;
        Phase    phase         = Phase::Idle;
    };

    // Convert the configured mechanical scale and programmed pitch once in
    // foreground code. Phase 5 intentionally supports at most one Z step per C
    // step; wider pitches need a separately reviewed interleaved-event design.
    inline Command make_command(float pitch_mm, float z_steps_per_mm, uint32_t c_steps_per_revolution) {
        Command command;
        if (!std::isfinite(pitch_mm) || !std::isfinite(z_steps_per_mm) ||
            pitch_mm <= 0.0f || z_steps_per_mm <= 0.0f || c_steps_per_revolution == 0) {
            return command;
        }

        const float z_steps = pitch_mm * z_steps_per_mm;
        if (z_steps < 1.0f || z_steps > static_cast<float>(std::numeric_limits<uint32_t>::max())) {
            return command;
        }

        command.c_steps_per_revolution = c_steps_per_revolution;
        command.z_steps_per_revolution = static_cast<uint32_t>(std::lround(z_steps));
        command.valid = command.z_steps_per_revolution != 0 &&
                        command.z_steps_per_revolution <= command.c_steps_per_revolution;
        return command;
    }

    inline bool rate_admissible(const Command& command,
                                 uint32_t       c_rate_millihz,
                                 float          z_steps_per_mm,
                                 float          z_max_rate_mm_per_min,
                                 float          z_acceleration_mm_per_sec2) {
        if (!command.valid || c_rate_millihz == 0 || !std::isfinite(z_steps_per_mm) ||
            !std::isfinite(z_max_rate_mm_per_min) || z_steps_per_mm <= 0.0f ||
            z_max_rate_mm_per_min <= 0.0f || !std::isfinite(z_acceleration_mm_per_sec2) ||
            z_acceleration_mm_per_sec2 <= 0.0f) {
            return false;
        }

        const float z_steps_per_minute =
            static_cast<float>(c_rate_millihz) * 0.06f * command.z_steps_per_revolution /
            command.c_steps_per_revolution;
        if (z_steps_per_minute >
            z_steps_per_mm * z_max_rate_mm_per_min) {
            return false;
        }

        // Phase 5 starts synchronized Z from rest without a separate lead-in.
        // Admit only rates where configured acceleration can cover one physical
        // Z step before the ratio schedules its first Z pulse. Wider/faster
        // threading needs an explicit lead-in design, not a bypass here.
        const uint32_t c_pulses_before_first_z =
            (command.c_steps_per_revolution - 1U) / command.z_steps_per_revolution;
        const float c_steps_per_second = static_cast<float>(c_rate_millihz) / 1000.0f;
        const float first_z_time_seconds =
            static_cast<float>(c_pulses_before_first_z) / c_steps_per_second;
        const float acceleration_distance_mm =
            0.5f * z_acceleration_mm_per_sec2 *
            first_z_time_seconds * first_z_time_seconds;
        return acceleration_distance_mm >= 1.0f / z_steps_per_mm;
    }

    THREADING_SCHEDULER_INLINE void reset(State& state) {
        state = {};
    }

    // Arm for the next commanded revolution boundary, never a boundary that
    // has already passed. Phase 6 will replace this synthetic boundary with a
    // fresh physical Index observation.
    THREADING_SCHEDULER_INLINE bool arm(State& state, const Command& command, uint32_t current_c_pulse) {
        if (!command.valid) {
            state.phase = Phase::Invalidated;
            return false;
        }

        state.command = command;
        const uint32_t phase = current_c_pulse % command.c_steps_per_revolution;
        const uint32_t pulses_to_boundary = phase == 0 ? command.c_steps_per_revolution
                                                       : command.c_steps_per_revolution - phase;
        state.start_c_pulse   = current_c_pulse + pulses_to_boundary;
        state.accumulator     = 0;
        state.phase           = Phase::Armed;
        return true;
    }

    THREADING_SCHEDULER_INLINE void invalidate(State& state) {
        state.phase = Phase::Invalidated;
    }

    THREADING_SCHEDULER_INLINE bool active(const State& state) {
        return state.phase == Phase::Armed || state.phase == Phase::Running;
    }

    THREADING_SCHEDULER_INLINE bool reached(uint32_t value, uint32_t target) {
        return static_cast<int32_t>(value - target) >= 0;
    }

    // Preview whether the next successfully emitted C pulse must also emit Z.
    // State changes are committed only by retire_c_pulse(), after production
    // confirms that the C pulse was actually scheduled.
    THREADING_SCHEDULER_INLINE bool z_due_on_next_c_pulse(const State& state, uint32_t current_c_pulse) {
        if (!active(state)) {
            return false;
        }

        const uint32_t next_c_pulse = current_c_pulse + 1U;
        if (state.phase == Phase::Armed && !reached(next_c_pulse, state.start_c_pulse)) {
            return false;
        }
        return state.accumulator + state.command.z_steps_per_revolution >=
               state.command.c_steps_per_revolution;
    }

    // Retire exactly one confirmed C pulse and return the Z decision associated
    // with that pulse. Unsigned pulse arithmetic is deliberate across wrap.
    THREADING_SCHEDULER_INLINE bool retire_c_pulse(State& state, uint32_t emitted_c_pulse) {
        if (!active(state)) {
            return false;
        }
        if (state.phase == Phase::Armed) {
            if (!reached(emitted_c_pulse, state.start_c_pulse)) {
                return false;
            }
            state.phase = Phase::Running;
        }

        state.accumulator += state.command.z_steps_per_revolution;
        if (state.accumulator < state.command.c_steps_per_revolution) {
            return false;
        }
        state.accumulator -= state.command.c_steps_per_revolution;
        return true;
    }

#undef THREADING_SCHEDULER_INLINE
}
