// PSA Crypto backend for CPYOS protected EPUBs on the Metalio Plus port.
//
// The reference firmware links wolfSSL; IDF 6.1 ships PSA (tf-psa-crypto) and
// no wolfSSL. The protected-book read path only needs AES-128-CBC plus
// SHA-1/SHA-256, all of which PSA provides. The RSA/PKCS#12 helpers belong to
// the legacy off-device rights scheme and are refused rather than faked.

#include "PortContentCrypto.h"

#include <esp_random.h>
#include <psa/crypto.h>

#include <cstring>

#include <esp_log.h>
#include <vector>

namespace freeink {
namespace content {

namespace {

bool psaReady() {
    static const bool ready = psa_crypto_init() == PSA_SUCCESS;
    return ready;
}

class Psacrypto final : public Crypto {
 public:
    int32_t rsaPrivateRaw(const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*, size_t) override {
        return -1;
    }

    bool aes128CbcDecrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t len,
                          uint8_t* out) override {
        if (!psaReady() || in == nullptr || out == nullptr || len == 0 || (len % 16) != 0) {
            return false;
        }
        mbedtls_svc_key_id_t keyId = MBEDTLS_SVC_KEY_ID_INIT;
        if (!import(keyId, key, PSA_KEY_USAGE_DECRYPT, PSA_ALG_CBC_NO_PADDING)) {
            return false;
        }
        size_t outLen = 0;
        const bool ok = cbc(false, keyId, iv, in, len, out, len, &outLen) && outLen == len;
        psa_destroy_key(keyId);
        return ok;
    }

    void sha1(const uint8_t* data, size_t len, uint8_t out[20]) override {
        hash(PSA_ALG_SHA_1, data, len, out, 20);
    }

    void sha256(const uint8_t* data, size_t len, uint8_t out[32]) override {
        hash(PSA_ALG_SHA_256, data, len, out, 32);
    }

    bool rsaGenerate(RsaKeyPairDer*) override { return false; }

    bool rsaPublicEncrypt(const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*, size_t,
                          size_t*) override {
        return false;
    }

    bool rsaPrivateSignRaw(const uint8_t*, size_t, const uint8_t[20], uint8_t[128]) override {
        return false;
    }

    bool aes128CbcEncrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t len,
                          uint8_t* out) override {
        if (!psaReady() || in == nullptr || out == nullptr) {
            return false;
        }
        const size_t padded = (len / 16 + 1) * 16;
        std::vector<uint8_t> plain(padded);
        std::memcpy(plain.data(), in, len);
        std::memset(plain.data() + len, static_cast<int>(padded - len), padded - len);

        mbedtls_svc_key_id_t keyId = MBEDTLS_SVC_KEY_ID_INIT;
        if (!import(keyId, key, PSA_KEY_USAGE_ENCRYPT, PSA_ALG_CBC_NO_PADDING)) {
            return false;
        }
        size_t outLen = 0;
        const bool ok = cbc(true, keyId, iv, plain.data(), plain.size(), out, padded, &outLen) && outLen == padded;
        psa_destroy_key(keyId);
        return ok;
    }

    bool pkcs12Extract(const uint8_t*, size_t, const std::string&, std::vector<uint8_t>*,
                       std::vector<uint8_t>*) override {
        return false;
    }

    void randomBytes(uint8_t* out, size_t len) override {
        if (out != nullptr && len > 0) {
            esp_fill_random(out, len);
        }
    }

 private:
    static bool import(mbedtls_svc_key_id_t& keyId, const uint8_t key[16], psa_key_usage_t usage,
                       psa_algorithm_t algorithm) {
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
        psa_set_key_bits(&attributes, 128);
        psa_set_key_usage_flags(&attributes, usage);
        psa_set_key_algorithm(&attributes, algorithm);
        return psa_import_key(&attributes, key, 16, &keyId) == PSA_SUCCESS;
    }

    // PSA's one-shot cipher calls take no IV; the multipart API is the only
    // way to use an explicit one.
    static bool cbc(bool encrypt, mbedtls_svc_key_id_t keyId, const uint8_t iv[16], const uint8_t* in,
                    size_t len, uint8_t* out, size_t outCap, size_t* outLen) {
        psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
        psa_status_t status = encrypt ? psa_cipher_encrypt_setup(&op, keyId, PSA_ALG_CBC_NO_PADDING)
                                      : psa_cipher_decrypt_setup(&op, keyId, PSA_ALG_CBC_NO_PADDING);
        size_t updateLen = 0;
        size_t finishLen = 0;
        if (status == PSA_SUCCESS) {
            status = psa_cipher_set_iv(&op, iv, 16);
        }
        if (status == PSA_SUCCESS) {
            status = psa_cipher_update(&op, in, len, out, outCap, &updateLen);
        }
        if (status == PSA_SUCCESS) {
            status = psa_cipher_finish(&op, out + updateLen, outCap - updateLen, &finishLen);
        }
        psa_cipher_abort(&op);
        if (status != PSA_SUCCESS) {
            return false;
        }
        *outLen = updateLen + finishLen;
        return true;
    }

    static void hash(psa_algorithm_t algorithm, const uint8_t* data, size_t len, uint8_t* out,
                     size_t outLen) {
        size_t written = 0;
        if (!psaReady() || psa_hash_compute(algorithm, data, len, out, outLen, &written) != PSA_SUCCESS ||
            written != outLen) {
            std::memset(out, 0, outLen);
        }
    }
};

}  // namespace

Crypto& portContentCrypto() {
    static Psacrypto crypto;
    return crypto;
}

bool portContentCryptoSelfTest() {
    static const uint8_t kPlain[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                       0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    static const uint8_t kKey[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                     0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
    static const uint8_t kIv[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    // SHA-256("abc")
    static const uint8_t kSha256Abc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};

    Crypto& crypto = portContentCrypto();
    uint8_t cipher[32] = {};
    uint8_t plain[32] = {};
    const bool enc = crypto.aes128CbcEncrypt(kKey, kIv, kPlain, sizeof(kPlain), cipher);
    const bool dec = enc && crypto.aes128CbcDecrypt(kKey, kIv, cipher, 32, plain);
    const bool cbcOk = dec && std::memcmp(plain, kPlain, sizeof(kPlain)) == 0;

    uint8_t digest[32] = {};
    crypto.sha256(reinterpret_cast<const uint8_t*>("abc"), 3, digest);
    const bool shaOk = std::memcmp(digest, kSha256Abc, sizeof(kSha256Abc)) == 0;

    if (!cbcOk || !shaOk) {
        ESP_LOGE("CPRO", "PSA self-test failed (cbc=%d sha256=%d)", cbcOk ? 1 : 0, shaOk ? 1 : 0);
        return false;
    }
    ESP_LOGI("CPRO", "PSA self-test ok (AES-128-CBC + SHA-256)");
    return true;
}

}  // namespace content
}  // namespace freeink
