# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.1.1)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.1.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

# picotool：优先使用 .pico-sdk 自带的预编译版本。本机构建环境无网络/无宿主 C++ 编译器，
# 若交给 SDK 的 FetchContent 去拉取 picotool 源码会失败（git submodule 不可用）。
# 该设置仅在本机存在预编译 picotool 时生效，其他机器上自动跳过（无害）。
if(NOT picotool_DIR AND EXISTS "C:/Users/Administrator/.pico-sdk/tools-2.1.1/picotool/picotool")
    set(picotool_DIR "C:/Users/Administrator/.pico-sdk/tools-2.1.1/picotool/picotool" CACHE PATH "Prebuilt picotool from .pico-sdk" FORCE)
endif()

project(i2s_mic C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1
# add_executable(i2s_mic main.cpp i2s.cpp NeoPixelConnect.cpp)
add_executable(i2s_mic
    src/main.c
    src/usb_descriptors.c
    src/led.c
    src/rec_buffer.c
    src/i2s.c
)

# PIO 头文件已预生成并提交（src/i2s.pio.h 由 pioasm 生成；src/ws2812.pio.h 为手写版本），
# 因此此处不再调用 pico_generate_pio_header，避免在本构建环境（无宿主 C++ 编译器）下触发 pioasm 源码编译。
# 若后续在完整工具链环境下开发，可改回 pico_generate_pio_header 并删除已提交的 .pio.h。
# I²S 数据引脚固定为实际硬件接线：SD=GP7(Pin10) / SCK=GP8(Pin11) / WS=GP9(Pin12)
# （PIO 硬约束：SCK=SD+1、WS=SD+2，SD 必须是编号最小的 GPIO；备用板型 GP18/19/20 已舍弃）

pico_set_program_name(i2s_mic "i2s_mic")
pico_set_program_version(i2s_mic "0.6")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(i2s_mic 1)
pico_enable_stdio_usb(i2s_mic 0)

# Add the standard library to the build
target_link_libraries(i2s_mic hardware_i2c
    hardware_dma
    hardware_pio
    hardware_irq
    hardware_clocks
    tinyusb_device
    tinyusb_board
    pico_rand
    pico_stdlib)

# Add the standard include files to the build
target_include_directories(i2s_mic PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
)

pico_add_extra_outputs(i2s_mic)
