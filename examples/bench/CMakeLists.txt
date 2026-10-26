# On-device validation harness: brings up the BNO085 and exposes the USB CDC
# line protocol that bench.py drives to run the checklist. Firmware-only
# (guarded by the parent examples/ add_subdirectory, which only runs for
# firmware builds).
#
# One image covers both buses: the harness picks one at run time with `bus`, so
# there is no build option and no second tree to keep in step.

add_executable(pico_bno085_bench main.cpp)

# Firmware-only, so pico_bno085_strict carries WERROR OFF: the SDK compiles its
# own sources into this target and cannot take it. This image's one source can.
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

# Generates pico_bno085_bench.uf2 (+ .bin/.hex/.elf.map) for flashing.
pico_add_extra_outputs(pico_bno085_bench)
