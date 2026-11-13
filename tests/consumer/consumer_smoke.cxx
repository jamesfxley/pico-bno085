// SPDX-License-Identifier: MIT
// Installed-package consumer. Every symbol must arrive through the package,
// including transitive pico-sh2, pico-logging and pico-util dependencies.
#include <cstdint>
#include <cstdio>

#include "pico_bno085/pico_bno085.hpp"
// Exercise the installed tracing header and target.
#include "pico_bno085/testing/tracing_transport.hpp"

// PICO_SH2_ACTIVE_PROFILE must come from the installed target.
// EXPECT_LARGE is the profile selected by the consumer harness.
using Profile = PICO_SH2_ACTIVE_PROFILE;
#ifdef EXPECT_LARGE
static_assert(Profile::MAX_CARGO_LEN == 1024,
              "the large profile did not reach this translation unit");
#else
static_assert(Profile::MAX_CARGO_LEN == 384,
              "the default profile did not reach this translation unit");
#endif

static_assert(pico_bno085::VERSION_MAJOR == 0 &&
              pico_bno085::VERSION_MINOR == 1);

int main() {
    std::printf("pico_bno085 %d.%d.%d\n", pico_bno085::VERSION_MAJOR,
                pico_bno085::VERSION_MINOR, pico_bno085::VERSION_PATCH);

    // Naming Bno085 checks its header and transitive pico-sh2 includes.
    using Driver = pico_bno085::Bno085;
    static_assert(sizeof(Driver) > 0);

    // Naming the header-only decorator checks its installed interface.
    using Tracer = pico_bno085::testing::TracingTransport;
    static_assert(sizeof(Tracer) > 0);

    // Exercise a transitive pico-sh2 type.
    constexpr auto SENSOR =
        static_cast<std::uint8_t>(pico_sh2::reports::SensorId::ROTATION_VECTOR);
    std::printf("rotation vector sensor id = %u\n", SENSOR);
    return 0;
}
