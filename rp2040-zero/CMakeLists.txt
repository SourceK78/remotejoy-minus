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
	mapping_config.c
	config_portal.c
	usb_descriptors.c
	# Profile model shared with the Pico 2 W build (same JSON format)
	${CMAKE_CURRENT_LIST_DIR}/../pico2w/config_model.c
	# DHCP server for the Web configuration mode's USB network
	${PICO_TINYUSB_PATH}/lib/networking/dhserver.c
)

target_include_directories(remotejoy_minus_rp2040 PRIVATE
	${CMAKE_CURRENT_LIST_DIR}
	${PICO_TINYUSB_PATH}/lib/networking
)

# The native USB port is a host (PSP) in normal mode and a CDC-NCM device
# (Web configuration) when L1+L2+Down is held at power-on, so both TinyUSB
# stacks are linked and one is initialized at boot.
target_link_libraries(remotejoy_minus_rp2040
	pico_stdlib
	hardware_adc
	hardware_flash
	hardware_pwm
	hardware_watchdog
	tinyusb_host
	tinyusb_device
	tinyusb_board
	pico_lwip_core
	pico_lwip_core4
	pico_lwip_arch
	pico_lwip_netif
)

pico_enable_stdio_usb(remotejoy_minus_rp2040 0)
pico_enable_stdio_uart(remotejoy_minus_rp2040 1)

pico_add_extra_outputs(remotejoy_minus_rp2040)
