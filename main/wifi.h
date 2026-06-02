#pragma once
#ifndef WIFI_H
#define WIFI_H

#include "esp_err.h"
#include <stdbool.h>

esp_err_t wifi_start_softap(void);
esp_err_t wifi_connect_sta(void);
void      wifi_stop(void);
bool      wifi_is_connected(void);
esp_err_t wifi_mdns_init(void);

#endif
