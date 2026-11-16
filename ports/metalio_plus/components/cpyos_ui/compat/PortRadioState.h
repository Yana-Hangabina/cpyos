#pragma once

// True while the port is bringing the radio up or down. The SD log mirror in
// main/ pauses its file work during that window: the WiFi driver needs large
// contiguous internal/DMA blocks, and an open FAT window on top of it made
// esp_wifi_init fail with ESP_ERR_NO_MEM even though the heap looked healthy.
extern volatile bool g_portRadioBusy;
