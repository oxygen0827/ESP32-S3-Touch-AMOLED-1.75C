#pragma once

// Clare Wi-Fi provisioning: an open setup hotspot + DNS captive portal that
// lets a phone configure the device's home Wi-Fi.  Inspired by the RV1106
// board's Wi-Fi provisioning UX: enter "WiFi Setup", the device opens a
// hotspot, the phone joins it and is dropped straight into a config page.
//
// The module reports through the same clare_net event callback as the rest
// of the transport; the password is never logged or emitted in events.
//
// Credential storage lives in the "clare_prov" NVS namespace ("ssid"/"pass")
// and overrides the Kconfig defaults on the next boot via
// clare_net_wifi_set_credentials().

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "clare_net.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register the event callback (same signature as clare_net).  Must be called
// before clare_prov_start(); idempotent.
esp_err_t clare_prov_init(const clare_net_config_t *config);

// Load credentials stored by a previous provisioning session.  Buffers are
// always NUL-initialised; returns ESP_ERR_NOT_FOUND when nothing is stored.
esp_err_t clare_prov_credentials_load(char *ssid, size_t ssid_len,
                                      char *pass, size_t pass_len);

// Open the setup hotspot and the captive portal.  Stops the STA radio
// first; calling it while a session is already active closes that session
// and opens a fresh one.  Emits PROV_STARTED (text = the hotspot SSID) once
// the portal is serving.
esp_err_t clare_prov_start(void);

// Close the hotspot and the portal.  Emits PROV_STOPPED.  Safe to call when
// not active.
esp_err_t clare_prov_stop(void);

bool clare_prov_is_active(void);

// Hotspot SSID of the active (or last) session, e.g. "Clare-S3-A1B2".
const char *clare_prov_ap_ssid(void);

#ifdef __cplusplus
}
#endif
