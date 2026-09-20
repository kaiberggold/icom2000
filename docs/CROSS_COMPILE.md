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

**This is proven, not theoretical** -- `cmake/toolchain-arm-linux-gnueabihf.cmake`
now checks it automatically at configure time (see "The automated ARMv6
probe" below) and will refuse to proceed on a toolchain that fails it, but
here's the manual repro in case you want to see it yourself. On Ubuntu
24.04's `gcc-arm-linux-gnueabihf` (13.3.0):

```sh
$ echo 'int main(void){return 0;}' > t.c
$ arm-linux-gnueabihf-gcc -marm -march=armv6zk -mfpu=vfp -mfloat-abi=hard -o t t.c
$ arm-linux-gnueabihf-readelf -A t | grep Tag_CPU_arch
  Tag_CPU_arch: v7
```

`v7`, despite `-march=armv6zk` on the command line. Digging further: the
*compiler* honors the flag correctly for your own code (a `.o` compiled
this way is genuinely tagged `v6KZ`) -- the problem is everything else
that gets linked into a final executable regardless of your flags:

```sh
$ arm-linux-gnueabihf-readelf -A "$(arm-linux-gnueabihf-gcc -print-file-name=crt1.o)" | grep Tag_CPU_arch
  Tag_CPU_arch: v7
```

`crt1.o` -- the C runtime startup object linked into *every* executable,
unconditionally, by the toolchain itself -- is hard-tagged `v7`. So is
every member of `libgcc.a` (division helpers, etc., pulled in whenever
your code needs them, which ARMv6 code often does since it lacks hardware
integer divide). No `-march`/`-mcpu` flag changes this: these are
prebuilt `.o`/`.a` files Debian/Ubuntu ship as-is, built once for their
armhf baseline. A final binary linked against them is architecturally
inconsistent -- individually-valid ARMv6 and ARMv7 machine code in the
same executable -- and can `SIGILL` on a real ARM1176JZF-S at whatever
point execution reaches one of the ARMv7-only instructions.

**libgpiod is a separate, related problem**, not fixed by any of the
above: nobody ships a prebuilt libgpiod for genuine ARMv6 either (Debian/
Ubuntu only build it for their ARMv7 armhf baseline, same as the
toolchain itself). See "Building libgpiod from source" below --
`ICOM_LIBGPIOD_BUILD_FROM_SOURCE` (on by default for `pi0-release`/
`pi0-debug`) cross-compiles it from source with whatever toolchain you
give CMake, sidestepping the "where do I even get this .deb" question
entirely. It does not, by itself, fix the ARMv6-vs-ARMv7 toolchain
problem above -- that's a property of the toolchain, not of any one
library -- but it does mean you no longer need a full sysroot just to get
libgpiod specifically.

Two ways to get a genuinely ARMv6-capable toolchain, in order of
recommendation:

## The automated ARMv6 probe

Every `cmake --preset pi0-release`/`pi0-debug` configure runs the exact
repro above automatically (compile a trivial program with the real
target flags, check its `Tag_CPU_arch`) and stops with `FATAL_ERROR`
naming the problem if it fails -- so a toolchain that can't actually
target ARMv6 is caught immediately, not after a full build and a
confusing SIGILL on real hardware. If you're certain you know what you're
doing (e.g. testing this CMake configuration itself, not producing a
binary you intend to run), `-DICOM_SKIP_ARMV6_CHECK=ON` bypasses it --
anything built that way should not be trusted on a real Pi Zero 1.x/Pi 1
without independently re-verifying it.

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

## Option B: build a real ARMv6 toolchain with crosstool-NG

`crosstool-NG` builds a complete, self-contained cross-compiler
(binutils + gcc + glibc, from source) for whatever target you configure
it for. Done right, its `crt1.o`/`libgcc.a` are genuinely built for
ARMv6, unlike Debian/Ubuntu's package -- `cmake/toolchain-arm-linux-gnueabihf.cmake`
and the `pi0-release`/`pi0-debug` presets are already set up for one, and
the automated probe above will confirm it actually worked.

There's a ready-made sample for exactly this board, confirmed by reading
crosstool-NG's own sample config: `armv6-unknown-linux-gnueabihf`, whose
`reported.by` file literally says *"Toolchain for the Raspberry Pi, with
hard-float"*, and whose settings are exactly right --
`CT_ARCH_CPU="arm1176jzf-s"`, `CT_ARCH_FPU="vfp"`, `CT_ARCH_FLOAT_HW=y`
(hard-float). Two things about it are worth knowing before you build,
both confirmed by inspection, not guessed:

- **Its default glibc version (2.44 as of the crosstool-NG revision
  checked) is almost certainly newer than whatever's on your actual Pi**,
  and a binary linked against a newer glibc than the target has will fail
  to run there (`version 'GLIBC_2.xx' not found`) -- glibc compatibility
  only goes one direction (older-built binaries run fine on newer
  systems, not the reverse). Check what your Pi actually has
  (`ssh pi@your-pi ldd --version`, first line) and pin crosstool-NG's
  glibc version to that or older, in step 3 below. If you don't know or
  don't want to bother checking, 2.31 (Debian Bullseye's baseline, and
  the value in the exact commands below) is a safe, conservative choice
  that will run on any Raspberry Pi OS release from Bullseye onward,
  including current Bookworm.
- **It sets `CT_TARGET_VENDOR="rpi"`**, so the built compiler is
  `arm-rpi-linux-gnueabihf-gcc`, not `arm-linux-gnueabihf-gcc`. This
  project's toolchain file already looks for both names automatically
  (see its `ICOM_TOOLCHAIN_PREFIX` handling) -- nothing extra to do here,
  just don't be surprised by the binary names it produces.

Concrete steps:

```sh
# 1. Host build dependencies (Ubuntu/Debian; crosstool-NG needs quite a
#    few -- this is the full list, not a partial one you'll have to top
#    up piecemeal):
sudo apt install build-essential gperf bison flex texinfo help2man \
    libtool-bin automake autoconf gawk libncurses-dev unzip rsync \
    bzip2 xz-utils patch git wget curl

# 2. Build crosstool-NG itself (this builds the *builder tool*, not the
#    cross-compiler yet -- a couple of minutes):
git clone https://github.com/crosstool-ng/crosstool-ng.git
cd crosstool-ng
./bootstrap && ./configure --enable-local && make -j"$(nproc)"

# 3. Load the Pi-specific sample, then pin the glibc version (see above
#    for why) before building:
mkdir ~/armv6-toolchain-build && cd ~/armv6-toolchain-build
~/crosstool-ng/ct-ng armv6-unknown-linux-gnueabihf
~/crosstool-ng/ct-ng menuconfig
#   -> "C-library" -> "Version of glibc" -> pick 2.31 (or your Pi's
#      version from `ldd --version`) -- everything else in this sample
#      is already correct, this is the one thing worth changing.
#   -> optionally "Paths and misc options" -> "Number of parallel jobs"
#      to match your core count.

# 4. Build. This compiles gcc twice (a bootstrap pass, then the real
#    one) plus binutils and glibc from source -- expect anywhere from
#    ~30 minutes to a couple of hours depending on your machine, and a
#    few GB of downloads (source tarballs for gcc/glibc/binutils/Linux
#    headers, mostly from gnu.org and kernel.org -- make sure nothing on
#    your network blocks those before you start):
~/crosstool-ng/ct-ng build

# 5. The finished toolchain lands in ~/x-tools/arm-rpi-linux-gnueabihf/bin
#    by default -- put it on PATH:
export PATH="$HOME/x-tools/arm-rpi-linux-gnueabihf/bin:$PATH"

# 6. Build icom2000 with it:
cd /path/to/icom2000
cmake --preset pi0-release
cmake --build --preset pi0-release
```

**Do not run crosstool-NG as root** -- it refuses by design (a real
safety feature, not a bug to work around), and legitimately shouldn't
need to be root for anything it does.

No `ICOM_PI_SYSROOT` needed for this alone -- libgpiod cross-builds from
source automatically (see below), and everything else intercomd links
against is either header-only or part of the toolchain's own bundled C/
C++ runtime. You'd still want a sysroot (rsynced from a real Pi, feeding
`ICOM_PI_SYSROOT`) for a library that genuinely has to match the target
Raspberry Pi OS build exactly and isn't practical to cross-build yourself
-- ALSA (`libasound`), when the real `AudioEngine` implementation arrives,
is the likely future example.

If getting a real ARMv6 toolchain turns out to be more yak-shaving than
it's worth, fall back to Option A -- it needs no custom toolchain at all.

## Building libgpiod from source

`ICOM_LIBGPIOD_BUILD_FROM_SOURCE` (default `ON` whenever
`ICOM_WITH_LIBGPIOD=ON`, i.e. on both `pi0-release` and `pi0-debug`)
cross-compiles libgpiod itself as part of the build
(`src/gpio/cmake/BuildLibgpiodFromSource.cmake`), via `ExternalProject_Add`
driving `meson`+`ninja`. This exists because there is nowhere to get a
prebuilt ARMv6 libgpiod from -- see "Read this first" above -- with either
Option A or B.

Requires `meson` and `ninja` on the **host** (`apt install meson
ninja-build`) -- they orchestrate the cross-build, they don't run on the
target. Uses whatever `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER` the
toolchain file resolved (re-expressed as a meson "cross file", since
meson has its own, different format for this), builds libgpiod's C
library and C++ bindings as static libraries (`libgpiod.a`/
`libgpiodcxx.a` -- two separate libraries; easy to miss since
`<gpiod.hpp>` lives in the same source tree as the C API, but the C++
bindings are a genuinely separate pkg-config module, `libgpiodcxx.pc`),
and installs them under
`<build-dir>/third_party/libgpiod/install`. Static, so a
`scripts/deploy.sh`-style "just copy the binary over" deployment keeps
working with no `.so` to also install and version-match on the target.

Pinned to a specific release (`ICOM_LIBGPIOD_GIT_TAG`, default `v2.3.1`)
fetched from `ICOM_LIBGPIOD_GIT_URL` (default a GitHub mirror of the
canonical `git.kernel.org` repo) -- both overridable cache variables if
you need a different version.

If you'd rather use a libgpiod that's already built and installed on a
sysroot (e.g. because it's already there from a full rsynced Raspberry Pi
OS filesystem, and rebuilding it yourself is pure waste), set
`-DICOM_LIBGPIOD_BUILD_FROM_SOURCE=OFF`: this falls back to the previous
`pkg_check_modules` lookup against `ICOM_PI_SYSROOT`, requiring both
`libgpiod.pc` and `libgpiodcxx.pc` to be present there.

## Sanity-checking a binary before deploying it

The automated probe (above) already checks the toolchain itself at
configure time, on every `pi0-release`/`pi0-debug` configure -- so this
manual check is now mostly a "trust but verify" on the actual
`intercomd` binary you're about to copy to hardware, worth doing once
after a toolchain change or before a first deploy:

```sh
file build/pi0-release/src/app/intercomd
# expect: ELF 32-bit LSB executable, ARM, EABI5 version 1 (SYSV) ...

arm-linux-gnueabihf-readelf -A build/pi0-release/src/app/intercomd | grep -i "^  Tag_CPU_arch"
# expect: Tag_CPU_arch: v6 (or a note naming ARM1176/ARMv6, not v7)
```

If that second line says `v7`, something's inconsistent with what the
configure-time probe checked (a stale build directory from before a
toolchain change is the most likely cause -- reconfigure from scratch) --
don't copy it over.

## Deploying

Once a `pi0-release` build exists, `scripts/deploy.sh` copies
`intercomd`/`intercomctl` to a running Pi over `scp`/`ssh` and restarts the
systemd unit -- see that script's header comment for prerequisites
(`systemd/intercomd.service` and `udev/99-icom2000-gpio.rules` already
installed on the target).
