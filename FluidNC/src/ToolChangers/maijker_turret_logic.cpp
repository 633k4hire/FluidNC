// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "maijker_turret_logic.h"

namespace ATCs::MaijkerTurretLogic {
    bool valid_station_tool(uint32_t tool, uint32_t station_count) {
        return tool >= 1 && tool <= station_count;
    }

    bool valid_config(const Config& config) {
        return config.station_count >= 2 && config.steps_per_station > 0;
    }

    uint32_t forward_delta(uint32_t current_tool, uint32_t target_tool, uint32_t station_count) {
        if (!valid_station_tool(current_tool, station_count) || !valid_station_tool(target_tool, station_count)) {
            return 0;
        }
        return (target_tool + station_count - current_tool) % station_count;
    }

    ChangePlan build_change_plan(const Config& config, uint32_t current_tool, bool confirmed, uint32_t target_tool) {
        ChangePlan plan;
        if (!valid_config(config)) {
            plan.error = Error::InvalidConfig;
            return plan;
        }
        if (!valid_station_tool(target_tool, config.station_count)) {
            plan.error = Error::InvalidTargetTool;
            return plan;
        }
        if (config.require_confirmed_tool && !confirmed) {
            plan.error = Error::UnconfirmedTool;
            return plan;
        }
        if (!valid_station_tool(current_tool, config.station_count)) {
            plan.error = Error::InvalidCurrentTool;
            return plan;
        }

        plan.delta_stations = forward_delta(current_tool, target_tool, config.station_count);
        if (plan.delta_stations == 0) {
            return plan;
        }

        plan.forward_steps = plan.delta_stations * config.steps_per_station + config.overshoot_steps;
        plan.lock_steps    = config.lock_backoff_steps;
        return plan;
    }

    HomeResult build_home_result(bool sensor_configured, bool sensor_found, uint32_t sensor_tool, uint32_t station_count) {
        HomeResult result;
        if (!sensor_configured) {
            result.error = Error::SensorUnavailable;
            return result;
        }
        if (!valid_station_tool(sensor_tool, station_count)) {
            result.error = Error::InvalidConfig;
            return result;
        }
        if (!sensor_found) {
            result.error = Error::SensorTimeout;
            return result;
        }

        result.tool_number = sensor_tool;
        result.confirmed   = true;
        return result;
    }

    const char* error_message(Error error) {
        switch (error) {
            case Error::None:
                return "ok";
            case Error::InvalidConfig:
                return "invalid turret config";
            case Error::InvalidTargetTool:
                return "invalid target tool";
            case Error::InvalidCurrentTool:
                return "invalid current tool";
            case Error::UnconfirmedTool:
                return "current tool is not confirmed";
            case Error::SensorUnavailable:
                return "turret sensor is not configured";
            case Error::SensorTimeout:
                return "turret home sensor timeout";
        }
        return "unknown turret error";
    }
}
