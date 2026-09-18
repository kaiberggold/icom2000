# Cross-compiling for the Pi Zero 1.1

## Read this first: ARMv6, not ARMv7

The Pi Zero (1.x) and the original Pi 1 use a Broadcom BCM2835 SoC with a
single **ARM1176JZF-S** core: **ARMv6** architecture, VFPv2 FPU. Every
later Raspberry Pi -- Pi 2, 3, 4, 400, Compute Modules, and even the
*Zero 2* -- is ARMv7-A or ARMv8-A.

Debian's and Ubuntu's `armhf` port targets an **ARMv7-A** baseline by
distro policy (`-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard`, roughly).
That means:

- `sudo apt install crossbuild-essential-armhf` on your dev machine gives
  you a compiler that defaults to ARMv7 code generation.
- Any prebuilt `.deb` you'd pull into a generic Debian/Ubuntu armhf sysroot
  (including a stock `libgpiod-dev`) is itself compiled for ARMv7.

A binary or `.so` built that way **will not run on a Pi Zero 1.1** -- it
crashes with `SIGILL` (illegal instruction) the moment it hits an
instruction the ARM1176 doesn't implement. This is *the* recurring trap
people hit cross-compiling for the Zero/Pi 1, and it's why "just use a
generic armhf cross toolchain" (correct advice for a Pi 3/4) is wrong
advice here.

Raspberry Pi OS itself sidesteps this by shipping its own ARMv6-baseline
32-bit distro (built to run identically on every Pi model, Zero through
4), which is why "Raspberry Pi OS" and "Debian armhf" are not
interchangeable for this board even though both call themselves `armhf`.

Two ways to get a correct toolchain, in order of recommendation:

## Option A (recommended): build inside a real Raspberry Pi OS rootfs via QEMU

Compile *inside* an actual Raspberry Pi OS (32-bit) chroot, emulated by
`qemu-arm-static` + `binfmt_misc`, on your dev machine. This sidesteps the
ARMv6-vs-ARMv7 toolchain question entirely -- gcc inside the chroot is the
one Raspberry Pi OS itself ships, targeting the right architecture by
construction -- and it gets you exact glibc/libgpiod ABI compatibility for
free, which a hand-built cross toolchain + rsynced sysroot can subtly get
wrong (mismatched glibc versions between host-built cross-libs and
target). It's slower than a native cross toolchain (QEMU user-mode
emulation overhead), but still dramatically faster than building natively
on a Pi Zero's single ARM1176 core, and it needs no `ICOM_PI_SYSROOT`.

```sh
sudo apt install qemu-user-static debootstrap  # or: podman/docker + an arm32v7-* image is NOT this -- must be armv6 Raspberry Pi OS specifically

# Simplest path: download a Raspberry Pi OS Lite (32-bit) image, mount its
# root partition, and chroot into it with qemu-arm-static registered:
#   https://www.raspberrypi.com/software/operating-systems/
# (the exact mount/chroot dance is standard Raspberry Pi OS QEMU-chroot
# tooling -- e.g. the `pi-gen`/`qemu-nbd` approach documented alongside
# that image -- and is intentionally not duplicated here since the exact
# commands depend on your host distro's QEMU packaging.)

# Once inside the chroot (now effectively "on" an armv6 Raspberry Pi OS):
sudo apt install build-essential cmake ninja-build libgpiod-dev
cd /path/to/icom2000   # bind-mount the source tree into the chroot
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DICOM_BUILD_TESTS=OFF
cmake --build build
```

This is effectively "native build," just running under emulation on a
faster host CPU instead of on the Pi Zero's own core -- so
`CMakePresets.json`'s `host-dev` preset's shape (no toolchain file) is
closer to what you want here than `pi0-release` is; skip the toolchain
file and just configure directly as shown above.

## Option B: a real ARMv6 cross toolchain + an rsynced sysroot

If you already have (or want to build with `crosstool-NG`) an
`arm-linux-gnueabihf-gcc`/`g++` that itself targets `arm1176jzf-s`/armv6,
`cmake/toolchain-arm-linux-gnueabihf.cmake` and the `pi0-release` preset
are set up for it:

1. **Get an armv6-targeting toolchain.** Options, roughly in order of
   effort: a `crosstool-NG` build configured for `arm-unknown-linux-gnueabihf`
   with `arm1176jzf-s`/`armv6` CPU settings; a community-maintained
   Pi-specific toolchain if you can find one still maintained for current
   glibc; or the historical `tools/arm-bcm2708/...` toolchain from the
   `raspberrypi/tools` repo (deprecated, old glibc -- fine for a quick
   experiment, not for anything you'll maintain).
2. **Get a sysroot.** Headers and `.so`s (glibc, libgpiod, ALSA) have to
   match what's actually on the Pi, so pull them from a real device rather
   than from Debian/Ubuntu packages:
   ```sh
   rsync -avz --rsync-path="sudo rsync" \
       pi@raspberrypi.local:/lib /usr/include /usr/lib \
       ./pi-sysroot/
   ```
3. **Configure and build:**
   ```sh
   cmake --preset pi0-release -DICOM_PI_SYSROOT=$(pwd)/pi-sysroot
   cmake --build --preset pi0-release
   ```
   `ICOM_PI_SYSROOT` feeds `CMAKE_SYSROOT`/`CMAKE_FIND_ROOT_PATH` and
   `PKG_CONFIG_SYSROOT_DIR`/`PKG_CONFIG_LIBDIR` (see the toolchain file),
   so `pkg_check_modules(... libgpiod ...)` in `src/gpio/CMakeLists.txt`
   resolves against the target's `libgpiod.pc`, not the host's.

If step 1 turns out to be more yak-shaving than it's worth, fall back to
Option A -- it needs no custom toolchain at all.

## Sanity-checking a binary before deploying it

Regardless of which option you used:

```sh
file build/pi0-release/src/app/intercomd
# expect: ELF 32-bit LSB executable, ARM, EABI5 version 1 (SYSV) ...

arm-linux-gnueabihf-readelf -A build/pi0-release/src/app/intercomd | grep -i "^  Tag_CPU_arch"
# expect: Tag_CPU_arch: v6 (or a note naming ARM1176/ARMv6, not v7)
```

If that second line says `v7`, the binary will not run on the Zero --
stop and fix the toolchain before copying anything over.

## Deploying

Once a `pi0-release` build exists, `scripts/deploy.sh` copies
`intercomd`/`intercomctl` to a running Pi over `scp`/`ssh` and restarts the
systemd unit -- see that script's header comment for prerequisites
(`systemd/intercomd.service` and `udev/99-icom2000-gpio.rules` already
installed on the target).
