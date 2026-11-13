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

# PRIVATE definitions bind the header version to project() without reaching
# installed consumers. The umbrella includes this module, so its header is
# reached through a private include path to avoid a circular dependency.
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

# Firmware needs per-source -Werror because SDK sources share the target.
if(PICO_BNO085_BUILD_FIRMWARE)
    house_cxx_werror_sources(bno085.cpp)
endif()
