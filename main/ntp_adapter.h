#pragma once

#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include "esp_sntp.h"
#include "esp_log.h"

#define NTP_SERVER_NONE 0
#define NTP_SERVER_DOMAIN_NAME 1
#define NTP_SERVER_IP_ADDRESS 2

typedef struct
{
    uint8_t server_type[5];
    uint8_t* lengths[5]; 
    char* server[5];
} ntp_config_t;


void init_ntp(ntp_config_t *config);
void start_ntp_sync_task(void);