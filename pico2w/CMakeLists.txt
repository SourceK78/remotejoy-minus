cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w CACHE STRING "Target board")
set(JOYPAD_OS_ROOT "${CMAKE_CURRENT_LIST_DIR}/external/joypad-os" CACHE PATH "joypad-os root")

if(NOT EXISTS "${JOYPAD_OS_ROOT}/src/bt/btstack/btstack_host.c")
    message(FATAL_ERROR "joypad-os was not found at JOYPAD_OS_ROOT=${JOYPAD_OS_ROOT}")
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(remotejoy_minus_pico2w C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

set(JP_SRC "${JOYPAD_OS_ROOT}/src")
set(BTSTACK_ROOT "${PICO_SDK_PATH}/lib/btstack")

add_executable(remotejoy_minus_pico2w
    main.c
    joypad_backend.c
    joypad_compat.c
    joypad_registry.c
    config_portal.c
    config_store.c
    runtime_ui.c
    gyro_aim.c
    config_model.c
    psp_host.c
    third_party/pico-examples/access_point/dhcpserver.c
    third_party/pico-examples/access_point/dnsserver.c
    ${JP_SRC}/bt/btstack/btstack_host.c
    ${JP_SRC}/bt/btstack/bt_device_db.c
    ${JP_SRC}/bt/transport/bt_transport.c
    ${JP_SRC}/bt/transport/bt_transport_cyw43.c
    ${JP_SRC}/bt/bthid/bthid.c
    ${JP_SRC}/bt/bthid/devices/generic/bthid_gamepad.c
    ${JP_SRC}/bt/bthid/devices/vendors/sony/ds3_bt.c
    ${JP_SRC}/bt/bthid/devices/vendors/sony/ds4_bt.c
    ${JP_SRC}/bt/bthid/devices/vendors/sony/ds5_bt.c
    ${JP_SRC}/usb/usbh/hid/devices/generic/hid_parser.c
)

target_compile_definitions(remotejoy_minus_pico2w PRIVATE
    CONFIG_BT_HOST=1
    ENABLE_BTSTACK=1
    BTSTACK_USE_CYW43=1
    RJM_JOYPAD_OS_BACKEND=1
    DISABLE_USB_HOST=1
    CYW43_LWIP=1
)

target_include_directories(remotejoy_minus_pico2w PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../rp2040-zero
    ${CMAKE_CURRENT_LIST_DIR}/third_party/pico-examples/access_point
    ${JP_SRC}
    ${JP_SRC}/bt
    ${JP_SRC}/bt/btstack
    ${JP_SRC}/bt/bthid
    ${BTSTACK_ROOT}/src
    ${BTSTACK_ROOT}/3rd-party/micro-ecc
    ${BTSTACK_ROOT}/3rd-party/rijndael
)

target_link_libraries(remotejoy_minus_pico2w PRIVATE
    pico_stdlib
    pico_rand
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_core
    pico_btstack_ble
    pico_btstack_classic
    pico_btstack_cyw43
    hardware_pwm
    tinyusb_host
    tinyusb_board
)

pico_enable_stdio_usb(remotejoy_minus_pico2w 0)
pico_enable_stdio_uart(remotejoy_minus_pico2w 0)
pico_add_extra_outputs(remotejoy_minus_pico2w)
