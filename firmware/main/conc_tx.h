/* SX1301 transmit path, run inside the radio task (single HAL owner). */
#pragma once

#include <stdbool.h>

/** TX config for lgw_* setconf: call before lgw_start. Returns 0 on success. */
int conc_tx_configure(void);

/** After lgw_start: switch the counter to free-running and check it. */
int conc_tx_after_start(void);

/** Admit queued requests and send frames that are due. Returns false when the
 *  concentrator looks reset (counter discontinuity): restart the HAL. */
bool conc_tx_service(void);
