# ArduinoJson (vendored)

- Upstream: https://github.com/bblanchon/ArduinoJson
- Version: the one CPYOS's PlatformIO build resolved (`.pio/libdeps/readpico/ArduinoJson`)
- License: MIT (see `LICENSE.txt`)
- Vendored: `src/` headers + LICENSE + README. Examples/extras are omitted.
- Reason: CPYOS settings, state and bookmark stores use ArduinoJson; the port
  must build offline and reproducibly.
