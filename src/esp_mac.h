#pragma once

#include "esp_err.h"

#include <cstdint>
#include <cstring>

typedef enum {
  ESP_MAC_WIFI_STA,
  ESP_MAC_WIFI_SOFTAP,
  ESP_MAC_BT,
  ESP_MAC_ETH,
  ESP_MAC_IEEE802154,
  ESP_MAC_BASE,
  ESP_MAC_EFUSE_FACTORY,
  ESP_MAC_EFUSE_CUSTOM,
  ESP_MAC_EFUSE_EXT,
} esp_mac_type_t;

static const uint8_t SIMULATOR_MAC[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};

// Simulator stub: return a fixed fake MAC address
static inline esp_err_t esp_efuse_mac_get_default(uint8_t mac[6]) {
  if (!mac) return ESP_ERR_INVALID_ARG;
  memcpy(mac, SIMULATOR_MAC, sizeof(SIMULATOR_MAC));
  return ESP_OK;
}

static inline esp_err_t esp_read_mac(uint8_t mac[6], esp_mac_type_t type) {
  if (type != ESP_MAC_WIFI_STA) return ESP_ERR_INVALID_ARG;
  return esp_efuse_mac_get_default(mac);
}
