# The bench firmware's formatters and parsers, built for the host. The header
# is the one examples/bench/main.cpp includes.
house_cxx_add_module_test(pico_bno085 driver
    NAME pico_bno085_bench_tests
    SOURCES bench_protocol_test.cpp
)
target_include_directories(pico_bno085_bench_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/examples/bench")
