#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <mbedtls/sha256.h>

namespace TamsFirmware {
    static constexpr size_t HeaderSize       = 24;
    static constexpr size_t MaxManifestBytes = 4096;
    static constexpr size_t MaxSignatureBytes = 128;
    static constexpr size_t MaxImageBytes    = 8 * 1024 * 1024;

    struct Manifest {
        uint16_t    manifestVersion = 0;
        std::string packageId;
        std::string product;
        std::string board;
        std::string hardwareRole;
        std::string espChip;
        std::string partitionScheme;
        std::string version;
        uint32_t    releaseCounter = 0;
        uint16_t    protocolMin    = 0;
        uint16_t    protocolMax    = 0;
        uint32_t    imageLength    = 0;
        std::string imageSha256;
        std::string signingKeyId;
        bool        recovery       = false;
        bool        allowDowngrade = false;
    };

    struct ValidationResult {
        bool        envelopeValid  = false;
        bool        manifestValid  = false;
        bool        signatureValid = false;
        bool        imageHashValid = false;
        bool        keyRecovery    = false;
        std::string manifestSha256;
        std::string imageSha256;
        std::string manifestRaw;
        std::string signatureRaw;
        std::string error;
        Manifest    manifest;

        bool valid() const {
            return envelopeValid && manifestValid && signatureValid && imageHashValid && error.empty();
        }
    };

    bool parseManifest(const uint8_t* raw, size_t length, Manifest& manifest, std::string& error);
    ValidationResult validateSignedManifest(
        const uint8_t* manifestRaw, size_t manifestLength, const uint8_t* signature, size_t signatureLength);
    bool targetCompatible(const Manifest& manifest,
                          const char* product,
                          const char* board,
                          const char* hardwareRole,
                          const char* espChip,
                          const char* partitionScheme,
                          uint16_t protocol,
                          uint32_t currentReleaseCounter,
                          bool physicalRecoveryConfirmed,
                          std::string& error);

    class StreamValidator {
    public:
        StreamValidator();
        ~StreamValidator();

        void reset();
        bool write(const uint8_t* data, size_t length);
        ValidationResult finish();
        size_t bytesReceived() const { return _received; }

    private:
        bool fail(const char* message);
        bool decodeHeader();

        uint8_t  _header[HeaderSize] = {};
        uint8_t  _manifest[MaxManifestBytes] = {};
        uint8_t  _signature[MaxSignatureBytes] = {};
        size_t   _received       = 0;
        uint32_t _manifestLength = 0;
        uint32_t _signatureLength = 0;
        uint32_t _imageLength    = 0;
        uint32_t _imageReceived  = 0;
        bool     _headerDecoded  = false;
        bool     _failed         = false;
        std::string _error;
        mbedtls_sha256_context _imageHash;
    };
}
