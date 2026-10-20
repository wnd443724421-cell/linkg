cmake_minimum_required(VERSION 3.12)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(linkg_supervisor VERSION 0.3.0 LANGUAGES C)
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

get_filename_component(LINKG_SUPERVISOR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
find_package(Threads REQUIRED)

set(SUPERVISOR_SOURCES
    src/supervisor_main.c
    src/supervisor_process.c
    src/supervisor_version.c
    src/supervisor_boot.c
    src/supervisor_links.c
    src/supervisor_release.c
    src/supervisor_storage.c
    "${LINKG_SUPERVISOR_ROOT}/infra/base/linkg_atomic_file.c"
    "${LINKG_SUPERVISOR_ROOT}/infra/base/linkg_release_state.c"
    "${LINKG_SUPERVISOR_ROOT}/infra/base/linkg_boot_env.c"
    "${LINKG_SUPERVISOR_ROOT}/third_party/cjson/cJSON.c"
)

add_executable(linkg-supervisor ${SUPERVISOR_SOURCES})
target_include_directories(linkg-supervisor PRIVATE include "${LINKG_SUPERVISOR_ROOT}/include/linkg/base" "${LINKG_SUPERVISOR_ROOT}/third_party/cjson")
target_compile_definitions(linkg-supervisor PRIVATE _GNU_SOURCE)
target_compile_options(linkg-supervisor PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS linkg-supervisor RUNTIME DESTINATION sbin)

option(LINKG_SUPERVISOR_BUILD_TESTS "Build isolated native Supervisor tests" OFF)

if(LINKG_SUPERVISOR_BUILD_TESTS)
    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "Supervisor execution tests require a native build")
    endif()

    enable_testing()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    add_library(supervisor-faults SHARED tests/supervisor_faults.c)
    target_compile_definitions(supervisor-faults PRIVATE _GNU_SOURCE)
    target_compile_options(supervisor-faults PRIVATE -Wall -Wextra -Wpedantic -Werror)

    add_library(supervisor-release-faults SHARED tests/release_faults.c)
    target_compile_definitions(supervisor-release-faults PRIVATE _GNU_SOURCE)
    target_compile_options(supervisor-release-faults PRIVATE -Wall -Wextra -Wpedantic -Werror)


    add_executable(linkg-supervisor-test ${SUPERVISOR_SOURCES})
    target_include_directories(linkg-supervisor-test PRIVATE include "${LINKG_SUPERVISOR_ROOT}/include/linkg/base" "${LINKG_SUPERVISOR_ROOT}/third_party/cjson")
    target_compile_definitions(linkg-supervisor-test PRIVATE _GNU_SOURCE LINKG_SUPERVISOR_TEST_BUILD)
    target_compile_options(linkg-supervisor-test PRIVATE -Wall -Wextra -Wpedantic -Werror)

    add_executable(linkg-supervisor-release-timing ${SUPERVISOR_SOURCES})
    target_include_directories(linkg-supervisor-release-timing PRIVATE include "${LINKG_SUPERVISOR_ROOT}/include/linkg/base" "${LINKG_SUPERVISOR_ROOT}/third_party/cjson")
    target_compile_definitions(linkg-supervisor-release-timing PRIVATE _GNU_SOURCE LINKG_SUPERVISOR_TEST_IO)
    target_compile_options(linkg-supervisor-release-timing PRIVATE -Wall -Wextra -Wpedantic -Werror)

    add_executable(supervisor-mock tests/supervisor_mock.c "${LINKG_SUPERVISOR_ROOT}/infra/base/linkg_supervisor_client.c")
    target_include_directories(supervisor-mock PRIVATE "${LINKG_SUPERVISOR_ROOT}/include/linkg/base")
    target_compile_definitions(supervisor-mock PRIVATE _GNU_SOURCE LINKG_SUPERVISOR_TEST_BUILD)
    target_compile_options(supervisor-mock PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(supervisor-mock PRIVATE Threads::Threads)

    add_executable(supervisor-mock-production tests/supervisor_mock.c "${LINKG_SUPERVISOR_ROOT}/infra/base/linkg_supervisor_client.c")
    target_include_directories(supervisor-mock-production PRIVATE "${LINKG_SUPERVISOR_ROOT}/include/linkg/base")
    target_compile_definitions(supervisor-mock-production PRIVATE _GNU_SOURCE)
    target_compile_options(supervisor-mock-production PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(supervisor-mock-production PRIVATE Threads::Threads)

    add_test(NAME supervisor-scenarios COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_supervisor.py" "$<TARGET_FILE:linkg-supervisor-test>" "$<TARGET_FILE:supervisor-mock>" "$<TARGET_FILE:supervisor-faults>")
    set_tests_properties(supervisor-scenarios PROPERTIES TIMEOUT 90)

    add_test(NAME supervisor-production-timing COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_supervisor.py" "$<TARGET_FILE:linkg-supervisor>" "$<TARGET_FILE:supervisor-mock-production>" "$<TARGET_FILE:supervisor-faults>" --production)
    set_tests_properties(supervisor-production-timing PROPERTIES TIMEOUT 150)

    add_test(NAME supervisor-release-scenarios COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_release.py" "$<TARGET_FILE:linkg-supervisor-test>" "$<TARGET_FILE:supervisor-release-faults>" "$<TARGET_FILE:supervisor-faults>")
    set_tests_properties(supervisor-release-scenarios PROPERTIES TIMEOUT 90)

    add_test(NAME supervisor-release-production-timing COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_release.py" "$<TARGET_FILE:linkg-supervisor-release-timing>" "$<TARGET_FILE:supervisor-release-faults>" "$<TARGET_FILE:supervisor-faults>" --production)
    set_tests_properties(supervisor-release-production-timing PROPERTIES TIMEOUT 120)

    add_test(NAME supervisor-env-lifecycle COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_release.py" "$<TARGET_FILE:linkg-supervisor-test>" "$<TARGET_FILE:supervisor-release-faults>" "$<TARGET_FILE:supervisor-faults>" --env-lifecycle)
    set_tests_properties(supervisor-env-lifecycle PROPERTIES TIMEOUT 150)
endif()
