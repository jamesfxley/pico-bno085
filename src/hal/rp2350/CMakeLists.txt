# Firmware-only I2C, SPI, GPIO and timer adapters.
add_library(pico_bno085_hal STATIC
    control_pins.cpp
    hintn_irq.cpp
    transport_core.cpp
    i2c_hal_transport.cpp
    spi_hal_transport.cpp
)
add_library(pico_bno085::hal ALIAS pico_bno085_hal)

target_sources(pico_bno085_hal PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/pico_bno085/hal/pins.hpp
        include/pico_bno085/hal/control_pins.hpp
        include/pico_bno085/hal/hintn_irq.hpp
        include/pico_bno085/hal/transport_core.hpp
        include/pico_bno085/hal/i2c_hal_transport.hpp
        include/pico_bno085/hal/spi_hal_transport.hpp
)

# The installed include prefix remains pico_bno085/hal/.
target_include_directories(pico_bno085_hal PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pico_bno085_hal PUBLIC cxx_std_20)
target_link_libraries(pico_bno085_hal
    PUBLIC
        pico_bno085::driver
        pico_bno085::hal_core
        pico_sh2::sh2
    PRIVATE
        pico_bno085_strict)

# SDK targets are not exported. The family helper wraps build links in
# BUILD_LOCAL_INTERFACE and restores them after the consumer's pico_sdk_init().
pico_family_sdk_link_libraries(pico_bno085_hal
    PUBLIC
        hardware_i2c
        hardware_spi
        hardware_gpio
        pico_stdlib
)

# Apply -Werror per owned source because SDK sources share the target.
# Source properties are scoped to the directory making this call.
house_cxx_werror_sources(
    control_pins.cpp
    hintn_irq.cpp
    transport_core.cpp
    i2c_hal_transport.cpp
    spi_hal_transport.cpp
)
