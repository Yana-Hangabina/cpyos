#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 4G data path for the Metalio E-INK4 Plus, ported from the official firmware:
 * UartEthModem (boards/common/board/uart_eth_modem.*) + uart_uhci (78/uart-uhci)
 * + espressif/iot_eth. The modem speaks a framed UART-Ethernet protocol on
 * UART2 at 2 Mbaud (TX58/RX59, MRDY60/SRDY3) and brings up its own esp_netif
 * with DHCP, so lwIP sockets / TLS work unchanged once it is ready.
 *
 * start() blocks until the modem reports Connected (or the timeout expires).
 * Call it from a worker, not the UI task.
 */
bool plus_cellular_start(int timeout_ms);
void plus_cellular_stop(void);
bool plus_cellular_ready(void);
bool plus_cellular_active(void);

/* Dotted IPv4 of the modem netif, or an empty string. */
const char* plus_cellular_ip(void);

/* Last CSQ (0..31; 99 = unknown). Returns 99 immediately while the modem is
 * starting/stopping, so a console probe can never race the data path. */
int plus_cellular_csq(void);

/* True when the modem reports home/roaming registration (CEREG stat 1 or 5).
 * Same try-lock rule as the CSQ probe. */
bool plus_cellular_registered(void);

#ifdef __cplusplus
}
#endif
