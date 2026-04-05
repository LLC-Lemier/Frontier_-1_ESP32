#include "simple_snmp_agent.h"
/* library for static IP */
#include "lwip/sockets.h"

/*
 * TODO: Debug gpio mib status
 * Gives information of atual gpio status in terminal. 1 or true to on, 0 or false to off.
*/
#define DEBUG_GPIO_STATUS 1

/*
 * TODO: Setup SNMP server
 * Define a specific address to send SNMP broadcast package.
*/
#define SNMP_SERVER_IP "172.16.0.250"

static const char *TAG = "simple_snmp_agent.c";

//function prototypes in this file

/* 
 * ----- TODO: Global variables for SNMP Trap vvv
 * Define your own vars SNMP_SYSDESCR for System Description, SNMP_SYSCONTACT 
 * for your contact mail, SNMP_SYSNAME for your system name, SNMP_SYSLOCATION
 * for your location. Also consider the size of each string in _LEN functions.
*/
static const struct snmp_mib *my_snmp_mibs[] = { &mib2, &gpio_mib };
//1.3.6.1.2.1.1.1.0
const u8_t * SNMP_SYSDESCR = (u8_t*) "simple_snmp_agent";		
const u16_t SNMP_SYSDESCR_LEN = sizeof("simple_snmp_agent");
//1.3.6.1.2.1.1.4.0
u8_t * SNMP_SYSCONTACT = (u8_t*) "yourmail@contact.com";		
u16_t SNMP_SYSCONTACT_LEN = sizeof("yourmail@contact.com");
//1.3.6.1.2.1.1.5.0
u8_t * SNMP_SYSNAME = (u8_t*) "ESP32_Core_board_V2";							
u16_t SNMP_SYSNAME_LEN = sizeof("ESP32_Core_board_V2");
//1.3.6.1.2.1.1.6.0
u8_t * SNMP_SYSLOCATION = (u8_t*) "Your Institute or Company"; 		
u16_t SNMP_SYSLOCATION_LEN = sizeof("Your Institute or Company");
/* 
 * ----- TODO: Global variables for SNMP Trap ^^^
*/

/* buffer for snmp service */
u16_t snmp_buffer = 64;

/* relation between mib node and gpio, exist in my_gpio.h */
u32_t *leds, *switches, *xgpio;


/**
 * Should be called at the beginning of the program to set up the snmp agent.
 *
 * @note You can look updated instructions in the link below
 * 		 http://www.nongnu.org/lwip/2_0_x/group__snmp.html
 */
static void initialize_snmp(void)
{	
	snmp_mib2_set_syscontact(SNMP_SYSCONTACT, &SNMP_SYSCONTACT_LEN, snmp_buffer);
	snmp_mib2_set_syslocation(SNMP_SYSLOCATION, &SNMP_SYSLOCATION_LEN, snmp_buffer);
	snmp_set_auth_traps_enabled(ENABLE);
	snmp_mib2_set_sysdescr(SNMP_SYSDESCR, &SNMP_SYSDESCR_LEN);
	snmp_mib2_set_sysname(SNMP_SYSNAME, &SNMP_SYSNAME_LEN, snmp_buffer);
	
	ip_addr_t gw = { 0 };
    ipaddr_aton(SNMP_SERVER_IP,&gw);
	
	snmp_trap_dst_ip_set(TRAP_DESTINATION_INDEX, &gw);
	snmp_trap_dst_enable(TRAP_DESTINATION_INDEX, ENABLE);
	snmp_set_mibs(my_snmp_mibs, LWIP_ARRAYSIZE(my_snmp_mibs));
	
    snmp_init();
	ESP_LOGI(TAG, "initialize_snmp() finished.");
	
}
