cmake_minimum_required(VERSION 3.26)
if(PICO_SDK_PATH)
    include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
    project(pico_bno085_consumer_smoke LANGUAGES C CXX ASM)
    pico_sdk_init()
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
else()
    project(pico_bno085_consumer_smoke LANGUAGES CXX)
endif()

# Compile against the installed package to check headers and transitive
# dependencies. The lint sweep must skip this isolated consumer through
# .lintignore because it has no entry in the main compile database.
find_package(pico_bno085 REQUIRED)

if(PICO_SDK_PATH)
    add_executable(consumer_smoke firmware_smoke.cxx)
    target_link_libraries(consumer_smoke PRIVATE pico_bno085::hal)
else()
    add_executable(consumer_smoke consumer_smoke.cxx)
endif()

# Link each public module explicitly to detect a missing exported target.
target_link_libraries(consumer_smoke PRIVATE
    pico_bno085::core
    pico_bno085::driver
    pico_bno085::hal_core
    pico_bno085::testing
)

target_compile_features(consumer_smoke PRIVATE cxx_std_20)
