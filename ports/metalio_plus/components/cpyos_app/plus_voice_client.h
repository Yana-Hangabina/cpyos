#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

/*
 * Push-to-talk client for the official xiaozhi (小智) AI-voice protocol.
 *
 * Ported from the vendor firmware (metalio-vendor: protocols/protocol.{h,cc},
 * protocols/websocket_protocol.{h,cc}, audio/audio_service.cc, MIT, (c) 2025
 * Shenzhen Xinzhi Future Technology Co., Ltd. / Project Contributors) with the
 * AFE / wake-word / MCP / cloud-provisioning paths removed: the ESP32-S31 build
 * has no esp-sr model and the server address/token are user supplied.
 *
 * SETTINGS FILE -- /.crosspoint/voice.json (UTF-8 JSON)
 *   {
 *     "url":     "wss://api.tenclass.net/xiaozhi/v1/",  // ws:// or wss://
 *     "token":   "<optional bearer token>",             // stored obfuscated
 *     "version": 1                                      // 1..3, default 1
 *   }
 * - Missing file: no URL (the page asks for one), no token, version 1.
 * - "token" is written with obfuscation::obfuscateToBase64 (eFuse-MAC XOR +
 *   base64, the same treatment as the other credential files) and is never
 *   logged. An unreadable/garbage token is treated as "no token".
 * - "version" selects the binary framing the channel negotiates: 1 = raw Opus
 *   payloads, 2 = BinaryProtocol2 (16-byte header), 3 = BinaryProtocol3
 *   (4-byte header). Server hello decides the Opus sample rate/frame length.
 *
 * Threading contract:
 * - start()/stop()/setPtt()/status() are called from the UI task only.
 * - The worker is one PSRAM-stack task on core 0 (started/removed with the
 *   caps variants). Only sockets and the I2S calls run on it, never an
 *   internal-flash operation, which is the rule that makes an external stack
 *   legal. It owns the WebSocket handle and the opus codecs, publishes atomics
 *   only, and deletes itself when it finishes. stop() waits a bounded time for
 *   that; a worker that outlives the page cannot touch freed memory because
 *   every buffer it uses is either its own stack or this module's static state.
 */
namespace plus_voice {

struct Config {
  char url[160] = {0};
  char token[96] = {0};
  uint8_t version = 1;
  bool valid() const { return url[0] != '\0'; }
};

enum class Phase : uint8_t {
  Idle,        // no worker
  Connecting,  // TCP/TLS + hello exchange in flight
  Connected,   // channel open, audio flows
  Failed,      // last attempt failed; error holds the reason
};

/* Status.error value for "no link was up, so the worker never opened a socket".
 * Public because the page maps it to a network hint instead of a generic
 * connection failure. */
constexpr int kErrorNoNetwork = 1005;

struct Status {
  Phase phase = Phase::Idle;
  int error = 0;  // esp_err_t or esp_websocket error code of the last failure
  bool insecure = false;  // wss fell back to "do not verify" (self-signed server)
  bool ptt = false;
  uint32_t sentFrames = 0;
  uint32_t recvFrames = 0;
};

// Reads/writes /.crosspoint/voice.json. Missing file is not an error: the
// returned Config is simply empty (valid() == false).
bool loadConfig(Config& out);
bool saveConfig(const Config& cfg);

// Spawns the worker. Returns false when one is already running or the task
// could not be created (internal heap exhausted).
bool start(const Config& cfg);
// Requests a stop, stops PTT and waits up to ~15 s for the worker to close the
// channel and delete itself. Safe to call when nothing is running.
// Stops the worker and waits for it to release the socket. Returns false when
// it is still exiting (the caller must not tear the radio down underneath it).
bool stop();
bool running();

// Level-triggered: true while the user holds the talk button. Ignored until the
// channel is open.
void setPtt(bool held);

Status status();

// Last server-side text (stt transcript or tts sentence) for the page.
void copyLastText(char* out, size_t cap);

}  // namespace plus_voice
