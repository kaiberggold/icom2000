# Audio smoke tests (Pi Zero + Codec Zero)

Manual bring-up checks for the audio hardware, to run on the Pi before
building on the ALSA engine (see docs/ARCHITECTURE.md "Audio boundary").
Ordered so each test only relies on what the previous ones proved. They
use the standard ALSA tools (`alsa-utils`), not our code: `intercomd`
still runs the no-op engine and never opens the sound card, so it can
stay running.

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
| door (left channel) | MIC jack (`Mic 1`) | speaker terminals (`Lineout`) |
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

Wired differently? Edit the script: swap which `Mic` goes into `Mixin
Left`/`Mixin Right`, or -- to put door on the headphone jack instead --
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

### S5. Both speakers at once

```sh
speaker-test -D icom_door_playback -c 1 -t sine -f 440 -l 3 &
speaker-test -D icom_inside_playback -c 1 -t sine -f 880 -l 3
```

**Pass:** both tones play at the same time, each on its own speaker (the
shared `dmix` device at work).

### S6. Our engine: `icom-audiotest`

Runs the ALSA intercom engine -- the same code `intercomd` will run in
step 3 -- between the first two stations in `/etc/icom2000.conf`, until
Ctrl-C. Copy it from the Pi build first:

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

## Not covered yet

`intercomd` itself still runs the no-op audio engine, so S6's
`icom-audiotest` is where our engine meets the hardware until step 3 puts
it into the daemon.
