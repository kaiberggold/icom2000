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
ls Pi-Codec    # pick the Codec Zero "...SPK_playback" or "Playback_only" file
sudo alsactl restore -f Pi-Codec/<that file>.state
amixer -c Zero contents | grep -iA2 spk     # speaker switch on, volume sane
```

**Pass:** the speaker controls read as unmuted.

## 2. Speaker plays

```sh
speaker-test -D plughw:Zero -c 2 -t sine -f 440 -l 2   # tone
speaker-test -D plughw:Zero -c 2 -t wav -l 1           # spoken "front left/right"
```

**Pass:** a clean tone, with no crackle or clicks. The speaker output is
mono, so you hear both "left" and "right".

## 3. Our named devices work

Checks the card name in `config/asound.conf`:

```sh
sudo cp asound.conf /etc/asound.conf       # config/asound.conf from the repo
speaker-test -D icom_inside_playback -c 1 -t sine -f 440 -l 2
speaker-test -D icom_door_playback   -c 1 -t sine -f 440 -l 2
```

**Pass:** both play. "No such device" means the `hw:Zero` card name in
`asound.conf` needs fixing.

## 4. Microphone records

```sh
arecord -D icom_door_capture -f S16_LE -r 48000 -c 1 -d 5 -V mono /tmp/mic.wav   # speak; the VU meter moves
aplay -D icom_inside_playback /tmp/mic.wav
```

**Pass:** you hear yourself. This is the format our ALSA engine asks for:
48 kHz, mono, 16-bit.

## 5. Capture and playback at the same time, without our code

```sh
arecord -D icom_door_capture -f S16_LE -r 48000 -c 1 | aplay -D icom_inside_playback -f S16_LE -r 48000 -c 1
```

Keep the volume low: the onboard mic next to the speaker will feedback,
which itself proves it works. Headphones are gentler.

**Pass:** runs for a minute without `underrun!!!`/`overrun!!!` messages.
This is exactly what the engine's first step does, so it has to work here
before our code is involved.

## 6. Two captures at once should fail

Confirms the known limitation step 2 has to solve:

```sh
arecord -D icom_door_capture -f S16_LE -r 48000 -c 1 /dev/null &
arecord -D icom_inside_capture -f S16_LE -r 48000 -c 1 /dev/null    # expect "Device or resource busy"
kill %1
```

**Pass means it fails.** That's the proof step 2 needs `dsnoop` (shared
capture) and `dmix` (shared playback) in `asound.conf`.

## 7. Mixer settings survive a reboot

```sh
sudo alsactl store -f /etc/codec-zero-intercom.state
sudo cp alsa-restore-codec-zero.service /etc/systemd/system/   # systemd/ in the repo
sudo systemctl enable alsa-restore-codec-zero && sudo reboot
# after the reboot:
systemctl status alsa-restore-codec-zero
speaker-test -D icom_inside_playback -c 1 -t sine -l 1
```

**Pass:** the tone plays after the reboot with no manual mixer work.

## Record for step 2

- The card id.
- What the hardware really runs at underneath the `plug` conversion layer
  -- rate, format, channel count -- while test 3 is playing:
  `cat /proc/asound/card*/pcm0p/sub0/hw_params`.
- A dump of the working mixer: `amixer -c Zero contents > mixer.txt`.
- The Pi's ALSA library version, to match `ICOM_ALSA_LIB_GIT_TAG`
  (docs/CROSS_COMPILE.md "Building alsa-lib from source"):
  `dpkg -s libasound2 | grep Version`.

## Results

| Test | Date | Result | Notes |
|---|---|---|---|
| 0. Card detected | | | |
| 1. Speaker routing | | | |
| 2. Speaker plays | | | |
| 3. Named devices | | | |
| 4. Microphone | | | |
| 5. Duplex | | | |
| 6. Two captures fail | | | |
| 7. Survives reboot | | | |

## Not covered yet

These tests exercise the hardware and config, not our `AlsaEngine`: the
daemon can't run it yet. A small on-target tool running test 5 through
`makeAlsaEngine()` would close that gap.
