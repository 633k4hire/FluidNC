#include "Driver/i2s_fractional_timing.h"

#include <gtest/gtest.h>

#include <cstdint>

TEST(I2sFractionalTiming, AmassLevelThreeIntervalsTotalExactlyOneCStepPeriod) {
    i2s_fractional_timing_t timing;
    i2s_fractional_timing_init(&timing, 40);
    i2s_fractional_timing_set_interval(&timing, 1875);

    const uint32_t expected_frames[] = { 46, 47, 47, 47, 47, 47, 47, 47 };
    uint32_t       total_frames      = 0;

    for (uint32_t expected : expected_frames) {
        const uint32_t frames = i2s_fractional_timing_next(&timing);
        EXPECT_EQ(frames, expected);
        total_frames += frames;
    }

    EXPECT_EQ(total_frames, 375u);
    EXPECT_EQ(timing.residual_ticks, 0u);
}

TEST(I2sFractionalTiming, CumulativeFramesMatchCumulativeTicksAcrossPeriodChanges) {
    i2s_fractional_timing_t timing;
    i2s_fractional_timing_init(&timing, 40);

    uint64_t total_ticks  = 0;
    uint64_t total_frames = 0;
    for (uint32_t interval = 0; interval < 4096; ++interval) {
        const uint32_t ticks = 1200 + (interval % 17) * 37;
        total_ticks += ticks;
        i2s_fractional_timing_set_interval(&timing, ticks);
        total_frames += i2s_fractional_timing_next(&timing);

        EXPECT_EQ(total_frames, total_ticks / 40);
        EXPECT_EQ(timing.residual_ticks, total_ticks % 40);
        EXPECT_LT(timing.residual_ticks, 40u);
    }
}

TEST(I2sFractionalTiming, ResetFencesResidualTicksBetweenPlannerRuns) {
    i2s_fractional_timing_t timing;
    i2s_fractional_timing_init(&timing, 40);
    i2s_fractional_timing_set_interval(&timing, 1875);

    EXPECT_EQ(i2s_fractional_timing_next(&timing), 46u);
    EXPECT_EQ(timing.residual_ticks, 35u);

    i2s_fractional_timing_reset(&timing);

    EXPECT_EQ(timing.residual_ticks, 0u);
    EXPECT_EQ(i2s_fractional_timing_next(&timing), 46u);
    EXPECT_EQ(timing.residual_ticks, 35u);
}

TEST(I2sFractionalTiming, MinimumSupportedIntervalLeavesPulseLowTime) {
    i2s_fractional_timing_t timing;
    i2s_fractional_timing_init(&timing, 40);
    i2s_fractional_timing_set_interval(&timing, 160);

    const uint32_t interval_frames = i2s_fractional_timing_next(&timing);
    const uint32_t pulse_frames    = 2;

    ASSERT_GE(interval_frames, pulse_frames);
    EXPECT_EQ(interval_frames - pulse_frames, 2u);
    EXPECT_EQ(timing.residual_ticks, 0u);
}
