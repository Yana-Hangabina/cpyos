#include "MD5Builder.h"

#include <cstring>

namespace {

constexpr uint32_t kShift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

constexpr uint32_t kTable[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

inline uint32_t rol(uint32_t value, uint32_t count) { return (value << count) | (value >> (32 - count)); }

void transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = static_cast<uint32_t>(block[i * 4]) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t f = 0;
        uint32_t g = 0;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rol(a + f + kTable[i] + m[g], kShift[i]);
        a = tmp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

}  // namespace

void MD5Builder::begin() {
    state_[0] = 0x67452301;
    state_[1] = 0xefcdab89;
    state_[2] = 0x98badcfe;
    state_[3] = 0x10325476;
    length_ = 0;
    blockLen_ = 0;
    std::memset(block_, 0, sizeof(block_));
    std::memset(digest_, 0, sizeof(digest_));
    started_ = true;
    finished_ = false;
}

void MD5Builder::add(const uint8_t *data, uint16_t len) {
    if (!started_) {
        begin();
    }
    if (data == nullptr || len == 0) {
        return;
    }
    length_ += len;
    size_t offset = 0;
    while (offset < len) {
        const size_t take = (sizeof(block_) - blockLen_) < (len - offset)
                                ? (sizeof(block_) - blockLen_)
                                : (len - offset);
        std::memcpy(block_ + blockLen_, data + offset, take);
        blockLen_ += take;
        offset += take;
        if (blockLen_ == sizeof(block_)) {
            transform(state_, block_);
            blockLen_ = 0;
        }
    }
}

void MD5Builder::add(const char *text) {
    if (text == nullptr) {
        return;
    }
    add(reinterpret_cast<const uint8_t *>(text), static_cast<uint16_t>(std::strlen(text)));
}

void MD5Builder::calculate() {
    if (!started_ || finished_) {
        return;
    }
    // Capture the message length before padding advances length_.
    const uint64_t bitLength = length_ * 8;
    uint8_t padding[72] = {0x80};
    const size_t padLen = (blockLen_ < 56) ? (56 - blockLen_) : (120 - blockLen_);
    add(padding, static_cast<uint16_t>(padLen));
    uint8_t lengthBytes[8];
    for (int i = 0; i < 8; ++i) {
        lengthBytes[i] = static_cast<uint8_t>(bitLength >> (8 * i));
    }
    add(lengthBytes, 8);

    for (int i = 0; i < 4; ++i) {
        digest_[i * 4] = static_cast<uint8_t>(state_[i]);
        digest_[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 8);
        digest_[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 16);
        digest_[i * 4 + 3] = static_cast<uint8_t>(state_[i] >> 24);
    }
    finished_ = true;
}

void MD5Builder::getBytes(uint8_t *out) const {
    if (out != nullptr) {
        std::memcpy(out, digest_, sizeof(digest_));
    }
}

String MD5Builder::toString() const {
    static constexpr char kHex[] = "0123456789abcdef";
    char text[33];
    for (size_t i = 0; i < sizeof(digest_); ++i) {
        text[i * 2] = kHex[digest_[i] >> 4];
        text[i * 2 + 1] = kHex[digest_[i] & 0x0F];
    }
    text[32] = '\0';
    return String(text);
}
