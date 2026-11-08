cmake_minimum_required(VERSION 3.16)
# PBConPC is a fork of picocalc-sim (MIT, Steve Gibson), see docs/upstream.
project(pbconpc C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(PICOCALC_SIM_VERSION "" CACHE STRING "Version to report, such as v1.2.3, for a build outside a git checkout; git describe when empty")
set(LWIP_DIR "" CACHE PATH "lwIP sources (src/core/tcp.c); found or fetched when empty")
option(PICOCALC_SIM_SDL "Build the SDL3 window frontend" ON)
option(PICOCALC_SIM_FETCH_SDL "Download and build SDL3 when it is not installed" ON)
set(PICOCALC_SIM_SDL3_SHA256 "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3" CACHE STRING "SHA-256 of the SDL3 release tarball")
set(IMGUI_DIR "" CACHE PATH "Dear ImGui sources (imgui.cpp); fetched when empty")
set(PICOCALC_SIM_IMGUI_SHA256 "21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99" CACHE STRING "SHA-256 of the Dear ImGui release tarball")

# PBConPC: a local Linux build is made for this computer's processor and optimised across files;
# together about 5 % faster (tools/build-pgo.sh adds profile-guided optimisation, about 4 % more).
# The Windows build stays portable.
option(PBCONPC_NATIVE "Optimise for this computer's processor (-march=native) and across files (LTO)" ON)
if(PBCONPC_NATIVE AND NOT WIN32 AND NOT CMAKE_CROSSCOMPILING)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT pbconpc_ipo OUTPUT pbconpc_ipo_msg LANGUAGES C)
    if(pbconpc_ipo)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    endif()
    add_compile_options(-march=native)
endif()

find_package(Threads REQUIRED)
include(cmake/Dependencies.cmake)

if(WIN32)
    # Windows 10 for WSAPoll and high-resolution timers, MinGW's own printf for C99 formats,
    # 64-bit file offsets for card images over 2 GB, and POSIX's localtime_r (in C only, as Dear
    # ImGui brings its own)
    add_compile_definitions(_WIN32_WINNT=0x0A00 __USE_MINGW_ANSI_STDIO=1 _FILE_OFFSET_BITS=64
                            $<$<COMPILE_LANGUAGE:C>:_POSIX_THREAD_SAFE_FUNCTIONS=200112L>)
    # each .exe then stands alone, without MinGW's runtime DLLs beside it
    add_link_options(-static)
endif()

set(LWIP_SRC ${LWIP_DIR}/src/core)
add_library(picocalc_sim_lwip STATIC
    ${LWIP_SRC}/def.c
    ${LWIP_SRC}/inet_chksum.c
    ${LWIP_SRC}/init.c
    ${LWIP_SRC}/ip.c
    ${LWIP_SRC}/mem.c
    ${LWIP_SRC}/memp.c
    ${LWIP_SRC}/netif.c
    ${LWIP_SRC}/pbuf.c
    ${LWIP_SRC}/stats.c
    ${LWIP_SRC}/sys.c
    ${LWIP_SRC}/tcp.c
    ${LWIP_SRC}/tcp_in.c
    ${LWIP_SRC}/tcp_out.c
    ${LWIP_SRC}/timeouts.c
    ${LWIP_SRC}/ipv4/icmp.c
    ${LWIP_SRC}/ipv4/ip4.c
    ${LWIP_SRC}/ipv4/ip4_addr.c
    ${LWIP_SRC}/ipv4/ip4_frag.c)
target_include_directories(picocalc_sim_lwip PUBLIC sim/net ${LWIP_DIR}/src/include)
target_compile_options(picocalc_sim_lwip PRIVATE -w)

# Everything but main(), so the Windows build can link two programs without compiling twice.
add_library(picocalc_sim_objs OBJECT
    emu/accessctrl.c
    emu/chip.c
    emu/cpu.c
    emu/cyw43.c
    emu/dcp.c
    emu/devices.c
    emu/dma.c
    emu/dw_i2c.c
    emu/gpio.c
    emu/jitmem.c
    emu/link.c
    emu/gdb.c
    emu/profile.c
    emu/hostprof.c
    emu/machine.c
    emu/pinwave.c
    emu/pio.c
    emu/pl011.c
    emu/pl022.c
    emu/pwm.c
    emu/qmi.c
    emu/rcp.c
    emu/rp2040.c
    emu/rom.c
    emu/sha256.c
    emu/sio.c
    emu/sys.c
    emu/timer.c
    emu/uf2.c
    emu/usb.c
    emu/usbhost.c
    emu/usbip.c
    emu/usbmsc.c
    sim/src/runtime.c
    sim/net/net.c
    sim/net/tcp.c
    sim/devices/audio.c
    sim/devices/esp_psram.c
    sim/devices/picoboy_board.c
    sim/devices/sdcard.c
    sim/devices/southbridge.c
    sim/devices/st7365p.c
    sim/devices/st7789.c
    frontend/keys.c
    frontend/keymap.c
    frontend/png.c
    frontend/script.c
    frontend/sdl.c
    frontend/skin.c
    frontend/serial.c
    frontend/settings.c
    frontend/terminal.c)
# Raspberry Pi's RP2350 boot ROMs from roms/rp2350, built in for --rp2350-rom A2 and A4.
set(PICOCALC_SIM_GENERATED ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PICOCALC_SIM_GENERATED})
foreach(rev A2 A4)
    string(TOLOWER ${rev} lower)
    set(rom ${CMAKE_SOURCE_DIR}/roms/rp2350/${rev}/bootrom-combined.bin)
    set(out ${PICOCALC_SIM_GENERATED}/rp2350_rom_${lower}.c)
    add_custom_command(OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -DIN=${rom} -DOUT=${out} -DNAME=rp2350_rom_${lower}
                -P ${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake
        DEPENDS ${rom} ${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake
        VERBATIM)
    target_sources(picocalc_sim_objs PRIVATE ${out})
endforeach()
# PBConPC: the render of the PicoBoy Color Plus the window draws around the LCD (assets/README.md)
set(skin_c ${PICOCALC_SIM_GENERATED}/pbc_front_png.c)
add_custom_command(OUTPUT ${skin_c}
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/assets/pbc_front.png -DOUT=${skin_c} -DNAME=pbc_front_png
            -P ${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/assets/pbc_front.png ${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake
    VERBATIM)
target_sources(picocalc_sim_objs PRIVATE ${skin_c})
set(version_c ${PICOCALC_SIM_GENERATED}/version.c)
add_custom_target(picocalc_sim_version
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${version_c}
            -DVERSION=${PICOCALC_SIM_VERSION} -P ${CMAKE_SOURCE_DIR}/cmake/Version.cmake
    BYPRODUCTS ${version_c}
    VERBATIM)
target_sources(picocalc_sim_objs PRIVATE ${version_c})
add_dependencies(picocalc_sim_objs picocalc_sim_version)
target_include_directories(picocalc_sim_objs PUBLIC emu sim/include)
target_compile_definitions(picocalc_sim_objs PRIVATE PBCONPC_TOOLS_DIR="${CMAKE_SOURCE_DIR}/tools")
target_compile_options(picocalc_sim_objs PUBLIC -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(picocalc_sim_objs PUBLIC picocalc_sim_lwip Threads::Threads m)
if(WIN32)
    target_link_libraries(picocalc_sim_objs PUBLIC ws2_32 iphlpapi winmm shell32)
endif()
if(PICOCALC_SIM_HAVE_SDL)
    target_sources(picocalc_sim_objs PRIVATE frontend/ui.cpp frontend/theme.cpp)
    target_compile_definitions(picocalc_sim_objs PRIVATE PICOCALC_SIM_HAVE_SDL=1)
    target_link_libraries(picocalc_sim_objs PUBLIC picocalc_sim_imgui)
    # the C++ runtime goes into the binary, which then still needs nothing beyond the C library
    if(NOT WIN32 AND NOT APPLE)
        target_link_options(picocalc_sim_objs PUBLIC -static-libstdc++ -static-libgcc)
    endif()
endif()

add_executable(pbconpc frontend/main.c)
target_link_libraries(pbconpc PRIVATE picocalc_sim_objs)
if(WIN32)
    # Started from Explorer, picocalc-sim.exe then opens no console window beside its own. A command
    # line neither waits for such a program nor shows its output, so picocalc-sim-console.exe is the
    # same program built for one.
    target_link_options(pbconpc PRIVATE -Wl,--subsystem,windows)
    add_executable(pbconpc-console frontend/main.c)
    target_link_libraries(pbconpc-console PRIVATE picocalc_sim_objs)
endif()

add_subdirectory(tools/mkcard)

# PBConPC tools
add_executable(uf2flash tools/uf2flash.c)
if(WIN32)
    # the window's "Connect USB to this PC" runs this beside pbconpc.exe
    configure_file(tools/pbc-usb-attach.ps1 ${CMAKE_BINARY_DIR}/pbc-usb-attach.ps1 COPYONLY)
endif()
