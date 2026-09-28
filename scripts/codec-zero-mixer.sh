#!/usr/bin/env bash
# Sets the Codec Zero's mixer for the two-station intercom (see
# docs/SMOKE_TESTS.md "S2" for the wiring this assumes and why):
#
#   door   (left channel):  AUX IN jack, left (Aux Left)  -> speaker terminals (Lineout)
#   inside (right channel): onboard mic (Mic 2)           -> headphone jack, left side
#
# The door input is line level -- an amplified mic -- so it goes into the
# AUX input, not a mic input.
#
# with the codec's own analog mic-to-output paths off -- intercomd carries
# the audio. Run it on the Pi, adjust levels in `alsamixer -c Zero` if
# needed, then keep the result with --store (or store it yourself), so
# systemd/alsa-restore-codec-zero.service restores it at every boot.
# intercomd itself never touches the mixer.
set -euo pipefail

usage() {
    echo "usage: $(basename "$0") [--card NAME] [--store]" >&2
    echo "  --card NAME  ALSA card name (default: Zero, see aplay -l)" >&2
    echo "  --store      afterwards save the mixer to ${STATE_FILE} (uses sudo)" >&2
}

STATE_FILE=/etc/codec-zero-intercom.state
CARD=Zero
STORE=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        --card) CARD="${2:?--card needs a name}"; shift 2 ;;
        --store) STORE=true; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage; exit 2 ;;
    esac
done

set_control() {
    if ! amixer -q -c "${CARD}" cset name="$1" "$2"; then
        echo "error: could not set '$1' to '$2' on card ${CARD}" >&2
        exit 1
    fi
}

# Inputs: door (AUX IN left) -> left channel, inside mic (onboard) -> right.
set_control 'AUX Jack Switch' on
set_control 'Onboard MIC Switch' on
set_control 'MIC Jack Switch' off
set_control 'Aux Switch' on,off
# 53 = 0 dB (the AUX gain runs -52.5 dB .. +15 dB in 1.5 dB steps).
set_control 'Aux Volume' 53,53
set_control 'Mic 1 Switch' off
set_control 'Mic 2 Switch' on
set_control 'Mic 2 Volume' 5
set_control 'Mic 2 Amp Source MUX' Differential
set_control 'Mixin Left Aux Left Switch' on
set_control 'Mixin Left Mic 1 Switch' off
set_control 'Mixin Left Mic 2 Switch' off
set_control 'Mixin Left Mixin Right Switch' off
set_control 'Mixin Right Mic 2 Switch' on
set_control 'Mixin Right Mic 1 Switch' off
set_control 'Mixin Right Aux Right Switch' off
set_control 'Mixin Right Mixin Left Switch' off
set_control 'Mixin PGA Switch' on,on
# Left 3 = 0 dB for the line-level door input, right 7 = +6 dB for the
# onboard mic (-4.5 dB .. +18 dB in 1.5 dB steps).
set_control 'Mixin PGA Volume' 3,7
set_control 'ADC Switch' on,on
set_control 'ADC Volume' 114,114
set_control 'DAI Left Source MUX' 'ADC Left'
set_control 'DAI Right Source MUX' 'ADC Right'

# Outputs. The speaker terminals are driven only by the codec's right
# output mix, so the channels cross over inside the codec:
#   door (left channel)    -> DAC right -> speaker terminals
#   inside (right channel) -> DAC left  -> headphone jack, left side
# DAC Mono off: on, it mixes both channels together.
set_control 'DAC Mono Switch' off,off
set_control 'DAC Left Source MUX' 'DAI Input Right'
set_control 'DAC Right Source MUX' 'DAI Input Left'
set_control 'DAC Soft Mute Switch' off
set_control 'DAC Volume' 112,112
set_control 'Mixout Left DAC Left Switch' on
set_control 'Mixout Right DAC Right Switch' on

# The codec's own analog mic/aux-to-output paths off -- intercomd carries
# the audio, so these would play it twice.
set_control 'Mixout Left Aux Left Switch' off
set_control 'Mixout Left Mixin Left Switch' off
set_control 'Mixout Left Mixin Right Switch' off
set_control 'Mixout Right Aux Right Switch' off
set_control 'Mixout Right Mixin Right Switch' off
set_control 'Mixout Right Mixin Left Switch' off

set_control 'Lineout Switch' on
set_control 'Lineout Volume' 48
set_control 'HP Jack Switch' on
# Headphone right off: it carries the same mix as the speaker terminals
# (door's audio), and a mono plug would short it to ground.
set_control 'Headphone Switch' on,off
set_control 'Headphone Volume' 49,49

echo "mixer set on card ${CARD}"
if [[ "${STORE}" == true ]]; then
    sudo alsactl store -f "${STATE_FILE}"
    echo "saved to ${STATE_FILE}"
else
    echo "to keep it across reboots: sudo alsactl store -f ${STATE_FILE}"
fi
