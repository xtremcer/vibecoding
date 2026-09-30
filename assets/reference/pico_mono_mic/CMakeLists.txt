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

project(i2s_mic C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1
# add_executable(i2s_mic main.cpp i2s.cpp NeoPixelConnect.cpp)
add_executable(i2s_mic main.c usb_descriptors.c led.c rec_buffer.c i2s.c)

pico_generate_pio_header(i2s_mic ${CMAKE_CURRENT_LIST_DIR}/i2s.pio)
pico_generate_pio_header(i2s_mic ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)

pico_set_program_name(i2s_mic "i2s_mic")
pico_set_program_version(i2s_mic "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(i2s_mic 1)
pico_enable_stdio_usb(i2s_mic 0)

# Add the standard library to the build
target_link_libraries(i2s_mic hardware_i2c
    hardware_dma
    hardware_pio
    hardware_clocks
    tinyusb_device
    tinyusb_board
    pico_rand
    pico_stdlib)

# Add the standard include files to the build
target_include_directories(i2s_mic PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

# 默认走标准流程（用 picotool 生成 .uf2）。
# 若环境无 picotool（如离线沙箱），可用 -DPICO_SKIP_EXTRA_OUTPUTS=1 跳过，
# 只产出 .elf，随后用 tools/elf2uf2.py 手动转 .uf2。
if (NOT PICO_SKIP_EXTRA_OUTPUTS)
    pico_add_extra_outputs(i2s_mic)
endif()
