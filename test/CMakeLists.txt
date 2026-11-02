cmake_minimum_required(VERSION 3.16)

project(linkg_v1_logic_tests VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

get_filename_component(LINKG_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

enable_testing()
find_package(Threads REQUIRED)
option(LINKG_TEST_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" ON)

set(LINKG_TEST_INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/common"
    "${LINKG_ROOT}/include/linkg/base"
    "${LINKG_ROOT}/include/linkg/config"
    "${LINKG_ROOT}/include/linkg/core/discovery"
    "${LINKG_ROOT}/include/linkg/core/link"
    "${LINKG_ROOT}/include/linkg/core/nat"
    "${LINKG_ROOT}/include/linkg/core/node"
    "${LINKG_ROOT}/include/linkg/core/packet"
    "${LINKG_ROOT}/include/linkg/core/probe"
    "${LINKG_ROOT}/include/linkg/core/route"
    "${LINKG_ROOT}/include/linkg/core/scheduler"
    "${LINKG_ROOT}/include/linkg/core/statistics"
    "${LINKG_ROOT}/include/linkg/core/switch"
    "${LINKG_ROOT}/include/linkg/core/transport"
    "${LINKG_ROOT}/include/linkg/core/tun"
    "${LINKG_ROOT}/include/linkg/core/udhcp"
    "${LINKG_ROOT}/include/linkg/modules/cellular"
    "${LINKG_ROOT}/include/linkg/modules/wifi"
    "${LINKG_ROOT}/include/linkg/platform"
    "${LINKG_ROOT}/include/linkg/platform/uart"
    "${LINKG_ROOT}/core/discovery"
    "${LINKG_ROOT}/core/probe"
    "${LINKG_ROOT}/core/switch"
    "${LINKG_ROOT}/core/transport"
    "${LINKG_ROOT}/infra/config"
    "${LINKG_ROOT}/modules/wifi"
    "${LINKG_ROOT}/platform/wifi"
    "${LINKG_ROOT}/third_party/cjson"
)

function(add_linkg_logic_test target)
    add_executable(${target}
        ${ARGN}
        "${CMAKE_CURRENT_LIST_DIR}/common/test_common.c"
    )
    target_include_directories(${target} PRIVATE ${LINKG_TEST_INCLUDE_DIRS})
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
    if(LINKG_TEST_SANITIZERS)
        target_compile_options(${target} PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=address,undefined
        )
        target_link_options(${target} PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=address,undefined
        )
    endif()
    target_link_libraries(${target} PRIVATE Threads::Threads m)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES
        ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"
        TIMEOUT 20
    )
endfunction()

set_source_files_properties(
    "${LINKG_ROOT}/third_party/cjson/cJSON.c"
    PROPERTIES
    COMPILE_OPTIONS "-Wno-error;-Wno-misleading-indentation;-Wno-parentheses;-Wno-implicit-fallthrough;-Wno-unused-function"
)

add_linkg_logic_test(test_base
    unit/test_base.c
    "${LINKG_ROOT}/infra/base/linkg_atomic_file.c"
    "${LINKG_ROOT}/infra/base/linkg_file.c"
    "${LINKG_ROOT}/infra/base/linkg_text.c"
    "${LINKG_ROOT}/infra/base/linkg_time.c"
)

add_linkg_logic_test(test_packet_pool
    unit/test_packet_pool.c
    "${LINKG_ROOT}/core/packet/linkg_packet_pool.c"
)

add_linkg_logic_test(test_node_path
    unit/test_node_path.c
    "${LINKG_ROOT}/core/link/linkg_path.c"
    "${LINKG_ROOT}/core/node/linkg_node.c"
    "${LINKG_ROOT}/core/statistics/linkg_path_stats.c"
    "${LINKG_ROOT}/infra/base/linkg_file.c"
    "${LINKG_ROOT}/infra/base/linkg_network_ops.c"
    "${LINKG_ROOT}/infra/base/linkg_time.c"
)

add_linkg_logic_test(test_transport
    unit/test_transport.c
    "${LINKG_ROOT}/core/packet/linkg_packet_pool.c"
    "${LINKG_ROOT}/core/transport/transport_reassembly.c"
    "${LINKG_ROOT}/core/transport/transport_window.c"
    "${LINKG_ROOT}/core/transport/transport_wire.c"
    "${LINKG_ROOT}/infra/base/linkg_time.c"
)

add_linkg_logic_test(test_wire_protocols
    unit/test_wire_protocols.c
    "${LINKG_ROOT}/core/discovery/discovery_wire.c"
    "${LINKG_ROOT}/core/packet/linkg_packet_pool.c"
    "${LINKG_ROOT}/core/probe/path_probe_wire.c"
)

add_linkg_logic_test(test_switch_wire
    unit/test_switch_wire.c
    "${LINKG_ROOT}/core/switch/switch_wire.c"
)

add_linkg_logic_test(test_switch_verify
    unit/test_switch_verify.c
    "${LINKG_ROOT}/core/switch/switch_policy_verify.c"
)

add_linkg_logic_test(test_config
    unit/test_config.c
    "${LINKG_ROOT}/infra/base/linkg_file.c"
    "${LINKG_ROOT}/infra/base/linkg_json.c"
    "${LINKG_ROOT}/infra/base/linkg_network_ops.c"
    "${LINKG_ROOT}/infra/base/linkg_time.c"
    "${LINKG_ROOT}/infra/config/config_internal.c"
    "${LINKG_ROOT}/infra/config/linkg_cellular_config.c"
    "${LINKG_ROOT}/infra/config/linkg_device_config.c"
    "${LINKG_ROOT}/infra/config/linkg_links_config.c"
    "${LINKG_ROOT}/infra/config/linkg_network_config.c"
    "${LINKG_ROOT}/infra/config/linkg_paths_config.c"
    "${LINKG_ROOT}/infra/config/linkg_uart_config.c"
    "${LINKG_ROOT}/infra/config/linkg_wifi_config.c"
    "${LINKG_ROOT}/modules/wifi/linkg_wifi_ops.c"
    "${LINKG_ROOT}/third_party/cjson/cJSON.c"
)
