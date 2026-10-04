#pragma once

#include <cstdint>

using byte = uint8_t;
using word32 = uint32_t;
constexpr int INVALID_DEVID = -2;
struct Aes {};
constexpr int AES_ENCRYPTION = 0;
inline int wc_AesSetKey(Aes *, const byte *, word32, const byte *, int) { return -1; }
inline int wc_AesCbcEncrypt(Aes *, byte *, const byte *, word32) { return -1; }

// Protected-book crypto is unsupported. Never report successful encryption or authentication.
inline int wc_AesInit(Aes *, void *, int) { return -1; }
inline void wc_AesFree(Aes *) {}
inline int wc_AesGcmSetKey(Aes *, const byte *, word32) { return -1; }
inline int wc_AesGcmEncrypt(Aes *, byte *, const byte *, word32, const byte *, word32, byte *, word32,
                            const byte *, word32) {
  return -1;
}
inline int wc_AesGcmDecrypt(Aes *, byte *, const byte *, word32, const byte *, word32, const byte *, word32,
                            const byte *, word32) {
  return -1;
}
