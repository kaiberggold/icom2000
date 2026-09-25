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
src/core/   EventLoop (reactor) + LoopThread (an EventLoop on its own
            thread), SignalWatcher, logging -- no hardware or GPIO
            knowledge at all.
src/config/ File (INI-style reader) + StationRegistry -- see
            "Configuration" below. No hardware knowledge either.
src/gpio/   IOutputPin/IInputPin/IBackend interfaces, plus two
            implementations: MockBackend (in-process, for host dev/tests)
            and GpiodBackend (libgpiod, for target hardware).
src/hw/     Domain logic built only on the gpio interfaces: Pwm (software
            PWM, see "Software PWM" below) + BellController built on it,
            StatusLed, and Tcm1171Controller (the stateful, event-driven
            example).
src/audio/  IEngine interface, a no-op NullEngine (what the daemon
            runs today), and a first ALSA engine not yet wired in -- see
            "Audio boundary" below.
src/ipc/    The Unix-socket control protocol and its server.
src/app/    daemon_main.cpp -- the composition root. Everything above is
            constructed and wired together here; nothing else in the tree
            knows this file exists.
src/cli/    intercomctl -- a thin client over src/ipc's protocol.
tests/      Host-only unit tests (EventLoop + LoopThread + MockBackend +
            File + StatusLed + Pwm + the architecture-invariants guard
            script, see "Testing").
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

`intercomd` is reactor-based (`icom::core::EventLoop`,
`src/core/include/icom/core/event_loop.hpp`) on its main thread, plus one
second thread running a second, independent `EventLoop` for the small
amount of timing-sensitive periodic work that shouldn't have to wait its
turn on the main one -- see "Software PWM" below for what that's for and
why. Everything else in this section describes the main loop; the second
thread is deliberately kept minimal and is covered separately. On the main
loop, every source of work is a file descriptor:

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

This reactor model is a deliberate fit for a Pi Zero: one ARM1176JZF-S
core at ~1GHz has nothing to gain from a thread *pool* for this workload.
It's mostly single-threaded too, in the sense that matters: everything in
`src/core`, `src/gpio`, `src/hw`, and `src/ipc` that talks to the *main*
loop still needs no locking there, and a future real `IEngine` still won't
share the reactor thread either (see "Audio boundary" below). The one
deliberate exception is the second thread "Software PWM" introduces --
narrowly scoped (one more `EventLoop`, plus whatever needs to schedule
work onto it from the main thread), not a general invitation to reach for
`std::thread` elsewhere in this codebase.

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
independently-leveled logger -- roughly one per module -- writing straight
to the systemd journal in its native protocol, not `syslog(3)`.
`journalctl -u intercomd` shows everything; `journalctl -t
icom2000` filters by the process-wide identifier `initJournal()` sets in
`main()`; `journalctl ICOM_COMPONENT=hw.bell` filters to just one
component's lines, via a custom journal field every `log()` call attaches
(see "Per-component levels" below for the component names) -- this is the
"tag" that `syslog(3)`'s plain-text messages can't offer, and the reason
this module uses the journal API directly rather than going through
syslog.

This is a deliberate, accepted portability tradeoff: logging only works
under systemd. That costs nothing here -- the sole deployment target,
Raspberry Pi OS, is itself systemd-based -- and it's a strictly stronger
guarantee than the `syslog(3)` version this replaced, which merely assumed
*something* was listening on `/dev/log` (journald, in practice, on every
install this project targets anyway).

### The journal protocol, without libsystemd

`libsystemd`'s `sd_journal_send()` would do the same job, but it would make
`libsystemd` a build dependency, and the self-built ARMv6 cross toolchain
(docs/CROSS_COMPILE.md) has no Raspberry Pi OS libraries to link it
against without a whole Pi sysroot. What it puts on the wire is simple and
documented (https://systemd.io/JOURNAL_NATIVE_PROTOCOL/), so
`icom/core/journal.hpp` does that directly instead: one `AF_UNIX` datagram
to `/run/systemd/journal/socket` per entry, each field `NAME=value\n`, or
-- for a value containing a newline -- `NAME\n`, the value's length as a
64-bit little-endian integer, then the raw value and `\n`. Every entry
carries `MESSAGE=[component] text`, `PRIORITY`, `SYSLOG_IDENTIFIER`,
`SYSLOG_FACILITY` and `ICOM_COMPONENT`.

Two deliberate differences from `sd_journal_send()`: the send is
non-blocking (`MSG_DONTWAIT`), so if journald is backed up an entry is
dropped rather than stalling the caller -- which matters once the audio
thread logs an underrun -- and an entry too big for one datagram is
dropped too, where libsystemd would fall back to passing a memfd. Log
lines here are far below that limit.

(This module started from a request to log "to the kernel log" --
`/dev/kmsg`/`dmesg`. That's a real, different thing: writing `/dev/kmsg`
requires `CAP_SYSLOG` or root and shows up in `dmesg` whether or not a log
daemon is even running, whereas both `syslog(3)` and the journal socket
need no special privilege but require something listening on the other
end. This project logs to the journal -- it's the conventional destination
for a userspace daemon's own logs under systemd, works with the
unprivileged systemd unit this project already ships, and unlocks
per-entry structured fields `/dev/kmsg` and plain `syslog(3)` text both
lack.)

### Per-component levels

Each `.cpp` file that logs declares its own logger once, at file scope:

```cpp
namespace {
icom::core::Logger& log = icom::core::getLogger("hw.bell");
} // namespace
```

and then just calls `log.debug(...)`/`.info(...)`/`.warn(...)`/`.error(...)`
wherever it wants -- that's the whole mechanism for "insert log output
where I want": add a `log` line if the file doesn't have one yet
(matching the dotted `module.submodule` naming already in use -- see any
existing `.cpp` under `src/` for the pattern), then log. Current
components: `core.event_loop`, `gpio.mock`, `gpio.gpiod`, `hw.bell`,
`hw.pwm`, `hw.status_led`, `hw.tcm1171`, `audio`, `audio.alsa`,
`ipc.control_server`, `app`.

Levels are set at startup, per component, via the `ICOM_LOG` environment
variable or `intercomd --log-level`, both parsed by the same
`configureLevels()`:

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

`--log-console` (`icom::core::setConsoleOutput(true)`) mirrors every
logged message to stderr, in addition to the journal, at whatever level(s)
`--log-level`/`ICOM_LOG` already set -- off by default, since a
systemd-managed run has nothing to gain from it (stderr just lands in the
journal a second time). It exists for interactive/debugger use, where
waiting on a second `journalctl -f` window is friction a plain `std::cerr`
line doesn't have: the "Debug intercomd" and "Debug
intercomd on Pi Zero" `.vscode/launch.json` configs both pass it, and with
`"externalConsole": false` (already set), VS Code's cppdbg captures that
stderr straight into the Debug Console. The remote-gdbserver config is the
one exception worth knowing: gdbserver doesn't pipe the debuggee's stdio
back over the wire, so that flag (baked into `pi-start-gdbserver`'s ssh
command in `.vscode/tasks.json`) shows up in the **task's own terminal
tab** ("pi-start-gdbserver"), not the Debug Console -- still one click away
in VS Code, just a different panel.

### Why the registry is a function-local static

`getLogger()`'s registry is a Meyer's singleton (a `static Registry` local
to a function), not a plain namespace-scope global. Several `.cpp` files
declare their `Logger& log` as a namespace-scope variable, which runs
during that translation unit's *dynamic initialization* -- and the C++
standard leaves the relative order of dynamic initialization across
different translation units unspecified. A plain global registry could
easily end up read by one TU's `log` initializer before another TU's
initializer had constructed it. A function-local static sidesteps the
question entirely: it's guaranteed to be constructed on its first use, no
matter which TU that first use comes from.

### Logging tests

`tests/logger_tests.cpp` covers the registry (identity, per-component
levels) and `configureLevels()`'s parsing, including that a rejected spec
changes nothing -- plus the journal writer: both field encodings, that
`sendJournalEntry()` delivers exactly the encoded bytes to a socket the
test binds itself, and that a missing socket fails without blocking.

That real journald accepts the encoding isn't in the suite (it needs a
running `systemd-journald`), so it was checked by hand: with a
`systemd-journald` running, `intercomd`'s entries came back through
`journalctl ICOM_COMPONENT=hw.pwm` and `journalctl -t icom2000` with every
field intact, and a message containing a newline and a tab came back
byte-for-byte through the binary-length encoding.

## GPIO abstraction

`icom::gpio::IOutputPin` / `IInputPin` / `IBackend`
(`src/gpio/include/icom/gpio/digital_pin.hpp`) are the only thing
`src/hw` and `src/app` are allowed to depend on for GPIO access -- neither
includes `<gpiod.hpp>` or knows libgpiod exists. Two backends implement
that interface:

- **MockBackend** (`src/gpio/src/mock`): in-process, backed by an
  `eventfd` per input pin so it plugs into the same `EventLoop` an fd from
  a real chip would. `injectMockEdge()` lets tests (and a future
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
  `IInputPin::read()`'s perspective regardless of that binding's own
  constness. **Not yet verified on real hardware** -- the toolchain used
  to compile-test it cannot itself produce valid ARMv6 output (see
  docs/CROSS_COMPILE.md's "Read this first"), so this confirms the code
  is correct C++ against the real API, not that it behaves correctly
  against a real GPIO chip.

Which backend `makeDefaultBackend()` returns is a compile-time choice
(`ICOM_WITH_LIBGPIOD`), not a runtime one -- there is no reason a Pi
binary should carry mock code or a dev-host binary should require
libgpiod headers to exist.

## Hardware layer

- **`BellController`** (`src/hw`) drives the bell via software PWM
  (`Pwm`, see "Software PWM" below) rather than a plain digital on/off --
  `ring()`/`silence()`/`ringFor(duration, loop)` are the same simple API
  as before (this used to just wrap one `IOutputPin`), they now move the
  PWM's duty time between a configured "ring" value and zero instead of
  writing the pin directly. Read this one first, then Pwm's own header
  for why a relay/buzzer wants PWM instead of a flat digital drive.
- **`Tcm1171Controller`** is the event-driven example: it owns two output
  pins (ring-mode enable, line polarity) and one input pin (hook detect),
  registers the input's edge fd with the `EventLoop` in its constructor,
  and exposes `LineState` (`OnHook`/`Ringing`/`OffHook`/`Fault`) plus
  `startRinging()`/`stopRinging()`.
- **`StatusLed`** drives the Codec Zero HAT's own onboard green status LED
  (GPIO23 -- see "Pin assignments" below for why that specific line).
  `blinkNTimes()` is its one interesting method: schedules `n` on/off
  cycles on an `EventLoop` and returns immediately (a self-rescheduling
  chain of one-shot timers, not a blocking sleep loop) -- `daemon_main`
  calls it right as the daemon starts up, 3 blinks, as a "the daemon is
  up" visual check with no console/network access needed. Runs on the
  same background loop `Pwm` does now (see "Software PWM"), not the main
  one, so its destructor has the same blocking-wait-for-cancellation
  shape `Pwm`'s does, for the same reason -- see its header comment.

## Software PWM

`icom::hw::Pwm` (`src/hw/include/icom/hw/pwm.hpp`) cycles one GPIO output
High for a configurable `dutyTime` out of every configurable `period`
(10ms by default, both settable per-use -- `BellController`'s constructor
takes them from `config/icom2000.conf`'s `[gpio.bell]` section,
`pwm_period_ms`/`pwm_duty_ms`), indefinitely, until destroyed. It's what
`BellController` now drives the bell through instead of a flat digital
on/off -- a relay/buzzer isn't a clean digital load, and driving it at
less than 100% duty controls how hard it strikes/how loud it buzzes (and
draws less current continuously driving some relay coils would rather not
have to).

**Why this needs its own thread.** A 10ms PWM period means toggling the
pin roughly every 5ms at 50% duty -- reliably, on schedule, regardless of
whatever else the daemon is doing. Sharing the main `EventLoop` would mean
that timing degrades every time an IPC command, a GPIO edge, or any other
main-loop callback takes a few milliseconds, which is exactly the kind of
jitter "Audio boundary" below already rules out sharing the reactor thread
for. So `Pwm` runs on a second `EventLoop`, on its own thread
(`icom::core::LoopThread`, `src/core/include/icom/core/loop_thread.hpp`)
-- `daemon_main.cpp` constructs exactly one `LoopThread` and both `Pwm`
(the bell) and `StatusLed`'s `blinkNTimes()` (the status LED) share its
loop, rather than each spinning up a thread of its own.

**Getting work onto that thread safely.** Every `EventLoop` method except
one (`addFd`, `addTimer`, ...) is only safe to call from whichever thread
is already running that loop's `run()` -- calling any of them from another
thread races the loop's own internal bookkeeping. The one exception is
`EventLoop::post(fn)`: thread-safe by design (a mutex-guarded queue plus
the loop's existing wakeup fd), it hands `fn` to the loop's own thread to
actually run. `Pwm`'s constructor uses `post()` internally to schedule its
first cycle, so constructing one from the main thread (as `daemon_main.cpp`
does) is safe without the caller having to think about it.
`StatusLed::blinkNTimes()` does NOT do this internally -- it's meant to be
usable against the main loop too, where doing so would be pointless
overhead -- so a caller pointing it at a `LoopThread`'s loop has to
`post()` the call itself, which is exactly what `daemon_main.cpp` does for
the startup blink.

**Destruction is the subtle part.** A cycle in flight captures `this` in
its next scheduled timer callback; if the object were destroyed while that
callback was still pending, the callback would eventually fire into freed
memory. Both `Pwm` and `StatusLed` close this with the same pattern: their
destructors `post()` a cancellation onto the loop and **block** until it
has actually run there (a mutex + condition variable, not just firing the
post and returning) -- only once that's confirmed is it safe to let the
object's memory go. That blocking wait has one real requirement of its
own: the loop has to still be *actively running* (something still calling
its `run()`) for the entire time such an object exists, or the wait never
returns. A `LoopThread`'s own loop satisfies this by construction, for as
long as the `LoopThread` itself exists; the daemon's main loop does NOT
once its own `run()` has returned during shutdown -- which is exactly why
`Pwm` and backgroundLoop-targeted `blinkNTimes()` calls are for a
`LoopThread`'s loop specifically, never the main one. `daemon_main.cpp`'s
declaration order encodes the corresponding requirement on ITS side:
`LoopThread` is declared before anything that uses its loop, so it's the
last thing destroyed at shutdown (C++ destroys locals in reverse
declaration order) -- getting this backwards would mean `Pwm`/`StatusLed`
posting a cancellation to a `LoopThread` whose background thread has
already been asked to stop (or worse, whose `EventLoop` has already been
destroyed).

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
   that pin and whatever GPIO `Tcm1171Controller::Pins::hookDetect`
   ends up wired to. `onHookEdge()`'s polarity (`High` == off-hook) is a
   placeholder guess, not a measured fact.
3. Update the `[gpio.*]` sections in `config/icom2000.conf` (and its
   installed copy, `/etc/icom2000.conf`) -- see "Configuration" below.
   No code change needed; that's the point.

## Audio boundary

`icom::audio::IEngine` (`src/audio/include/icom/audio/audio_engine.hpp`)
defines the seam a real implementation plugs into; `NullEngine` is what
the daemon runs today, so it builds, runs, and reports `audio=down`...
`audio=up`-but-silent honestly via `intercomctl status` without every
other component needing to special-case "audio doesn't exist yet".

### ALSA engine (step 1 of 3)

`makeAlsaEngine(captureFrom, playbackTo)`
(`src/audio/src/alsa_audio_engine.cpp`) is the first real
implementation, deliberately narrow: **one** mono route, 48 kHz S16_LE,
capturing from one station's named capture device and writing each
period straight out to another station's named playback device, on its
own `std::jthread`. It proves the ALSA plumbing in isolation --
open/configure via `snd_pcm_set_params()` (the `plug` layer in
`config/asound.conf` handles any rate/format conversion the codec needs),
playback primed with one period short of a full buffer of silence
(~40 ms of slack before an underrun, and roughly the route's latency),
and overrun/underrun/suspend recovery via `snd_pcm_recover()`. `start()`
never throws on a device problem: it logs why and leaves `isRunning()`
false, and so does an unrecoverable error on the audio thread later --
audio failing shouldn't take the bell down with it.

**Not yet wired into the daemon**: `daemon_main.cpp` still runs
`makeNullEngine()`. The remaining steps:

2. Both stations at once, a lock-free queue to the reactor thread, and
   real-time scheduling for the audio thread(s). Blocked on
   `config/asound.conf` first: all four named devices are `plug` aliases
   onto the one `hw:Zero`, and a raw `hw` device only allows one capture
   stream open at a time -- a second station's capture needs `dsnoop`
   (and shared playback `dmix`) there, which that file's own caveats say
   to verify on real hardware first.
3. Swap it in as the daemon's engine and test against a real Pi Zero +
   Codec Zero. **Open design question for this step**: `config/asound.conf`
   currently says the live door/inside audio runs entirely inside the
   DA7212 codec's own analog crossbar (set once at boot by alsactl), with
   these PCM devices reserved for intercomd's own sounds. A digital
   door->inside route on top of that would double the audio, so step 3
   has to pick one path for live audio, not run both.

Tested without a sound card by `tests/alsa_engine_tests.cpp`: it points
`HOME` at a scratch `.asoundrc` defining test PCMs on alsa-lib's own
`file` plugin over its `null` device. Capture reads a known sample
pattern from a FIFO -- not a regular file, because `null` isn't paced in
real time and the engine would otherwise spin at hundreds of MB/s; over
a FIFO it consumes exactly what the test wrote and then blocks -- and the
test checks the pattern arrives in the playback file bit-exact, with no
dropped, repeated, or reordered samples (confirmed to catch a deliberately
duplicated period and a deliberately dropped sample per period).

When it's implemented, it should **not** join the reactor thread. Two mono
ALSA duplex streams against the Codec Zero (handset audio, and
door/ambient) need real-time-ish scheduling that a single-threaded
`poll()` loop shouldn't be put at risk of jittering (a slow control-socket
client blocking the loop for a few ms is a UX blemish; blocking the audio
callback for a few ms is an audible glitch). The expected shape: its own
thread(s), talking to the reactor thread via a lock-free queue or a
handful of atomics, not shared mutable state.

It should also **not** open a sound-card device string itself.
`makeNullEngine()` already takes a `config::StationRegistry`
(`src/config`, see "Configuration" below) and every `Station` in it
carries a named ALSA PCM device per station, resolved from
`config/asound.conf` -- a real engine's constructor signature has nowhere
left to reach for a raw device string, only names the registry handed it.
See "Architecture invariants" for how that's enforced, not just requested.

## Configuration

`icom::config::File` (`src/config/include/icom/config/config_file.hpp`)
is a hand-rolled reader for a small INI-style format -- `[section]`
headers, `key = value` lines, `#`/`;` comments -- backing
`config/icom2000.conf` (installed as `/etc/icom2000.conf`). Same
philosophy as the logging module's `configureLevels()`: no third-party
YAML/JSON library, because the actual configuration surface here is small
and flat, and results (not exceptions) cross the load/parse boundary
because this reads content a human edits by hand, where a typo should
become a clean startup error, not a stack unwind.

`File::load()` distinguishes two failure shapes on purpose:

- **File missing** (`fileFound = false`, `ok = true`): not an error.
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
the `IEngine` factory; later, whatever actually streams audio --
refers to stations as `"door"`/`"inside"` and nothing else. No code
anywhere works with "left"/"right" or a channel index; there wouldn't
even be a natural place to put that, since a `Station` only exposes
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
devices every `Station` names: `icom_door_capture`,
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
   but see "Stations" above -- `Station` structurally has no room
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

Commands are registered by name (`registerCommand()`) rather than
switched on inside `ControlServer` -- `daemon_main.cpp` is the only place
that knows `BELL`, `LINE`, `STATUS`, and `PING` exist. Adding a command
later means adding a `registerCommand()` call at the wiring site, not
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

Seven `ctest` cases: `icom_tests` covers `MockBackend` output/input
behavior, that an injected mock edge reaches an `EventLoop` callback
end-to-end (the same path `Tcm1171Controller` depends on in production),
`EventLoop::post()`, and `LoopThread` (confirming a posted callback
genuinely runs on its background thread, not just eventually). Threaded
tests there use a real background thread rather than faking one, since
`post()`'s whole point is being safe to call across real threads.
`icom_logger_tests` covers the logging registry, `configureLevels()`
parsing, and the journal protocol writer (see "Logging" above). `icom_config_tests` covers `File` parsing
(including the missing-vs-malformed-file distinction) and
`StationRegistry`'s defaults/overrides (see "Configuration" above).
`icom_hw_tests` covers `StatusLed` -- on/off state tracking, and
`blinkNTimes()` against a real `LoopThread` (needed for correctness, not
just realism: see "Software PWM" for why `~StatusLed()` requires an
actively-running loop). `icom_pwm_tests` covers `Pwm` the same way:
construction/`setDutyTime()` clamping, and that 0%/100%/partial duty
actually produce the expected pin behavior over a real background loop.
`icom_audio_tests` covers the ALSA engine with no sound card, via
file-backed test PCMs: bad device names leave it stopped, and a known
sample pattern arrives bit-exact at playback (see "Audio boundary").
`architecture_invariants` just runs
`scripts/check_architecture_invariants.sh` (see "Architecture
invariants"). `GpiodBackend` isn't in this `ctest` suite (host-dev builds
without it entirely, see "GPIO abstraction") but is compile/link-tested
under the `pi0-release`/`pi0-debug` presets, per that section. What's
still not covered: real hardware verification of `GpiodBackend`, and
`src/ipc`/`src/app` beyond logging and config (straightforward to add
following the same pattern; left out of this pass to keep it to "one
representative example per layer" per the scaffold's brief).

## What's next

Roughly in the order it'd need doing to become a real intercom:

1. Confirm TCM1171 pin wiring against the actual board (see "Pin
   assignments"); fix polarity/line numbers.
2. Pulse-dial decoding off the same hook-detect edges
   `Tcm1171Controller` already timestamps.
3. Finish the ALSA `IEngine` (step 1 of 3 done -- see "Audio boundary"
   for steps 2 and 3), plus a ring/tone generator for the TCM1171's ring
   cadence.
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
