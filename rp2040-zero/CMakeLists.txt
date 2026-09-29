cmake_minimum_required(VERSION 3.13)

if(DEFINED PICO_SDK_PATH)
	include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
elseif(DEFINED ENV{PICO_SDK_PATH})
	include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
else()
	message(FATAL_ERROR "PICO_SDK_PATH is not set")
endif()

project(remotejoy_minus_rp2040 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(remotejoy_minus_rp2040
	standalone_usbhost.c
)

target_include_directories(remotejoy_minus_rp2040 PRIVATE
	${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(remotejoy_minus_rp2040
	pico_stdlib
	hardware_adc
	tinyusb_host
	tinyusb_board
)

pico_enable_stdio_usb(remotejoy_minus_rp2040 0)
pico_enable_stdio_uart(remotejoy_minus_rp2040 1)

pico_add_extra_outputs(remotejoy_minus_rp2040)
