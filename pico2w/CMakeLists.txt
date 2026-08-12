cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w CACHE STRING "Target board")
set(BLUEPAD32_ROOT ${CMAKE_CURRENT_LIST_DIR}/external/bluepad32 CACHE PATH "Bluepad32 root")
set(BTSTACK_ROOT ${PICO_SDK_PATH}/lib/btstack)

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(remotejoy_minus_pico2w_config C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

# Bluepad32 itself includes these application-level configuration headers.
include_directories(${CMAKE_CURRENT_LIST_DIR})
include_directories(${BLUEPAD32_ROOT}/examples/pico_w/src)
include_directories(${BTSTACK_ROOT}/src)
include_directories(${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/include)
include_directories(${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/include)

add_executable(remotejoy_minus_pico2w_config
    main.c
    bluepad_platform.c
    config_portal.c
    config_store.c
    runtime_ui.c
    config_model.c
    psp_host.c
    ${CMAKE_CURRENT_LIST_DIR}/third_party/pico-examples/access_point/dhcpserver.c
    ${CMAKE_CURRENT_LIST_DIR}/third_party/pico-examples/access_point/dnsserver.c
)

target_include_directories(remotejoy_minus_pico2w_config PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../rp2040-zero
    ${CMAKE_CURRENT_LIST_DIR}/third_party/pico-examples/access_point
    ${BLUEPAD32_ROOT}/examples/pico_w/src
    ${BLUEPAD32_ROOT}/src/components/bluepad32/include
    ${BTSTACK_ROOT}/src
    ${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/include
    ${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/include
)

add_subdirectory(${BLUEPAD32_ROOT}/src/components/bluepad32 libbluepad32)

# Bluepad32's stock Pico target selects pico_cyw43_arch_none. This application
# also runs lwIP, so both Bluepad32 and the portal must share one CYW43 arch.
get_target_property(BLUEPAD32_LINK_LIBRARIES bluepad32 LINK_LIBRARIES)
list(REMOVE_ITEM BLUEPAD32_LINK_LIBRARIES pico_cyw43_arch_none)
set_property(TARGET bluepad32 PROPERTY LINK_LIBRARIES "${BLUEPAD32_LINK_LIBRARIES}")
get_target_property(BLUEPAD32_INTERFACE_LIBRARIES bluepad32 INTERFACE_LINK_LIBRARIES)
list(REMOVE_ITEM BLUEPAD32_INTERFACE_LIBRARIES pico_cyw43_arch_none)
set_property(TARGET bluepad32 PROPERTY INTERFACE_LINK_LIBRARIES "${BLUEPAD32_INTERFACE_LIBRARIES}")
target_link_libraries(bluepad32 pico_cyw43_arch_lwip_threadsafe_background)

target_link_libraries(remotejoy_minus_pico2w_config PRIVATE
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_core
    pico_btstack_classic
    pico_btstack_cyw43
    hardware_pwm
    tinyusb_host
    tinyusb_board
    bluepad32
)

pico_enable_stdio_usb(remotejoy_minus_pico2w_config 0)
pico_enable_stdio_uart(remotejoy_minus_pico2w_config 0)
pico_add_extra_outputs(remotejoy_minus_pico2w_config)
