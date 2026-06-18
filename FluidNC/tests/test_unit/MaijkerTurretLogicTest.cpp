#include "../src/ToolChangers/maijker_turret_logic.h"

#include <gtest/gtest.h>

using namespace ATCs::MaijkerTurretLogic;

TEST(MaijkerTurretLogic, ForwardDeltaWrapsAcrossStationOne) {
    EXPECT_EQ(forward_delta(1, 2, 5), 1u);
    EXPECT_EQ(forward_delta(4, 1, 5), 2u);
    EXPECT_EQ(forward_delta(5, 1, 5), 1u);
}

TEST(MaijkerTurretLogic, DuplicateToolProducesNoMotion) {
    Config config;
    auto   plan = build_change_plan(config, 3, true, 3);

    EXPECT_EQ(plan.error, Error::None);
    EXPECT_EQ(plan.delta_stations, 0u);
    EXPECT_EQ(plan.forward_steps, 0u);
    EXPECT_EQ(plan.lock_steps, 0u);
}

TEST(MaijkerTurretLogic, ChangePlanUsesNativeStepCounts) {
    Config config;
    config.steps_per_station  = 320;
    config.overshoot_steps    = 32;
    config.lock_backoff_steps = 24;

    auto plan = build_change_plan(config, 1, true, 3);

    EXPECT_EQ(plan.error, Error::None);
    EXPECT_EQ(plan.delta_stations, 2u);
    EXPECT_EQ(plan.forward_steps, 672u);
    EXPECT_EQ(plan.lock_steps, 24u);
}

TEST(MaijkerTurretLogic, InvalidTargetToolFails) {
    Config config;
    EXPECT_EQ(build_change_plan(config, 1, true, 0).error, Error::InvalidTargetTool);
    EXPECT_EQ(build_change_plan(config, 1, true, 6).error, Error::InvalidTargetTool);
}

TEST(MaijkerTurretLogic, UnconfirmedCurrentToolBlocksWhenRequired) {
    Config config;
    config.require_confirmed_tool = true;

    auto plan = build_change_plan(config, 1, false, 2);

    EXPECT_EQ(plan.error, Error::UnconfirmedTool);
}

TEST(MaijkerTurretLogic, InvalidCurrentToolFailsAfterConfirmationCheck) {
    Config config;

    auto plan = build_change_plan(config, 0, true, 2);

    EXPECT_EQ(plan.error, Error::InvalidCurrentTool);
}

TEST(MaijkerTurretLogic, M61StyleConfirmationAllowsFutureMotion) {
    Config config;

    auto blocked = build_change_plan(config, 0, false, 2);
    ASSERT_EQ(blocked.error, Error::UnconfirmedTool);

    auto initialized = build_change_plan(config, 1, true, 2);
    EXPECT_EQ(initialized.error, Error::None);
    EXPECT_EQ(initialized.delta_stations, 1u);
}

TEST(MaijkerTurretLogic, SensorHomeResultRequiresConfiguredSensor) {
    auto result = build_home_result(false, true, 1, 5);

    EXPECT_EQ(result.error, Error::SensorUnavailable);
    EXPECT_FALSE(result.confirmed);
}

TEST(MaijkerTurretLogic, SensorHomeTimeoutDoesNotConfirmTool) {
    auto result = build_home_result(true, false, 1, 5);

    EXPECT_EQ(result.error, Error::SensorTimeout);
    EXPECT_FALSE(result.confirmed);
}

TEST(MaijkerTurretLogic, SensorHomeSuccessConfirmsSensorTool) {
    auto result = build_home_result(true, true, 1, 5);

    EXPECT_EQ(result.error, Error::None);
    EXPECT_EQ(result.tool_number, 1u);
    EXPECT_TRUE(result.confirmed);
}
