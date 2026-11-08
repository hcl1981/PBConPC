# tilttest -- reads the STK8BA58 as pbc-plus's MicroPython driver does (pbc_hw/accel.c) and prints
# the chip ID and the device's x, y, z in g on UART0 (PBConPC: --console, script command tilt).
# Build: cmake -S . -B build && cmake --build build
cmake_minimum_required(VERSION 3.13)
set(PICO_PLATFORM rp2350-arm-s)
set(PICO_BOARD pico2)
include(pico_sdk_import.cmake)
project(tilttest C CXX ASM)
pico_sdk_init()
add_executable(tilttest tilttest.c)
target_link_libraries(tilttest pico_stdlib hardware_i2c)
pico_add_extra_outputs(tilttest)
pico_enable_stdio_usb(tilttest 0)
pico_enable_stdio_uart(tilttest 1)
