find_package(Threads REQUIRED)

house_cxx_add_module_test(pico_bno085 hal_core
    NAME pico_bno085_hal_tests
    SOURCES
        duplex_write_test.cpp
        edge_latch_test.cpp
        split_read_test.cpp
        strap_plan_test.cpp
    LIBRARIES Threads::Threads
)

# Drives the split-read compensation over arbitrary transfer streams. Off by
# default; needs a Clang toolchain with -fsanitize=fuzzer.
house_cxx_add_fuzzer(pico_bno085_split_read_fuzz
    PACKAGE pico_bno085
    SOURCE split_read_fuzz.cpp
    LIBRARIES pico_bno085::hal_core
)
