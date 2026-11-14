set(PICO_BNO085_PUBLIC_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_subdirectory(driver)

# Installed diagnostic support, also used by the firmware benchmark.
add_subdirectory(testing)

# The SDK-free HAL core is host-tested. Its peripheral drivers build only
# for firmware.
add_subdirectory(hal)

add_library(pico_bno085_core INTERFACE)
add_library(pico_bno085::core ALIAS pico_bno085_core)

target_sources(pico_bno085_core INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${PICO_BNO085_PUBLIC_INCLUDE_DIR}
    FILES
        ${PICO_BNO085_PUBLIC_INCLUDE_DIR}/pico_bno085/pico_bno085.hpp
)

target_include_directories(pico_bno085_core INTERFACE
    $<BUILD_INTERFACE:${PICO_BNO085_PUBLIC_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pico_bno085_core INTERFACE cxx_std_20)

target_link_libraries(pico_bno085_core INTERFACE
    pico_bno085::driver
)

# Host consumers supply their own transport.
if(PICO_BNO085_BUILD_FIRMWARE)
    target_link_libraries(pico_bno085_core INTERFACE pico_bno085::hal)
endif()
