// Copyright (c) 2014 Luc Lebosse. All rights reserved.
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "Machine/MachineConfig.h"
#include "Serial.h"    // is_realtime_command()
#include "Settings.h"  // settings_execute_line()

#include "WebUIServer.h"

#include "Mdns.h"

#include <WiFi.h>
#include <StreamString.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <esp_wifi_types.h>
#include <DNSServer.h>

#include "WSChannel.h"

#include "WebClient.h"

#include "Protocol.h"  // protocol_send_event
#include "RealtimeCmd.h"
#include "State.h"
#include "Planner.h"
#include "GCode.h"
#include "Lathe.h"
#include "LatheDiagnostics.h"
#include "Report.h"
#include "ToolChangers/maijker_turret.h"
#include "FluidPath.h"
#include "JSONEncoder.h"
#include "FileStream.h"
#include "DialFirmwareClient.h"
#include "TamsFirmwarePackage.h"
#include "TamsFirmwareTrust.h"

#include "HashFS.h"
#include <list>
#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <cctype>
#include <cmath>
#include <memory>
#include <vector>

#include "Mime.h"  // getContentType

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "WebDAV.h"

namespace WebUI {
    const byte DNS_PORT = 53;
    DNSServer  dnsServer;
}

namespace {
    struct FirmwareValidationContext {
        TamsFirmware::StreamValidator validator;
        size_t                        expected = 0;
    };

    struct FirmwareRequestBody {
        std::vector<uint8_t> bytes;
        size_t               expected = 0;
        bool                 overflow = false;
    };

    struct LastFirmwareValidation {
        TamsFirmware::ValidationResult result;
        std::string                    target;
        uint32_t                       at         = 0;
        bool                           compatible = false;
    };

    LastFirmwareValidation lastFirmwareValidation;

    struct ConsoleSession {
        bool        active = false;
        uint32_t    remoteAddress = 0;
        std::string id;
        std::string csrf;
        std::array<std::string, 8> controls;
        size_t                      controlCursor = 0;
    };

    std::array<ConsoleSession, 8> consoleSessions;
    size_t                        consoleSessionCursor = 0;

    struct ControllerFirmwareDeployment {
        bool                           active   = false;
        bool                           terminal = false;
        bool                           success  = false;
        bool                           receiptPersisted = false;
        uint8_t                        stage    = 0;
        uint32_t                       acceptedOffset = 0;
        uint32_t                       startedAt = 0;
        uint32_t                       lastActivity = 0;
        std::string                    deploymentId;
        std::string                    targetPartition;
        std::string                    error;
        std::string                    result;
        TamsFirmware::ValidationResult package;
        mbedtls_sha256_context         imageHash;
        bool                           hashInitialized = false;
    } controllerDeployment;
    std::string lastRecordedDialReceipt;
    std::atomic_bool       firmwareMaintenanceLock { false };
    constexpr size_t        FirmwareRelayChunkSize  = 4096;
    constexpr size_t        FirmwareBodyLimit       = 8192;
    constexpr size_t        FirmwareReceiptMaxBytes = 2048;
    constexpr const char*   FirmwareReceiptPath     = "/firmware-receipts.jsonl";
    constexpr const char*   FirmwareReceiptTempPath = "/firmware-receipts.tmp";
    constexpr const char*   FirmwareReceiptBackupPath = "/firmware-receipts.bak";
    std::deque<std::string> firmwareReceipts;
    bool                    firmwareReceiptsLoaded = false;

    std::string jsonEscape(const std::string& input) {
        std::string out;
        out.reserve(input.size() + 8);
        for (unsigned char c : input) {
            switch (c) {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c >= 0x20) out += static_cast<char>(c);
                    break;
            }
        }
        return out;
    }

    std::string controllerDeviceId() {
        const char label[] = "tams-fluidnc-controller-v1";
        uint64_t   mac     = ESP.getEfuseMac();
        uint8_t    digest[32];
        mbedtls_sha256_context context;
        mbedtls_sha256_init(&context);
        mbedtls_sha256_starts_ret(&context, 0);
        mbedtls_sha256_update_ret(&context, reinterpret_cast<const uint8_t*>(label), sizeof(label) - 1);
        mbedtls_sha256_update_ret(&context, reinterpret_cast<const uint8_t*>(&mac), sizeof(mac));
        mbedtls_sha256_finish_ret(&context, digest);
        mbedtls_sha256_free(&context);
        static const char hex[] = "0123456789abcdef";
        std::string id = "fluidnc-";
        for (size_t index = 0; index < 8; ++index) {
            id += hex[digest[index] >> 4];
            id += hex[digest[index] & 0xf];
        }
        return id;
    }

    std::string randomHex(size_t bytes) {
        static const char digits[] = "0123456789abcdef";
        std::string       value;
        value.reserve(bytes * 2);
        for (size_t index = 0; index < bytes; ++index) {
            uint8_t octet = static_cast<uint8_t>(esp_random());
            value += digits[octet >> 4];
            value += digits[octet & 0x0f];
        }
        return value;
    }

    bool constantStringEquals(const std::string& left, const std::string& right) {
        if (left.size() != right.size()) return false;
        uint8_t difference = 0;
        for (size_t index = 0; index < left.size(); ++index) {
            difference |= static_cast<uint8_t>(left[index] ^ right[index]);
        }
        return difference == 0;
    }

    std::string cookieValue(AsyncWebServerRequest* request, const char* name) {
        if (!request || !request->hasHeader("Cookie")) return {};
        std::string cookies(request->getHeader("Cookie")->value().c_str());
        std::string marker = std::string(name) + "=";
        size_t position = cookies.find(marker);
        while (position != std::string::npos && position > 0 && cookies[position - 1] != ' ' && cookies[position - 1] != ';') {
            position = cookies.find(marker, position + marker.size());
        }
        if (position == std::string::npos) return {};
        position += marker.size();
        size_t end = cookies.find(';', position);
        return cookies.substr(position, end == std::string::npos ? std::string::npos : end - position);
    }

    ConsoleSession* consoleSessionForRequest(AsyncWebServerRequest* request) {
        const std::string id = cookieValue(request, "TAMSCONSOLE");
        if (id.empty()) return nullptr;
        const uint32_t remote = static_cast<uint32_t>(request->client()->remoteIP());
        for (auto& session : consoleSessions) {
            if (session.active && session.remoteAddress == remote && constantStringEquals(session.id, id)) {
                return &session;
            }
        }
        return nullptr;
    }

    ConsoleSession& createConsoleSession(AsyncWebServerRequest* request) {
        ConsoleSession& session = consoleSessions[consoleSessionCursor++ % consoleSessions.size()];
        session.active = true;
        session.remoteAddress = static_cast<uint32_t>(request->client()->remoteIP());
        session.id = randomHex(16);
        session.csrf = randomHex(16);
        for (auto& control : session.controls) control.clear();
        session.controlCursor = 0;
        return session;
    }

    bool consoleCsrfAuthorized(AsyncWebServerRequest* request) {
        ConsoleSession* session = consoleSessionForRequest(request);
        if (!session || !request->hasHeader("X-CSRF-Token")) return false;
        return constantStringEquals(session->csrf, request->getHeader("X-CSRF-Token")->value().c_str());
    }

    bool consoleControlAuthorized(AsyncWebServerRequest* request) {
        ConsoleSession* session = consoleSessionForRequest(request);
        if (!session || !consoleCsrfAuthorized(request) || !request->hasHeader("X-TAMS-Control-Token")) {
            return false;
        }
        const std::string candidate = request->getHeader("X-TAMS-Control-Token")->value().c_str();
        if (candidate.empty()) return false;
        for (const auto& control : session->controls) {
            if (!control.empty() && constantStringEquals(control, candidate)) return true;
        }
        return false;
    }

    std::string issueConsoleControl(ConsoleSession& session) {
        std::string& control = session.controls[session.controlCursor++ % session.controls.size()];
        control = randomHex(24);
        return control;
    }

    void revokeConsoleControl(ConsoleSession& session, AsyncWebServerRequest* request) {
        if (!request->hasHeader("X-TAMS-Control-Token")) return;
        const std::string candidate = request->getHeader("X-TAMS-Control-Token")->value().c_str();
        for (auto& control : session.controls) {
            if (!control.empty() && constantStringEquals(control, candidate)) {
                control.clear();
                return;
            }
        }
    }

    uint32_t controllerReleaseCounter() {
        Preferences preferences;
        preferences.begin("tamsfw", true);
        uint32_t counter = preferences.getULong("release_ctr", 0);
        preferences.end();
        return counter;
    }

    void reconcileControllerReleaseCounter() {
        Preferences preferences;
        preferences.begin("tamsfw", false);
        if (preferences.getBool("pending", false)) {
            std::string expected = preferences.getString("pending_ver", "").c_str();
            std::string pendingPartition = preferences.getString("pending_part", "").c_str();
            const esp_partition_t* running = esp_ota_get_running_partition();
            if (!expected.empty() && expected == git_info && running &&
                !pendingPartition.empty() && pendingPartition == running->label) {
                preferences.putULong("release_ctr", preferences.getULong("pending_rel", 0));
                preferences.putString("last_result", "success");
            } else {
                preferences.putString("last_result", "rollback_or_recovery");
            }
            preferences.putBool("pending", false);
        }
        preferences.end();
    }

    std::string bodyString(const FirmwareRequestBody* body) {
        if (!body || body->bytes.empty()) return {};
        return std::string(reinterpret_cast<const char*>(body->bytes.data()), body->bytes.size());
    }

    std::string bodyJsonString(const std::string& json, const char* key) {
        std::string marker = "\"" + std::string(key) + "\"";
        size_t      at     = json.find(marker);
        if (at == std::string::npos) return {};
        at = json.find(':', at + marker.size());
        if (at == std::string::npos) return {};
        at = json.find('"', at + 1);
        if (at == std::string::npos) return {};
        size_t end = json.find('"', at + 1);
        if (end == std::string::npos) return {};
        return json.substr(at + 1, end - at - 1);
    }

    bool bodyJsonNumber(const std::string& json, const char* key, double& value) {
        std::string marker = "\"" + std::string(key) + "\"";
        size_t      at     = json.find(marker);
        if (at == std::string::npos) return false;
        at = json.find(':', at + marker.size());
        if (at == std::string::npos) return false;
        char* end = nullptr;
        value = strtod(json.c_str() + at + 1, &end);
        return end != json.c_str() + at + 1 && std::isfinite(value);
    }

    bool bodyJsonTrue(const std::string& json, const char* key) {
        std::string marker = "\"" + std::string(key) + "\"";
        size_t      at     = json.find(marker);
        if (at == std::string::npos) return false;
        at = json.find(':', at + marker.size());
        if (at == std::string::npos) return false;
        ++at;
        while (at < json.size() && isspace(static_cast<unsigned char>(json[at]))) ++at;
        return json.compare(at, 4, "true") == 0;
    }

    std::string deploymentIdFromUrl(const std::string& url) {
        constexpr const char prefix[] = "/api/v1/firmware/deployments/";
        if (url.rfind(prefix, 0) != 0) return {};
        size_t begin = sizeof(prefix) - 1;
        size_t end   = url.find('/', begin);
        return url.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    }

    void loadFirmwareReceipts() {
        if (firmwareReceiptsLoaded) return;
        firmwareReceiptsLoaded = true;
        try {
            FluidPath path(FirmwareReceiptPath, LocalFS);
            if (!stdfs::exists(path)) {
                path = FluidPath(FirmwareReceiptBackupPath, LocalFS);
                if (!stdfs::exists(path)) return;
            }
            FileStream input(path, "r");
            std::string current;
            bool        overflow = false;
            while (input.available()) {
                int value = input.read();
                if (value < 0) break;
                if (value == '\n') {
                    if (!overflow && !current.empty()) firmwareReceipts.push_back(current);
                    current.clear();
                    overflow = false;
                } else if (value != '\r') {
                    if (current.size() < FirmwareReceiptMaxBytes) current += static_cast<char>(value);
                    else overflow = true;
                }
            }
            if (!overflow && !current.empty()) firmwareReceipts.push_back(current);
            while (firmwareReceipts.size() > 16) firmwareReceipts.pop_front();
        } catch (...) {
            firmwareReceipts.clear();
        }
    }

    bool persistFirmwareReceipts() {
        try {
            FluidPath temp(FirmwareReceiptTempPath, LocalFS);
            {
                FileStream output(temp, "w");
                for (const auto& line : firmwareReceipts) {
                    output.write(reinterpret_cast<const uint8_t*>(line.data()), line.size());
                    output.write(static_cast<uint8_t>('\n'));
                }
            }
            FluidPath destination(FirmwareReceiptPath, LocalFS);
            FluidPath backup(FirmwareReceiptBackupPath, LocalFS);
            std::error_code error;
            if (stdfs::exists(backup)) stdfs::remove(backup, error);
            error.clear();
            bool hadDestination = stdfs::exists(destination);
            if (hadDestination) {
                stdfs::rename(destination, backup, error);
                if (error) return false;
            }
            error.clear();
            stdfs::rename(temp, destination, error);
            if (error) {
                if (hadDestination) {
                    std::error_code restoreError;
                    stdfs::rename(backup, destination, restoreError);
                }
                return false;
            }
            if (hadDestination) {
                error.clear();
                stdfs::remove(backup, error);
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    bool appendFirmwareReceipt(const WebUI::DialDeploymentState& deployment) {
        loadFirmwareReceipts();
        std::string receipt = "{\"deployment_id\":\"" + jsonEscape(deployment.deploymentId) +
                              "\",\"controller_id\":\"" + controllerDeviceId() + "\",\"controller_version\":\"" +
                              jsonEscape(git_info) + "\",\"target\":\"m5dial\",\"target_id\":\"" +
                              jsonEscape(WebUI::DialFirmwareClient::instance().state().deviceId) +
                              "\",\"target_fingerprint\":\"" +
                              jsonEscape(WebUI::DialFirmwareClient::instance().state().fingerprint) +
                              "\",\"target_ip\":\"" +
                              jsonEscape(WebUI::DialFirmwareClient::instance().state().ip) +
                              "\",\"package_id\":\"" + jsonEscape(deployment.packageId) +
                              "\",\"signing_key_id\":\"" + jsonEscape(deployment.signingKeyId) +
                              "\",\"from_version\":\"" + jsonEscape(deployment.fromVersion) +
                              "\",\"to_version\":\"" + jsonEscape(deployment.toVersion) +
                              "\",\"from_release_counter\":" + std::to_string(deployment.fromReleaseCounter) +
                              ",\"to_release_counter\":" + std::to_string(deployment.toReleaseCounter) +
                              ",\"manifest_sha256\":\"" + jsonEscape(deployment.manifestDigest) +
                              "\",\"image_sha256\":\"" + jsonEscape(deployment.imageSha256) +
                              "\",\"bytes\":" + std::to_string(deployment.acceptedOffset) +
                              ",\"validation_stages\":[\"controller_signature\",\"controller_hash\",\"target_identity\","
                              "\"target_signature\",\"target_hash\",\"reboot\",\"reconnect\",\"health\"]"
                              ",\"result\":\"" + jsonEscape(deployment.result) +
                              "\",\"health\":\"" +
                              jsonEscape(WebUI::DialFirmwareClient::instance().state().health) +
                              "\",\"fluidnc_link_state\":\"" +
                              jsonEscape(WebUI::DialFirmwareClient::instance().state().fluidNcLinkState) +
                              "\",\"rollback_recovery\":\"" +
                               (deployment.success ? "not_required" : "available_on_previous_ota_slot") +
                               "\",\"started_uptime_ms\":" + std::to_string(deployment.startedAt) +
                               ",\"recorded_uptime_ms\":" + std::to_string(millis()) +
                               ",\"receipt_persisted\":true}";
        if (receipt.size() > FirmwareReceiptMaxBytes) return false;
        std::string removed;
        bool removedOldest = firmwareReceipts.size() >= 16;
        if (removedOldest) {
            removed = firmwareReceipts.front();
            firmwareReceipts.pop_front();
        }
        firmwareReceipts.push_back(receipt);
        if (persistFirmwareReceipts()) return true;
        if (!firmwareReceipts.empty()) firmwareReceipts.pop_back();
        if (removedOldest) firmwareReceipts.push_front(removed);
        return false;
    }

    bool appendControllerFirmwareReceipt() {
        loadFirmwareReceipts();
        const auto& package = controllerDeployment.package;
        std::string receipt =
            "{\"deployment_id\":\"" + jsonEscape(controllerDeployment.deploymentId) +
            "\",\"controller_id\":\"" + controllerDeviceId() + "\",\"target\":\"fluidnc_controller\","
            "\"package_id\":\"" + jsonEscape(package.manifest.packageId) + "\",\"signing_key_id\":\"" +
            jsonEscape(package.manifest.signingKeyId) + "\",\"from_version\":\"" + jsonEscape(git_info) +
            "\",\"to_version\":\"" + jsonEscape(package.manifest.version) + "\",\"to_release_counter\":" +
            std::to_string(package.manifest.releaseCounter) + ",\"manifest_sha256\":\"" +
            jsonEscape(package.manifestSha256) + "\",\"image_sha256\":\"" +
            jsonEscape(package.manifest.imageSha256) + "\",\"bytes\":" +
            std::to_string(controllerDeployment.acceptedOffset) +
            ",\"validation_stages\":[\"controller_signature\",\"controller_hash\",\"esp_image\"],\"result\":\"" +
            jsonEscape(controllerDeployment.result) +
            "\",\"rollback_recovery\":\"previous_ota_slot\",\"started_uptime_ms\":" +
            std::to_string(controllerDeployment.startedAt) + ",\"recorded_uptime_ms\":" +
            std::to_string(millis()) + ",\"receipt_persisted\":true}";
        if (receipt.size() > FirmwareReceiptMaxBytes) return false;
        std::string removed;
        bool removedOldest = firmwareReceipts.size() >= 16;
        if (removedOldest) {
            removed = firmwareReceipts.front();
            firmwareReceipts.pop_front();
        }
        firmwareReceipts.push_back(receipt);
        if (persistFirmwareReceipts()) return true;
        if (!firmwareReceipts.empty()) firmwareReceipts.pop_back();
        if (removedOldest) firmwareReceipts.push_front(removed);
        return false;
    }

    std::string controllerDeploymentJson() {
        return "{\"deployment_id\":\"" + jsonEscape(controllerDeployment.deploymentId) +
               "\",\"active\":" + (controllerDeployment.active ? "true" : "false") +
               ",\"terminal\":" + (controllerDeployment.terminal ? "true" : "false") +
               ",\"success\":" + (controllerDeployment.success ? "true" : "false") +
               ",\"stage_index\":" + std::to_string(controllerDeployment.stage) +
               ",\"accepted_offset\":" + std::to_string(controllerDeployment.acceptedOffset) +
               ",\"expected_bytes\":" + std::to_string(controllerDeployment.package.manifest.imageLength) +
               ",\"error\":\"" + jsonEscape(controllerDeployment.error) + "\",\"result\":\"" +
               jsonEscape(controllerDeployment.result) + "\",\"receipt_persisted\":" +
               (controllerDeployment.receiptPersisted ? "true" : "false") + "}";
    }

    bool firmwareSafety(std::string& reason) {
        if (firmwareMaintenanceLock) {
            reason = "another firmware deployment owns the maintenance lock";
            return false;
        }
        if (!state_is(State::Idle)) {
            reason = "machine state is not Idle";
            return false;
        }
        if (plan_get_current_block() != nullptr) {
            reason = "planner is not empty";
            return false;
        }
        if (gc_state.modal.spindle != SpindleState::Disable) {
            reason = "spindle is not off";
            return false;
        }
        if (Lathe::shared_chuck_enabled() && Lathe::shared_chuck_mode() != Lathe::SharedChuckMode::Idle) {
            reason = "shared chuck is not idle";
            return false;
        }
        auto turret = ATCs::maijker_turret_status();
        if (turret.configured && turret.target_tool != 0) {
            reason = "turret action is pending";
            return false;
        }
        reason = "Idle, planner empty, spindle off, shared chuck idle, turret idle";
        return true;
    }

    bool firmwareReadOnlyCommand(String command) {
        command.trim();
        command.toUpperCase();
        return command == "?" || command == "$G" || command == "ESP421" || command == "[ESP421]" ||
               command == "ESP424" || command == "[ESP424]" || command == "ESP426" ||
               command == "[ESP426]" || command == "ESP425" || command == "[ESP425]";
    }

    struct FileListChunkState {
        enum class Phase : uint8_t { Begin, FileEntries, Footer, End, Done };

        explicit FileListChunkState(
            FluidPath root,
            std::string request_path,
            std::string response_status,
            std::string total_bytes,
            std::string used_bytes,
            uint8_t occupation_percent) :
            root_path(std::move(root)),
            path(std::move(request_path)),
            status(std::move(response_status)),
            total(std::move(total_bytes)),
            used(std::move(used_bytes)),
            percent(occupation_percent),
            encoder([this](const char* s) { pending += s; }) {}

        FileListChunkState(const FileListChunkState&) = delete;
        FileListChunkState& operator=(const FileListChunkState&) = delete;

        Phase                     phase          = Phase::Begin;
        stdfs::directory_iterator iter;
        stdfs::directory_iterator end;
        FluidPath                 root_path;
        std::string               path;
        std::string               status;
        std::string               total;
        std::string               used;
        std::string               pending;
        size_t                    pending_offset = 0;
        uint8_t                   percent        = 100;
        bool                      emit_files     = false;
        JSONencoder               encoder;
    };

    int32_t file_entry_size(const stdfs::directory_entry& dir_entry) {
        std::error_code ec;

        if (dir_entry.is_directory(ec) || ec) {
            return -1;
        }

        ec = {};
        if (!dir_entry.is_regular_file(ec) || ec) {
            return -1;
        }

        ec = {};
        auto entry_size = dir_entry.file_size(ec);
        if (ec || entry_size == static_cast<uintmax_t>(-1)) {
            return -1;
        }

        return static_cast<int32_t>(entry_size);
    }

    void advance_file_iterator(FileListChunkState& state) {
        std::error_code ec;
        state.iter.increment(ec);
        if (ec) {
            state.iter = state.end;
        }
    }

    void append_file_entry(FileListChunkState& state) {
        const auto& dir_entry = *state.iter;
        std::string name      = dir_entry.path().filename().string();
        int32_t     size      = file_entry_size(dir_entry);

        state.encoder.begin_object();
        state.encoder.member("name", name);
        state.encoder.member("shortname", name);
        state.encoder.member("size", size);
        state.encoder.member("datetime", "");
        state.encoder.end_object();
        advance_file_iterator(state);
    }

    bool advance_file_list_chunk(FileListChunkState& state) {
        switch (state.phase) {
            case FileListChunkState::Phase::Begin:
                state.encoder.begin();
                if (state.emit_files) {
                    state.encoder.begin_array("files");
                    state.phase = FileListChunkState::Phase::FileEntries;
                } else {
                    state.phase = FileListChunkState::Phase::Footer;
                }
                state.encoder.flush();
                return true;

            case FileListChunkState::Phase::FileEntries:
                if (state.iter == state.end) {
                    state.encoder.end_array();
                    state.phase = FileListChunkState::Phase::Footer;
                } else {
                    append_file_entry(state);
                }
                state.encoder.flush();
                return true;

            case FileListChunkState::Phase::Footer:
                state.encoder.member("path", state.path.c_str());
                state.encoder.member("total", state.total.c_str());
                state.encoder.member("used", state.used.c_str());
                state.encoder.member("occupation", state.percent);
                state.encoder.member("status", state.status.c_str());
                state.phase = FileListChunkState::Phase::End;
                state.encoder.flush();
                return true;

            case FileListChunkState::Phase::End:
                state.encoder.end();
                state.phase = FileListChunkState::Phase::Done;
                return true;

            case FileListChunkState::Phase::Done:
                return false;
        }

        return false;
    }

    AsyncWebServerResponse* create_file_list_response(AsyncWebServerRequest* request,
                                                      const FluidPath&          fpath,
                                                      const std::string&        path,
                                                      const std::string&        status,
                                                      bool                      list_files) {
        std::error_code ec;
        auto            space      = stdfs::space(fpath, ec);
        uint64_t        totalspace = space.capacity;
        uint64_t        usedspace  = totalspace - space.available;
        uint8_t         percent    = totalspace ? (usedspace * 100) / totalspace : 100;

        auto state = std::make_shared<FileListChunkState>(
            fpath,
            path,
            status,
            formatBytes(totalspace),
            formatBytes(usedspace + 1),
            percent);

        if (list_files) {
            state->iter       = stdfs::directory_iterator { fpath, stdfs::directory_options::skip_permission_denied, ec };
            state->emit_files = !ec;
        }

        AsyncWebServerResponse* response = request->beginChunkedResponse(
            asyncsrv::T_application_json,
            [state](uint8_t* buffer, size_t maxLen, size_t total) mutable -> size_t {
                (void)total;

                size_t written = 0;

                while (written < maxLen) {
                    if (state->pending_offset < state->pending.length()) {
                        size_t chunk_len = std::min(maxLen - written, state->pending.length() - state->pending_offset);
                        memcpy(buffer + written, state->pending.data() + state->pending_offset, chunk_len);
                        state->pending_offset += chunk_len;
                        written += chunk_len;
                        continue;
                    }

                    state->pending.clear();
                    state->pending_offset = 0;

                    if (!advance_file_list_chunk(*state)) {
                        break;
                    }
                }

                return written;
            });
        response->addHeader(asyncsrv::T_Cache_Control, asyncsrv::T_no_cache);
        return response;
    }
}

using namespace asyncsrv;

//embedded response file if no files on LocalFS
#include "NoFile.h"

namespace WebUI {
    bool firmwareMaintenanceActive() {
        return firmwareMaintenanceLock.load();
    }

    bool firmwareCommandAllowedDuringMaintenance(const char* command) {
        return firmwareReadOnlyCommand(String(command ? command : ""));
    }

    class FirmwareDeploymentHandler : public AsyncWebHandler {
    public:
        bool canHandle(AsyncWebServerRequest* request) const override final {
            return request->url().startsWith("/api/v1/firmware/deployments");
        }

        void handleRequest(AsyncWebServerRequest* request) override final {
            WebUI_Server::handleFirmwareDeploymentRequest(request);
        }

        void handleBody(
            AsyncWebServerRequest* request, unsigned char* data, size_t len, size_t index, size_t total) override final {
            WebUI_Server::FirmwareDeploymentBody(request, data, len, index, total);
        }

        void handleUpload(
            AsyncWebServerRequest*, const String&, size_t, uint8_t*, size_t, bool) override final {}
    };

    class LatheApiHandler : public AsyncWebHandler {
    public:
        bool canHandle(AsyncWebServerRequest* request) const override final {
            return request->url().startsWith("/api/v1/lathe/");
        }

        void handleRequest(AsyncWebServerRequest* request) override final {
            WebUI_Server::handleLatheApiRequest(request);
        }

        void handleBody(
            AsyncWebServerRequest* request, unsigned char* data, size_t len, size_t index, size_t total) override final {
            WebUI_Server::LatheApiBody(request, data, len, index, total);
        }

        void handleUpload(
            AsyncWebServerRequest*, const String&, size_t, uint8_t*, size_t, bool) override final {}
    };

    // Error codes for upload
    const int ESP_ERROR_AUTHENTICATION   = 1;
    const int ESP_ERROR_FILE_CREATION    = 2;
    const int ESP_ERROR_FILE_WRITE       = 3;
    const int ESP_ERROR_UPLOAD           = 4;
    const int ESP_ERROR_NOT_ENOUGH_SPACE = 5;
    const int ESP_ERROR_UPLOAD_CANCELLED = 6;
    const int ESP_ERROR_FILE_CLOSE       = 7;

    static const char LOCATION_HEADER[] = "Location";

    bool     WebUI_Server::_setupdone            = false;
    uint16_t WebUI_Server::_port                 = 0;
    bool     WebUI_Server::_schedule_reboot      = false;
    uint32_t WebUI_Server::_schedule_reboot_time = 0;

    UploadStatus               WebUI_Server::_upload_status   = UploadStatus::NONE;
    AsyncWebServer*            WebUI_Server::_webserver       = NULL;
    AsyncWebServer*            WebUI_Server::_websocketserver = NULL;
    AsyncHeaderFreeMiddleware* WebUI_Server::_headerFilter    = NULL;
    AsyncWebSocket*            WebUI_Server::_socket_server   = NULL;
    std::string                WebUI_Server::current_session  = "";
#ifdef ENABLE_AUTHENTICATION
    AuthenticationIP* WebUI_Server::_head  = NULL;
    uint8_t           WebUI_Server::_nb_ip = 0;
    const int         MAX_AUTH_IP          = 10;
#endif
    FileStream* WebUI_Server::_uploadFile = nullptr;
    std::string WebUI_Server::_uploadPath = "";  // Store upload directory path for listing

    EnumSetting *http_enable, *http_block_during_motion;
    IntSetting*  http_port;

    WebUI_Server::~WebUI_Server() {
        deinit();
    }

    void WebUI_Server::init() {
#ifdef ENABLE_AUTHENTICATION
        make_authentication_settings();
#endif
        http_port   = new IntSetting("HTTP Port", WEBSET, WA, "ESP121", "HTTP/Port", DEFAULT_HTTP_PORT, MIN_HTTP_PORT, MAX_HTTP_PORT);
        http_enable = new EnumSetting("HTTP Enable", WEBSET, WA, "ESP120", "HTTP/Enable", DEFAULT_HTTP_STATE, &onoffOptions);
        http_block_during_motion = new EnumSetting("Block serving HTTP content during motion",
                                                   WEBSET,
                                                   WA,
                                                   NULL,
                                                   "HTTP/BlockDuringMotion",
                                                   DEFAULT_HTTP_BLOCKED_DURING_MOTION,
                                                   &onoffOptions);

        _setupdone = false;

        if (WiFi.getMode() == WIFI_OFF || !http_enable->get()) {
            return;
        }

        _port = http_port->get();

        //create instance
        _webserver    = new AsyncWebServer(_port);
        _headerFilter = new AsyncHeaderFreeMiddleware();
        reconcileControllerReleaseCounter();
        DialFirmwareClient::instance().init(controllerDeviceId());

        //here the list of headers to be recorded
        _headerFilter->keep("Accept");
        _headerFilter->keep("Accept-Encoding");
        _headerFilter->keep("Cookie");
        _headerFilter->keep("Content-Length");
        _headerFilter->keep("Content-Type");
        _headerFilter->keep("If-None-Match");
        _headerFilter->keep("User-Agent");
        _headerFilter->keep("X-CSRF-Token");
        _headerFilter->keep("X-TAMS-Control-Token");
        _headerFilter->keep("X-TAMS-Target");

        // WebDAV needs these
        _headerFilter->keep("Depth");
        _headerFilter->keep("Destination");

        //For websockets we need to keep these headers, otherwise this wouldn't work!
        _headerFilter->keep("Upgrade");
        _headerFilter->keep("Connection");
        _headerFilter->keep("Sec-WebSocket-Key");
        _headerFilter->keep("Sec-WebSocket-Version");
        _headerFilter->keep("Sec-WebSocket-Protocol");
        _headerFilter->keep("Sec-WebSocket-Extensions");

        _webserver->addMiddlewares({ _headerFilter });

        // No metadata on the FLASH filesystem; it consumes too much space
        auto flash_dav = new WebDAV("/flash", LocalFS, true);
        auto sd_dav    = new WebDAV("/sd", SD, true);

        _webserver->addHandler(flash_dav);
        _webserver->addHandler(sd_dav);

        // The only major difference with websockets for v2 webui vs v3 seems to be the currentID vs CURRENT_ID and activeID vs ACTIVE_ID
        // In order to only have one websocket server (for simplicity and maintability reasons) we could:
        // 1 - Send both messages types all the time
        // 2 - Don't do anything and just send v3 payloads, since at this point it seems we don't rely on this pageId mechanism anymore with
        // our async and cookie session implementation
        // 3 - Remove all of these active and current IDs altogether since again, we may have no need for this anymore
        // 4 - Potentially check for a difference in requests headers of v2 vs v3 to dynamically send the proper payload in the same handler
        // For now, I've settled with #3
        _socket_server = new AsyncWebSocket("/");

        _socket_server->addMiddleware([](AsyncWebServerRequest* request, ArMiddlewareNext next) {
            current_session = getSessionCookie(request);
            next();  // continue middleware chain
        });
        // Passing the current_session globally, lets hope there is no async switch back of other requests to change this in between
        _socket_server->onEvent(
            [](AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
                WSChannels::handleEvent(server, client, type, arg, data, len, current_session);
            });

        _webserver->addHandler(_socket_server);

        //events functions
        //_web_events->onConnect(handle_onevent_connect);
        //events management
        // _webserver->addHandler(_web_events);

        //Web server handlers
        //trick to catch command line on "/" before file being processed
        _webserver->on("/", HTTP_ANY, handle_root);

        //Page not found handler
        _webserver->onNotFound(handle_not_found);

        //need to be there even no authentication to say to UI no authentication
        _webserver->on("/login", HTTP_ANY, handle_login);

        //web commands
        _webserver->on("/command", HTTP_ANY, handle_web_command);
        _webserver->on("/command_silent", HTTP_ANY, handle_web_command_silent);
        _webserver->on("/feedhold_reload", HTTP_ANY, handleFeedholdReload);
        _webserver->on("/cyclestart_reload", HTTP_ANY, handleCyclestartReload);
        _webserver->on("/restart_reload", HTTP_ANY, handleRestartReload);
        _webserver->on("/did_restart", HTTP_ANY, handleDidRestart);

        //LocalFS
        _webserver->on("/files", HTTP_ANY, handleFileList, LocalFSFileupload);

        //web update
        _webserver->on("/updatefw", HTTP_ANY, handleUpdate, WebUpdateUpload);

        // Signed firmware deployment APIs. Raw /updatefw remains available for
        // attended controller recovery; the Maijker production UI never calls it.
        _webserver->on("/api/v1/console/session", HTTP_GET, handleConsoleSession);
        _webserver->on("/api/v1/console/unlock", HTTP_POST, handleConsoleUnlock);
        _webserver->on("/api/v1/console/lock", HTTP_POST, handleConsoleLock);
        _webserver->on("/api/v1/settings", HTTP_GET, handleSettingsApi);
        _webserver->on("/api/v1/settings", HTTP_PUT, handleSettingsApi, nullptr, LatheApiBody);
        _webserver->on("/api/v1/diagnostics/controller", HTTP_GET, handleDiagnosticsApi);
        _webserver->on("/api/v1/diagnostics/m5/grant", HTTP_GET, handleDiagnosticsApi);
        _webserver->on("/api/v1/diagnostics/m5/verify",
                       HTTP_POST,
                       handleDiagnosticsApi,
                       nullptr,
                       LatheApiBody);
        _webserver->on("/api/v1/firmware/devices", HTTP_GET, handleFirmwareDevices);
        _webserver->on("/api/v1/firmware/packages/validate",
                       HTTP_POST,
                       handleFirmwarePackageValidation,
                       nullptr,
                       FirmwarePackageBody);
        _webserver->on("/api/v1/firmware/receipts", HTTP_GET, handleFirmwareReceipts);
        _webserver->on("/api/v1/firmware/pair/start", HTTP_POST, handleFirmwarePairStart);
        _webserver->on("/api/v1/firmware/pair/confirm", HTTP_POST, handleFirmwarePairConfirm);
        _webserver->on("/api/v1/firmware/pair/status", HTTP_GET, handleFirmwarePairStatus);
        _webserver->addHandler(new FirmwareDeploymentHandler());
        _webserver->addHandler(new LatheApiHandler());

        //Direct SD management
        _webserver->on("/upload", HTTP_ANY, handle_direct_SDFileList, SDFileUpload);
        //_webserver->on("/SD", HTTP_ANY, handle_SDCARD);

        if (WiFi.getMode() == WIFI_AP) {
            // if DNSServer is started with "*" for domain name, it will reply with
            // provided IP to all DNS request
            dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
            log_info("Captive Portal Started");
            _webserver->on("/generate_204", HTTP_ANY, handle_root);
            _webserver->on("/gconnectivitycheck.gstatic.com", HTTP_ANY, handle_root);
            //do not forget the / at the end
            _webserver->on("/fwlink/", HTTP_ANY, handle_root);
        }

        log_info("HTTP started on port " << WebUI::http_port->get());
        //start webserver
        _webserver->begin();

        Mdns::add("_http", "_tcp", _port);

        HashFS::hash_all();

        _setupdone = true;
    }

    void WebUI_Server::deinit() {
        _setupdone = false;

        //        SSDP.end();

        Mdns::remove("_http", "_tcp");

        if (_socket_server) {
            delete _socket_server;
            _socket_server = NULL;
        }

        if (_webserver) {
            delete _webserver;
            _webserver = NULL;
        }

        if (_websocketserver) {
            delete _websocketserver;
            _websocketserver = NULL;
        }

        if (_headerFilter) {
            delete _headerFilter;
            _headerFilter = NULL;
        }

#ifdef ENABLE_AUTHENTICATION
        while (_head) {
            AuthenticationIP* current = _head;
            _head                     = _head->_next;
            delete current;
        }
        _nb_ip = 0;
#endif
    }

    std::string WebUI_Server::getSessionCookie(AsyncWebServerRequest* request) {
        if (request->hasHeader("Cookie")) {
            std::string cookies = request->getHeader("Cookie")->value().c_str();

            int pos = cookies.find("sessionId=");
            if (pos != std::string::npos) {
                int pos2 = cookies.find(";", pos);
                return cookies.substr(pos + strlen("sessionId="), pos2);
            }
        }
        return "";
    }

    static void get_random_string(char* str, unsigned int len) {
        unsigned int i;

        // reseed the random number generator
        srand(time(NULL));

        for (i = 0; i < len; i++) {
            // Add random printable ASCII char
            str[i] = (rand() % ('A' - 'Z')) + 'A';
        }
        str[i] = '\0';
    }
    // Send a file, either the specified path or path.gz
    bool WebUI_Server::myStreamFile(AsyncWebServerRequest* request, const char* path, bool download, bool setSession) {
        std::error_code ec;
        FluidPath       fpath { path, LocalFS, ec };
        if (ec) {
            return false;
        }

        bool acceptGz = false;
        if (request->hasHeader("Accept-Encoding")) {
            auto encodings = std::string(request->getHeader("Accept-Encoding")->value().c_str());
            if (encodings.find("gzip") != std::string::npos) {
                acceptGz = true;
            }
        }

        std::string hash;

        // If you load or reload WebUI while a program is running, there is a high
        // risk of stalling the motion because serving a file from
        // the local FLASH filesystem takes away a lot of CPU cycles.  If we get
        // a request for a file when running, reject it to preserve the motion
        // integrity.
        // This can make it hard to debug ISR IRAM problems, because the easiest
        // way to trigger such problems is to refresh WebUI during motion.
        if (http_block_during_motion->get() && inMotionState()) {
            // Check to see if we have a cached hash of the file that can be retrieved without accessing FLASH
            hash = HashFS::hash(fpath, true);
            if (!hash.length() && acceptGz) {
                std::filesystem::path gzpath(fpath);
                gzpath += ".gz";
                hash = HashFS::hash(gzpath, true);
            }

            if (hash.length() && request->hasHeader("If-None-Match") &&
                std::string(request->getHeader("If-None-Match")->value().c_str()) == hash) {
                request->send(304);
                return true;
            }

            WebUI_Server::handleReloadBlocked(request);
            return true;
        }

        // Check for browser cache match
        hash = HashFS::hash(fpath);
        if (!hash.length() && acceptGz) {
            std::filesystem::path gzpath(fpath);
            gzpath += ".gz";
            hash = HashFS::hash(gzpath);
        }
        if (hash.length() && request->hasHeader("If-None-Match") &&
            std::string(request->getHeader("If-None-Match")->value().c_str()) == hash) {
            if (setSession && getSessionCookie(request) == "") {
                char session[9];
                get_random_string(session, sizeof(session) - 1);
                AsyncWebServerResponse* response = request->beginResponse(304);
                response->addHeader("Set-Cookie", ("sessionId=" + std::string(session)).c_str());
                request->send(response);
            } else {
                request->send(304);
            }
            return true;
        }

        bool        isGzip = false;
        FileStream* file   = NULL;
        try {
            file = new FileStream(path, "r", LocalFS);
        } catch (const Error err) {
            if (acceptGz) {
                try {
                    std::string gzpath(fpath);
                    //                    std::filesystem::path gzpath(fpath);
                    gzpath += ".gz";
                    file   = new FileStream(gzpath, "r", LocalFS);
                    isGzip = true;
                } catch (const Error err) {}
            }
        }
        if (!file) {
            log_debug(path << " not found");
            return false;
        }

        AsyncWebServerResponse* response = request->beginResponse(
            getContentType(path), file->size(), [file, request](uint8_t* buffer, size_t maxLen, size_t total) mutable -> size_t {
                if (!file) {
                    request->client()->close();
                    return 0;  //RESPONSE_TRY_AGAIN; // This only works for ChunkedResponse
                }
                if (total >= file->size() || request->method() != HTTP_GET) {
                    file = nullptr;
                    return 0;
                }
                size_t bytes  = min(file->size() - total, maxLen);
                int    actual = file->read(buffer, bytes);  // return 0 even when no bytes were loaded
                if (actual == 0 || (actual + total) >= file->size()) {
                    file = nullptr;
                }
                return actual;  // Return actual bytes read, not requested bytes
            });

        request->onDisconnect([request, file]() { delete file; });

        if (setSession && getSessionCookie(request) == "") {
            char session[9];
            get_random_string(session, sizeof(session) - 1);
            response->addHeader("Set-Cookie", ("sessionId=" + std::string(session)).c_str());
        }
        if (download) {
            response->addHeader("Content-Disposition", "attachment");
        }
        if (hash.length()) {
            response->addHeader("ETag", hash.c_str());
        }
        // content length is set automatically
        // response->setContentLength(file->size());
        if (isGzip) {
            response->addHeader(T_Content_Encoding, T_gzip);
        }
        request->send(response);

        return true;
    }
    void WebUI_Server::sendWithOurAddress(AsyncWebServerRequest* request, const char* content, uint16_t code) {
        auto        ip    = WiFi.getMode() == WIFI_STA ? WiFi.localIP() : WiFi.softAPIP();
        std::string ipstr = IP_string(ip);
        if (_port != 80) {
            ipstr += ":";
            ipstr += std::to_string(_port);
        }

        std::string scontent(content);
        replace_string_in_place(scontent, "$WEB_ADDRESS$", ipstr);
        replace_string_in_place(scontent, "$QUERY$", request->url().c_str());
        request->send(code, "text/html", scontent.c_str());
    }

    // Captive Portal Page for use in AP mode
    const char PAGE_CAPTIVE[] =
        "<HTML>\n<HEAD>\n<title>Captive Portal</title> \n</HEAD>\n<BODY>\n<CENTER>Captive Portal page : $QUERY$- you will be "
        "redirected...\n<BR><BR>\nif not redirected, <a href='http://$WEB_ADDRESS$'>click here</a>\n<BR><BR>\n<PROGRESS name='prg' "
        "id='prg'></PROGRESS>\n\n<script>\nvar i = 0; \nvar x = document.getElementById(\"prg\"); \nx.max=5; \nvar "
        "interval=setInterval(function(){\ni=i+1; \nvar x = document.getElementById(\"prg\"); \nx.value=i; \nif (i>5) "
        "\n{\nclearInterval(interval);\nwindow.location.href='/';\n}\n},1000);\n</script>\n</CENTER>\n</BODY>\n</HTML>\n\n";

    void WebUI_Server::sendCaptivePortal(AsyncWebServerRequest* request) {
        sendWithOurAddress(request, PAGE_CAPTIVE, 200);
    }

    //Default 404 page that is sent when a request cannot be satisfied
    const char PAGE_404[] =
        "<HTML>\n<HEAD>\n<title>Redirecting...</title> \n</HEAD>\n<BODY>\n<CENTER>Unknown page : $QUERY$- you will be "
        "redirected...\n<BR><BR>\nif not redirected, <a href='http://$WEB_ADDRESS$'>click here</a>\n<BR><BR>\n<PROGRESS name='prg' "
        "id='prg'></PROGRESS>\n\n<script>\nvar i = 0; \nvar x = document.getElementById(\"prg\"); \nx.max=5; \nvar "
        "interval=setInterval(function(){\ni=i+1; \nvar x = document.getElementById(\"prg\"); \nx.value=i; \nif (i>5) "
        "\n{\nclearInterval(interval);\nwindow.location.href='/';\n}\n},1000);\n</script>\n</CENTER>\n</BODY>\n</HTML>\n\n";

    void WebUI_Server::send404Page(AsyncWebServerRequest* request) {
        sendWithOurAddress(request, PAGE_404, 404);
    }

    void WebUI_Server::handle_root(AsyncWebServerRequest* request) {
        log_info("WebUI: Request from " << request->client()->remoteIP());
        if (!(request->hasParam("forcefallback") && request->getParam("forcefallback")->value() == "yes")) {
            const char* index = Lathe::enabled() ? "index.html" : "index-legacy.html";
            if (myStreamFile(request, index, false, true)) {
                return;
            }
        }

        // If we did not send index.html, send the default content that provides simple localfs file management
        AsyncWebServerResponse* response = request->beginResponse(200, "text/html", (const uint8_t*)PAGE_NOFILES, PAGE_NOFILES_SIZE);
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    }

    // Handle filenames and other things that are not explicitly registered
    void WebUI_Server::handle_not_found(AsyncWebServerRequest* request) {
        if (!Lathe::enabled() && is_authenticated(request) == AuthenticationLevel::LEVEL_GUEST) {
            request->redirect("/");
            //_webserver->client().stop();
            return;
        }

        std::string path(request->url().c_str());  //request->urlDecode(request->url()).c_str());

        if (path.rfind("/api/", 0) == 0) {
            request->send(404);
            return;
        }

        // Download a file.  The true forces a download instead of displaying the file
        if (myStreamFile(request, path.c_str(), true)) {
            return;
        }

        if (WiFi.getMode() == WIFI_AP) {
            sendCaptivePortal(request);
            return;
        }

        // This lets the user customize the not-found page by
        // putting a "404.htm" file on the local filesystem
        if (myStreamFile(request, "404.htm")) {
            return;
        }

        send404Page(request);
    }

    // WebUI sends a PAGEID arg to identify the websocket it is using
    uint32_t WebUI_Server::getPageid(AsyncWebServerRequest* request) {
        if (request->hasParam("PAGEID")) {
            return request->getParam("PAGEID")->value().toInt();
        }
        return 0;  // ID 0 means none
    }

    void WebUI_Server::synchronousCommand(
        AsyncWebServerRequest* request, const char* cmd, bool silent, AuthenticationLevel auth_level, bool allowedInMotion) {
        (void)auth_level;
        // Can we do this with async?
        if (http_block_during_motion->get() && inMotionState() && !allowedInMotion) {  // ESP800 is to allow a cached paged reload on webui3
            request->send(503, "text/plain", "Try again when not moving\n");
            return;
        }
        char line[256];
        if (!cmd || strlen(cmd) >= sizeof(line)) {
            request->send(413, "application/json", "{\"error\":\"typed command exceeds FluidNC line capacity; use the YAML file editor for structured configuration\"}");
            return;
        }
        strncpy(line, cmd, sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        WebClient* webClient = new WebClient();
        webClient->attachWS(silent);
        webClient->executeCommandBackground(line);
        AsyncWebServerResponse* response =
            request->beginChunkedResponse("", [webClient](uint8_t* buffer, size_t maxLen, size_t total) mutable -> size_t {
                return webClient->copyBufferSafe(buffer, min((int)maxLen, 1024), total);
            });
        // Commands from typed POST/PUT APIs need the same execution path as
        // read-only GET commands. The response callback drains the channel.
        request->onDisconnect([webClient]() {
            webClient->detachWS();
            allChannels.kill(webClient);
        });
        response->addHeader(T_Cache_Control, T_no_cache);
        request->send(response);
        return;
    }

    std::string getSession(AsyncClient* client) {
        return (std::to_string(IPAddress(client->getRemoteAddress())) + ":" + std::to_string(client->getRemotePort()));
    }
    void WebUI_Server::websocketCommand(AsyncWebServerRequest* request, const char* cmd, uint32_t pageid, AuthenticationLevel auth_level) {
        if (auth_level == AuthenticationLevel::LEVEL_GUEST) {
            request->send(401, "text/plain", "Authentication failed\n");
            return;
        }
        std::string session  = getSessionCookie(request);
        bool        hasError = WSChannels::runGCode(pageid, cmd, session);
        request->send(hasError ? 500 : 200, "text/plain", hasError ? "WebSocket dead" : "");
    }

    bool WebUI_Server::isAllowedInMotion(String cmd) {
        if (cmd.startsWith("[ESP800]"))
            return true;

        return false;
    }
    void WebUI_Server::_handle_web_command(AsyncWebServerRequest* request, bool silent) {
        AuthenticationLevel auth_level = is_authenticated(request);
        if (request->hasParam("cmd") || request->hasParam("commandText")) {
            String cmd;
            if (request->hasParam("cmd"))
                cmd = request->getParam("cmd")->value();
            else
                cmd = request->getParam("commandText")->value();
            if (firmwareMaintenanceLock && !firmwareReadOnlyCommand(cmd)) {
                request->send(423, "text/plain", "Firmware maintenance lock rejects machine-control commands\n");
                return;
            }
            // [ESPXXX] commands expect data in the HTTP response
            String cmdUpper = cmd;
            cmdUpper.toUpperCase();
            // Modified async hack // no longer needed...
            //if (cmdUpper.startsWith("[ESP") || cmdUpper.startsWith("$/") || cmdUpper.startsWith("$ESP") {
            // Original check (now also work with $ESP400, but is slower than if it was returned as http response)
            if (cmdUpper.startsWith("[ESP") || cmdUpper.startsWith("$/")) {
                synchronousCommand(request, cmd.c_str(), silent, auth_level, isAllowedInMotion(cmdUpper));
            } else {
                websocketCommand(request, cmd.c_str(), getPageid(request), auth_level);
            }
            return;
        }
        if (request->hasParam("plain")) {
            String command = request->getParam("plain")->value();
            if (firmwareMaintenanceLock && !firmwareReadOnlyCommand(command)) {
                request->send(423, "text/plain", "Firmware maintenance lock rejects machine-control commands\n");
                return;
            }
            synchronousCommand(request, command.c_str(), silent, auth_level);
            return;
        }
        request->send(500, "text/plain", "Invalid command");
    }

    //login status check
    void WebUI_Server::handle_login(AsyncWebServerRequest* request) {
#ifdef ENABLE_AUTHENTICATION
        auto formValue = [request](const char* name) -> String {
            if (request->hasParam(name, true)) return request->getParam(name, true)->value();
            if (request->hasParam(name)) return request->getParam(name)->value();
            return {};
        };
        auto hasFormValue = [request](const char* name) {
            return request->hasParam(name, true) || request->hasParam(name);
        };
        auto sendLogin = [request](uint16_t code,
                                   const char* status,
                                   const char* level,
                                   const char* user,
                                   const char* cookie = nullptr) {
            std::string json = "{\"status\":\"" + jsonEscape(status ? status : "") +
                               "\",\"authentication_lvl\":\"" + jsonEscape(level ? level : "guest") +
                               "\",\"user\":\"" + jsonEscape(user ? user : "") + "\"}";
            AsyncWebServerResponse* response = request->beginResponse(code, T_application_json, json.c_str());
            response->addHeader(T_Cache_Control, T_no_cache);
            if (cookie) response->addHeader("Set-Cookie", cookie);
            request->send(response);
        };

        if (hasFormValue("DISCONNECT")) {
            AuthenticationIP* auth = getAuthForRequest(request);
            if (auth) ClearAuthIP(auth->ip, auth->sessionID);
            sendLogin(200, "Ok", "guest", "", "ESPSESSIONID=0; Max-Age=0; HttpOnly; SameSite=Strict; Path=/");
            return;
        }

        if (!hasFormValue("SUBMIT")) {
            AuthenticationIP* auth = getAuthForRequest(request);
            if (!auth || static_cast<uint32_t>(millis() - auth->last_time) > 360000U) {
                if (auth) ClearAuthIP(auth->ip, auth->sessionID);
                sendLogin(200, "Ok", "guest", "");
                return;
            }
            auth->last_time = millis();
            const char* level = auth->level == AuthenticationLevel::LEVEL_ADMIN ? "admin"
                                : auth->level == AuthenticationLevel::LEVEL_USER ? "user"
                                                                                : "guest";
            sendLogin(200, "Ok", level, auth->userID);
            return;
        }

        if (!hasFormValue("USER") || !hasFormValue("PASSWORD")) {
            sendLogin(400, "Error: Missing data", "guest", "");
            return;
        }
        std::string user     = formValue("USER").c_str();
        std::string password = formValue("PASSWORD").c_str();
        AuthenticationLevel level = AuthenticationLevel::LEVEL_GUEST;
        bool valid = false;
        if (user == DEFAULT_ADMIN_LOGIN) {
            valid = authentication_password_matches(true, password.c_str());
            level = AuthenticationLevel::LEVEL_ADMIN;
        } else if (user == DEFAULT_USER_LOGIN) {
            valid = authentication_password_matches(false, password.c_str());
            level = AuthenticationLevel::LEVEL_USER;
        }
        if (!valid) {
            sendLogin(401, "Error: Incorrect user or password", "guest", "");
            return;
        }

        if (hasFormValue("NEWPASSWORD")) {
            String newPassword = formValue("NEWPASSWORD");
            if (!authentication_set_password(level == AuthenticationLevel::LEVEL_ADMIN, newPassword.c_str())) {
                sendLogin(422, "Error: Password cannot contain spaces", "guest", "");
                return;
            }
        }

        AuthenticationIP* oldAuth = getAuthForRequest(request);
        if (oldAuth) ClearAuthIP(oldAuth->ip, oldAuth->sessionID);
        auto* auth = new AuthenticationIP;
        auth->level = level;
        auth->ip = request->client()->remoteIP();
        strcpy(auth->sessionID, create_session_ID());
        strcpy(auth->csrfToken, create_csrf_token());
        strncpy(auth->userID, user.c_str(), sizeof(auth->userID) - 1);
        auth->userID[sizeof(auth->userID) - 1] = '\0';
        auth->last_time = millis();
        auth->authenticated_at = auth->last_time;
        if (!AddAuthIP(auth)) {
            delete auth;
            sendLogin(503, "Error: Too many authenticated sessions", "guest", "");
            return;
        }
        std::string cookie = "ESPSESSIONID=" + std::string(auth->sessionID) +
                             "; HttpOnly; SameSite=Strict; Path=/";
        sendLogin(200,
                  "Ok",
                  level == AuthenticationLevel::LEVEL_ADMIN ? "admin" : "user",
                  auth->userID,
                  cookie.c_str());
#else
        sendAuth(request, "Ok", "admin", "");
#endif
    }

    // This page is used when you try to reload WebUI during motion,
    // to avoid interrupting that motion.  It lets you wait until
    // motion is finished.
    void WebUI_Server::handleReloadBlocked(AsyncWebServerRequest* request) {
        request->send(503,
                      "text/html",
                      "<!DOCTYPE html><html><body>"
                      "<h3>Cannot load WebUI while GCode Program is Running</h3>"

                      "<button onclick='window.location.replace(\"/feedhold_reload\")'>Pause</button>"
                      "&nbsp;Pause the GCode program with feedhold<br><br>"

                      "<button onclick='window.location.replace(\"/restart_reload\")'>Stop</button>"
                      "&nbsp;Stop the GCode Program with reset<br><br>"

                      "<button onclick='window.location.reload()'>Reload WebUI</button>"
                      "&nbsp;(You must first stop the GCode program or wait for it to finish)<br><br>"

                      "</body></html>");
    }
    void WebUI_Server::handleDidRestart(AsyncWebServerRequest* request) {
        request->send(503,
                      "text/html",
                      "<!DOCTYPE html><html><body>"
                      "<h3>GCode Program has been stopped</h3>"
                      "<button onclick='window.location.replace(\"/\")'>Reload WebUI</button>"
                      "</body></html>");
    }
    // This page issues a feedhold to pause the motion then retries the WebUI reload
    void WebUI_Server::handleFeedholdReload(AsyncWebServerRequest* request) {
        if (firmwareMaintenanceLock) {
            request->send(423, "text/plain", "Firmware maintenance lock rejects feed hold\n");
            return;
        }
        protocol_send_event(&feedHoldEvent);
        //        delay(100);
        //        delay(100);
        // Go to the main page
        request->redirect("/");
    }
    // This page issues a feedhold to pause the motion then retries the WebUI reload
    void WebUI_Server::handleCyclestartReload(AsyncWebServerRequest* request) {
        if (firmwareMaintenanceLock) {
            request->send(423, "text/plain", "Firmware maintenance lock rejects cycle start\n");
            return;
        }
        protocol_send_event(&cycleStartEvent);
        //        delay(100);
        //        delay(100);
        // Go to the main page
        request->redirect("/");
    }
    // This page issues a feedhold to pause the motion then retries the WebUI reload
    void WebUI_Server::handleRestartReload(AsyncWebServerRequest* request) {
        if (firmwareMaintenanceLock) {
            request->send(423, "text/plain", "Firmware maintenance lock rejects reset\n");
            return;
        }
        protocol_send_event(&rtResetEvent);
        //        delay(100);
        //        delay(100);
        // Go to the main page
        request->redirect("/did_restart");
    }

    // push error code and message to websocket.  Used by upload code
    void WebUI_Server::pushError(AsyncWebServerRequest* request, uint16_t code, const char* st, int32_t web_error, uint16_t timeout) {
        if (_socket_server && st) {
            std::string s("ERROR:");
            s += std::to_string(code) + ":";
            s += st;

            WSChannels::sendError(getPageid(request), st, getSessionCookie(request));

            if (web_error != 0 && request) {
                request->send(web_error, "text/xml", st);
            }
        }
    }

    //abort reception of packages
    void WebUI_Server::cancelUpload(AsyncWebServerRequest* request) {
        request->client()->close();
        delay(100);
    }

    //LocalFS files uploader handle
    void WebUI_Server::fileUpload(
        AsyncWebServerRequest* request, const Volume& fs, String filename, size_t index, uint8_t* data, size_t len, bool final) {
        if (!index) {
            if (Lathe::enabled() && !consoleControlAuthorized(request)) {
                _upload_status = UploadStatus::FAILED;
                pushError(request, ESP_ERROR_AUTHENTICATION, "Unlock this browser tab before uploading files", 403);
                return;
            }
            std::string sizeargname(filename.c_str());
            sizeargname += "S";
            size_t filesize = request->hasParam(sizeargname.c_str()) ? request->getParam(sizeargname.c_str())->value().toInt() : 0;
            uploadStart(request, filename.c_str(), filesize, fs);
        }
        if (_upload_status == UploadStatus::ONGOING) {
            uploadWrite(request, data, len);
            if (final) {
                std::string sizeargname(filename.c_str());
                sizeargname += "S";
                size_t filesize = request->hasParam(sizeargname.c_str()) ? request->getParam(sizeargname.c_str())->value().toInt() : 0;
                uploadEnd(request, filesize);
            }
        } else {
            uploadStop();
            return;
        }

        uploadCheck(request);

        return;
    }

    void WebUI_Server::sendJSON(AsyncWebServerRequest* request, uint16_t code, const char* s) {
        AsyncWebServerResponse* response = request->beginResponse(code, T_application_json, s);
        response->addHeader(T_Cache_Control, T_no_cache);
        request->send(response);
    }

    void WebUI_Server::handleConsoleSession(AsyncWebServerRequest* request) {
        if (!Lathe::enabled()) {
            request->send(404, "application/json", "{\"error\":\"operator console is available only for a confirmed lathe\"}");
            return;
        }
        ConsoleSession* session = consoleSessionForRequest(request);
        bool            created = false;
        if (!session) {
            session = &createConsoleSession(request);
            created = true;
        }
        std::string json = "{\"locked\":true,\"csrf_token\":\"" + session->csrf +
                           "\",\"scope\":\"browser_tab\"}";
        AsyncWebServerResponse* response = request->beginResponse(200, T_application_json, json.c_str());
        response->addHeader(T_Cache_Control, T_no_cache);
        if (created) {
            std::string cookie = "TAMSCONSOLE=" + session->id + "; HttpOnly; SameSite=Strict; Path=/";
            response->addHeader("Set-Cookie", cookie.c_str());
        }
        request->send(response);
    }

    void WebUI_Server::handleConsoleUnlock(AsyncWebServerRequest* request) {
        if (!Lathe::enabled() || !consoleCsrfAuthorized(request)) {
            request->send(403, "application/json", "{\"error\":\"valid console session and CSRF token required\"}");
            return;
        }
        ConsoleSession* session = consoleSessionForRequest(request);
        const std::string control = issueConsoleControl(*session);
        sendJSON(request,
                 200,
                 "{\"locked\":false,\"control_token\":\"" + jsonEscape(control) + "\"}");
    }

    void WebUI_Server::handleConsoleLock(AsyncWebServerRequest* request) {
        if (!Lathe::enabled() || !consoleCsrfAuthorized(request)) {
            request->send(403, "application/json", "{\"error\":\"valid console session and CSRF token required\"}");
            return;
        }
        ConsoleSession* session = consoleSessionForRequest(request);
        revokeConsoleControl(*session, request);
        sendJSON(request, 200, "{\"locked\":true}");
    }

    void WebUI_Server::handleSettingsApi(AsyncWebServerRequest* request) {
        auto* body = static_cast<FirmwareRequestBody*>(request->_tempObject);
        auto cleanup = [&]() {
            delete body;
            request->_tempObject = nullptr;
        };
        if (!Lathe::enabled()) {
            cleanup();
            request->send(404, "application/json", "{\"error\":\"lathe settings API is not active\"}");
            return;
        }
        if (request->method() == HTTP_GET) {
            cleanup();
            synchronousCommand(request, "[ESP400]json=yes", true, AuthenticationLevel::LEVEL_ADMIN, true);
            return;
        }
        if (request->method() != HTTP_PUT) {
            cleanup();
            request->send(405, "application/json", "{\"error\":\"settings API requires GET or PUT\"}");
            return;
        }
        if (!consoleControlAuthorized(request)) {
            cleanup();
            request->send(403, "application/json", "{\"error\":\"unlock this browser tab before changing settings\"}");
            return;
        }
        if (!body || body->overflow || body->bytes.size() != body->expected) {
            cleanup();
            request->send(400, "application/json", "{\"error\":\"a bounded JSON body is required\"}");
            return;
        }
        const std::string json = bodyString(body);
        cleanup();
        const std::string path = bodyJsonString(json, "path");
        const std::string type = bodyJsonString(json, "type");
        const std::string value = bodyJsonString(json, "value");
        auto safeField = [](const std::string& field) {
            return field.find_first_of("\r\n\"\\") == std::string::npos;
        };
        if (path.empty() || path.size() > 128 || type.size() != 1 ||
            std::string("IRSAB").find(type[0]) == std::string::npos || value.size() > 256 ||
            !safeField(path) || !safeField(value)) {
            request->send(422, "application/json", "{\"error\":\"invalid typed setting value\"}");
            return;
        }
        std::string command = "[ESP401]P=" + path + " T=" + type + " V=" + value;
        // ESP401 applies the setting's own type, range, and state policy.
        synchronousCommand(request, command.c_str(), true, AuthenticationLevel::LEVEL_ADMIN, true);
    }

    void WebUI_Server::handleDiagnosticsApi(AsyncWebServerRequest* request) {
        auto* body = static_cast<FirmwareRequestBody*>(request->_tempObject);
        auto cleanup = [&]() {
            delete body;
            request->_tempObject = nullptr;
        };
        if (!Lathe::enabled()) {
            cleanup();
            request->send(404,
                          "application/json",
                          "{\"error\":\"lathe diagnostics are not active for this configuration\"}");
            return;
        }

        const std::string url = request->url().c_str();
        if (request->method() == HTTP_GET &&
            url == "/api/v1/diagnostics/controller") {
            cleanup();
            sendJSON(request, 200, LatheDiagnostics::snapshotJson());
            return;
        }

        auto& dial = DialFirmwareClient::instance();
        if (request->method() == HTTP_GET &&
            url == "/api/v1/diagnostics/m5/grant") {
            cleanup();
            const std::string resource =
                request->hasParam("resource")
                    ? request->getParam("resource")->value().c_str()
                    : "";
            const std::string path =
                resource == "link"
                    ? "/api/v1/diagnostics/link"
                    : resource == "screen"
                          ? "/api/v1/diagnostics/screen.bmp"
                          : "";
            DialDiagnosticGrant grant;
            if (path.empty() || !dial.issueDiagnosticGrant(path, grant)) {
                sendJSON(request,
                         path.empty() ? 422 : 503,
                         "{\"error\":\"" +
                             jsonEscape(path.empty()
                                            ? "resource must be link or screen"
                                            : dial.state().lastError) +
                             "\"}");
                return;
            }
            sendJSON(
                request,
                200,
                "{\"ip\":\"" + jsonEscape(grant.ip) + "\",\"path\":\"" +
                    jsonEscape(grant.path) + "\",\"target\":\"" +
                    jsonEscape(grant.target) + "\",\"nonce\":\"" +
                    jsonEscape(grant.nonce) + "\",\"counter\":" +
                    std::to_string(grant.counter) + ",\"manifest_sha256\":\"" +
                    jsonEscape(grant.manifestDigest) + "\",\"body_sha256\":\"" +
                    jsonEscape(grant.bodyDigest) + "\",\"authorization\":\"" +
                    jsonEscape(grant.authorization) + "\",\"expires_ms\":" +
                    std::to_string(grant.expiresMs) + "}");
            return;
        }

        if (request->method() == HTTP_POST &&
            url == "/api/v1/diagnostics/m5/verify") {
            if (!body || body->overflow || body->bytes.size() != body->expected) {
                cleanup();
                request->send(400,
                              "application/json",
                              "{\"error\":\"a bounded JSON body is required\"}");
                return;
            }
            const std::string json = bodyString(body);
            cleanup();
            double counterValue = 0;
            double statusValue = 0;
            const std::string nonce = bodyJsonString(json, "nonce");
            const std::string digest = bodyJsonString(json, "body_sha256");
            const std::string proof = bodyJsonString(json, "response_auth");
            if (!bodyJsonNumber(json, "counter", counterValue) ||
                !bodyJsonNumber(json, "status", statusValue) ||
                counterValue < 1 || counterValue > UINT32_MAX ||
                floor(counterValue) != counterValue ||
                statusValue < 100 || statusValue > 599 ||
                floor(statusValue) != statusValue) {
                request->send(422,
                              "application/json",
                              "{\"error\":\"invalid diagnostic verification fields\"}");
                return;
            }
            const bool valid = dial.verifyDiagnosticResponse(
                nonce,
                static_cast<uint32_t>(counterValue),
                static_cast<int>(statusValue),
                digest,
                proof);
            sendJSON(request,
                     valid ? 200 : 401,
                     valid ? "{\"valid\":true}" :
                             "{\"valid\":false,\"error\":\"M5Dial response authentication failed\"}");
            return;
        }

        cleanup();
        request->send(404, "application/json", "{\"error\":\"unknown diagnostics endpoint\"}");
    }

    void WebUI_Server::sendAuth(AsyncWebServerRequest* request, const char* status, const char* level, const char* user) {
        AsyncResponseStream* response = request->beginResponseStream(T_application_json);
        response->setCode(200);
        response->addHeader(T_Cache_Control, T_no_cache);

        JSONencoder j([response](const char* s) { response->print(s); });
        j.begin();
        j.member("status", status);
        if (*level != '\0') {
            j.member("authentication_lvl", level);
        }
        if (*user != '\0') {
            j.member("user", user);
        }
        j.end();
        request->send(response);
    }

    void WebUI_Server::sendStatus(AsyncWebServerRequest* request, uint16_t code, const char* status) {
        AsyncResponseStream* response = request->beginResponseStream(T_application_json);
        response->setCode(code);
        response->addHeader(T_Cache_Control, T_no_cache);

        JSONencoder j([response](const char* s) { response->print(s); });
        j.begin();
        j.member("status", status);
        j.end();
        request->send(response);
    }

    void WebUI_Server::sendAuthFailed(AsyncWebServerRequest* request) {
        sendStatus(request, 401, "Authentication failed");
    }

    void WebUI_Server::LocalFSFileupload(AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
        fileUpload(request, LocalFS, filename, index, data, len, final);
    }
    void WebUI_Server::SDFileUpload(AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
        fileUpload(request, SD, filename, index, data, len, final);
    }

    //Web Update handler
    void WebUI_Server::handleUpdate(AsyncWebServerRequest* request) {
        bool authorized = consoleMutationAuthorized(request);
        if (!authorized) {
            _upload_status = UploadStatus::NONE;
            request->send(403, "text/plain", "Unlock this browser tab before updating firmware\n");
            return;
        }

        //if success restart
        if (_upload_status == UploadStatus::SUCCESSFUL) {
            sendStatus(request, 200, std::to_string(int(_upload_status)).c_str());
            _schedule_reboot_time = millis() + 1000;
            _schedule_reboot      = true;
        } else {
            sendStatus(request, 200, std::to_string(int(_upload_status)).c_str());
            _upload_status = UploadStatus::NONE;
            firmwareMaintenanceLock = false;
        }
    }

    //File upload for Web update
    void WebUI_Server::WebUpdateUpload(AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
        static size_t   last_upload_update;
        static uint32_t maxSketchSpace = UINT32_MAX;

        bool authorized = consoleMutationAuthorized(request);
        if (!authorized) {
            _upload_status = UploadStatus::FAILED;
            firmwareMaintenanceLock = false;
            log_info("Upload rejected");
            sendAuthFailed(request);
            //pushError(request, ESP_ERROR_AUTHENTICATION, "Upload rejected", 401);
        } else {
            //Upload start
            //**************
            if (!index) {  //upload.status == UPLOAD_FILE_START) {
                log_info("Update Firmware");
                _upload_status = UploadStatus::ONGOING;
                std::string safetyReason;
                if (!firmwareSafety(safetyReason)) {
                    _upload_status = UploadStatus::FAILED;
                    log_info("Update rejected: " << safetyReason);
                    pushError(request, ESP_ERROR_UPLOAD, safetyReason.c_str());
                } else {
                    firmwareMaintenanceLock = true;
                }
                std::string sizeargname(filename.c_str());
                sizeargname += "S";
                if (request->hasParam(sizeargname.c_str()))
                    maxSketchSpace = request->getParam(sizeargname.c_str())->value().toInt();
                else if (request->hasHeader("Content-Length"))
                    maxSketchSpace = request->getHeader("Content-Length")->value().toInt();
                //check space
                size_t flashsize = 0;
                if (esp_ota_get_running_partition()) {
                    const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
                    if (partition) {
                        flashsize = partition->size;
                    }
                }
                if (flashsize < maxSketchSpace) {
                    String msg = String("Upload rejected, not enough space (needs " + String(maxSketchSpace) + ", has " + String(flashsize));
                    pushError(request, ESP_ERROR_NOT_ENOUGH_SPACE, msg.c_str());
                    _upload_status = UploadStatus::FAILED;
                    log_info("Update cancelled");
                }
                if (_upload_status != UploadStatus::FAILED) {
                    last_upload_update = 0;
                    if (!Update.begin()) {  //start with max available size
                        _upload_status = UploadStatus::FAILED;
                        log_info("Update cancelled");
                        pushError(request, ESP_ERROR_NOT_ENOUGH_SPACE, "Upload rejected, not enough space");
                    } else {
                        log_info("Update 0%");
                    }
                }
            }
            //Upload write
            //**************
            //check if no error
            if (_upload_status == UploadStatus::ONGOING) {
                if (((100 * index) / maxSketchSpace) != last_upload_update) {
                    if (maxSketchSpace > 0) {
                        last_upload_update = (100 * index) / maxSketchSpace;
                    } else {
                        last_upload_update = index;
                    }

                    log_info("Update " << last_upload_update << "%");
                }
                if (Update.write(data, len) != len) {
                    _upload_status = UploadStatus::FAILED;
                    log_info("Update write failed");
                    pushError(request, ESP_ERROR_FILE_WRITE, "File write failed");
                }
            }
            //Upload end
            //**************
            if (final) {
                if (_upload_status == UploadStatus::ONGOING && Update.end(true)) {  //true to set the size to the current progress
                    //Now Reboot
                    log_info("Update 100%");
                    _upload_status = UploadStatus::SUCCESSFUL;
                } else {
                    _upload_status = UploadStatus::FAILED;
                    firmwareMaintenanceLock = false;
                    log_info("Update failed");
                    pushError(request, ESP_ERROR_UPLOAD, "Update upload failed");
                }
            }
        }
    }

    void WebUI_Server::handleFirmwareDevices(AsyncWebServerRequest* request) {
        if (!Lathe::enabled()) {
            request->send(404, "application/json", "{\"error\":\"Maijker firmware service requires a confirmed lathe\"}");
            return;
        }
        auto& dial = DialFirmwareClient::instance();
        if (dial.discover() && dial.state().paired) dial.refreshHealth();
        std::string safetyReason;
        bool        safe     = firmwareSafety(safetyReason);
        const auto* inactive = esp_ota_get_next_update_partition(nullptr);
        std::string json     = "{\"controller\":{\"online\":true,\"device_id\":\"" + controllerDeviceId() +
                           "\",\"version\":\"" + jsonEscape(git_info) +
                           "\",\"hardware_role\":\"fluidnc_controller\",\"board\":\"maijker_dlc32\",";
        json += "\"inactive_partition\":\"";
        json += inactive ? inactive->label : "unavailable";
        json += "\",\"inactive_partition_size\":";
        json += std::to_string(inactive ? inactive->size : 0);
        json += ",\"release_counter\":";
        json += std::to_string(controllerReleaseCounter());
        json += "},\"m5dial\":";
        json += dial.stateJson();
        json += ",";
        json += "\"trust_configured\":";
        json += TamsFirmware::trustConfigured() ? "true" : "false";
        json += ",\"safe\":";
        json += safe ? "true" : "false";
        json += ",\"safety_reason\":\"" + jsonEscape(safetyReason) + "\",\"maintenance_lock\":";
        json += firmwareMaintenanceLock ? "true}" : "false}";
        sendJSON(request, 200, json);
    }

    void WebUI_Server::FirmwarePackageBody(
        AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        if (!request->_tempObject) {
            auto* context         = new FirmwareValidationContext();
            context->expected     = total;
            request->_tempObject = context;
        }
        auto* context = static_cast<FirmwareValidationContext*>(request->_tempObject);
        if (index != context->validator.bytesReceived() || total != context->expected) {
            return;
        }
        context->validator.write(data, len);
    }

    void WebUI_Server::handleFirmwarePackageValidation(AsyncWebServerRequest* request) {
        if (!Lathe::enabled() || !consoleCsrfAuthorized(request)) {
            if (request->_tempObject) {
                delete static_cast<FirmwareValidationContext*>(request->_tempObject);
                request->_tempObject = nullptr;
            }
            request->send(403, "application/json", "{\"error\":\"valid same-origin console session required\"}");
            return;
        }
        auto* context = static_cast<FirmwareValidationContext*>(request->_tempObject);
        if (!context) {
            request->send(400, "application/json", "{\"error\":\"package body is required\"}");
            return;
        }
        TamsFirmware::ValidationResult result = context->validator.finish();
        delete context;
        request->_tempObject = nullptr;

        std::string target =
            request->hasHeader("X-TAMS-Target") ? request->getHeader("X-TAMS-Target")->value().c_str() : "";
        std::string compatibilityError;
        bool        compatible = false;
        auto&       dial       = DialFirmwareClient::instance();
        bool        targetExact = false;
        bool        recoveryConfirmationRequired = false;
        if (result.valid()) {
            if (target == "dial") {
                targetExact = dial.discover() && dial.state().paired && !dial.state().ambiguous && dial.refreshHealth();
                recoveryConfirmationRequired = result.keyRecovery && result.manifest.recovery &&
                                               result.manifest.allowDowngrade &&
                                               result.manifest.releaseCounter <= dial.state().releaseCounter;
                compatible = TamsFirmware::targetCompatible(result.manifest,
                                                             "fluiddial",
                                                             "maijker_m5dial",
                                                             "m5dial_hmi",
                                                             "esp32s3",
                                                             "default_8mb_ab",
                                                             1,
                                                             dial.state().releaseCounter,
                                                             recoveryConfirmationRequired,
                                                             compatibilityError);
            } else if (target == "controller") {
                targetExact = true;
                compatible = TamsFirmware::targetCompatible(result.manifest,
                                                             "fluidnc",
                                                             "maijker_dlc32",
                                                             "fluidnc_controller",
                                                             "esp32",
                                                             "min_littlefs_ab",
                                                             1,
                                                             controllerReleaseCounter(),
                                                             false,
                                                             compatibilityError);
            } else {
                compatibilityError = "unknown target role";
            }
        }
        std::string safetyReason;
        bool        safe        = firmwareSafety(safetyReason);
        bool        valid       = result.valid() && compatible;
        lastFirmwareValidation  = { result, target, millis(), compatible };

        std::string reason = result.error.empty() ? compatibilityError : result.error;
        if (reason.empty() && !targetExact) reason = "no exact paired M5Dial is configured";
        if (reason.empty() && !safe) reason = safetyReason;
        if (reason.empty() && recoveryConfirmationRequired)
            reason = "signed recovery downgrade requires green-button confirmation on the M5Dial";
        if (reason.empty()) reason = "package, target, and machine state validated";
        std::string json = "{\"valid\":";
        json += valid ? "true" : "false";
        json += ",\"safe\":";
        json += safe ? "true" : "false";
        json += ",\"target_exact\":";
        json += targetExact ? "true" : "false";
        json += ",\"recovery_confirmation_required\":";
        json += recoveryConfirmationRequired ? "true" : "false";
        json += ",\"manifest_sha256\":\"" + jsonEscape(result.manifestSha256) + "\",\"reason\":\"" + jsonEscape(reason) + "\",";
        json += "\"validation\":{\"signature\":";
        json += result.signatureValid ? "true" : "false";
        json += ",\"hash\":";
        json += result.imageHashValid ? "true" : "false";
        json += ",\"product\":";
        json += (target == "dial" ? result.manifest.product == "fluiddial" : result.manifest.product == "fluidnc") ? "true" : "false";
        json += ",\"board\":";
        json += (target == "dial" ? result.manifest.board == "maijker_m5dial" : result.manifest.board == "maijker_dlc32") ? "true" : "false";
        json += ",\"hardware_role\":";
        json +=
            (target == "dial" ? result.manifest.hardwareRole == "m5dial_hmi" : result.manifest.hardwareRole == "fluidnc_controller")
                ? "true"
                : "false";
        json += ",\"compatibility\":";
        json += compatible ? "true" : "false";
        json += ",\"version\":";
        json += result.manifest.releaseCounter > 0 ? "true" : "false";
        json += "}}";
        sendJSON(request, valid ? 200 : 422, json);
    }

    void WebUI_Server::handleFirmwareReceipts(AsyncWebServerRequest* request) {
        if (!Lathe::enabled()) {
            request->send(404, "application/json", "{\"error\":\"Maijker firmware service requires a confirmed lathe\"}");
            return;
        }
        loadFirmwareReceipts();
        std::string json = "[";
        bool        first = true;
        for (auto it = firmwareReceipts.rbegin(); it != firmwareReceipts.rend(); ++it) {
            if (!first) json += ",";
            json += *it;
            first = false;
        }
        json += "]";
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json.c_str());
        response->addHeader(T_Cache_Control, T_no_cache);
        if (request->hasParam("download")) {
            response->addHeader("Content-Disposition", "attachment; filename=\"firmware-receipts.json\"");
        }
        request->send(response);
    }

    void WebUI_Server::handleFirmwarePairStart(AsyncWebServerRequest* request) {
        if (!consoleMutationAuthorized(request)) {
            request->send(403, "application/json", "{\"error\":\"unlock this browser tab before pairing\"}");
            return;
        }
        if (firmwareMaintenanceLock) {
            request->send(409, "application/json", "{\"error\":\"firmware maintenance lock is active\"}");
            return;
        }
        std::string comparisonCode;
        if (!DialFirmwareClient::instance().startPairing(comparisonCode)) {
            sendJSON(request,
                     409,
                     "{\"error\":\"" + jsonEscape(DialFirmwareClient::instance().state().lastError) + "\"}");
            return;
        }
        sendJSON(request,
                 200,
                 "{\"comparison_code\":\"" + jsonEscape(comparisonCode) +
                     "\",\"instruction\":\"Compare this code, then press the M5Dial center button\"}");
    }

    void WebUI_Server::handleFirmwarePairConfirm(AsyncWebServerRequest* request) {
        if (!consoleMutationAuthorized(request)) {
            request->send(403, "application/json", "{\"error\":\"unlock this browser tab before pairing\"}");
            return;
        }
        String comparisonCode =
            request->hasParam("comparison_code", true) ? request->getParam("comparison_code", true)->value() : "";
        if (comparisonCode.length() != 6 ||
            !DialFirmwareClient::instance().confirmPairing(comparisonCode.c_str())) {
            request->send(409, "application/json", "{\"error\":\"pairing code was not accepted by the M5Dial\"}");
            return;
        }
        request->send(202, "application/json", "{\"status\":\"waiting_for_physical_confirmation\"}");
    }

    void WebUI_Server::handleFirmwarePairStatus(AsyncWebServerRequest* request) {
        if (!Lathe::enabled()) {
            request->send(404, "application/json", "{\"error\":\"Maijker firmware service requires a confirmed lathe\"}");
            return;
        }
        bool paired = DialFirmwareClient::instance().pollPairing();
        std::string json = "{\"paired\":";
        json += paired ? "true" : "false";
        json += ",\"m5dial\":" + DialFirmwareClient::instance().stateJson() + "}";
        sendJSON(request, 200, json);
    }

    void WebUI_Server::FirmwareDeploymentBody(
        AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        if (!request->_tempObject) {
            auto* body          = new FirmwareRequestBody();
            body->expected      = total;
            body->overflow      = total > FirmwareBodyLimit;
            request->_tempObject = body;
        }
        auto* body = static_cast<FirmwareRequestBody*>(request->_tempObject);
        if (body->overflow || total != body->expected || index != body->bytes.size() ||
            body->bytes.size() + len > FirmwareBodyLimit) {
            body->overflow = true;
            return;
        }
        body->bytes.insert(body->bytes.end(), data, data + len);
    }

    void WebUI_Server::handleFirmwareDeploymentRequest(AsyncWebServerRequest* request) {
        auto* body = static_cast<FirmwareRequestBody*>(request->_tempObject);
        auto cleanup = [&]() {
            delete body;
            request->_tempObject = nullptr;
        };
        const std::string url = request->url().c_str();
        const std::string id  = deploymentIdFromUrl(url);
        auto&             dial = DialFirmwareClient::instance();

        if (body && (body->overflow || body->bytes.size() != body->expected)) {
            cleanup();
            request->send(413, "application/json", "{\"error\":\"deployment body exceeded the bounded relay size\"}");
            return;
        }

        if (request->method() == HTTP_GET && !id.empty()) {
            cleanup();
            if (!Lathe::enabled()) {
                request->send(404, "application/json", "{\"error\":\"Maijker firmware service requires a confirmed lathe\"}");
                return;
            }
            if (id == controllerDeployment.deploymentId) {
                sendJSON(request, 200, controllerDeploymentJson());
                return;
            }
            if (id != dial.deployment().deploymentId) {
                request->send(404, "application/json", "{\"error\":\"deployment not found\"}");
                return;
            }
            if (!dial.deployment().terminal && dial.deployment().stage >= 4) {
                dial.discover(true);
                dial.refreshHealth();
            }
            if (dial.deployment().terminal && lastRecordedDialReceipt != dial.deployment().deploymentId) {
                dial.setReceiptPersisted(true);
                bool persisted = appendFirmwareReceipt(dial.deployment());
                dial.setReceiptPersisted(persisted);
                if (persisted) lastRecordedDialReceipt = dial.deployment().deploymentId;
                firmwareMaintenanceLock = false;
            }
            sendJSON(request, 200, dial.deploymentJson());
            return;
        }

        if (!consoleMutationAuthorized(request)) {
            cleanup();
            request->send(403, "application/json", "{\"error\":\"unlock this browser tab before deploying firmware\"}");
            return;
        }

        if (request->method() == HTTP_POST && url == "/api/v1/firmware/deployments") {
            std::string json           = bodyString(body);
            std::string target         = bodyJsonString(json, "target");
            std::string packageId      = bodyJsonString(json, "package_id");
            std::string manifestDigest = bodyJsonString(json, "manifest_sha256");
            cleanup();
            if (target != "dial" && target != "controller") {
                request->send(422, "application/json", "{\"error\":\"unknown firmware target\"}");
                return;
            }
            std::string safetyReason;
            bool validationFresh = lastFirmwareValidation.at &&
                                   static_cast<uint32_t>(millis() - lastFirmwareValidation.at) <= 300000U;
            if (!validationFresh || lastFirmwareValidation.target != target ||
                !lastFirmwareValidation.result.valid() || !lastFirmwareValidation.compatible ||
                lastFirmwareValidation.result.manifest.packageId != packageId ||
                lastFirmwareValidation.result.manifestSha256 != manifestDigest) {
                request->send(409, "application/json", "{\"error\":\"a matching recent signed-package validation is required\"}");
                return;
            }
            if (!firmwareSafety(safetyReason)) {
                sendJSON(request, 409, "{\"error\":\"" + jsonEscape(safetyReason) + "\"}");
                return;
            }
            std::string deploymentId = randomHex(16);
            if (target == "controller") {
                const auto* inactive = esp_ota_get_next_update_partition(nullptr);
                if (!inactive || lastFirmwareValidation.result.manifest.imageLength > inactive->size) {
                    request->send(413, "application/json", "{\"error\":\"signed controller image does not fit the inactive slot\"}");
                    return;
                }
                if (controllerDeployment.hashInitialized) {
                    mbedtls_sha256_free(&controllerDeployment.imageHash);
                    controllerDeployment.hashInitialized = false;
                }
                controllerDeployment.active = controllerDeployment.terminal = controllerDeployment.success = false;
                controllerDeployment.receiptPersisted = false;
                controllerDeployment.stage = 1;
                controllerDeployment.acceptedOffset = 0;
                controllerDeployment.startedAt = millis();
                controllerDeployment.lastActivity = controllerDeployment.startedAt;
                controllerDeployment.deploymentId = deploymentId;
                controllerDeployment.targetPartition = inactive->label;
                controllerDeployment.error.clear();
                controllerDeployment.result = "receiving signed controller image";
                controllerDeployment.package = lastFirmwareValidation.result;
                mbedtls_sha256_init(&controllerDeployment.imageHash);
                mbedtls_sha256_starts_ret(&controllerDeployment.imageHash, 0);
                controllerDeployment.hashInitialized = true;
                if (!Update.begin(controllerDeployment.package.manifest.imageLength, U_FLASH)) {
                    mbedtls_sha256_free(&controllerDeployment.imageHash);
                    controllerDeployment.hashInitialized = false;
                    controllerDeployment.error = "inactive controller OTA slot could not be opened";
                    request->send(500, "application/json", "{\"error\":\"inactive controller OTA slot could not be opened\"}");
                    return;
                }
                controllerDeployment.active = true;
                firmwareMaintenanceLock = true;
                sendJSON(request,
                         201,
                         "{\"deployment_id\":\"" + deploymentId + "\",\"chunk_size\":" +
                             std::to_string(FirmwareRelayChunkSize) + "}");
                return;
            }
            if (!dial.discover(true) || !dial.state().paired || dial.state().ambiguous || !dial.refreshHealth()) {
                request->send(409, "application/json", "{\"error\":\"the exact paired M5Dial is not authenticated and online\"}");
                return;
            }
            firmwareMaintenanceLock  = true;
            if (!dial.beginDeployment(lastFirmwareValidation.result, deploymentId)) {
                firmwareMaintenanceLock = false;
                sendJSON(request, 502, "{\"error\":\"" + jsonEscape(dial.state().lastError) + "\"}");
                return;
            }
            sendJSON(request,
                     201,
                     "{\"deployment_id\":\"" + deploymentId + "\",\"chunk_size\":" +
                         std::to_string(FirmwareRelayChunkSize) + "}");
            return;
        }

        bool controllerTarget = !id.empty() && id == controllerDeployment.deploymentId;
        if (id.empty() || (!controllerTarget && id != dial.deployment().deploymentId)) {
            cleanup();
            request->send(404, "application/json", "{\"error\":\"deployment not found\"}");
            return;
        }

        if (request->method() == HTTP_PUT && url.find("/chunks") != std::string::npos) {
            uint32_t offset =
                request->hasParam("offset") ? static_cast<uint32_t>(request->getParam("offset")->value().toInt()) : UINT32_MAX;
            bool ok = false;
            if (controllerTarget) {
                ok = controllerDeployment.active && body && !body->bytes.empty() &&
                     offset == controllerDeployment.acceptedOffset &&
                     controllerDeployment.acceptedOffset + body->bytes.size() <=
                         controllerDeployment.package.manifest.imageLength &&
                     Update.write(body->bytes.data(), body->bytes.size()) == body->bytes.size();
                if (ok) {
                    mbedtls_sha256_update_ret(
                        &controllerDeployment.imageHash, body->bytes.data(), body->bytes.size());
                    controllerDeployment.acceptedOffset += body->bytes.size();
                    controllerDeployment.lastActivity = millis();
                    controllerDeployment.stage = 2;
                } else {
                    controllerDeployment.error = "controller chunk offset, size, or flash write failed";
                }
            } else {
                ok = body && !body->bytes.empty() &&
                     dial.relayChunk(offset, body->bytes.data(), body->bytes.size());
            }
            cleanup();
            if (!ok) {
                sendJSON(request,
                         409,
                         "{\"error\":\"" +
                             jsonEscape(controllerTarget ? controllerDeployment.error : dial.deployment().error) + "\"}");
                return;
            }
            sendJSON(request,
                     200,
                     "{\"accepted_offset\":" +
                         std::to_string(controllerTarget ? controllerDeployment.acceptedOffset
                                                         : dial.deployment().acceptedOffset) +
                         "}");
            return;
        }
        cleanup();

        if (request->method() == HTTP_POST && url.find("/commit") != std::string::npos) {
            if (controllerTarget) {
                if (!controllerDeployment.active ||
                    controllerDeployment.acceptedOffset != controllerDeployment.package.manifest.imageLength) {
                    request->send(409, "application/json", "{\"error\":\"controller image transfer is incomplete\"}");
                    return;
                }
                uint8_t digest[32];
                mbedtls_sha256_finish_ret(&controllerDeployment.imageHash, digest);
                static const char hex[] = "0123456789abcdef";
                std::string actual(64, '0');
                for (size_t index = 0; index < sizeof(digest); ++index) {
                    actual[index * 2] = hex[digest[index] >> 4];
                    actual[index * 2 + 1] = hex[digest[index] & 0x0f];
                }
                memset(digest, 0, sizeof(digest));
                mbedtls_sha256_free(&controllerDeployment.imageHash);
                controllerDeployment.hashInitialized = false;
                if (actual != controllerDeployment.package.manifest.imageSha256 || !Update.end(false)) {
                    Update.abort();
                    controllerDeployment.active = false;
                    controllerDeployment.terminal = true;
                    controllerDeployment.error = "controller image hash or ESP image validation failed";
                    controllerDeployment.result = "failed";
                    firmwareMaintenanceLock = false;
                    controllerDeployment.receiptPersisted = appendControllerFirmwareReceipt();
                    sendJSON(request, 422, controllerDeploymentJson());
                    return;
                }
                controllerDeployment.active = false;
                controllerDeployment.terminal = true;
                controllerDeployment.success = true;
                controllerDeployment.stage = 8;
                controllerDeployment.result = "verified_rebooting";
                controllerDeployment.lastActivity = millis();
                Preferences preferences;
                preferences.begin("tamsfw", false);
                preferences.putBool("pending", true);
                preferences.putULong("pending_rel", controllerDeployment.package.manifest.releaseCounter);
                preferences.putString("pending_ver", controllerDeployment.package.manifest.version.c_str());
                preferences.putString("pending_id", controllerDeployment.deploymentId.c_str());
                preferences.putString("pending_part", controllerDeployment.targetPartition.c_str());
                preferences.end();
                controllerDeployment.receiptPersisted = appendControllerFirmwareReceipt();
                _schedule_reboot_time = millis() + 3000;
                _schedule_reboot = true;
                sendJSON(request, 200, controllerDeploymentJson());
                return;
            }
            if (!dial.commitDeployment()) {
                sendJSON(request, 409, "{\"error\":\"" + jsonEscape(dial.deployment().error) + "\"}");
                return;
            }
            sendJSON(request, 200, dial.deploymentJson());
            return;
        }
        if (request->method() == HTTP_POST && url.find("/abort") != std::string::npos) {
            if (controllerTarget) {
                if (controllerDeployment.active) Update.abort();
                if (controllerDeployment.hashInitialized) {
                    mbedtls_sha256_free(&controllerDeployment.imageHash);
                    controllerDeployment.hashInitialized = false;
                }
                controllerDeployment.active = false;
                controllerDeployment.terminal = true;
                controllerDeployment.success = false;
                controllerDeployment.result = "aborted";
                firmwareMaintenanceLock = false;
                controllerDeployment.receiptPersisted = appendControllerFirmwareReceipt();
                sendJSON(request, 200, controllerDeploymentJson());
                return;
            }
            dial.abortDeployment();
            firmwareMaintenanceLock = false;
            dial.setReceiptPersisted(true);
            bool persisted = appendFirmwareReceipt(dial.deployment());
            dial.setReceiptPersisted(persisted);
            if (persisted) lastRecordedDialReceipt = dial.deployment().deploymentId;
            sendJSON(request, 200, dial.deploymentJson());
            return;
        }
        request->send(405, "application/json", "{\"error\":\"unsupported deployment operation\"}");
    }

    void WebUI_Server::LatheApiBody(
        AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        if (!request->_tempObject) {
            auto* body           = new FirmwareRequestBody();
            body->expected       = total;
            body->overflow       = total > 1024;
            request->_tempObject = body;
        }
        auto* body = static_cast<FirmwareRequestBody*>(request->_tempObject);
        if (body->overflow || total != body->expected || index != body->bytes.size() || body->bytes.size() + len > 1024) {
            body->overflow = true;
            return;
        }
        body->bytes.insert(body->bytes.end(), data, data + len);
    }

    void WebUI_Server::handleLatheApiRequest(AsyncWebServerRequest* request) {
        auto* body = static_cast<FirmwareRequestBody*>(request->_tempObject);
        auto cleanup = [&]() {
            delete body;
            request->_tempObject = nullptr;
        };
        const std::string url = request->url().c_str();
        if (request->method() == HTTP_GET && url == "/api/v1/lathe/status") {
            cleanup();
            if (!Lathe::enabled()) {
                request->send(409, "application/json", "{\"error\":\"the active configuration is not a confirmed lathe\"}");
                return;
            }
            synchronousCommand(request, "[ESP425]", true, AuthenticationLevel::LEVEL_ADMIN, true);
            return;
        }
        if (request->method() != HTTP_POST) {
            cleanup();
            request->send(405, "application/json", "{\"error\":\"lathe API requires GET status or a typed POST action\"}");
            return;
        }
        const bool stopAction = url == "/api/v1/lathe/spindle-stop" || url == "/api/v1/lathe/jog-cancel";
        if (stopAction && !consoleCsrfAuthorized(request)) {
            cleanup();
            request->send(403, "application/json", "{\"error\":\"valid same-origin console session required\"}");
            return;
        }
        if (!stopAction && !consoleMutationAuthorized(request)) {
            cleanup();
            request->send(403, "application/json", "{\"error\":\"unlock this browser tab before controlling the lathe\"}");
            return;
        }
        if (!Lathe::enabled()) {
            cleanup();
            request->send(409, "application/json", "{\"error\":\"the active configuration is not a confirmed lathe\"}");
            return;
        }
        if (stopAction) {
            cleanup();
            if (url == "/api/v1/lathe/jog-cancel") {
                if (state_is(State::Jog)) protocol_send_event(&motionCancelEvent);
                sendJSON(request, 200, "{\"status\":\"jog_cancel_requested\"}");
            } else {
                synchronousCommand(request, "M5", true, AuthenticationLevel::LEVEL_ADMIN, true);
            }
            return;
        }
        if (firmwareMaintenanceLock) {
            cleanup();
            request->send(423, "application/json", "{\"error\":\"firmware maintenance lock rejects machine-control actions\"}");
            return;
        }
        if (!body || body->overflow || body->bytes.size() != body->expected) {
            cleanup();
            request->send(400, "application/json", "{\"error\":\"a bounded JSON body is required\"}");
            return;
        }

        const std::string json = bodyString(body);
        cleanup();
        char command[128] = {};

        if (url == "/api/v1/lathe/home") {
            const std::string axis = bodyJsonString(json, "axis");
            if (axis == "X") {
                strncpy(command, "$HX", sizeof(command) - 1);
            } else if (axis == "Z") {
                strncpy(command, "$HZ", sizeof(command) - 1);
            } else if (axis == "ALL" || axis == "XZ") {
                strncpy(command, "$H=XZ", sizeof(command) - 1);
            } else {
                request->send(422, "application/json", "{\"error\":\"home axis must be X, Z, or ALL\"}");
                return;
            }
        } else if (url == "/api/v1/lathe/jog") {
            std::string axis = bodyJsonString(json, "axis");
            double direction = 0, increment = 0, feed = 0;
            if (!bodyJsonNumber(json, "direction", direction) || !bodyJsonNumber(json, "increment", increment) ||
                !bodyJsonNumber(json, "feed", feed) || (axis != "X" && axis != "Z" && axis != "C") ||
                (direction != -1.0 && direction != 1.0) || increment <= 0.0 || increment > 360.0 ||
                feed < 1.0 || feed > 2000.0) {
                request->send(422, "application/json", "{\"error\":\"invalid bounded jog request\"}");
                return;
            }
            if (axis == "C" &&
                (!Lathe::encoder_enabled() || !Lathe::shared_chuck_enabled() ||
                 Lathe::shared_chuck_mode() != Lathe::SharedChuckMode::CPositioning)) {
                request->send(409,
                              "application/json",
                              "{\"error\":\"C positioning requires commissioned encoder feedback and C-positioning chuck ownership\"}");
                return;
            }
            if (axis != "C" && increment > 10.0) {
                request->send(422, "application/json", "{\"error\":\"linear jog increment exceeds 10 mm\"}");
                return;
            }
            snprintf(command,
                     sizeof(command),
                     "$J=G91 G21 %s%.4f F%.1f",
                     axis.c_str(),
                     increment * direction,
                     feed);
        } else if (url == "/api/v1/lathe/spindle") {
            std::string direction = bodyJsonString(json, "direction");
            double      rpm       = 0;
            if (!bodyJsonNumber(json, "rpm", rpm) || rpm <= 0.0 || rpm > 10000.0 ||
                (direction != "cw" && direction != "ccw")) {
                request->send(422, "application/json", "{\"error\":\"invalid spindle direction or RPM\"}");
                return;
            }
            if (Lathe::shared_chuck_enabled() && Lathe::shared_chuck_mode() != Lathe::SharedChuckMode::Spindle) {
                request->send(409, "application/json", "{\"error\":\"shared chuck ownership is not assigned to the spindle\"}");
                return;
            }
            snprintf(command, sizeof(command), "%s S%.1f", direction == "cw" ? "M3" : "M4", rpm);
        } else if (url == "/api/v1/lathe/turret") {
            double tool = 0;
            auto   status = ATCs::maijker_turret_status();
            if (!bodyJsonNumber(json, "tool", tool) || floor(tool) != tool || tool < 1.0 ||
                tool > status.station_count || !status.configured || status.target_tool != 0) {
                request->send(409, "application/json", "{\"error\":\"turret target is invalid or the turret is not idle\"}");
                return;
            }
            snprintf(command, sizeof(command), "T%d M6", static_cast<int>(tool));
        } else if (url == "/api/v1/lathe/turret/confirm") {
            double tool = 0;
            auto   status = ATCs::maijker_turret_status();
            if (!bodyJsonTrue(json, "visual_inspection") || !bodyJsonNumber(json, "tool", tool) ||
                floor(tool) != tool || tool < 1.0 || tool > status.station_count || !status.configured) {
                request->send(422, "application/json", "{\"error\":\"explicit visual station confirmation is required\"}");
                return;
            }
            snprintf(command, sizeof(command), "M61 Q%d", static_cast<int>(tool));
        } else if (url == "/api/v1/lathe/tool") {
            double tool = 0, gx = 0, gz = 0, wx = 0, wz = 0, radius = 0, orientation = 0;
            if (!bodyJsonNumber(json, "tool", tool) || !bodyJsonNumber(json, "geometry_x", gx) ||
                !bodyJsonNumber(json, "geometry_z", gz) || !bodyJsonNumber(json, "wear_x", wx) ||
                !bodyJsonNumber(json, "wear_z", wz) || !bodyJsonNumber(json, "nose_radius", radius) ||
                !bodyJsonNumber(json, "orientation", orientation) || floor(tool) != tool || tool < 1 || tool > 5 ||
                fabs(gx) > 1000 || fabs(gz) > 1000 || fabs(wx) > 100 || fabs(wz) > 100 || radius < 0 ||
                radius > 100 || floor(orientation) != orientation || orientation < 0 || orientation > 9) {
                request->send(422, "application/json", "{\"error\":\"invalid bounded tool geometry\"}");
                return;
            }
            snprintf(command,
                     sizeof(command),
                     "[ESP422]T=%d GX=%.4f GZ=%.4f WX=%.4f WZ=%.4f NR=%.4f O=%d",
                     static_cast<int>(tool),
                     gx,
                     gz,
                     wx,
                     wz,
                     radius,
                     static_cast<int>(orientation));
        } else if (url == "/api/v1/lathe/touch-off") {
            double      tool = 0, machine = 0, reference = 0;
            std::string axis = bodyJsonString(json, "axis");
            std::string mode = bodyJsonString(json, "mode");
            if (!bodyJsonNumber(json, "tool", tool) || !bodyJsonNumber(json, "machine", machine) ||
                !bodyJsonNumber(json, "reference", reference) || floor(tool) != tool || tool < 1 || tool > 5 ||
                (axis != "X" && axis != "Z") || (mode != "diameter" && mode != "radius") ||
                fabs(machine) > 10000 || fabs(reference) > 10000) {
                request->send(422, "application/json", "{\"error\":\"invalid explicit touch-off request\"}");
                return;
            }
            snprintf(command,
                     sizeof(command),
                     axis == "X" ? "[ESP423]T=%d MX=%.4f RX=%.4f MODE=%s"
                                 : "[ESP423]T=%d MZ=%.4f RZ=%.4f MODE=%s",
                     static_cast<int>(tool),
                     machine,
                     reference,
                     mode.c_str());
        } else if (url == "/api/v1/lathe/chuck") {
            std::string mode = bodyJsonString(json, "mode");
            if (!Lathe::shared_chuck_enabled() ||
                (mode != "idle" && mode != "c_positioning" && mode != "spindle")) {
                request->send(422, "application/json", "{\"error\":\"invalid or unavailable shared-chuck ownership mode\"}");
                return;
            }
            const char* value = mode == "idle" ? "IDLE" : mode == "c_positioning" ? "C_POSITIONING" : "SPINDLE";
            snprintf(command, sizeof(command), "[ESP426]MODE=%s", value);
        } else {
            request->send(404, "application/json", "{\"error\":\"unknown typed lathe action\"}");
            return;
        }

        // The typed endpoint validates the request shape; FluidNC's command
        // handlers remain authoritative for state, alarms, limits, and hardware.
        synchronousCommand(request, command, true, AuthenticationLevel::LEVEL_ADMIN, true);
    }

    void WebUI_Server::handleFileOps(AsyncWebServerRequest* request, const Volume& fs) {
        const bool mutation = request->hasParam("action") &&
                              request->getParam("action")->value() != "list";
        if (Lathe::enabled() && mutation && !consoleControlAuthorized(request)) {
            _upload_status = UploadStatus::NONE;
            request->send(403, "application/json", "{\"error\":\"unlock this browser tab before changing files\"}");
            return;
        }
        // Generic FluidNC keeps the upstream account-based file policy.
        if (!Lathe::enabled() && is_authenticated(request) == AuthenticationLevel::LEVEL_GUEST) {
            _upload_status = UploadStatus::NONE;
            sendAuthFailed(request);
            return;
        }

        std::error_code ec;

        std::string path("");
        std::string sstatus("Ok");
        if ((_upload_status == UploadStatus::FAILED) || (_upload_status == UploadStatus::FAILED)) {
            sstatus = "Upload failed";
        }
        _upload_status      = UploadStatus::NONE;
        bool     list_files = true;

        //get current path
        if (request->hasParam("path")) {
            path += request->getParam("path")->value().c_str();
        } else if (!_uploadPath.empty()) {
            // If no path parameter but we have a stored upload path, use it
            path = _uploadPath;
            _uploadPath.clear();  // Clear it after use
        }

        if (!path.empty()) {
            // path.trim();
            replace_string_in_place(path, "//", "/");
            if (path[path.length() - 1] == '/') {
                path = path.substr(0, path.length() - 1);
            }
            if (path.length() && path[0] == '/') {
                path = path.substr(1);
            }
        }

        FluidPath fpath { path, fs, ec };
        if (ec) {
            sendJSON(request, 200, "{\"status\":\"No SD card\"}");
            return;
        }

        // Handle deletions and directory creation
        if (request->hasParam("action") && request->hasParam("filename")) {
            std::string action(request->getParam("action")->value().c_str());
            std::string filename = std::string(request->getParam("filename")->value().c_str());
            if (action == "delete") {
                if (stdfs::remove(fpath / filename, ec)) {
                    sstatus = filename + " deleted";
                    HashFS::delete_file(fpath / filename);
                } else {
                    sstatus = "Cannot delete ";
                    sstatus += filename + " " + ec.message();
                }
            } else if (action == "deletedir") {
                stdfs::path dirpath { fpath / filename };
                log_debug("Deleting directory " << dirpath.string().c_str());
                size_t count = stdfs::remove_all(dirpath, ec);
                if (count > 0) {
                    sstatus = filename + " deleted";
                    HashFS::report_change();
                } else {
                    log_debug("remove_all returned " << count);
                    sstatus = "Cannot delete ";
                    sstatus += filename + " " + ec.message();
                }
            } else if (action == "createdir") {
                if (stdfs::create_directory(fpath / filename, ec)) {
                    sstatus = filename + " created";
                    HashFS::report_change();
                } else {
                    sstatus = "Cannot create ";
                    sstatus += filename + " " + ec.message();
                }
            } else if (action == "rename") {
                if (!request->hasParam("newname")) {
                    sstatus = "Missing new filename";
                } else {
                    std::string newname = std::string(request->getParam("newname")->value().c_str());
                    std::filesystem::rename(fpath / filename, fpath / newname, ec);
                    if (ec) {
                        sstatus = "Cannot rename ";
                        sstatus += filename + " " + ec.message();
                    } else {
                        sstatus = filename + " renamed to " + newname;
                        HashFS::rename_file(fpath / filename, fpath / newname);
                    }
                }
            }
        }

        //check if no need build file list
        if (request->hasParam("dontlist") && request->getParam("dontlist")->value() == "yes") {
            list_files = false;
        }

        request->send(create_file_list_response(request, fpath, path, sstatus, list_files));
    }

    void WebUI_Server::handle_direct_SDFileList(AsyncWebServerRequest* request) {
        handleFileOps(request, SD);
    }
    void WebUI_Server::handleFileList(AsyncWebServerRequest* request) {
        handleFileOps(request, LocalFS);
    }

    // File upload
    void WebUI_Server::uploadStart(AsyncWebServerRequest* request, const char* filename, size_t filesize, const Volume& fs) {
        std::error_code ec;

        FluidPath fpath { filename, fs, ec };
        if (ec) {
            _upload_status = UploadStatus::FAILED;
            log_info("Upload filesystem inaccessible");
            pushError(request, ESP_ERROR_FILE_CREATION, "Upload rejected, filesystem inaccessible");
            return;
        }

        // Store the directory path of the uploaded file for later listing
        stdfs::path filepath(filename);
        _uploadPath = filepath.parent_path().string();
        if (_uploadPath == ".") {
            _uploadPath = "";  // Root directory
        }

        auto space = stdfs::space(fpath);
        if (filesize && filesize > space.available) {
            // If the file already exists, maybe there will be enough space
            // when we replace it.
            auto existing_size = stdfs::file_size(fpath, ec);
            if (ec || (filesize > (space.available + existing_size))) {
                _upload_status = UploadStatus::FAILED;
                log_info("Upload not enough space");
                pushError(request, ESP_ERROR_NOT_ENOUGH_SPACE, "Upload rejected, not enough space");
                return;
            }
        }

        if (_upload_status != UploadStatus::FAILED) {
            //Create file for writing
            try {
                _uploadFile    = new FileStream(fpath, "w");
                _upload_status = UploadStatus::ONGOING;
            } catch (const Error err) {
                _uploadFile    = nullptr;
                _upload_status = UploadStatus::FAILED;
                log_info("Upload failed - cannot create file");
                pushError(request, ESP_ERROR_FILE_CREATION, "File creation failed");
            }
        }
    }

    void WebUI_Server::uploadWrite(AsyncWebServerRequest* request, uint8_t* buffer, size_t length) {
        delay_ms(1);
        if (_uploadFile && _upload_status == UploadStatus::ONGOING) {
            //no error write post data
            if (length != _uploadFile->write(buffer, length)) {
                _upload_status = UploadStatus::FAILED;
                log_info("Upload failed - file write failed");
                pushError(request, ESP_ERROR_FILE_WRITE, "File write failed");
            }
        } else {  //if error set flag UploadStatus::FAILED
            _upload_status = UploadStatus::FAILED;
            log_info("Upload failed - file not open");
            pushError(request, ESP_ERROR_FILE_WRITE, "File not open");
        }
    }

    void WebUI_Server::uploadEnd(AsyncWebServerRequest* request, size_t filesize) {
        //if file is open close it
        if (_uploadFile) {
            //            delete _uploadFile;
            // _uploadFile = nullptr;

            std::string pathname = _uploadFile->fpath();
            delete _uploadFile;
            _uploadFile = nullptr;
            log_debug("pathname " << pathname);

            FluidPath filepath { pathname, LocalFS };

            HashFS::rehash_file(filepath);

            // Check size
            if (filesize) {
                size_t actual_size;
                try {
                    actual_size = stdfs::file_size(filepath);
                } catch (const Error err) { actual_size = 0; }

                if (filesize != actual_size) {
                    _upload_status = UploadStatus::FAILED;
                    pushError(request, ESP_ERROR_UPLOAD, "File upload mismatch");
                    log_info("Upload failed - size mismatch - exp " << filesize << " got " << actual_size);
                }
            }
        } else {
            _upload_status = UploadStatus::FAILED;
            log_info("Upload failed - file not open");
            pushError(request, ESP_ERROR_FILE_CLOSE, "File close failed");
        }
        if (_upload_status == UploadStatus::ONGOING) {
            _upload_status = UploadStatus::SUCCESSFUL;
        } else {
            _upload_status = UploadStatus::FAILED;
            pushError(request, ESP_ERROR_UPLOAD, "Upload error 8");
        }
    }
    void WebUI_Server::uploadStop() {
        _upload_status = UploadStatus::FAILED;
        _uploadPath.clear();  // Clear stored upload path on failure
        if (_uploadFile) {
            log_info("Upload cancelled");
            std::filesystem::path filepath = _uploadFile->fpath();
            delete _uploadFile;
            _uploadFile = nullptr;
            HashFS::rehash_file(filepath);
        }
    }
    void WebUI_Server::uploadCheck(AsyncWebServerRequest* request) {
        std::error_code error_code;
        if (_upload_status == UploadStatus::FAILED) {
            cancelUpload(request);
            if (_uploadFile) {
                std::filesystem::path filepath = _uploadFile->fpath();
                delete _uploadFile;
                _uploadFile = nullptr;
                stdfs::remove(filepath, error_code);
                HashFS::rehash_file(filepath);
            }
        }
    }

    void WebUI_Server::poll() {
        static uint32_t start_time = millis();
        auto& dial = DialFirmwareClient::instance();
        if (controllerDeployment.active && controllerDeployment.lastActivity &&
            static_cast<uint32_t>(millis() - controllerDeployment.lastActivity) > 90000U) {
            Update.abort();
            if (controllerDeployment.hashInitialized) {
                mbedtls_sha256_free(&controllerDeployment.imageHash);
                controllerDeployment.hashInitialized = false;
            }
            controllerDeployment.active = false;
            controllerDeployment.terminal = true;
            controllerDeployment.success = false;
            controllerDeployment.stage = 8;
            controllerDeployment.error = "controller deployment timed out";
            controllerDeployment.result = "timeout";
            firmwareMaintenanceLock = false;
            controllerDeployment.receiptPersisted = appendControllerFirmwareReceipt();
        }
        if (!dial.deployment().terminal && dial.deployment().lastActivity &&
            static_cast<uint32_t>(millis() - dial.deployment().lastActivity) > 180000U) {
            dial.expireDeployment("M5Dial deployment timed out before verified reconnection");
            firmwareMaintenanceLock = false;
            if (lastRecordedDialReceipt != dial.deployment().deploymentId) {
                dial.setReceiptPersisted(true);
                bool persisted = appendFirmwareReceipt(dial.deployment());
                dial.setReceiptPersisted(persisted);
                if (persisted) lastRecordedDialReceipt = dial.deployment().deploymentId;
            }
        }
        if (WiFi.getMode() == WIFI_AP) {
            dnsServer.processNextRequest();
        }
        if (_schedule_reboot && static_cast<int32_t>(millis() - _schedule_reboot_time) >= 0) {
            _schedule_reboot = false;
            protocol_send_event(&fullResetEvent);
        }
        if ((millis() - start_time) > 10000) {
            uint32_t heapsize = xPortGetFreeHeapSize();
            log_verbose("memory: " << heapsize << " min: " << heapLowWater);
            if (_socket_server) {
                _socket_server->cleanupClients();
                WSChannels::sendPing();
            }
            start_time = millis();
        }
    }

    //check authentication
    AuthenticationLevel WebUI_Server::is_authenticated(AsyncWebServerRequest* request) {
#ifdef ENABLE_AUTHENTICATION
        AuthenticationIP* auth = getAuthForRequest(request);
        if (!auth) return AuthenticationLevel::LEVEL_GUEST;
        if (static_cast<uint32_t>(millis() - auth->last_time) > 360000U) {
            IPAddress remote = auth->ip;
            char sessionID[sizeof(auth->sessionID)];
            strncpy(sessionID, auth->sessionID, sizeof(sessionID) - 1);
            sessionID[sizeof(sessionID) - 1] = '\0';
            ClearAuthIP(remote, sessionID);
            return AuthenticationLevel::LEVEL_GUEST;
        }
        auth->last_time = millis();
        return auth->level;
#else
        (void)request;
        return AuthenticationLevel::LEVEL_ADMIN;
#endif
    }

#ifdef ENABLE_AUTHENTICATION
    AuthenticationIP* WebUI_Server::getAuthForRequest(AsyncWebServerRequest* request) {
        if (!request || !request->hasHeader("Cookie")) {
            return nullptr;
        }
        std::string cookie(request->getHeader("Cookie")->value().c_str());
        size_t      pos = cookie.find("ESPSESSIONID=");
        if (pos == std::string::npos) {
            return nullptr;
        }
        pos += strlen("ESPSESSIONID=");
        size_t end = cookie.find(';', pos);
        std::string sessionID = cookie.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        IPAddress remote = request->client()->remoteIP();
        return GetAuth(remote, sessionID.c_str());
    }
#endif

    bool WebUI_Server::consoleMutationAuthorized(AsyncWebServerRequest* request) {
        return Lathe::enabled() && consoleControlAuthorized(request);
    }

#ifdef ENABLE_AUTHENTICATION

    //add the information in the linked list if possible
    bool WebUI_Server::AddAuthIP(AuthenticationIP* item) {
        if (_nb_ip >= MAX_AUTH_IP) {
            return false;
        }
        item->_next = _head;
        _head       = item;
        _nb_ip++;
        return true;
    }

    namespace {
        const char* random_hex_token(char (&token)[33]) {
            uint8_t bytes[16];
            esp_fill_random(bytes, sizeof(bytes));
            for (size_t i = 0; i < sizeof(bytes); ++i) {
                snprintf(token + (i * 2), 3, "%02x", bytes[i]);
            }
            token[32] = '\0';
            return token;
        }
    }

    const char* WebUI_Server::create_session_ID() {
        static char sessionID[33];
        return random_hex_token(sessionID);
    }

    const char* WebUI_Server::create_csrf_token() {
        static char csrfToken[33];
        return random_hex_token(csrfToken);
    }

    bool WebUI_Server::ClearAuthIP(IPAddress ip, const char* sessionID) {
        AuthenticationIP* current  = _head;
        AuthenticationIP* previous = NULL;
        bool              done     = false;
        while (current) {
            if ((ip == current->ip) && (strcmp(sessionID, current->sessionID) == 0)) {
                //remove
                done = true;
                if (current == _head) {
                    _head = current->_next;
                    _nb_ip--;
                    delete current;
                    current = _head;
                } else {
                    previous->_next = current->_next;
                    _nb_ip--;
                    delete current;
                    current = previous->_next;
                }
            } else {
                previous = current;
                current  = current->_next;
            }
        }
        return done;
    }

    //Get info
    AuthenticationIP* WebUI_Server::GetAuth(IPAddress ip, const char* sessionID) {
        AuthenticationIP* current = _head;
        //AuthenticationIP * previous = NULL;
        while (current) {
            if (ip == current->ip) {
                if (strcmp(sessionID, current->sessionID) == 0) {
                    //found
                    return current;
                }
            }
            //previous = current;
            current = current->_next;
        }
        return NULL;
    }

    //Review all IP to reset timers
    AuthenticationLevel WebUI_Server::ResetAuthIP(IPAddress ip, const char* sessionID) {
        AuthenticationIP* current  = _head;
        AuthenticationIP* previous = NULL;
        while (current) {
            if ((millis() - current->last_time) > 360000) {
                //remove
                if (current == _head) {
                    _head = current->_next;
                    _nb_ip--;
                    delete current;
                    current = _head;
                } else {
                    previous->_next = current->_next;
                    _nb_ip--;
                    delete current;
                    current = previous->_next;
                }
            } else {
                if (ip == current->ip && strcmp(sessionID, current->sessionID) == 0) {
                    //reset time
                    current->last_time = millis();
                    return (AuthenticationLevel)current->level;
                }
                previous = current;
                current  = current->_next;
            }
        }
        return AuthenticationLevel::LEVEL_GUEST;
    }
#endif

    ModuleFactory::InstanceBuilder<WebUI_Server> __attribute__((init_priority(108))) webui_server_module("webuiserver", true);
}
