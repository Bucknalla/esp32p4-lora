# Semtech lora_gateway libloragw (SX1301, v5.0.1, see README.md) on ESP-IDF.
# Built only when CONFIG_GW_CONC_SX1301 is set: sx1302_hal defines the same
# lgw_* symbols. Requirements stay unconditional; only sources and include
# dirs depend on the choice.
set(lgw_src libloragw/src)
set(upstream_srcs
    # unmodified
    "${lgw_src}/loragw_hal.c" "${lgw_src}/loragw_reg.c" "${lgw_src}/loragw_radio.c"
    "${lgw_src}/loragw_fpga.c" "${lgw_src}/loragw_lbt.c"
    # patched (patches/esp32p4-port.diff)
    "${lgw_src}/loragw_spi.native.c" "${lgw_src}/loragw_aux.c")
# Not built: loragw_gps.c (Class B out of scope).

set(srcs)
set(incs)
if(CONFIG_GW_CONC_SX1301)
    set(srcs ${upstream_srcs})
    set(incs "libloragw/inc" "config")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${incs}
    REQUIRES lgw_port)

if(CONFIG_GW_CONC_SX1301)
    # Third-party code: upstream warnings are not errors (ADR-0005).
    set_source_files_properties(${upstream_srcs} PROPERTIES COMPILE_OPTIONS
        "-Wno-error;-Wno-format")
endif()
