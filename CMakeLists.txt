cmake_minimum_required(VERSION 3.26)

include(cmake/pico_family_bootstrap.cmake)

# AR-5. pico-logging's lock-free atomics need the RP2350's exclusive access. As a
# dependency the board is the consumer's, and only that is checked of it.
pico_family_bootstrap(PICO_BNO085 PLATFORM rp2350)

project(pico_bno085 VERSION 0.1.0 LANGUAGES C CXX ASM)

house_cxx_common_settings(${PICO_BNO085_BUILD_FIRMWARE})
pico_family_init_sdk(${PICO_BNO085_BUILD_FIRMWARE})

# VENDOR_SOURCES takes the firmware flag: the Pico SDK attaches its own sources
# through INTERFACE libraries, so they compile into whichever target links them
# and a target-scoped -Werror would land on vendor code no consumer can fix. The
# firmware translation units this repo owns take it per source instead, via
# house_cxx_werror_sources() next to the sources it names. The host tree links
# no SDK sources, so there the target carries the flag directly. FREESTANDING
# follows the toolchain, which is the helper's default.
house_cxx_add_strict_target(pico_bno085_strict
    VENDOR_SOURCES ${PICO_BNO085_BUILD_FIRMWARE}
    LANGUAGES CXX
)

# Named sizing profile. "large" widens pico-sh2's reassembly buffer and
# enabled-sensor count; the profile's static_asserts live in pico-sh2's
# profile.hpp, and pico-sh2 exports the selected profile as a usage requirement,
# so this project's TUs inherit it by linking the library.
pico_family_declare_profile(PICO_BNO085 NAME PROFILE
    DEFAULT default VALUES default large
    DOC "pico-bno085 sizing profile")

# Forwarded to the pico-sh2 dependency (below) so the whole build -- driver,
# HAL, examples, and the protocol library -- is sized from one choice. The two
# projects name the same value set, and the helper above has already rejected
# anything outside it, so this passes the value through rather than mapping it.
#
# FORCE, so a tree configured before this line existed does not keep a stale
# value -- but FORCE also overwrites a value the developer set deliberately,
# and silently. Say so rather than inverting the request without a word.
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

# Host-only ASan+UBSan build, declared before the targets so the flags reach
# them. Directory-scoped, so the library, the tests and the fetched GoogleTest
# are all instrumented: ASan's container-overflow detector needs both sides of a
# std::string or std::vector boundary built with the same instrumentation, and
# partial coverage is the documented cause of false positives there.
house_cxx_enable_sanitizers(PICO_BNO085)

# Every knob this build has, declared before anything is added: a subdirectory
# reading an option that has not been declared yet reads an empty string.
house_cxx_declare_tests_option(PICO_BNO085)

house_cxx_declare_fuzzers_option(PICO_BNO085)

# Example firmware images are firmware-only.
set(PICO_BNO085_ENABLE_EXAMPLES_DEFAULT OFF)
if(PROJECT_IS_TOP_LEVEL AND PICO_BNO085_BUILD_FIRMWARE)
    set(PICO_BNO085_ENABLE_EXAMPLES_DEFAULT ON)
endif()
option(PICO_BNO085_ENABLE_EXAMPLES "Build the firmware examples"
    ${PICO_BNO085_ENABLE_EXAMPLES_DEFAULT})

# Install/export only when this is the top-level project: as a subproject the
# parent owns the export set. A firmware configure installs too: the HAL's SDK
# links go through pico_family_sdk_link_libraries, which is the form an export
# set can carry, so a firmware consumer can resolve this package through
# find_package rather than only through FetchContent.
house_cxx_declare_install_option(PICO_BNO085)

# When this project's targets are exported, the fetched dependencies they link
# must be in an export set too. Before the fetches below, or a dependency is
# configured with its default first.
pico_family_propagate_install(PICO_BNO085 PICO_SH2 PICO_LOGGING PICO_UTIL)

# pico-sh2 carries the platform-independent SH-2/SHTP protocol. All direct
# pins are declared before population, so these win the dependency diamond.
# pico-logging is the family logging dependency.
pico_family_dependency(pico_sh2
    REPOSITORY https://github.com/jamesfxley/pico-sh2.git
    REVISION 0bc759bbccf4ee14e6de08918a6bad450929a783)
pico_family_dependency(pico_logging
    REPOSITORY https://github.com/jamesfxley/pico-logging.git
    REVISION fe23964417288a0dedfa27ddbd7e2f85b0915bae)

# The driver's public Result is pico_util::Result<T, Error>, so this reaches
# every consumer; the bench example additionally arms HeapWatermark. Fetched
# here rather than under examples/ so its explicit pin is declared with the
# other direct dependencies before any are populated.
#
# pico_util:FR-6 makes discarded Result values a compiler diagnostic.
pico_family_dependency(pico_util
    REPOSITORY https://github.com/jamesfxley/pico-util.git
    REVISION 2afe3150a52429cb61c2330041cb4ec2e26c00ef)

pico_family_make_available(pico_sh2 pico_logging pico_util)

add_subdirectory(src)

if(PICO_BNO085_ENABLE_TESTS)
    house_cxx_enable_gtest()
    add_subdirectory(tests)
endif()

# A host configure with -DPICO_BNO085_ENABLE_EXAMPLES=ON is a silent no-op
# rather than an error: the images need the SDK.
if(PICO_BNO085_ENABLE_EXAMPLES AND PICO_BNO085_BUILD_FIRMWARE)
    add_subdirectory(examples)
endif()

if(PICO_BNO085_ENABLE_INSTALL)
    # pico_bno085_testing ships with the package. A downstream project writing
    # tests against this driver needs the same tracing decorator this
    # repository's own tests and bench image use; without it in the export set
    # it has to build from source or write its own, and a hand-written double
    # drifts from the seam it stands in for. The target is header-only and
    # built in both trees, so it is present wherever these rules run.
    set(PICO_BNO085_EXPORT_TARGETS
        pico_bno085_strict
        pico_bno085_core
        pico_bno085_driver
        pico_bno085_hal_core
        pico_bno085_testing
    )

    # The RP2350 transports exist only in a firmware build, so they join the
    # export set only there -- which is what lets a downstream firmware project
    # resolve this driver through find_package rather than only through
    # FetchContent. Naming a target that was never created fails the configure.
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
