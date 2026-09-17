# icom2000

A home intercom daemon for a Raspberry Pi Zero 1.1: an old analog
telephone wired through a TCM1171 SLIC, a Codec Zero HAT for audio, and a
GPIO-driven door bell. Command-line only -- no GUI on the Pi -- controlled
via a small Unix-socket protocol and a companion CLI (`intercomctl`).

**Status: architecture scaffold, not a working intercom yet.** See
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for what's real, what's
placeholder, and why the code is shaped the way it is.

## Requirements

- CMake >= 3.25, Ninja
- A C++20 compiler (GCC 12+ on target -- Raspberry Pi OS Bookworm; any
  reasonably recent GCC/Clang on your dev host)
- Host dev/test build: nothing else -- GPIO is mocked in-process.
- Target build: libgpiod >= 2.0 and an ARMv6 cross toolchain (**not** a
  generic Debian/Ubuntu armhf one -- see
  [`docs/CROSS_COMPILE.md`](docs/CROSS_COMPILE.md), it matters).

## Build & test (dev host, mock GPIO)

```sh
cmake --preset host-dev
cmake --build --preset host-dev
ctest --preset host-dev --output-on-failure
```

Run it locally:

```sh
./build/host-dev/src/app/intercomd --socket /tmp/icom2000.sock &
./build/host-dev/src/cli/intercomctl -s /tmp/icom2000.sock status
./build/host-dev/src/cli/intercomctl -s /tmp/icom2000.sock bell ring 300
```

## Build for the Pi Zero

```sh
cmake --preset pi0-release -DICOM_PI_SYSROOT=/path/to/pi-sysroot
cmake --build --preset pi0-release
```

Read [`docs/CROSS_COMPILE.md`](docs/CROSS_COMPILE.md) before running the
above -- the Pi Zero 1.1 is ARMv6, and the toolchain you already have
`apt install`ed almost certainly targets ARMv7, which will not run on this
board.

Then `scripts/deploy.sh user@pi-hostname` to copy the binaries over and
restart the service (`systemd/intercomd.service`,
`udev/99-icom2000-gpio.rules`).

## Layout

```
src/core/   reactor (EventLoop), signal handling, logging
src/gpio/   OutputPin/InputPin interfaces + mock and libgpiod backends
src/hw/     BellController, Tcm1171Controller
src/audio/  AudioEngine interface (stubbed -- see docs/ARCHITECTURE.md)
src/ipc/    Unix-socket control protocol + server
src/app/    intercomd (composition root)
src/cli/    intercomctl
tests/      host-only unit tests
docs/       architecture + cross-compile notes
systemd/    intercomd.service
udev/       GPIO group-permission rule
scripts/    deploy.sh
```

Full writeup: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).
