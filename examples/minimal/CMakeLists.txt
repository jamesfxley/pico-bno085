# The smallest thing that streams: reset the BNO085 over I2C, enable the
# rotation vector at 50 Hz and print the quaternion over USB CDC. Firmware-only
# (guarded by the parent examples/ add_subdirectory, which only runs for
# firmware builds).

add_executable(pico_bno085_minimal main.cpp)

# Firmware-only, so pico_bno085_strict carries WERROR OFF: the SDK compiles its
# own sources into this target and cannot take it. This image's one source can.
house_cxx_werror_sources(main.cpp)

pico_set_program_name(pico_bno085_minimal "pico_bno085_minimal")
pico_set_program_version(pico_bno085_minimal "${PROJECT_VERSION}")

pico_enable_stdio_usb(pico_bno085_minimal 1)
pico_enable_stdio_uart(pico_bno085_minimal 0)

target_link_libraries(pico_bno085_minimal PRIVATE
    pico_bno085_strict
    pico_bno085::core
    pico_logging::core
    pico_stdlib
)

# Generates pico_bno085_minimal.uf2 (+ .bin/.hex/.elf.map) for flashing.
pico_add_extra_outputs(pico_bno085_minimal)
