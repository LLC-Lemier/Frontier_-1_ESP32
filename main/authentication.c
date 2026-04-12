#include "authentication.h"

#include "esp_log.h"
#include "esp_err.h"


#include <mbedtls/sha512.h>

static const char *TAG = "SESSION_MANAGER";

static user_t* s_users;
static uint32_t s_users_len;
static session_t s_sessions[MAX_SESSIONS];
static uint32_t s_sessions_len = 0;


esp_err_t init_session_storage(void) {
    // Монтируем SD-карту (предполагаем, что уже настроена)
    // Создаём директории если их нет
    /*struct stat st;
    
    if (stat("/sdcard", &st) != 0) {
        ESP_LOGE(TAG, "SD card not mounted");
        return ESP_FAIL;
    }
    
    // Создаём директорию для сессий
    if (stat(SESSION_DIR, &st) != 0) {
        mkdir(SESSION_DIR, 0755);
        ESP_LOGI(TAG, "Created sessions directory");
    }
    
    // Создаём директорию для пользователей
    if (stat(USERS_DIR, &st) != 0) {
        mkdir(USERS_DIR, 0755);
        ESP_LOGI(TAG, "Created users directory");
        
        // Создаём тестового пользователя
        create_default_user();
    }*/

    create_default_user();

    s_users = malloc(sizeof(user_t) * 5);
    if (!s_users) {
        ESP_LOGE(TAG, "Failed to allocate memory for users");
        return ESP_FAIL;
    }
    s_users_len = 1; // Пока только один тестовый пользователь
    
    // Запускаем таймер очистки старых сессий
    start_session_cleanup_timer();
    
    return ESP_OK;
}


void sha512_string(const char *data,char *hash_result) {
    mbedtls_sha512_context ctx;
    mbedtls_sha512_init(&ctx);
    mbedtls_sha512_update(&ctx, (const unsigned char *)data, strlen(data));
    mbedtls_sha512_finish(&ctx, (unsigned char *)hash_result);
    mbedtls_sha512_free(&ctx);
}

void save_user(const user_t *user) {
    // Здесь должен быть код для сохранения пользователя на SD-карту
    // Для демонстрации просто сохраняем в массиве
    if (s_users_len < 5) {
        s_users[s_users_len++] = *user;
        ESP_LOGI(TAG, "User saved: %s", user->username);
    } else {
        ESP_LOGE(TAG, "User storage full");
    }
}

void create_default_user(void) {
    user_t admin = {
        .username = "admin",
        .password_hash = "",  // Заполним позже
        .role = "admin",
        .is_active = true
    };

    // Хешируем пароль "admin123"
    sha512_string("admin123", admin.password_hash);

    save_user(&admin);
} 

// Генерация уникального session_id
void generate_session_id(char *buffer, size_t len) {
    uint8_t random_bytes[16];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    
    for (int i = 0; i < 16; i++) {
        sprintf(buffer + (i * 2), "%02x", random_bytes[i]);
    }
    buffer[32] = '\0';
}

esp_err_t load_user(const char *username, user_t *user) {
    // Здесь должен быть код для загрузки пользователя из SD-карты
    // Для демонстрации просто ищем в массиве
    for (uint32_t i = 0; i < s_users_len; i++) {
        if (strcmp(s_users[i].username, username) == 0) {
            *user = s_users[i];
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t authenticate_user(const char *username, const char *password, user_t *user) {
    // Загружаем пользователя из SD-карты
    if (load_user(username, user) != ESP_OK) {
        return ESP_FAIL;
    }
    
    if (!user->is_active) {
        return ESP_FAIL;
    }
    
    // Хешируем введённый пароль и сравниваем
    char password_hash[65];
    sha512_string(password, password_hash);
    
    if (strcmp((const char *)password_hash, (const char *)user->password_hash) == 0) {
        return ESP_OK;
    }
    
    return ESP_FAIL;
}

// Сохранение сессии в файл
esp_err_t save_session(session_t *session) {
    ESP_LOGI(TAG, "Saving session: %s for user: %s", session->session_id, session->user_id);
    if (!session) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t i = 0; i < s_sessions_len; i++) {
        if (strcmp(s_sessions[i].session_id, session->session_id) == 0) {
            s_sessions[i] = *session;
            return ESP_OK;
        }
    }

    if (s_sessions_len < MAX_SESSIONS) {
        s_sessions[s_sessions_len++] = *session;
        return ESP_OK;
    } 
    ESP_LOGE(TAG, "Session storage full");
    return ESP_FAIL;
    /*char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/sess_%s.json", 
             SESSION_DIR, session->session_id);
    
    FILE *f = fopen(filepath, "w");
    if (!f) return ESP_FAIL;
    
    fprintf(f, "{\n");
    fprintf(f, "  \"session_id\": \"%s\",\n", session->session_id);
    fprintf(f, "  \"user_id\": \"%s\",\n", session->user_id);
    fprintf(f, "  \"created_at\": %ld,\n", session->created_at);
    fprintf(f, "  \"expires_at\": %ld,\n", session->expires_at);
    fprintf(f, "  \"ip_address\": \"%s\",\n", session->ip_address);
    fprintf(f, "  \"user_agent\": \"%s\",\n", session->user_agent);
    fprintf(f, "  \"is_active\": %s\n", session->is_active ? "true" : "false");
    fprintf(f, "}");
    
    fclose(f);
    
    ESP_LOGI(TAG, "Session saved: %s", session->session_id);
    return ESP_OK;*/
}

// Загрузка сессии из файла
esp_err_t load_session(const char *session_id, session_t *session) {
    /*char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/sess_%s.json", 
             SESSION_DIR, session_id);
    
    FILE *f = fopen(filepath, "r");
    if (!f) return ESP_FAIL;
    
    // Простой парсинг JSON (для демонстрации)
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "\"session_id\"")) {
            sscanf(line, "  \"session_id\": \"%[^\"]\",", session->session_id);
        } else if (strstr(line, "\"user_id\"")) {
            sscanf(line, "  \"user_id\": \"%[^\"]\",", session->user_id);
        } else if (strstr(line, "\"created_at\"")) {
            sscanf(line, "  \"created_at\": %ld,", &session->created_at);
        } else if (strstr(line, "\"expires_at\"")) {
            sscanf(line, "  \"expires_at\": %ld,", &session->expires_at);
        } else if (strstr(line, "\"ip_address\"")) {
            sscanf(line, "  \"ip_address\": \"%[^\"]\",", session->ip_address);
        } else if (strstr(line, "\"is_active\"")) {
            char active_str[8];
            sscanf(line, "  \"is_active\": %s", active_str);
            session->is_active = (strcmp(active_str, "true") == 0);
        }
    }
    
    fclose(f);*/

    ESP_LOGI(TAG, "Loading session: %s", session_id);
    for (uint32_t i = 0; i < s_sessions_len; i++) {
        ESP_LOGI(TAG, "Checking session: %s", s_sessions[i].session_id);
        if (strcmp(s_sessions[i].session_id, session_id) == 0) {
            ESP_LOGI(TAG, "Session found: %s", session_id);
            *session = s_sessions[i];
            return ESP_OK;
        }
    }

    return ESP_FAIL;
}

// Удаление сессии (logout)
esp_err_t delete_session(const char *session_id) {

    for (uint32_t i = 0; i < s_sessions_len; i++) {
        if (strcmp(s_sessions[i].session_id, session_id) == 0) {
            // Удаляем сессию из массива
            for (uint32_t j = i; j < s_sessions_len - 1; j++) {
                s_sessions[j] = s_sessions[j + 1];
            }
            s_sessions_len--;
            return ESP_OK;
        }
    }
    return ESP_FAIL;
    /*char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/sess_%s.json", 
             SESSION_DIR, session_id);
    
    if (remove(filepath) == 0) {
        ESP_LOGI(TAG, "Session deleted: %s", session_id);
        return ESP_OK;
    }
    
    return ESP_FAIL;*/
}


void cleanup_user_sessions(const char *username) {
    for (uint32_t i = 0; i < s_sessions_len; ) {
        if (strcmp(s_sessions[i].user_id, username) == 0) {
            for (uint32_t j = i; j < s_sessions_len - 1; j++) {
                s_sessions[j] = s_sessions[j + 1];
            }
            s_sessions_len--;
        } else {
            i++;
        }
    }
}

// Создание новой сессии при логине
session_t* create_session(
    const char *username, 
    const char *password, 
    const char *ip, 
    const char *user_agent
) {
    ESP_LOGI(TAG, "Creating session for user: %s", username);
    // 1. Проверяем пользователя
    user_t user;
    if (!authenticate_user(username, password, &user)) {
        ESP_LOGE(TAG, "Authentication failed for %s", username);
        return NULL;
    }
    
    // 2. Создаём новую сессию
    session_t *session = calloc(1, sizeof(session_t));
    generate_session_id(session->session_id, sizeof(session->session_id));
    strncpy(session->user_id, username, sizeof(session->user_id) - 1);
    session->created_at = time(NULL);
    session->expires_at = session->created_at + SESSION_TTL;
    strncpy(session->ip_address, ip, sizeof(session->ip_address) - 1);
    strncpy(session->user_agent, user_agent, sizeof(session->user_agent) - 1);
    session->is_active = true;
    // 4. Очищаем старые сессии этого пользователя (опционально)
    //
    cleanup_user_sessions(username);
    

    // 3. Сохраняем на SD-карту
    if (save_session(session) != ESP_OK) {
        //free(session);
        ESP_LOGE(TAG, "Failed to save session for user: %s", username);
        return NULL;
    }
    
    
    return session;
}

// Проверка валидности сессии
session_t* validate_session(const char *session_id, const char *ip_address) {
    session_t session;
    
    if (load_session(session_id, &session) != ESP_OK) {
        ESP_LOGW(TAG, "Session not found: %s", session_id);
        return NULL;
    }
    
    // Проверяем активность
    if (!session.is_active) {
        ESP_LOGW(TAG, "Session inactive: %s", session_id);
        return NULL;
    }
    
    // Проверяем время жизни
    time_t now = time(NULL);
    if (now > session.expires_at) {
        // Сессия истекла - удаляем
        ESP_LOGW(TAG, "Session expired: %s", session_id);
        delete_session(session_id);
        return NULL;
    }
    
    // Опционально: проверяем IP (для безопасности)

    if (strcmp(session.ip_address, ip_address) != 0) {
        ESP_LOGW(TAG, "IP mismatch for session %s", session_id);
        ESP_LOGW(TAG, "Expected IP: %s, Actual IP: %s", session.ip_address, ip_address);
        // Можно запретить или разрешить - зависит от требований безопасности
        return NULL;
    }
    
    // Продлеваем сессию (скользящий TTL)
    session.expires_at = now + SESSION_TTL;
    save_session(&session);
    
    for (uint32_t i = 0; i < s_sessions_len; i++) {
        if (strcmp(s_sessions[i].session_id, session_id) == 0) {
            return &s_sessions[i];
        }
    }
    return NULL;
}


void cleanup_expired_sessions(void) {
    ESP_LOGI(TAG, "Starting session cleanup...");


    /*
    DIR *dir = opendir(SESSION_DIR);
    if (!dir) {
        ESP_LOGE(TAG, "Cannot open sessions directory");
        return;
    }
    
    struct dirent *entry;
    time_t now = time(NULL);
    int deleted_count = 0;
    
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, "sess_") == entry->d_name) {
            char filepath[128];
            snprintf(filepath, sizeof(filepath), "%s/%s", SESSION_DIR, entry->d_name);
            
            // Загружаем сессию и проверяем expires_at
            session_t session;
            char session_id[33];
            strncpy(session_id, entry->d_name + 5, 32);
            session_id[32] = '\0';
            
            if (load_session(session_id, &session) == ESP_OK) {
                if (now > session.expires_at) {
                    remove(filepath);
                    deleted_count++;
                }
            } else {
                // Файл повреждён - удаляем
                remove(filepath);
                deleted_count++;
            }
        }
    }
    
    closedir(dir);*/
//    ESP_LOGI(TAG, "Cleanup finished. Deleted %d expired sessions", deleted_count);
}

void start_session_cleanup_timer(void) {
    const esp_timer_create_args_t timer_args = {
        .callback = &cleanup_expired_sessions_timer_cb,
        .name = "session_cleanup"
    };
    
    //esp_timer_handle_t timer;
    //esp_timer_create(&timer_args, &timer);
    // Запускаем каждые 15 минут
    //esp_timer_start_periodic(timer, 15 * 60 * 1000000);
}

void cleanup_expired_sessions_timer_cb(void *arg) {
    cleanup_expired_sessions();
}

