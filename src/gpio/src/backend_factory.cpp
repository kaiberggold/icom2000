#include "icom/gpio/digital_pin.hpp"
#include "icom/gpio/mock_backend.hpp"

#if defined(ICOM_WITH_LIBGPIOD)
#include "gpiod/gpiod_backend.hpp"
#endif

namespace icom::gpio {

std::unique_ptr<GpioBackend> make_default_backend() {
#if defined(ICOM_WITH_LIBGPIOD)
    return std::make_unique<GpiodBackend>();
#else
    return std::make_unique<MockBackend>();
#endif
}

} // namespace icom::gpio
