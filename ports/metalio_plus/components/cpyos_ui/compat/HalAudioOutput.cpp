#include "HalAudioOutput.h"

#include <CrossPointSettings.h>
#include <Logging.h>
#include <plus_audio.h>

#include <cstdint>

/*
 * CPYOS sound backend for the Metalio E-INK4 Plus.
 *
 * The readpico backend streams to a local I2S DAC through the SDK AudioManager.
 * This board has no such DAC: PCM goes to the BH1098G over an I2S slave link
 * that only carries data after the vendor "mode 1" handshake, so the backend is
 * a thin shim over plus_audio.c (ported from the vendor firmware, MIT).
 *
 * The SDK contract is simple: seek(0), then read() until it returns 0. There is
 * no restartable engine here, so restart() reports false and every cue is played
 * synchronously - exactly the fallback path readpico takes the first time.
 */

namespace HalAudioOutput {
namespace {

constexpr uint32_t kSupportedRate = 16000;
constexpr int kSilenceFrames = 160; /* 10 ms at 16 kHz: drains the I2S DMA */

// The cue level selects a relative loudness; the user's speaker volume (side
// keys, SETTINGS.audioVolume) scales it. Both are 0..100.
uint8_t effectiveVolume(const uint8_t levelVolume) {
  const int scaled = (static_cast<int>(levelVolume) * SETTINGS.audioVolume + 50) / 100;
  return static_cast<uint8_t>(scaled < 0 ? 0 : (scaled > 100 ? 100 : scaled));
}

bool stream(const ReadCallback read, const SeekCallback seek) {
  if (!seek(0)) return false;
  plus_audio_pa(true);
  int16_t chunk[256];
  bool ok = true;
  for (;;) {
    const int bytes = read(reinterpret_cast<uint8_t*>(chunk), sizeof(chunk));
    if (bytes <= 0) break;
    const int frames = bytes / static_cast<int>(sizeof(int16_t));
    if (frames <= 0) break;
    if (plus_audio_write_pcm(chunk, frames) < 0) {
      ok = false;
      break;
    }
  }
  int16_t silence[kSilenceFrames] = {0};
  plus_audio_write_pcm(silence, kSilenceFrames);
  plus_audio_pa(false);
  return ok;
}

}  // namespace

bool playPcm(const ReadCallback read, const SeekCallback seek, const uint32_t sampleRate,
             const uint8_t channels, const uint8_t volume) {
  if (!read) return false;
  if (sampleRate != kSupportedRate || channels != 1) {
    LOG_ERR("SND", "unsupported cue (%u Hz, %u ch); the BH1098 link is fixed at 16 kHz mono",
            static_cast<unsigned>(sampleRate), static_cast<unsigned>(channels));
    return false;
  }
  if (!plus_audio_mode_ready()) {
    // The BH1098 handshake costs ~2.5 s and must not run on the UI task. Kick
    // it once in the background and drop this cue; the next key beeps.
    plus_audio_init();
    LOG_INF("SND", "audio path warming up; cue dropped");
    return false;
  }
  plus_audio_set_volume(effectiveVolume(volume));
  return stream(read, seek);
}

bool restart(const uint8_t volume) {
  plus_audio_set_volume(effectiveVolume(volume));
  return false; /* no async engine: caller falls back to playPcm() */
}

void setVolume(const uint8_t volume) { plus_audio_set_volume(effectiveVolume(volume)); }

void shutdown() {
  /* Keep the BH1098 mode: only mute the amplifier. A full re-handshake costs
   * ~2.5 s and a BT power reset, which is not what turning feedback off means. */
  plus_audio_pa(false);
}

}  // namespace HalAudioOutput
