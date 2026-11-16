// Host tests for the streaming multipart parser used by POST /upload.
#include "multipart.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

struct Collected {
    std::string name;
    std::string body;
    std::vector<std::string> names;
};

bool onStart(void* ctx, const std::string& name) {
    auto* c = static_cast<Collected*>(ctx);
    if (!c->body.empty()) return false;  // one file per request in this test
    c->name = name;
    c->names.push_back(name);
    return true;
}
bool onWrite(void* ctx, const uint8_t* data, size_t len) {
    static_cast<Collected*>(ctx)->body.append(reinterpret_cast<const char*>(data), len);
    return true;
}

std::string makeBody(const std::string& boundary, const std::string& name, const std::string& bytes) {
    return "--" + boundary + "\r\n" +
           "Content-Disposition: form-data; name=\"file\"; filename=\"" + name + "\"\r\n" +
           "Content-Type: application/octet-stream\r\n\r\n" + bytes + "\r\n--" + boundary + "--\r\n";
}

void runAll(const std::string& boundary, const std::string& body, const std::string& expectName,
            const std::string& expectBody, size_t chunk) {
    Collected collected;
    multipart::Sink sink{onStart, onWrite, nullptr, &collected};
    multipart::Parser parser(boundary, sink);
    for (size_t i = 0; i < body.size(); i += chunk) {
        const size_t len = std::min(chunk, body.size() - i);
        if (!parser.feed(reinterpret_cast<const uint8_t*>(body.data() + i), len)) {
            check(false, "feed failed");
            return;
        }
    }
    check(parser.finish(), "finish");
    check(collected.name == expectName, "filename");
    check(collected.body == expectBody, "body bytes");
}
}  // namespace

int main() {
    std::string boundary;
    check(multipart::extractBoundary("multipart/form-data; boundary=abc123", boundary) && boundary == "abc123",
          "unquoted boundary");
    check(multipart::extractBoundary("multipart/form-data; boundary=\"a:b c\"", boundary) && boundary == "a:b c",
          "quoted boundary");
    check(!multipart::extractBoundary("text/plain", boundary), "missing boundary");

    const std::string b = "----CPYOSBoundary123";
    const std::string bytes = "hello\r\nworld --not-a-boundary\r\n";
    const std::string body = makeBody(b, "book.epub", bytes);

    runAll(b, body, "book.epub", bytes, body.size());   // one chunk
    runAll(b, body, "book.epub", bytes, 1);             // byte by byte (splits boundary)
    runAll(b, body, "book.epub", bytes, 7);             // awkward chunks
    runAll(b, "junk preamble\r\n" + body, "book.epub", bytes, 3);

    // Multiple parts: finish() must be called before the next filename.
    {
        Collected collected;
        multipart::Sink sink{[](void* ctx, const std::string& name) {
                                 auto* c = static_cast<Collected*>(ctx);
                                 c->body.clear();
                                 c->names.push_back(name);
                                 return true;
                             }, [](void* ctx, const uint8_t* d, size_t n) {
                                 static_cast<Collected*>(ctx)->body.append(reinterpret_cast<const char*>(d), n);
                                 return true;
                             }, nullptr, &collected};
        multipart::Parser parser(b, sink);
        const std::string part = "--" + b + "\r\n" +
                                 "Content-Disposition: form-data; name=\"file\"; filename=\"x.epub\"\r\n\r\n" +
                                 bytes + "\r\n";
        const std::string multi = part + part + "--" + b + "--\r\n";
        check(parser.feed(reinterpret_cast<const uint8_t*>(multi.data()), multi.size()), "multi feed");
        check(parser.done(), "multi done");
        check(collected.names.size() == 2, "two parts seen");
    }

    // Truncated body must fail, not silently accept.
    {
        Collected collected;
        multipart::Sink sink{onStart, onWrite, nullptr, &collected};
        multipart::Parser parser(b, sink);
        const std::string cut = body.substr(0, body.size() / 2);
        parser.feed(reinterpret_cast<const uint8_t*>(cut.data()), cut.size());
        check(!parser.finish(), "truncated body rejected");
    }

    // A named text part without a filename is a field (official FontsPage).
    {
        std::string fieldName;
        std::string fieldValue;
        auto onField = +[](void* ctx, const std::string& name, const std::string& value) {
            auto* out = static_cast<std::pair<std::string, std::string>*>(ctx);
            *out = {name, value};
            return true;
        };
        std::pair<std::string, std::string> captured;
        multipart::Sink sink{nullptr, nullptr, nullptr, &captured, onField};
        multipart::Parser parser(b, sink);
        const std::string field = "--" + b + "\r\nContent-Disposition: form-data; name=\"family\"\r\n\r\nTest Font\r\n--" + b + "--\r\n";
        check(parser.feed(reinterpret_cast<const uint8_t*>(field.data()), field.size()), "field part accepted");
        check(captured.first == "family" && captured.second == "Test Font", "field name/value captured");
    }

    // A part with neither name nor filename is still rejected.
    {
        Collected collected;
        multipart::Sink sink{onStart, onWrite, nullptr, &collected};
        multipart::Parser parser(b, sink);
        const std::string bad = "--" + b + "\r\nContent-Disposition: form-data;\r\n\r\nabc\r\n--" + b + "--\r\n";
        check(!parser.feed(reinterpret_cast<const uint8_t*>(bad.data()), bad.size()), "unnamed part rejected");
    }

    if (failures == 0) std::printf("multipart: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
