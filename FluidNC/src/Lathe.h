// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include "Config.h"
#include "Error.h"
#include "SpindleDatatypes.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
#    include <esp_attr.h>
#    define LATHE_IRAM_ATTR IRAM_ATTR
#else
#    define LATHE_IRAM_ATTR
#endif

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

    // The Maijker lathe has one physical chuck driven in two mutually-exclusive
    // ways: as the continuously rotating spindle and as the indexed C axis.
    // These values describe that physical ownership, not a second motor.
    enum class SharedChuckMode : uint8_t {
        Unavailable = 0,
        Idle,
        CPositioning,
        Spindle,
    };

    enum class SharedChuckDisposition : uint8_t {
        Allow = 0,
        AllowAfterSynchronize,
        Reject,
    };

    enum class SharedChuckConflict : uint8_t {
        None = 0,
        SimultaneousSpindleAndCAxis,
    };

    struct SharedChuckDecision {
        SharedChuckDisposition disposition = SharedChuckDisposition::Allow;
        SharedChuckMode        next_mode   = SharedChuckMode::Unavailable;
        SharedChuckConflict    conflict    = SharedChuckConflict::None;
    };

    enum class BoundedProbeRequestError : uint8_t {
        None = 0,
        Malformed,
        InvalidAxis,
        InvalidDistance,
        InvalidFeed,
    };

    struct BoundedProbeRequest {
        axis_t                   axis        = X_AXIS;
        float                    distance_mm = 0.0f;
        float                    feed_mm_min = 0.0f;
        BoundedProbeRequestError error       = BoundedProbeRequestError::Malformed;
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

    struct TouchOffSpec {
        uint32_t     tool_number     = 0;
        float        machine_x_mm    = 0.0f;
        float        machine_z_mm    = 0.0f;
        float        reference_x_mm  = 0.0f;
        float        reference_z_mm  = 0.0f;
        DiameterMode x_mode          = DiameterMode::Radius;
        bool         set_x           = true;
        bool         set_z           = true;
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

    struct FinishingCycleSpec {
        float start_x_mm = 0.0f;
        float end_x_mm   = 0.0f;
        float start_z_mm = 0.0f;
        float end_z_mm   = 0.0f;
        float feed_mm_min = 0.0f;
    };

    struct GroovingCycleSpec {
        float start_x_mm    = 0.0f;
        float final_x_mm    = 0.0f;
        float z_mm          = 0.0f;
        float peck_depth_mm = 0.0f;
        float feed_mm_min   = 0.0f;
    };

    struct PeckDrillingCycleSpec {
        float x_mm          = 0.0f;
        float start_z_mm    = 0.0f;
        float final_z_mm    = 0.0f;
        float peck_depth_mm = 0.0f;
        float feed_mm_min   = 0.0f;
    };

    struct ThreadingSyncState {
        float start_z_mm = 0.0f;
        float end_z_mm   = 0.0f;
        float pitch_mm   = 0.0f;
        float start_spindle_revolutions = 0.0f;
    };

    struct FeedbackStatus {
        SpindleSpeed commanded_rpm = 0;
        float        measured_rpm  = 0.0f;
        uint32_t     timestamp_ms  = 0;
        float        angular_position_rev = 0.0f;
        uint32_t     pulse_count = 0;
        uint32_t     index_count = 0;
        uint32_t     revolution_count = 0;
        uint32_t     last_index_pulses = 0;
        uint32_t     last_pulse_age_ms = 0;
        int8_t       measured_direction = 0;
        bool         has_measured_rpm : 1;
        bool         has_index_pulse : 1;
        bool         has_angular_position : 1;
        bool         has_direction : 1;
        bool         stale : 1;
        bool         fault : 1;

        FeedbackStatus() : has_measured_rpm(false), has_index_pulse(false), has_angular_position(false), has_direction(false), stale(false), fault(false) {}
    };

    class SpindleFeedback {
    public:
        virtual FeedbackStatus status() const { return FeedbackStatus {}; }
        virtual bool synchronize_for_threading_start() const { return false; }
        virtual ~SpindleFeedback() = default;
    };

    class NullSpindleFeedback : public SpindleFeedback {};

    class EncoderSpindleFeedback : public SpindleFeedback {
    public:
        void configure(uint32_t pulses_per_revolution, uint32_t stale_timeout_ms);
        void set_commanded_rpm(SpindleSpeed rpm);
        void LATHE_IRAM_ATTR record_pulse(uint32_t timestamp_us, int8_t direction);
        void LATHE_IRAM_ATTR record_index(uint32_t timestamp_us);
        FeedbackStatus status() const override;
        FeedbackStatus status_at(uint32_t now_ms) const;
        bool synchronize_for_threading_start() const override;
        uint32_t pulses_per_revolution() const { return _pulses_per_revolution.load(std::memory_order_relaxed); }

    private:
        std::atomic<uint32_t> _snapshot_generation { 0 };
        std::atomic<uint32_t> _pulses_per_revolution { 1 };
        std::atomic<uint32_t> _stale_timeout_ms { 250 };
        std::atomic<uint32_t> _last_pulse_us { 0 };
        std::atomic<uint32_t> _filtered_period_us { 0 };
        std::atomic<uint32_t> _pulse_count { 0 };
        std::atomic<uint32_t> _index_pulse_count { 0 };
        std::atomic<uint32_t> _last_index_pulse_count { 0 };
        std::atomic<uint32_t> _last_index_pulses { 0 };
        std::atomic<int32_t>  _signed_position { 0 };
        std::atomic<int8_t>   _measured_direction { 0 };
        std::atomic<SpindleSpeed> _commanded_rpm { 0 };
    };

    bool enabled();
    bool feature_enabled(Feature feature);
    Error validate_feature(Feature feature);
    const char* feature_name(Feature feature);
    const char* unsupported_message(Feature feature);

    constexpr float Pi = 3.14159265358979323846f;

    float max_css_rpm();
    float min_css_diameter_mm();
    bool encoder_enabled();
    uint32_t encoder_pulses_per_revolution();
    axis_t x_axis();
    axis_t z_axis();
    bool shared_chuck_enabled();
    axis_t c_axis();
    SharedChuckDecision evaluate_shared_chuck_transition(
        bool enabled, SharedChuckMode current_mode, bool c_axis_motion, SpindleState requested_spindle);
    SharedChuckMode shared_chuck_mode();
    const char* shared_chuck_mode_name(SharedChuckMode mode);
    const char* shared_chuck_conflict_message(SharedChuckConflict conflict);
    BoundedProbeRequest parse_bounded_probe_request(const std::string& request);
    const char* bounded_probe_request_error_message(BoundedProbeRequestError error);
    void reset_shared_chuck_state();
    bool select_shared_chuck_mode(SharedChuckMode mode);
    void note_shared_chuck_c_motion();
    void note_shared_chuck_cycle_complete();
    void note_shared_chuck_spindle_state(SpindleState state);
    void record_program_name(const std::string& name);
    const std::string& program_name();
    float css_rpm_from_diameter_mm(float surface_speed, float diameter_mm, bool surface_speed_is_inches_per_minute);
    float css_rpm_from_diameter_mm(float surface_speed, float diameter_mm, float minimum_diameter_mm, bool surface_speed_is_inches_per_minute);
    float clamp_css_rpm(float rpm);
    float clamp_css_rpm(float rpm, float max_rpm);
    float feed_per_rev_to_mm_per_min(float feed_per_rev, float rpm, bool feed_is_inches);
    bool feedback_supports_threading(const FeedbackStatus& status);

    float x_offset_to_machine_mm(float x_offset, DiameterMode mode);
    float x_program_to_machine_mm(float x_programmed, DiameterMode mode);
    float x_machine_to_diameter_mm(float x_machine);
    bool load_tool_table();
    bool save_tool_table();
    void clear_tool_table(bool persist);
    void set_tool_data(uint32_t tool_number, const ToolData& data);
    std::optional<ToolData> get_tool_data(uint32_t tool_number);
    ActiveToolOffset select_tool(uint32_t tool_number);
    ActiveToolOffset active_tool_offset();
    Error touch_off_tool(const TouchOffSpec& spec);

    CyclePlan build_threading_cycle(const ThreadingCycleSpec& spec);
    CyclePlan build_rough_turning_cycle(const RoughTurningCycleSpec& spec);
    CyclePlan build_finishing_cycle(const FinishingCycleSpec& spec);
    CyclePlan build_grooving_cycle(const GroovingCycleSpec& spec);
    CyclePlan build_peck_drilling_cycle(const PeckDrillingCycleSpec& spec);
    float spindle_revolutions(const FeedbackStatus& status);
    float synchronized_thread_z(const ThreadingSyncState& state, float spindle_revolutions);
    float synchronized_thread_progress(const ThreadingSyncState& state, float spindle_revolutions);
}
