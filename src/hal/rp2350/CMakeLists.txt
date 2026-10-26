# The SDK-backed half of the HAL: the I2C and SPI transports, the strap and
# reset lines, and the HINTN interrupt. Firmware-only, because it is the part
# that touches the RP2350 peripherals -- which is why it is a target of its own
# rather than a conditional arm of pico_bno085::hal_core.
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

# The nesting under rp2350/ is invisible to a consumer: this exports the same
# pico_bno085/hal/<name>.hpp spelling the core does (CXX-13.3).
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

# The SDK peripherals go through this rather than target_link_libraries, because
# a target linking them cannot be exported: no SDK target is in any export set,
# and install(EXPORT) refuses a target whose link interface names one. They are
# wrapped in $<BUILD_LOCAL_INTERFACE:> and re-applied by the installed config
# file, once the consumer's own pico_sdk_init() has created them.
# pico-tooling's exportability requirement, cited by description because AR is
# this repository's own prefix (EN-02.8).
pico_family_sdk_link_libraries(pico_bno085_hal
    PUBLIC
        hardware_i2c
        hardware_spi
        hardware_gpio
        pico_stdlib
)

# This tree is firmware-only, so pico_bno085_strict always carries WERROR OFF
# here; the SDK compiles its own sources into this target and cannot take it.
# The call belongs beside the target: it sets a source property, and those are
# scoped to the directory that sets them.
house_cxx_werror_sources(
    control_pins.cpp
    hintn_irq.cpp
    transport_core.cpp
    i2c_hal_transport.cpp
    spi_hal_transport.cpp
)
