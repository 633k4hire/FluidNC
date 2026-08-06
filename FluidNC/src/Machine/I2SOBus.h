// Copyright (c) 2021 -  Stefan de Bruijn
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include "Configuration/Configurable.h"

#include <cstdint>

namespace Machine {
    class I2SOBus : public Configuration::Configurable {
        static const int NUMBER_PINS = 32;

    public:
        I2SOBus() = default;

        Pin _bck;
        Pin _data;
        Pin _ws;
        Pin _oe;

        uint32_t _min_pulse_us = 2;
#ifdef TAMS_MAIJKER_I2S_FIFO_PROFILE
        uint32_t _fifoThreshold = 40;
        uint32_t _fifoReload    = 23;
#else
        uint32_t _fifoThreshold = 16;
        uint32_t _fifoReload    = 8;
#endif

        void validate() override;
        void group(Configuration::HandlerBase& handler) override;

        void init();
        void push();

        ~I2SOBus() = default;
    };
}
