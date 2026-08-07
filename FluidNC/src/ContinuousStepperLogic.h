// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace Machine::ContinuousStepperLogic {
    inline uint32_t stop_timeout_ms(uint32_t current_rate,
                                    uint32_t deceleration_per_sec,
                                    uint32_t margin_ms = 500) {
        if (current_rate == 0) {
            return margin_ms;
        }
        if (deceleration_per_sec == 0) {
            return 0;
        }

        const uint64_t ramp_ms =
            (static_cast<uint64_t>(current_rate) * 1000U + deceleration_per_sec - 1U) / deceleration_per_sec;
        const uint64_t total_ms = ramp_ms + margin_ms;
        return total_ms > std::numeric_limits<uint32_t>::max()
                   ? std::numeric_limits<uint32_t>::max()
                   : static_cast<uint32_t>(total_ms);
    }

    inline uint32_t ramp_rate(uint32_t current,
                              uint32_t target,
                              uint32_t acceleration_per_sec,
                              uint32_t elapsed_ms,
                              uint32_t& fractional_remainder) {
        if (current == target || elapsed_ms == 0) {
            if (current == target) {
                fractional_remainder = 0;
            }
            return current;
        }

        const uint64_t scaled = static_cast<uint64_t>(acceleration_per_sec) * elapsed_ms + fractional_remainder;
        const uint64_t quotient = scaled / 1000U;
        fractional_remainder = static_cast<uint32_t>(scaled % 1000U);

        const uint32_t maximum_delta = quotient > std::numeric_limits<uint32_t>::max()
                                           ? std::numeric_limits<uint32_t>::max()
                                           : static_cast<uint32_t>(quotient);
        if (maximum_delta == 0) {
            return current;
        }
        if (current < target) {
            return target - current <= maximum_delta ? target : current + maximum_delta;
        }
        return current - target <= maximum_delta ? target : current - maximum_delta;
    }
}
