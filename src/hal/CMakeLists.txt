# SDK-free, header-only HAL logic, tested on host.
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

# The SDK-backed transport module builds only for firmware.
if(PICO_BNO085_BUILD_FIRMWARE)
    add_subdirectory(rp2350)
endif()
