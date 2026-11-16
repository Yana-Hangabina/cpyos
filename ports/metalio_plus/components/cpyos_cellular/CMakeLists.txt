# 4G data path, ported from the official Metalio firmware
# (boards/common/board/uart_eth_modem.*, Apache-2.0) on top of 78/uart-uhci and
# espressif/iot_eth. The modem owns its esp_netif + DHCP, so the rest of CPYOS
# keeps using plain lwIP sockets.
idf_component_register(
    SRCS "PlusCellular.cc" "uart_eth_modem.cc"
    INCLUDE_DIRS "."
    REQUIRES uart_uhci iot_eth cpyos_metalio_bsp
             esp_netif esp_event esp_timer esp_hw_support esp_rom
             esp_driver_uart esp_driver_gpio driver lwip hal)
