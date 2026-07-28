#include "TamsFirmwarePackage.h"
#include "TamsFirmwareTrust.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

#include <mbedtls/pk.h>

namespace TamsFirmware {
    namespace {
        constexpr uint8_t Magic[8] = { 'T', 'A', 'M', 'S', 'F', 'W', '1', 0 };
        constexpr uint32_t AllFields = 0xFFFFU;

        uint16_t le16(const uint8_t* p) {
            return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
        }
        uint32_t le32(const uint8_t* p) {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }
        std::string hexDigest(const uint8_t* digest, size_t length) {
            static const char hex[] = "0123456789abcdef";
            std::string out(length * 2, '0');
            for (size_t index = 0; index < length; ++index) {
                out[index * 2]     = hex[digest[index] >> 4];
                out[index * 2 + 1] = hex[digest[index] & 0x0f];
            }
            return out;
        }
        bool isIdentifier(const std::string& value) {
            if (value.empty() || value.size() > 64 || !std::islower(static_cast<unsigned char>(value[0])) &&
                    !std::isdigit(static_cast<unsigned char>(value[0]))) {
                return false;
            }
            return std::all_of(value.begin(), value.end(), [](unsigned char c) {
                return std::islower(c) || std::isdigit(c) || c == '.' || c == '_' || c == '-';
            });
        }
        bool isHex(const std::string& value, size_t length) {
            return value.size() == length && std::all_of(value.begin(), value.end(), [](unsigned char c) {
                return std::isdigit(c) || (c >= 'a' && c <= 'f');
            });
        }
        bool isUuid(const std::string& value) {
            if (value.size() != 36) return false;
            for (size_t i = 0; i < value.size(); ++i) {
                if (i == 8 || i == 13 || i == 18 || i == 23) {
                    if (value[i] != '-') return false;
                } else if (!(std::isdigit(static_cast<unsigned char>(value[i])) || (value[i] >= 'a' && value[i] <= 'f'))) {
                    return false;
                }
            }
            return true;
        }
        bool isSemver(const std::string& value) {
            unsigned dots = 0;
            if (value.empty() || !std::isdigit(static_cast<unsigned char>(value[0]))) return false;
            for (unsigned char c : value) {
                if (c == '.') ++dots;
                else if (!(std::isalnum(c) || c == '-' || c == '+')) return false;
            }
            return dots == 2;
        }

        class Parser {
        public:
            Parser(const uint8_t* data, size_t length) : _begin(reinterpret_cast<const char*>(data)), _cursor(_begin), _end(_begin + length) {}

            bool parse(Manifest& manifest, std::string& error) {
                skip();
                if (!consume('{')) return setError(error, "manifest must be an object");
                uint32_t fields = 0;
                skip();
                while (_cursor < _end && *_cursor != '}') {
                    std::string key;
                    if (!parseString(key) || !consume(':')) return setError(error, "invalid manifest key");
                    int field = fieldIndex(key);
                    if (field < 0) return setError(error, "unknown manifest key");
                    uint32_t mask = 1U << field;
                    if (fields & mask) return setError(error, "duplicate manifest key");
                    fields |= mask;
                    if (!parseField(field, manifest)) return setError(error, "invalid manifest value");
                    skip();
                    if (_cursor < _end && *_cursor == ',') {
                        ++_cursor;
                        skip();
                    } else {
                        break;
                    }
                }
                if (!consume('}') || _cursor != _end || fields != AllFields) return setError(error, "manifest fields are incomplete");
                if (manifest.manifestVersion != 1 || !isUuid(manifest.packageId) || !isSemver(manifest.version) ||
                    !isHex(manifest.imageSha256, 64) || !isIdentifier(manifest.product) || !isIdentifier(manifest.board) ||
                    !isIdentifier(manifest.hardwareRole) || !isIdentifier(manifest.espChip) ||
                    !isIdentifier(manifest.partitionScheme) || !isIdentifier(manifest.signingKeyId) ||
                    !manifest.releaseCounter || !manifest.protocolMin || manifest.protocolMin > manifest.protocolMax ||
                    !manifest.imageLength || manifest.imageLength > MaxImageBytes ||
                    (manifest.allowDowngrade && !manifest.recovery)) {
                    return setError(error, "manifest policy validation failed");
                }
                return true;
            }

        private:
            void skip() {
                while (_cursor < _end && (*_cursor == ' ' || *_cursor == '\t' || *_cursor == '\r' || *_cursor == '\n')) ++_cursor;
            }
            bool consume(char expected) {
                skip();
                if (_cursor >= _end || *_cursor != expected) return false;
                ++_cursor;
                skip();
                return true;
            }
            bool parseString(std::string& value) {
                skip();
                if (_cursor >= _end || *_cursor++ != '"') return false;
                const char* start = _cursor;
                while (_cursor < _end && *_cursor != '"') {
                    unsigned char c = static_cast<unsigned char>(*_cursor);
                    if (c < 0x20 || c == '\\') return false;
                    ++_cursor;
                }
                if (_cursor >= _end) return false;
                value.assign(start, _cursor++);
                skip();
                return true;
            }
            bool parseUint(uint32_t& value) {
                skip();
                if (_cursor >= _end || !std::isdigit(static_cast<unsigned char>(*_cursor))) return false;
                uint64_t parsed = 0;
                while (_cursor < _end && std::isdigit(static_cast<unsigned char>(*_cursor))) {
                    parsed = parsed * 10 + (*_cursor++ - '0');
                    if (parsed > 0xFFFFFFFFULL) return false;
                }
                value = static_cast<uint32_t>(parsed);
                skip();
                return true;
            }
            bool parseBool(bool& value) {
                skip();
                if ((_end - _cursor) >= 4 && memcmp(_cursor, "true", 4) == 0) {
                    _cursor += 4; value = true; skip(); return true;
                }
                if ((_end - _cursor) >= 5 && memcmp(_cursor, "false", 5) == 0) {
                    _cursor += 5; value = false; skip(); return true;
                }
                return false;
            }
            static int fieldIndex(const std::string& key) {
                static const char* names[] = {
                    "manifest_version", "package_id", "product", "board", "hardware_role", "esp_chip",
                    "partition_scheme", "version", "release_counter", "protocol_min", "protocol_max",
                    "image_length", "image_sha256", "signing_key_id", "recovery", "allow_downgrade"
                };
                for (int index = 0; index < 16; ++index) if (key == names[index]) return index;
                return -1;
            }
            bool parseField(int field, Manifest& m) {
                uint32_t value = 0;
                switch (field) {
                    case 0: if (!parseUint(value) || value > 0xffff) return false; m.manifestVersion = value; return true;
                    case 1: return parseString(m.packageId);
                    case 2: return parseString(m.product);
                    case 3: return parseString(m.board);
                    case 4: return parseString(m.hardwareRole);
                    case 5: return parseString(m.espChip);
                    case 6: return parseString(m.partitionScheme);
                    case 7: return parseString(m.version);
                    case 8: return parseUint(m.releaseCounter);
                    case 9: if (!parseUint(value) || value > 0xffff) return false; m.protocolMin = value; return true;
                    case 10: if (!parseUint(value) || value > 0xffff) return false; m.protocolMax = value; return true;
                    case 11: return parseUint(m.imageLength);
                    case 12: return parseString(m.imageSha256);
                    case 13: return parseString(m.signingKeyId);
                    case 14: return parseBool(m.recovery);
                    case 15: return parseBool(m.allowDowngrade);
                }
                return false;
            }
            static bool setError(std::string& error, const char* message) { error = message; return false; }
            const char* _begin;
            const char* _cursor;
            const char* _end;
        };
    }

    bool parseManifest(const uint8_t* raw, size_t length, Manifest& manifest, std::string& error) {
        if (!raw || !length || length > MaxManifestBytes) {
            error = "manifest length is outside the supported range";
            return false;
        }
        return Parser(raw, length).parse(manifest, error);
    }

    ValidationResult validateSignedManifest(
        const uint8_t* manifestRaw, size_t manifestLength, const uint8_t* signature, size_t signatureLength) {
        ValidationResult result;
        if (!manifestRaw || !signature || !signatureLength || signatureLength > MaxSignatureBytes) {
            result.error = "signed manifest input is invalid";
            return result;
        }
        if (!parseManifest(manifestRaw, manifestLength, result.manifest, result.error)) return result;
        result.manifestValid = true;
        uint8_t manifestDigest[32];
        mbedtls_sha256_ret(manifestRaw, manifestLength, manifestDigest, 0);
        result.manifestSha256 = hexDigest(manifestDigest, sizeof(manifestDigest));
        const TrustKey* key = findTrustKey(result.manifest.signingKeyId.c_str());
        if (!key) {
            result.error = trustConfigured() ? "untrusted signing key" : "production signing trust is not configured";
            return result;
        }
        if (result.manifest.recovery != key->recovery) {
            result.error = "signing key role does not match package recovery policy";
            return result;
        }
        result.keyRecovery = key->recovery;
        mbedtls_pk_context context;
        mbedtls_pk_init(&context);
        int parsed = mbedtls_pk_parse_public_key(&context,
                                                reinterpret_cast<const unsigned char*>(key->pem),
                                                strlen(key->pem) + 1);
        int verified = parsed == 0 ? mbedtls_pk_verify(&context,
                                                      MBEDTLS_MD_SHA256,
                                                      manifestDigest,
                                                      sizeof(manifestDigest),
                                                      signature,
                                                      signatureLength)
                                   : parsed;
        mbedtls_pk_free(&context);
        result.signatureValid = verified == 0;
        if (!result.signatureValid) result.error = "manifest signature verification failed";
        return result;
    }

    bool targetCompatible(const Manifest& m,
                          const char* product,
                          const char* board,
                          const char* hardwareRole,
                          const char* espChip,
                          const char* partitionScheme,
                          uint16_t protocol,
                          uint32_t currentReleaseCounter,
                          bool physicalRecoveryConfirmed,
                          std::string& error) {
        if (m.product != product) error = "product mismatch";
        else if (m.board != board) error = "board mismatch";
        else if (m.hardwareRole != hardwareRole) error = "hardware role mismatch";
        else if (m.espChip != espChip) error = "ESP chip mismatch";
        else if (m.partitionScheme != partitionScheme) error = "partition scheme mismatch";
        else if (protocol < m.protocolMin || protocol > m.protocolMax) error = "OTA protocol mismatch";
        else if (m.releaseCounter <= currentReleaseCounter &&
                 !(m.recovery && m.allowDowngrade && physicalRecoveryConfirmed)) error = "release counter replay or downgrade";
        else return true;
        return false;
    }

    StreamValidator::StreamValidator() {
        mbedtls_sha256_init(&_imageHash);
        reset();
    }
    StreamValidator::~StreamValidator() {
        mbedtls_sha256_free(&_imageHash);
    }
    void StreamValidator::reset() {
        _received = _manifestLength = _signatureLength = _imageLength = _imageReceived = 0;
        _headerDecoded = _failed = false;
        _error.clear();
        memset(_header, 0, sizeof(_header));
        memset(_manifest, 0, sizeof(_manifest));
        memset(_signature, 0, sizeof(_signature));
        mbedtls_sha256_starts_ret(&_imageHash, 0);
    }
    bool StreamValidator::fail(const char* message) {
        _failed = true;
        _error = message;
        return false;
    }
    bool StreamValidator::decodeHeader() {
        if (memcmp(_header, Magic, sizeof(Magic)) != 0) return fail("wrong package magic");
        if (le16(_header + 8) != 1 || le16(_header + 10) != 0) return fail("unsupported package version or flags");
        _manifestLength  = le32(_header + 12);
        _signatureLength = le32(_header + 16);
        _imageLength     = le32(_header + 20);
        if (!_manifestLength || _manifestLength > MaxManifestBytes || !_signatureLength ||
            _signatureLength > MaxSignatureBytes || !_imageLength || _imageLength > MaxImageBytes) {
            return fail("package lengths are outside the supported range");
        }
        _headerDecoded = true;
        return true;
    }
    bool StreamValidator::write(const uint8_t* data, size_t length) {
        if (_failed || (!data && length)) return false;
        while (length) {
            if (_received < HeaderSize) {
                size_t chunk = std::min(length, HeaderSize - _received);
                memcpy(_header + _received, data, chunk);
                _received += chunk; data += chunk; length -= chunk;
                if (_received == HeaderSize && !decodeHeader()) return false;
                continue;
            }
            size_t relative = _received - HeaderSize;
            if (relative < _manifestLength) {
                size_t chunk = std::min<size_t>(length, _manifestLength - relative);
                memcpy(_manifest + relative, data, chunk);
                _received += chunk; data += chunk; length -= chunk;
                continue;
            }
            relative -= _manifestLength;
            if (relative < _signatureLength) {
                size_t chunk = std::min<size_t>(length, _signatureLength - relative);
                memcpy(_signature + relative, data, chunk);
                _received += chunk; data += chunk; length -= chunk;
                continue;
            }
            relative -= _signatureLength;
            if (relative >= _imageLength || length > (_imageLength - relative)) return fail("package has trailing or oversized image data");
            mbedtls_sha256_update_ret(&_imageHash, data, length);
            _imageReceived += length;
            _received += length;
            data += length;
            length = 0;
        }
        return true;
    }
    ValidationResult StreamValidator::finish() {
        ValidationResult result;
        result.error = _error;
        if (_failed || !_headerDecoded ||
            _received != HeaderSize + _manifestLength + _signatureLength + _imageLength ||
            _imageReceived != _imageLength) {
            if (result.error.empty()) result.error = "truncated package";
            return result;
        }
        result.envelopeValid = true;
        result.manifestRaw.assign(reinterpret_cast<const char*>(_manifest), _manifestLength);
        result.signatureRaw.assign(reinterpret_cast<const char*>(_signature), _signatureLength);
        if (!parseManifest(_manifest, _manifestLength, result.manifest, result.error)) return result;
        result.manifestValid = true;
        if (result.manifest.imageLength != _imageLength) {
            result.error = "signed image length differs from envelope";
            return result;
        }
        uint8_t imageDigest[32];
        mbedtls_sha256_finish_ret(&_imageHash, imageDigest);
        result.imageSha256  = hexDigest(imageDigest, sizeof(imageDigest));
        result.imageHashValid = result.imageSha256 == result.manifest.imageSha256;
        if (!result.imageHashValid) {
            result.error = "application image SHA-256 mismatch";
            return result;
        }
        ValidationResult signedManifest =
            validateSignedManifest(_manifest, _manifestLength, _signature, _signatureLength);
        result.manifestSha256 = signedManifest.manifestSha256;
        result.signatureValid = signedManifest.signatureValid;
        result.keyRecovery    = signedManifest.keyRecovery;
        if (!result.signatureValid) result.error = signedManifest.error;
        return result;
    }
}
