# Installed, header-only tracing support for host tests and firmware benches.
add_library(pico_bno085_testing INTERFACE)
add_library(pico_bno085::testing ALIAS pico_bno085_testing)

target_sources(pico_bno085_testing INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/pico_bno085/testing/tracing_transport.hpp
)

target_include_directories(pico_bno085_testing INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pico_bno085_testing INTERFACE cxx_std_20)

# The transport interface is declared by pico-sh2.
target_link_libraries(pico_bno085_testing INTERFACE pico_sh2::sh2)
