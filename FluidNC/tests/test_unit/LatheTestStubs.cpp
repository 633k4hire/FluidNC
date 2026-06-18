#include "../../src/EnumItem.h"
#include "../../src/Machine/MachineConfig.h"
#include "../../include/Driver/NVS.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

Machine::MachineConfig* config = nullptr;

const EnumItem messageLevels2[] = { EnumItem(0) };

void protocol_buffer_synchronize() {}

namespace {
    std::map<std::string, std::vector<uint8_t>> nvs_blobs;
}

NVS nvs("test");

NVS::NVS(const char* name) {}

bool NVS::get_i32(const char* key, int32_t* out_value) {
    return true;
}

bool NVS::get_i8(const char* key, int8_t* out_value) {
    return true;
}

bool NVS::get_str(const char* key, char* out_value, size_t* length) {
    return true;
}

bool NVS::get_blob(const char* key, void* out_value, size_t* length) {
    auto it = nvs_blobs.find(key);
    if (it == nvs_blobs.end() || *length < it->second.size()) {
        return true;
    }

    std::memcpy(out_value, it->second.data(), it->second.size());
    *length = it->second.size();
    return false;
}

bool NVS::erase_key(const char* key) {
    nvs_blobs.erase(key);
    return false;
}

bool NVS::erase_all() {
    nvs_blobs.clear();
    return false;
}

bool NVS::set_i8(const char* key, int8_t value) {
    return false;
}

bool NVS::set_i32(const char* key, int32_t value) {
    return false;
}

bool NVS::set_str(const char* key, const char* value) {
    return false;
}

bool NVS::set_blob(const char* key, const void* value, size_t length) {
    auto* bytes = static_cast<const uint8_t*>(value);
    nvs_blobs[key] = std::vector<uint8_t>(bytes, bytes + length);
    return false;
}

bool NVS::get_stats(size_t& used, size_t& free, size_t& total) {
    used = 0;
    for (const auto& entry : nvs_blobs) {
        used += entry.second.size();
    }
    free = 0;
    total = used;
    return false;
}
