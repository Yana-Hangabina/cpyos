idf_component_register(SRCS "iot_eth.c" "iot_eth_netif_glue.c"
                    INCLUDE_DIRS "include" "interface"
                    REQUIRES esp_netif)

# The upstream CMake derives these from cmake_utilities' cu_pkg_define_version;
# this port carries the component as plain source, so pin the version it was
# vendored from (idf_component.yml: 0.1.0).
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    IOT_ETH_VER_MAJOR=0 IOT_ETH_VER_MINOR=1 IOT_ETH_VER_PATCH=0)
