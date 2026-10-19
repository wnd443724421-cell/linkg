cmake_minimum_required(VERSION 3.12)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(linkg_supervisor VERSION 0.1.0 LANGUAGES C)
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

get_filename_component(LINKG_SUPERVISOR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
find_package(Threads REQUIRED)

add_executable(linkg-supervisor src/supervisor_main.c src/supervisor_process.c)
target_include_directories(linkg-supervisor PRIVATE include "${LINKG_SUPERVISOR_ROOT}/include/linkg/base")
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

    add_executable(linkg-supervisor-test src/supervisor_main.c src/supervisor_process.c)
    target_include_directories(linkg-supervisor-test PRIVATE include "${LINKG_SUPERVISOR_ROOT}/include/linkg/base")
    target_compile_definitions(linkg-supervisor-test PRIVATE _GNU_SOURCE LINKG_SUPERVISOR_TEST_BUILD)
    target_compile_options(linkg-supervisor-test PRIVATE -Wall -Wextra -Wpedantic -Werror)

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

    add_test(NAME supervisor-scenarios COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_supervisor.py" "$<TARGET_FILE:linkg-supervisor-test>" "$<TARGET_FILE:supervisor-mock>")
    set_tests_properties(supervisor-scenarios PROPERTIES TIMEOUT 90)

    add_test(NAME supervisor-production-timing COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_supervisor.py" "$<TARGET_FILE:linkg-supervisor>" "$<TARGET_FILE:supervisor-mock-production>" --production)
    set_tests_properties(supervisor-production-timing PROPERTIES TIMEOUT 150)
endif()
