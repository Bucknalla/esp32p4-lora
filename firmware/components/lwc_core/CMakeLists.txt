# Wraps the OS-independent core library (/core) as an ESP-IDF component.
# The sources stay in /core so the host build and the firmware share one copy.
set(core_dir "${CMAKE_CURRENT_LIST_DIR}/../../../core")

idf_component_register(
    SRCS "${core_dir}/src/frame.c" "${core_dir}/src/console_cmd.c" "${core_dir}/src/gwmp.c" "${core_dir}/src/txpk.c"
         "${core_dir}/src/logring.c" "${core_dir}/src/supervisor.c"
         "${core_dir}/src/session.c" "${core_dir}/src/form.c" "${core_dir}/src/netaddr.c"
    INCLUDE_DIRS "${core_dir}/include")

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
