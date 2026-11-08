# fptest -- single-precision FPU work and edge cases, printed as bit patterns on UART0 (PBConPC:
# --console): for checking the JIT's FPU translation against the handler (PBCONPC_NO_FPOPT=1) and
# timing it. Build: cmake -S . -B build && cmake --build build
cmake_minimum_required(VERSION 3.13)
set(PICO_PLATFORM rp2350-arm-s)
set(PICO_BOARD pico2)
include(pico_sdk_import.cmake)
project(fptest C CXX ASM)
pico_sdk_init()
add_executable(fptest fptest.c)
target_compile_options(fptest PRIVATE -O2 -fno-math-errno -ffp-contract=off)
target_link_libraries(fptest pico_stdlib)
pico_add_extra_outputs(fptest)
pico_enable_stdio_usb(fptest 0)
pico_enable_stdio_uart(fptest 1)
