# Architecture

icom2000 is a home intercom daemon for a Raspberry Pi Zero 1.1: an old
analog telephone wired through a TCM1171-family SLIC for the handset side,
a Codec Zero HAT for two mono audio channels, and a digital-I/O-driven door
bell. No display, no window system -- everything is operated from the
shell, either interactively or from scripts/cron/other daemons.

This document is the map. Code comments explain *why* a given line does
what it does; this file explains how the pieces fit together and why they
are split the way they are.

## Status

This is a scaffold, not a working intercom. `intercomd` runs, accepts
control-socket commands, drives a real bell pin end-to-end (via the mock
GPIO backend on a dev host; via libgpiod on target), and models TCM1171
line state -- but hook detection polarity, pulse-dial decoding, ring
cadence timing, and the entire audio path are unimplemented or
placeholder. See inline `TODO`s and "Pin assignments" below before
connecting real hardware.

## Module layout

```
src/core/   EventLoop (reactor), SignalWatcher, logging -- no hardware
            or GPIO knowledge at all.
src/gpio/   OutputPin/InputPin/GpioBackend interfaces, plus two
            implementations: MockBackend (in-process, for host dev/tests)
            and GpiodBackend (libgpiod, for target hardware).
src/hw/     Domain logic built only on the gpio interfaces: BellController
            (the simple end-to-end example) and Tcm1171Controller (the
            stateful, event-driven example).
src/audio/  AudioEngine interface + a no-op NullAudioEngine. Real ALSA
            code is a later pass -- see "Audio boundary" below.
src/ipc/    The Unix-socket control protocol and its server.
src/app/    daemon_main.cpp -- the composition root. Everything above is
            constructed and wired together here; nothing else in the tree
            knows this file exists.
src/cli/    intercomctl -- a thin client over src/ipc's protocol.
tests/      Host-only unit tests (EventLoop + MockBackend).
```

Each module is its own static library target with its own `include/`
directory, so `src/hw` can depend on `icom::gpio`'s public headers without
seeing `src/gpio`'s libgpiod internals, and `tests/` can link `icom::gpio`
without linking `icom::hw` or `icom::ipc` at all. The dependency graph is
intentionally a DAG that flows one way, core -> gpio -> hw -> app, with ipc
and audio hanging off the side:

```
core <- gpio <- hw \
  ^        ^        > app -> (cli talks to app only over the socket)
  |        |        /
  +------- ipc -----
  |
  +------- audio ---
```

## Run model

`intercomd` is single-threaded and reactor-based (`icom::core::EventLoop`,
`src/core/include/icom/core/event_loop.hpp`). Every source of work is a
file descriptor:

- the TCM1171 hook-detect GPIO line's edge-event fd (from libgpiod, or the
  mock backend's eventfd),
- the control socket's listening fd, and one fd per connected client,
- timers, via `timerfd_create(2)` -- a one-shot "stop ringing after 300ms"
  is just another fd the loop polls, not a separate subsystem,
- SIGINT/SIGTERM, via `signalfd(2)` (`icom::core::SignalWatcher`) rather
  than a signal handler -- the signals are blocked process-wide with
  `sigprocmask` and collected as ordinary fd readability instead, so
  there's no async-signal-safety tightrope to walk and no risk of a signal
  landing mid-mutation of shared state.

All of it is multiplexed with `poll(2)`. Why poll() and not epoll() on a
system that has both: the fd count here is a handful (one GPIO chip, one
listening socket, a few client connections, a couple of timers) --
epoll's O(1) add/remove bookkeeping buys nothing at that scale, and
poll()'s flat vector-of-pollfd is simpler to unit-test (see
`tests/gpio_and_event_loop_tests.cpp`) and to reason about when a callback
mutates the fd set mid-dispatch. `EventLoop::run()` snapshots the pollfd
vector before each `poll()` call and copies each callback out of the map
before invoking it, specifically so a callback is free to add/remove any
fd -- including its own -- without corrupting the dispatch loop that's
currently calling it. (An earlier version of this code took a reference
into the callback map instead of a copy; a one-shot timer removing itself
from inside its own callback then use-after-freed its own captured `this`.
If you're ever tempted to "optimize away" that copy, don't.)

This single-thread-plus-reactor model is a deliberate fit for a Pi Zero:
one ARM1176JZF-S core at ~1GHz has nothing to gain from a thread pool for
this workload, and a single thread means no locking anywhere in
`src/core`, `src/gpio`, `src/hw`, or `src/ipc` -- the only place this
project should ever need a mutex is inside a future real `AudioEngine`,
which explicitly does *not* share the reactor thread (see below).

### OS interaction, concretely

- **Startup**: `daemon_main.cpp` is the composition root -- it constructs
  the GPIO backend, hardware controllers, audio engine, and control server,
  in dependency order, and owns them all on the stack for the process
  lifetime.
- **Shutdown**: SIGTERM (systemd's default stop signal) or SIGINT (Ctrl-C
  during development) reach `SignalWatcher`, which calls `EventLoop::stop()`
  from the reactor thread. `run()` returns, `main()` falls through, engine
  and controller destructors release GPIO lines and close the socket.
  There is currently no persisted state to flush, so this is a clean exit
  by construction rather than something that needs explicit draining.
- **Service supervision**: intended to run under systemd
  (`systemd/intercomd.service`) as an unprivileged user in the `gpio` and
  `audio` groups (permissions come from `udev/99-icom2000-gpio.rules` and
  the system's default audio group ACLs, not from running as root).
  `Restart=on-failure` covers crashes; nothing here yet talks to
  `sd_notify()` (no `Type=notify`, no watchdog ping) -- worth adding once
  there's a real failure mode to detect and recover from.
- **Logging**: `icom::core::log_*` writes leveled lines to stderr, which
  journald captures automatically under systemd; no explicit journald
  integration needed for that alone.

## GPIO abstraction

`icom::gpio::OutputPin` / `InputPin` / `GpioBackend`
(`src/gpio/include/icom/gpio/digital_pin.hpp`) are the only thing
`src/hw` and `src/app` are allowed to depend on for GPIO access -- neither
includes `<gpiod.hpp>` or knows libgpiod exists. Two backends implement
that interface:

- **MockBackend** (`src/gpio/src/mock`): in-process, backed by an
  `eventfd` per input pin so it plugs into the same `EventLoop` an fd from
  a real chip would. `inject_mock_edge()` lets tests (and a future
  software-loopback demo mode) simulate a physical edge. This is what the
  `host-dev` CMake preset builds, and what `tests/` link against -- no
  GPIO chip, no root, no target hardware needed to develop the control
  logic.
- **GpiodBackend** (`src/gpio/src/gpiod`): the real thing, against
  libgpiod's v2 C++ API. Only compiled when `-DICOM_WITH_LIBGPIOD=ON`
  (the `pi0-release` preset does this). **Not compile-tested in this
  pass** -- there is no libgpiod and no armv6 toolchain in the environment
  this scaffold was built in. Treat it as a strong first draft; the first
  `cmake --build --preset pi0-release` on real target headers is expected
  to need small signature fixes.

Which backend `make_default_backend()` returns is a compile-time choice
(`ICOM_WITH_LIBGPIOD`), not a runtime one -- there is no reason a Pi
binary should carry mock code or a dev-host binary should require
libgpiod headers to exist.

## Hardware layer

- **`BellController`** (`src/hw`) is the simple, fully-implemented example
  the "one working digital pin" requirement asked for: one `OutputPin`,
  `ring()`/`silence()`/`ring_for(duration, loop)`. Read this one first.
- **`Tcm1171Controller`** is the event-driven example: it owns two output
  pins (ring-mode enable, line polarity) and one input pin (hook detect),
  registers the input's edge fd with the `EventLoop` in its constructor,
  and exposes `LineState` (`OnHook`/`Ringing`/`OffHook`/`Fault`) plus
  `start_ringing()`/`stop_ringing()`.

### Pin assignments (placeholder)

**Everything below is illustrative, not verified against a schematic.**
`src/app/daemon_main.cpp` hardcodes `gpiochip0` lines 17 (bell), 27
(TCM1171 ring-mode), 22 (TCM1171 polarity), and 23 (hook detect) purely so
the daemon has *something* to construct and run end-to-end. Before wiring
real hardware:

1. Confirm the TCM1171's actual digital control pins on your board (RM,
   FR) and which GPIOs they land on.
2. Confirm how hook state reaches a GPIO at all. The TCM1171 itself
   exposes loop current as an *analog* signal (IL); on-hook/off-hook
   detection normally needs an external comparator or optocoupler between
   that pin and whatever GPIO `Tcm1171Controller::Pins::hook_detect`
   ends up wired to. `on_hook_edge()`'s polarity (`High` == off-hook) is a
   placeholder guess, not a measured fact.
3. Update the `k*PinLine` constants in `daemon_main.cpp` (or, better,
   move them into a small config file once there's more than one
   plausible board revision -- not worth the abstraction yet at n=1).

## Audio boundary

Out of scope for this pass by design. `icom::audio::AudioEngine`
(`src/audio/include/icom/audio/audio_engine.hpp`) defines the seam a real
implementation plugs into; `NullAudioEngine` satisfies it today so the
daemon builds, runs, and reports `audio=down`... `audio=up`-but-silent
honestly via `intercomctl status` without every other component needing
to special-case "audio doesn't exist yet".

When it's implemented, it should **not** join the reactor thread. Two mono
ALSA duplex streams against the Codec Zero (handset audio, and
door/ambient) need real-time-ish scheduling that a single-threaded
`poll()` loop shouldn't be put at risk of jittering (a slow control-socket
client blocking the loop for a few ms is a UX blemish; blocking the audio
callback for a few ms is an audible glitch). The expected shape: its own
thread(s), talking to the reactor thread via a lock-free queue or a
handful of atomics, not shared mutable state.

## Control protocol / IPC

`icom::ipc::ControlServer` (`src/ipc`) listens on a Unix domain socket
(`SOCK_STREAM`) and speaks a flat, line-based text protocol
(`src/ipc/include/icom/ipc/protocol.hpp`):

```
request:   "<COMMAND> [ARG ...]\n"
response:  "OK [message]\n"   or   "ERR <message>\n"
```

Commands are registered by name (`register_command()`) rather than
switched on inside `ControlServer` -- `daemon_main.cpp` is the only place
that knows `BELL`, `LINE`, `STATUS`, and `PING` exist. Adding a command
later means adding a `register_command()` call at the wiring site, not
touching `src/ipc` at all. Command names are matched case-insensitively;
so are the keyword arguments the currently-registered handlers use (`ON`/
`OFF`/`RING`, `STATUS`) -- each handler is responsible for its own
argument casing, `ControlServer` only normalizes the command name.

`intercomctl` (`src/cli/ctl_main.cpp`) is a deliberately dumb client: it
joins argv, writes one line, reads one line, and turns `OK`/`ERR` into an
exit code. All the logic lives in the daemon, on purpose, so the protocol
stays usable from `socat -` or `nc -U` too, not just this one binary.

Because it's a plain Unix socket, permissions are filesystem permissions:
`ControlServer::start()` chmods the socket 0660, and
`systemd/intercomd.service` puts it in a 0770 `RuntimeDirectory` -- put an
operator in the `icom2000` group to let them run `intercomctl` without
being root.

## Testing

`tests/` builds only under the `host-dev` preset
(`ICOM_BUILD_TESTS=OFF` on `pi0-release` -- there is no reason to spend a
Pi Zero's single core building tests it can't meaningfully run any
differently than a dev host can). It intentionally does not pull in a
third-party framework (Catch2/doctest/GTest) via `FetchContent`, so
`ctest --preset host-dev` works with zero network access; `tests/check.hpp`
is a ~20-line `CHECK()` macro. Swapping in a real framework later is a
one-file change once the suite outgrows that.

What's covered: `MockBackend` output/input behavior, and that an injected
mock edge reaches an `EventLoop` callback end-to-end -- i.e. the same path
`Tcm1171Controller` depends on in production. What's not: `GpiodBackend`
(no libgpiod in this environment) and anything in `src/hw`/`src/ipc`/
`src/app` (straightforward to add following the same pattern; left out of
this pass to keep it to "one representative example per layer" per the
scaffold's brief).

## What's next

Roughly in the order it'd need doing to become a real intercom:

1. Confirm TCM1171 pin wiring against the actual board (see "Pin
   assignments"); fix polarity/line numbers.
2. Pulse-dial decoding off the same hook-detect edges
   `Tcm1171Controller` already timestamps.
3. A real `AudioEngine` against the Codec Zero (ALSA duplex, its own
   thread(s), a ring/tone generator for the TCM1171's ring cadence).
4. Persist/report richer line state over the control protocol (e.g. call
   duration, last-ring time) once there's a client that wants it.
5. `sd_notify()`/watchdog integration once there's a concrete failure mode
   worth detecting.
