#pragma once

// wolfCrypt SHA-256-compatible API for the Metalio Plus port (PSA-backed).

#include <cstddef>
#include <cstdint>

typedef std::uint8_t byte;
typedef std::uint32_t word32;

#define WC_SHA256_DIGEST_SIZE 32

int wc_Sha256Hash(const byte *data, word32 len, byte *hash);
