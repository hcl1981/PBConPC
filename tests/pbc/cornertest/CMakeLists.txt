# cornertest -- a firmware for the PicoBoy Color Plus that shows where the panel's visible area
# ends, especially how its corners are rounded, for measuring it with a camera
# (tools/cornertest_measure.py). Build: cmake -S . -B build && cmake --build build
cmake_minimum_required(VERSION 3.13)
set(PICO_PLATFORM rp2350-arm-s)
set(PICO_BOARD pico2)
include(pico_sdk_import.cmake)
project(cornertest C CXX ASM)
pico_sdk_init()
add_executable(cornertest cornertest.c)
target_link_libraries(cornertest pico_stdlib hardware_spi hardware_pwm)
pico_add_extra_outputs(cornertest)
pico_enable_stdio_usb(cornertest 1)
pico_enable_stdio_uart(cornertest 0)
