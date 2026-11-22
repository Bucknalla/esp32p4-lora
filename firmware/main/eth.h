/* Ethernet bring-up: internal EMAC + IP101 PHY over RMII, DHCP client. */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** Install the driver, attach esp_netif and start. Needs esp_netif_init() and
 *  the default event loop first. */
esp_err_t eth_start(void);

bool eth_link_up(void);
bool eth_has_ip(void);
