# Host build of the OS-independent core library and its tests.
#   cmake -S core -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host
cmake_minimum_required(VERSION 3.16)
project(lwc_core C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(LWC_SANITIZE "Build with ASan + UBSan" ON)
option(LWC_FUZZ "Build libFuzzer targets (clang only)" OFF)

set(LWC_SOURCES src/frame.c src/console_cmd.c src/gwmp.c src/txpk.c src/logring.c src/supervisor.c)
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
add_executable(test_frame test/test_frame.c)
target_link_libraries(test_frame PRIVATE lwc_core)
target_compile_options(test_frame PRIVATE ${LWC_WARNINGS})
add_test(NAME frame COMMAND test_frame)

add_executable(test_console_cmd test/test_console_cmd.c)
target_link_libraries(test_console_cmd PRIVATE lwc_core)
target_compile_options(test_console_cmd PRIVATE ${LWC_WARNINGS})
add_test(NAME console_cmd COMMAND test_console_cmd)

add_executable(test_gwmp test/test_gwmp.c)
target_link_libraries(test_gwmp PRIVATE lwc_core)
target_compile_options(test_gwmp PRIVATE ${LWC_WARNINGS})
add_test(NAME gwmp COMMAND test_gwmp)

add_executable(test_txpk test/test_txpk.c)
target_link_libraries(test_txpk PRIVATE lwc_core)
target_compile_options(test_txpk PRIVATE ${LWC_WARNINGS})
add_test(NAME txpk COMMAND test_txpk)

foreach(t logring supervisor)
    add_executable(test_${t} test/test_${t}.c)
    target_link_libraries(test_${t} PRIVATE lwc_core)
    target_compile_options(test_${t} PRIVATE ${LWC_WARNINGS})
    add_test(NAME ${t} COMMAND test_${t})
endforeach()

if(LWC_FUZZ)
    add_executable(fuzz_frame fuzz/fuzz_frame.c ${LWC_SOURCES})
    target_include_directories(fuzz_frame PRIVATE include)
    target_compile_options(fuzz_frame PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_frame PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(fuzz_console_cmd fuzz/fuzz_console_cmd.c ${LWC_SOURCES})
    target_include_directories(fuzz_console_cmd PRIVATE include)
    target_compile_options(fuzz_console_cmd PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_console_cmd PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(fuzz_gwmp fuzz/fuzz_gwmp.c ${LWC_SOURCES})
    target_include_directories(fuzz_gwmp PRIVATE include)
    target_compile_options(fuzz_gwmp PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_gwmp PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(fuzz_txpk fuzz/fuzz_txpk.c ${LWC_SOURCES})
    target_include_directories(fuzz_txpk PRIVATE include)
    target_compile_options(fuzz_txpk PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_txpk PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(fuzz_logring fuzz/fuzz_logring.c ${LWC_SOURCES})
    target_include_directories(fuzz_logring PRIVATE include)
    target_compile_options(fuzz_logring PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_logring PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
