# coretalk -- how fast the two cores answer each other: FIFO ping-pong, a contended spinlock and
# ping-pong through a flag in RAM, printed on UART0 (PBConPC: --console). For comparing chunk
# lengths in the emulator (machine.c) with the real chip. Build: cmake -S . -B build && cmake --build build
cmake_minimum_required(VERSION 3.13)
set(PICO_PLATFORM rp2350-arm-s)
set(PICO_BOARD pico2)
include(pico_sdk_import.cmake)
project(coretalk C CXX ASM)
pico_sdk_init()
add_executable(coretalk coretalk.c)
target_link_libraries(coretalk pico_stdlib pico_multicore hardware_sync)
pico_add_extra_outputs(coretalk)
pico_enable_stdio_usb(coretalk 1)
pico_enable_stdio_uart(coretalk 1)
