#pragma once

// IDF 6.1's mbedTLS 4.x moved the SHA-256 API into the PSA crypto private
// tree. The context type is visible there, but the legacy function
// declarations are hidden unless the (internally enabled) SHA256 config
// symbol is visible to every consumer, so they are re-declared here. The
// implementations live in libtfpsacrypto and are linked through esp-tls.

#include <cstddef>

#include <mbedtls/private/sha256.h>

#ifdef __cplusplus
extern "C" {
#endif

void mbedtls_sha256_init(mbedtls_sha256_context *ctx);
void mbedtls_sha256_free(mbedtls_sha256_context *ctx);
int mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224);
int mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *input, size_t ilen);
int mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char output[32]);

#ifdef __cplusplus
}
#endif
