// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "Lathe.h"

#include "Machine/MachineConfig.h"
#include "Logging.h"
#include "Protocol.h"
#include "Settings.h"

#include <algorithm>
#include <cmath>
#include <array>

namespace Lathe {
    namespace {
        struct ToolSlot {
            uint32_t tool_number = 0;
            ToolData data;
        };

        constexpr size_t MaxLatheTools = 32;
        constexpr const char* ToolTableNvsKey = "LatheTools";
        std::array<ToolSlot, MaxLatheTools> tool_table;
        ActiveToolOffset active_offset;
        bool tool_table_loaded = false;

        void reset_tool_table() {
            tool_table = {};
            active_offset = {};
        }

        void ensure_tool_table_loaded() {
            if (!tool_table_loaded) {
                load_tool_table();
            }
        }
    }

    bool enabled() {
        return config != nullptr && config->_lathe != nullptr && config->_lathe->_enable;
    }

    bool feature_enabled(Feature feature) {
        if (!enabled()) {
            return false;
        }

        switch (feature) {
            case Feature::ConstantSurfaceSpeed:
                return config->_lathe->_enableCSS;
            case Feature::FeedPerRevolution:
                return config->_lathe->_enableFeedPerRev;
            case Feature::Threading:
                return config->_lathe->_enableThreading;
        }
        return false;
    }

    const char* feature_name(Feature feature) {
        switch (feature) {
            case Feature::ConstantSurfaceSpeed:
                return "constant surface speed";
            case Feature::FeedPerRevolution:
                return "feed per revolution";
            case Feature::Threading:
                return "threading";
        }
        return "lathe feature";
    }

    const char* unsupported_message(Feature feature) {
        switch (feature) {
            case Feature::ConstantSurfaceSpeed:
                return "Lathe constant surface speed (G96/G97) is scaffolded but not implemented";
            case Feature::FeedPerRevolution:
                return "Lathe feed per revolution (G95) is scaffolded but not implemented";
            case Feature::Threading:
                return "Lathe threading is scaffolded but not implemented";
        }
        return "Lathe feature is scaffolded but not implemented";
    }

    float max_css_rpm() {
        return enabled() ? config->_lathe->_maxCssRpm : 0.0f;
    }

    float min_css_diameter_mm() {
        return enabled() ? config->_lathe->_minCssDiameter : 1.0f;
    }

    axis_t x_axis() {
        return enabled() ? static_cast<axis_t>(config->_lathe->_xAxis) : X_AXIS;
    }

    axis_t z_axis() {
        return enabled() ? static_cast<axis_t>(config->_lathe->_zAxis) : Z_AXIS;
    }

    float css_rpm_from_diameter_mm(float surface_speed, float diameter_mm, bool surface_speed_is_inches_per_minute) {
        return css_rpm_from_diameter_mm(surface_speed, diameter_mm, min_css_diameter_mm(), surface_speed_is_inches_per_minute);
    }

    float css_rpm_from_diameter_mm(float surface_speed, float diameter_mm, float minimum_diameter_mm, bool surface_speed_is_inches_per_minute) {
        if (surface_speed <= 0.0f) {
            return 0.0f;
        }
        const float safe_diameter_mm = std::max(std::fabs(diameter_mm), minimum_diameter_mm);
        const float surface_mm_per_min = surface_speed_is_inches_per_minute ? surface_speed * 25.4f : surface_speed;
        return surface_mm_per_min / (Pi * safe_diameter_mm);
    }

    float clamp_css_rpm(float rpm) {
        return clamp_css_rpm(rpm, max_css_rpm());
    }

    float clamp_css_rpm(float rpm, float max_rpm) {
        if (rpm <= 0.0f) {
            return 0.0f;
        }
        return max_rpm > 0.0f ? std::min(rpm, max_rpm) : rpm;
    }

    float feed_per_rev_to_mm_per_min(float feed_per_rev, float rpm, bool feed_is_inches) {
        const float feed_mm = feed_is_inches ? feed_per_rev * 25.4f : feed_per_rev;
        return feed_mm * std::max(rpm, 0.0f);
    }

    bool feedback_supports_threading(const FeedbackStatus& status) {
        return status.has_measured_rpm && status.has_index_pulse && status.has_angular_position && !status.stale && !status.fault &&
               status.measured_rpm > 0;
    }

    float x_offset_to_machine_mm(float x_offset, DiameterMode mode) {
        return mode == DiameterMode::Diameter ? x_offset * 0.5f : x_offset;
    }

    float x_program_to_machine_mm(float x_programmed, DiameterMode mode) {
        return x_offset_to_machine_mm(x_programmed, mode);
    }

    float x_machine_to_diameter_mm(float x_machine) {
        return std::fabs(x_machine) * 2.0f;
    }

    bool load_tool_table() {
        reset_tool_table();
        tool_table_loaded = true;

        size_t len = sizeof(tool_table);
        if (nvs.get_blob(ToolTableNvsKey, tool_table.data(), &len) || len != sizeof(tool_table)) {
            reset_tool_table();
            return false;
        }
        return true;
    }

    bool save_tool_table() {
        ensure_tool_table_loaded();
        if (FORCE_BUFFER_SYNC_DURING_NVS_WRITE) {
            protocol_buffer_synchronize();
        }
        return !nvs.set_blob(ToolTableNvsKey, tool_table.data(), sizeof(tool_table));
    }

    void clear_tool_table(bool persist) {
        reset_tool_table();
        tool_table_loaded = true;
        if (persist) {
            if (FORCE_BUFFER_SYNC_DURING_NVS_WRITE) {
                protocol_buffer_synchronize();
            }
            nvs.erase_key(ToolTableNvsKey);
        }
    }

    void set_tool_data(uint32_t tool_number, const ToolData& data) {
        ensure_tool_table_loaded();
        ToolData stored = data;
        stored.valid    = true;

        for (auto& slot : tool_table) {
            if (slot.data.valid && slot.tool_number == tool_number) {
                slot.data = stored;
                save_tool_table();
                return;
            }
        }

        for (auto& slot : tool_table) {
            if (!slot.data.valid) {
                slot.tool_number = tool_number;
                slot.data        = stored;
                save_tool_table();
                return;
            }
        }

        // Fixed-size first release: replace the last slot rather than allocating dynamically.
        tool_table.back().tool_number = tool_number;
        tool_table.back().data        = stored;
        save_tool_table();
    }

    std::optional<ToolData> get_tool_data(uint32_t tool_number) {
        ensure_tool_table_loaded();
        for (const auto& slot : tool_table) {
            if (slot.data.valid && slot.tool_number == tool_number) {
                return slot.data;
            }
        }
        return std::nullopt;
    }

    ActiveToolOffset select_tool(uint32_t tool_number) {
        ensure_tool_table_loaded();
        active_offset = {};
        active_offset.tool_number = tool_number;

        auto tool = get_tool_data(tool_number);
        if (!tool) {
            return active_offset;
        }

        active_offset.x_mm           = tool->geometry_x_mm + tool->wear_x_mm;
        active_offset.z_mm           = tool->geometry_z_mm + tool->wear_z_mm;
        active_offset.nose_radius_mm = tool->nose_radius_mm;
        active_offset.orientation    = tool->orientation;
        active_offset.valid          = true;
        return active_offset;
    }

    ActiveToolOffset active_tool_offset() {
        return active_offset;
    }

    namespace {
        bool append_cycle_move(CyclePlan& plan, CycleMoveKind kind, float x_mm, float z_mm, float feed) {
            if (plan.count >= MaxCycleMoves) {
                plan.error = Error::Overflow;
                plan.valid = false;
                return false;
            }
            plan.moves[plan.count++] = CycleMove { kind, x_mm, z_mm, feed };
            return true;
        }
    }

    CyclePlan build_threading_cycle(const ThreadingCycleSpec& spec) {
        CyclePlan plan;
        if (spec.passes == 0 || spec.passes > MaxCycleMoves || spec.pitch_mm <= 0.0f || spec.start_z_mm == spec.end_z_mm ||
            spec.start_x_mm == spec.end_x_mm) {
            plan.error = Error::GcodeValueWordInvalid;
            return plan;
        }

        for (uint8_t pass = 1; pass <= spec.passes; ++pass) {
            const float blend = static_cast<float>(pass) / static_cast<float>(spec.passes);
            const float x_mm  = spec.start_x_mm + ((spec.end_x_mm - spec.start_x_mm) * blend);
            if (!append_cycle_move(plan, CycleMoveKind::Threading, x_mm, spec.end_z_mm, spec.pitch_mm)) {
                return plan;
            }
            if (pass != spec.passes && !append_cycle_move(plan, CycleMoveKind::Rapid, spec.start_x_mm, spec.start_z_mm, 0.0f)) {
                return plan;
            }
        }

        plan.valid = true;
        return plan;
    }

    CyclePlan build_rough_turning_cycle(const RoughTurningCycleSpec& spec) {
        CyclePlan plan;
        if (spec.depth_step_mm <= 0.0f || spec.rough_feed_mm_min <= 0.0f || spec.start_z_mm == spec.end_z_mm ||
            spec.start_x_mm == spec.final_x_mm) {
            plan.error = Error::GcodeValueWordInvalid;
            return plan;
        }

        const float direction = spec.final_x_mm > spec.start_x_mm ? 1.0f : -1.0f;
        float x_mm = spec.start_x_mm;
        while ((direction > 0.0f && x_mm < spec.final_x_mm) || (direction < 0.0f && x_mm > spec.final_x_mm)) {
            x_mm += direction * spec.depth_step_mm;
            if ((direction > 0.0f && x_mm > spec.final_x_mm) || (direction < 0.0f && x_mm < spec.final_x_mm)) {
                x_mm = spec.final_x_mm;
            }
            if (!append_cycle_move(plan, CycleMoveKind::Linear, x_mm, spec.end_z_mm, spec.rough_feed_mm_min)) {
                return plan;
            }
            if (x_mm != spec.final_x_mm && !append_cycle_move(plan, CycleMoveKind::Rapid, spec.start_x_mm, spec.start_z_mm, 0.0f)) {
                return plan;
            }
        }

        if (spec.include_finish_pass && !append_cycle_move(plan, CycleMoveKind::Linear, spec.final_x_mm, spec.end_z_mm, spec.rough_feed_mm_min)) {
            return plan;
        }

        plan.valid = plan.count > 0;
        return plan;
    }

    Error validate_feature(Feature feature) {
        if (!enabled()) {
            log_info("Lathe " << feature_name(feature) << " requires the machine/lathe configuration section to be enabled");
            return Error::GcodeUnsupportedCommand;
        }
        if (!feature_enabled(feature)) {
            log_info("Lathe " << feature_name(feature) << " is disabled in configuration");
            return Error::GcodeUnsupportedCommand;
        }

        return Error::Ok;
    }
}
