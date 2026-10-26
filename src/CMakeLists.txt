set(PICO_BNO085_PUBLIC_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_subdirectory(driver)

# Test and diagnostic support, in the install export set alongside the driver.
# Built for firmware too -- the bench example drives it.
add_subdirectory(testing)

# The HAL's SDK-free core (split-read compensation, transfer classification)
# builds everywhere and is host-tested; the peripheral drivers inside it are
# firmware-only. src/hal/CMakeLists.txt makes that split.
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

# The HAL joins the umbrella only in firmware builds; host consumers get the
# platform-independent driver façade and supply their own transport.
if(PICO_BNO085_BUILD_FIRMWARE)
    target_link_libraries(pico_bno085_core INTERFACE pico_bno085::hal)
endif()
