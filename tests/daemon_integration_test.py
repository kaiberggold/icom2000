#!/usr/bin/env python3
# Runs the real intercomd against stand-in audio devices and checks, from
# the outside, what the unit tests can't:
#
#  - with working devices: STATUS says audio=up, each station's speaker
#    gets exactly the other station's mic, and SIGTERM shuts it down
#    through intercomd's own handler (exit 0, "stopped") -- which also
#    catches the audio threads being created before SignalWatcher blocks
#    the signals, since one of them would then take the SIGTERM and kill
#    the process (exit -15);
#  - without them: STATUS says audio=down, the bell still works, and it
#    shuts down cleanly;
#  - the line mute: unconfigured with the shipped config (LINE MUTE ON
#    fails), working once [gpio.tcm1171] mute_line is set, and an invalid
#    mute_active stopping intercomd at startup.
#
# The stand-in devices are alsa-lib `file` PCMs in a scratch HOME's
# .asoundrc, capture fed from FIFOs -- see tests/alsa_engine_tests.cpp for
# why FIFOs. Usage: daemon_integration_test.py INTERCOMD INTERCOMCTL CONFIG
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time

INTERCOMD, INTERCOMCTL, CONFIG = sys.argv[1:4]
PATTERNS = {
    "door": [1 + i % 997 for i in range(4800)],
    "inside": [3000 + i % 991 for i in range(4800)],
}
failures = []


def check(condition, what):
    print(("ok:   " if condition else "FAIL: ") + what)
    if not condition:
        failures.append(what)


def played_exactly(path, pattern):
    if not os.path.exists(path):
        return False
    data = open(path, "rb").read()
    samples = struct.unpack(f"<{len(data) // 2}h", data)
    first = 0
    while first < len(samples) and samples[first] == 0:
        first += 1
    return list(samples[first:first + len(pattern)]) == pattern


def run(scratch, with_devices, with_mute):
    label = ("with devices" if with_devices else "without devices") + (", mute line" if with_mute else "")
    config = CONFIG
    if with_mute:
        # The shipped config's own commented-out example, switched on.
        text = open(CONFIG).read()
        check("\n#mute_line = 26\n" in text, f"{label}: the shipped config leaves mute_line unset")
        config = os.path.join(scratch, "icom2000.conf")
        with open(config, "w") as f:
            f.write(text.replace("\n#mute_line = 26\n", "\nmute_line = 26\n"))
    home = os.path.join(scratch, "home")
    os.makedirs(home, exist_ok=True)
    asoundrc = ""
    fifos = []
    if with_devices:
        for station, pattern in PATTERNS.items():
            fifo = os.path.join(scratch, f"{station}.fifo")
            speaker = os.path.join(scratch, f"{station}_speaker.raw")
            asoundrc += (f'pcm.icom_{station}_capture {{ type file; slave.pcm null; file "/dev/null"; '
                         f'infile "{fifo}"; format raw }}\n'
                         f'pcm.icom_{station}_playback {{ type file; slave.pcm null; file "{speaker}"; '
                         f'format raw }}\n')
            os.mkfifo(fifo)
            # O_RDWR: a write-only open would block until intercomd opens it.
            fd = os.open(fifo, os.O_RDWR)
            os.write(fd, struct.pack(f"<{len(pattern)}h", *pattern))
            fifos.append(fd)
    with open(os.path.join(home, ".asoundrc"), "w") as f:
        f.write(asoundrc)

    sock = os.path.join(scratch, "icom.sock")
    env = dict(os.environ, HOME=home)
    env.pop("XDG_CONFIG_HOME", None)
    env.pop("ICOM_LOG", None)
    daemon = subprocess.Popen(
        [INTERCOMD, "--config", config, "--socket", sock, "--log-console", "--log-level", "info"],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    def ctl(*args):
        return subprocess.run([INTERCOMCTL, "-s", sock, *args], capture_output=True, text=True,
                              timeout=5).stdout.strip()

    deadline = time.monotonic() + 5
    while not os.path.exists(sock) and time.monotonic() < deadline and daemon.poll() is None:
        time.sleep(0.02)
    time.sleep(0.3)  # let the engine drain the FIFOs

    status = ctl("status")
    check(f"audio={'up' if with_devices else 'down'}" in status, f"{label}: STATUS reports {status!r}")
    check("ringing" in ctl("bell", "ring", "50"), f"{label}: the bell still works")
    if with_mute:
        check("mute=off" in status, f"{label}: the line starts unmuted")
        check(ctl("line", "mute", "on") == "OK on", f"{label}: LINE MUTE ON mutes")
        check("mute=on" in ctl("status"), f"{label}: STATUS reports mute=on")
        check(ctl("line", "mute") == "OK on", f"{label}: LINE MUTE reports it")
        check(ctl("line", "mute", "off") == "OK off", f"{label}: LINE MUTE OFF unmutes")
    else:
        check("mute=unconfigured" in status, f"{label}: STATUS reports mute=unconfigured")
        check(ctl("line", "mute", "on").startswith("ERR"), f"{label}: LINE MUTE ON fails")
        check(ctl("line", "mute") == "OK unconfigured", f"{label}: LINE MUTE reports unconfigured")

    daemon.send_signal(signal.SIGTERM)
    # The audio threads are blocked reading the empty FIFOs; closing them
    # just after the signal lets them return and see the stop request.
    time.sleep(0.05)
    for fd in fifos:
        os.close(fd)
    try:
        output, _ = daemon.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        daemon.kill()
        output, _ = daemon.communicate()
        check(False, f"{label}: intercomd shut down within 5 s of SIGTERM")
    check(daemon.returncode == 0, f"{label}: exit code {daemon.returncode} after SIGTERM")
    check("received signal 15" in output and "stopped" in output,
          f"{label}: SIGTERM went through intercomd's own handler")

    if with_devices:
        check(played_exactly(os.path.join(scratch, "inside_speaker.raw"), PATTERNS["door"]),
              f"{label}: the inside speaker got exactly the door mic")
        check(played_exactly(os.path.join(scratch, "door_speaker.raw"), PATTERNS["inside"]),
              f"{label}: the door speaker got exactly the inside mic")
    if failures:
        print("--- intercomd output:\n" + output)


def invalid_mute_active(scratch):
    text = open(CONFIG).read()
    check("\n#mute_active = high\n" in text, "the shipped config leaves mute_active at its default")
    config = os.path.join(scratch, "icom2000.conf")
    with open(config, "w") as f:
        f.write(text.replace("\n#mute_active = high\n", "\nmute_active = sideways\n"))
    result = subprocess.run([INTERCOMD, "--config", config, "--socket", os.path.join(scratch, "icom.sock")],
                            capture_output=True, text=True, timeout=5)
    check(result.returncode == 1 and "mute_active must be high or low" in result.stderr,
          f"invalid mute_active: intercomd refuses to start ({result.returncode}, {result.stderr.strip()!r})")


for with_devices, with_mute in ((True, False), (False, False), (False, True)):
    scratch = tempfile.mkdtemp(prefix="icom_daemon_test_")
    try:
        run(scratch, with_devices, with_mute)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

scratch = tempfile.mkdtemp(prefix="icom_daemon_test_")
try:
    invalid_mute_active(scratch)
finally:
    shutil.rmtree(scratch, ignore_errors=True)

if failures:
    print(f"{len(failures)} check(s) failed")
    sys.exit(1)
print("all checks passed")
