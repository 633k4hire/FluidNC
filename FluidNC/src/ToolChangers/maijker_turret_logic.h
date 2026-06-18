// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include <cstdint>

namespace ATCs::MaijkerTurretLogic {
    enum class Error {
        None,
        InvalidConfig,
        InvalidTargetTool,
        InvalidCurrentTool,
        UnconfirmedTool,
        SensorUnavailable,
        SensorTimeout,
    };

    struct Config {
        uint32_t station_count         = 5;
        uint32_t steps_per_station     = 320;
        uint32_t overshoot_steps       = 32;
        uint32_t lock_backoff_steps    = 24;
        bool     require_confirmed_tool = true;
    };

    struct ChangePlan {
        Error    error          = Error::None;
        uint32_t delta_stations = 0;
        uint32_t forward_steps  = 0;
        uint32_t lock_steps     = 0;
    };

    struct HomeResult {
        Error    error       = Error::None;
        uint32_t tool_number = 0;
        bool     confirmed   = false;
    };

    bool valid_station_tool(uint32_t tool, uint32_t station_count);
    bool valid_config(const Config& config);
    uint32_t forward_delta(uint32_t current_tool, uint32_t target_tool, uint32_t station_count);
    ChangePlan build_change_plan(const Config& config, uint32_t current_tool, bool confirmed, uint32_t target_tool);
    HomeResult build_home_result(bool sensor_configured, bool sensor_found, uint32_t sensor_tool, uint32_t station_count);
    const char* error_message(Error error);
}
