// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include "Lathe.h"

namespace Lathe {
    void init_encoder();
    void shutdown_encoder();
    bool encoder_capture_active();
    void set_encoder_commanded_rpm(SpindleSpeed rpm);
    const SpindleFeedback& configured_spindle_feedback();
}
