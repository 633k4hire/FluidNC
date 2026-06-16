// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include "Config.h"
#include "Error.h"
#include "SpindleDatatypes.h"

#include <array>
#include <cstdint>
#include <optional>

namespace Lathe {
    enum class SpindleSpeedMode : uint8_t {
        FixedRPM = 0,
        ConstantSurfaceSpeed,
    };

    enum class FeedMode : uint8_t {
        UnitsPerMinute = 0,
        InverseTime,
        UnitsPerRevolution,
    };

    enum class DiameterMode : uint8_t {
        Radius = 0,
        Diameter,
    };

    enum class Feature : uint8_t {
        ConstantSurfaceSpeed,
        FeedPerRevolution,
        Threading,
    };

    enum class InsertOrientation : uint8_t {
        Unknown = 0,
        FrontTurning = 1,
        BackTurning = 2,
        FrontFacing = 3,
        BackFacing = 4,
    };

    struct ToolData {
        float             geometry_x_mm = 0.0f;
        float             geometry_z_mm = 0.0f;
        float             wear_x_mm     = 0.0f;
        float             wear_z_mm     = 0.0f;
        float             nose_radius_mm = 0.0f;
        InsertOrientation orientation    = InsertOrientation::Unknown;
        bool              valid          = false;
    };

    struct ActiveToolOffset {
        uint32_t tool_number = 0;
        float    x_mm        = 0.0f;
        float    z_mm        = 0.0f;
        float    nose_radius_mm = 0.0f;
        InsertOrientation orientation = InsertOrientation::Unknown;
        bool     valid       = false;
    };

    enum class CycleMoveKind : uint8_t {
        Linear,
        Threading,
        Rapid,
    };

    struct CycleMove {
        CycleMoveKind kind = CycleMoveKind::Linear;
        float         x_mm = 0.0f;
        float         z_mm = 0.0f;
        float         feed = 0.0f;
    };

    constexpr size_t MaxCycleMoves = 24;

    struct CyclePlan {
        std::array<CycleMove, MaxCycleMoves> moves {};
        size_t count = 0;
        bool   valid = false;
        Error  error = Error::Ok;
    };

    struct ThreadingCycleSpec {
        float    start_x_mm = 0.0f;
        float    end_x_mm   = 0.0f;
        float    start_z_mm = 0.0f;
        float    end_z_mm   = 0.0f;
        float    pitch_mm   = 0.0f;
        uint8_t  passes     = 0;
    };

    struct RoughTurningCycleSpec {
        float   start_x_mm      = 0.0f;
        float   final_x_mm      = 0.0f;
        float   start_z_mm      = 0.0f;
        float   end_z_mm        = 0.0f;
        float   depth_step_mm   = 0.0f;
        float   rough_feed_mm_min = 0.0f;
        bool    include_finish_pass = false;
    };

    struct FeedbackStatus {
        SpindleSpeed commanded_rpm = 0;
        SpindleSpeed measured_rpm  = 0;
        uint32_t     timestamp_ms  = 0;
        bool         has_measured_rpm : 1;
        bool         has_index_pulse : 1;
        bool         has_angular_position : 1;
        bool         stale : 1;
        bool         fault : 1;

        FeedbackStatus() : has_measured_rpm(false), has_index_pulse(false), has_angular_position(false), stale(false), fault(false) {}
    };

    class SpindleFeedback {
    public:
        virtual FeedbackStatus status() const { return FeedbackStatus {}; }
        virtual bool synchronize_for_threading_start() const { return false; }
        virtual ~SpindleFeedback() = default;
    };

    class NullSpindleFeedback : public SpindleFeedback {};

    bool enabled();
    bool feature_enabled(Feature feature);
    Error validate_feature(Feature feature);
    const char* feature_name(Feature feature);
    const char* unsupported_message(Feature feature);

    constexpr float Pi = 3.14159265358979323846f;

    float max_css_rpm();
    float min_css_diameter_mm();
    axis_t x_axis();
    axis_t z_axis();
    float css_rpm_from_diameter_mm(float surface_speed, float diameter_mm, bool surface_speed_is_inches_per_minute);
    float css_rpm_from_diameter_mm(float surface_speed, float diameter_mm, float minimum_diameter_mm, bool surface_speed_is_inches_per_minute);
    float clamp_css_rpm(float rpm);
    float clamp_css_rpm(float rpm, float max_rpm);
    float feed_per_rev_to_mm_per_min(float feed_per_rev, float rpm, bool feed_is_inches);
    bool feedback_supports_threading(const FeedbackStatus& status);

    float x_offset_to_machine_mm(float x_offset, DiameterMode mode);
    void set_tool_data(uint32_t tool_number, const ToolData& data);
    std::optional<ToolData> get_tool_data(uint32_t tool_number);
    ActiveToolOffset select_tool(uint32_t tool_number);
    ActiveToolOffset active_tool_offset();

    CyclePlan build_threading_cycle(const ThreadingCycleSpec& spec);
    CyclePlan build_rough_turning_cycle(const RoughTurningCycleSpec& spec);
}
