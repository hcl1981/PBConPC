# smc -- self-modifying code in SRAM, where only the code after a block changes: checks that
# PBConPC's JIT drops a translation whose flags analysis relied on that code (jit.inc jit_drop).
# Prints on UART0 (PBConPC: --console). Build: cmake -S . -B build && cmake --build build
cmake_minimum_required(VERSION 3.13)
set(PICO_PLATFORM rp2350-arm-s)
set(PICO_BOARD pico2)
include(pico_sdk_import.cmake)
project(smc C CXX ASM)
pico_sdk_init()
add_executable(smc smc.c)
target_link_libraries(smc pico_stdlib)
pico_add_extra_outputs(smc)
pico_enable_stdio_usb(smc 1)
pico_enable_stdio_uart(smc 1)
