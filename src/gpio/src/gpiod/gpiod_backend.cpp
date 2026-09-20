// Targets the libgpiod >= 2.0 C++ API (<gpiod.hpp>), as packaged for
// Raspberry Pi OS Bookworm (libgpiod-dev). Written against the documented
// v2 API surface but NOT compile-tested against real headers in this pass
// (this sandbox has no libgpiod and no arm-linux-gnueabihf toolchain) --
// treat it as a strong first draft and verify method names/signatures
// against the installed <gpiod.hpp> on first build with
// -DICOM_WITH_LIBGPIOD=ON.
#include "gpiod_backend.hpp"
#include "icom/core/logger.hpp"

#include <chrono>
#include <stdexcept>
#include <string>

#include <gpiod.hpp>

namespace icom::gpio {

namespace {

core::Logger& kLog = core::get_logger("gpio.gpiod");

std::string chip_path(const std::string& chip) {
    // Accept either a bare name ("gpiochip0") or an already-qualified path.
    if (!chip.empty() && chip.front() == '/') {
        return chip;
    }
    return "/dev/" + chip;
}

::gpiod::line::edge to_gpiod_edge(Edge edge) {
    switch (edge) {
        case Edge::Rising:  return ::gpiod::line::edge::RISING;
        case Edge::Falling: return ::gpiod::line::edge::FALLING;
        case Edge::Both:    return ::gpiod::line::edge::BOTH;
    }
    return ::gpiod::line::edge::BOTH;
}

// libstdc++/glibc on Linux back steady_clock with CLOCK_MONOTONIC, which is
// also what the kernel GPIO uAPI timestamps edge events with -- so treating
// the two as the same clock is safe in practice, if not portable in theory.
std::chrono::steady_clock::time_point to_time_point(std::uint64_t timestamp_ns) {
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(timestamp_ns));
}

class GpiodOutputPin final : public OutputPin {
public:
    GpiodOutputPin(::gpiod::line_request request, unsigned line, Level initial)
        : request_(std::move(request)), line_(line), driven_(initial) {}

    void write(Level level) override {
        request_.set_value(line_, level == Level::High ? ::gpiod::line::value::ACTIVE
                                                         : ::gpiod::line::value::INACTIVE);
        driven_ = level;
    }

    Level driven_level() const override { return driven_; }

private:
    ::gpiod::line_request request_;
    unsigned line_;
    Level driven_;
};

class GpiodInputPin final : public InputPin {
public:
    GpiodInputPin(::gpiod::line_request request, unsigned line)
        : request_(std::move(request)), line_(line) {}

    Level read() const override {
        return request_.get_value(line_) == ::gpiod::line::value::ACTIVE ? Level::High : Level::Low;
    }

    int event_fd() const override { return request_.fd(); }

    void consume_events(const EdgeCallback& callback) override {
        ::gpiod::edge_event_buffer buffer;
        request_.read_edge_events(buffer);
        for (const auto& event : buffer) {
            const Level level =
                event.type() == ::gpiod::edge_event::event_type::RISING_EDGE ? Level::High : Level::Low;
            callback(level, to_time_point(event.timestamp_ns()));
        }
    }

private:
    // mutable: gpiod::line_request::get_value() isn't const in libgpiod's
    // C++ API (confirmed against the real header -- this file wasn't
    // compile-tested when first written, see the file-level comment
    // above), even though reading a pin's value is logically const from
    // InputPin::read()'s perspective, same as any other hardware read.
    mutable ::gpiod::line_request request_;
    unsigned line_;
};

} // namespace

std::unique_ptr<OutputPin> GpiodBackend::request_output(const PinConfig& config, Level initial) {
    ::gpiod::chip chip(chip_path(config.chip));

    ::gpiod::line_settings settings;
    settings.set_direction(::gpiod::line::direction::OUTPUT)
        .set_output_value(initial == Level::High ? ::gpiod::line::value::ACTIVE
                                                   : ::gpiod::line::value::INACTIVE);

    auto request = chip.prepare_request()
                       .set_consumer(config.consumer)
                       .add_line_settings(config.line, settings)
                       .do_request();

    kLog.info("requested output " + config.chip + ":" + std::to_string(config.line) + " (" +
              config.consumer + ")");
    return std::make_unique<GpiodOutputPin>(std::move(request), config.line, initial);
}

std::unique_ptr<InputPin> GpiodBackend::request_input(const PinConfig& config, Edge edge) {
    ::gpiod::chip chip(chip_path(config.chip));

    ::gpiod::line_settings settings;
    // TODO: whether this line needs an internal pull (e.g. PULL_UP for an
    // open-drain hook-detect comparator) depends on the actual board wiring
    // -- confirm against the schematic before relying on this in hardware.
    settings.set_direction(::gpiod::line::direction::INPUT).set_edge_detection(to_gpiod_edge(edge));

    auto request = chip.prepare_request()
                       .set_consumer(config.consumer)
                       .add_line_settings(config.line, settings)
                       .do_request();

    kLog.info("requested input " + config.chip + ":" + std::to_string(config.line) + " (" +
              config.consumer + ")");
    return std::make_unique<GpiodInputPin>(std::move(request), config.line);
}

} // namespace icom::gpio
