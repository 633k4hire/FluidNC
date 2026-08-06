// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Local state for composing one auxiliary step stream into planner-produced
// I2S frames. The ISR snapshots this state once per FIFO refill, runs this
// constant-time function for each frame, then commits the mutable fields once.
typedef struct {
    uint32_t phase;
    uint32_t increment;
    uint32_t pulse_frames_remaining;
    uint32_t pulse_frames;
    uint32_t step_bit;
    uint32_t generated_pulses;
    bool     step_active_high;
} i2s_aux_frame_state_t;

static inline __attribute__((always_inline)) uint32_t i2s_aux_compose_frame(uint32_t sample, i2s_aux_frame_state_t* state) {
    const uint32_t prior = state->phase;
    state->phase         = prior + state->increment;
    if (state->phase < prior && state->pulse_frames_remaining == 0) {
        state->pulse_frames_remaining = state->pulse_frames;
        ++state->generated_pulses;
    }

    if (state->pulse_frames_remaining != 0) {
        if (state->step_active_high) {
            sample |= state->step_bit;
        } else {
            sample &= ~state->step_bit;
        }
        --state->pulse_frames_remaining;
    }
    return sample;
}
