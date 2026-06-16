// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include "Configuration/Configurable.h"
#include "Configuration/HandlerBase.h"
#include <cstdint>
#include <stdexcept>

namespace Machine {
    class LatheConfig : public Configuration::Configurable {
    public:
        bool     _enable           = false;
        bool     _enableCSS        = false;
        bool     _enableFeedPerRev = false;
        bool     _enableThreading  = false;
        float    _minCssDiameter   = 1.0f;
        float    _maxCssRpm        = 0.0f;
        int32_t  _xAxis            = 0;
        int32_t  _zAxis            = 2;
        uint32_t _feedbackStaleMs  = 250;

        void group(Configuration::HandlerBase& handler) override {
            handler.item("enable", _enable);
            handler.item("enable_css", _enableCSS);
            handler.item("enable_feed_per_rev", _enableFeedPerRev);
            handler.item("enable_threading", _enableThreading);
            handler.item("min_css_diameter_mm", _minCssDiameter, 0.001f, 1000000.0f);
            handler.item("max_css_rpm", _maxCssRpm, 0.0f, 10000000.0f);
            handler.item("x_axis", _xAxis, 0, 5);
            handler.item("z_axis", _zAxis, 0, 5);
            handler.item("feedback_stale_ms", _feedbackStaleMs, 1, 60000);
        }

        void validate() override {
            if (!_enable && (_enableCSS || _enableFeedPerRev || _enableThreading)) {
                throw std::runtime_error("Lathe sub-features require lathe/enable: true");
            }
            if (_enableCSS && _maxCssRpm <= 0.0f) {
                throw std::runtime_error("Lathe CSS requires max_css_rpm greater than 0");
            }
            if (_enableCSS && _minCssDiameter <= 0.0f) {
                throw std::runtime_error("Lathe CSS requires min_css_diameter_mm greater than 0");
            }
            if (_enable && _xAxis == _zAxis) {
                throw std::runtime_error("Lathe x_axis and z_axis must be different");
            }
        }
    };
}
