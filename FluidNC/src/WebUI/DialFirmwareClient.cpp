#include "DialFirmwareClient.h"

#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFiClient.h>
#include <esp_random.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstring>

namespace WebUI {
    namespace {
        constexpr char Namespace[] = "tamsdial";
        constexpr char PairLabel[] = "tams-fluiddial-http-pair-v1";
        constexpr uint32_t HttpTimeoutMs = 5000;

        struct HttpResponse {
            int         status = 0;
            std::string body;
            std::string responseAuth;
        };

        void secureZero(void* value, size_t length) {
            volatile uint8_t* p = static_cast<volatile uint8_t*>(value);
            while (length--) *p++ = 0;
        }
        int rng(void*, unsigned char* output, size_t length) {
            esp_fill_random(output, length);
            return 0;
        }
        std::string hex(const uint8_t* data, size_t length) {
            static const char chars[] = "0123456789abcdef";
            std::string out(length * 2, '0');
            for (size_t i = 0; i < length; ++i) {
                out[i * 2] = chars[data[i] >> 4];
                out[i * 2 + 1] = chars[data[i] & 0xf];
            }
            return out;
        }
        int nibble(char value) {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            return -1;
        }
        bool unhex(const std::string& text, uint8_t* output, size_t length) {
            if (text.size() != length * 2) return false;
            for (size_t i = 0; i < length; ++i) {
                int high = nibble(text[i * 2]);
                int low  = nibble(text[i * 2 + 1]);
                if (high < 0 || low < 0) return false;
                output[i] = static_cast<uint8_t>((high << 4) | low);
            }
            return true;
        }
        bool constantHexEquals(const std::string& supplied, const uint8_t expected[32]) {
            uint8_t actual[32];
            if (!unhex(supplied, actual, sizeof(actual))) return false;
            uint8_t difference = 0;
            for (size_t index = 0; index < sizeof(actual); ++index) {
                difference |= actual[index] ^ expected[index];
            }
            secureZero(actual, sizeof(actual));
            return difference == 0;
        }
        std::string sha256Hex(const uint8_t* data, size_t length) {
            uint8_t digest[32];
            mbedtls_sha256_ret(data, length, digest, 0);
            std::string value = hex(digest, sizeof(digest));
            secureZero(digest, sizeof(digest));
            return value;
        }
        std::string sha256Hex(const std::string& value) {
            return sha256Hex(reinterpret_cast<const uint8_t*>(value.data()), value.size());
        }
        void hmac(const uint8_t key[32], const std::string& value, uint8_t output[32]) {
            auto* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
            mbedtls_md_hmac(info,
                            key,
                            32,
                            reinterpret_cast<const uint8_t*>(value.data()),
                            value.size(),
                            output);
        }
        bool makeKeypair(uint8_t privateKey[32], uint8_t publicKey[65]) {
            mbedtls_ecp_group group;
            mbedtls_mpi d;
            mbedtls_ecp_point q;
            size_t written = 0;
            mbedtls_ecp_group_init(&group);
            mbedtls_mpi_init(&d);
            mbedtls_ecp_point_init(&q);
            bool ok = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
                      mbedtls_ecp_gen_keypair(&group, &d, &q, rng, nullptr) == 0 &&
                      mbedtls_mpi_write_binary(&d, privateKey, 32) == 0 &&
                      mbedtls_ecp_point_write_binary(&group, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &written, publicKey, 65) == 0 &&
                      written == 65;
            mbedtls_ecp_point_free(&q);
            mbedtls_mpi_free(&d);
            mbedtls_ecp_group_free(&group);
            return ok;
        }
        bool sharedSecret(const uint8_t privateKey[32], const uint8_t peerPublic[65], uint8_t output[32]) {
            mbedtls_ecp_group group;
            mbedtls_mpi d, z;
            mbedtls_ecp_point q;
            mbedtls_ecp_group_init(&group);
            mbedtls_mpi_init(&d);
            mbedtls_mpi_init(&z);
            mbedtls_ecp_point_init(&q);
            bool ok = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
                      mbedtls_mpi_read_binary(&d, privateKey, 32) == 0 &&
                      mbedtls_ecp_point_read_binary(&group, &q, peerPublic, 65) == 0 &&
                      mbedtls_ecp_check_pubkey(&group, &q) == 0 &&
                      mbedtls_ecdh_compute_shared(&group, &z, &q, &d, rng, nullptr) == 0 &&
                      mbedtls_mpi_write_binary(&z, output, 32) == 0;
            mbedtls_ecp_point_free(&q);
            mbedtls_mpi_free(&z);
            mbedtls_mpi_free(&d);
            mbedtls_ecp_group_free(&group);
            return ok;
        }
        std::string jsonString(const std::string& json, const char* key) {
            std::string needle = "\"" + std::string(key) + "\":\"";
            size_t start = json.find(needle);
            if (start == std::string::npos) return {};
            start += needle.size();
            size_t end = json.find('"', start);
            return end == std::string::npos ? std::string() : json.substr(start, end - start);
        }
        uint32_t jsonUint(const std::string& json, const char* key) {
            std::string needle = "\"" + std::string(key) + "\":";
            size_t start = json.find(needle);
            if (start == std::string::npos) return 0;
            return strtoul(json.c_str() + start + needle.size(), nullptr, 10);
        }
        bool jsonBool(const std::string& json, const char* key) {
            std::string needle = "\"" + std::string(key) + "\":true";
            return json.find(needle) != std::string::npos;
        }
        std::string jsonEscape(const std::string& input) {
            std::string output;
            for (unsigned char c : input) {
                if (c == '"' || c == '\\') output += '\\';
                if (c >= 0x20) output += static_cast<char>(c);
            }
            return output;
        }
        HttpResponse httpRequest(const std::string& ip,
                                 const char* method,
                                 const std::string& path,
                                 const std::string& contentType,
                                 const std::string& body,
                                 const std::string& headers = {}) {
            HttpResponse result;
            WiFiClient client;
            client.setTimeout(HttpTimeoutMs / 1000);
            if (!client.connect(ip.c_str(), 80)) return result;
            std::string request = std::string(method) + " " + path + " HTTP/1.1\r\nHost: " + ip +
                                  "\r\nConnection: close\r\nUser-Agent: FluidNC-Maijker-OTA\r\n";
            if (!contentType.empty()) request += "Content-Type: " + contentType + "\r\n";
            request += headers;
            request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
            client.write(reinterpret_cast<const uint8_t*>(request.data()), request.size());
            if (!body.empty()) client.write(reinterpret_cast<const uint8_t*>(body.data()), body.size());
            uint32_t deadline = millis() + HttpTimeoutMs;
            while (!client.available() && client.connected() && static_cast<int32_t>(deadline - millis()) > 0) delay(1);
            std::string statusLine = client.readStringUntil('\n').c_str();
            size_t first = statusLine.find(' ');
            if (first != std::string::npos) result.status = atoi(statusLine.c_str() + first + 1);
            int contentLength = -1;
            while (client.connected()) {
                std::string line = client.readStringUntil('\n').c_str();
                if (line == "\r" || line.empty()) break;
                if (line.rfind("Content-Length:", 0) == 0) contentLength = atoi(line.c_str() + 15);
                if (line.rfind("X-TAMS-Response-Auth:", 0) == 0) {
                    size_t begin = line.find_first_not_of(" \t", 21);
                    size_t end = line.find_last_not_of("\r\n \t");
                    if (begin != std::string::npos && end != std::string::npos && end >= begin) {
                        result.responseAuth = line.substr(begin, end - begin + 1);
                    }
                }
            }
            deadline = millis() + HttpTimeoutMs;
            while ((client.connected() || client.available()) && static_cast<int32_t>(deadline - millis()) > 0) {
                while (client.available()) {
                    result.body += static_cast<char>(client.read());
                    if (contentLength >= 0 && result.body.size() >= static_cast<size_t>(contentLength)) break;
                }
                if (contentLength >= 0 && result.body.size() >= static_cast<size_t>(contentLength)) break;
                delay(1);
            }
            client.stop();
            return result;
        }
    }

    DialFirmwareClient& DialFirmwareClient::instance() {
        static DialFirmwareClient client;
        return client;
    }

    void DialFirmwareClient::init(const std::string& controllerDeviceId) {
        if (_initialized) return;
        _controllerDeviceId = controllerDeviceId;
        _initialized = loadIdentity();
    }

    bool DialFirmwareClient::loadIdentity() {
        Preferences preferences;
        preferences.begin(Namespace, false);
        bool haveIdentity = preferences.getBytesLength("id_priv") == sizeof(_controllerPrivate) &&
                            preferences.getBytesLength("id_pub") == sizeof(_controllerPublic);
        if (haveIdentity) {
            preferences.getBytes("id_priv", _controllerPrivate, sizeof(_controllerPrivate));
            preferences.getBytes("id_pub", _controllerPublic, sizeof(_controllerPublic));
        } else if (makeKeypair(_controllerPrivate, _controllerPublic)) {
            preferences.putBytes("id_priv", _controllerPrivate, sizeof(_controllerPrivate));
            preferences.putBytes("id_pub", _controllerPublic, sizeof(_controllerPublic));
        }
        _controllerFingerprint = sha256Hex(_controllerPublic, sizeof(_controllerPublic));
        _state.paired = preferences.getBool("paired", false) &&
                        preferences.getBytesLength("secret") == sizeof(_pairSecret);
        if (_state.paired) {
            preferences.getBytes("secret", _pairSecret, sizeof(_pairSecret));
            _state.deviceId = preferences.getString("dial_id", "").c_str();
            _state.fingerprint = preferences.getString("dial_fp", "").c_str();
            _state.ip = preferences.getString("dial_ip", "").c_str();
            _state.version = preferences.getString("dial_ver", "").c_str();
        }
        preferences.end();
        return _controllerPublic[0] == 0x04;
    }

    bool DialFirmwareClient::persistPair() {
        if (_pendingDeviceId.empty() || _pendingFingerprint.size() != 64 || _pendingIp.empty()) return false;
        Preferences preferences;
        preferences.begin(Namespace, false);
        preferences.putBytes("secret", _pendingSecret, sizeof(_pendingSecret));
        preferences.putString("dial_id", _pendingDeviceId.c_str());
        preferences.putString("dial_fp", _pendingFingerprint.c_str());
        preferences.putString("dial_ip", _pendingIp.c_str());
        preferences.putBool("paired", true);
        preferences.end();
        memcpy(_pairSecret, _pendingSecret, sizeof(_pairSecret));
        _state.paired = true;
        _state.deviceId = _pendingDeviceId;
        _state.fingerprint = _pendingFingerprint;
        _state.ip = _pendingIp;
        cancelPairing();
        return true;
    }

    bool DialFirmwareClient::discover(bool force) {
        if (!force && _lastDiscoveryAt &&
            static_cast<uint32_t>(millis() - _lastDiscoveryAt) < 5000U) {
            return _lastDiscoveryOk;
        }
        _lastDiscoveryAt = millis();
        _state.ambiguous = false;
        int found = MDNS.queryService("tams-fluiddial", "tcp");
        if (found != 1) {
            _state.online = false;
            _state.ambiguous = found > 1;
            _state.lastError = found > 1 ? "multiple M5Dial services discovered" : "paired M5Dial not discovered";
            _lastDiscoveryOk = false;
            return false;
        }
        std::string ip = MDNS.IP(0).toString().c_str();
        HttpResponse response = httpRequest(ip, "GET", "/api/v1/device", "", "");
        if (response.status != 200 || jsonString(response.body, "hardware_role") != "m5dial_hmi") {
            _state.online = false;
            _state.lastError = "discovered service did not identify as an M5Dial";
            _lastDiscoveryOk = false;
            return false;
        }
        if (!_deployment.active && !_deployment.terminal && _deployment.stage >= 4) {
            _deployment.stage = 5;
        }
        if (_state.paired) {
            // mDNS only proposes an address. Treat the device as online only
            // after the pair secret authenticates its full stable identity.
            std::string oldIp = _state.ip;
            _state.ip = ip;
            if (!refreshHealth()) {
                _state.ip = oldIp;
                _state.online = false;
                _state.lastError = "M5Dial address changed without identity proof";
                _lastDiscoveryOk = false;
                return false;
            }
            if (ip != oldIp) {
                Preferences preferences;
                preferences.begin(Namespace, false);
                preferences.putString("dial_ip", ip.c_str());
                preferences.end();
            }
        } else if (!_state.paired) {
            _state.ip = ip;
            _state.online = true;
        }
        _state.lastError.clear();
        _lastDiscoveryOk = true;
        return true;
    }

    bool DialFirmwareClient::startPairing(std::string& comparisonCode) {
        comparisonCode.clear();
        if (!_initialized || !discover(true) || _state.ambiguous) return false;
        uint8_t controllerPublic[65];
        if (!makeKeypair(_pendingPrivate, controllerPublic)) return false;
        std::string body = "controller_id=" + _controllerDeviceId + "&controller_fingerprint=" +
                           _controllerFingerprint + "&controller_pub=" + hex(controllerPublic, sizeof(controllerPublic));
        HttpResponse response = httpRequest(_state.ip,
                                            "POST",
                                            "/api/v1/pair/start",
                                            "application/x-www-form-urlencoded",
                                            body);
        if (response.status != 200) {
            _state.lastError = response.body;
            return false;
        }
        std::string devicePublicHex = jsonString(response.body, "device_pub");
        uint8_t devicePublic[65], rawShared[32];
        _pendingDeviceId = jsonString(response.body, "device_id");
        _pendingFingerprint = jsonString(response.body, "identity_fingerprint");
        _pendingIp = _state.ip;
        _pendingCode = jsonString(response.body, "comparison_code");
        if (!unhex(devicePublicHex, devicePublic, sizeof(devicePublic)) ||
            !sharedSecret(_pendingPrivate, devicePublic, rawShared)) {
            cancelPairing();
            return false;
        }
        std::string transcript = std::string(PairLabel) + "\n" + _controllerDeviceId + "\n" +
                                 _controllerFingerprint + "\n" + _pendingDeviceId + "\n" +
                                 _pendingFingerprint + "\n" + hex(controllerPublic, sizeof(controllerPublic)) +
                                 "\n" + devicePublicHex;
        uint8_t pairDigest[32], codeDigest[32];
        hmac(rawShared, transcript, pairDigest);
        memcpy(_pendingSecret, pairDigest, sizeof(_pendingSecret));
        hmac(_pendingSecret, "comparison", codeDigest);
        uint32_t code = (uint32_t(codeDigest[0]) << 16 | uint32_t(codeDigest[1]) << 8 | codeDigest[2]) % 1000000U;
        char localCode[7];
        snprintf(localCode, sizeof(localCode), "%06lu", static_cast<unsigned long>(code));
        secureZero(rawShared, sizeof(rawShared));
        secureZero(pairDigest, sizeof(pairDigest));
        secureZero(codeDigest, sizeof(codeDigest));
        secureZero(devicePublic, sizeof(devicePublic));
        if (_pendingCode != localCode || _pendingFingerprint.size() != 64) {
            _state.lastError = "pairing comparison transcript did not match";
            cancelPairing();
            return false;
        }
        comparisonCode = _pendingCode;
        return true;
    }

    bool DialFirmwareClient::confirmPairing(const std::string& comparisonCode) {
        if (_pendingCode.empty() || comparisonCode != _pendingCode) return false;
        std::string body = "comparison_code=" + comparisonCode;
        HttpResponse response = httpRequest(_pendingIp,
                                            "POST",
                                            "/api/v1/pair/confirm",
                                            "application/x-www-form-urlencoded",
                                            body);
        if (response.status != 202) {
            _state.lastError = response.body;
            return false;
        }
        return true;
    }

    bool DialFirmwareClient::pollPairing() {
        if (_pendingIp.empty()) return false;
        HttpResponse response = httpRequest(_pendingIp, "GET", "/api/v1/pair/status", "", "");
        if (response.status != 200) return false;
        if (jsonBool(response.body, "paired") && jsonString(response.body, "device_id") == _pendingDeviceId) {
            return persistPair();
        }
        return false;
    }

    void DialFirmwareClient::cancelPairing() {
        secureZero(_pendingPrivate, sizeof(_pendingPrivate));
        secureZero(_pendingSecret, sizeof(_pendingSecret));
        _pendingDeviceId.clear();
        _pendingFingerprint.clear();
        _pendingIp.clear();
        _pendingCode.clear();
    }

    bool DialFirmwareClient::requestChallenge(uint32_t requestedCounter, std::string& nonce, uint32_t& counter) {
        if (!_state.paired || _state.ip.empty()) return false;
        uint8_t random[16];
        esp_fill_random(random, sizeof(random));
        std::string clientNonce = hex(random, sizeof(random));
        std::string canonical = "challenge\n" + _controllerDeviceId + "\n" + _state.deviceId + "\n" + clientNonce;
        uint8_t proof[32];
        hmac(_pairSecret, canonical, proof);
        std::string headers = "X-TAMS-Client-Nonce: " + clientNonce + "\r\nX-TAMS-Challenge-Auth: " +
                              hex(proof, sizeof(proof)) + "\r\n";
        secureZero(random, sizeof(random));
        secureZero(proof, sizeof(proof));
        std::string body = "controller_id=" + _controllerDeviceId;
        HttpResponse response = httpRequest(_state.ip,
                                            "POST",
                                            "/api/v1/challenge",
                                            "application/x-www-form-urlencoded",
                                            body,
                                            headers);
        if (response.status != 200) {
            _state.lastError = response.body;
            return false;
        }
        nonce = jsonString(response.body, "nonce");
        uint32_t next = jsonUint(response.body, "next_deployment_counter");
        counter = requestedCounter ? requestedCounter : next;
        return nonce.size() == 32 && counter >= next;
    }

    bool DialFirmwareClient::authenticatedRequest(const char* method,
                                                  const char* requestPath,
                                                  const char* canonicalPath,
                                                  const std::string& contentType,
                                                  const std::string& body,
                                                  const std::string& bodyDigest,
                                                  uint32_t counter,
                                                  std::string& responseBody,
                                                  uint32_t* usedCounter) {
        std::string nonce;
        uint32_t authorizedCounter = 0;
        if (!requestChallenge(counter, nonce, authorizedCounter)) return false;
        if (usedCounter) *usedCounter = authorizedCounter;
        std::string canonical = std::string(method) + "\n" + canonicalPath + "\n" + _state.deviceId + "\n" + nonce +
                                "\n" + std::to_string(authorizedCounter) + "\n" + _deployment.manifestDigest + "\n" + bodyDigest;
        uint8_t authentication[32];
        hmac(_pairSecret, canonical, authentication);
        std::string headers = "X-TAMS-Target: " + _state.deviceId + "\r\nX-TAMS-Nonce: " + nonce +
                              "\r\nX-TAMS-Counter: " + std::to_string(authorizedCounter) +
                              "\r\nX-TAMS-Manifest: " + _deployment.manifestDigest +
                              "\r\nX-TAMS-Body-SHA256: " + bodyDigest + "\r\nX-TAMS-Auth: " +
                              hex(authentication, sizeof(authentication)) + "\r\n";
        secureZero(authentication, sizeof(authentication));
        HttpResponse response = httpRequest(_state.ip, method, requestPath, contentType, body, headers);
        responseBody = response.body;
        std::string responseCanonical = "response\n" + nonce + "\n" +
                                        std::to_string(authorizedCounter) + "\n" +
                                        std::to_string(response.status) + "\n" +
                                        sha256Hex(response.body);
        uint8_t expectedResponseAuth[32];
        hmac(_pairSecret, responseCanonical, expectedResponseAuth);
        bool responseAuthenticated = constantHexEquals(response.responseAuth, expectedResponseAuth);
        secureZero(expectedResponseAuth, sizeof(expectedResponseAuth));
        if (!responseAuthenticated) {
            _state.lastError = "M5Dial response authentication failed";
            return false;
        }
        if (response.status < 200 || response.status >= 300) {
            _state.lastError = response.body;
            return false;
        }
        return true;
    }

    bool DialFirmwareClient::exactTargetOnline() {
        if (!discover(true) || !_state.paired || _state.ambiguous) return false;
        return _state.online;
    }

    bool DialFirmwareClient::beginDeployment(const TamsFirmware::ValidationResult& package, const std::string& deploymentId) {
        if (_deployment.active || !package.valid() || !exactTargetOnline()) return false;
        _deployment = {};
        _deployment.active = true;
        _deployment.stage = 1;
        _deployment.deploymentId = deploymentId;
        _deployment.manifestDigest = package.manifestSha256;
        _deployment.imageSha256 = package.manifest.imageSha256;
        _deployment.expectedBytes = package.manifest.imageLength;
        _deployment.fromVersion = _state.version;
        _deployment.toVersion = package.manifest.version;
        _deployment.fromReleaseCounter = _state.releaseCounter;
        _deployment.toReleaseCounter = package.manifest.releaseCounter;
        _deployment.packageId = package.manifest.packageId;
        _deployment.signingKeyId = package.manifest.signingKeyId;
        _deployment.startedAt = millis();
        _deployment.lastActivity = _deployment.startedAt;
        std::string manifestHex = hex(reinterpret_cast<const uint8_t*>(package.manifestRaw.data()), package.manifestRaw.size());
        std::string signatureHex = hex(reinterpret_cast<const uint8_t*>(package.signatureRaw.data()), package.signatureRaw.size());
        std::string body = "deployment_id=" + deploymentId + "&manifest=" + manifestHex + "&signature=" + signatureHex;
        std::string semanticBody = deploymentId + "\n" + manifestHex + "\n" + signatureHex;
        std::string response;
        if (!authenticatedRequest("POST",
                                  "/api/v1/ota/begin",
                                  "/api/v1/ota/begin",
                                  "application/x-www-form-urlencoded",
                                  body,
                                  sha256Hex(semanticBody),
                                  0,
                                  response,
                                  &_deployment.counter)) {
            _deployment.active = false;
            _deployment.error = _state.lastError;
            return false;
        }
        _deployment.acceptedOffset = jsonUint(response, "accepted_offset");
        _deployment.stage = 2;
        return true;
    }

    bool DialFirmwareClient::relayChunk(uint32_t offset, const uint8_t* data, size_t length) {
        if (!_deployment.active || !data || !length || length > 8192 || offset != _deployment.acceptedOffset) return false;
        std::string body(reinterpret_cast<const char*>(data), length);
        std::string path = "/api/v1/ota/chunk?deployment_id=" + _deployment.deploymentId + "&offset=" + std::to_string(offset);
        std::string response;
        auto sendChunk = [&]() {
            return authenticatedRequest("PUT",
                                        path.c_str(),
                                        "/api/v1/ota/chunk",
                                        "application/octet-stream",
                                        body,
                                        sha256Hex(reinterpret_cast<const uint8_t*>(body.data()), body.size()),
                                        _deployment.counter,
                                        response);
        };
        if (!sendChunk()) {
            // A dropped HTTP response is ambiguous: the target may already
            // have durably accepted this exact sequential chunk. Reconcile
            // its authenticated offset before deciding whether to retry.
            std::string status;
            bool statusOk = authenticatedRequest("GET",
                                                 "/api/v1/ota/status",
                                                 "/api/v1/ota/status",
                                                 "",
                                                 "",
                                                 sha256Hex(std::string()),
                                                 _deployment.counter,
                                                 status);
            uint32_t received = statusOk ? jsonUint(status, "received") : UINT32_MAX;
            if (received == offset + length) {
                _deployment.acceptedOffset = received;
                _deployment.lastActivity = millis();
                return true;
            }
            if (received != offset || !sendChunk()) {
                _deployment.error = _state.lastError;
                return false;
            }
        }
        _deployment.acceptedOffset = jsonUint(response, "accepted_offset");
        _deployment.lastActivity = millis();
        return _deployment.acceptedOffset == offset + length;
    }

    bool DialFirmwareClient::commitDeployment() {
        if (!_deployment.active || _deployment.acceptedOffset != _deployment.expectedBytes) return false;
        std::string response;
        if (!authenticatedRequest("POST",
                                  "/api/v1/ota/commit",
                                  "/api/v1/ota/commit",
                                  "text/plain",
                                  "",
                                  sha256Hex(std::string()),
                                  _deployment.counter,
                                  response)) {
            _deployment.error = _state.lastError;
            return false;
        }
        _deployment.stage = 4;
        _deployment.lastActivity = millis();
        _deployment.active = false;
        _deployment.result = "waiting for reboot and authenticated health";
        return true;
    }

    void DialFirmwareClient::expireDeployment(const char* reason) {
        _deployment.active = false;
        _deployment.terminal = true;
        _deployment.success = false;
        _deployment.stage = 8;
        _deployment.error = reason ? reason : "deployment timed out";
        _deployment.result = "timeout";
    }

    bool DialFirmwareClient::abortDeployment() {
        if (!_deployment.active) return false;
        std::string response;
        bool ok = authenticatedRequest("POST",
                                       "/api/v1/ota/abort",
                                       "/api/v1/ota/abort",
                                       "text/plain",
                                       "",
                                       sha256Hex(std::string()),
                                       _deployment.counter,
                                       response);
        _deployment.active = false;
        _deployment.terminal = true;
        _deployment.success = false;
        _deployment.result = "aborted";
        return ok;
    }

    bool DialFirmwareClient::refreshHealth() {
        if (!_state.paired || _state.ip.empty()) return false;
        if (_deployment.manifestDigest.empty()) _deployment.manifestDigest = std::string(64, '0');
        std::string response;
        if (!authenticatedRequest("GET",
                                  "/api/v1/health",
                                  "/api/v1/health",
                                  "",
                                  "",
                                  sha256Hex(std::string()),
                                  0,
                                  response)) {
            _state.online = false;
            return false;
        }
        std::string id = jsonString(response, "device_id");
        std::string fingerprint = jsonString(response, "identity_fingerprint");
        if (id != _state.deviceId || fingerprint != _state.fingerprint ||
            jsonString(response, "hardware_role") != "m5dial_hmi") {
            _state.online = false;
            _state.lastError = "authenticated M5Dial identity mismatch";
            return false;
        }
        _state.version = jsonString(response, "version");
        _state.releaseCounter = jsonUint(response, "release_counter");
        _state.health = jsonBool(response, "healthy") ? "healthy" : "unhealthy";
        _state.fluidNcLinkState = jsonString(response, "fluidnc_link_state");
        _state.online = true;
        std::string lastDeploymentId = jsonString(response, "last_deployment_id");
        std::string lastResult       = jsonString(response, "last_result");
        if (!_deployment.active && !_deployment.terminal && _deployment.stage >= 4) {
            _deployment.stage = 6;
        }
        if (!_deployment.toVersion.empty() && _state.version == _deployment.toVersion &&
            _state.health == "healthy" && lastDeploymentId == _deployment.deploymentId &&
            lastResult == "success") {
            _deployment.stage = 8;
            _deployment.terminal = true;
            _deployment.success = true;
            _deployment.result = "success";
        } else if (!_deployment.deploymentId.empty() && lastDeploymentId == _deployment.deploymentId &&
                   lastResult == "rollback") {
            _deployment.stage = 8;
            _deployment.terminal = true;
            _deployment.success = false;
            _deployment.result = "rollback";
            _deployment.error = "M5Dial rolled back to the previous application";
        }
        Preferences preferences;
        preferences.begin(Namespace, false);
        preferences.putString("dial_ver", _state.version.c_str());
        preferences.end();
        return true;
    }

    std::string DialFirmwareClient::stateJson() const {
        return "{\"paired\":" + std::string(_state.paired ? "true" : "false") +
               ",\"online\":" + (_state.online ? "true" : "false") +
               ",\"ambiguous\":" + (_state.ambiguous ? "true" : "false") +
               ",\"device_id\":\"" + jsonEscape(_state.deviceId) + "\",\"fingerprint\":\"" +
               jsonEscape(_state.fingerprint) + "\",\"ip\":\"" + jsonEscape(_state.ip) +
               "\",\"hardware_role\":\"m5dial_hmi\",\"version\":\"" + jsonEscape(_state.version) +
               "\",\"release_counter\":" + std::to_string(_state.releaseCounter) +
               ",\"health\":\"" + jsonEscape(_state.health) + "\",\"fluidnc_link_state\":\"" +
               jsonEscape(_state.fluidNcLinkState) + "\",\"error\":\"" +
               jsonEscape(_state.lastError) + "\"}";
    }

    std::string DialFirmwareClient::deploymentJson() const {
        return "{\"deployment_id\":\"" + jsonEscape(_deployment.deploymentId) + "\",\"active\":" +
               (_deployment.active ? "true" : "false") + ",\"terminal\":" +
               (_deployment.terminal ? "true" : "false") + ",\"success\":" +
               (_deployment.success ? "true" : "false") + ",\"stage_index\":" +
               std::to_string(_deployment.stage) + ",\"accepted_offset\":" +
               std::to_string(_deployment.acceptedOffset) + ",\"expected_bytes\":" +
               std::to_string(_deployment.expectedBytes) + ",\"error\":\"" +
               jsonEscape(_deployment.error) + "\",\"package_id\":\"" + jsonEscape(_deployment.packageId) +
               "\",\"from_version\":\"" + jsonEscape(_deployment.fromVersion) + "\",\"to_version\":\"" +
               jsonEscape(_deployment.toVersion) + "\",\"from_release_counter\":" +
               std::to_string(_deployment.fromReleaseCounter) + ",\"to_release_counter\":" +
               std::to_string(_deployment.toReleaseCounter) + ",\"result\":\"" + jsonEscape(_deployment.result) +
               "\",\"receipt_persisted\":" + (_deployment.receiptPersisted ? "true" : "false") + "}";
    }
}
