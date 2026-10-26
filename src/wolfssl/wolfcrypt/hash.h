#pragma once

#include "aes.h"

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif

// Host SHA-256, as in WolfsslCrypto.h. Protected-book keys still fail at the
// AES stubs in aes.h.
inline int wc_Sha256Hash(const byte *data, word32 len, byte *out) {
#ifdef __APPLE__
  CC_SHA256(data, static_cast<CC_LONG>(len), out);
#else
  SHA256(data, len, out);
#endif
  return 0;
}
