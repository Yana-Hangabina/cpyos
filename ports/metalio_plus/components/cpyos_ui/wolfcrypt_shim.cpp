// wolfCrypt compatibility shim for the Metalio Plus port.
//
// IDF 6.1 ships mbedTLS 4.x, whose legacy AES/GCM/SHA headers moved out of the
// public include path. The CPYOS book engine only needs AES-GCM and SHA-256, so
// this shim implements the exact wolfSSL calls it makes on top of PSA Crypto.
// The wire format is identical (AES-GCM with a 16-byte tag; SHA-256), which is
// what the on-device key file and content protection depend on.

#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/hash.h>

#include <cstdlib>
#include <cstring>

#include <psa/crypto.h>

namespace {

bool psaReady() {
    static const bool ready = psa_crypto_init() == PSA_SUCCESS;
    return ready;
}

// IDF 6.1's PSA variant takes an imported key id rather than raw key bytes.
bool importAes(const Aes* aes, mbedtls_svc_key_id_t* key) {
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, aes->keyLen * 8);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    return psa_import_key(&attributes, aes->key, aes->keyLen, key) == PSA_SUCCESS;
}

}  // namespace

int wc_AesInit(Aes *aes, void *, int devId) {
    if (aes == nullptr) {
        return -1;
    }
    std::memset(aes, 0, sizeof(*aes));
    aes->devId = devId;
    return psaReady() ? 0 : -1;
}

void wc_AesFree(Aes *aes) {
    if (aes != nullptr) {
        std::memset(aes, 0, sizeof(*aes));
    }
}

int wc_AesGcmSetKey(Aes *aes, const byte *key, word32 len) {
    if (aes == nullptr || key == nullptr || (len != 16 && len != 24 && len != 32)) {
        return -1;
    }
    std::memcpy(aes->key, key, len);
    aes->keyLen = len;
    return 0;
}

int wc_AesGcmEncrypt(Aes *aes, byte *out, const byte *in, word32 sz, const byte *iv, word32 ivSz,
                     byte *authTag, word32 authTagSz, const byte *authIn, word32 authInSz) {
    if (aes == nullptr || out == nullptr || in == nullptr || iv == nullptr || authTag == nullptr) {
        return -1;
    }
    if (authTagSz != 16) {
        return -1;
    }
    // PSA appends the tag to the ciphertext; wolfSSL keeps them separate.
    byte *buffer = static_cast<byte *>(std::malloc(sz + authTagSz));
    if (buffer == nullptr) {
        return -1;
    }
    mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
    size_t outLen = 0;
    psa_status_t status = PSA_ERROR_GENERIC_ERROR;
    if (importAes(aes, &key)) {
        status = psa_aead_encrypt(key, PSA_ALG_GCM, iv, ivSz, authIn, authInSz, in, sz, buffer,
                                  sz + authTagSz, &outLen);
        psa_destroy_key(key);
    }
    int result = -1;
    if (status == PSA_SUCCESS && outLen == sz + authTagSz) {
        std::memcpy(out, buffer, sz);
        std::memcpy(authTag, buffer + sz, authTagSz);
        result = 0;
    }
    std::free(buffer);
    return result;
}

int wc_AesGcmDecrypt(Aes *aes, byte *out, const byte *in, word32 sz, const byte *iv, word32 ivSz,
                     const byte *authTag, word32 authTagSz, const byte *authIn, word32 authInSz) {
    if (aes == nullptr || out == nullptr || in == nullptr || iv == nullptr || authTag == nullptr) {
        return -1;
    }
    if (authTagSz != 16) {
        return -1;
    }
    byte *buffer = static_cast<byte *>(std::malloc(sz + authTagSz));
    if (buffer == nullptr) {
        return -1;
    }
    std::memcpy(buffer, in, sz);
    std::memcpy(buffer + sz, authTag, authTagSz);
    mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
    size_t outLen = 0;
    psa_status_t status = PSA_ERROR_GENERIC_ERROR;
    if (importAes(aes, &key)) {
        status = psa_aead_decrypt(key, PSA_ALG_GCM, iv, ivSz, authIn, authInSz, buffer, sz + authTagSz,
                                  out, sz, &outLen);
        psa_destroy_key(key);
    }
    std::free(buffer);
    return (status == PSA_SUCCESS && outLen == sz) ? 0 : -1;
}

int wc_Sha256Hash(const byte *data, word32 len, byte *hash) {
    if (hash == nullptr) {
        return -1;
    }
    size_t outLen = 0;
    const psa_status_t status =
        psa_hash_compute(PSA_ALG_SHA_256, data, len, hash, WC_SHA256_DIGEST_SIZE, &outLen);
    return (status == PSA_SUCCESS && outLen == WC_SHA256_DIGEST_SIZE) ? 0 : -1;
}
