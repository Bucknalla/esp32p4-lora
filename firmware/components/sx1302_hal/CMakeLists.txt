# Semtech sx1302_hal libloragw (V2.1.0, see README.md) on ESP-IDF.
# Built only when CONFIG_GW_CONC_SX1302 is set: sx1301_hal defines the same
# lgw_* symbols. Requirements stay unconditional (IDF resolves them before the
# config is loaded); only sources and include dirs depend on the choice.
set(lgw_src libloragw/src)
set(upstream_srcs
    # unmodified
    "${lgw_src}/loragw_reg.c" "${lgw_src}/loragw_sx1302.c" "${lgw_src}/loragw_sx1302_rx.c"
    "${lgw_src}/loragw_sx1302_timestamp.c" "${lgw_src}/loragw_sx1250.c"
    "${lgw_src}/loragw_sx125x.c" "${lgw_src}/loragw_sx1261.c" "${lgw_src}/loragw_cal.c"
    "${lgw_src}/loragw_debug.c" "${lgw_src}/loragw_lbt.c" "${lgw_src}/loragw_stts751.c"
    "${lgw_src}/loragw_ad5338r.c" "${lgw_src}/loragw_com.c" "${lgw_src}/sx1250_com.c"
    "${lgw_src}/sx125x_com.c" "${lgw_src}/sx1261_com.c" "libtools/src/tinymt32.c"
    # patched (patches/esp32p4-port.diff)
    "${lgw_src}/loragw_hal.c" "${lgw_src}/loragw_aux.c" "${lgw_src}/loragw_spi.c"
    "${lgw_src}/sx1250_spi.c" "${lgw_src}/sx125x_spi.c" "${lgw_src}/sx1261_spi.c")
# Not built: loragw_usb.c sx1250_usb.c sx1261_usb.c loragw_mcu.c (USB
# concentrators), loragw_gps.c (Class B out of scope), loragw_i2c.c (Linux
# i2c-dev; stubbed in port/lgw_sx1302_stubs.c).

set(srcs)
set(incs)
set(priv_incs)
if(CONFIG_GW_CONC_SX1302)
    set(srcs ${upstream_srcs} "port/lgw_sx1302_stubs.c")
    set(incs "libloragw/inc" "config")
    set(priv_incs "libtools/inc")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${incs}
    PRIV_INCLUDE_DIRS ${priv_incs}
    REQUIRES lgw_port)

if(CONFIG_GW_CONC_SX1302)
    # Third-party code: upstream warnings are not errors (ADR-0004/0005).
    set_source_files_properties(${upstream_srcs} PROPERTIES COMPILE_OPTIONS
        "-Wno-error;-Wno-format;-Wno-unterminated-string-initialization")
    set_source_files_properties("port/lgw_sx1302_stubs.c" PROPERTIES COMPILE_OPTIONS
        "-Wall;-Wextra;-Werror")
endif()
