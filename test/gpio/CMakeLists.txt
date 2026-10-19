cmake_minimum_required(VERSION 3.13)
project(linkg_gpio_tests C)

enable_testing()
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_EXTENSIONS ON)

add_executable(linkg_gpio_tests test_gpio.c ../../platform/gpio/linkg_gpio.c)
target_include_directories(linkg_gpio_tests PRIVATE ../../include/linkg/platform/gpio)
target_compile_definitions(linkg_gpio_tests PRIVATE _GNU_SOURCE)
target_compile_options(linkg_gpio_tests PRIVATE -Wall -Wextra -Werror -Wmaybe-uninitialized)
target_link_options(linkg_gpio_tests PRIVATE -Wl,--wrap=open -Wl,--wrap=close -Wl,--wrap=ioctl -Wl,--wrap=fcntl)
add_test(NAME gpio_lifecycle COMMAND linkg_gpio_tests)
set_tests_properties(gpio_lifecycle PROPERTIES TIMEOUT 30)
