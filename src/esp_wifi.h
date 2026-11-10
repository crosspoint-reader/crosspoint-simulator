#pragma once

#include <cstdint>
#include "esp_err.h"

enum wifi_ps_type_t {
  WIFI_PS_NONE = 0,
  WIFI_PS_MIN_MODEM = 1,
  WIFI_PS_MAX_MODEM = 2,
};

typedef enum {
  WIFI_IF_STA = 0,
  WIFI_IF_AP = 1,
} wifi_interface_t;

#define WIFI_PROTOCOL_11B 1
#define WIFI_PROTOCOL_11G 2
#define WIFI_PROTOCOL_11N 4
#define WIFI_PROTOCOL_11AX 8

inline esp_err_t esp_wifi_set_ps(wifi_ps_type_t) { return ESP_OK; }
inline esp_err_t esp_wifi_set_max_tx_power(int8_t) { return ESP_OK; }
inline esp_err_t esp_wifi_set_protocol(wifi_interface_t, uint8_t) { return ESP_OK; }
inline esp_err_t esp_wifi_get_ps(wifi_ps_type_t *ps) {
  if (!ps) return ESP_ERR_INVALID_ARG;
  *ps = WIFI_PS_NONE;
  return ESP_OK;
}
inline esp_err_t esp_wifi_stop() { return ESP_OK; }
