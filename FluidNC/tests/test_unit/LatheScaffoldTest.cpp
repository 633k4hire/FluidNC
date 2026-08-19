#include "../src/Lathe.h"
#include "../src/LatheEncoder.h"
#include "../src/ContinuousStepperLogic.h"
#include "../src/ContinuousEventScheduler.h"
#include "../src/Spindles/CStepperSpindleLogic.h"

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

TEST(LatheScaffold, ThreadingFeedbackRequiresMeasuredRpmIndexAngleAndDirection) {
    Lathe::FeedbackStatus status;

    EXPECT_FALSE(Lathe::feedback_supports_threading(status));

    status.has_measured_rpm      = true;
    status.has_index_pulse       = true;
    status.has_angular_position  = true;
    status.has_direction         = true;
    status.measured_direction    = 1;
    status.measured_rpm          = 600;
    EXPECT_TRUE(Lathe::feedback_supports_threading(status));

    status.stale = true;
    EXPECT_FALSE(Lathe::feedback_supports_threading(status));
    status.stale = false;

    status.fault = true;
    EXPECT_FALSE(Lathe::feedback_supports_threading(status));
}

TEST(LatheScaffold, EncoderFeedbackComputesRpmPhaseAndStaleState) {
    Lathe::EncoderSpindleFeedback feedback;
    feedback.configure(100, 250);
    feedback.set_commanded_rpm(600);
    feedback.record_index(1000000);
    feedback.record_pulse(1001000, 1);
    feedback.record_pulse(1002000, 1);

    auto status = feedback.status_at(1002);
    EXPECT_TRUE(status.has_measured_rpm);
    EXPECT_TRUE(status.has_index_pulse);
    EXPECT_TRUE(status.has_angular_position);
    EXPECT_FALSE(status.stale);
    EXPECT_EQ(status.commanded_rpm, 600);
    EXPECT_NEAR(status.measured_rpm, 600.0f, 0.001f);
    EXPECT_TRUE(status.has_direction);
    EXPECT_EQ(status.measured_direction, 1);
    EXPECT_EQ(status.pulse_count, 2u);
    EXPECT_EQ(status.index_count, 1u);
    EXPECT_NEAR(status.angular_position_rev, 0.02f, 0.001f);
    EXPECT_TRUE(Lathe::feedback_supports_threading(status));

    auto stale = feedback.status_at(2000);
    EXPECT_TRUE(stale.stale);
    EXPECT_FALSE(Lathe::feedback_supports_threading(stale));
}

TEST(LatheScaffold, EncoderTimingTraceRetainsHighResolutionWindows) {
    Lathe::EncoderSpindleFeedback feedback;
    feedback.configure(1000, 250);
    for (uint32_t pulse = 0; pulse < 250; ++pulse) {
        feedback.record_pulse(1000000U + pulse * 100U, 1);
    }

    const auto status = feedback.status_at(1025);
    EXPECT_EQ(status.raw_period_us, 100U);
    EXPECT_EQ(status.filtered_period_us, 100U);
    EXPECT_GE(status.timing_trace_head, 1U);

    Lathe::EncoderTimingWindow sample;
    ASSERT_TRUE(feedback.timing_trace_sample(status.timing_trace_head, sample));
    EXPECT_GT(sample.period_count, 0U);
    EXPECT_EQ(sample.min_period_us, 100U);
    EXPECT_EQ(sample.max_period_us, 100U);
    EXPECT_EQ(sample.period_sum_us, sample.period_count * 100U);
    EXPECT_EQ(sample.end_us - sample.start_us, sample.period_sum_us);
}

TEST(LatheScaffold, QuadratureFeedbackTracksReverseMotionWithoutIndex) {
    Lathe::EncoderSpindleFeedback feedback;
    feedback.configure(100, 250);
    feedback.record_pulse(1000000, 1);
    feedback.record_pulse(1001000, 1);
    feedback.record_pulse(1002000, -1);

    auto status = feedback.status_at(1002);
    EXPECT_TRUE(status.has_angular_position);
    EXPECT_TRUE(status.has_direction);
    EXPECT_FALSE(status.has_index_pulse);
    EXPECT_FALSE(status.fault);
    EXPECT_EQ(status.measured_direction, -1);
    EXPECT_EQ(status.pulse_count, 3u);
    EXPECT_NEAR(status.angular_position_rev, 0.01f, 0.001f);
}

TEST(LatheScaffold, IndexIsObservedButDoesNotGateOrFaultQuadratureFeedback) {
    Lathe::EncoderSpindleFeedback feedback;
    feedback.configure(4, 250);
    for (uint32_t pulse = 0; pulse < 10; ++pulse) {
        feedback.record_pulse(1000000 + pulse * 1000, 1);
    }

    auto withoutIndex = feedback.status_at(1009);
    EXPECT_TRUE(withoutIndex.has_measured_rpm);
    EXPECT_TRUE(withoutIndex.has_angular_position);
    EXPECT_FALSE(withoutIndex.has_index_pulse);
    EXPECT_FALSE(withoutIndex.fault);

    feedback.record_index(1010000);
    auto withIndex = feedback.status_at(1010);
    EXPECT_TRUE(withIndex.has_index_pulse);
    EXPECT_EQ(withIndex.index_count, 1u);
    EXPECT_FALSE(withIndex.fault);
}


TEST(LatheScaffold, ConfiguredEncoderFeedbackFallsBackToNullWhenInactive) {
    EXPECT_FALSE(Lathe::encoder_capture_active());
    auto status = Lathe::configured_spindle_feedback().status();
    EXPECT_FALSE(status.has_measured_rpm);
    EXPECT_FALSE(status.has_index_pulse);
    EXPECT_FALSE(status.has_angular_position);
}

TEST(LatheScaffold, SharedChuckPolicyRejectsSimultaneousSpindleAndCAxis) {
    auto decision = Lathe::evaluate_shared_chuck_transition(
        true, Lathe::SharedChuckMode::Idle, true, SpindleState::Cw);

    EXPECT_EQ(decision.disposition, Lathe::SharedChuckDisposition::Reject);
    EXPECT_EQ(decision.conflict, Lathe::SharedChuckConflict::SimultaneousSpindleAndCAxis);
    EXPECT_EQ(decision.next_mode, Lathe::SharedChuckMode::Idle);
}

TEST(LatheScaffold, SharedChuckPolicyAllowsM5AndCAxisAsOrderedTransition) {
    auto decision = Lathe::evaluate_shared_chuck_transition(
        true, Lathe::SharedChuckMode::Spindle, true, SpindleState::Disable);

    EXPECT_EQ(decision.disposition, Lathe::SharedChuckDisposition::Allow);
    EXPECT_EQ(decision.conflict, Lathe::SharedChuckConflict::None);
    EXPECT_EQ(decision.next_mode, Lathe::SharedChuckMode::CPositioning);
}

TEST(LatheScaffold, SharedChuckPolicyRequiresSynchronizationBeforeSpindleOwnership) {
    auto decision = Lathe::evaluate_shared_chuck_transition(
        true, Lathe::SharedChuckMode::CPositioning, false, SpindleState::Cw);

    EXPECT_EQ(decision.disposition, Lathe::SharedChuckDisposition::AllowAfterSynchronize);
    EXPECT_EQ(decision.next_mode, Lathe::SharedChuckMode::Spindle);
}

TEST(LatheScaffold, SharedChuckPolicyLeavesPendingCPositioningStoppedByM5) {
    auto decision = Lathe::evaluate_shared_chuck_transition(
        true, Lathe::SharedChuckMode::CPositioning, false, SpindleState::Disable);

    EXPECT_EQ(decision.disposition, Lathe::SharedChuckDisposition::Allow);
    EXPECT_EQ(decision.next_mode, Lathe::SharedChuckMode::CPositioning);
}

TEST(LatheScaffold, SharedChuckPolicyIsInertWhenFeatureIsDisabled) {
    auto decision = Lathe::evaluate_shared_chuck_transition(
        false, Lathe::SharedChuckMode::Spindle, true, SpindleState::Cw);

    EXPECT_EQ(decision.disposition, Lathe::SharedChuckDisposition::Allow);
    EXPECT_EQ(decision.next_mode, Lathe::SharedChuckMode::Unavailable);
}

TEST(LatheScaffold, CStepperScaleMatchesEightMicrostepDirectDrive) {
    EXPECT_EQ(Spindles::CStepperLogic::steps_per_revolution(4.444444f), 1600u);
}

TEST(LatheScaffold, CStepperRpmLimitsAreInclusiveAndRejectOutOfRangeCommands) {
    EXPECT_FALSE(Spindles::CStepperLogic::rpm_is_commandable(49.0f, 50.0f, 675.0f));
    EXPECT_TRUE(Spindles::CStepperLogic::rpm_is_commandable(50.0f, 50.0f, 675.0f));
    EXPECT_TRUE(Spindles::CStepperLogic::rpm_is_commandable(675.0f, 50.0f, 675.0f));
    EXPECT_FALSE(Spindles::CStepperLogic::rpm_is_commandable(676.0f, 50.0f, 675.0f));
}

TEST(LatheScaffold, CStepperRpmProducesExpectedPulseRates) {
    constexpr uint32_t stepsPerRev = 1600;
    EXPECT_EQ(Spindles::CStepperLogic::step_rate_millihz(0.5f, stepsPerRev), 13333u);
    EXPECT_EQ(Spindles::CStepperLogic::step_rate_millihz(1.0f, stepsPerRev), 26667u);
    EXPECT_EQ(Spindles::CStepperLogic::step_rate_millihz(5.0f, stepsPerRev), 133333u);
    EXPECT_EQ(Spindles::CStepperLogic::acceleration_millihz_per_sec(100.0f, stepsPerRev), 2666667u);
}

TEST(LatheScaffold, ContinuousStepperRampAdvancesOutsideTheI2sIsr) {
    uint32_t remainder = 0;
    const uint32_t rate = Machine::ContinuousStepperLogic::ramp_rate(
        0, 1333333, 2666667, 500, remainder);

    EXPECT_EQ(rate, 1333333u);
    EXPECT_EQ(remainder, 500u);
}

TEST(LatheScaffold, ContinuousStepperRampPreservesFractionalProgress) {
    uint32_t remainder = 0;
    uint32_t rate      = 0;
    for (int i = 0; i < 4; ++i) {
        rate = Machine::ContinuousStepperLogic::ramp_rate(rate, 10, 333, 1, remainder);
    }

    EXPECT_EQ(rate, 1u);
    EXPECT_EQ(remainder, 332u);
    EXPECT_EQ(Machine::ContinuousStepperLogic::ramp_rate(rate, 0, 333, 4, remainder), 0u);
}

TEST(LatheScaffold, CStepperGracefulStopDeadlineTracksLiveRate) {
    constexpr uint32_t stepsPerRev = 1600;
    const uint32_t deceleration =
        Spindles::CStepperLogic::acceleration_millihz_per_sec(100.0f, stepsPerRev);

    EXPECT_EQ(Machine::ContinuousStepperLogic::stop_timeout_ms(
                  Spindles::CStepperLogic::step_rate_millihz(50.0f, stepsPerRev), deceleration),
              1000u);
    EXPECT_EQ(Machine::ContinuousStepperLogic::stop_timeout_ms(
                  Spindles::CStepperLogic::step_rate_millihz(100.0f, stepsPerRev), deceleration),
              1500u);
    EXPECT_EQ(Machine::ContinuousStepperLogic::stop_timeout_ms(
                  Spindles::CStepperLogic::step_rate_millihz(250.0f, stepsPerRev), deceleration),
              3000u);
    EXPECT_EQ(Machine::ContinuousStepperLogic::stop_timeout_ms(
                  Spindles::CStepperLogic::step_rate_millihz(500.0f, stepsPerRev), deceleration),
              5500u);
    EXPECT_EQ(Machine::ContinuousStepperLogic::stop_timeout_ms(
                  Spindles::CStepperLogic::step_rate_millihz(675.0f, stepsPerRev), deceleration),
              7250u);
}

TEST(LatheScaffold, CStepperGracefulStopRampsThroughMinimumToZero) {
    constexpr uint32_t stepsPerRev = 1600;
    const uint32_t deceleration =
        Spindles::CStepperLogic::acceleration_millihz_per_sec(100.0f, stepsPerRev);
    uint32_t remainder = 0;
    uint32_t rate = Spindles::CStepperLogic::step_rate_millihz(50.0f, stepsPerRev);

    rate = Machine::ContinuousStepperLogic::ramp_rate(rate, 0, deceleration, 250, remainder);
    EXPECT_NEAR(rate, Spindles::CStepperLogic::step_rate_millihz(25.0f, stepsPerRev), 1u);
    rate = Machine::ContinuousStepperLogic::ramp_rate(rate, 0, deceleration, 250, remainder);
    EXPECT_EQ(rate, 0u);
}

TEST(LatheScaffold, ContinuousSchedulerPreservesExactAverageAtCommissioningRates) {
    constexpr uint32_t timerHz = 20000000;
    constexpr uint32_t stepsPerRev = 1600;
    const float rpms[] = { 50.0f, 200.0f, 675.0f };

    for (const float rpm : rpms) {
        const uint32_t rate = Spindles::CStepperLogic::step_rate_millihz(rpm, stepsPerRev);
        const auto command = Machine::ContinuousEventScheduler::make_rate_command(timerHz, rate);
        ASSERT_TRUE(command.valid);

        Machine::ContinuousEventScheduler::IntervalState state;
        ASSERT_TRUE(Machine::ContinuousEventScheduler::apply_rate(state, command));
        uint64_t scheduledTicks = 0;
        for (uint32_t pulse = 0; pulse < stepsPerRev; ++pulse) {
            scheduledTicks += state.ticks_until_step;
            Machine::ContinuousEventScheduler::retire_step(state);
        }
        const uint64_t expectedTicks = (static_cast<uint64_t>(timerHz) * 1000ULL * stepsPerRev) / rate;
        EXPECT_EQ(scheduledTicks, expectedTicks) << "RPM " << rpm;
    }
}

TEST(LatheScaffold, ContinuousSchedulerMergesOnlyTheDueCAxisBit) {
    EXPECT_EQ(Machine::ContinuousEventScheduler::merged_step_mask(0x03u, 0x20u, false), 0x03u);
    EXPECT_EQ(Machine::ContinuousEventScheduler::merged_step_mask(0x03u, 0x20u, true), 0x23u);
}

TEST(LatheScaffold, ContinuousSchedulerRetiresOnlyOneActuallyEmittedPulse) {
    EXPECT_FALSE(Machine::ContinuousEventScheduler::pulse_was_emitted(41u, 41u));
    EXPECT_TRUE(Machine::ContinuousEventScheduler::pulse_was_emitted(41u, 42u));
    EXPECT_FALSE(Machine::ContinuousEventScheduler::pulse_was_emitted(41u, 43u));
    EXPECT_TRUE(Machine::ContinuousEventScheduler::pulse_was_emitted(UINT32_MAX, 0u));
}

TEST(LatheScaffold, ContinuousSchedulerFallsBackOnlyAtNormalPlannerCompletion) {
    using Action = Machine::ContinuousEventScheduler::MissingDuePulseAction;

    EXPECT_EQ(Machine::ContinuousEventScheduler::missing_due_pulse_action(false),
              Action::EmitContinuousOnly);
    EXPECT_EQ(Machine::ContinuousEventScheduler::missing_due_pulse_action(true),
              Action::Fault);
}

TEST(LatheScaffold, ContinuousSchedulerMovesOnlyPlannerEventsAtPulseWidthCoincidences) {
    using Action = Machine::ContinuousEventScheduler::CoincidenceAction;

    EXPECT_EQ(Machine::ContinuousEventScheduler::choose_coincidence(true, 0, true, 40, 80),
              Action::DelayPlannerToContinuous);
    EXPECT_EQ(Machine::ContinuousEventScheduler::choose_coincidence(true, 40, true, 0, 80),
              Action::AdvancePlannerToContinuous);
    EXPECT_EQ(Machine::ContinuousEventScheduler::choose_coincidence(true, 0, true, 0, 80), Action::None);
    EXPECT_EQ(Machine::ContinuousEventScheduler::choose_coincidence(true, 0, true, 80, 80), Action::None);
    EXPECT_EQ(Machine::ContinuousEventScheduler::choose_coincidence(true, 40, false, 0, 80), Action::None);
}

TEST(LatheScaffold, ContinuousSchedulerKeepsCExactAndRepaysPlannerMergeTiming) {
    constexpr uint32_t timerHz = 20000000;
    constexpr uint32_t stepsPerRev = 1600;
    const uint32_t rate = Spindles::CStepperLogic::step_rate_millihz(420.0f, stepsPerRev);
    const auto command = Machine::ContinuousEventScheduler::make_rate_command(timerHz, rate);
    Machine::ContinuousEventScheduler::IntervalState continuous;
    ASSERT_TRUE(Machine::ContinuousEventScheduler::apply_rate(continuous, command));

    uint64_t continuousTicks = 0;
    for (uint32_t pulse = 0; pulse < rate / 1000U; ++pulse) {
        continuousTicks += continuous.ticks_until_step;
        const int32_t plannerAdjustment = (pulse & 1U) ? -40 : 40;
        (void)Machine::ContinuousEventScheduler::adjusted_planner_period(1000, plannerAdjustment);
        Machine::ContinuousEventScheduler::retire_step(continuous);
    }

    // One second at 420 RPM is exactly 22,400 C pulses. Planner merge
    // compensation must never enter or perturb the continuous-C interval.
    EXPECT_EQ(continuousTicks, static_cast<uint64_t>(timerHz));
    EXPECT_EQ(Machine::ContinuousEventScheduler::adjusted_planner_period(1000, 40), 1040u);
    EXPECT_EQ(Machine::ContinuousEventScheduler::adjusted_planner_period(1000, -40), 960u);
    EXPECT_EQ(Machine::ContinuousEventScheduler::adjusted_planner_period(20, -40), 1u);
}

TEST(LatheScaffold, ContinuousSchedulerRateUpdatesNeverMoveAnAlreadyDuePulse) {
    Machine::ContinuousEventScheduler::IntervalState state;
    ASSERT_TRUE(Machine::ContinuousEventScheduler::apply_rate(
        state, Machine::ContinuousEventScheduler::make_rate_command(20000000, 1000000)));
    state.ticks_until_step = 0;

    ASSERT_TRUE(Machine::ContinuousEventScheduler::apply_rate(
        state, Machine::ContinuousEventScheduler::make_rate_command(20000000, 2000000)));
    EXPECT_EQ(state.ticks_until_step, 0u);
}

TEST(LatheScaffold, ContinuousSchedulerCombinedRateAdmissionFailsClosed) {
    EXPECT_TRUE(Machine::ContinuousEventScheduler::combined_rate_admissible(36000000, 6400, 125000));
    EXPECT_FALSE(Machine::ContinuousEventScheduler::combined_rate_admissible(120000000, 6400, 125000));
}

TEST(LatheScaffold, ContinuousSchedulerZeroRateDisarmsWithoutLosingPulseDiagnostics) {
    Machine::ContinuousEventScheduler::IntervalState state;
    ASSERT_TRUE(Machine::ContinuousEventScheduler::apply_rate(
        state, Machine::ContinuousEventScheduler::make_rate_command(20000000, 36000000)));
    Machine::ContinuousEventScheduler::retire_step(state);
    ASSERT_TRUE(Machine::ContinuousEventScheduler::apply_rate(
        state, Machine::ContinuousEventScheduler::make_rate_command(20000000, 0)));

    EXPECT_FALSE(state.active);
    EXPECT_EQ(state.ticks_until_step, 0u);
    EXPECT_EQ(state.emitted_pulses, 1u);
}

TEST(LatheScaffold, ProgramNameIsBoundedAndStripsControlCharacters) {
    std::string unsafe(150, 'A');
    unsafe[4] = '\n';
    Lathe::record_program_name(unsafe);

    EXPECT_EQ(Lathe::program_name().size(), 127u);
    EXPECT_EQ(Lathe::program_name()[4], '?');
}

TEST(LatheScaffold, BoundedProbeRequestAcceptsExactXAndZContracts) {
    auto x = Lathe::parse_bounded_probe_request("PROBE,AXIS=X,DISTANCE=-12.5,FEED=75");
    EXPECT_EQ(x.error, Lathe::BoundedProbeRequestError::None);
    EXPECT_EQ(x.axis, X_AXIS);
    EXPECT_FLOAT_EQ(x.distance_mm, -12.5f);
    EXPECT_FLOAT_EQ(x.feed_mm_min, 75.0f);

    auto z = Lathe::parse_bounded_probe_request("PROBE,AXIS=Z,DISTANCE=100,FEED=1000");
    EXPECT_EQ(z.error, Lathe::BoundedProbeRequestError::None);
    EXPECT_EQ(z.axis, Z_AXIS);
}

TEST(LatheScaffold, BoundedProbeRequestRejectsUnsupportedAxesAndExtraFields) {
    EXPECT_EQ(
        Lathe::parse_bounded_probe_request("PROBE,AXIS=C,DISTANCE=1,FEED=10").error,
        Lathe::BoundedProbeRequestError::InvalidAxis);
    EXPECT_EQ(
        Lathe::parse_bounded_probe_request("PROBE,AXIS=X,DISTANCE=1,FEED=10,EXTRA=1").error,
        Lathe::BoundedProbeRequestError::Malformed);
}

TEST(LatheScaffold, BoundedProbeRequestRejectsNonFiniteOrUnsafeMotion) {
    EXPECT_EQ(
        Lathe::parse_bounded_probe_request("PROBE,AXIS=X,DISTANCE=nan,FEED=10").error,
        Lathe::BoundedProbeRequestError::InvalidDistance);
    EXPECT_EQ(
        Lathe::parse_bounded_probe_request("PROBE,AXIS=X,DISTANCE=0,FEED=10").error,
        Lathe::BoundedProbeRequestError::InvalidDistance);
    EXPECT_EQ(
        Lathe::parse_bounded_probe_request("PROBE,AXIS=X,DISTANCE=100.1,FEED=10").error,
        Lathe::BoundedProbeRequestError::InvalidDistance);
    EXPECT_EQ(
        Lathe::parse_bounded_probe_request("PROBE,AXIS=X,DISTANCE=1,FEED=0").error,
        Lathe::BoundedProbeRequestError::InvalidFeed);
    EXPECT_EQ(
        Lathe::parse_bounded_probe_request("PROBE,AXIS=X,DISTANCE=1,FEED=1000.1").error,
        Lathe::BoundedProbeRequestError::InvalidFeed);
}

TEST(LatheScaffold, XOffsetConvertsDiameterModeToMachineRadiusOffset) {
    EXPECT_FLOAT_EQ(Lathe::x_offset_to_machine_mm(2.0f, Lathe::DiameterMode::Radius), 2.0f);
    EXPECT_FLOAT_EQ(Lathe::x_offset_to_machine_mm(2.0f, Lathe::DiameterMode::Diameter), 1.0f);
}


TEST(LatheScaffold, DiameterModeConversionPolicyCoversCoordinateAndCycleEntry) {
    const float diameter_x = 24.0f;
    const float internal_radius_x = Lathe::x_program_to_machine_mm(diameter_x, Lathe::DiameterMode::Diameter);

    EXPECT_FLOAT_EQ(internal_radius_x, 12.0f);
    EXPECT_FLOAT_EQ(Lathe::x_program_to_machine_mm(internal_radius_x, Lathe::DiameterMode::Radius), 12.0f);
    EXPECT_FLOAT_EQ(Lathe::x_machine_to_diameter_mm(internal_radius_x), 24.0f);

    Lathe::RoughTurningCycleSpec roughing;
    roughing.start_x_mm = internal_radius_x;
    roughing.final_x_mm = Lathe::x_program_to_machine_mm(20.0f, Lathe::DiameterMode::Diameter);
    roughing.start_z_mm = 0.0f;
    roughing.end_z_mm = -5.0f;
    roughing.depth_step_mm = 1.0f;
    roughing.rough_feed_mm_min = 100.0f;

    auto plan = Lathe::build_rough_turning_cycle(roughing);
    ASSERT_TRUE(plan.valid);
    EXPECT_FLOAT_EQ(plan.moves[plan.count - 1].x_mm, 10.0f);
}

TEST(LatheScaffold, XDiameterProgrammingConvertsToInternalRadiusCoordinates) {
    EXPECT_FLOAT_EQ(Lathe::x_program_to_machine_mm(24.0f, Lathe::DiameterMode::Diameter), 12.0f);
    EXPECT_FLOAT_EQ(Lathe::x_program_to_machine_mm(12.0f, Lathe::DiameterMode::Radius), 12.0f);
    EXPECT_FLOAT_EQ(Lathe::x_machine_to_diameter_mm(12.0f), 24.0f);
    EXPECT_FLOAT_EQ(Lathe::x_machine_to_diameter_mm(-12.0f), 24.0f);
}

TEST(LatheScaffold, LatheToolDataStoresGeometryWearNoseAndOrientation) {
    Lathe::clear_tool_table(false);

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
    EXPECT_FLOAT_EQ(stored->geometry_z_mm, 2.0f);
    EXPECT_FLOAT_EQ(stored->wear_x_mm, 0.1f);
    EXPECT_FLOAT_EQ(stored->wear_z_mm, -0.2f);
    EXPECT_FLOAT_EQ(stored->nose_radius_mm, 0.4f);
    EXPECT_EQ(stored->orientation, Lathe::InsertOrientation::FrontTurning);

    auto active = Lathe::select_tool(7);
    EXPECT_TRUE(active.valid);
    EXPECT_EQ(active.tool_number, 7u);
    EXPECT_FLOAT_EQ(active.x_mm, 1.1f);
    EXPECT_FLOAT_EQ(active.z_mm, 1.8f);
    EXPECT_FLOAT_EQ(active.nose_radius_mm, 0.4f);

    auto telemetry_source = Lathe::get_tool_data(active.tool_number);
    ASSERT_TRUE(telemetry_source.has_value());
    EXPECT_FLOAT_EQ(telemetry_source->geometry_x_mm, 1.0f);
    EXPECT_FLOAT_EQ(telemetry_source->geometry_z_mm, 2.0f);
    EXPECT_FLOAT_EQ(telemetry_source->wear_x_mm, 0.1f);
    EXPECT_FLOAT_EQ(telemetry_source->wear_z_mm, -0.2f);
}

TEST(LatheScaffold, LatheToolTableCanBeClearedWithoutPersisting) {
    Lathe::ToolData tool;
    tool.geometry_x_mm = 3.0f;

    Lathe::set_tool_data(9, tool);
    ASSERT_TRUE(Lathe::get_tool_data(9).has_value());

    Lathe::clear_tool_table(false);
    EXPECT_FALSE(Lathe::get_tool_data(9).has_value());
}

TEST(LatheScaffold, TouchOffCalculatesGeometryOffsetsAndPreservesWear) {
    Lathe::clear_tool_table(false);

    Lathe::ToolData tool;
    tool.wear_x_mm = 0.2f;
    tool.wear_z_mm = -0.1f;
    Lathe::set_tool_data(11, tool);

    Lathe::TouchOffSpec spec;
    spec.tool_number    = 11;
    spec.machine_x_mm   = 10.0f;
    spec.machine_z_mm   = -4.0f;
    spec.reference_x_mm = 24.0f;
    spec.reference_z_mm = 0.0f;
    spec.x_mode         = Lathe::DiameterMode::Diameter;

    EXPECT_EQ(Lathe::touch_off_tool(spec), Error::Ok);

    auto stored = Lathe::get_tool_data(11);
    ASSERT_TRUE(stored.has_value());
    EXPECT_FLOAT_EQ(stored->geometry_x_mm, 1.8f);
    EXPECT_FLOAT_EQ(stored->geometry_z_mm, 4.1f);
    EXPECT_FLOAT_EQ(stored->wear_x_mm, 0.2f);
    EXPECT_FLOAT_EQ(stored->wear_z_mm, -0.1f);
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

TEST(LatheScaffold, FinishingCycleBuildsSingleFinishMove) {
    Lathe::FinishingCycleSpec spec;
    spec.start_x_mm  = 26.0f;
    spec.end_x_mm    = 25.8f;
    spec.start_z_mm  = 0.0f;
    spec.end_z_mm    = -20.0f;
    spec.feed_mm_min = 80.0f;

    auto plan = Lathe::build_finishing_cycle(spec);
    ASSERT_TRUE(plan.valid);
    EXPECT_EQ(plan.count, 1u);
    EXPECT_EQ(plan.moves[0].kind, Lathe::CycleMoveKind::Linear);
    EXPECT_FLOAT_EQ(plan.moves[0].x_mm, 25.8f);
    EXPECT_FLOAT_EQ(plan.moves[0].z_mm, -20.0f);
}

TEST(LatheScaffold, GroovingCyclePecksAndRetracts) {
    Lathe::GroovingCycleSpec spec;
    spec.start_x_mm    = 20.0f;
    spec.final_x_mm    = 16.0f;
    spec.z_mm          = -5.0f;
    spec.peck_depth_mm = 2.0f;
    spec.feed_mm_min   = 60.0f;

    auto plan = Lathe::build_grooving_cycle(spec);
    ASSERT_TRUE(plan.valid);
    EXPECT_EQ(plan.count, 3u);
    EXPECT_FLOAT_EQ(plan.moves[0].x_mm, 18.0f);
    EXPECT_EQ(plan.moves[1].kind, Lathe::CycleMoveKind::Rapid);
    EXPECT_FLOAT_EQ(plan.moves[2].x_mm, 16.0f);
}

TEST(LatheScaffold, PeckDrillingCyclePecksAlongZAndRetracts) {
    Lathe::PeckDrillingCycleSpec spec;
    spec.x_mm          = 0.0f;
    spec.start_z_mm    = 1.0f;
    spec.final_z_mm    = -5.0f;
    spec.peck_depth_mm = 3.0f;
    spec.feed_mm_min   = 50.0f;

    auto plan = Lathe::build_peck_drilling_cycle(spec);
    ASSERT_TRUE(plan.valid);
    EXPECT_EQ(plan.count, 3u);
    EXPECT_FLOAT_EQ(plan.moves[0].z_mm, -2.0f);
    EXPECT_EQ(plan.moves[1].kind, Lathe::CycleMoveKind::Rapid);
    EXPECT_FLOAT_EQ(plan.moves[2].z_mm, -5.0f);
}


TEST(LatheScaffold, SynchronizedThreadingProgressUsesStartRevolutionOffset) {
    Lathe::ThreadingSyncState state;
    state.start_z_mm = 0.0f;
    state.end_z_mm   = -10.0f;
    state.pitch_mm   = 2.0f;
    state.start_spindle_revolutions = 4.0f;

    EXPECT_FLOAT_EQ(Lathe::synchronized_thread_z(state, 4.0f), 0.0f);
    EXPECT_FLOAT_EQ(Lathe::synchronized_thread_z(state, 6.0f), -4.0f);
    EXPECT_NEAR(Lathe::synchronized_thread_progress(state, 6.0f), 0.4f, 0.001f);
}

TEST(LatheScaffold, SynchronizedThreadingTrajectoryFollowsSpindleRevolutions) {
    Lathe::ThreadingSyncState state;
    state.start_z_mm = 0.0f;
    state.end_z_mm   = -10.0f;
    state.pitch_mm   = 1.5f;

    EXPECT_FLOAT_EQ(Lathe::synchronized_thread_z(state, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(Lathe::synchronized_thread_z(state, 2.0f), -3.0f);
    EXPECT_FLOAT_EQ(Lathe::synchronized_thread_z(state, 100.0f), -10.0f);
}
