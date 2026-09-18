#pragma once

#include "icom/gpio/digital_pin.hpp"

namespace icom::gpio {

// Real GPIO backend for the target: talks to the kernel's GPIO character
// device (/dev/gpiochipN) via libgpiod's v2 C++ bindings. Only compiled when
// ICOM_WITH_LIBGPIOD is ON (see cmake/toolchain-arm-linux-gnueabihf.cmake
// and the pi0-release CMake preset) -- host dev builds use MockBackend
// instead so nobody needs a GPIO chip to build or run the tests.
class GpiodBackend final : public GpioBackend {
public:
    std::unique_ptr<OutputPin> request_output(const PinConfig& config, Level initial) override;
    std::unique_ptr<InputPin> request_input(const PinConfig& config, Edge edge) override;
};

} // namespace icom::gpio
