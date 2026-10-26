// SPDX-License-Identifier: MIT
// Consumer-side view of the installed package. Nothing here reaches into the
// source tree: every symbol below must arrive through the installed
// pico_bno085::core target, resolved by find_package, so a header the package
// fails to ship is a compile error in this test rather than a discovery made by
// the first downstream project.
//
// This also exercises the transitive half. pico_bno085Config.cmake calls
// find_dependency(pico_sh2) and find_dependency(pico_logging) and
// find_dependency(pico_util), and the driver façade's own headers include
// pico_sh2's. A dependency that fails to install alongside, or that is missing
// from the DEPENDENCIES list, fails here and nowhere in-tree.
#include <cstdint>
#include <cstdio>

#include "pico_bno085/pico_bno085.hpp"
// The tracing decorator the package ships for downstream tests. Included so a
// pico_bno085::testing that left the export set, or whose headers did not
// install, is a compile error here rather than a downstream discovery.
#include "pico_bno085/testing/tracing_transport.hpp"

// Consumer-side view of the sizing profile. Nothing here defines a profile
// macro; PICO_SH2_ACTIVE_PROFILE must arrive through the installed target's
// compile definitions, since PICO_BNO085_PROFILE forwards to pico-sh2.
// EXPECT_LARGE is set by the harness to the profile it installed, so a profile
// lost at the install boundary is a compile error here rather than a silently
// default-sized consumer.
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

    // The driver façade is platform-independent: a host consumer supplies its
    // own transport. Naming the type is enough to prove the header arrived and
    // that pico_sh2's headers arrived behind it.
    using Driver = pico_bno085::Bno085;
    static_assert(sizeof(Driver) > 0);

    // The shipped decorator. It is header-only, so naming the type is the whole
    // proof: the header arrived and compiles against the pico_sh2 seam it
    // decorates, which had to install alongside.
    using Tracer = pico_bno085::testing::TracingTransport;
    static_assert(sizeof(Tracer) > 0);

    // A pico_sh2 type reached through the façade's own includes, so this fails
    // if find_dependency(pico_sh2) did not resolve.
    constexpr auto SENSOR =
        static_cast<std::uint8_t>(pico_sh2::reports::SensorId::ROTATION_VECTOR);
    std::printf("rotation vector sensor id = %u\n", SENSOR);
    return 0;
}
