#pragma once

#include "icom/gpio/digital_pin.hpp"

namespace icom::gpio {

// In-process GPIO backend with no kernel/hardware dependency. Used for the
// host-dev CMake preset and for unit tests. `inject_edge()` lets a test (or
// a future "software loopback" demo mode) simulate a physical edge without
// real hardware.
//
// Exposed as a concrete type (not just via makeDefaultBackend()) so tests
// can reach MockInputPin::inject_edge() on the pins they requested.
class MockBackend final : public Backend {
public:
    std::unique_ptr<OutputPin> requestOutput(const PinConfig& config, Level initial) override;
    std::unique_ptr<InputPin> requestInput(const PinConfig& config, Edge edge) override;
};

// Injects a simulated edge into a pin previously obtained from MockBackend.
// No-op (and returns false) if `pin` was not produced by MockBackend.
bool injectMockEdge(InputPin& pin, Level level);

} // namespace icom::gpio
