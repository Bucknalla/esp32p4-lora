# Host build of the OS-independent core library and its tests.
#   cmake -S core -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host
cmake_minimum_required(VERSION 3.16)
project(lwc_core C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(LWC_SANITIZE "Build with ASan + UBSan" ON)
option(LWC_FUZZ "Build libFuzzer targets (clang only)" OFF)

set(LWC_SOURCES src/frame.c src/console_cmd.c src/txpk.c src/logring.c src/supervisor.c src/session.c src/form.c src/netaddr.c src/led.c src/aes.c src/lorawan.c src/ns.c src/hex.c src/mac.c
    src/json.c src/lbs.c src/plan.c src/mqtt.c src/dlq.c src/gps.c src/region.c)
set(LWC_WARNINGS -Wall -Wextra -Werror -Wshadow -Wconversion -Wstrict-prototypes)

add_library(lwc_core STATIC ${LWC_SOURCES})
target_include_directories(lwc_core PUBLIC include)
target_compile_options(lwc_core PRIVATE ${LWC_WARNINGS})

if(LWC_SANITIZE)
    set(SAN_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_compile_options(lwc_core PUBLIC ${SAN_FLAGS})
    target_link_options(lwc_core PUBLIC ${SAN_FLAGS})
endif()

enable_testing()
# EU868 golden snapshot (ADR-0023): the output before the region layer
add_executable(test_golden_eu868 test/test_golden_eu868.c)
target_link_libraries(test_golden_eu868 PRIVATE lwc_core)
target_compile_options(test_golden_eu868 PRIVATE ${LWC_WARNINGS})
add_test(NAME golden_eu868 COMMAND test_golden_eu868 ${CMAKE_CURRENT_SOURCE_DIR}/test/golden_eu868.txt)
add_executable(test_frame test/test_frame.c)
target_link_libraries(test_frame PRIVATE lwc_core)
target_compile_options(test_frame PRIVATE ${LWC_WARNINGS})
add_test(NAME frame COMMAND test_frame)

add_executable(test_console_cmd test/test_console_cmd.c)
target_link_libraries(test_console_cmd PRIVATE lwc_core)
target_compile_options(test_console_cmd PRIVATE ${LWC_WARNINGS})
add_test(NAME console_cmd COMMAND test_console_cmd)


add_executable(test_txpk test/test_txpk.c)
target_link_libraries(test_txpk PRIVATE lwc_core)
target_compile_options(test_txpk PRIVATE ${LWC_WARNINGS})
add_test(NAME txpk COMMAND test_txpk)

foreach(t logring supervisor session form netaddr led aes lorawan ns hex mac lbs plan mqtt dlq gps region)
    add_executable(test_${t} test/test_${t}.c)
    target_link_libraries(test_${t} PRIVATE lwc_core)
    target_compile_options(test_${t} PRIVATE ${LWC_WARNINGS})
    add_test(NAME ${t} COMMAND test_${t})
endforeach()

# ---- payload codecs (ADR-0021): QuickJS-NG, upstream v0.14.0 for host tests ----
# (the firmware uses Espressif's port of the same version, espressif/quickjs-ng)
option(LWC_CODEC "Build the JavaScript codec glue and its tests (fetches QuickJS-NG)" ON)
if(LWC_CODEC)
    include(FetchContent)
    FetchContent_Declare(quickjs_ng
        URL https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.14.0.tar.gz
        URL_HASH SHA256=928e9406addd99eb8623348f2cfcd916eade9a263c60d42be79bc7aee4ee8453
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR no-cmake) # sources only: its own CMake builds tools we do not need
    FetchContent_MakeAvailable(quickjs_ng)
    add_library(qjs STATIC ${quickjs_ng_SOURCE_DIR}/quickjs.c ${quickjs_ng_SOURCE_DIR}/libregexp.c
        ${quickjs_ng_SOURCE_DIR}/libunicode.c ${quickjs_ng_SOURCE_DIR}/dtoa.c)
    target_include_directories(qjs SYSTEM PUBLIC ${quickjs_ng_SOURCE_DIR})
    target_compile_definitions(qjs PRIVATE _GNU_SOURCE)
    set_target_properties(qjs PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    target_compile_options(qjs PRIVATE -w)
    if(LWC_SANITIZE)
        target_compile_options(qjs PUBLIC ${SAN_FLAGS})
        target_link_options(qjs PUBLIC ${SAN_FLAGS})
    endif()
    find_library(MATH_LIB m)
    if(MATH_LIB)
        target_link_libraries(qjs PUBLIC ${MATH_LIB})
    endif()
    find_package(Threads)
    if(Threads_FOUND)
        target_link_libraries(qjs PUBLIC Threads::Threads)
    endif()

    add_library(lwc_codec STATIC src/codec.c)
    target_include_directories(lwc_codec PUBLIC include)
    target_compile_options(lwc_codec PRIVATE ${LWC_WARNINGS})
    target_link_libraries(lwc_codec PUBLIC qjs)

    add_executable(test_codec test/test_codec.c)
    target_link_libraries(test_codec PRIVATE lwc_codec lwc_core)
    target_compile_options(test_codec PRIVATE ${LWC_WARNINGS})
    add_test(NAME codec COMMAND test_codec)
endif()

if(LWC_FUZZ)
    add_executable(fuzz_frame fuzz/fuzz_frame.c ${LWC_SOURCES})
    target_include_directories(fuzz_frame PRIVATE include)
    target_compile_options(fuzz_frame PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_frame PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(fuzz_console_cmd fuzz/fuzz_console_cmd.c ${LWC_SOURCES})
    target_include_directories(fuzz_console_cmd PRIVATE include)
    target_compile_options(fuzz_console_cmd PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_console_cmd PRIVATE -fsanitize=fuzzer,address,undefined)




    add_executable(fuzz_form fuzz/fuzz_form.c ${LWC_SOURCES})
    target_include_directories(fuzz_form PRIVATE include)
    target_compile_options(fuzz_form PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_form PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(fuzz_logring fuzz/fuzz_logring.c ${LWC_SOURCES})
    target_include_directories(fuzz_logring PRIVATE include)
    target_compile_options(fuzz_logring PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_logring PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(fuzz_lbs fuzz/fuzz_lbs.c ${LWC_SOURCES})
    target_include_directories(fuzz_lbs PRIVATE include)
    target_compile_options(fuzz_lbs PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_lbs PRIVATE -fsanitize=fuzzer,address,undefined)
    add_executable(fuzz_mqtt fuzz/fuzz_mqtt.c ${LWC_SOURCES})
    target_include_directories(fuzz_mqtt PRIVATE include)
    target_compile_options(fuzz_mqtt PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_mqtt PRIVATE -fsanitize=fuzzer,address,undefined)
    add_executable(fuzz_gps fuzz/fuzz_gps.c ${LWC_SOURCES})
    target_include_directories(fuzz_gps PRIVATE include)
    target_compile_options(fuzz_gps PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_gps PRIVATE -fsanitize=fuzzer,address,undefined)
    add_executable(fuzz_lorawan fuzz/fuzz_lorawan.c ${LWC_SOURCES})
    target_include_directories(fuzz_lorawan PRIVATE include)
    target_compile_options(fuzz_lorawan PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_lorawan PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
