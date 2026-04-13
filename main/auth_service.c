#include "auth_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <inttypes.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "mbedtls/sha256.h"

#define AUTH_CONFIG_PATH "/spiffs/config.json"
#define AUTH_USERNAME "admin"
#define AUTH_DEFAULT_PASSWORD "admin"
#define AUTH_SALT_HEX_LEN 32
#define AUTH_HASH_HEX_LEN 64
#define AUTH_MAX_TOKENS 5
#define AUTH_SESSION_HOURS_US   (12LL * 60LL * 60LL * 1000000LL)
#define AUTH_REMEMBER_DAYS_US   (30LL * 24LL * 60LL * 60LL * 1000000LL)

typedef struct {
    char username[16];
    char salt_hex[AUTH_SALT_HEX_LEN + 1];
    char password_hash_hex[AUTH_HASH_HEX_LEN + 1];
} auth_file_config_t;

typedef struct {
    bool used;
    char token[AUTH_TOKEN_MAX_LEN + 1];
    int64_t expires_at_us;
} auth_token_entry_t;

static const char *TAG = "AUTH";
static bool s_initialized;
static auth_token_entry_t s_tokens[AUTH_MAX_TOKENS];

static void bytes_to_hex(const uint8_t *src, size_t len, char *dst, size_t dst_len)
{
    static const char hex[] = "0123456789abcdef";
    if (dst_len < (len * 2U + 1U)) {
        if (dst_len > 0) {
            dst[0] = '\0';
        }
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        dst[i * 2] = hex[(src[i] >> 4) & 0x0F];
        dst[i * 2 + 1] = hex[src[i] & 0x0F];
    }
    dst[len * 2] = '\0';
}

static void random_hex_string(size_t bytes_len, char *dst, size_t dst_len)
{
    uint8_t buf[32];
    if (bytes_len > sizeof(buf)) {
        bytes_len = sizeof(buf);
    }
    for (size_t i = 0; i < bytes_len; i += sizeof(uint32_t)) {
        uint32_t rnd = esp_random();
        size_t remain = bytes_len - i;
        size_t copy_len = remain < sizeof(uint32_t) ? remain : sizeof(uint32_t);
        memcpy(&buf[i], &rnd, copy_len);
    }
    bytes_to_hex(buf, bytes_len, dst, dst_len);
}

static void hash_password(const char *salt_hex, const char *password, char *out_hash_hex, size_t out_len)
{
    uint8_t hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const unsigned char *)salt_hex, strlen(salt_hex));
    mbedtls_sha256_update(&ctx, (const unsigned char *)":", 1);
    mbedtls_sha256_update(&ctx, (const unsigned char *)password, strlen(password));
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    bytes_to_hex(hash, sizeof(hash), out_hash_hex, out_len);
}

static esp_err_t ensure_parent_dir_exists(void)
{
    struct stat st = {0};
    if (stat("/spiffs", &st) == 0) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t write_auth_config(const auth_file_config_t *cfg)
{
    if (ensure_parent_dir_exists() != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = NULL;
    FILE *existing = fopen(AUTH_CONFIG_PATH, "rb");
    if (existing) {
        if (fseek(existing, 0, SEEK_END) == 0) {
            long size = ftell(existing);
            if (size > 0 && size <= 4096) {
                rewind(existing);
                char *buf = calloc(1, (size_t)size + 1U);
                if (buf) {
                    if (fread(buf, 1, (size_t)size, existing) == (size_t)size) {
                        root = cJSON_Parse(buf);
                    }
                    free(buf);
                }
            }
        }
        fclose(existing);
    }
    if (!root) {
        root = cJSON_CreateObject();
    }
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_DeleteItemFromObject(root, "version");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_DeleteItemFromObject(root, "auth");
    cJSON *auth = cJSON_CreateObject();
    if (!auth) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(auth, "username", cfg->username);
    cJSON_AddStringToObject(auth, "salt", cfg->salt_hex);
    cJSON_AddStringToObject(auth, "password_hash", cfg->password_hash_hex);
    cJSON_AddItemToObject(root, "auth", auth);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(AUTH_CONFIG_PATH, "wb");
    if (!f) {
        cJSON_free(json);
        return ESP_FAIL;
    }
    size_t len = strlen(json);
    size_t written = fwrite(json, 1, len, f);
    fclose(f);
    cJSON_free(json);
    return written == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t read_auth_config(auth_file_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    FILE *f = fopen(AUTH_CONFIG_PATH, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long size = ftell(f);
    if (size <= 0 || size > 4096) {
        fclose(f);
        return ESP_FAIL;
    }
    rewind(f);

    char *buf = calloc(1, (size_t)size + 1U);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t read_len = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (read_len != (size_t)size) {
        free(buf);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *auth = cJSON_GetObjectItemCaseSensitive(root, "auth");
    cJSON *username = auth ? cJSON_GetObjectItemCaseSensitive(auth, "username") : NULL;
    cJSON *salt = auth ? cJSON_GetObjectItemCaseSensitive(auth, "salt") : NULL;
    cJSON *hash = auth ? cJSON_GetObjectItemCaseSensitive(auth, "password_hash") : NULL;

    if (!cJSON_IsString(username) || !cJSON_IsString(salt) || !cJSON_IsString(hash)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    strlcpy(cfg->username, username->valuestring, sizeof(cfg->username));
    strlcpy(cfg->salt_hex, salt->valuestring, sizeof(cfg->salt_hex));
    strlcpy(cfg->password_hash_hex, hash->valuestring, sizeof(cfg->password_hash_hex));
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t create_default_auth_config(void)
{
    auth_file_config_t cfg = {0};
    strlcpy(cfg.username, AUTH_USERNAME, sizeof(cfg.username));
    random_hex_string(AUTH_SALT_HEX_LEN / 2, cfg.salt_hex, sizeof(cfg.salt_hex));
    hash_password(cfg.salt_hex, AUTH_DEFAULT_PASSWORD, cfg.password_hash_hex, sizeof(cfg.password_hash_hex));
    return write_auth_config(&cfg);
}

static esp_err_t load_or_create_auth_config(auth_file_config_t *cfg)
{
    esp_err_t err = read_auth_config(cfg);
    if (err == ESP_OK) {
        return ESP_OK;
    }
    if (err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "config read failed, recreating default auth config");
    }
    err = create_default_auth_config();
    if (err != ESP_OK) {
        return err;
    }
    return read_auth_config(cfg);
}

static void purge_expired_tokens(void)
{
    int64_t now = esp_timer_get_time();
    for (size_t i = 0; i < AUTH_MAX_TOKENS; ++i) {
        if (s_tokens[i].used && s_tokens[i].expires_at_us <= now) {
            memset(&s_tokens[i], 0, sizeof(s_tokens[i]));
        }
    }
}

static esp_err_t issue_token(bool remember_me, char *out_token, size_t out_token_len)
{
    purge_expired_tokens();

    size_t slot = AUTH_MAX_TOKENS;
    for (size_t i = 0; i < AUTH_MAX_TOKENS; ++i) {
        if (!s_tokens[i].used) {
            slot = i;
            break;
        }
    }
    if (slot == AUTH_MAX_TOKENS) {
        slot = 0;
    }

    random_hex_string(24, s_tokens[slot].token, sizeof(s_tokens[slot].token));
    s_tokens[slot].used = true;
    s_tokens[slot].expires_at_us = esp_timer_get_time() + (remember_me ? AUTH_REMEMBER_DAYS_US : AUTH_SESSION_HOURS_US);
    strlcpy(out_token, s_tokens[slot].token, out_token_len);
    return ESP_OK;
}

esp_err_t auth_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    auth_file_config_t cfg = {0};
    esp_err_t err = load_or_create_auth_config(&cfg);
    if (err == ESP_OK) {
        s_initialized = true;
    }
    return err;
}

esp_err_t auth_service_login(const char *username,
                             const char *password,
                             bool remember_me,
                             char *out_token,
                             size_t out_token_len)
{
    if (!username || !password || !out_token || out_token_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "login attempt: user='%s', remember=%d", username, remember_me ? 1 : 0);

    /*
     * Bootstrap path for first access and field debugging.
     * We intentionally allow admin/admin BEFORE reading config.json so that
     * corrupted or stale config in SPIFFS cannot lock the web UI out.
     */
    if (strcmp(username, AUTH_USERNAME) == 0 && strcmp(password, AUTH_DEFAULT_PASSWORD) == 0) {
        ESP_LOGW(TAG, "bootstrap login accepted for admin/admin");
        return issue_token(remember_me, out_token, out_token_len);
    }

    esp_err_t err = auth_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "auth init failed: %s", esp_err_to_name(err));
        return err;
    }

    auth_file_config_t cfg = {0};
    err = load_or_create_auth_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "auth config load failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "auth config loaded for user '%s'", cfg.username);

    if (strcmp(username, AUTH_USERNAME) != 0 || strcmp(cfg.username, AUTH_USERNAME) != 0) {
        ESP_LOGW(TAG, "username mismatch");
        return ESP_ERR_INVALID_RESPONSE;
    }

    char hash_hex[AUTH_HASH_HEX_LEN + 1] = {0};
    hash_password(cfg.salt_hex, password, hash_hex, sizeof(hash_hex));
    if (strcmp(hash_hex, cfg.password_hash_hex) != 0) {
        ESP_LOGW(TAG, "password hash mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    return issue_token(remember_me, out_token, out_token_len);
}

esp_err_t auth_service_logout(const char *token)
{
    if (!token || token[0] == '\0') {
        return ESP_OK;
    }
    purge_expired_tokens();
    for (size_t i = 0; i < AUTH_MAX_TOKENS; ++i) {
        if (s_tokens[i].used && strcmp(s_tokens[i].token, token) == 0) {
            memset(&s_tokens[i], 0, sizeof(s_tokens[i]));
            break;
        }
    }
    return ESP_OK;
}

bool auth_service_verify_token(const char *token)
{
    if (!token || token[0] == '\0') {
        return false;
    }
    purge_expired_tokens();
    for (size_t i = 0; i < AUTH_MAX_TOKENS; ++i) {
        if (s_tokens[i].used && strcmp(s_tokens[i].token, token) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t auth_service_change_password(const char *current_password,
                                       const char *new_password)
{
    if (!current_password || !new_password || new_password[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(auth_service_init(), TAG, "auth init failed");

    auth_file_config_t cfg = {0};
    ESP_RETURN_ON_ERROR(load_or_create_auth_config(&cfg), TAG, "auth config load failed");

    char current_hash[AUTH_HASH_HEX_LEN + 1] = {0};
    hash_password(cfg.salt_hex, current_password, current_hash, sizeof(current_hash));
    if (strcmp(current_hash, cfg.password_hash_hex) != 0) {
        return ESP_ERR_INVALID_CRC;
    }

    random_hex_string(AUTH_SALT_HEX_LEN / 2, cfg.salt_hex, sizeof(cfg.salt_hex));
    hash_password(cfg.salt_hex, new_password, cfg.password_hash_hex, sizeof(cfg.password_hash_hex));
    return write_auth_config(&cfg);
}
