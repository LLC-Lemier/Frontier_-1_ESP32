#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUTH_TOKEN_MAX_LEN 64

esp_err_t auth_service_init(void);
esp_err_t auth_service_login(const char *username,
                             const char *password,
                             bool remember_me,
                             char *out_token,
                             size_t out_token_len);
esp_err_t auth_service_logout(const char *token);
bool auth_service_verify_token(const char *token);
esp_err_t auth_service_change_password(const char *current_password,
                                       const char *new_password);

#ifdef __cplusplus
}
#endif
