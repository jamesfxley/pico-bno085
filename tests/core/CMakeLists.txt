house_cxx_add_module_test(pico_bno085 core
    SOURCES pico_bno085_test.cpp
)

# The umbrella header's version constants are hand-maintained; feeding the
# project() version in here is what makes the test able to catch drift between
# the two rather than comparing the header against a literal copied from it.
#
# Host-side only: the constants live in the umbrella header, which belongs to
# the INTERFACE target pico_bno085::core and so has no translation unit of its
# own to static_assert from. Putting the definitions on that target instead
# would propagate build-time values into every installed consumer.
house_cxx_version_definitions(pico_bno085_core_tests pico_bno085 PRIVATE)
