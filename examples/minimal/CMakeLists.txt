# I2C example streaming rotation vectors at 50 Hz over USB CDC. Firmware-only.

add_executable(pico_bno085_minimal main.cpp)

# Apply -Werror per owned source because SDK sources share this target.
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

pico_add_extra_outputs(pico_bno085_minimal)
