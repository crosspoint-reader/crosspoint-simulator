#pragma once

#include <cstddef>
#include <cstring>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
using sim_sha256_context = CC_SHA256_CTX;
#elif defined(__linux__)
#include <openssl/sha.h>
using sim_sha256_context = SHA256_CTX;
#else
#error "Unsupported host OS for simulator SHA-256"
#endif

struct mbedtls_sha256_context {
  sim_sha256_context native;
};

inline void mbedtls_sha256_init(mbedtls_sha256_context *ctx) {
  if (ctx)
    std::memset(ctx, 0, sizeof(*ctx));
}

inline int mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224) {
  if (!ctx || is224)
    return -1;
#if defined(__APPLE__)
  return CC_SHA256_Init(&ctx->native) == 1 ? 0 : -1;
#else
  return SHA256_Init(&ctx->native) == 1 ? 0 : -1;
#endif
}

inline int mbedtls_sha256_update(mbedtls_sha256_context *ctx,
                                 const unsigned char *input, size_t len) {
  if (!ctx || (!input && len))
    return -1;
#if defined(__APPLE__)
  return CC_SHA256_Update(&ctx->native, input, static_cast<CC_LONG>(len)) == 1
             ? 0
             : -1;
#else
  return SHA256_Update(&ctx->native, input, len) == 1 ? 0 : -1;
#endif
}

inline int mbedtls_sha256_finish(mbedtls_sha256_context *ctx,
                                 unsigned char output[32]) {
  if (!ctx || !output)
    return -1;
#if defined(__APPLE__)
  return CC_SHA256_Final(output, &ctx->native) == 1 ? 0 : -1;
#else
  return SHA256_Final(output, &ctx->native) == 1 ? 0 : -1;
#endif
}

inline void mbedtls_sha256_free(mbedtls_sha256_context *ctx) {
  if (ctx)
    std::memset(ctx, 0, sizeof(*ctx));
}
