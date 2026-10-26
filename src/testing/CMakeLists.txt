# Test and diagnostic support, exported with the package.
#
# TracingTransport decorates the HAL seam to report the raw bytes of every
# transfer. Nothing in the driver uses it -- its callers are this repository's
# own tests, the bench example, and any downstream project testing against the
# same seam, which is why it ships rather than staying in-tree.
#
# Header-only, and built for firmware as well as host: the bench example is a
# firmware image and drives it behind its `trace 0|1` command.
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

# The decorator wraps pico_sh2's transport interface, which is where the seam
# it decorates is declared.
target_link_libraries(pico_bno085_testing INTERFACE pico_sh2::sh2)
