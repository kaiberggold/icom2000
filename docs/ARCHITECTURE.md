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
src/config/ ConfigFile (INI-style reader) + StationRegistry -- see
            "Configuration" below. No hardware knowledge either.
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
tests/      Host-only unit tests (EventLoop + MockBackend + ConfigFile +
            the architecture-invariants guard script, see "Testing").
config/     Deployment configuration -- see "Configuration" below.
```

Each module is its own static library target with its own `include/`
directory, so `src/hw` can depend on `icom::gpio`'s public headers without
seeing `src/gpio`'s libgpiod internals, and `tests/` can link `icom::gpio`
without linking `icom::hw` or `icom::ipc` at all. The dependency graph is
intentionally a DAG that flows one way, core -> gpio -> hw -> app, with ipc,
config, and audio hanging off the side:

```
core <- gpio <- hw \
  ^        ^        > app -> (cli talks to app only over the socket)
  |        |        /
  +------- ipc -----
  |
  +------- config <- audio
```

`src/hw` and `src/gpio` never depend on `config` or `audio` -- a physical
GPIO trigger (a hook-state change, a future button press) is decoupled from
whatever eventually reacts to it (today: nothing; later: possibly audio),
by construction, not by convention. See "Architecture invariants" below for
how that's actually enforced.

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
- **Logging**: see "Logging" below.

## Logging

`icom::core::Logger` (`src/core/include/icom/core/logger.hpp`) is a named,
independently-leveled logger -- roughly one per module -- writing through
`syslog(3)`. Under systemd that lands in the journal exactly like any other
well-behaved daemon's log output (`journalctl -u intercomd`, or filter by
identifier with `journalctl -t icom2000`); on a plain Raspberry Pi OS
install without a separate rsyslog it's journald providing `/dev/log`
either way, so there's nothing extra to set up.

(This module started from a request to log "to the kernel log" --
`/dev/kmsg`/`dmesg`. That's a real, different thing from syslog: writing
`/dev/kmsg` requires `CAP_SYSLOG` or root and shows up in `dmesg` whether
or not a syslog daemon is even running, whereas `syslog(3)` needs no
special privilege but requires something listening on `/dev/log`. This
project uses `syslog(3)` -- it's the conventional destination for a
userspace daemon's own logs, works with the unprivileged systemd unit this
project already ships, and every message still ends up in the journal.)

### Per-component levels

Each `.cpp` file that logs declares its own logger once, at file scope:

```cpp
namespace {
icom::core::Logger& kLog = icom::core::get_logger("hw.bell");
} // namespace
```

and then just calls `kLog.debug(...)`/`.info(...)`/`.warn(...)`/`.error(...)`
wherever it wants -- that's the whole mechanism for "insert log output
where I want": add a `kLog` line if the file doesn't have one yet
(matching the dotted `module.submodule` naming already in use -- see any
existing `.cpp` under `src/` for the pattern), then log. Current
components: `core.event_loop`, `gpio.mock`, `gpio.gpiod`, `hw.bell`,
`hw.tcm1171`, `audio`, `ipc.control_server`, `app`.

Levels are set at startup, per component, via the `ICOM_LOG` environment
variable or `intercomd --log-level`, both parsed by the same
`configure_levels()`:

```
ICOM_LOG="warn,gpio.mock=debug,ipc.control_server=debug" intercomd
intercomd --log-level "warn,gpio.mock=debug,ipc.control_server=debug"
```

The bare `warn` sets the default for every component not otherwise named;
`gpio.mock=debug` and `ipc.control_server=debug` override just those two.
`--log-level` takes precedence over `ICOM_LOG` when both are given (so you
can bump one component for a single run without editing the systemd unit's
`Environment=`). Either input is rejected as a whole -- `intercomd` prints
an error and exits nonzero -- if it contains an unrecognized level name or
malformed component/level pair, rather than silently keeping whatever
levels components happened to default to.

### Seeing log output while debugging

`--log-console` (`icom::core::set_console_output(true)`) mirrors every
logged message to stderr, in addition to syslog, at whatever level(s)
`--log-level`/`ICOM_LOG` already set -- off by default, since a
systemd-managed run has nothing to gain from it (stderr just lands in the
journal a second time). It exists for interactive/debugger use, where
waiting on a second `journalctl -f`/fake-`/dev/log` window is friction a
plain `std::cerr` line doesn't have: the "Debug intercomd" and "Debug
intercomd on Pi Zero" `.vscode/launch.json` configs both pass it, and with
`"externalConsole": false` (already set), VS Code's cppdbg captures that
stderr straight into the Debug Console. The remote-gdbserver config is the
one exception worth knowing: gdbserver doesn't pipe the debuggee's stdio
back over the wire, so that flag (baked into `pi-start-gdbserver`'s ssh
command in `.vscode/tasks.json`) shows up in the **task's own terminal
tab** ("pi-start-gdbserver"), not the Debug Console -- still one click away
in VS Code, just a different panel.

### Why the registry is a function-local static

`get_logger()`'s registry is a Meyer's singleton (a `static Registry` local
to a function), not a plain namespace-scope global. Several `.cpp` files
declare their `Logger& kLog` as a namespace-scope variable, which runs
during that translation unit's *dynamic initialization* -- and the C++
standard leaves the relative order of dynamic initialization across
different translation units unspecified. A plain global registry could
easily end up read by one TU's `kLog` initializer before another TU's
initializer had constructed it. A function-local static sidesteps the
question entirely: it's guaranteed to be constructed on its first use, no
matter which TU that first use comes from.

### Logging tests

`tests/logger_tests.cpp` covers the registry (identity, per-component
levels) and `configure_levels()`'s parsing, including that a rejected spec
changes nothing. It does not check that a message actually reaches
`syslog` -- that was instead verified manually against this exact build,
by standing up a throwaway `AF_UNIX SOCK_DGRAM` listener at `/dev/log` and
running `intercomd` against it (there's no standing fake-syslog fixture in
the repo; it isn't worth automating a Unix-socket receiver for one
integration check when the actual `syslog(3)` call is standard,
decades-stable POSIX API).

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
  (the `pi0-release`/`pi0-debug` presets do this). Compile- and
  link-tested end to end -- cross-built against a real libgpiod v2.3.1
  (see "Building libgpiod from source" in docs/CROSS_COMPILE.md) with an
  `arm-linux-gnueabihf` compiler, producing a real `intercomd` ELF binary.
  One real bug turned up this way and was fixed: `GpiodInputPin::read()`
  called `gpiod::line_request::get_value()`, which isn't `const` in the
  actual API, from a `const` member function -- `request_` is now
  `mutable`, since reading a pin's value is logically const from
  `InputPin::read()`'s perspective regardless of that binding's own
  constness. **Not yet verified on real hardware** -- the toolchain used
  to compile-test it cannot itself produce valid ARMv6 output (see
  docs/CROSS_COMPILE.md's "Read this first"), so this confirms the code
  is correct C++ against the real API, not that it behaves correctly
  against a real GPIO chip.

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
- **`StatusLed`** drives the Codec Zero HAT's own onboard green status LED
  (GPIO23 -- see "Pin assignments" below for why that specific line).
  `blink_n_times()` is its one interesting method: schedules `n` on/off
  cycles on the `EventLoop` and returns immediately (a self-rescheduling
  chain of one-shot timers, not a blocking sleep loop), so it's safe to
  call right before `loop.run()` without delaying startup -- `daemon_main`
  does exactly that, 3 blinks, as a "the daemon is up" visual check with
  no console/network access needed.

### Pin assignments (placeholder)

**Everything below is illustrative, not verified against a schematic**,
with one exception: GPIO23/24/27 are fixed by the HiFiBerry Codec Zero
HAT's own spec, not a software choice -- **Power LED** (unconditional,
not GPIO-driven), **green status LED = GPIO23**, **red status LED =
GPIO24**, **tactile button = GPIO27**. `gpiochip0` lines 17 (bell), 5
(TCM1171 ring-mode), 22 (TCM1171 polarity), 6 (hook detect), and 23
(status LED) -- config/icom2000.conf's `[gpio.*]` sections, and
`daemon_main.cpp`'s built-in defaults if that file is missing -- exist
purely so the daemon has *something* to construct and run end-to-end
(ring-mode/hook-detect originally sat on 27/23, which would have
collided with the HAT's own button/LED the moment it was actually
populated -- moved to 5/6 instead). GPIO24 and 27 are reserved the same
way but nothing in this project drives them yet. Before wiring real
hardware:

1. Confirm the TCM1171's actual digital control pins on your board (RM,
   FR) and which GPIOs they land on.
2. Confirm how hook state reaches a GPIO at all. The TCM1171 itself
   exposes loop current as an *analog* signal (IL); on-hook/off-hook
   detection normally needs an external comparator or optocoupler between
   that pin and whatever GPIO `Tcm1171Controller::Pins::hook_detect`
   ends up wired to. `on_hook_edge()`'s polarity (`High` == off-hook) is a
   placeholder guess, not a measured fact.
3. Update the `[gpio.*]` sections in `config/icom2000.conf` (and its
   installed copy, `/etc/icom2000.conf`) -- see "Configuration" below.
   No code change needed; that's the point.

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

It should also **not** open a sound-card device string itself.
`make_null_audio_engine()` already takes a `config::StationRegistry`
(`src/config`, see "Configuration" below) and every `StationConfig` in it
carries a named ALSA PCM device per station, resolved from
`config/asound.conf` -- a real engine's constructor signature has nowhere
left to reach for a raw device string, only names the registry handed it.
See "Architecture invariants" for how that's enforced, not just requested.

## Configuration

`icom::config::ConfigFile` (`src/config/include/icom/config/config_file.hpp`)
is a hand-rolled reader for a small INI-style format -- `[section]`
headers, `key = value` lines, `#`/`;` comments -- backing
`config/icom2000.conf` (installed as `/etc/icom2000.conf`). Same
philosophy as the logging module's `configure_levels()`: no third-party
YAML/JSON library, because the actual configuration surface here is small
and flat, and results (not exceptions) cross the load/parse boundary
because this reads content a human edits by hand, where a typo should
become a clean startup error, not a stack unwind.

`ConfigFile::load()` distinguishes two failure shapes on purpose:

- **File missing** (`file_found = false`, `ok = true`): not an error.
  `daemon_main.cpp` logs a warning and falls back to built-in defaults
  that exactly match the shipped `config/icom2000.conf` -- so a fresh
  checkout with nothing installed at `/etc/icom2000.conf` behaves
  identically to having that file installed.
- **File exists but is malformed** (`ok = false`): `intercomd` prints the
  parse error (with a line number) and exits nonzero, the same fail-fast
  posture `ICOM_LOG`/`--log-level` already have. Silently keeping stale
  defaults next to a config file nobody noticed was broken would be worse.

Precedence for the handful of values with a CLI flag (`--socket`,
`--gpio-chip`) is: the flag, if given, wins; otherwise the config file's
`[daemon]` section; otherwise the built-in default. GPIO line numbers
(`[gpio.bell]`, `[gpio.tcm1171]`) have no CLI override -- there was no
reason to add one -- but follow the same config-file-then-built-in-default
fallback.

### Stations

`icom::config::StationRegistry` (`src/config/include/icom/config/station_registry.hpp`)
is the **one place** a station name is tied to a physical/logical audio
channel: `config/icom2000.conf`'s `[stations]` (the name list) and
`[station.<name>]` (that station's named capture/playback devices,
defined in `config/asound.conf`) sections. Everything else -- today, just
the `AudioEngine` factory; later, whatever actually streams audio --
refers to stations as `"door"`/`"inside"` and nothing else. No code
anywhere works with "left"/"right" or a channel index; there wouldn't
even be a natural place to put that, since a `StationConfig` only exposes
a name and two device-name strings.

This is what makes the eventual network extension (a third,
possibly-remote station, or "door"/"inside" moving from the codec's
analog crossbar to independent per-station digital capture/playback) a
config-file change: add a `[station.mumble]` entry, or repoint an
existing station's `capture_device`/`playback_device` at a different
named PCM device in `config/asound.conf`. Nothing that refers to stations
by name needs to change, because nothing ever encoded an assumption about
*how many* stations there are or *what* backs each one.

### Named ALSA devices

`config/asound.conf` (installed as `/etc/asound.conf`) defines the PCM
devices every `StationConfig` names: `icom_door_capture`,
`icom_door_playback`, `icom_inside_capture`, `icom_inside_playback`. Today
they're plain 1:1 aliases onto the one physical card (the file has the
full rationale and caveats) -- what matters architecturally is that this
is the *only* place a sound-card device string exists at all.
`scripts/check_architecture_invariants.sh` greps `src/` for one and fails
the build if it finds one (see "Architecture invariants"), so this isn't
just a convention, it's checked on every `ctest` run.

### Centralized mixer state

Mixer state (the DA7212's crossbar routing between "door" and "inside",
levels, switches) is set exactly once, at boot, by
`systemd/alsa-restore-codec-zero.service` running
`alsactl restore -f /etc/codec-zero-intercom.state` -- see that unit's
comments and `config/README.md` for why the state file itself isn't
shipped in this repo (it has to be captured from a live, already-tuned
system; fabricating DA7212 control names without hardware to verify them
against would be actively wrong). `intercomd` never calls into the ALSA
control API or shells out to a mixer CLI at runtime, full stop -- if a
user-facing volume control is ever added, it must go through exactly one
function, not calls scattered across whatever component happens to want
to change a level. `scripts/check_architecture_invariants.sh` greps
`src/` for that too.

Re-pointing a whole variant of the hardware (e.g. a DAC/ADC-based board
supporting the network extension) at a different mixer layout means
swapping the state file `alsa-restore-codec-zero.service` restores from --
no code change.

## Architecture invariants

Four invariants came out of a requirements pass explicitly aimed at
making the (currently unplanned, unimplemented) network/third-station
extension possible later without a rewrite. Each is enforced by
`scripts/check_architecture_invariants.sh`, which also runs as a `ctest`
test (`architecture_invariants`) so it can't silently bit-rot:

1. **No hardcoded ALSA card names in application code** (`grep` for a
   quoted `hw:`/`plughw:` in `src/`). All audio access goes through the
   named devices in `config/asound.conf` -- see "Named ALSA devices"
   above. Eases the extension: swapping single-process `plug` aliases for
   `dmix`/`dsnoop`/`route` (for concurrent per-station access once a
   network-connected station needs the card at the same time as the local
   ones) is then an `asound.conf`-only change.
2. **No scattered mixer manipulation** (`grep` for `amixer`/`snd_mixer_`
   in `src/`). See "Centralized mixer state" above. Eases the extension:
   a hardware variant with a different routing story is a different state
   file, not a different code path.
3. **`src/hw` and `src/gpio` never depend on `src/audio`** (`grep` for an
   `#include "icom/audio/` in either). Physical GPIO triggers (today:
   `Tcm1171Controller`'s hook-detect edge, `BellController`'s output pin;
   later: a door/talk button) are wired to whatever reacts to them only
   in the composition root (`daemon_main.cpp`), via plain interfaces and
   `std::function` callbacks -- `Tcm1171Controller`'s
   `StateChangeCallback` is the existing example. Eases the extension: a
   future button handler is a GPIO input pin plus a callback, exactly
   like the existing ones; it is structurally incapable of knowing
   whether what it triggers is an analog route, a Mumble push-to-talk, or
   both, because it never includes anything that would tell it.
4. **Stations are names, not channels.** Not independently `grep`-able
   (there is no "channel index" type left in the codebase to search for),
   but see "Stations" above -- `StationConfig` structurally has no room
   for one.

There is deliberately no fifth invariant abstracting "analog vs. digital
vs. Mumble backend" -- nothing here decides what a station's audio path
*is*, only what it's *named* and *called*, which is what the four
invariants above actually needed to hold up.

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

Four `ctest` cases: `icom_tests` covers `MockBackend` output/input
behavior, and that an injected mock edge reaches an `EventLoop` callback
end-to-end -- i.e. the same path `Tcm1171Controller` depends on in
production. `icom_logger_tests` covers the logging registry and
`configure_levels()` parsing (see "Logging" above). `icom_config_tests`
covers `ConfigFile` parsing (including the missing-vs-malformed-file
distinction) and `StationRegistry`'s defaults/overrides (see
"Configuration" above). `architecture_invariants` just runs
`scripts/check_architecture_invariants.sh` (see "Architecture
invariants"). `GpiodBackend` isn't in this `ctest` suite (host-dev builds
without it entirely, see "GPIO abstraction") but is compile/link-tested
under the `pi0-release`/`pi0-debug` presets, per that section. What's
still not covered: real hardware verification of `GpiodBackend`, and
anything in `src/hw`/`src/ipc`/`src/app` beyond logging and config
(straightforward to add following the same pattern; left out of this
pass to keep it to "one representative example per layer" per the
scaffold's brief).

## What's next

Roughly in the order it'd need doing to become a real intercom:

1. Confirm TCM1171 pin wiring against the actual board (see "Pin
   assignments"); fix polarity/line numbers.
2. Pulse-dial decoding off the same hook-detect edges
   `Tcm1171Controller` already timestamps.
3. A real `AudioEngine` against the Codec Zero (ALSA duplex, its own
   thread(s), a ring/tone generator for the TCM1171's ring cadence),
   opening the named devices `StationRegistry` already hands it.
4. Verified per-channel routing in `config/asound.conf` (currently plain
   1:1 aliases -- see that file's caveats) once there's real hardware to
   check `route`/channel indices against.
5. Persist/report richer line state over the control protocol (e.g. call
   duration, last-ring time) once there's a client that wants it.
6. `sd_notify()`/watchdog integration once there's a concrete failure mode
   worth detecting.
7. The network/third-station extension itself (a Mumble hub, or another
   Pi) -- explicitly out of scope for this pass; see "Configuration" and
   "Architecture invariants" for what was done instead to make it
   possible without a rewrite.
