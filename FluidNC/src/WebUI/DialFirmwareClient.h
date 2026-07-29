#pragma once

#include "TamsFirmwarePackage.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace WebUI {
    struct DialFirmwareState {
        bool        paired    = false;
        bool        online    = false;
        bool        ambiguous = false;
        std::string deviceId;
        std::string fingerprint;
        std::string ip;
        std::string version;
        std::string health;
        std::string fluidNcLinkState;
        std::string pairTag;
        std::string lastError;
        uint32_t    releaseCounter = 0;
    };

    struct DialDeploymentState {
        bool        active    = false;
        bool        terminal  = false;
        bool        success   = false;
        bool        receiptPersisted = false;
        uint8_t     stage     = 0;
        uint32_t    acceptedOffset = 0;
        uint32_t    expectedBytes  = 0;
        uint32_t    counter   = 0;
        uint32_t    fromReleaseCounter = 0;
        uint32_t    toReleaseCounter   = 0;
        uint32_t    startedAt          = 0;
        uint32_t    lastActivity       = 0;
        std::string deploymentId;
        std::string packageId;
        std::string signingKeyId;
        std::string manifestDigest;
        std::string imageSha256;
        std::string fromVersion;
        std::string toVersion;
        std::string error;
        std::string result;
    };

    struct DialDiagnosticGrant {
        std::string ip;
        std::string path;
        std::string target;
        std::string nonce;
        std::string manifestDigest;
        std::string bodyDigest;
        std::string authorization;
        uint32_t    counter   = 0;
        uint32_t    expiresMs = 0;
    };

    class DialFirmwareClient {
    public:
        static DialFirmwareClient& instance();

        void init(const std::string& controllerDeviceId);
        bool discover(bool force = false);
        const DialFirmwareState& state() const { return _state; }
        const DialDeploymentState& deployment() const { return _deployment; }

        bool startPairing(std::string& comparisonCode);
        bool confirmPairing(const std::string& comparisonCode);
        bool pollPairing();
        void cancelPairing();
        bool pairFromUart(const std::string& deviceId,
                          const std::string& fingerprint,
                          const std::string& deviceNonce,
                          std::string& response);

        bool beginDeployment(const TamsFirmware::ValidationResult& package, const std::string& deploymentId);
        bool relayChunk(uint32_t offset, const uint8_t* data, size_t length);
        bool commitDeployment();
        bool abortDeployment();
        bool refreshHealth();
        bool issueDiagnosticGrant(const std::string& path, DialDiagnosticGrant& grant);
        bool verifyDiagnosticResponse(const std::string& nonce,
                                      uint32_t counter,
                                      int status,
                                      const std::string& bodyDigest,
                                      const std::string& responseAuthorization);
        void expireDeployment(const char* reason);
        void setReceiptPersisted(bool persisted) { _deployment.receiptPersisted = persisted; }

        std::string stateJson() const;
        std::string deploymentJson() const;

    private:
        DialFirmwareClient() = default;
        bool loadIdentity();
        bool persistPair();
        bool exactTargetOnline();
        bool requestChallenge(uint32_t requestedCounter, std::string& nonce, uint32_t& counter);
        bool authenticatedRequest(const char* method,
                                  const char* requestPath,
                                  const char* canonicalPath,
                                  const std::string& contentType,
                                  const std::string& body,
                                  const std::string& bodyDigest,
                                  uint32_t counter,
                                  std::string& response,
                                  uint32_t* authorizedCounter = nullptr);

        DialFirmwareState    _state;
        DialDeploymentState _deployment;
        std::string _controllerDeviceId;
        std::string _controllerFingerprint;
        uint8_t     _controllerPrivate[32] = {};
        uint8_t     _controllerPublic[65]  = {};
        uint8_t     _pairSecret[32]        = {};
        uint8_t     _pendingPrivate[32]    = {};
        uint8_t     _pendingSecret[32]     = {};
        std::string _pendingDeviceId;
        std::string _pendingFingerprint;
        std::string _pendingIp;
        std::string _pendingCode;
        std::string _diagnosticNonce;
        uint32_t    _diagnosticCounter   = 0;
        uint32_t    _diagnosticExpiresMs = 0;
        bool        _diagnosticPending   = false;
        bool        _initialized = false;
        bool        _lastDiscoveryOk = false;
        uint32_t    _lastDiscoveryAt = 0;
    };
}
