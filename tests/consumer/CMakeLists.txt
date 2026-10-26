cmake_minimum_required(VERSION 3.26)
if(PICO_SDK_PATH)
    include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
    project(pico_bno085_consumer_smoke LANGUAGES C CXX ASM)
    pico_sdk_init()
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
else()
    project(pico_bno085_consumer_smoke LANGUAGES CXX)
endif()

# Stands in for a downstream project consuming an installed pico-bno085. Its
# purpose is to prove the package is reachable by find_package, that the driver
# façade's headers ship, and that the transitive dependencies resolve --
# pico_bno085Config.cmake calls find_dependency(pico_sh2) and
# find_dependency(pico_logging) and find_dependency(pico_util), and the façade's
# own headers include pico_sh2's.
# None of that can fail in-tree, where every dependency is a live subproject.
#
# This translation unit compiles only against an installed package, so it has no
# entry in the merged compile database and the repo-wide clang-tidy sweep must
# skip it. The root .lintignore is what does that; the .cxx extension does not,
# since the lint gate enumerates .cxx alongside .cpp and .cc.
find_package(pico_bno085 REQUIRED)

if(PICO_SDK_PATH)
    add_executable(consumer_smoke firmware_smoke.cxx)
    target_link_libraries(consumer_smoke PRIVATE pico_bno085::hal)
else()
    add_executable(consumer_smoke consumer_smoke.cxx)
endif()

# pico_bno085::testing is linked as well as pico_bno085::core: the tracing
# decorator is part of the shipped surface, and an export set that drops it
# fails only here.
# Every exported target is named, not only the aggregate. The aggregate pulls
# most of them in transitively, so a module dropped from the export set would
# still resolve here and the smoke would report nothing -- which is the one
# contract this executable exists to prove.
target_link_libraries(consumer_smoke PRIVATE
    pico_bno085::core
    pico_bno085::driver
    pico_bno085::hal_core
    pico_bno085::testing
)

target_compile_features(consumer_smoke PRIVATE cxx_std_20)
