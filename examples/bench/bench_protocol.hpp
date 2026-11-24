// SPDX-License-Identifier: MIT
#pragma once

/// \file bench_protocol.hpp
/// Line formatting and argument parsing for the bench firmware. The firmware
/// and the host test (tests/bench) share these functions, so what the test
/// checks is what the firmware prints (LP-1).

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <variant>

#include "pico_bno085/driver/bno085.hpp"
#include "pico_sh2/reports/sensor_event.hpp"
#include "pico_sh2/sh2/command.hpp"

namespace pico_bno085::bench {

/// Digits after the decimal point in a payload number (LP-6).
inline constexpr int FIXED_DIGITS = 5;
/// Ten to the power FIXED_DIGITS.
inline constexpr std::int64_t FIXED_SCALE = 100'000;
/// Magnitudes at or above this print as `nan`; no decoded field reaches it
/// (LP-6).
inline constexpr double FIXED_LIMIT = 1.0e9;

/// Longest line the formatters produce, including the terminating NUL (LP-1).
inline constexpr std::size_t LINE_BUFFER_SIZE = 128;

inline constexpr int BASE_DEC = 10;
inline constexpr int BASE_HEX = 16;

/// Payload lines for the kinds of sample that have one (LP-5).
enum class LineTag : char {
    EVENT = 'E',
    ROTATION = 'Q',
    GAME_ROTATION = 'R',
    VECTOR = 'V',
    UNCALIBRATED = 'U',
    GYRO_INTEGRATED = 'G',
};

/// Appends to a caller's buffer and records overflow instead of truncating
/// silently.
class LineWriter {
   public:
    explicit LineWriter(std::span<char> out) : out_(out) {}

    void put(char ch) { put(std::string_view(&ch, 1)); }

    void put(std::string_view text) {
        // One byte stays free for the terminating NUL.
        const std::size_t room = out_.empty() ? 0 : out_.size() - 1 - size_;
        const std::size_t count = text.size() < room ? text.size() : room;
        std::copy_n(text.begin(), count,
                    out_.begin() + static_cast<std::ptrdiff_t>(size_));
        size_ += count;
        overflow_ = overflow_ || count < text.size();
    }

    void put_uint(std::uint64_t value) {
        std::array<char, 20> digits{};
        const auto result =
            std::to_chars(digits.data(), digits.data() + digits.size(), value);
        put(std::string_view(digits.data(), static_cast<std::size_t>(
                                                result.ptr - digits.data())));
    }

    /// Two lowercase hexadecimal digits.
    void put_hex2(std::uint8_t value) {
        constexpr std::string_view HEX_DIGITS = "0123456789abcdef";
        constexpr unsigned NIBBLE_BITS = 4;
        constexpr unsigned NIBBLE_MASK = 0x0FU;
        const unsigned wide = value;
        put(HEX_DIGITS[(wide >> NIBBLE_BITS) & NIBBLE_MASK]);
        put(HEX_DIGITS[wide & NIBBLE_MASK]);
    }

    /// A decimal with FIXED_DIGITS places, rounded to nearest with ties away
    /// from zero. A value that rounds to zero carries no sign (LP-6).
    void put_fixed(float value) {
        const auto wide = static_cast<double>(value);
        if (!std::isfinite(wide) || std::fabs(wide) >= FIXED_LIMIT) {
            put("nan");
            return;
        }
        const std::int64_t scaled =
            std::llround(wide * static_cast<double>(FIXED_SCALE));
        if (scaled < 0) {
            put('-');
        }
        const auto magnitude =
            static_cast<std::uint64_t>(scaled < 0 ? -scaled : scaled);
        constexpr auto SCALE = static_cast<std::uint64_t>(FIXED_SCALE);
        put_uint(magnitude / SCALE);
        put('.');
        const std::uint64_t fraction = magnitude % SCALE;
        constexpr std::uint64_t TEN = 10;
        std::uint64_t place = SCALE / TEN;
        for (int digit = 0; digit < FIXED_DIGITS; ++digit) {
            put(static_cast<char>('0' + ((fraction / place) % TEN)));
            place /= TEN;
        }
    }

    /// Append a name and a decimal value, such as ` seq=` and `12`.
    void put_field(std::string_view name, std::uint64_t value) {
        put(name);
        put_uint(value);
    }

    /// Terminate the text. Return its length, or zero if it did not fit.
    [[nodiscard]] std::size_t finish() {
        if (overflow_ || out_.empty()) {
            return 0;
        }
        out_[size_] = '\0';
        return size_;
    }

   private:
    std::span<char> out_;
    std::size_t size_ = 0;
    bool overflow_ = false;
};

/// The tag of the line that carries a sample of this kind, or EVENT for a kind
/// with no payload line (LP-5).
constexpr LineTag payload_tag(pico_sh2::reports::EventKind kind) {
    using pico_sh2::reports::EventKind;
    switch (kind) {
        case EventKind::ROTATION:
            return LineTag::ROTATION;
        case EventKind::GAME_ROTATION:
            return LineTag::GAME_ROTATION;
        case EventKind::VECTOR:
            return LineTag::VECTOR;
        case EventKind::UNCALIBRATED:
            return LineTag::UNCALIBRATED;
        case EventKind::GYRO_INTEGRATED:
            return LineTag::GYRO_INTEGRATED;
        case EventKind::NONE:
        case EventKind::TAP:
        case EventKind::STEP_COUNT:
        case EventKind::STEP_DETECT:
        case EventKind::STABILITY:
        case EventKind::RAW:
            return LineTag::EVENT;
    }
    return LineTag::EVENT;
}

namespace detail {

/// The numbers after the common fields of a payload line.
class FieldList {
   public:
    static constexpr std::size_t CAPACITY = 7;

    void add(float value) {
        if (count_ < values_.size()) {
            values_[count_] = value;
            ++count_;
        }
    }

    // Quaternion in the order w, i, j, k (LP-6).
    void add(const pico_sh2::reports::Quaternion& quat) {
        add(quat.real);
        add(quat.i);
        add(quat.j);
        add(quat.k);
    }

    void add(const pico_sh2::reports::Vec3& vec) {
        add(vec.x);
        add(vec.y);
        add(vec.z);
    }

    [[nodiscard]] std::span<const float> values() const {
        return std::span<const float>(values_).first(count_);
    }

   private:
    std::array<float, CAPACITY> values_{};
    std::size_t count_ = 0;
};

// The numbers a sample contributes to its payload line, empty for a kind with
// none.
inline FieldList sample_fields(const pico_sh2::reports::SensorEvent& event) {
    using pico_sh2::reports::GyroIntegratedSample;
    using pico_sh2::reports::Quaternion;
    using pico_sh2::reports::RotationSample;
    using pico_sh2::reports::UncalibratedSample;
    using pico_sh2::reports::Vec3;
    FieldList fields;
    if (const auto* rotation = std::get_if<RotationSample>(&event.payload)) {
        fields.add(rotation->unit);
        fields.add(rotation->accuracy_rad);
    } else if (const auto* game = std::get_if<Quaternion>(&event.payload)) {
        fields.add(*game);
    } else if (const auto* vector = std::get_if<Vec3>(&event.payload)) {
        fields.add(*vector);
    } else if (const auto* raw =
                   std::get_if<UncalibratedSample>(&event.payload)) {
        fields.add(raw->value);
        fields.add(raw->bias);
    } else if (const auto* gyro =
                   std::get_if<GyroIntegratedSample>(&event.payload)) {
        fields.add(gyro->unit);
        fields.add(gyro->angular_velocity);
    }
    return fields;
}

}  // namespace detail

/// Format one event as a line ending in a newline and a NUL. With payload off
/// it is the E line alone (LP-4); with payload on, a kind that has a payload
/// line prints it instead of the E line (LP-5, LP-6).
/// \return the length excluding the NUL, or zero if `out` is too small.
inline std::size_t format_event(std::span<char> out,
                                const pico_sh2::reports::SensorEvent& event,
                                bool payload) {
    const LineTag tag = payload ? payload_tag(event.kind()) : LineTag::EVENT;
    LineWriter line(out);
    line.put(static_cast<char>(tag));
    line.put_field(",", static_cast<std::uint8_t>(event.sensor));
    line.put_field(",", event.sequence);
    line.put_field(",", event.event_us);
    line.put_field(",", static_cast<std::uint8_t>(event.accuracy));
    if (tag != LineTag::EVENT) {
        const detail::FieldList fields = detail::sample_fields(event);
        for (const float value : fields.values()) {
            line.put(',');
            line.put_fixed(value);
        }
    }
    line.put('\n');
    return line.finish();
}

/// Format a command response (LP-7). Save-DCD and ME-calibration responses are
/// decoded; every other response is printed as its raw result bytes.
/// \return the length excluding the NUL, or zero if `out` is too small.
inline std::size_t format_command_response(
    std::span<char> out, const pico_sh2::CommandResponse& response) {
    LineWriter line(out);
    const auto& results = response.results;
    if (response.command == pico_sh2::command::SAVE_DCD) {
        line.put_field("# dcd status=", results[0]);
    } else if (response.command == pico_sh2::command::ME_CALIBRATION) {
        line.put_field("# cal status=", results[0]);
        line.put_field(" accel=", results[1]);
        line.put_field(" gyro=", results[2]);
        line.put_field(" mag=", results[3]);
        line.put_field(" planar=", results[4]);
        line.put_field(" table=", results[5]);
    } else {
        line.put("# resp cmd=");
        line.put_hex2(response.command);
        line.put_field(" seq=", response.command_sequence);
        line.put_field(" rseq=", response.response_sequence);
        line.put(" r=");
        for (const std::uint8_t byte : results) {
            line.put_hex2(byte);
        }
    }
    line.put('\n');
    return line.finish();
}

/// The lower-case name an error takes in an `# err` reply (LP-3).
constexpr std::string_view error_name(Error error) {
    switch (error) {
        case Error::NONE:
            return "none";
        case Error::BUS_ERROR:
            return "bus_error";
        case Error::TIMEOUT:
            return "timeout";
        case Error::NOT_READY:
            return "not_ready";
        case Error::PROTOCOL:
            return "protocol";
        case Error::BUSY:
            return "busy";
        case Error::NO_ROOM:
            return "no_room";
        case Error::UNSUPPORTED:
            return "unsupported";
        case Error::INVALID_ARGUMENT:
            return "invalid_argument";
    }
    return "unknown";
}

/// Parse and advance past an unsigned field. Return false if no digits parse.
inline bool take_uint(std::string_view& text, unsigned& out, int base) {
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), out, base);
    if (ec != std::errc()) {
        return false;
    }
    text.remove_prefix(static_cast<std::size_t>(ptr - text.data()));
    return true;
}

/// Parse one field that must be 0 or 1.
inline bool take_flag(std::string_view& text, bool& out) {
    unsigned value = 0;
    if (!take_uint(text, value, BASE_DEC) || value > 1) {
        return false;
    }
    out = value == 1;
    return true;
}

/// True when only spaces remain.
inline bool only_spaces(std::string_view text) {
    return text.find_first_not_of(' ') == std::string_view::npos;
}

/// Parse `0|1` (LP-2).
inline std::optional<bool> parse_flag(std::string_view args) {
    bool value = false;
    if (!take_flag(args, value) || !only_spaces(args)) {
        return std::nullopt;
    }
    return value;
}

/// Parse `<axes_hex> <basis>` for tare: axes 7 or 4 and a basis from 0 to 5,
/// the values the driver supports (LP-2).
inline std::optional<Bno085::TareOptions> parse_tare(std::string_view args) {
    constexpr unsigned MAX_BASIS = 5;
    unsigned axes = 0;
    unsigned basis = 0;
    if (!take_uint(args, axes, BASE_HEX) || !take_uint(args, basis, BASE_DEC) ||
        !only_spaces(args) || basis > MAX_BASIS) {
        return std::nullopt;
    }
    if (axes != static_cast<unsigned>(Bno085::TareAxes::ALL) &&
        axes != static_cast<unsigned>(Bno085::TareAxes::Z_ONLY)) {
        return std::nullopt;
    }
    return Bno085::TareOptions{.axes = static_cast<Bno085::TareAxes>(axes),
                               .basis = static_cast<Bno085::TareBasis>(basis)};
}

/// Parse `<accel> <gyro> <mag>`, each 0 or 1 (LP-2).
inline std::optional<Bno085::CalibrationOptions> parse_calibration(
    std::string_view args) {
    Bno085::CalibrationOptions options;
    if (!take_flag(args, options.accel) || !take_flag(args, options.gyro) ||
        !take_flag(args, options.mag) || !only_spaces(args)) {
        return std::nullopt;
    }
    return options;
}

}  // namespace pico_bno085::bench
