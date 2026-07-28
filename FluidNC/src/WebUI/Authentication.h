#pragma once

#include <cstdint>

//Authentication level
enum class AuthenticationLevel : uint8_t { LEVEL_GUEST = 0, LEVEL_USER = 1, LEVEL_ADMIN = 2 };

#ifdef ENABLE_AUTHENTICATION
void make_authentication_settings();
bool authentication_password_matches(bool administrator, const char* password);
bool authentication_set_password(bool administrator, const char* password);
bool authentication_admin_password_is_default();
void authentication_reset_user_password();

#endif
