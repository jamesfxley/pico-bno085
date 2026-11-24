cmake_minimum_required(VERSION 3.26)

include(cmake/pico_family_bootstrap.cmake)

# pico-logging's lock-free atomics require RP2350 exclusive access (AR-5).
pico_family_bootstrap(PICO_BNO085 PLATFORM rp2350)

project(pico_bno085 VERSION 0.1.1 LANGUAGES C CXX ASM)

house_cxx_common_settings(${PICO_BNO085_BUILD_FIRMWARE})
pico_family_init_sdk(${PICO_BNO085_BUILD_FIRMWARE})

# SDK INTERFACE sources compile in the linking target. Firmware therefore
# applies -Werror per owned source; host targets take it directly.
house_cxx_add_strict_target(pico_bno085_strict
    VENDOR_SOURCES ${PICO_BNO085_BUILD_FIRMWARE}
    LANGUAGES CXX
)

# The profile selects pico-sh2 buffer and sensor capacities.
pico_family_declare_profile(PICO_BNO085 NAME PROFILE
    DEFAULT default VALUES default large
    DOC "pico-bno085 sizing profile")

# FORCE keeps driver and protocol capacities aligned. Warn before overriding
# a conflicting explicit pico-sh2 profile.
if(DEFINED CACHE{PICO_SH2_PROFILE}
   AND NOT "$CACHE{PICO_SH2_PROFILE}" STREQUAL "${PICO_BNO085_PROFILE}")
    message(WARNING
        "PICO_SH2_PROFILE was set to '$CACHE{PICO_SH2_PROFILE}' but is "
        "forced to '${PICO_BNO085_PROFILE}' by PICO_BNO085_PROFILE: this "
        "project sizes pico-sh2 from its own profile. Set PICO_BNO085_PROFILE "
        "instead.")
endif()

set(PICO_SH2_PROFILE "${PICO_BNO085_PROFILE}" CACHE STRING
    "pico-sh2 sizing profile" FORCE)

# Enable sanitizers before creating targets so GoogleTest and library code
# share instrumentation. Partial ASan container instrumentation can report
# false overflows.
house_cxx_enable_sanitizers(PICO_BNO085)

# Declare options before subdirectories read them.
house_cxx_declare_tests_option(PICO_BNO085)

house_cxx_declare_fuzzers_option(PICO_BNO085)

set(PICO_BNO085_ENABLE_EXAMPLES_DEFAULT OFF)
if(PROJECT_IS_TOP_LEVEL AND PICO_BNO085_BUILD_FIRMWARE)
    set(PICO_BNO085_ENABLE_EXAMPLES_DEFAULT ON)
endif()
option(PICO_BNO085_ENABLE_EXAMPLES "Build the firmware examples"
    ${PICO_BNO085_ENABLE_EXAMPLES_DEFAULT})

# Top-level builds own installation. Firmware SDK links use the family
# helper so the package remains exportable.
house_cxx_declare_install_option(PICO_BNO085)

# Linked dependencies must also be exported. Set installation options before
# fetching so dependency defaults cannot win.
pico_family_propagate_install(PICO_BNO085 PICO_SH2 PICO_LOGGING PICO_UTIL)

# Declare direct pins before population to resolve the dependency diamond.
pico_family_dependency(pico_sh2
    REPOSITORY https://github.com/jamesfxley/pico-sh2.git
    REVISION 1ead5c29413c462f97d81d9d48cb59fcc4a16558)
pico_family_dependency(pico_logging
    REPOSITORY https://github.com/jamesfxley/pico-logging.git
    REVISION 724f4555863d60fb68b1ddff944c0ce89398c16a)

# All consumers use pico_util::Result. Declare the pin before population.
# Discarded results produce a compiler diagnostic (pico_util:FR-6).
pico_family_dependency(pico_util
    REPOSITORY https://github.com/jamesfxley/pico-util.git
    REVISION 870e68b33012c9052d3336be4ad9abd58d44d4d1)

pico_family_make_available(pico_sh2 pico_logging pico_util)

add_subdirectory(src)

if(PICO_BNO085_ENABLE_TESTS)
    house_cxx_enable_gtest()
    add_subdirectory(tests)
endif()

# Host builds ignore the firmware example option.
if(PICO_BNO085_ENABLE_EXAMPLES AND PICO_BNO085_BUILD_FIRMWARE)
    add_subdirectory(examples)
endif()

if(PROJECT_IS_TOP_LEVEL AND PICO_BNO085_BUILD_FIRMWARE)
    add_subdirectory(tests/device)
endif()

if(PICO_BNO085_ENABLE_INSTALL)
    # Tracing support ships in host and firmware packages.
    set(PICO_BNO085_EXPORT_TARGETS
        pico_bno085_strict
        pico_bno085_core
        pico_bno085_driver
        pico_bno085_hal_core
        pico_bno085_testing
    )

    # Export the SDK transports only when their firmware target exists.
    if(PICO_BNO085_BUILD_FIRMWARE)
        list(APPEND PICO_BNO085_EXPORT_TARGETS pico_bno085_hal)
    endif()

    pico_family_sdk_config_snippet(PICO_BNO085_SDK_CONFIG TARGETS ${PICO_BNO085_EXPORT_TARGETS})
    house_cxx_install_package(
        PACKAGE pico_bno085 VERSION ${PROJECT_VERSION}
        CONFIG_SNIPPET "${PICO_BNO085_SDK_CONFIG}"
        NAMESPACE pico_bno085
        TARGETS ${PICO_BNO085_EXPORT_TARGETS}
        DEPENDS pico_sh2 pico_logging pico_util
    )
endif()
