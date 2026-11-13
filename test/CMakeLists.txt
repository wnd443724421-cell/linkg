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

add_executable(fake_iperf3 unit/fake_iperf3.c)
set_target_properties(fake_iperf3 PROPERTIES OUTPUT_NAME iperf3 RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake-bin")
add_linkg_logic_test(test_web_speedtest
    unit/test_web_speedtest.c
    "${LINKG_ROOT}/infra/base/linkg_thread.c"
    "${LINKG_ROOT}/infra/base/linkg_time.c"
)
target_include_directories(test_web_speedtest PRIVATE "${LINKG_ROOT}/service/web/speedtest")
add_dependencies(test_web_speedtest fake_iperf3)
set_tests_properties(test_web_speedtest PROPERTIES TIMEOUT 40 ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;PATH=${CMAKE_BINARY_DIR}/fake-bin:$ENV{PATH}")

add_linkg_logic_test(test_config_transfer
    unit/test_config_transfer.c
    "${LINKG_ROOT}/service/uart/uart_apply.c"
    "${LINKG_ROOT}/infra/config/linkg_config.c"
    "${LINKG_ROOT}/infra/config/config_codec.c"
    "${LINKG_ROOT}/service/web/handlers/web_handler_config.c"
    "${LINKG_ROOT}/service/web/web_response.c"
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

target_include_directories(test_config_transfer PRIVATE "${LINKG_ROOT}/service/web" "${LINKG_ROOT}/service/uart" "${LINKG_ROOT}/include/linkg/service/uart")
target_link_options(test_config_transfer PRIVATE -Wl,--wrap=linkg_file_write_all_atomic -Wl,--wrap=linkg_file_read_all -Wl,--wrap=linkg_time_elapsed_ms -Wl,--wrap=pthread_create)

add_linkg_logic_test(test_wifi_scan
    unit/test_wifi_scan.c
    "${LINKG_ROOT}/modules/wifi/wifi_scan_result.c"
)

add_linkg_logic_test(test_network_task
    unit/test_network_task.c
    "${LINKG_ROOT}/service/network/network_manager.c"
    "${LINKG_ROOT}/infra/base/linkg_thread.c"
    "${LINKG_ROOT}/infra/base/linkg_time.c"
)
target_include_directories(test_network_task PRIVATE "${LINKG_ROOT}/service/network" "${LINKG_ROOT}/include/linkg/service/network")
set_tests_properties(test_network_task PROPERTIES TIMEOUT 60)

add_linkg_logic_test(test_lifecycle_wakeup
    unit/test_lifecycle_wakeup.c
    "${LINKG_ROOT}/infra/base/linkg_lifecycle.c"
)
target_link_options(test_lifecycle_wakeup PRIVATE -Wl,--wrap=write -Wl,--wrap=poll)

add_linkg_logic_test(test_scan_bss_iteration
    unit/test_scan_bss_iteration.c
    "${LINKG_ROOT}/modules/wifi/wifi_scan_result.c"
)
target_compile_options(test_scan_bss_iteration PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_scan_bss_iteration PRIVATE -Wl,--gc-sections)
file(GLOB_RECURSE LINKG_SCAN_TEST_HEADERS "${LINKG_ROOT}/third_party/*/wpa_ctrl.h")
foreach(header IN LISTS LINKG_SCAN_TEST_HEADERS)
    get_filename_component(directory "${header}" DIRECTORY)
    target_include_directories(test_scan_bss_iteration PRIVATE "${directory}")
endforeach()

add_linkg_logic_test(test_scan_state
    unit/test_scan_state.c
    "${LINKG_ROOT}/service/network/network_scan.c"
    "${LINKG_ROOT}/modules/wifi/wifi_scan_result.c"
)
target_include_directories(test_scan_state PRIVATE "${LINKG_ROOT}/include/linkg/service/network")
