#include <cassert>
#include <esp_random.h>
#include <nvs.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/hash.h>

int main() {
  uint8_t buffer[34] = {};
  buffer[0] = buffer[33] = 0xa5;
  for (size_t size = 0; size <= 32; ++size) {
    esp_fill_random(buffer + 1, size);
    assert(buffer[0] == 0xa5 && buffer[33] == 0xa5);
  }
  nvs_handle_t handle;
  assert(nvs_open("devid", NVS_READWRITE, &handle) == ESP_OK);
  size_t size = 32;
  assert(nvs_get_blob(handle, "secret", buffer + 1, &size) == ESP_ERR_NVS_NOT_FOUND);
  assert(nvs_set_blob(handle, "secret", buffer + 1, size) == ESP_OK);
  assert(nvs_commit(handle) == ESP_OK);
  nvs_close(handle);

  Aes aes;
  assert(wc_AesInit(&aes, nullptr, INVALID_DEVID) != 0);
  assert(wc_AesGcmSetKey(&aes, buffer + 1, 32) != 0);
  assert(wc_AesGcmEncrypt(&aes, buffer + 1, buffer + 1, 16, buffer + 1, 12, buffer + 1, 16, nullptr, 0) != 0);
  assert(wc_AesGcmDecrypt(&aes, buffer + 1, buffer + 1, 16, buffer + 1, 12, buffer + 1, 16, nullptr, 0) != 0);
  assert(wc_Sha256Hash(buffer + 1, 32, buffer + 1) != 0);
  wc_AesFree(&aes);
}
