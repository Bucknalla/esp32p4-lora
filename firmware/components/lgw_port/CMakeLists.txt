# Platform layer shared by the libloragw ports (sx1301_hal, sx1302_hal):
# SPI (single bus owner, no DMA), delays, fixed temperature.
idf_component_register(
    SRCS "lgw_port_spi.c" "lgw_port_time.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_spi esp_rom freertos log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
