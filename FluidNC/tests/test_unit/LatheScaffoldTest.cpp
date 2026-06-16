#include "../src/Lathe.h"

#include <gtest/gtest.h>

TEST(LatheScaffold, FeedbackStatusDefaultsToNoHardwareCapabilities) {
    Lathe::FeedbackStatus status;

    EXPECT_EQ(status.commanded_rpm, 0);
    EXPECT_EQ(status.measured_rpm, 0);
    EXPECT_EQ(status.timestamp_ms, 0u);
    EXPECT_FALSE(status.has_measured_rpm);
    EXPECT_FALSE(status.has_index_pulse);
    EXPECT_FALSE(status.has_angular_position);
    EXPECT_FALSE(status.stale);
    EXPECT_FALSE(status.fault);
}

TEST(LatheScaffold, NullSpindleFeedbackCannotSynchronizeThreadingStart) {
    Lathe::NullSpindleFeedback feedback;
    EXPECT_FALSE(feedback.synchronize_for_threading_start());
}

TEST(LatheScaffold, NullSpindleFeedbackReportsNoHardwareCapabilities) {
    Lathe::NullSpindleFeedback feedback;
    auto status = feedback.status();

    EXPECT_FALSE(status.has_measured_rpm);
    EXPECT_FALSE(status.has_index_pulse);
    EXPECT_FALSE(status.has_angular_position);
    EXPECT_FALSE(status.stale);
    EXPECT_FALSE(status.fault);
}

TEST(LatheScaffold, CssRpmMathUsesMetricDiameterAndNearCenterClamp) {
    EXPECT_NEAR(Lathe::css_rpm_from_diameter_mm(314.15927f, 100.0f, 1.0f, false), 1.0f, 0.001f);
    EXPECT_NEAR(Lathe::css_rpm_from_diameter_mm(314.15927f, 0.0f, 100.0f, false), 1.0f, 0.001f);
}

TEST(LatheScaffold, CssRpmMathConvertsInchSurfaceSpeedToMillimeters) {
    EXPECT_NEAR(Lathe::css_rpm_from_diameter_mm(12.368475f, 100.0f, 1.0f, true), 1.0f, 0.001f);
}

TEST(LatheScaffold, CssRpmClampLimitsPositiveRpm) {
    EXPECT_FLOAT_EQ(Lathe::clamp_css_rpm(2500.0f, 1200.0f), 1200.0f);
    EXPECT_FLOAT_EQ(Lathe::clamp_css_rpm(800.0f, 1200.0f), 800.0f);
    EXPECT_FLOAT_EQ(Lathe::clamp_css_rpm(-800.0f, 1200.0f), 0.0f);
}

TEST(LatheScaffold, FeedPerRevConvertsToMillimetersPerMinute) {
    EXPECT_NEAR(Lathe::feed_per_rev_to_mm_per_min(0.2f, 500.0f, false), 100.0f, 0.001f);
    EXPECT_NEAR(Lathe::feed_per_rev_to_mm_per_min(0.01f, 1000.0f, true), 254.0f, 0.001f);
}

TEST(LatheScaffold, ThreadingFeedbackRequiresMeasuredRpmIndexAndAngularPosition) {
    Lathe::FeedbackStatus status;

    EXPECT_FALSE(Lathe::feedback_supports_threading(status));

    status.has_measured_rpm      = true;
    status.has_index_pulse       = true;
    status.has_angular_position  = true;
    status.measured_rpm          = 600;
    EXPECT_TRUE(Lathe::feedback_supports_threading(status));

    status.stale = true;
    EXPECT_FALSE(Lathe::feedback_supports_threading(status));
    status.stale = false;

    status.fault = true;
    EXPECT_FALSE(Lathe::feedback_supports_threading(status));
}

TEST(LatheScaffold, XOffsetConvertsDiameterModeToMachineRadiusOffset) {
    EXPECT_FLOAT_EQ(Lathe::x_offset_to_machine_mm(2.0f, Lathe::DiameterMode::Radius), 2.0f);
    EXPECT_FLOAT_EQ(Lathe::x_offset_to_machine_mm(2.0f, Lathe::DiameterMode::Diameter), 1.0f);
}

TEST(LatheScaffold, XDiameterProgrammingConvertsToInternalRadiusCoordinates) {
    EXPECT_FLOAT_EQ(Lathe::x_program_to_machine_mm(24.0f, Lathe::DiameterMode::Diameter), 12.0f);
    EXPECT_FLOAT_EQ(Lathe::x_program_to_machine_mm(12.0f, Lathe::DiameterMode::Radius), 12.0f);
    EXPECT_FLOAT_EQ(Lathe::x_machine_to_diameter_mm(12.0f), 24.0f);
    EXPECT_FLOAT_EQ(Lathe::x_machine_to_diameter_mm(-12.0f), 24.0f);
}

TEST(LatheScaffold, LatheToolDataStoresGeometryWearNoseAndOrientation) {
    Lathe::ToolData tool;
    tool.geometry_x_mm  = 1.0f;
    tool.geometry_z_mm  = 2.0f;
    tool.wear_x_mm      = 0.1f;
    tool.wear_z_mm      = -0.2f;
    tool.nose_radius_mm = 0.4f;
    tool.orientation    = Lathe::InsertOrientation::FrontTurning;

    Lathe::set_tool_data(7, tool);
    auto stored = Lathe::get_tool_data(7);
    ASSERT_TRUE(stored.has_value());
    EXPECT_FLOAT_EQ(stored->geometry_x_mm, 1.0f);
    EXPECT_FLOAT_EQ(stored->wear_z_mm, -0.2f);
    EXPECT_FLOAT_EQ(stored->nose_radius_mm, 0.4f);
    EXPECT_EQ(stored->orientation, Lathe::InsertOrientation::FrontTurning);

    auto active = Lathe::select_tool(7);
    EXPECT_TRUE(active.valid);
    EXPECT_EQ(active.tool_number, 7u);
    EXPECT_FLOAT_EQ(active.x_mm, 1.1f);
    EXPECT_FLOAT_EQ(active.z_mm, 1.8f);
    EXPECT_FLOAT_EQ(active.nose_radius_mm, 0.4f);
}

TEST(LatheScaffold, ThreadingCycleExpandsIntoThreadingPassesAndRetracts) {
    Lathe::ThreadingCycleSpec spec;
    spec.start_x_mm = 20.0f;
    spec.end_x_mm   = 18.0f;
    spec.start_z_mm = 0.0f;
    spec.end_z_mm   = -10.0f;
    spec.pitch_mm   = 1.5f;
    spec.passes     = 2;

    auto plan = Lathe::build_threading_cycle(spec);
    ASSERT_TRUE(plan.valid);
    EXPECT_EQ(plan.count, 3u);
    EXPECT_EQ(plan.moves[0].kind, Lathe::CycleMoveKind::Threading);
    EXPECT_FLOAT_EQ(plan.moves[0].x_mm, 19.0f);
    EXPECT_FLOAT_EQ(plan.moves[0].feed, 1.5f);
    EXPECT_EQ(plan.moves[1].kind, Lathe::CycleMoveKind::Rapid);
    EXPECT_EQ(plan.moves[2].kind, Lathe::CycleMoveKind::Threading);
    EXPECT_FLOAT_EQ(plan.moves[2].x_mm, 18.0f);
}

TEST(LatheScaffold, RoughTurningCycleExpandsIntoRoughingAndFinishMoves) {
    Lathe::RoughTurningCycleSpec spec;
    spec.start_x_mm           = 30.0f;
    spec.final_x_mm           = 26.0f;
    spec.start_z_mm           = 0.0f;
    spec.end_z_mm             = -20.0f;
    spec.depth_step_mm        = 2.0f;
    spec.rough_feed_mm_min    = 120.0f;
    spec.include_finish_pass  = true;

    auto plan = Lathe::build_rough_turning_cycle(spec);
    ASSERT_TRUE(plan.valid);
    EXPECT_EQ(plan.count, 4u);
    EXPECT_EQ(plan.moves[0].kind, Lathe::CycleMoveKind::Linear);
    EXPECT_FLOAT_EQ(plan.moves[0].x_mm, 28.0f);
    EXPECT_EQ(plan.moves[1].kind, Lathe::CycleMoveKind::Rapid);
    EXPECT_FLOAT_EQ(plan.moves[2].x_mm, 26.0f);
    EXPECT_EQ(plan.moves[3].kind, Lathe::CycleMoveKind::Linear);
    EXPECT_FLOAT_EQ(plan.moves[3].feed, 120.0f);
}

TEST(LatheScaffold, CycleValidationRejectsUnsafeGeometry) {
    Lathe::ThreadingCycleSpec threading;
    threading.pitch_mm = 0.0f;
    threading.passes   = 1;
    EXPECT_FALSE(Lathe::build_threading_cycle(threading).valid);

    Lathe::RoughTurningCycleSpec roughing;
    roughing.depth_step_mm     = 0.0f;
    roughing.rough_feed_mm_min = 100.0f;
    EXPECT_FALSE(Lathe::build_rough_turning_cycle(roughing).valid);
}
