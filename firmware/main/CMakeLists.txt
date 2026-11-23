idf_component_register(
    SRCS "main.c" "board.c" "eth.c" "conc.c" "conc_hal.c" "wifi.c" "c6_update.c" "console.c" "conc_tx.c" "ota.c" "fault.c" "logbuf.c" "sv.c" "web.c" "webauth.c" "led.c" "ble_prov.c" "gwmode.c" "lns.c" "lbsfwd.c" "radioplan.c" "tasks.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_uart esp_wifi esp_timer esp_partition esp_hosted bootloader_support lwip esp_eth esp_netif esp_event
                  esp_psram esp_system esp_hw_support spi_flash nvs_flash esp_app_format app_update esp_https_ota esp_http_client mbedtls esp_mm log freertos esp_https_server espressif__qrcode espressif__mdns
                  espressif__network_provisioning espressif__esp_websocket_client protocomm bt
                  espcoredump lwc_core lgw_port sx1301_hal sx1302_hal)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

# Web UI: gzip the static files at build time and embed them (web.c).
foreach(f index.html app.js app.css)
    set(gz "${CMAKE_CURRENT_BINARY_DIR}/${f}.gz")
    add_custom_command(OUTPUT "${gz}"
        COMMAND sh -c "gzip -9 -n -c \"$1\" > \"$2\"" gzip "${CMAKE_CURRENT_SOURCE_DIR}/www/${f}" "${gz}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/www/${f}" VERBATIM)
    target_add_binary_data(${COMPONENT_TARGET} "${gz}" BINARY)
    list(APPEND web_gz "${gz}")
endforeach()
add_custom_target(web_gz DEPENDS ${web_gz})
add_dependencies(${COMPONENT_LIB} web_gz)
