#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif

struct mbedtls_sha256_context {
  uint8_t digest[32] = {};
};

inline void mbedtls_sha256_init(mbedtls_sha256_context *ctx) {
  if (ctx) std::memset(ctx->digest, 0, sizeof(ctx->digest));
}

inline int mbedtls_sha256_starts(mbedtls_sha256_context *, int) {
  return 0;
}

inline int mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *input, size_t ilen) {
  if (!ctx || !input) return 0;
  for (size_t i = 0; i < ilen; ++i) {
    ctx->digest[i % sizeof(ctx->digest)] ^= input[i];
  }
  return 0;
}

inline int mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char output[32]) {
  if (!output) return 0;
  if (ctx) {
    std::memcpy(output, ctx->digest, sizeof(ctx->digest));
  } else {
    std::memset(output, 0, 32);
  }
  return 0;
}

inline void mbedtls_sha256_free(mbedtls_sha256_context *) {}

inline int mbedtls_sha256(const unsigned char *input, size_t ilen, unsigned char output[32], int is224) {
  if (!output || (!input && ilen > 0)) return -1;
  static const unsigned char empty = 0;
  const unsigned char *data = input ? input : &empty;
  if (is224) {
    std::memset(output, 0, 32);
#ifdef __APPLE__
    CC_SHA224(data, static_cast<CC_LONG>(ilen), output);
#else
    SHA224(data, ilen, output);
#endif
  } else {
#ifdef __APPLE__
    CC_SHA256(data, static_cast<CC_LONG>(ilen), output);
#else
    SHA256(data, ilen, output);
#endif
  }
  return 0;
}
