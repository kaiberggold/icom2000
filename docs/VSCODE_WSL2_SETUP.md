# VS Code on Windows + WSL2

Everything this project needs (cmake, ninja, a C++20 compiler, the ARMv6
cross toolchain, gdb) is Linux tooling. WSL2 gives you a real Linux
userspace to run all of it in, and VS Code's WSL extension makes that
userspace look, from the editor's side, exactly like a native Linux dev
box -- so most of this document is just "the normal Linux setup", plus the
handful of things that are genuinely WSL2/Windows-specific: opening the
repo, reaching the Pi over the network, and where `gdb-multiarch` and the
cross toolchain get installed.

This gets you, all driven from inside VS Code:

- Building `host-dev` (mock GPIO, runs and debugs entirely inside WSL2)
  and `pi0-release`/`pi0-debug` (cross-compiled for the Pi Zero) from the
  same window, switchable from the CMake Tools status bar.
- `F5` debugging of `intercomd` locally inside WSL2.
- `F5` debugging of `intercomd` *running on the Pi Zero itself*, via
  `gdbserver` on the target and `gdb-multiarch` in WSL2 talking to it over
  TCP -- real breakpoints, real GPIO state, real timing.

## 1. Prerequisites

- Windows 10 (2004+) or Windows 11 with WSL2 enabled, and a Linux
  distro installed (Ubuntu is assumed below; Debian works the same way).
  `wsl --install` from an elevated PowerShell if you don't have one yet.
- VS Code on Windows, plus the **WSL** extension
  (`ms-vscode-remote.remote-wsl`).
- **Clone the repo inside the WSL2 filesystem**, not under `/mnt/c/...`.
  From a WSL2 terminal:
  ```sh
  git clone <repo-url> ~/icom2000
  cd ~/icom2000
  code .
  ```
  `code .` from inside WSL2 opens VS Code in "WSL: Ubuntu" mode
  automatically (bottom-left green indicator confirms it). Working from
  `/mnt/c` instead works but is dramatically slower for anything
  filesystem-heavy (CMake configure, git status, the editor's own file
  watcher) -- there's no upside to it here.
- When VS Code opens the folder the first time, it'll offer to install
  the recommended extensions from `.vscode/extensions.json`
  (`ms-vscode.cpptools`, `ms-vscode.cmake-tools`) *inside* the WSL side --
  accept that.

## 2. One-time package install (inside WSL2)

```sh
sudo apt update
sudo apt install build-essential cmake ninja-build gdb-multiarch openssh-client git
```

`gdb-multiarch` (not plain `gdb`) is what talks to the Pi Zero's `gdbserver`
remotely: it understands ARM code without needing to itself be a
cross-compiled binary, because `gdbserver` does the actual execution --
`gdb-multiarch` just interprets the wire protocol and the target's debug
info. This is a completely different concern from the compiler cross
toolchain covered next.

For the `pi0-release`/`pi0-debug` presets you additionally need an ARMv6
cross toolchain and (usually) a sysroot -- **read
[`docs/CROSS_COMPILE.md`](CROSS_COMPILE.md) before doing anything here**,
the Pi Zero 1.1's ARMv6 vs. every-other-Pi's ARMv7 distinction is the one
mistake that wastes the most time. Both approaches documented there
(QEMU-chroot, or a real cross toolchain + rsynced sysroot) work fine
inside WSL2.

If you go the QEMU-chroot route and it needs `binfmt_misc`/
`qemu-user-static` registration: WSL2's default init isn't full systemd,
which some of that tooling's setup scripts assume. Easiest fix -- enable
real systemd in WSL2 (Ubuntu 22.04+/WSL 0.67.6+ support this): add to
`/etc/wsl.conf` inside the distro,
```ini
[boot]
systemd=true
```
then `wsl --shutdown` from PowerShell and reopen the distro. Everything
else in this document works identically either way.

## 3. First-time CMake configure

`host-dev` needs nothing beyond what you just installed:

```sh
cmake --preset host-dev
cmake --build --preset host-dev
ctest --preset host-dev --output-on-failure
```

`pi0-release`/`pi0-debug` need `ICOM_PI_SYSROOT` pointed at wherever you
put the sysroot from `docs/CROSS_COMPILE.md`:

```sh
cmake --preset pi0-release -DICOM_PI_SYSROOT=$HOME/pi-sysroot
cmake --preset pi0-debug   -DICOM_PI_SYSROOT=$HOME/pi-sysroot
```

You only need to *configure* each preset once (the cache variable sticks);
after that, building is just `cmake --build --preset <name>` or the
matching task/CMake Tools button. `pi0-debug` is identical to
`pi0-release` except `-O0 -g` instead of `-O2 -g` -- use it when you're
about to attach a debugger and optimized code's variable/step behavior is
getting in the way; use `pi0-release` for anything you're actually
deploying.

From here on, CMake Tools' status bar (bottom of the window) shows the
active configure preset and lets you switch between `host-dev`,
`pi0-release`, and `pi0-debug` -- switching also switches which
`compile_commands.json` IntelliSense reads, so jump-to-definition and
squiggles stay correct for whichever target you're looking at code for.
`Ctrl+Shift+B` (or the "cmake-build-host-dev" task) builds `host-dev` by
default; the other presets have their own tasks
(`cmake-build-pi0-release`, `cmake-build-pi0-debug`) reachable from
**Terminal > Run Task**.

## 4. SSH access to the Pi

The debug-on-target and deploy tasks all shell out to `ssh`/`scp` from
WSL2, non-interactively -- set up key-based auth once so they don't hang
waiting for a password prompt VS Code's task runner won't show you:

```sh
ssh-keygen -t ed25519            # if you don't already have a key
ssh-copy-id pi@raspberrypi.local  # replace with your Pi's user/host
ssh pi@raspberrypi.local           # confirm it connects with no prompt
```

WSL2's NAT networking can reach devices on your LAN by hostname/IP the
same way any other machine on the network can, so `raspberrypi.local`
(mDNS) or a plain IP both work from inside WSL2 without extra
configuration in the common case. If your network setup blocks mDNS or
WSL2 outbound LAN traffic, use the Pi's IP address directly.

On the Pi itself:

```sh
sudo apt install gdbserver
sudo usermod -aG gpio,audio "$USER"   # log out/in for this to take effect
```

The `gpio`/`audio` group membership matters for a debug session that's
actually touching hardware: `gdbserver` runs the inferior as whatever user
you SSH'd in as, and that user needs the same GPIO/audio device access the
systemd-run production binary gets from `systemd/intercomd.service`'s
`SupplementaryGroups=`.

## 5. Building and running (host)

Either use CMake Tools' build button with `host-dev` active, or **Terminal
> Run Task > cmake-build-host-dev**. Run it directly in a WSL2 terminal to
poke at it manually:

```sh
./build/host-dev/src/app/intercomd --socket /tmp/icom2000.sock &
./build/host-dev/src/cli/intercomctl -s /tmp/icom2000.sock status
```

## 6. Debugging on host

**Run and Debug (`Ctrl+Shift+D`) > "Debug intercomd (host, mock GPIO)" >
F5.** This builds `host-dev` (the `preLaunchTask`) and launches
`intercomd` under `gdb` entirely inside WSL2 -- set breakpoints in any
`src/` file first, they'll bind normally. `"Debug intercomctl (host,
against a running intercomd)"` does the same for the CLI, so you can
single-step both sides of a control-socket exchange in one window if you
start `intercomd` separately (or under its own debug session) first.

## 7. Debugging on the Pi Zero itself

**Run and Debug > "Debug intercomd on Pi Zero (remote gdbserver)" > F5.**
The first time (per VS Code session) you'll be prompted for the Pi's
hostname, SSH user, and a scratch directory on the Pi to copy the debug
binary into -- these are cached for the rest of the session (see the
`inputs` block in `.vscode/tasks.json`/`launch.json` if you want to change
the defaults).

What actually happens, all as the configuration's `preLaunchTask` chain
(`.vscode/tasks.json`):

1. `cmake --build --preset pi0-debug` -- cross-compiles a fresh,
   unoptimized, `-g` binary.
2. `scp`'s that exact binary to the Pi as `intercomd-debug`.
3. `ssh`'s in and starts `gdbserver :2345 ./intercomd-debug ...` there,
   killing any stale one first. VS Code waits for gdbserver's own
   "Listening on port 2345" line (via the task's background problem
   matcher) before moving on -- you'll see this happen in the integrated
   terminal.
4. Only then does VS Code start a local `gdb-multiarch`, pointed at the
   **local** `build/pi0-debug/src/app/intercomd` for symbols and at
   `<pi-host>:2345` for the actual running process. Those two must be the
   exact same build, which is why step 1-2 always run immediately before
   this connects -- don't skip them by hand-editing the binary on the Pi
   in between.

It stops at `main()` (`stopAtEntry`) so you have a moment to set further
breakpoints before continuing. From there it's a normal debug session --
breakpoints, stepping, variable inspection, watch expressions -- just
proxied over TCP to `gdbserver`, so expect a bit more latency on
step/continue than the local host config.

`gdbserver`'s default port `:2345` has no authentication -- it's bound to
whatever interface the Pi is reachable on for as long as the task is
running, so treat a debug session as something you start when you need it
and let finish (gdbserver exits on its own once the debugged process exits
or you stop debugging), not something to leave running unattended.

## Troubleshooting

- **"Could not connect to gdbserver" / connection refused** -- the
  background task's readiness detection didn't see gdbserver's "Listening
  on port" line before VS Code tried to connect. Check the integrated
  terminal for the `pi-start-gdbserver` task; a `pkill`/`ssh` failure or a
  firewall blocking port 2345 on the Pi's side will show up there.
- **Port already in use on the Pi** -- a previous debug session's
  `gdbserver` didn't exit cleanly. The task already `pkill`s a stale one
  before starting a new one; if that's not enough, `ssh` in and
  `pkill gdbserver` manually.
- **Breakpoints don't bind / wrong source shown** -- almost always a stale
  binary on the Pi not matching what gdb has symbols for. Re-run the debug
  session (it always rebuilds+redeploys first) rather than reusing an old
  `gdbserver` instance.
- **GPIO calls fail / permission denied on the target** -- the SSH user
  needs `gpio`/`audio` group membership (step 4); a shell you were already
  in before running `usermod` won't pick up the new group until you
  reconnect.
- **`arm-linux-gnueabihf-gcc not found` when configuring `pi0-*`** -- see
  [`docs/CROSS_COMPILE.md`](CROSS_COMPILE.md); this is expected until the
  cross toolchain step there is done.
