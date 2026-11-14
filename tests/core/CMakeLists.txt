house_cxx_add_module_test(pico_bno085 core
    SOURCES pico_bno085_test.cpp
)

# Compare header constants with CMake's project version. Keep these
# definitions private to avoid exposing build values to installed consumers.
house_cxx_version_definitions(pico_bno085_core_tests pico_bno085 PRIVATE)
