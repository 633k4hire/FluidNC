// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "LatheEncoder.h"

#include "Logging.h"
#include "Machine/MachineConfig.h"
#include "Pin.h"

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
#    include <Arduino.h>
#endif

namespace Lathe {
    namespace {
        EncoderSpindleFeedback encoder_feedback;
        NullSpindleFeedback    null_feedback;
        bool                   capture_active = false;
        pinnum_t               pulse_gpio     = INVALID_PINNUM;
        pinnum_t               index_gpio     = INVALID_PINNUM;

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
        void IRAM_ATTR pulse_isr(void* arg) {
            auto* feedback = static_cast<EncoderSpindleFeedback*>(arg);
            feedback->record_pulse(micros());
        }

        void IRAM_ATTR index_isr(void* arg) {
            auto* feedback = static_cast<EncoderSpindleFeedback*>(arg);
            feedback->record_index(micros());
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
            index_gpio     = INVALID_PINNUM;
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
        auto& index_pin = config->_lathe->_encoderIndexPin;

        pulse_pin.setAttr(Pin::Attr::Input | Pin::Attr::ISR);
        pulse_gpio = pulse_pin.getNative(Pin::Capabilities::Input | Pin::Capabilities::ISR);
        attachInterruptArg(digitalPinToInterrupt(pulse_gpio), pulse_isr, &encoder_feedback, RISING);

        if (!index_pin.undefined()) {
            index_pin.setAttr(Pin::Attr::Input | Pin::Attr::ISR);
            index_gpio = index_pin.getNative(Pin::Capabilities::Input | Pin::Capabilities::ISR);
            attachInterruptArg(digitalPinToInterrupt(index_gpio), index_isr, &encoder_feedback, RISING);
        }

        capture_active = true;
        log_info("Lathe encoder capture enabled pulse:" << pulse_pin.name() << " index:" << (index_pin.undefined() ? "none" : index_pin.name())
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
