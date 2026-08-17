// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Phase 1 is a pure transport model.  Production I2S code does not include
// this header until the planner-only Phase 2 integration is reviewed.
#define I2S_SHALLOW_DMA_DESCRIPTOR_COUNT 4u
#define I2S_SHALLOW_DMA_FRAMES_PER_DESCRIPTOR 32u
#define I2S_SHALLOW_DMA_FRAME_PERIOD_US 2u
#define I2S_SHALLOW_DMA_DESCRIPTOR_PERIOD_US \
    (I2S_SHALLOW_DMA_FRAMES_PER_DESCRIPTOR * I2S_SHALLOW_DMA_FRAME_PERIOD_US)
#define I2S_SHALLOW_DMA_RING_PERIOD_US \
    (I2S_SHALLOW_DMA_DESCRIPTOR_COUNT * I2S_SHALLOW_DMA_DESCRIPTOR_PERIOD_US)
#define I2S_SHALLOW_DMA_NO_DESCRIPTOR UINT8_MAX

#if defined(__cplusplus)
static_assert(I2S_SHALLOW_DMA_DESCRIPTOR_COUNT == 4u, "DLC32 shallow DMA requires four descriptors");
static_assert(I2S_SHALLOW_DMA_FRAMES_PER_DESCRIPTOR == 32u, "DLC32 shallow DMA requires 32-frame descriptors");
static_assert(I2S_SHALLOW_DMA_DESCRIPTOR_PERIOD_US == 64u, "descriptor horizon must remain 64 us");
static_assert(I2S_SHALLOW_DMA_RING_PERIOD_US == 256u, "ring horizon must remain 256 us");
#else
_Static_assert(I2S_SHALLOW_DMA_DESCRIPTOR_COUNT == 4u, "DLC32 shallow DMA requires four descriptors");
_Static_assert(I2S_SHALLOW_DMA_FRAMES_PER_DESCRIPTOR == 32u, "DLC32 shallow DMA requires 32-frame descriptors");
_Static_assert(I2S_SHALLOW_DMA_DESCRIPTOR_PERIOD_US == 64u, "descriptor horizon must remain 64 us");
_Static_assert(I2S_SHALLOW_DMA_RING_PERIOD_US == 256u, "ring horizon must remain 256 us");
#endif

// Each descriptor follows one four-state ownership cycle.  A completed
// descriptor becomes ISR-owned for refill; it cannot return to hardware until
// a complete new frame set has been published.
typedef enum {
    I2S_SHALLOW_DMA_OWNER_SOFTWARE = 0,
    I2S_SHALLOW_DMA_OWNER_REFILLING,
    I2S_SHALLOW_DMA_OWNER_READY,
    I2S_SHALLOW_DMA_OWNER_ACTIVE,
    I2S_SHALLOW_DMA_OWNER_COUNT,
} i2s_shallow_dma_owner_t;

typedef enum {
    I2S_SHALLOW_CHUCK_IDLE = 0,
    I2S_SHALLOW_CHUCK_C_POSITIONING,
    I2S_SHALLOW_CHUCK_SPINDLE,
} i2s_shallow_chuck_mode_t;

typedef enum {
    I2S_SHALLOW_DMA_FAULT_NONE = 0,
    I2S_SHALLOW_DMA_FAULT_OWNERSHIP,
    I2S_SHALLOW_DMA_FAULT_STARVATION,
    I2S_SHALLOW_DMA_FAULT_DESCRIPTOR,
    I2S_SHALLOW_DMA_FAULT_STALE_REPLAY,
    I2S_SHALLOW_DMA_FAULT_COMPLETION,
    I2S_SHALLOW_DMA_FAULT_PLANNER_C_CONFLICT,
    I2S_SHALLOW_DMA_FAULT_CHUCK_MODE,
} i2s_shallow_dma_fault_t;

// Diagnostics are fixed-width atomic scalars.  Task-context code may snapshot
// and format them later; ISR paths only update these counters.
typedef struct {
    volatile uint32_t refill_count;
    volatile uint32_t frame_count;
    volatile uint32_t transmission_count;
    volatile uint32_t eof_count;
    volatile uint32_t max_refill_us;
    volatile uint32_t planner_start_count;
    volatile uint32_t planner_completion_mark_count;
    volatile uint32_t planner_physical_completion_count;
    volatile uint32_t planner_reset_count;
    volatile uint32_t ownership_fault_count;
    volatile uint32_t starvation_fault_count;
    volatile uint32_t descriptor_fault_count;
    volatile uint32_t stale_replay_fault_count;
    volatile uint32_t completion_fault_count;
    volatile uint32_t planner_c_conflict_count;
    volatile uint32_t chuck_mode_fault_count;
} i2s_shallow_dma_diagnostics_t;

typedef struct {
    volatile uint8_t owner;
    uint8_t          ring_index;
    uint16_t         reserved;
    uint32_t         refill_sequence;
    uint32_t         transmitted_sequence;
    uint32_t         static_revision;
    uint32_t         completion_motion;
} i2s_shallow_dma_descriptor_t;

typedef struct {
    i2s_shallow_dma_descriptor_t descriptors[I2S_SHALLOW_DMA_DESCRIPTOR_COUNT];
    i2s_shallow_dma_diagnostics_t diagnostics;
    i2s_shallow_chuck_mode_t      chuck_mode;
    i2s_shallow_dma_fault_t       fault_reason;
    uint32_t                      latest_static_revision;
    uint32_t                      retired_static_revision;
    uint32_t                      next_motion;
    uint32_t                      active_motion;
    uint32_t                      marker_motion;
    uint32_t                      pending_motion;
    uint8_t                       next_descriptor;
    uint8_t                       active_descriptor;
    uint8_t                       marker_descriptor;
    bool                          running;
    bool                          planner_active;
    bool                          auxiliary_c_active;
    bool                          faulted;
    bool                          completion_marker_valid;
    bool                          completion_pending;
} i2s_shallow_dma_state_t;

static inline uint32_t i2s_shallow_dma_atomic_load(const volatile uint32_t* value) {
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}

static inline void i2s_shallow_dma_atomic_increment(volatile uint32_t* value) {
    (void)__atomic_fetch_add(value, 1u, __ATOMIC_RELAXED);
}

static inline void i2s_shallow_dma_atomic_add(volatile uint32_t* value, uint32_t increment) {
    (void)__atomic_fetch_add(value, increment, __ATOMIC_RELAXED);
}

static inline void i2s_shallow_dma_atomic_max(volatile uint32_t* value, uint32_t candidate) {
    uint32_t current = __atomic_load_n(value, __ATOMIC_RELAXED);
    while (candidate > current &&
           !__atomic_compare_exchange_n(value,
                                        &current,
                                        candidate,
                                        true,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {}
}

static inline uint32_t i2s_shallow_dma_next_nonzero(uint32_t value) {
    ++value;
    return value == 0u ? 1u : value;
}

// Serial-number comparison is valid while fewer than 2^31 revisions are
// outstanding, which is far beyond the four-descriptor ring.
static inline bool i2s_shallow_dma_serial_newer(uint32_t candidate, uint32_t reference) {
    return (int32_t)(candidate - reference) > 0;
}

static inline i2s_shallow_dma_owner_t i2s_shallow_dma_owner_get(
    const i2s_shallow_dma_descriptor_t* descriptor) {
    return (i2s_shallow_dma_owner_t)__atomic_load_n(&descriptor->owner, __ATOMIC_ACQUIRE);
}

static inline bool i2s_shallow_dma_owner_transition_is_legal(i2s_shallow_dma_owner_t from,
                                                              i2s_shallow_dma_owner_t to) {
    return (from == I2S_SHALLOW_DMA_OWNER_SOFTWARE && to == I2S_SHALLOW_DMA_OWNER_REFILLING) ||
           (from == I2S_SHALLOW_DMA_OWNER_REFILLING && to == I2S_SHALLOW_DMA_OWNER_READY) ||
           (from == I2S_SHALLOW_DMA_OWNER_READY && to == I2S_SHALLOW_DMA_OWNER_ACTIVE) ||
           (from == I2S_SHALLOW_DMA_OWNER_ACTIVE && to == I2S_SHALLOW_DMA_OWNER_REFILLING);
}

static inline void i2s_shallow_dma_latch_fault(i2s_shallow_dma_state_t* state,
                                                i2s_shallow_dma_fault_t fault) {
    if (state->faulted) {
        return;
    }

    state->faulted          = true;
    state->fault_reason     = fault;
    state->running          = false;
    state->planner_active   = false;
    state->auxiliary_c_active = false;

    switch (fault) {
        case I2S_SHALLOW_DMA_FAULT_OWNERSHIP:
            i2s_shallow_dma_atomic_increment(&state->diagnostics.ownership_fault_count);
            break;
        case I2S_SHALLOW_DMA_FAULT_STARVATION:
            i2s_shallow_dma_atomic_increment(&state->diagnostics.starvation_fault_count);
            break;
        case I2S_SHALLOW_DMA_FAULT_DESCRIPTOR:
            i2s_shallow_dma_atomic_increment(&state->diagnostics.descriptor_fault_count);
            break;
        case I2S_SHALLOW_DMA_FAULT_STALE_REPLAY:
            i2s_shallow_dma_atomic_increment(&state->diagnostics.stale_replay_fault_count);
            break;
        case I2S_SHALLOW_DMA_FAULT_COMPLETION:
            i2s_shallow_dma_atomic_increment(&state->diagnostics.completion_fault_count);
            break;
        case I2S_SHALLOW_DMA_FAULT_PLANNER_C_CONFLICT:
            i2s_shallow_dma_atomic_increment(&state->diagnostics.planner_c_conflict_count);
            break;
        case I2S_SHALLOW_DMA_FAULT_CHUCK_MODE:
            i2s_shallow_dma_atomic_increment(&state->diagnostics.chuck_mode_fault_count);
            break;
        case I2S_SHALLOW_DMA_FAULT_NONE:
            break;
    }
}

static inline void i2s_shallow_dma_init(i2s_shallow_dma_state_t* state) {
    uint8_t index;
    for (index = 0; index < I2S_SHALLOW_DMA_DESCRIPTOR_COUNT; ++index) {
        state->descriptors[index].owner                = (uint8_t)I2S_SHALLOW_DMA_OWNER_SOFTWARE;
        state->descriptors[index].ring_index           = index;
        state->descriptors[index].reserved             = 0u;
        state->descriptors[index].refill_sequence      = 0u;
        state->descriptors[index].transmitted_sequence = 0u;
        state->descriptors[index].static_revision      = 0u;
        state->descriptors[index].completion_motion    = 0u;
    }

    __atomic_store_n(&state->diagnostics.refill_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&state->diagnostics.frame_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&state->diagnostics.transmission_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&state->diagnostics.eof_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&state->diagnostics.max_refill_us, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&state->diagnostics.planner_start_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&state->diagnostics.planner_completion_mark_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&state->diagnostics.planner_physical_completion_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&state->diagnostics.planner_reset_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&state->diagnostics.ownership_fault_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&state->diagnostics.starvation_fault_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&state->diagnostics.descriptor_fault_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&state->diagnostics.stale_replay_fault_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&state->diagnostics.completion_fault_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&state->diagnostics.planner_c_conflict_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&state->diagnostics.chuck_mode_fault_count, 0u, __ATOMIC_RELAXED);

    state->chuck_mode                 = I2S_SHALLOW_CHUCK_IDLE;
    state->fault_reason               = I2S_SHALLOW_DMA_FAULT_NONE;
    state->latest_static_revision     = 0u;
    state->retired_static_revision    = 0u;
    state->next_motion                = 0u;
    state->active_motion              = 0u;
    state->marker_motion              = 0u;
    state->pending_motion             = 0u;
    state->next_descriptor            = 0u;
    state->active_descriptor          = I2S_SHALLOW_DMA_NO_DESCRIPTOR;
    state->marker_descriptor          = I2S_SHALLOW_DMA_NO_DESCRIPTOR;
    state->running                    = false;
    state->planner_active             = false;
    state->auxiliary_c_active         = false;
    state->faulted                    = false;
    state->completion_marker_valid    = false;
    state->completion_pending         = false;
}

static inline bool i2s_shallow_dma_descriptor_valid(i2s_shallow_dma_state_t* state,
                                                     uint8_t index) {
    if (index >= I2S_SHALLOW_DMA_DESCRIPTOR_COUNT || state->descriptors[index].ring_index != index ||
        i2s_shallow_dma_owner_get(&state->descriptors[index]) >= I2S_SHALLOW_DMA_OWNER_COUNT) {
        i2s_shallow_dma_latch_fault(state, I2S_SHALLOW_DMA_FAULT_DESCRIPTOR);
        return false;
    }
    return true;
}

static inline bool i2s_shallow_dma_owner_transition(i2s_shallow_dma_state_t* state,
                                                     uint8_t index,
                                                     i2s_shallow_dma_owner_t desired) {
    if (state->faulted || !i2s_shallow_dma_descriptor_valid(state, index)) {
        return false;
    }

    i2s_shallow_dma_descriptor_t* descriptor = &state->descriptors[index];
    const i2s_shallow_dma_owner_t current = i2s_shallow_dma_owner_get(descriptor);
    if (!i2s_shallow_dma_owner_transition_is_legal(current, desired)) {
        i2s_shallow_dma_latch_fault(state, I2S_SHALLOW_DMA_FAULT_OWNERSHIP);
        return false;
    }

    uint8_t expected = (uint8_t)current;
    if (!__atomic_compare_exchange_n(&descriptor->owner,
                                     &expected,
                                     (uint8_t)desired,
                                     false,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        i2s_shallow_dma_latch_fault(state, I2S_SHALLOW_DMA_FAULT_OWNERSHIP);
        return false;
    }
    return true;
}

static inline bool i2s_shallow_dma_claim_initial_refill(i2s_shallow_dma_state_t* state,
                                                         uint8_t index) {
    return i2s_shallow_dma_owner_transition(state, index, I2S_SHALLOW_DMA_OWNER_REFILLING);
}

static inline uint32_t i2s_shallow_dma_queue_static_revision(i2s_shallow_dma_state_t* state) {
    if (state->faulted) {
        return 0u;
    }
    state->latest_static_revision = i2s_shallow_dma_next_nonzero(state->latest_static_revision);
    return state->latest_static_revision;
}

static inline bool i2s_shallow_dma_finish_refill(i2s_shallow_dma_state_t* state,
                                                  uint8_t index,
                                                  uint32_t static_revision,
                                                  uint32_t frame_count,
                                                  uint32_t refill_us) {
    if (state->faulted || !i2s_shallow_dma_descriptor_valid(state, index)) {
        return false;
    }

    if (frame_count != I2S_SHALLOW_DMA_FRAMES_PER_DESCRIPTOR ||
        i2s_shallow_dma_serial_newer(static_revision, state->latest_static_revision)) {
        i2s_shallow_dma_latch_fault(state, I2S_SHALLOW_DMA_FAULT_DESCRIPTOR);
        return false;
    }

    i2s_shallow_dma_descriptor_t* descriptor = &state->descriptors[index];
    if (i2s_shallow_dma_owner_get(descriptor) != I2S_SHALLOW_DMA_OWNER_REFILLING) {
        i2s_shallow_dma_latch_fault(state, I2S_SHALLOW_DMA_FAULT_OWNERSHIP);
        return false;
    }

    descriptor->static_revision = static_revision;
    descriptor->refill_sequence = i2s_shallow_dma_next_nonzero(descriptor->refill_sequence);
    if (!i2s_shallow_dma_owner_transition(state, index, I2S_SHALLOW_DMA_OWNER_READY)) {
        return false;
    }

    i2s_shallow_dma_atomic_increment(&state->diagnostics.refill_count);
    i2s_shallow_dma_atomic_add(&state->diagnostics.frame_count, frame_count);
    i2s_shallow_dma_atomic_max(&state->diagnostics.max_refill_us, refill_us);
    return true;
}

static inline bool i2s_shallow_dma_start(i2s_shallow_dma_state_t* state) {
    uint8_t index;
    if (state->faulted) {
        return false;
    }
    for (index = 0; index < I2S_SHALLOW_DMA_DESCRIPTOR_COUNT; ++index) {
        if (!i2s_shallow_dma_descriptor_valid(state, index)) {
            return false;
        }
        const i2s_shallow_dma_descriptor_t* descriptor = &state->descriptors[index];
        if (i2s_shallow_dma_owner_get(descriptor) != I2S_SHALLOW_DMA_OWNER_READY) {
            i2s_shallow_dma_latch_fault(state, I2S_SHALLOW_DMA_FAULT_STARVATION);
            return false;
        }
        if (descriptor->refill_sequence == descriptor->transmitted_sequence) {
            i2s_shallow_dma_latch_fault(state, I2S_SHALLOW_DMA_FAULT_STALE_REPLAY);
            return false;
        }
    }
    state->running           = true;
    state->next_descriptor   = 0u;
    state->active_descriptor = I2S_SHALLOW_DMA_NO_DESCRIPTOR;
    return true;
}

static inline bool i2s_shallow_dma_begin_transmit(i2s_shallow_dma_state_t* state,
                                                   uint8_t index) {
    if (state->faulted || !state->running || !i2s_shallow_dma_descriptor_valid(state, index)) {
        return false;
    }
    if (state->active_descriptor != I2S_SHALLOW_DMA_NO_DESCRIPTOR || index != state->next_descriptor) {
        i2s_shallow_dma_latch_fault(state, I2S_SHALLOW_DMA_FAULT_DESCRIPTOR);
        return false;
    }

    i2s_shallow_dma_descriptor_t* descriptor = &state->descriptors[index];
    if (i2s_shallow_dma_owner_get(descriptor) != I2S_SHALLOW_DMA_OWNER_READY) {
        i2s_shallow_dma_latch_fault(state, I2S_SHALLOW_DMA_FAULT_STARVATION);
        return false;
    }
    if (descriptor->refill_sequence == descriptor->transmitted_sequence) {
        i2s_shallow_dma_latch_fault(state, I2S_SHALLOW_DMA_FAULT_STALE_REPLAY);
        return false;
    }
    if (!i2s_shallow_dma_owner_transition(state, index, I2S_SHALLOW_DMA_OWNER_ACTIVE)) {
        return false;
    }

    descriptor->transmitted_sequence = descriptor->refill_sequence;
    state->active_descriptor          = index;
    i2s_shallow_dma_atomic_increment(&state->diagnostics.transmission_count);
    return true;
}

static inline bool i2s_shallow_dma_planner_begin(i2s_shallow_dma_state_t* state,
                                                  uint32_t* motion) {
    if (state->faulted || !state->running || state->planner_active ||
        state->completion_marker_valid || state->completion_pending) {
        return false;
    }
    state->next_motion    = i2s_shallow_dma_next_nonzero(state->next_motion);
    state->active_motion  = state->next_motion;
    state->planner_active = true;
    if (motion != 0) {
        *motion = state->active_motion;
    }
    i2s_shallow_dma_atomic_increment(&state->diagnostics.planner_start_count);
    return true;
}

static inline bool i2s_shallow_dma_mark_planner_completion(i2s_shallow_dma_state_t* state,
                                                            uint8_t index) {
    if (state->faulted || !i2s_shallow_dma_descriptor_valid(state, index)) {
        return false;
    }
    i2s_shallow_dma_descriptor_t* descriptor = &state->descriptors[index];
    if (!state->planner_active || state->active_motion == 0u ||
        state->completion_marker_valid || state->completion_pending ||
        i2s_shallow_dma_owner_get(descriptor) != I2S_SHALLOW_DMA_OWNER_REFILLING ||
        descriptor->completion_motion != 0u) {
        i2s_shallow_dma_latch_fault(state, I2S_SHALLOW_DMA_FAULT_COMPLETION);
        return false;
    }

    descriptor->completion_motion = state->active_motion;
    state->marker_motion           = state->active_motion;
    state->marker_descriptor       = index;
    state->completion_marker_valid = true;
    state->planner_active          = false;
    state->active_motion           = 0u;
    i2s_shallow_dma_atomic_increment(&state->diagnostics.planner_completion_mark_count);
    return true;
}

static inline void i2s_shallow_dma_planner_reset(i2s_shallow_dma_state_t* state) {
    uint8_t index;
    state->planner_active          = false;
    state->active_motion           = 0u;
    state->marker_motion           = 0u;
    state->pending_motion          = 0u;
    state->marker_descriptor       = I2S_SHALLOW_DMA_NO_DESCRIPTOR;
    state->completion_marker_valid = false;
    state->completion_pending      = false;
    for (index = 0; index < I2S_SHALLOW_DMA_DESCRIPTOR_COUNT; ++index) {
        state->descriptors[index].completion_motion = 0u;
    }
    i2s_shallow_dma_atomic_increment(&state->diagnostics.planner_reset_count);
}

static inline bool i2s_shallow_dma_physical_eof(i2s_shallow_dma_state_t* state,
                                                 uint8_t index) {
    if (state->faulted || !state->running || !i2s_shallow_dma_descriptor_valid(state, index)) {
        return false;
    }
    if (state->active_descriptor != index ||
        i2s_shallow_dma_owner_get(&state->descriptors[index]) != I2S_SHALLOW_DMA_OWNER_ACTIVE) {
        i2s_shallow_dma_latch_fault(state, I2S_SHALLOW_DMA_FAULT_OWNERSHIP);
        return false;
    }

    i2s_shallow_dma_descriptor_t* descriptor = &state->descriptors[index];
    if (i2s_shallow_dma_serial_newer(descriptor->static_revision, state->retired_static_revision)) {
        state->retired_static_revision = descriptor->static_revision;
    }

    if (descriptor->completion_motion != 0u) {
        if (!state->completion_marker_valid || state->completion_pending ||
            state->marker_descriptor != index ||
            state->marker_motion != descriptor->completion_motion) {
            i2s_shallow_dma_latch_fault(state, I2S_SHALLOW_DMA_FAULT_COMPLETION);
            return false;
        }
        state->pending_motion             = descriptor->completion_motion;
        state->completion_pending         = true;
        state->completion_marker_valid    = false;
        state->marker_motion              = 0u;
        state->marker_descriptor          = I2S_SHALLOW_DMA_NO_DESCRIPTOR;
        descriptor->completion_motion     = 0u;
        i2s_shallow_dma_atomic_increment(&state->diagnostics.planner_physical_completion_count);
    }

    if (!i2s_shallow_dma_owner_transition(state, index, I2S_SHALLOW_DMA_OWNER_REFILLING)) {
        return false;
    }
    state->active_descriptor = I2S_SHALLOW_DMA_NO_DESCRIPTOR;
    state->next_descriptor = (uint8_t)((index + 1u) % I2S_SHALLOW_DMA_DESCRIPTOR_COUNT);
    i2s_shallow_dma_atomic_increment(&state->diagnostics.eof_count);
    return true;
}

static inline bool i2s_shallow_dma_take_planner_completion(i2s_shallow_dma_state_t* state,
                                                            uint32_t* motion) {
    if (state->faulted || !state->completion_pending) {
        return false;
    }
    if (motion != 0) {
        *motion = state->pending_motion;
    }
    state->pending_motion     = 0u;
    state->completion_pending = false;
    return true;
}

static inline bool i2s_shallow_dma_static_revision_retired(const i2s_shallow_dma_state_t* state,
                                                            uint32_t revision) {
    return !state->faulted &&
           (revision == 0u || revision == state->retired_static_revision ||
            i2s_shallow_dma_serial_newer(state->retired_static_revision, revision));
}

static inline bool i2s_shallow_dma_set_chuck_mode(i2s_shallow_dma_state_t* state,
                                                   i2s_shallow_chuck_mode_t mode,
                                                   bool auxiliary_c_active) {
    if (state->faulted || (int)mode < (int)I2S_SHALLOW_CHUCK_IDLE ||
        mode > I2S_SHALLOW_CHUCK_SPINDLE ||
        (auxiliary_c_active && mode != I2S_SHALLOW_CHUCK_SPINDLE) ||
        (!auxiliary_c_active && mode == I2S_SHALLOW_CHUCK_SPINDLE)) {
        i2s_shallow_dma_latch_fault(state, I2S_SHALLOW_DMA_FAULT_CHUCK_MODE);
        return false;
    }
    state->chuck_mode        = mode;
    state->auxiliary_c_active = auxiliary_c_active;
    return true;
}

// The planner word is composed first.  In spindle mode, a planner-originated C
// pulse is an ownership violation and no output word is written.  Otherwise
// the DDS C level is overlaid without changing any X/Z bits.
static inline bool i2s_shallow_dma_compose_frame(i2s_shallow_dma_state_t* state,
                                                  uint32_t planner_word,
                                                  bool planner_c_pulse,
                                                  uint32_t c_step_bit,
                                                  bool c_step_active_high,
                                                  bool auxiliary_c_step_asserted,
                                                  uint32_t* output_word) {
    if (state->faulted || output_word == 0) {
        return false;
    }
    if (state->chuck_mode == I2S_SHALLOW_CHUCK_SPINDLE && planner_c_pulse) {
        i2s_shallow_dma_latch_fault(state, I2S_SHALLOW_DMA_FAULT_PLANNER_C_CONFLICT);
        return false;
    }

    uint32_t composed = planner_word;
    if (state->auxiliary_c_active) {
        const bool c_high = auxiliary_c_step_asserted == c_step_active_high;
        if (c_high) {
            composed |= c_step_bit;
        } else {
            composed &= ~c_step_bit;
        }
    }
    *output_word = composed;
    return true;
}
