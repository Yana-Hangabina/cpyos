// Minimal CPYOS native plugin. All output goes through the host API table.
#include "cpyos_plugin_api.h"

int app_main(int argc, char** argv) {
  if (argc < 1 || argv == 0 || argv[0] == 0) return 1;
  const CpyosPluginApi* api = (const CpyosPluginApi*)argv[0];
  if (api->api_version != CPYOS_PLUGIN_API_VERSION) return 2;
  api->clear(0);
  api->draw_text(24, 24, CPYOS_FONT_TITLE, "Hello from a CPYOS plugin", 1);
  api->draw_text(24, 80, CPYOS_FONT_SMALL, "Back exits.", 1);
  api->present();
  while (!api->exit_requested()) {
    CpyosPluginEvent event;
    while (api->poll_event(&event)) {
      if (event.type == CPYOS_EVENT_BUTTON_DOWN && event.button == CPYOS_BUTTON_CONFIRM) {
        api->log("confirm");
      }
    }
    api->delay_ms(50);
  }
  return 0;
}
