#pragma once

#include "Error.h"

#include <cstdint>
#include <string>

namespace LatheDiagnostics {
    void recordLine(const char* source, const char* line, Error result);
    void recordRealtime(const char* source, uint32_t command);
    void recordStepperWake();
    void recordStepperIdle();

    // A bounded, credential-free snapshot intended for the read-only
    // diagnostics API. Recent events are fixed-size and never contain settings
    // values or network credentials.
    std::string snapshotJson();
    std::string encoderTimingJson();
}
