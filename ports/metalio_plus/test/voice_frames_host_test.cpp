#include <cstdio>
#include <cstring>

#include "plus_voice_frames.h"

namespace {
int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}
}  // namespace

int main() {
    using namespace plus_voice;
    uint8_t buf[32] = {};

    // v1 has no header: the whole frame is Opus.
    {
        const uint8_t payload[3] = {1, 2, 3};
        const uint8_t* out = nullptr;
        size_t len = 0;
        expect(buildAudioHeader(1, kTypeOpus, 0, 3, buf, sizeof(buf)) == 0, "v1 has no header");
        expect(parseAudioHeader(1, payload, sizeof(payload), &out, &len), "v1 parses");
        expect(out == payload && len == 3, "v1 payload is the whole frame");
    }

    // v2 header: version/type/reserved/timestamp/size, big-endian.
    {
        const size_t n = buildAudioHeader(2, kTypeOpus, 0x01020304, 0x0506, buf, sizeof(buf));
        expect(n == 16, "v2 header is 16 bytes");
        expect(readBe16(buf) == 2 && readBe16(buf + 2) == kTypeOpus, "v2 version/type");
        expect(readBe32(buf + 4) == 0, "v2 reserved");
        expect(readBe32(buf + 8) == 0x01020304, "v2 timestamp");
        expect(readBe32(buf + 12) == 0x0506, "v2 payload size");
        const uint8_t* out = nullptr;
        size_t len = 0;
        uint8_t frame[16 + 4] = {};
        std::memcpy(frame, buf, 16);
        expect(parseAudioHeader(2, frame, sizeof(frame), &out, &len), "v2 parses");
        expect(out == frame + 16 && len == 4, "v2 payload starts after 16 bytes");
        // Declared size larger than what arrived must not read past the buffer.
        writeBe32(frame + 12, 100);
        expect(parseAudioHeader(2, frame, 18, &out, &len) && len == 2, "v2 clamps to received bytes");
        // Non-Opus binary type is rejected.
        writeBe16(frame + 2, 1);
        expect(!parseAudioHeader(2, frame, sizeof(frame), &out, &len), "v2 rejects JSON type");
        expect(buildAudioHeader(2, kTypeOpus, 0, 0, buf, 8) == 0, "v2 needs 16 bytes");
    }

    // v3 header: 4 bytes, 16-bit size.
    {
        const size_t n = buildAudioHeader(3, kTypeOpus, 0, 0x1234, buf, sizeof(buf));
        expect(n == 4, "v3 header is 4 bytes");
        expect(buf[0] == kTypeOpus && buf[1] == 0, "v3 type/reserved");
        expect(readBe16(buf + 2) == 0x1234, "v3 size");
        uint8_t frame[4 + 2] = {kTypeOpus, 0, 0, 2, 0xAA, 0xBB};
        const uint8_t* out = nullptr;
        size_t len = 0;
        expect(parseAudioHeader(3, frame, sizeof(frame), &out, &len), "v3 parses");
        expect(out == frame + 4 && len == 2, "v3 payload after 4 bytes");
        expect(buildAudioHeader(3, kTypeOpus, 0, 0x10000, buf, sizeof(buf)) == 0, "v3 rejects >16-bit size");
        expect(buildAudioHeader(3, kTypeOpus, 0, 1, nullptr, 0) == 0, "v3 null buffer rejected");
    }

    // Truncation and null inputs.
    {
        const uint8_t* out = nullptr;
        size_t len = 0;
        expect(!parseAudioHeader(2, buf, 15, &out, &len), "v2 truncated");
        expect(!parseAudioHeader(3, buf, 3, &out, &len), "v3 truncated");
        expect(!parseAudioHeader(2, nullptr, 16, &out, &len), "null data rejected");
        expect(!parseAudioHeader(2, buf, 16, nullptr, &len), "null payload out rejected");
    }

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("voice frames: all checks passed\n");
    return 0;
}
