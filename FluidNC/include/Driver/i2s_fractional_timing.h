// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include <stdint.h>

// Carries timer ticks that do not fill a complete I2S frame into the next
// planner interval. The caller guarantees interval_ticks + residual_ticks fits
// in uint32_t; FluidNC planner intervals are limited to 16 bits.
typedef struct {
    uint32_t ticks_per_frame;
    uint32_t interval_ticks;
    uint32_t interval_frames;
    uint32_t interval_remainder_ticks;
    uint32_t residual_ticks;
} i2s_fractional_timing_t;

static inline __attribute__((always_inline)) void i2s_fractional_timing_init(i2s_fractional_timing_t* state,
                                                                            uint32_t                 ticks_per_frame) {
    state->ticks_per_frame = ticks_per_frame ? ticks_per_frame : 1;
    state->interval_ticks  = 0;
    state->interval_frames = 0;
    state->interval_remainder_ticks = 0;
    state->residual_ticks  = 0;
}

static inline __attribute__((always_inline)) void i2s_fractional_timing_reset(i2s_fractional_timing_t* state) {
    state->residual_ticks = 0;
}

static inline __attribute__((always_inline)) void i2s_fractional_timing_set_interval(i2s_fractional_timing_t* state,
                                                                                    uint32_t interval_ticks) {
    state->interval_ticks           = interval_ticks;
    state->interval_frames          = interval_ticks / state->ticks_per_frame;
    state->interval_remainder_ticks = interval_ticks - state->interval_frames * state->ticks_per_frame;
}

// Hot-path conversion: set_interval performs the division once when the planner
// loads a segment. Each emitted interval then needs only bounded integer math.
static inline __attribute__((always_inline)) uint32_t i2s_fractional_timing_next(i2s_fractional_timing_t* state) {
    uint32_t frames = state->interval_frames;
    state->residual_ticks += state->interval_remainder_ticks;
    if (state->residual_ticks >= state->ticks_per_frame) {
        state->residual_ticks -= state->ticks_per_frame;
        ++frames;
    }
    return frames;
}
