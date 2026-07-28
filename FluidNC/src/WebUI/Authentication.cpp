#include "Authentication.h"

#ifdef ENABLE_AUTHENTICATION
#include "Config.h"
#include "Settings.h"

namespace {
    constexpr int MinLocalPasswordLength = 12;
    constexpr int MaxLocalPasswordLength = 16;

    class AuthPasswordSetting : public StringSetting {
    public:
        AuthPasswordSetting(const char* description, const char* name, const char* defaultValue) :
            StringSetting(description,
                          WEBSET,
                          WA,
                          nullptr,
                          name,
                          defaultValue,
                          MinLocalPasswordLength,
                          MaxLocalPasswordLength) {}

        const char* getDefaultString() override { return "********"; }
        const char* getStringValue() override { return "********"; }
        Error setStringValue(std::string_view value) override {
            for (char character : value) {
                if (character == ' ') return Error::InvalidValue;
            }
            return StringSetting::setStringValue(value);
        }
    };

    AuthPasswordSetting* userPassword  = nullptr;
    AuthPasswordSetting* adminPassword = nullptr;
}

void make_authentication_settings() {
    if (userPassword || adminPassword) return;
    userPassword  = new AuthPasswordSetting("User password", "WebUI/UserPassword", DEFAULT_USER_PWD);
    adminPassword = new AuthPasswordSetting("Admin password", "WebUI/AdminPassword", DEFAULT_ADMIN_PWD);
}

bool authentication_password_matches(bool administrator, const char* password) {
    make_authentication_settings();
    AuthPasswordSetting* setting = administrator ? adminPassword : userPassword;
    return password && setting && std::string_view(password) == setting->get();
}

bool authentication_set_password(bool administrator, const char* password) {
    make_authentication_settings();
    AuthPasswordSetting* setting = administrator ? adminPassword : userPassword;
    return setting && setting->setStringValue(password ? password : "") == Error::Ok;
}

bool authentication_admin_password_is_default() {
    make_authentication_settings();
    return !adminPassword || std::string_view(adminPassword->get()) == DEFAULT_ADMIN_PWD;
}

void authentication_reset_user_password() {
    make_authentication_settings();
    if (userPassword) userPassword->setDefault();
}
#endif
