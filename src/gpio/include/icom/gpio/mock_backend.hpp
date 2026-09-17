#pragma once

#include "icom/gpio/digital_pin.hpp"

namespace icom::gpio {

// In-process GPIO backend with no kernel/hardware dependency. Used for the
// host-dev CMake preset and for unit tests. `inject_edge()` lets a test (or
// a future "software loopback" demo mode) simulate a physical edge without
// real hardware.
//
// Exposed as a concrete type (not just via make_default_backend()) so tests
// can reach MockInputPin::inject_edge() on the pins they requested.
class MockBackend final : public GpioBackend {
public:
    std::unique_ptr<OutputPin> request_output(const PinConfig& config, Level initial) override;
    std::unique_ptr<InputPin> request_input(const PinConfig& config, Edge edge) override;
};

// Injects a simulated edge into a pin previously obtained from MockBackend.
// No-op (and returns false) if `pin` was not produced by MockBackend.
bool inject_mock_edge(InputPin& pin, Level level);

} // namespace icom::gpio
