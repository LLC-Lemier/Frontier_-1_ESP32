#ifndef SNMP_AGENT_H
#define SNMP_AGENT_H

//--- global library
#include <string.h>

//--- RTOS library and Espressif ESP32 library
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"

//--- GPIO library
#include "driver/gpio.h"

//--- LwIP library
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"

//--- SMNP library (adapted) 
#include "port/lwipopts.h"

#include "lwip/apps/snmp_opts.h"
#include "lwip/snmp.h"
#include "lwip/apps/snmp.h"
#include "lwip/apps/snmp_core.h"
#include "lwip/apps/snmp_mib2.h"
#include "lwip/apps/snmp_scalar.h"

//-- private MIB library (included)
#include "my_mib.h"

//----- global definition vars (do not change it!)
#define ENABLE 1
#define DISABLE 0

#define true 1
#define false 0

#define TRAP_DESTINATION_INDEX 0
//----- ^^^

/* transport information to my_mib.c */
extern const struct snmp_mib gpio_mib;
static void initialize_snmp(void);

#endif //SNMP_AGENT_H