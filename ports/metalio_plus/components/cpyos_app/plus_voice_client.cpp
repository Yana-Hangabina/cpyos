#include "plus_voice_client.h"
#include "plus_voice_frames.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <CrossPointSettings.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_websocket_client.h>
#include <opus_decoder.h>
#include <opus_encoder.h>
#include <plus_audio.h>
#include <PlusCellular.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace plus_voice {
namespace {

constexpr const char* kTag = "VOICE";
constexpr const char* kConfigPath = "/.crosspoint/voice.json";
constexpr const char* kConfigDir = "/.crosspoint";

/* Requested audio parameters, identical to the vendor hello: the server replies
 * with its own TTS parameters in the hello JSON (typically 24 kHz), and libopus
 * converts down to our fixed 16 kHz speaker clock inside opus_decode(). */
constexpr int kSampleRate = 16000;
constexpr int kFrameMs = 60;
constexpr int kFrameSamples = kSampleRate / 1000 * kFrameMs; /* 960 */
constexpr int kChannels = 1;

/* PSRAM stack: only sockets and I2S run here, never an internal-flash write.
 * xTaskCreatePinnedToCoreWithCaps counts StackType_t WORDS on this core, so
 * 16K words = 64 KiB. The measured worker used ~52 KiB before exiting (stack
 * free 13752 B); do not "correct" this to 16 KiB, the TLS/Opus frames live on
 * this stack. */
constexpr uint32_t kWorkerStackBytes = 16 * 1024;
constexpr int kConnectWaitMs = 12000;
constexpr int kHelloWaitMs = 10000;
constexpr int kSendTimeoutMs = 1000;
constexpr uint32_t kStopWaitMs = 15000;

/* Server->client audio queue. 8 x 60 ms = 480 ms of slack, in PSRAM so it does
 * not eat the internal heap the TLS session and libopus states need. */
constexpr uint32_t kAudioSlots = 8;
constexpr uint32_t kAudioMax = 1024;

enum WorkerError : int {
  kErrNone = 0,
  kErrAudioReady = 1000,  // BH1098 never reached mode 1
  kErrMemory = 1001,      // PSRAM queue or opus state allocation failed
  kErrSocket = 1002,      // WebSocket init/start failed
  kErrConnect = 1003,     // no CONNECTED event within the timeout
  kErrHello = 1004,       // server hello missing or rejected
  kErrNoNetwork = kErrorNoNetwork,  // no link; refused before touching lwIP
};

struct Slot {
  uint16_t len;
  uint8_t data[kAudioMax];
};

struct Ctx {
  Config cfg;

  std::atomic<bool> stop{true};
  std::atomic<bool> ptt{false};
  std::atomic<bool> done{true};
  std::atomic<int> phase{static_cast<int>(Phase::Idle)};
  std::atomic<int> error{kErrNone};
  std::atomic<bool> insecure{false};
  std::atomic<bool> connected{false};
  std::atomic<bool> disconnected{false};
  std::atomic<bool> helloSeen{false};
  std::atomic<uint32_t> sent{0};
  std::atomic<uint32_t> recv{0};

  /* Worker-owned handles (never touched from the UI task). */
  esp_websocket_client_handle_t ws = nullptr;
  Slot* slots = nullptr;
  uint32_t head = 0;  // produced
  uint32_t tail = 0;  // consumed
  portMUX_TYPE ringMux = portMUX_INITIALIZER_UNLOCKED;

  /* Handshake results. */
  char sessionId[72] = {0};
  int serverSampleRate = 24000;
  int serverFrameMs = kFrameMs;

  /* Last transcript / sentence for the page, guarded by textMux. */
  portMUX_TYPE textMux = portMUX_INITIALIZER_UNLOCKED;
  char text[128] = {0};
};

Ctx g_ctx;
TaskHandle_t g_task = nullptr;
char g_headers[512];

/* Static PCM scratch: kept off the 16 KiB worker stack because libopus uses
 * alloca for its own per-frame arrays and the two must not compete. */
int16_t g_micStereo[2 * kFrameSamples];
uint8_t g_txFrame[16 + MAX_OPUS_PACKET_SIZE];

void setText(const char* text) {
  if (text == nullptr || text[0] == '\0') return;
  const size_t len = std::strlen(text);
  portENTER_CRITICAL(&g_ctx.textMux);
  std::strncpy(g_ctx.text, text, sizeof(g_ctx.text) - 1);
  g_ctx.text[sizeof(g_ctx.text) - 1] = '\0';
  portEXIT_CRITICAL(&g_ctx.textMux);
  LOG_INF(kTag, "text: %u B", static_cast<unsigned>(len));
}

/* Scheme://host[:port] only: xiaozhi servers often carry the token in the query
 * string, so the path and query never reach the log. */
std::string describeUrl(const char* url) {
  const char* sep = std::strstr(url, "://");
  const size_t schemeLen = sep ? static_cast<size_t>(sep - url) + 3 : 0;
  const char* hostStart = url + schemeLen;
  const char* hostEnd = hostStart + std::strcspn(hostStart, "/?#");
  std::string out(url, static_cast<size_t>(hostEnd - url));
  return out;
}

const char* deviceMac() {
  static char mac[18] = {0};
  if (mac[0] == '\0') {
    uint8_t raw[6] = {0};
    if (esp_efuse_mac_get_default(raw) == ESP_OK) {
      std::snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", raw[0], raw[1], raw[2], raw[3], raw[4],
                    raw[5]);
    } else {
      std::snprintf(mac, sizeof(mac), "00:00:00:00:00:00");
    }
  }
  return mac;
}

/* "AABBCCDDEEFF-cpyos": the vendor sends the board UUID; we have none, and a
 * MAC-derived id is stable across boots, which is what the server keys on. */
const char* clientId() {
  static char id[32] = {0};
  if (id[0] == '\0') {
    char compact[13] = {0};
    int out = 0;
    for (const char* p = deviceMac(); *p && out < 12; ++p) {
      if (*p != ':') compact[out++] = *p;
    }
    std::snprintf(id, sizeof(id), "%s-cpyos", compact);
  }
  return id;
}



bool pushAudio(const uint8_t* data, size_t len) {
  if (len == 0 || len > kAudioMax) return false;
  bool ok = false;
  portENTER_CRITICAL(&g_ctx.ringMux);
  if (g_ctx.head - g_ctx.tail < kAudioSlots) {
    Slot& slot = g_ctx.slots[g_ctx.head % kAudioSlots];
    slot.len = static_cast<uint16_t>(len);
    std::memcpy(slot.data, data, len);
    ++g_ctx.head;
    ok = true;
  }
  portEXIT_CRITICAL(&g_ctx.ringMux);
  return ok;
}

bool popAudio(Slot& out) {
  bool ok = false;
  portENTER_CRITICAL(&g_ctx.ringMux);
  if (g_ctx.head != g_ctx.tail) {
    out = g_ctx.slots[g_ctx.tail % kAudioSlots];
    ++g_ctx.tail;
    ok = true;
  }
  portEXIT_CRITICAL(&g_ctx.ringMux);
  return ok;
}

void onServerHello(const cJSON* root) {
  const cJSON* session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
  if (cJSON_IsString(session) && session->valuestring) {
    std::strncpy(g_ctx.sessionId, session->valuestring, sizeof(g_ctx.sessionId) - 1);
    g_ctx.sessionId[sizeof(g_ctx.sessionId) - 1] = '\0';
  }
  const cJSON* params = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
  if (cJSON_IsObject(params)) {
    const cJSON* rate = cJSON_GetObjectItemCaseSensitive(params, "sample_rate");
    if (cJSON_IsNumber(rate) && rate->valueint > 0) g_ctx.serverSampleRate = rate->valueint;
    const cJSON* duration = cJSON_GetObjectItemCaseSensitive(params, "frame_duration");
    if (cJSON_IsNumber(duration) && duration->valueint > 0) g_ctx.serverFrameMs = duration->valueint;
  }
  g_ctx.helloSeen.store(true);
  LOG_INF(kTag, "server hello: session=%u B, %d Hz, %d ms", static_cast<unsigned>(std::strlen(g_ctx.sessionId)),
          g_ctx.serverSampleRate, g_ctx.serverFrameMs);
}

/* Text frames: hello, and the stt/tts state machine. Everything else (mcp,
 * llm, iot, detect) is ignored — this build has no MCP tool surface. */
void onServerText(const char* data, size_t len) {
  cJSON* root = cJSON_ParseWithLength(data, len);
  if (root == nullptr) {
    LOG_INF(kTag, "ignoring unparsable server text (%u B)", static_cast<unsigned>(len));
    return;
  }
  const cJSON* type = cJSON_GetObjectItemCaseSensitive(root, "type");
  if (cJSON_IsString(type) && type->valuestring) {
    if (std::strcmp(type->valuestring, "hello") == 0) {
      onServerHello(root);
    } else if (std::strcmp(type->valuestring, "stt") == 0 || std::strcmp(type->valuestring, "tts") == 0) {
      const cJSON* text = cJSON_GetObjectItemCaseSensitive(root, "text");
      const cJSON* state = cJSON_GetObjectItemCaseSensitive(root, "state");
      /* tts repeats the sentence with a "text" field per sentence_start; a
       * plain "stop" carries none. */
      const bool usable = cJSON_IsString(text) && text->valuestring && text->valuestring[0] != '\0' &&
                          (!cJSON_IsString(state) || std::strcmp(state->valuestring, "start") == 0 ||
                           std::strcmp(state->valuestring, "sentence_start") == 0);
      if (usable) setText(text->valuestring);
    }
  }
  cJSON_Delete(root);
}

/* Audio frames. v1 is a bare Opus payload; v2/v3 prepend a header whose type 0
 * is Opus (type 1 would be JSON, which this build never asks the server for). */
void onServerBinary(const uint8_t* data, size_t len) {
  const uint8_t version = g_ctx.cfg.version;
  const uint8_t* payload = data;
  size_t payloadLen = len;
  if (!parseAudioHeader(version, data, len, &payload, &payloadLen)) return;

  if (!pushAudio(payload, payloadLen)) {
    LOG_INF(kTag, "incoming audio dropped (queue full or oversized: %u B)", static_cast<unsigned>(payloadLen));
    return;
  }
  g_ctx.recv.fetch_add(1);
}

void onWsEvent(void* arg, esp_event_base_t, int32_t eventId, void* eventData) {
  auto* ctx = static_cast<Ctx*>(arg);
  auto* ev = static_cast<esp_websocket_event_data_t*>(eventData);
  switch (eventId) {
    case WEBSOCKET_EVENT_CONNECTED:
      ctx->connected.store(true);
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
      ctx->connected.store(false);
      ctx->disconnected.store(true);
      break;
    case WEBSOCKET_EVENT_DATA:
      if (ev == nullptr || ev->data_ptr == nullptr || ev->data_len <= 0) break;
      /* Frames larger than buffer_size arrive in pieces; audio never is. */
      if (ev->payload_len != ev->data_len || ev->payload_offset != 0) break;
      if (ev->op_code == 0x02) {
        onServerBinary(reinterpret_cast<const uint8_t*>(ev->data_ptr), static_cast<size_t>(ev->data_len));
      } else if (ev->op_code == 0x01) {
        onServerText(ev->data_ptr, static_cast<size_t>(ev->data_len));
      }
      break;
    case WEBSOCKET_EVENT_ERROR:
      if (ev != nullptr) {
        ctx->error.store(ev->error_handle.esp_tls_last_esp_err != 0
                             ? static_cast<int>(ev->error_handle.esp_tls_last_esp_err)
                             : ev->error_handle.esp_ws_handshake_status_code);
      }
      ctx->connected.store(false);
      ctx->disconnected.store(true);
      break;
    default:
      break;
  }
}

bool waitFlag(const std::atomic<bool>& flag, int timeoutMs) {
  const uint32_t deadline = millis() + static_cast<uint32_t>(timeoutMs);
  while (!flag.load()) {
    if (g_ctx.stop.load()) return false;
    if (static_cast<int32_t>(deadline - millis()) <= 0) return false;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return true;
}

bool sendText(const char* text) {
  if (g_ctx.ws == nullptr) return false;
  const int sent = esp_websocket_client_send_text(g_ctx.ws, text, static_cast<int>(std::strlen(text)),
                                                  pdMS_TO_TICKS(kSendTimeoutMs));
  return sent >= 0;
}

bool sendJson(const char* json) {
  if (!sendText(json)) {
    LOG_ERR(kTag, "text frame rejected");
    return false;
  }
  return true;
}

bool sendHello() {
  cJSON* root = cJSON_CreateObject();
  if (root == nullptr) return false;
  cJSON_AddStringToObject(root, "type", "hello");
  cJSON_AddNumberToObject(root, "version", g_ctx.cfg.version);
  /* mcp:false because this port has no MCP tool surface; the vendor announces
   * true and then drops every mcp payload. */
  cJSON* features = cJSON_AddObjectToObject(root, "features");
  cJSON_AddBoolToObject(features, "mcp", false);
  cJSON_AddStringToObject(root, "transport", "websocket");
  cJSON* params = cJSON_AddObjectToObject(root, "audio_params");
  cJSON_AddStringToObject(params, "format", "opus");
  cJSON_AddNumberToObject(params, "sample_rate", kSampleRate);
  cJSON_AddNumberToObject(params, "channels", kChannels);
  cJSON_AddNumberToObject(params, "frame_duration", kFrameMs);

  char* printed = cJSON_PrintUnformatted(root);
  const bool ok = printed != nullptr && sendText(printed);
  if (printed != nullptr) cJSON_free(printed);
  cJSON_Delete(root);
  LOG_INF(kTag, "client hello sent (version %u)", static_cast<unsigned>(g_ctx.cfg.version));
  return ok;
}

bool sendListen(const char* state) {
  /* Manual mode: the client decides when the turn ends (button release), which
   * is what push-to-talk is. */
  char msg[160];
  std::snprintf(msg, sizeof(msg), "{\"session_id\":\"%.64s\",\"type\":\"listen\",\"state\":\"%s\",\"mode\":\"manual\"}",
                g_ctx.sessionId, state);
  return sendJson(msg);
}

bool sendOpus(const uint8_t* opus, size_t len, uint32_t timestampMs) {
  const uint8_t version = g_ctx.cfg.version;
  const size_t header = buildAudioHeader(version, kTypeOpus, timestampMs, len, g_txFrame, sizeof(g_txFrame));
  if ((version == 2 || version == 3) && header == 0) return false;
  if (len + header > sizeof(g_txFrame)) return false;
  std::memcpy(g_txFrame + header, opus, len);
  const int sent = esp_websocket_client_send_bin(g_ctx.ws, reinterpret_cast<const char*>(g_txFrame),
                                                 static_cast<int>(len + header), pdMS_TO_TICKS(kSendTimeoutMs));
  return sent >= 0;
}

bool ensureMode1() {
  if (!plus_audio_wait_ready(4000)) {
    LOG_ERR(kTag, "audio not ready (state %d), retrying once", static_cast<int>(plus_audio_state()));
    if (plus_audio_retry() != ESP_OK && plus_audio_state() != PLUS_AUDIO_READY) return false;
    if (!plus_audio_wait_ready(4000)) return false;
  }
  if (plus_audio_bt_mode() != 1) {
    const esp_err_t err = plus_audio_bt_set_mode(1);
    if (err != ESP_OK) {
      LOG_ERR(kTag, "mode 1 restore failed: %s", esp_err_to_name(err));
      return false;
    }
  }
  return true;
}

/* One WebSocket attempt. On a wss URL the first try verifies against the CA
 * bundle (what the vendor does); if that fails the caller retries without any
 * certificate check, so a self-signed user server still works. */
bool openSocket(const bool verifyTls) {
  esp_websocket_client_config_t cfg = {};
  cfg.uri = g_ctx.cfg.url;
  cfg.headers = g_headers;
  cfg.disable_auto_reconnect = true;
  /* 8 KiB: this task also carries the mbedtls handshake and our JSON callback,
   * and the 4 KiB default is where the 78 wrapper ran close to the edge. */
  cfg.task_stack = 8192;
  cfg.task_prio = 2;
  cfg.disable_pingpong_discon = true;
  cfg.buffer_size = 2048;
  cfg.network_timeout_ms = 15000;
  cfg.reconnect_timeout_ms = 1000;
  cfg.user_context = &g_ctx;
  if (verifyTls) cfg.crt_bundle_attach = esp_crt_bundle_attach;

  g_ctx.ws = esp_websocket_client_init(&cfg);
  if (g_ctx.ws == nullptr) {
    g_ctx.error.store(kErrSocket);
    return false;
  }
  esp_websocket_register_events(g_ctx.ws, WEBSOCKET_EVENT_ANY, onWsEvent, &g_ctx);
  g_ctx.connected.store(false);
  g_ctx.disconnected.store(false);
  if (esp_websocket_client_start(g_ctx.ws) != ESP_OK) {
    g_ctx.error.store(kErrSocket);
    return false;
  }
  if (!waitFlag(g_ctx.connected, kConnectWaitMs)) {
    g_ctx.error.store(kErrConnect);
    return false;
  }
  g_ctx.insecure.store(!verifyTls);
  return true;
}

void closeSocket() {
  if (g_ctx.ws == nullptr) return;
  esp_websocket_client_close(g_ctx.ws, pdMS_TO_TICKS(1000));
  esp_websocket_client_destroy(g_ctx.ws);
  g_ctx.ws = nullptr;
}

/* Decode everything the WebSocket task queued and push it to the speaker. */
void drainIncoming(OpusDecoderWrapper& decoder, std::vector<uint8_t>& packet, std::vector<int16_t>& pcm,
                   bool& paOn, uint32_t& lastPlayMs) {
  Slot slot;
  while (popAudio(slot)) {
    packet.assign(slot.data, slot.data + slot.len);
    if (!decoder.Decode(std::move(packet), pcm)) {
      LOG_ERR(kTag, "opus decode failed (%u B)", static_cast<unsigned>(slot.len));
      continue;
    }
    if (!pcm.empty()) {
      if (!paOn) {
        plus_audio_pa(true);
        paOn = true;
      }
      if (plus_audio_write_pcm(pcm.data(), static_cast<int>(pcm.size())) < 0) {
        LOG_ERR(kTag, "speaker write failed");
        break;
      }
      lastPlayMs = millis();
    }
  }
  /* Amp off after a quiet second: it hisses while idle and would feed the mic. */
  if (paOn && millis() - lastPlayMs > 1500) {
    plus_audio_pa(false);
    paOn = false;
  }
}

/* Assemble exactly one 60 ms frame (reads return fewer samples on an I2S
 * timeout) and send it. */
void captureAndSend(OpusEncoderWrapper& encoder, std::vector<int16_t>& frame, std::vector<uint8_t>& opus,
                    uint32_t timestampMs) {
  int filled = 0;
  while (filled < kFrameSamples) {
    if (g_ctx.stop.load() || !g_ctx.ptt.load()) return;
    const int want = kFrameSamples - filled;
    const int got = plus_audio_read_mic(g_micStereo + 2 * filled, want);
    if (got < 0) {
      LOG_ERR(kTag, "microphone read failed");
      return;
    }
    if (got == 0) continue; /* 250 ms driver timeout; re-check the button */
    /* L is the microphone, R the board's reference; their mean is the mono
     * signal, the same reduction the vendor codec path uses. */
    for (int i = 0; i < got; ++i) {
      const int32_t sum = int32_t(g_micStereo[2 * (filled + i)]) + int32_t(g_micStereo[2 * (filled + i) + 1]);
      frame[static_cast<size_t>(filled + i)] = static_cast<int16_t>(sum / 2);
    }
    filled += got;
  }
  if (!encoder.Encode(std::move(frame), opus)) {
    LOG_ERR(kTag, "opus encode failed");
    return;
  }
  if (opus.empty()) return;
  if (!sendOpus(opus.data(), opus.size(), timestampMs)) {
    LOG_ERR(kTag, "audio send failed");
    return;
  }
  g_ctx.sent.fetch_add(1);
}

void runWorker() {
  g_ctx.phase.store(static_cast<int>(Phase::Connecting));
  g_ctx.error.store(kErrNone);
  g_ctx.insecure.store(false);
  g_ctx.helloSeen.store(false);
  g_ctx.sent.store(0);
  g_ctx.recv.store(0);
  g_ctx.sessionId[0] = '\0';
  g_ctx.serverSampleRate = 24000;
  g_ctx.serverFrameMs = kFrameMs;

  /* Releasing the radio deinitializes lwIP, so the first socket call after
   * that aborts inside tcpip_send_msg_wait_sem ("Invalid mbox"). The page
   * normally joins Wi-Fi before start(), but a dead link (or the bench probe)
   * must end as a visible state, never as a reboot. Checked before the audio
   * rail and the WebSocket task are touched. */
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR(kTag, "no network link; refusing to open a socket");
    g_ctx.error.store(kErrNoNetwork);
    g_ctx.phase.store(static_cast<int>(Phase::Failed));
    return;
  }

  const bool cellularWasActive = plus_cellular_active();
  if (!ensureMode1()) {
    g_ctx.error.store(kErrAudioReady);
    return;
  }
  plus_audio_set_volume(SETTINGS.audioVolume);
  plus_audio_pa(false);

  g_ctx.slots = static_cast<Slot*>(heap_caps_malloc(sizeof(Slot) * kAudioSlots, MALLOC_CAP_SPIRAM));
  if (g_ctx.slots == nullptr) {
    LOG_ERR(kTag, "no PSRAM for the audio queue");
    g_ctx.error.store(kErrMemory);
    return;
  }
  g_ctx.head = 0;
  g_ctx.tail = 0;

  char mac[18];
  std::snprintf(mac, sizeof(mac), "%s", deviceMac());
  const bool wantsTls = std::strncmp(g_ctx.cfg.url, "wss://", 6) == 0;
  if (g_ctx.cfg.token[0] != '\0') {
    std::snprintf(g_headers, sizeof(g_headers),
                  "Authorization: Bearer %s\r\nProtocol-Version: %u\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",
                  g_ctx.cfg.token, static_cast<unsigned>(g_ctx.cfg.version), mac, clientId());
  } else {
    std::snprintf(g_headers, sizeof(g_headers), "Protocol-Version: %u\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",
                  static_cast<unsigned>(g_ctx.cfg.version), mac, clientId());
  }
  LOG_INF(kTag, "connecting %s (version %u, token=%d)", describeUrl(g_ctx.cfg.url).c_str(),
          static_cast<unsigned>(g_ctx.cfg.version), g_ctx.cfg.token[0] != '\0' ? 1 : 0);

  // plus_audio's mode-1 handshake may cut P14 and stop the modem. The
  // vendor recovers it asynchronously; do not race the socket against that
  // recovery when 4G was the active link before the handshake.
  if (cellularWasActive && !plus_cellular_active()) {
    LOG_INF(kTag, "waiting for 4G recovery after the audio rail reset");
    const uint32_t deadline = millis() + 20000;
    while (!plus_cellular_active() && static_cast<int32_t>(deadline - millis()) > 0) {
      vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!plus_cellular_active()) {
      g_ctx.error.store(kErrConnect);
      LOG_ERR(kTag, "4G did not recover after the audio rail reset");
      return;
    }
  }
  bool opened = openSocket(wantsTls);
  if (!opened && wantsTls && !g_ctx.stop.load()) {
    /* Vendor-adjacent fallback: the port already builds with
     * CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY, and self-signed servers are common
     * on a LAN. Retry once without the bundle and say so in the page. */
    LOG_ERR(kTag, "TLS verify failed (err %d); retrying without certificate check", g_ctx.error.load());
    closeSocket();
    opened = openSocket(false);
  }
  if (!opened) {
    if (g_ctx.error.load() == kErrNone) g_ctx.error.store(kErrConnect);
    LOG_ERR(kTag, "channel open failed (err %d)", g_ctx.error.load());
    return;
  }

  if (!sendHello() || !waitFlag(g_ctx.helloSeen, kHelloWaitMs)) {
    g_ctx.error.store(kErrHello);
    LOG_ERR(kTag, "server hello missing");
    return;
  }

  // The encoder/decoder are fixed at 60 ms; a server asking for another frame
  // length would otherwise report Connected and then emit unplayable audio.
  if (g_ctx.serverFrameMs != kFrameMs) {
    g_ctx.error.store(kErrHello);
    LOG_ERR(kTag, "server frame %d ms unsupported (this build uses %d ms)", g_ctx.serverFrameMs, kFrameMs);
    return;
  }
  OpusEncoderWrapper encoder(kSampleRate, kChannels, kFrameMs);
  /* Decode straight to 16 kHz: our speaker clock is fixed and libopus performs
   * the rate conversion inside opus_decode(). The vendor decodes at the server
   * rate and resamples because its DAC can do 24 kHz. */
  OpusDecoderWrapper decoder(kSampleRate, kChannels, kFrameMs);
  std::vector<int16_t> frame(static_cast<size_t>(kFrameSamples));
  std::vector<uint8_t> opus;
  std::vector<uint8_t> packet;
  std::vector<int16_t> pcm;
  opus.reserve(MAX_OPUS_PACKET_SIZE);
  pcm.reserve(2 * kFrameSamples);
  packet.reserve(kAudioMax);

  // Prove the encoder actually allocated before declaring the channel usable;
  // libopus allocates lazily after hello, and a failure used to leave the page
  // Connected with every Encode() silently false.
  {
    std::vector<int16_t> probe(static_cast<size_t>(kFrameSamples), 0);
    std::vector<uint8_t> probeOpus;
    if (!encoder.Encode(std::move(probe), probeOpus)) {
      g_ctx.error.store(kErrMemory);
      LOG_ERR(kTag, "opus encoder unavailable after hello");
      return;
    }
  }

  g_ctx.phase.store(static_cast<int>(Phase::Connected));
  LOG_INF(kTag, "channel open: server %d Hz/%d ms, ptt ready", g_ctx.serverSampleRate, g_ctx.serverFrameMs);

  bool paOn = false;
  bool listening = false;
  uint32_t lastPlayMs = millis();
  uint32_t pttStartMs = 0;

  while (!g_ctx.stop.load() && !g_ctx.disconnected.load()) {
    drainIncoming(decoder, packet, pcm, paOn, lastPlayMs);

    const bool wantTalk = g_ctx.ptt.load();
    if (wantTalk && !listening) {
      if (paOn) {
        plus_audio_pa(false); /* half duplex: never drive the speaker while capturing */
        paOn = false;
      }
      if (!sendListen("start")) break;
      listening = true;
      pttStartMs = millis();
      LOG_INF(kTag, "ptt start");
    } else if (!wantTalk && listening) {
      sendListen("stop");
      listening = false;
      LOG_INF(kTag, "ptt stop (%u frame(s))", static_cast<unsigned>(g_ctx.sent.load()));
    }

    if (listening) {
      captureAndSend(encoder, frame, opus, millis() - pttStartMs);
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  if (listening) sendListen("stop");
  plus_audio_pa(false);
  LOG_INF(kTag, "channel closing (%u sent, %u received)", static_cast<unsigned>(g_ctx.sent.load()),
          static_cast<unsigned>(g_ctx.recv.load()));
}

void workerTrampoline(void*) {
  runWorker();
  closeSocket();
  plus_audio_pa(false);
  if (g_ctx.slots != nullptr) {
    heap_caps_free(g_ctx.slots);
    g_ctx.slots = nullptr;
  }
  /* A user-requested stop returns to Idle; anything else (open/hello failure, a
   * server-side close) leaves Failed so the page can show the reason. */
  const bool userStop = g_ctx.stop.load();
  g_ctx.phase.store(static_cast<int>(userStop && g_ctx.error.load() == kErrNone ? Phase::Idle : Phase::Failed));
  g_ctx.ptt.store(false);
  /* g_task is deliberately NOT cleared here: it is written only by the UI task
   * (start/stop), so the worker never races the handle the page reads. */
  g_ctx.done.store(true);
  LOG_INF(kTag, "worker exited; stack free=%u",
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  vTaskDeleteWithCaps(nullptr);
}

}  // namespace

/* ------------------------------------------------------------- settings --- */

bool loadConfig(Config& out) {
  out = Config{};
  if (!Storage.exists(kConfigPath)) return false;
  const String raw = Storage.readFile(kConfigPath);
  if (raw.isEmpty()) return false;

  JsonDocument doc;
  if (deserializeJson(doc, raw.c_str()) != DeserializationError::Ok) {
    LOG_ERR(kTag, "voice.json is not valid JSON");
    return false;
  }
  const char* url = doc["url"] | "";
  std::strncpy(out.url, url, sizeof(out.url) - 1);
  const int version = doc["version"] | 1;
  out.version = static_cast<uint8_t>(version >= 1 && version <= 3 ? version : 1);
  const char* obfuscated = doc["token"] | "";
  if (obfuscated[0] != '\0') {
    bool ok = false;
    const std::string token = obfuscation::deobfuscateFromBase64(obfuscated, sizeof(Config::token) - 1, &ok, nullptr);
    if (ok && !token.empty()) {
      std::strncpy(out.token, token.c_str(), sizeof(out.token) - 1);
    } else {
      LOG_ERR(kTag, "saved token could not be decoded; treating it as unset");
    }
  }
  LOG_INF(kTag, "config loaded: url=%d token=%d version=%u", out.url[0] != '\0' ? 1 : 0,
          out.token[0] != '\0' ? 1 : 0, static_cast<unsigned>(out.version));
  return true;
}

bool saveConfig(const Config& cfg) {
  if (!Storage.ensureDirectoryExists(kConfigDir)) {
    LOG_ERR(kTag, "cannot create %s", kConfigDir);
    return false;
  }
  JsonDocument doc;
  doc["url"] = cfg.url;
  doc["version"] = cfg.version >= 1 && cfg.version <= 3 ? cfg.version : 1;
  if (cfg.token[0] != '\0') {
    doc["token"] = obfuscation::obfuscateToBase64(std::string(cfg.token));
  }
  std::string payload;
  serializeJson(doc, payload);
  if (!Storage.writeFile(kConfigPath, String(payload.c_str()))) {
    LOG_ERR(kTag, "cannot write %s", kConfigPath);
    return false;
  }
  /* Values are never logged; only that the record was written. */
  LOG_INF(kTag, "config saved (%u B)", static_cast<unsigned>(payload.size()));
  return true;
}

/* -------------------------------------------------------------- session --- */

bool start(const Config& cfg) {
  /* `done` is the authoritative liveness flag: the worker may have exited on its
   * own (server closed, hello timeout) without the page noticing. */
  if (!g_ctx.done.load()) {
    LOG_ERR(kTag, "a worker is already running");
    return false;
  }
  if (!cfg.valid()) return false;
  g_ctx.cfg = cfg;
  g_task = nullptr; /* a previous worker already deleted itself */
  g_ctx.stop.store(false);
  g_ctx.ptt.store(false);
  g_ctx.done.store(false);
  g_ctx.phase.store(static_cast<int>(Phase::Connecting));
  g_ctx.error.store(kErrNone);
  portENTER_CRITICAL(&g_ctx.textMux);
  g_ctx.text[0] = '\0';
  portEXIT_CRITICAL(&g_ctx.textMux);

  // TLS, the websocket client and libopus still need a contiguous internal
  // block; a 16 KiB stack taken from the same pool was the difference between
  // "connects" and "not enough memory" once Wi-Fi was resident. The stack
  // itself only waits on sockets/I2S and never touches internal flash, so it
  // lives in PSRAM (the same rule the JJWXC/browser workers follow).
  constexpr size_t kMinInternalLargest = 16 * 1024;
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < kMinInternalLargest) {
    g_task = nullptr;
    g_ctx.done.store(true);
    g_ctx.phase.store(static_cast<int>(Phase::Idle));
    g_ctx.error.store(kErrMemory);
    LOG_ERR(kTag, "worker not started: internal largest %u < %u",
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned>(kMinInternalLargest));
    return false;
  }
  const BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
      workerTrampoline, "plusVoice", kWorkerStackBytes, nullptr, tskIDLE_PRIORITY, &g_task, 0,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ok != pdPASS) {
    g_task = nullptr;
    g_ctx.done.store(true);
    g_ctx.phase.store(static_cast<int>(Phase::Idle));
    g_ctx.error.store(kErrMemory);
    LOG_ERR(kTag, "worker task creation failed");
    return false;
  }
  LOG_INF(kTag, "worker up: stack in PSRAM, internal largest=%u",
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  return true;
}

bool stop() {
  g_ctx.ptt.store(false);
  g_ctx.stop.store(true);
  if (g_task == nullptr) {
    g_ctx.done.store(true);
    return true;
  }
  const uint32_t deadline = millis() + kStopWaitMs;
  while (!g_ctx.done.load() && static_cast<int32_t>(deadline - millis()) > 0) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  const bool stopped = g_ctx.done.load();
  if (!stopped) {
    /* Every blocking call on the worker has a bound (mic 250 ms, send 1 s, TLS
     * connect 15 s), so this is a stuck socket, not a normal exit. The worker's
     * state is static, so it is safe to leave it finishing on its own; the
     * handle is dropped here because the task deletes itself. */
    LOG_ERR(kTag, "worker did not stop in time; leaving it to exit on its own");
  }
  g_task = nullptr; /* self-deleting task: only ever drop the handle */
  return stopped;
}

bool running() { return !g_ctx.done.load(); }

void setPtt(const bool held) {
  if (held && !g_ctx.connected.load()) return;
  g_ctx.ptt.store(held);
}

Status status() {
  Status s;
  s.phase = static_cast<Phase>(g_ctx.phase.load());
  s.error = g_ctx.error.load();
  s.insecure = g_ctx.insecure.load();
  s.ptt = g_ctx.ptt.load();
  s.sentFrames = g_ctx.sent.load();
  s.recvFrames = g_ctx.recv.load();
  return s;
}

void copyLastText(char* out, const size_t cap) {
  if (out == nullptr || cap == 0) return;
  portENTER_CRITICAL(&g_ctx.textMux);
  std::strncpy(out, g_ctx.text, cap - 1);
  out[cap - 1] = '\0';
  portEXIT_CRITICAL(&g_ctx.textMux);
}

}  // namespace plus_voice
