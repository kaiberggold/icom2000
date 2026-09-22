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
- Target build: an ARMv6-**capable** cross toolchain (**not** a generic
  Debian/Ubuntu `gcc-arm-linux-gnueabihf` -- confirmed, not just
  suspected, to silently produce ARMv7 binaries regardless of flags; see
  [`docs/CROSS_COMPILE.md`](docs/CROSS_COMPILE.md), checked automatically
  at configure time), plus `meson` and `ninja` on the host (libgpiod
  cross-builds from source as part of the build -- no prebuilt armv6
  libgpiod exists anywhere to install instead).

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

Logs go straight to the systemd journal (`journalctl -t icom2000`, or
`journalctl ICOM_COMPONENT=gpio.mock` to filter to one component), per
component and level-filterable: `intercomd --log-level "warn,gpio.mock=debug"`
or the equivalent `ICOM_LOG` environment variable -- see
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) "Logging".

Runtime config (GPIO lines, the station name -> ALSA device mapping) comes
from `config/icom2000.conf` (`--config PATH` to point elsewhere; built-in
defaults if it's missing) -- see [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
"Configuration".

## Build for the Pi Zero

```sh
cmake --preset pi0-release
cmake --build --preset pi0-release
```

Read [`docs/CROSS_COMPILE.md`](docs/CROSS_COMPILE.md) before running the
above -- the Pi Zero 1.1 is ARMv6, and the toolchain you already have
`apt install`ed almost certainly targets ARMv7, which will not run on this
board. Configure will refuse to proceed with a toolchain that fails an
automated ARMv6 capability check rather than let you find out from a
SIGILL crash on real hardware later.

One-time target setup: install `systemd/intercomd.service`,
`systemd/alsa-restore-codec-zero.service`, `udev/99-icom2000-gpio.rules`,
`config/icom2000.conf` -> `/etc/icom2000.conf`, and `config/asound.conf`
-> `/etc/asound.conf` (see [`config/README.md`](config/README.md) for the
one piece that isn't tracked here, the alsactl state file itself). After
that, `scripts/deploy.sh user@pi-hostname` copies just the binaries over
and restarts `intercomd` for every subsequent update.

## VS Code (Windows + WSL2)

The repo ships a `.vscode/` setup (CMake Tools presets, build tasks, and
`F5` debug configs for both the host build and *live gdbserver debugging
on the Pi Zero itself*). See
[`docs/VSCODE_WSL2_SETUP.md`](docs/VSCODE_WSL2_SETUP.md) for the one-time
setup and how the remote-debug flow works.

## Layout

```
src/core/   reactor (EventLoop) + LoopThread, signal handling, logging
src/config/ File (INI-style config reader), StationRegistry
src/gpio/   OutputPin/InputPin interfaces + mock and libgpiod backends
src/hw/     Pwm (software PWM), BellController, StatusLed, Tcm1171Controller
src/audio/  Engine interface (stubbed -- see docs/ARCHITECTURE.md)
src/ipc/    Unix-socket control protocol + server
src/app/    intercomd (composition root)
src/cli/    intercomctl
tests/      host-only unit tests + the architecture-invariants guard script
docs/       architecture, cross-compile, and VS Code/WSL2 setup notes
config/     icom2000.conf, asound.conf (see config/README.md)
systemd/    intercomd.service, alsa-restore-codec-zero.service
udev/       GPIO group-permission rule
scripts/    deploy.sh, check_architecture_invariants.sh
.vscode/    CMake presets wiring, build tasks, F5 debug configs (host + remote gdbserver)
```

Full writeup: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).
