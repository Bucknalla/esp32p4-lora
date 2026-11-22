idf_component_register(
    SRCS "main.c" "board.c" "eth.c" "conc.c" "conc_hal.c" "wifi.c" "c6_update.c" "console.c" "pktfwd.c" "conc_tx.c" "ota.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_uart esp_wifi esp_timer esp_partition esp_hosted bootloader_support lwip esp_eth esp_netif esp_event
                  esp_psram esp_system esp_hw_support spi_flash nvs_flash esp_app_format app_update esp_https_ota esp_http_client mbedtls
                  espcoredump lwc_core lgw_port sx1301_hal sx1302_hal)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
