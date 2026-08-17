#include "Driver/i2s_shallow_dma.h"

#include <gtest/gtest.h>

namespace {

void initialize_ready_ring(i2s_shallow_dma_state_t& state) {
    i2s_shallow_dma_init(&state);
    for (uint8_t index = 0; index < I2S_SHALLOW_DMA_DESCRIPTOR_COUNT; ++index) {
        ASSERT_TRUE(i2s_shallow_dma_claim_initial_refill(&state, index));
        ASSERT_TRUE(i2s_shallow_dma_finish_refill(
            &state, index, 0u, I2S_SHALLOW_DMA_FRAMES_PER_DESCRIPTOR, 8u + index));
    }
    ASSERT_TRUE(i2s_shallow_dma_start(&state));
}

void retire_and_refill(i2s_shallow_dma_state_t& state, uint8_t index) {
    ASSERT_TRUE(i2s_shallow_dma_begin_transmit(&state, index));
    ASSERT_TRUE(i2s_shallow_dma_physical_eof(&state, index));
    ASSERT_TRUE(i2s_shallow_dma_finish_refill(&state,
                                               index,
                                               state.latest_static_revision,
                                               I2S_SHALLOW_DMA_FRAMES_PER_DESCRIPTOR,
                                               7u));
}

}  // namespace

TEST(I2sShallowDmaModel, GeometryIsFourByThirtyTwoAtTwoMicroseconds) {
    EXPECT_EQ(I2S_SHALLOW_DMA_DESCRIPTOR_COUNT, 4u);
    EXPECT_EQ(I2S_SHALLOW_DMA_FRAMES_PER_DESCRIPTOR, 32u);
    EXPECT_EQ(I2S_SHALLOW_DMA_DESCRIPTOR_PERIOD_US, 64u);
    EXPECT_EQ(I2S_SHALLOW_DMA_RING_PERIOD_US, 256u);
}

TEST(I2sShallowDmaModel, EveryLegalOwnerTransitionSucceedsAndEveryIllegalOneFaults) {
    for (uint8_t from = 0; from < I2S_SHALLOW_DMA_OWNER_COUNT; ++from) {
        for (uint8_t to = 0; to < I2S_SHALLOW_DMA_OWNER_COUNT; ++to) {
            i2s_shallow_dma_state_t state;
            i2s_shallow_dma_init(&state);
            state.descriptors[0].owner = from;

            const bool expected = i2s_shallow_dma_owner_transition_is_legal(
                static_cast<i2s_shallow_dma_owner_t>(from),
                static_cast<i2s_shallow_dma_owner_t>(to));
            EXPECT_EQ(i2s_shallow_dma_owner_transition(
                          &state, 0u, static_cast<i2s_shallow_dma_owner_t>(to)),
                      expected)
                << "from=" << unsigned(from) << " to=" << unsigned(to);

            if (expected) {
                EXPECT_FALSE(state.faulted);
                EXPECT_EQ(i2s_shallow_dma_owner_get(&state.descriptors[0]), to);
            } else {
                EXPECT_TRUE(state.faulted);
                EXPECT_EQ(state.fault_reason, I2S_SHALLOW_DMA_FAULT_OWNERSHIP);
                EXPECT_EQ(i2s_shallow_dma_atomic_load(&state.diagnostics.ownership_fault_count), 1u);
            }
        }
    }
}

TEST(I2sShallowDmaModel, DescriptorCannotReachHardwareTwiceWithoutRefill) {
    i2s_shallow_dma_state_t state;
    initialize_ready_ring(state);

    ASSERT_TRUE(i2s_shallow_dma_begin_transmit(&state, 0u));
    ASSERT_TRUE(i2s_shallow_dma_physical_eof(&state, 0u));
    for (uint8_t index = 1u; index < I2S_SHALLOW_DMA_DESCRIPTOR_COUNT; ++index) {
        retire_and_refill(state, index);
    }

    EXPECT_FALSE(i2s_shallow_dma_begin_transmit(&state, 0u));
    EXPECT_TRUE(state.faulted);
    EXPECT_FALSE(state.running);
    EXPECT_EQ(state.fault_reason, I2S_SHALLOW_DMA_FAULT_STARVATION);
    EXPECT_EQ(i2s_shallow_dma_atomic_load(&state.diagnostics.starvation_fault_count), 1u);
}

TEST(I2sShallowDmaModel, ReplayedRefillSequenceFailsClosed) {
    i2s_shallow_dma_state_t state;
    i2s_shallow_dma_init(&state);
    for (uint8_t index = 0; index < I2S_SHALLOW_DMA_DESCRIPTOR_COUNT; ++index) {
        ASSERT_TRUE(i2s_shallow_dma_claim_initial_refill(&state, index));
        ASSERT_TRUE(i2s_shallow_dma_finish_refill(
            &state, index, 0u, I2S_SHALLOW_DMA_FRAMES_PER_DESCRIPTOR, 5u));
    }
    state.descriptors[2].transmitted_sequence = state.descriptors[2].refill_sequence;

    EXPECT_FALSE(i2s_shallow_dma_start(&state));
    EXPECT_TRUE(state.faulted);
    EXPECT_EQ(state.fault_reason, I2S_SHALLOW_DMA_FAULT_STALE_REPLAY);
    EXPECT_EQ(i2s_shallow_dma_atomic_load(&state.diagnostics.stale_replay_fault_count), 1u);
}

TEST(I2sShallowDmaModel, TwoFinitePlannerMotionsPublishOnlyAtTheirMarkedPhysicalEof) {
    i2s_shallow_dma_state_t state;
    initialize_ready_ring(state);

    uint32_t first_motion = 0u;
    ASSERT_TRUE(i2s_shallow_dma_planner_begin(&state, &first_motion));
    ASSERT_NE(first_motion, 0u);

    ASSERT_TRUE(i2s_shallow_dma_begin_transmit(&state, 0u));
    ASSERT_TRUE(i2s_shallow_dma_physical_eof(&state, 0u));
    ASSERT_TRUE(i2s_shallow_dma_mark_planner_completion(&state, 0u));
    ASSERT_TRUE(i2s_shallow_dma_finish_refill(
        &state, 0u, 0u, I2S_SHALLOW_DMA_FRAMES_PER_DESCRIPTOR, 6u));

    uint32_t completed_motion = 0u;
    EXPECT_FALSE(i2s_shallow_dma_take_planner_completion(&state, &completed_motion));
    for (uint8_t index = 1u; index < I2S_SHALLOW_DMA_DESCRIPTOR_COUNT; ++index) {
        retire_and_refill(state, index);
        EXPECT_FALSE(i2s_shallow_dma_take_planner_completion(&state, &completed_motion));
    }
    ASSERT_TRUE(i2s_shallow_dma_begin_transmit(&state, 0u));
    ASSERT_TRUE(i2s_shallow_dma_physical_eof(&state, 0u));
    EXPECT_TRUE(state.completion_pending);
    ASSERT_TRUE(i2s_shallow_dma_take_planner_completion(&state, &completed_motion));
    EXPECT_EQ(completed_motion, first_motion);
    ASSERT_TRUE(i2s_shallow_dma_finish_refill(
        &state, 0u, 0u, I2S_SHALLOW_DMA_FRAMES_PER_DESCRIPTOR, 6u));

    uint32_t second_motion = 0u;
    ASSERT_TRUE(i2s_shallow_dma_planner_begin(&state, &second_motion));
    ASSERT_NE(second_motion, first_motion);
    ASSERT_TRUE(i2s_shallow_dma_begin_transmit(&state, 1u));
    ASSERT_TRUE(i2s_shallow_dma_physical_eof(&state, 1u));
    ASSERT_TRUE(i2s_shallow_dma_mark_planner_completion(&state, 1u));
    ASSERT_TRUE(i2s_shallow_dma_finish_refill(
        &state, 1u, 0u, I2S_SHALLOW_DMA_FRAMES_PER_DESCRIPTOR, 6u));

    retire_and_refill(state, 2u);
    retire_and_refill(state, 3u);
    retire_and_refill(state, 0u);
    EXPECT_FALSE(i2s_shallow_dma_take_planner_completion(&state, &completed_motion));
    ASSERT_TRUE(i2s_shallow_dma_begin_transmit(&state, 1u));
    ASSERT_TRUE(i2s_shallow_dma_physical_eof(&state, 1u));
    ASSERT_TRUE(i2s_shallow_dma_take_planner_completion(&state, &completed_motion));
    EXPECT_EQ(completed_motion, second_motion);

    EXPECT_EQ(i2s_shallow_dma_atomic_load(&state.diagnostics.planner_start_count), 2u);
    EXPECT_EQ(i2s_shallow_dma_atomic_load(&state.diagnostics.planner_completion_mark_count), 2u);
    EXPECT_EQ(i2s_shallow_dma_atomic_load(&state.diagnostics.planner_physical_completion_count), 2u);
}

TEST(I2sShallowDmaModel, PlannerCannotRearmUntilMarkedCompletionIsConsumed) {
    i2s_shallow_dma_state_t state;
    initialize_ready_ring(state);

    ASSERT_TRUE(i2s_shallow_dma_planner_begin(&state, nullptr));
    ASSERT_TRUE(i2s_shallow_dma_begin_transmit(&state, 0u));
    ASSERT_TRUE(i2s_shallow_dma_physical_eof(&state, 0u));
    ASSERT_TRUE(i2s_shallow_dma_mark_planner_completion(&state, 0u));
    ASSERT_TRUE(i2s_shallow_dma_finish_refill(
        &state, 0u, 0u, I2S_SHALLOW_DMA_FRAMES_PER_DESCRIPTOR, 6u));
    EXPECT_FALSE(i2s_shallow_dma_planner_begin(&state, nullptr));
    EXPECT_FALSE(state.faulted);

    retire_and_refill(state, 1u);
    retire_and_refill(state, 2u);
    retire_and_refill(state, 3u);
    ASSERT_TRUE(i2s_shallow_dma_begin_transmit(&state, 0u));
    ASSERT_TRUE(i2s_shallow_dma_physical_eof(&state, 0u));
    EXPECT_FALSE(i2s_shallow_dma_planner_begin(&state, nullptr));
    ASSERT_TRUE(i2s_shallow_dma_take_planner_completion(&state, nullptr));
    EXPECT_TRUE(i2s_shallow_dma_planner_begin(&state, nullptr));
}

TEST(I2sShallowDmaModel, PlannerResetInvalidatesCompletionBeforePhysicalEof) {
    i2s_shallow_dma_state_t state;
    initialize_ready_ring(state);

    ASSERT_TRUE(i2s_shallow_dma_planner_begin(&state, nullptr));
    ASSERT_TRUE(i2s_shallow_dma_begin_transmit(&state, 0u));
    ASSERT_TRUE(i2s_shallow_dma_physical_eof(&state, 0u));
    ASSERT_TRUE(i2s_shallow_dma_mark_planner_completion(&state, 0u));
    ASSERT_TRUE(i2s_shallow_dma_finish_refill(
        &state, 0u, 0u, I2S_SHALLOW_DMA_FRAMES_PER_DESCRIPTOR, 6u));

    i2s_shallow_dma_planner_reset(&state);
    retire_and_refill(state, 1u);
    retire_and_refill(state, 2u);
    retire_and_refill(state, 3u);
    ASSERT_TRUE(i2s_shallow_dma_begin_transmit(&state, 0u));
    ASSERT_TRUE(i2s_shallow_dma_physical_eof(&state, 0u));

    EXPECT_FALSE(state.completion_pending);
    EXPECT_FALSE(i2s_shallow_dma_take_planner_completion(&state, nullptr));
    EXPECT_EQ(i2s_shallow_dma_atomic_load(&state.diagnostics.planner_physical_completion_count), 0u);
    EXPECT_EQ(i2s_shallow_dma_atomic_load(&state.diagnostics.planner_reset_count), 1u);
}

TEST(I2sShallowDmaModel, PlannerFrameIsUnchangedWhenDdsIsInactive) {
    i2s_shallow_dma_state_t state;
    i2s_shallow_dma_init(&state);
    ASSERT_TRUE(i2s_shallow_dma_set_chuck_mode(
        &state, I2S_SHALLOW_CHUCK_C_POSITIONING, false));

    uint32_t output = 0u;
    ASSERT_TRUE(i2s_shallow_dma_compose_frame(
        &state, 0x5au, true, 0x20u, true, false, &output));
    EXPECT_EQ(output, 0x5au);
}

TEST(I2sShallowDmaModel, DdsCOverlayPreservesPlannerXAndZBits) {
    i2s_shallow_dma_state_t state;
    i2s_shallow_dma_init(&state);
    ASSERT_TRUE(i2s_shallow_dma_set_chuck_mode(&state, I2S_SHALLOW_CHUCK_SPINDLE, true));

    constexpr uint32_t x_and_z = 0x0au;
    constexpr uint32_t c_step  = 0x20u;
    uint32_t output            = 0u;
    ASSERT_TRUE(i2s_shallow_dma_compose_frame(
        &state, x_and_z, false, c_step, true, true, &output));
    EXPECT_EQ(output, x_and_z | c_step);

    ASSERT_TRUE(i2s_shallow_dma_compose_frame(
        &state, x_and_z | c_step, false, c_step, true, false, &output));
    EXPECT_EQ(output, x_and_z);
}

TEST(I2sShallowDmaModel, PlannerCPulseDuringSpindleOwnershipFaultsBeforeOutput) {
    i2s_shallow_dma_state_t state;
    i2s_shallow_dma_init(&state);
    ASSERT_TRUE(i2s_shallow_dma_set_chuck_mode(&state, I2S_SHALLOW_CHUCK_SPINDLE, true));

    uint32_t output = 0xdeadbeefu;
    EXPECT_FALSE(i2s_shallow_dma_compose_frame(
        &state, 0x0au, true, 0x20u, true, true, &output));
    EXPECT_EQ(output, 0xdeadbeefu);
    EXPECT_TRUE(state.faulted);
    EXPECT_EQ(state.fault_reason, I2S_SHALLOW_DMA_FAULT_PLANNER_C_CONFLICT);
    EXPECT_EQ(i2s_shallow_dma_atomic_load(&state.diagnostics.planner_c_conflict_count), 1u);
}

TEST(I2sShallowDmaModel, EnableAndDirectionRevisionsRetireInPhysicalOrder) {
    i2s_shallow_dma_state_t state;
    i2s_shallow_dma_init(&state);

    const uint32_t enable_revision = i2s_shallow_dma_queue_static_revision(&state);
    ASSERT_TRUE(i2s_shallow_dma_claim_initial_refill(&state, 0u));
    ASSERT_TRUE(i2s_shallow_dma_finish_refill(
        &state, 0u, enable_revision, I2S_SHALLOW_DMA_FRAMES_PER_DESCRIPTOR, 5u));

    const uint32_t direction_revision = i2s_shallow_dma_queue_static_revision(&state);
    for (uint8_t index = 1u; index < I2S_SHALLOW_DMA_DESCRIPTOR_COUNT; ++index) {
        ASSERT_TRUE(i2s_shallow_dma_claim_initial_refill(&state, index));
        ASSERT_TRUE(i2s_shallow_dma_finish_refill(
            &state, index, direction_revision, I2S_SHALLOW_DMA_FRAMES_PER_DESCRIPTOR, 5u));
    }
    ASSERT_TRUE(i2s_shallow_dma_start(&state));

    EXPECT_FALSE(i2s_shallow_dma_static_revision_retired(&state, enable_revision));
    ASSERT_TRUE(i2s_shallow_dma_begin_transmit(&state, 0u));
    ASSERT_TRUE(i2s_shallow_dma_physical_eof(&state, 0u));
    EXPECT_TRUE(i2s_shallow_dma_static_revision_retired(&state, enable_revision));
    EXPECT_FALSE(i2s_shallow_dma_static_revision_retired(&state, direction_revision));
    ASSERT_TRUE(i2s_shallow_dma_finish_refill(&state,
                                               0u,
                                               direction_revision,
                                               I2S_SHALLOW_DMA_FRAMES_PER_DESCRIPTOR,
                                               5u));

    ASSERT_TRUE(i2s_shallow_dma_begin_transmit(&state, 1u));
    ASSERT_TRUE(i2s_shallow_dma_physical_eof(&state, 1u));
    EXPECT_TRUE(i2s_shallow_dma_static_revision_retired(&state, direction_revision));
}

TEST(I2sShallowDmaModel, CorruptDescriptorAndWrongFrameCountFailClosed) {
    i2s_shallow_dma_state_t corrupt;
    initialize_ready_ring(corrupt);
    ASSERT_TRUE(i2s_shallow_dma_begin_transmit(&corrupt, 0u));
    corrupt.descriptors[0].ring_index = 3u;
    EXPECT_FALSE(i2s_shallow_dma_physical_eof(&corrupt, 0u));
    EXPECT_TRUE(corrupt.faulted);
    EXPECT_FALSE(corrupt.running);
    EXPECT_EQ(corrupt.fault_reason, I2S_SHALLOW_DMA_FAULT_DESCRIPTOR);

    i2s_shallow_dma_state_t short_refill;
    i2s_shallow_dma_init(&short_refill);
    ASSERT_TRUE(i2s_shallow_dma_claim_initial_refill(&short_refill, 0u));
    EXPECT_FALSE(i2s_shallow_dma_finish_refill(
        &short_refill, 0u, 0u, I2S_SHALLOW_DMA_FRAMES_PER_DESCRIPTOR - 1u, 5u));
    EXPECT_TRUE(short_refill.faulted);
    EXPECT_EQ(short_refill.fault_reason, I2S_SHALLOW_DMA_FAULT_DESCRIPTOR);
}

TEST(I2sShallowDmaModel, DiagnosticsRemainScalarAndTrackRefillMaximum) {
    i2s_shallow_dma_state_t state;
    initialize_ready_ring(state);

    EXPECT_EQ(i2s_shallow_dma_atomic_load(&state.diagnostics.refill_count), 4u);
    EXPECT_EQ(i2s_shallow_dma_atomic_load(&state.diagnostics.frame_count), 128u);
    EXPECT_EQ(i2s_shallow_dma_atomic_load(&state.diagnostics.max_refill_us), 11u);
    EXPECT_EQ(sizeof(state.diagnostics.max_refill_us), sizeof(uint32_t));
}
