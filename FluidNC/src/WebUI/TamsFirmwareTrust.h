// TAMS firmware signing trust configuration.
//
// Production public keys are injected by the release build as PEM strings.
// Private keys must never be placed in this repository.
#pragma once

#include <cstring>

#ifndef TAMS_FW_PRODUCTION_KEY_ID
#    define TAMS_FW_PRODUCTION_KEY_ID "xza-production-p256-f6a017cc2049"
#endif
#ifndef TAMS_FW_PRODUCTION_KEY_PEM
#    define TAMS_FW_PRODUCTION_KEY_PEM                                                                                  \
        "-----BEGIN PUBLIC KEY-----\n"                                                                                  \
        "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEC5k1yIpyZuXQWWIDSFGCHYGErSyn\n"                                             \
        "6rlq66iEKuZYPcVLby3wcl8BL298cK73LgyQ0wmHKcf8TF6dIyP9V2paVA==\n"                                                 \
        "-----END PUBLIC KEY-----\n"
#endif
#ifndef TAMS_FW_RECOVERY_KEY_ID
#    define TAMS_FW_RECOVERY_KEY_ID "xza-recovery-p256-dae6bdeea1af"
#endif
#ifndef TAMS_FW_RECOVERY_KEY_PEM
#    define TAMS_FW_RECOVERY_KEY_PEM                                                                                    \
        "-----BEGIN PUBLIC KEY-----\n"                                                                                  \
        "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEq2VdhtnfOqCy8LTPiVGrYrWZngrN\n"                                             \
        "1rMpm/XUxjVS64rgV/XHVMRulVWg3EEUrSkKMvVLcfJv3xbgGRMCqmjZFw==\n"                                                 \
        "-----END PUBLIC KEY-----\n"
#endif

namespace TamsFirmware {
    struct TrustKey {
        const char* id;
        const char* pem;
        bool        recovery;
    };

    inline const TrustKey* findTrustKey(const char* id) {
        static const TrustKey keys[] = {
            { TAMS_FW_PRODUCTION_KEY_ID, TAMS_FW_PRODUCTION_KEY_PEM, false },
            { TAMS_FW_RECOVERY_KEY_ID, TAMS_FW_RECOVERY_KEY_PEM, true },
        };
        if (!id || !id[0]) {
            return nullptr;
        }
        for (const auto& key : keys) {
            if (key.id[0] && key.pem[0] && strcmp(key.id, id) == 0) {
                return &key;
            }
        }
        return nullptr;
    }

    inline bool trustConfigured() {
        return TAMS_FW_PRODUCTION_KEY_ID[0] && TAMS_FW_PRODUCTION_KEY_PEM[0];
    }
}
