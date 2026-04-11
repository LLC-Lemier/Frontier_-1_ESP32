#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

// Максимальное количество одновременных сессий
#define MAX_SESSIONS 32
#define SESSION_TTL 3600  // 1 час
#define SESSION_DIR "/sdcard/sessions"
#define USERS_DIR "/sdcard/users"


typedef struct {
    char session_id[33];      // 32 байта + \0
    char user_id[64];
    time_t created_at;
    time_t expires_at;
    char ip_address[16];
    char user_agent[128];
    bool is_active;
} session_t;

typedef struct {
    char username[64];
    char password_hash[65];   // SHA256 хеш пароля
    char role[32];            // "admin", "user", "sensor"
    bool is_active;
} user_t;

session_t* create_session(
    const char *username, 
    const char *password, 
    const char *ip, 
    const char *user_agent
); 
