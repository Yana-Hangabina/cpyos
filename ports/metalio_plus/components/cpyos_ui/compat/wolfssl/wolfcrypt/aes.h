#pragma once

// wolfCrypt AES-compatible API for the Metalio Plus port, implemented on
// mbedTLS/PSA (IDF 6.1 ships mbedTLS 4.x, whose legacy AES/GCM headers are no
// longer public). Only the calls the CPYOS book engine makes are provided:
// AES-GCM encrypt/decrypt with optional AAD, byte-for-byte compatible with
// wolfSSL's wc_AesGcm* because AES-GCM is a standard construction.

#include <cstddef>
#include <cstdint>

#define INVALID_DEVID (-2)

typedef std::uint8_t byte;
typedef std::uint32_t word32;

struct Aes {
    std::uint8_t key[32];
    std::size_t keyLen;
    int devId;
};

int wc_AesInit(Aes *aes, void *heap, int devId);
void wc_AesFree(Aes *aes);
int wc_AesGcmSetKey(Aes *aes, const byte *key, word32 len);
int wc_AesGcmEncrypt(Aes *aes, byte *out, const byte *in, word32 sz, const byte *iv, word32 ivSz,
                     byte *authTag, word32 authTagSz, const byte *authIn, word32 authInSz);
int wc_AesGcmDecrypt(Aes *aes, byte *out, const byte *in, word32 sz, const byte *iv, word32 ivSz,
                     const byte *authTag, word32 authTagSz, const byte *authIn, word32 authInSz);
