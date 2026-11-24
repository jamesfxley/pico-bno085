# Firmware benchmark with runtime bus selection. The line protocol is specified
# in docs/technical-specification.md (LP).

add_executable(pico_bno085_bench main.cpp)

# Apply -Werror per owned source because SDK sources share this target.
house_cxx_werror_sources(main.cpp)

pico_set_program_name(pico_bno085_bench "pico_bno085_bench")
pico_set_program_version(pico_bno085_bench "${PROJECT_VERSION}")

pico_enable_stdio_usb(pico_bno085_bench 1)
pico_enable_stdio_uart(pico_bno085_bench 0)

target_link_libraries(pico_bno085_bench PRIVATE
    pico_bno085_strict
    pico_bno085::core
    pico_logging::core
    pico_util::core
    pico_bno085::testing
    pico_stdlib
)

pico_add_extra_outputs(pico_bno085_bench)
