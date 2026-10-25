#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "SimulatorStorageRoot.h"
#include "esp_err.h"

using nvs_handle_t = uint32_t;

constexpr int NVS_READONLY = 0;
constexpr int NVS_READWRITE = 1;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
constexpr esp_err_t ESP_ERR_NVS_INVALID_LENGTH = 0x110c;

// NVS is flash, so values survive a restart. Each value is a file in
// <SD folder>/.simulator-nvs/<namespace>/<key>, so a fresh SD folder is also
// a fresh device.
namespace sim_nvs {
inline std::mutex mutex;
inline std::vector<std::string> namespaces;  // index is handle - 1

inline std::string namespaceDir(const std::string &name) {
  return crosspoint_simulator::storageRoot() + "/.simulator-nvs/" + name;
}

inline bool valuePath(nvs_handle_t handle, const char *key, std::string &path) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!key || handle == 0 || handle > namespaces.size()) return false;
  path = namespaceDir(namespaces[handle - 1]) + "/" + key;
  return true;
}

inline esp_err_t read(nvs_handle_t handle, const char *key, std::string &value) {
  std::string path;
  if (!valuePath(handle, key, path)) return ESP_FAIL;
  FILE *file = std::fopen(path.c_str(), "rb");
  if (!file) return ESP_ERR_NVS_NOT_FOUND;
  value.clear();
  char buffer[256];
  size_t got;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) value.append(buffer, got);
  std::fclose(file);
  return ESP_OK;
}

inline esp_err_t write(nvs_handle_t handle, const char *key, const void *data, size_t length) {
  std::string path;
  if (!valuePath(handle, key, path) || (length && !data)) return ESP_FAIL;
  const std::string dir = path.substr(0, path.rfind('/'));
  ::mkdir(dir.substr(0, dir.rfind('/')).c_str(), 0755);
  ::mkdir(dir.c_str(), 0755);
  // Replace the old value only once the new one is written.
  const std::string staged = path + ".tmp";
  FILE *file = std::fopen(staged.c_str(), "wb");
  if (!file) return ESP_FAIL;
  const bool written = std::fwrite(data, 1, length, file) == length;
  if (std::fclose(file) != 0 || !written || std::rename(staged.c_str(), path.c_str()) != 0) {
    std::remove(staged.c_str());
    return ESP_FAIL;
  }
  return ESP_OK;
}
} // namespace sim_nvs

inline esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) {
  if (!name || !handle) return ESP_FAIL;
  struct stat info{};
  if (mode == NVS_READONLY && ::stat(sim_nvs::namespaceDir(name).c_str(), &info) != 0) {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  std::lock_guard<std::mutex> lock(sim_nvs::mutex);
  sim_nvs::namespaces.emplace_back(name);
  *handle = static_cast<nvs_handle_t>(sim_nvs::namespaces.size());
  return ESP_OK;
}

inline esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value) {
  std::string stored;
  const esp_err_t err = sim_nvs::read(handle, key, stored);
  if (err != ESP_OK) return err;
  if (stored.size() != 1 || !value) return ESP_FAIL;
  *value = static_cast<uint8_t>(stored[0]);
  return ESP_OK;
}

inline esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value) {
  return sim_nvs::write(handle, key, &value, 1);
}

inline esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *length) {
  if (!length) return ESP_FAIL;
  std::string stored;
  const esp_err_t err = sim_nvs::read(handle, key, stored);
  if (err != ESP_OK) return err;
  if (out) {
    if (*length < stored.size()) return ESP_ERR_NVS_INVALID_LENGTH;
    stored.copy(static_cast<char *>(out), stored.size());
  }
  *length = stored.size();
  return ESP_OK;
}

inline esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length) {
  return sim_nvs::write(handle, key, value, length);
}

// Values are written immediately.
inline esp_err_t nvs_commit(nvs_handle_t /*handle*/) { return ESP_OK; }
inline void nvs_close(nvs_handle_t /*handle*/) {}
