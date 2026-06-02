#pragma once
#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include "esp_err.h"
#include <stddef.h>

#define DEVICE_ID_LEN   37   /* "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx\0" */
#define API_KEY_LEN     65   /* 64 hex chars + '\0' */
#define TM_SALT_LEN     33   /* 32 hex chars + '\0' */

/**
 * @brief Load or generate device identity from NVS.
 *        No WiFi needed. Call after storage_init().
 */
esp_err_t device_identity_init(void);

/**
 * @brief POST /api/v1/register with stored identity.
 *        Requires WiFi. Idempotent.
 *        Generates a new identity and re-registers if server returns 404.
 */
esp_err_t device_identity_register(void);

void device_identity_get_id(char *buf, size_t len);
void device_identity_get_api_key(char *buf, size_t len);
void device_identity_get_salt(char *buf, size_t len);

#endif /* DEVICE_IDENTITY_H */
