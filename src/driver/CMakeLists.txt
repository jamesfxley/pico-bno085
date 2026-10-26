add_library(pico_bno085_driver STATIC
    bno085.cpp
)
add_library(pico_bno085::driver ALIAS pico_bno085_driver)

target_sources(pico_bno085_driver PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/pico_bno085/driver/bno085.hpp
)

# PRIVATE: build-time facts for the static_assert in bno085.cpp, not part of the
# interface. PUBLIC would propagate this build's version into every installed
# consumer's compile line. The umbrella header belongs to pico_bno085::core,
# which links this module rather than the other way round, so it is reached by a
# private include directory and not a link dependency.
house_cxx_version_definitions(pico_bno085_driver pico_bno085 PRIVATE)

target_include_directories(pico_bno085_driver PRIVATE
    $<BUILD_INTERFACE:${PICO_BNO085_PUBLIC_INCLUDE_DIR}>
)

target_include_directories(pico_bno085_driver PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pico_bno085_driver PUBLIC cxx_std_20)
target_link_libraries(pico_bno085_driver
    PUBLIC
        pico_sh2::core
        pico_logging::core
        pico_util::core
    PRIVATE
        pico_bno085_strict)

# This target builds for host and for firmware. Only the firmware configure
# drops -Werror from pico_bno085_strict, so only there does the flag have to be
# put back per source; on host it is already on the target.
if(PICO_BNO085_BUILD_FIRMWARE)
    house_cxx_werror_sources(bno085.cpp)
endif()
