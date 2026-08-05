// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Spindles::CStepperLogic {
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
}
