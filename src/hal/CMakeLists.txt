# The protocol-level parts of the HAL that touch no SDK: the split-read
# compensation, the transfer-code classification, the pin-mux rules and the strap
# order. Header-only and built on
# host as well as firmware, so the reasoning they carry is unit-tested rather
# than only exercised on a wired board.
add_library(pico_bno085_hal_core INTERFACE)
add_library(pico_bno085::hal_core ALIAS pico_bno085_hal_core)

target_sources(pico_bno085_hal_core INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/pico_bno085/hal/detail/pin_mux.hpp
        include/pico_bno085/hal/detail/strap_plan.hpp
        include/pico_bno085/hal/duplex_write.hpp
        include/pico_bno085/hal/edge_latch.hpp
        include/pico_bno085/hal/split_read.hpp
)

target_include_directories(pico_bno085_hal_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pico_bno085_hal_core INTERFACE cxx_std_20)
target_link_libraries(pico_bno085_hal_core INTERFACE pico_sh2::sh2)

# The rest of the HAL drives the SDK's I2C/GPIO/timer peripherals, so it is
# firmware-only and is a module of its own (rp2350/) rather than a conditional
# arm of this one: CXX-13.1 puts one target in one directory, and the two
# differ in what they link and where they can run.
if(PICO_BNO085_BUILD_FIRMWARE)
    add_subdirectory(rp2350)
endif()
