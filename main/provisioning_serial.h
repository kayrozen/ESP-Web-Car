#pragma once
#include <stdbool.h>

/* Wait up to 30 s on UART0 for a PROVISION:{...}\n line.
 * Validates the payload, writes device_name + playlist_json to NVS, replies OK\n.
 * Returns false silently on timeout; prints ERR:<reason>\n on bad input.
 * Must be called before WiFi/BT init. */
bool provisioning_serial_wait(void);
