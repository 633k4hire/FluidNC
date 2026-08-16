// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "LatheEncoder.h"

#include "Logging.h"
#include "Machine/MachineConfig.h"
#include "Pin.h"

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
#    include <Arduino.h>
#    include <esp_timer.h>
#    include <soc/gpio_struct.h>
#endif

namespace Lathe {
    namespace {
        EncoderSpindleFeedback encoder_feedback;
        NullSpindleFeedback    null_feedback;
        bool                   capture_active = false;
        pinnum_t               pulse_gpio     = INVALID_PINNUM;
        pinnum_t               direction_gpio = INVALID_PINNUM;
        pinnum_t               index_gpio     = INVALID_PINNUM;
        bool                   direction_invert = false;

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
        bool IRAM_ATTR encoder_b_high() {
            const uint32_t gpio = static_cast<uint32_t>(direction_gpio);
            return gpio < 32U ? ((GPIO.in >> gpio) & 1U) != 0
                              : ((GPIO.in1.val >> (gpio - 32U)) & 1U) != 0;
        }

        void IRAM_ATTR pulse_isr(void* arg) {
            auto* feedback = static_cast<EncoderSpindleFeedback*>(arg);
            int8_t direction = 0;
            if (direction_gpio != INVALID_PINNUM) {
                direction = encoder_b_high() ? -1 : 1;
                if (direction_invert) direction = -direction;
            }
            feedback->record_pulse(static_cast<uint32_t>(esp_timer_get_time()), direction);
        }

        void IRAM_ATTR index_isr(void* arg) {
            auto* feedback = static_cast<EncoderSpindleFeedback*>(arg);
            feedback->record_index(static_cast<uint32_t>(esp_timer_get_time()));
        }
#endif

        void detach_encoder_interrupts() {
#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
            if (pulse_gpio != INVALID_PINNUM) {
                detachInterrupt(pulse_gpio);
            }
            if (index_gpio != INVALID_PINNUM) {
                detachInterrupt(index_gpio);
            }
#endif
            pulse_gpio     = INVALID_PINNUM;
            direction_gpio = INVALID_PINNUM;
            index_gpio     = INVALID_PINNUM;
            direction_invert = false;
            capture_active = false;
        }
    }

    void init_encoder() {
        shutdown_encoder();

        if (!encoder_enabled()) {
            encoder_feedback.configure(1, 1);
            return;
        }

        encoder_feedback.configure(encoder_pulses_per_revolution(), config->_lathe->_feedbackStaleMs);

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
        auto& pulse_pin = config->_lathe->_encoderPulsePin;
        auto& direction_pin = config->_lathe->_encoderBPin;
        auto& index_pin = config->_lathe->_encoderIndexPin;
        direction_invert = config->_lathe->_encoderDirectionInvert;

        pulse_pin.setAttr(Pin::Attr::Input | Pin::Attr::ISR);
        pulse_gpio = pulse_pin.getNative(Pin::Capabilities::Input | Pin::Capabilities::ISR);
        if (!direction_pin.undefined()) {
            direction_pin.setAttr(Pin::Attr::Input);
            direction_gpio = direction_pin.getNative(Pin::Capabilities::Input);
        }
        attachInterruptArg(digitalPinToInterrupt(pulse_gpio), pulse_isr, &encoder_feedback, RISING);

        if (!index_pin.undefined()) {
            index_pin.setAttr(Pin::Attr::Input | Pin::Attr::ISR);
            index_gpio = index_pin.getNative(Pin::Capabilities::Input | Pin::Capabilities::ISR);
            attachInterruptArg(digitalPinToInterrupt(index_gpio), index_isr, &encoder_feedback, RISING);
        }

        capture_active = true;
        log_info("Lathe encoder capture enabled a:" << pulse_pin.name()
                                                     << " b:" << (direction_pin.undefined() ? "none" : direction_pin.name())
                                                               << " b_invert:" << (direction_invert ? "true" : "false")
                                                               << " index:" << (index_pin.undefined() ? "none" : index_pin.name())
                                                               << " ppr:" << encoder_pulses_per_revolution());
#else
        capture_active = true;
        log_warn("Lathe encoder capture configured but hardware GPIO interrupts are unavailable on this build");
#endif
    }

    void shutdown_encoder() {
        detach_encoder_interrupts();
    }

    bool encoder_capture_active() {
        return capture_active;
    }

    void set_encoder_commanded_rpm(SpindleSpeed rpm) {
        encoder_feedback.set_commanded_rpm(rpm);
    }

    const SpindleFeedback& configured_spindle_feedback() {
        return encoder_enabled() && capture_active ? static_cast<const SpindleFeedback&>(encoder_feedback) : static_cast<const SpindleFeedback&>(null_feedback);
    }
}
