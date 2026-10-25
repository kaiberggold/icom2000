# Smoke tests (Pi Zero + Codec Zero + TCM1171)

Manual bring-up checks for the audio hardware and the daemon's audio, to
run on the Pi (see docs/ARCHITECTURE.md "Audio boundary"), and for the
telephone line (Step 4). Ordered so each test only relies on what the
previous ones proved. Tests 0-7 and S1-S5 use the standard ALSA tools
(`alsa-utils`), S6 our engine on its own, D1-D3 the daemon itself. L1-L7
check the TCM1171 and its GPIO lines, with the libgpiod tools and then
the daemon.

**Stop `intercomd` for tests 0-7 and S1-S6.** It runs the intercom from
startup, and the shared devices let it use the card next to the test
tools -- so it would play along: a test tone would also be picked up by
the other station's mic and played back, and S6 would make every station
hear the other twice.

**Safety:** the Codec Zero's speaker output is a bridged class-D amp. Use
a 4-8 Ω speaker, never connect either speaker terminal to ground (a scope
ground clip counts), and start at low volume.

## Setup on the Pi

For smoke tests, and for running `intercomd` from the VS Code debugger,
the Pi needs only the audio config -- not a full deployment. Skip
`systemd/intercomd.service`, `udev/99-icom2000-gpio.rules` and
`scripts/deploy.sh` (docs/CROSS_COMPILE.md "Deploying"); they're for
running `intercomd` as a service.

From the repo checkout on your dev machine:

```sh
scripts/sync-config.sh <user>@<pi-host>     # asound.conf, icom2000.conf -> /etc on the Pi
scp systemd/alsa-restore-codec-zero.service scripts/codec-zero-mixer.sh <user>@<pi-host>:
```

`sync-config.sh` (VS Code task `pi-sync-config`) installs the named
devices (`icom_*`) the tests use and `/etc/icom2000.conf`, keeping any
older version as `/etc/<file>.bak-<timestamp>`; rerun it after changing
either file in the repo. `alsa-restore-codec-zero.service` is only needed
for test 7, `codec-zero-mixer.sh` for S2.

On the Pi:

```sh
sudo apt install alsa-utils    # aplay/arecord/speaker-test/alsactl; usually preinstalled
groups                         # should include "audio" and "gpio"
```

`/etc/icom2000.conf` matters because the debugger starts `intercomd`
without `--config`, so it reads that path. To change it -- e.g. to switch
a station to the onboard mic (test 4) -- edit `config/icom2000.conf` in
the repo and sync again; an edit made on the Pi directly survives only
until the next sync, as a `.bak-` file.

The debugger runs `intercomd` as your SSH user, so that user needs the
`audio` and `gpio` groups. Raspberry Pi OS's default user has both.

## 0. Card detected

```sh
cat /proc/asound/cards          # the Codec Zero is listed; note its id
aplay -l; arecord -l
dmesg | grep -i da721           # DA7212 codec driver probed, no errors
```

If it's missing, check the overlay in `/boot/firmware/config.txt`
(`dtoverlay=rpi-codeczero`; confirm the exact name with
`ls /boot/firmware/overlays | grep -i codec`) and reboot.

**Pass:** the card is listed, and you know its id. `config/asound.conf`
assumes `Zero`.

## 1. Route playback to the speaker

The codec starts with its outputs muted and unrouted, so nothing plays
until the mixer is set. Raspberry Pi publishes ready-made Codec Zero mixer
settings:

```sh
git clone https://github.com/raspberrypi/Pi-Codec.git
ls Pi-Codec    # pick the Codec Zero file for onboard mic + speaker, named
               # like Codec_Zero_OnboardMIC_record_and_SPK_playback.state
sudo alsactl restore -f Pi-Codec/<that file>.state
amixer -c Zero contents | grep -iA2 spk     # speaker switch on, volume sane
```

The onboard-mic-plus-speaker file is the one to use while no external mic
is wired up: tests 4 and 5 record from the board's own microphone.

**Pass:** the speaker controls read as unmuted.

## 2. Speaker plays

```sh
speaker-test -D plughw:Zero -c 2 -t sine -f 440 -l 2   # tone
speaker-test -D plughw:Zero -c 2 -t wav -l 1           # spoken "front left/right"
```

**Pass:** a clean tone, with no crackle or clicks. The speaker output is
mono, so you hear both "left" and "right".

## 3. Our named devices work

Checks the card name in `/etc/asound.conf` (installed in "Setup on the
Pi"):

```sh
speaker-test -D icom_inside_playback -c 1 -t sine -f 440 -l 2
speaker-test -D icom_door_playback   -c 1 -t sine -f 440 -l 2
```

**Pass:** both play. "No such device" means the `hw:Zero` card name in
`asound.conf` needs fixing.

Door plays on the card's left channel and inside on its right (see "Step
2" below). With a single speaker, one of the two can be silent if the
mixer drives that speaker from one channel only -- expected until the
two-station mixer setup in S2.

## 4. Microphone records

With no external mic connected, record from the Codec Zero's onboard mic
(`icom_onboard_mic_capture` in `config/asound.conf`; needs the onboard-mic
mixer state from test 1):

```sh
arecord -D icom_onboard_mic_capture -f S16_LE -r 48000 -c 1 -d 5 -V mono /tmp/mic.wav   # speak; the VU meter moves
aplay -D icom_inside_playback /tmp/mic.wav
```

**Pass:** you hear yourself. This is the format our ALSA engine asks for:
48 kHz, mono, 16-bit. Once an external mic is wired up, repeat with
`icom_door_capture` and the matching mixer state.

To make a station use the onboard mic -- so `intercomd` logs it at
startup, and the audio engine will pick it up once it's wired in --
uncomment `capture_device = icom_onboard_mic_capture` in that station's
section of `/etc/icom2000.conf`.

## 5. Capture and playback at the same time, without our code

```sh
arecord -D icom_onboard_mic_capture -f S16_LE -r 48000 -c 1 | aplay -D icom_inside_playback -f S16_LE -r 48000 -c 1
```

Keep the volume low: the onboard mic next to the speaker will feedback,
which itself proves it works. Headphones are gentler.

**Pass:** runs for a minute without `underrun!!!`/`overrun!!!` messages.
This is exactly what the engine's first step does, so it has to work here
before our code is involved.

## 6. Two captures at once

With the current `config/asound.conf` the card's capture stream is shared
(`dsnoop`), so both stations can record at the same time:

```sh
arecord -D icom_door_capture -f S16_LE -r 48000 -c 1 /dev/null &
arecord -D icom_inside_capture -f S16_LE -r 48000 -c 1 /dev/null    # keeps running, no error
kill %1; kill %2
```

**Pass:** both run. "Device or resource busy" means `/etc/asound.conf` is
still the older version without the shared devices -- copy the repo's
`config/asound.conf` over again. (With that older version, this failure
was expected and is what step 2 fixed.)

## 7. Mixer settings survive a reboot

**Optional while smoke testing.** Raspberry Pi OS's own `alsa-utils`
usually already saves the mixer at shutdown and restores it at boot
(`systemctl status alsa-restore`). So after test 1, just reboot and rerun
test 3. If the tone still plays, nothing else is needed for now.

`alsa-restore-codec-zero.service` is the project's own version, for the
real installation: it restores one fixed, known-good state file at every
boot, instead of whatever the mixer happened to be at the last shutdown.
Nothing has to be written -- the unit file is `systemd/alsa-restore-codec-zero.service`
in the repo, copied to the Pi in "Setup on the Pi". Setting it up:

```sh
# 1. Save the working mixer (after tests 1-5 pass) where the unit reads it:
sudo alsactl store -f /etc/codec-zero-intercom.state
# 2. Install and enable the unit:
sudo cp alsa-restore-codec-zero.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable alsa-restore-codec-zero
sudo reboot
# 3. After the reboot:
systemctl status alsa-restore-codec-zero    # "active (exited)", no errors
speaker-test -D icom_inside_playback -c 1 -t sine -l 1
```

If `/etc/codec-zero-intercom.state` doesn't exist, the unit skips itself
("condition failed" in its status) rather than failing. After changing the
mixer later, rerun step 1 to update the file.

**Pass:** the tone plays after the reboot with no manual mixer work.

## Worth recording

- The card id.
- What the hardware really runs at underneath the conversion layers --
  rate, format, channel count. `/proc/asound/card*/pcm0p/sub0/hw_params`
  only shows it while something is playing (otherwise it says `closed`),
  so run it while test 3 plays in a second terminal.
- A dump of the working mixer: `amixer -c Zero contents > mixer.txt`.
- The Pi's ALSA library version, to match `ICOM_ALSA_LIB_GIT_TAG`
  (docs/CROSS_COMPILE.md "Building alsa-lib from source"):
  `dpkg -l 'libasound2*'` (the package is `libasound2t64` on trixie).

## Step 2: two stations

With a mic and a speaker per station. The layout (`config/asound.conf`):
the card's stereo channels are shared between the stations, **door =
left, inside = right**, and `intercomd` -- not the codec -- carries the
audio from one station to the other.

### S1. Card runs the shared format

`asound.conf`'s shared devices fix the card at 48 kHz, 16-bit, 2 channels:

```sh
aplay -D hw:Zero --dump-hw-params -f S16_LE -r 48000 -c 2 -d 1 /dev/zero
```

**Pass:** it plays a second of silence without an error, and the dump's
`FORMAT` line includes `S16_LE`. If the card needs another format or rate,
change `format`/`rate` in both shared devices in `asound.conf`.

### S2. Mixer: each connector on its own channel

Which connector feeds which channel is set in the codec's mixer.
`scripts/codec-zero-mixer.sh` sets it up for this wiring:

| Station | Mic | Speaker |
|---|---|---|
| door (left channel) | amplified mic on the AUX IN jack, left (`Aux Left`) | speaker terminals (`Lineout`) |
| inside (right channel) | onboard mic (`Mic 2`) | headphone jack, left side (`Headphone`) |

Two things about the Codec Zero decide the output side (from the kernel's
`da7213` codec driver and Raspberry Pi's `iqaudio-codec` board driver):

- the speaker terminals are driven only by the codec's **right** output
  mix, so door's left channel is crossed over to the right DAC inside the
  codec -- the `DAC ... Source MUX` lines;
- `DAC Mono Switch` must be off: on, it mixes both channels together,
  which is why a single speaker played both tones in test 3.

`scripts/codec-zero-mixer.sh` sets all of it (control names and starting
levels come from Raspberry Pi's Pi-Codec state files, test 1). On the Pi,
after copying it over (see "Setup on the Pi"):

```sh
./codec-zero-mixer.sh            # set the mixer (--card NAME if yours isn't "Zero")
./codec-zero-mixer.sh --store    # ... and save it for alsa-restore-codec-zero.service
```

It stops with an error naming the control if one can't be set.

The headphone jack's right side stays off because it carries the same mix
as the speaker terminals, i.e. door's audio. That also makes a mono
3.5 mm plug safe there: a mono plug shorts the jack's right output to
ground.

The door mic is amplified, i.e. line level, so it goes into the AUX input
at 0 dB rather than a mic input: those are built for a bare microphone --
they supply a bias voltage, and Raspberry Pi's settings run them at
+24 dB of preamp gain, which would clip a line-level signal. An
unamplified mic would go on the MIC jack (`Mic 1`) instead.

Wired differently? Edit the script: change which input (`Aux Left`,
`Mic 1`, `Mic 2`) goes into `Mixin Left`/`Mixin Right`, or -- to put door
on the headphone jack instead --
set both `DAC ... Source MUX` to their own side (`'DAI Input Left'` for
left, `'DAI Input Right'` for right) and switch the headphone right side
on instead of the left.

Levels (`Mic`, `Mixin PGA`, `Lineout`, `Headphone` volumes) are starting
values: adjust with `alsamixer -c Zero` while talking. Then save the
result as in test 7, so it survives a reboot:

```sh
sudo alsactl store -f /etc/codec-zero-intercom.state
```

### S3. Each speaker on its own

```sh
speaker-test -D icom_door_playback   -c 1 -t sine -f 440 -l 1
speaker-test -D icom_inside_playback -c 1 -t sine -f 880 -l 1
```

**Pass:** the low tone comes only from the door speaker, the high tone
only from the inside speaker. Both at the door or both inside means the
mixer feeds both speakers from the same channel.

### S4. Each mic on its own

```sh
arecord -D icom_door_capture   -f S16_LE -r 48000 -c 1 -V mono /dev/null   # talk into each mic in turn
arecord -D icom_inside_capture -f S16_LE -r 48000 -c 1 -V mono /dev/null
```

**Pass:** each VU meter moves for its own mic. A little movement from the
other mic in the same room is acoustic crosstalk, not a routing mistake.
If the door meter sits at the top while you talk, the amplified mic is
clipping: lower `Aux` in `alsamixer -c Zero` (or the mic amplifier's own
gain); if it barely moves, raise `Mixin PGA` left.

### S5. Both speakers at once

```sh
speaker-test -D icom_door_playback -c 1 -t sine -f 440 -l 3 &
speaker-test -D icom_inside_playback -c 1 -t sine -f 880 -l 3
```

**Pass:** both tones play at the same time, each on its own speaker (the
shared `dmix` device at work).

### S6. Our engine: `icom-audiotest`

Runs the ALSA intercom engine -- the same code `intercomd` runs --
between the first two stations in `/etc/icom2000.conf`, until Ctrl-C,
without the rest of the daemon. Copy it from the Pi build first:

```sh
scripts/sync-app.sh <user>@<pi-host>    # on the dev machine (VS Code task: pi-sync-app)
./icom-audiotest                        # on the Pi; --help for options
```

Talk into each mic. Keep the stations in different rooms, or the volume
low: a speaker next to the other station's mic will feed back.

**Pass:**

- each station hears the other, and never itself;
- no `recovered from` warnings during a minute of talking -- those are
  dropouts;
- Ctrl-C prints `stopped`.

It starts both directions or neither: if one station's device can't be
opened, it prints why and exits.

The audio threads should run at real-time priority (`[audio]
rt_priority` in `icom2000.conf`), which a login user usually isn't
allowed: then it logs `runs at normal priority, not SCHED_FIFO` and works
anyway, just with less protection against dropouts. To allow it, add a
file `/etc/security/limits.d/icom2000.conf` containing
`@audio - rtprio 20` and log in again; `chrt -a -p $(pidof icom-audiotest)`
then shows `SCHED_FIFO` priority 20 for two of its threads. (The
`intercomd` service has this permission built in, via `LimitRTPRIO`.)

## Step 3: `intercomd`

The daemon itself, running the same intercom as S6 from startup. After S6
passes, with `icom-audiotest` stopped.

### D1. Audio comes up

Start `intercomd` from the VS Code debugger ("Debug intercomd on Pi
Zero"; it reads `/etc/icom2000.conf`), or after `scripts/sync-app.sh` on
the Pi directly:

```sh
./intercomd-debug --socket /tmp/icom.sock --log-console
./intercomctl -s /tmp/icom.sock status          # in a second terminal
```

**Pass:** `STATUS` ends in `audio=up`, and the log shows two `routing
icom_..._capture -> icom_..._playback` lines. `audio=down` means the
engine couldn't start: the `audio.alsa` error just before says why (e.g.
an old `/etc/asound.conf`, or `icom-audiotest` still holding the card).
The bell (`./intercomctl -s /tmp/icom.sock bell ring 300`) works either
way.

### D2. Talk through it

As in S6: each station hears the other, never itself, with no `recovered
from` warnings in a minute of talking.

### D3. Clean shutdown

Ctrl-C in the debug session's terminal (or `kill <pid>`).

**Pass:** the log ends with `received signal`, then `stopped` -- the audio
threads stopped and the daemon exited on its own. A terminal that just
says `Terminated`, without `stopped`, means the signal killed the process
instead.

## Step 4: TCM1171 line interface

The telephone side: the TCM1171 SLIC and `Tcm1171Controller`
(docs/ARCHITECTURE.md "Hardware layer", "Pin assignments"). Independent
of the audio tests -- it needs only the SLIC wired to the Pi and the
telephone on its line.

What the controller does today, so you know what to look for:

- Three lines from `[gpio.tcm1171]` in `/etc/icom2000.conf` -- placeholders
  until this step confirms them: `ring_mode_line` (RM, output, default 5),
  `polarity_line` (F/R, output, default 22), `hook_detect_line` (input,
  default 6, edges in both directions). The commands below use the
  defaults; substitute yours. A fourth, `mute_line` (output; `mute_active`
  sets whether High or Low mutes), is optional and unset by default -- see
  L7.
- At startup both outputs go low and the state is `on-hook`, whatever the
  hook line says. A rising edge means `off-hook`, a falling edge `on-hook`
  -- a guess at the polarity, which L2 checks. There's no debouncing.
- Ringing only sets RM high, nothing else; F/R stays low. And nothing
  calls it yet: `intercomctl` can read the line state (`line status`, and
  `line=` in `status`) and set the optional mute, but not ring. So L3-L5
  drive the SLIC directly with the libgpiod tools, and only L6 and L7 are
  the daemon itself.

**Stop `intercomd` for L1-L5.** It holds its three lines while it runs,
so the tools fail with "Device or resource busy". The other way round, an
`intercomd` started while a `gpioset` still holds a line fails to start.

**Safety:**

- The ringing voltage on the telephone line is tens of volts AC, enough
  for a shock. Don't touch the line wires or the SLIC's tip/ring pins
  while anything drives RM high.
- The Pi's GPIOs take 3.3 V at most and aren't 5 V tolerant. If the SLIC
  runs from 5 V, measure its hook output (L2) before connecting it to the
  Pi; a 5 V output needs a voltage divider or level shifter. Check in its
  datasheet that 3.3 V from the Pi counts as high on RM and F/R.

On the Pi:

```sh
sudo apt install gpiod    # gpioinfo, gpioget, gpioset, gpiomon (libgpiod v2 syntax below)
```

### L1. Lines free and as expected

```sh
gpioinfo -c gpiochip0 5 6 22
pinctrl get 5,6,22        # Raspberry Pi OS: function, level and pull of each pin
```

**Pass:** no line is `used` by anything else (a consumer name means
another driver or overlay owns it -- pick other lines in
`[gpio.tcm1171]`). Note the pulls: on the Pi, GPIO0-8 default to a
pull-up, so with nothing driving line 6 it reads high -- which the
controller would take as off-hook.

### L2. Hook signal

First, with a multimeter between the SLIC's hook output and ground: note
the voltage on hook and off hook. **Pass for wiring it to the Pi:** both
at most 3.3 V, and clearly different. Then, on the Pi:

```sh
gpiomon -c gpiochip0 -e both 6
```

Lift the handset and hang up a few times, slowly. Then lift it and dial a
digit on the rotary dial (e.g. a 3).

**Pass:** each lift and each hang-up shows up, the same way every time.
Record:

- Which level means off-hook. The controller assumes high; if it's low,
  that's the polarity fix in `Tcm1171Controller::onHookEdge()`.
- How many edges one lift or hang-up gives -- more than one is contact
  bounce, and with no debouncing the daemon would see several state
  changes.
- The dial: a dialed 3 should show 3 short pulses (roughly 60 ms
  interrupted, 40 ms closed each, at 10 pulses per second). Note the
  timestamps; pulse-dial decoding will need them.
- If the line doesn't change at all, the output may be open-drain: try
  `gpiomon -c gpiochip0 -b pull-up -e both 6`. If that works, the backend
  will need to request the pull-up too (it requests no bias today).

### L3. Line polarity (F/R)

With the handset on hook, a multimeter on DC volts across the telephone's
tip and ring wires:

```sh
gpioset -c gpiochip0 22=0     # note the voltage, then Ctrl-C
gpioset -c gpiochip0 22=1     # note the voltage, then Ctrl-C
```

`gpioset` holds the value only while it runs.

**Pass:** the same voltage (record it) with the sign flipped between the
two.

### L4. Ringing

With the handset on hook. First RM on its own, as the controller does it
(for a few seconds, then Ctrl-C):

```sh
gpioset -c gpiochip0 5=1
```

Then RM high with F/R toggling at the ring frequency, in two terminals:

```sh
gpioset -c gpiochip0 5=1                # terminal 1: RM high
gpioset -c gpiochip0 -t 20ms 22=1       # terminal 2: F/R toggling every 20 ms = 25 Hz
```

25 Hz is the usual ring frequency in Germany; `-t 25ms` gives 20 Hz (the
US one). Stop with Ctrl-C in terminal 2 first, then terminal 1.

**Pass:** the telephone rings in one of the two cases. Record which one.
On SLICs of the same kind (e.g. Silvertel's Ag1171) the ringing voltage
comes from toggling F/R at the ring frequency while RM is high; if that's
the variant that rings here, setting RM alone -- all
`Tcm1171Controller::startRinging()` does today -- can't ring the bell,
and the controller needs that toggle. Ringing as real telephones do it
(e.g. 1 s on, 4 s off) is cadence on top of that, also still to do.

### L5. Ring trip

Ringing as in L4, the variant that rang, plus a third terminal:

```sh
gpiomon -c gpiochip0 -e both 6
```

Let it ring with the handset on hook for a few seconds, then lift the
handset -- don't put it to your ear -- and stop ringing at once (Ctrl-C
in terminals 2 and 1).

**Pass:**

- While it rings on hook, the hook line stays quiet. Edges there mean the
  ringing current triggers the hook detection: the controller would read
  that as off-hook and stop ringing at once.
- Lifting the handset during ringing shows up as off-hook. That's what the
  controller uses to stop ringing when someone answers.

### L6. `intercomd` sees the hook

With the handset on hook and all `gpioset`/`gpiomon` stopped, start
`intercomd` as in D1, then:

```sh
./intercomctl -s /tmp/icom.sock line status
```

**Pass:**

- The log shows `requested output gpiochip0:5 (icom2000-tcm1171-rm)`,
  `requested output gpiochip0:22 (icom2000-tcm1171-fr)` and `requested
  input gpiochip0:6 (icom2000-tcm1171-hook)`, and `gpioinfo -c gpiochip0
  5 6 22` lists those consumers.
- `line status` says `on-hook`.
- Lifting the handset logs `on-hook -> off-hook` (component `hw.tcm1171`),
  and `line status` says `off-hook`; hanging up logs `off-hook ->
  on-hook`.

Record if it's the wrong way round (the polarity from L2), or if one lift
logs several changes (bounce from L2). Also try starting `intercomd` with
the handset already lifted: `line status` then says `on-hook` until the
next edge, because the controller doesn't read the line's level at
startup -- a known gap, worth knowing about when reading the results.

### L7. Line mute (optional)

Only if a mute line is wired -- a GPIO that mutes the telephone line's
audio. Without one, skip this: `status` reports
`mute=unconfigured` and `line mute on` answers `ERR line mute not
configured`.

Set `mute_line` in `[gpio.tcm1171]` in `config/icom2000.conf` (it ships
commented out, as `#mute_line = 26`), and `mute_active = low` if the mute
input is active low (the default is `high`); sync it to the Pi
(`scripts/sync-config.sh`) and restart `intercomd`. Then, talking through
the handset:

```sh
./intercomctl -s /tmp/icom.sock status            # ... mute=off ...
./intercomctl -s /tmp/icom.sock line mute on      # OK on
./intercomctl -s /tmp/icom.sock line mute off     # OK off
```

**Pass:** the log shows `line mute: gpio line 26, active high` (or `low`)
at startup and `line
muted`/`line unmuted` (component `hw.tcm1171`); `gpioinfo -c gpiochip0
26` lists `icom2000-tcm1171-mute`; and the line's audio goes quiet
between `on` and `off`. If it's quiet the other way round, flip
`mute_active`. Lifting and hanging up doesn't change the mute.

## Troubleshooting: leftover shared memory

The shared devices (`dsnoop`/`dmix`) keep their state in System V shared
memory and semaphores. A process that crashed, or an older `asound.conf`
whose keys clashed (see the comment above the shared devices there), can
leave segments behind, and opening a station device then fails -- e.g.
"unable to create IPC shm instance" or "Invalid argument". With nothing
using the card (stop `icom-audiotest`, `intercomd`, `arecord`/`aplay`):

```sh
ipcs -m                 # leftovers show "nattch" 0
ipcrm -m <shmid>        # each leftover, by its shmid (second column)
ipcs -s                 # the same for semaphores, if it still fails
ipcrm -s <semid>
```

Use `sudo ipcrm` for segments another user created, e.g. the `icom2000`
service user. Only remove entries with `nattch` 0 -- anything attached
still belongs to a running program.

## Results

| Test | Date | Result | Notes |
|---|---|---|---|
| 0. Card detected | | | |
| 1. Speaker routing | | | |
| 2. Speaker plays | | | |
| 3. Named devices | | | |
| 4. Microphone | | | |
| 5. Duplex | | | |
| 6. Two captures at once | | | |
| 7. Survives reboot | | | |
| S1. Shared format | | | |
| S2. Mixer channels | | | |
| S3. Each speaker | | | |
| S4. Each mic | | | |
| S5. Both speakers | | | |
| S6. icom-audiotest | | | |
| D1. intercomd audio up | | | |
| D2. Talk through intercomd | | | |
| D3. Clean shutdown | | | |
| L1. GPIO lines | | | |
| L2. Hook signal | | | |
| L3. Line polarity | | | |
| L4. Ringing | | | |
| L5. Ring trip | | | |
| L6. intercomd sees the hook | | | |
| L7. Line mute (optional) | | | |

## Not covered yet

The audio runs always-on: there's no call state yet that mutes it while
nobody's talking, and no ring or dial tones (docs/ARCHITECTURE.md "Audio
boundary").

On the telephone side: `intercomd` can't ring the telephone yet -- there's
no command for it, and the controller's ringing may need more than RM
(L4) -- and doesn't decode pulse dialing. The handset's audio (how the
telephone's speech reaches the codec) isn't part of these tests either.
