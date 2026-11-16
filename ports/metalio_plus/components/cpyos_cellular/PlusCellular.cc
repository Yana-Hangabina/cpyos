#include "PlusCellular.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <lwip/ip_addr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include "epd_board_metalio_eink4_plus.h"
#include "plus_audio.h"
#include "uart_eth_modem.h"

namespace {

constexpr const char* kTag = "PLUS-CELL";
constexpr uart_port_t kUart = UART_NUM_2;
constexpr int kBaud = 2000000;
constexpr gpio_num_t kTx = GPIO_NUM_58;
constexpr gpio_num_t kRx = GPIO_NUM_59;
constexpr gpio_num_t kMrdy = GPIO_NUM_60;
constexpr gpio_num_t kSrdy = GPIO_NUM_3;

constexpr EventBits_t kConnectedBit = BIT0;
constexpr EventBits_t kFailedBit = BIT1;

std::mutex s_mutex;
std::shared_ptr<UartEthModem> s_modem;
EventGroupHandle_t s_events = nullptr;
std::atomic<bool> s_active{false};
std::atomic<bool> s_ready{false};
std::atomic<bool> s_netifInitDone{false};

void ensureNetif(void) {
    if (s_netifInitDone.load()) return;
    esp_netif_init();                    // idempotent
    esp_event_loop_create_default();     // ESP_ERR_INVALID_STATE if already up
    gpio_install_isr_service(0);         // likely already installed by HalGPIO
    s_netifInitDone.store(true);
}

}  // namespace

bool plus_cellular_start(int timeout_ms) {
    // The BH1098 boot reset drives the shared P14 rail. Wait for an audio init
    // to finish *before* taking the modem mutex: that worker may need the same
    // mutex to stop the modem for its rail cycle, so holding it here would park
    // the reset behind this wait. Two callers that both pass the wait are
    // serialised by the lock below (the second joins the same event group).
    for (int i = 0; i < 150 && plus_audio_state() == PLUS_AUDIO_INITIALIZING; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_active.load() && s_ready.load()) return true;
    if (s_active.load()) {
        // A previous start is still running; wait on the same event group.
        if (!s_events) return false;
        const EventBits_t bits = xEventGroupWaitBits(s_events, kConnectedBit | kFailedBit, pdFALSE, pdFALSE,
                                                     pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : 60000));
        return (bits & kConnectedBit) != 0;
    }

    ensureNetif();
    if (s_events == nullptr) s_events = xEventGroupCreate();
    if (s_events == nullptr) return false;
    xEventGroupClearBits(s_events, kConnectedBit | kFailedBit);
    s_ready.store(false);

    // P14 is the shared 4G/BT/PA rail (high by default). Keep it high and give
    // the modem the vendor's 1 s settle before the first frame.
    epd_board_metalio_eink4_plus_aux_rail_set(true);
    vTaskDelay(pdMS_TO_TICKS(1000));

    UartEthModem::Config config = {};
    config.uart_num = kUart;
    config.baud_rate = kBaud;
    config.tx_pin = kTx;
    config.rx_pin = kRx;
    config.mrdy_pin = kMrdy;
    config.srdy_pin = kSrdy;

    auto modem = std::make_shared<UartEthModem>(config);
    modem->SetNetworkEventCallback([](UartEthModem::UartEthModemEvent event, const std::string& detail) {
        switch (event) {
            case UartEthModem::UartEthModemEvent::Connected:
                s_ready.store(true);
                if (s_events) xEventGroupSetBits(s_events, kConnectedBit);
                ESP_LOGI(kTag, "4G connected");
                break;
            case UartEthModem::UartEthModemEvent::Disconnected:
                s_ready.store(false);
                ESP_LOGW(kTag, "4G disconnected");
                break;
            case UartEthModem::UartEthModemEvent::Connecting:
                ESP_LOGI(kTag, "4G connecting");
                break;
            case UartEthModem::UartEthModemEvent::ErrorNoSim:
            case UartEthModem::UartEthModemEvent::ErrorRegistrationDenied:
            case UartEthModem::UartEthModemEvent::ErrorInitFailed:
                s_ready.store(false);
                if (s_events) xEventGroupSetBits(s_events, kFailedBit);
                ESP_LOGE(kTag, "4G error: %s%s%s",
                         UartEthModem::GetNetworkEventName(event), detail.empty() ? "" : " - ",
                         detail.c_str());
                break;
        }
    });

    s_active.store(true);
    s_modem = modem;
    const esp_err_t started = modem->Start();
    if (started != ESP_OK) {
        ESP_LOGE(kTag, "modem start failed: %s", esp_err_to_name(started));
        s_active.store(false);
        s_modem.reset();
        return false;
    }

    const EventBits_t bits = xEventGroupWaitBits(s_events, kConnectedBit | kFailedBit, pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : 60000));
    if ((bits & kConnectedBit) != 0 && s_ready.load()) return true;

    ESP_LOGW(kTag, "modem did not connect within %d ms (bits=0x%lx)", timeout_ms,
             static_cast<unsigned long>(bits));
    // Leave it stopped rather than half-initialised: the caller falls back to
    // reporting no network instead of pretending the radio works.
    if (s_modem) s_modem->Stop();
    s_modem.reset();
    s_active.store(false);
    return false;
}

void plus_cellular_stop(void) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_modem) {
        ESP_LOGI(kTag, "stopping modem");
        s_modem->Stop();
        s_modem.reset();
    }
    s_ready.store(false);
    s_active.store(false);
}

bool plus_cellular_ready(void) { return s_ready.load(); }
bool plus_cellular_active(void) { return s_active.load(); }

int plus_cellular_csq(void) {
    std::unique_lock<std::mutex> lock(s_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !s_modem || !s_ready.load()) return 99;
    return s_modem->GetSignalStrength();
}

bool plus_cellular_registered(void) {
    std::unique_lock<std::mutex> lock(s_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !s_modem || !s_ready.load()) return false;
    const int stat = s_modem->GetCellInfo().stat;
    return stat == 1 || stat == 5;
}

const char* plus_cellular_ip(void) {
    static char text[16] = {};
    text[0] = '\0';
    if (!s_ready.load()) return text;
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (netif == nullptr) return text;
    esp_netif_ip_info_t info = {};
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) return text;
    snprintf(text, sizeof(text), IPSTR, IP2STR(&info.ip));
    return text;
}

/* Strong definitions of the weak hooks in plus_audio.c. P14 is shared, so a
 * BH1098 power reset cuts the modem. The official board does not refuse audio
 * while 4G is up: it tears the modem down before the reset and re-initialises
 * it on the P14 rising edge (Nt26Board::PreparePowerOff /
 * RecoverAfterPowerOnAsync). Mirror that here instead of failing audio. */
namespace {
std::atomic<bool> sRailCycleHadModem{false};
std::atomic<bool> sRailRecoverActive{false};

void railRecoverTask(void*) {
    // P14 has just come back up: give the modem the vendor 1 s settle before
    // the UART/netif are recreated. Audio's own handshake runs concurrently.
    vTaskDelay(pdMS_TO_TICKS(1000));
    const bool ok = plus_cellular_start(45000);
    if (ok) {
        ESP_LOGI(kTag, "4G recovered after the BT power reset");
    } else {
        ESP_LOGE(kTag, "4G recovery after the BT power reset failed");
    }
    sRailRecoverActive.store(false);
    vTaskDelete(nullptr);
}
}  // namespace

extern "C" void plus_audio_prepare_rail_cycle(void) {
    const bool wasActive = plus_cellular_active() || plus_cellular_ready();
    sRailCycleHadModem.store(wasActive);
    if (!wasActive) return;
    ESP_LOGW(kTag, "BT power reset will cut P14: stopping the modem first");
    plus_cellular_stop();
}

extern "C" void plus_audio_rail_cycle_done(void) {
    if (!sRailCycleHadModem.exchange(false)) return;
    bool expected = false;
    if (!sRailRecoverActive.compare_exchange_strong(expected, true)) return;
    if (xTaskCreate(railRecoverTask, "cell-recover", 8192, nullptr, 3, nullptr) != pdPASS) {
        sRailRecoverActive.store(false);
        ESP_LOGE(kTag, "cannot start the 4G recovery task");
    }
}
