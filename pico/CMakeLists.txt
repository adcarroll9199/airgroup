cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico_w CACHE STRING "pico_w or pico2_w")
include(pico_sdk_import.cmake)
project(airgroup_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# Wi-Fi credentials and the speaker group live in config.local.cmake (git-ignored).
include(${CMAKE_CURRENT_LIST_DIR}/config.local.cmake OPTIONAL RESULT_VARIABLE HAVE_LOCAL_CONFIG)
if(NOT HAVE_LOCAL_CONFIG)
    message(FATAL_ERROR "Copy config.local.cmake.example to config.local.cmake and fill it in.")
endif()
foreach(var WIFI_SSID WIFI_PASSWORD CAST_TARGET)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} is not set in config.local.cmake")
    endif()
    # Escape for use inside a C string literal.
    string(REPLACE "\\" "\\\\" ${var}_C "${${var}}")
    string(REPLACE "\"" "\\\"" ${var}_C "${${var}_C}")
endforeach()
if(NOT DEFINED TEST_SECONDS)
    set(TEST_SECONDS 30)
endif()
# Generated into the build dir so secrets never appear on compiler command lines.
configure_file(src/config.h.in ${CMAKE_BINARY_DIR}/generated/config.h @ONLY)

add_executable(airgroup
    src/main.c
    src/log.c
    src/mdns_find.c
    src/castv2.c
    src/stream.c
)
target_include_directories(airgroup PRIVATE src ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(airgroup PRIVATE
    CYW43_HOST_NAME=\"airgroup-pico\"
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=3000
)
target_compile_options(airgroup PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(airgroup
    pico_stdlib
    pico_cyw43_arch_lwip_poll
    pico_lwip_mbedtls
    pico_mbedtls
)
pico_enable_stdio_usb(airgroup 1)
pico_enable_stdio_uart(airgroup 0)
pico_add_extra_outputs(airgroup)
