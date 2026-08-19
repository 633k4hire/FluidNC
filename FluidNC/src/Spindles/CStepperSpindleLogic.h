// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Spindles::CStepperLogic {
    inline bool rpm_is_commandable(float rpm, float minimum_rpm, float maximum_rpm) {
        return std::isfinite(rpm) && rpm >= minimum_rpm && rpm <= maximum_rpm;
    }

    inline uint32_t steps_per_revolution(float steps_per_degree) {
        return steps_per_degree <= 0.0f ? 0U : static_cast<uint32_t>(std::lround(steps_per_degree * 360.0f));
    }

    inline uint32_t step_rate_millihz(float rpm, uint32_t steps_per_rev) {
        if (rpm <= 0.0f || steps_per_rev == 0) {
            return 0;
        }
        return static_cast<uint32_t>(std::lround(rpm * static_cast<float>(steps_per_rev) * 1000.0f / 60.0f));
    }

    inline uint32_t acceleration_millihz_per_sec(float rpm_per_sec, uint32_t steps_per_rev) {
        if (rpm_per_sec <= 0.0f || steps_per_rev == 0) {
            return 0;
        }
        return static_cast<uint32_t>(std::lround(rpm_per_sec * static_cast<float>(steps_per_rev) * 1000.0f / 60.0f));
    }

    inline int32_t indexed_angle_steps(float angular_position_rev, uint32_t steps_per_rev) {
        if (!std::isfinite(angular_position_rev) || steps_per_rev == 0) {
            return 0;
        }
        const float normalized = angular_position_rev - std::floor(angular_position_rev);
        const int64_t rounded = static_cast<int64_t>(std::llround(normalized * static_cast<float>(steps_per_rev)));
        return static_cast<int32_t>(rounded % static_cast<int64_t>(steps_per_rev));
    }
}
