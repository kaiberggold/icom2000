#pragma once

#include "icom/config/station_registry.hpp"

#include <memory>

namespace icom::audio {

// Boundary for the audio path -- out of scope for this pass (see
// docs/ARCHITECTURE.md "Audio boundary"), stubbed here so the daemon's
// composition root has a real seam to wire a working implementation into
// later without reshaping app/daemon_main.cpp.
//
// The eventual implementation owns two mono ALSA duplex streams against the
// Codec Zero (handset audio to/from the TCM1171's VSP pins on one channel,
// door/bell/ambient on the other) on its own thread(s) with real-time
// scheduling, communicating with the reactor thread via lock-free queues --
// deliberately NOT folded into the EventLoop's poll(), since audio I/O has
// hard timing needs poll()'s single-threaded dispatch shouldn't be allowed
// to jitter.
class IEngine {
public:
    virtual ~IEngine() = default;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
};

// `stations` supplies each station's named ALSA PCM devices (defined in
// config/asound.conf, resolved from config/icom2000.conf -- see
// docs/ARCHITECTURE.md "Stations"). NullEngine itself does nothing
// with them; they're threaded through here anyway so the *signature* a
// real engine has to implement never has room for a hardcoded sound-card
// device string -- device names come from the registry, full stop.
//
// Swap the returned type out in the composition root
// (src/app/daemon_main.cpp) once a real ALSA-backed Engine exists.
std::unique_ptr<IEngine> makeNullEngine(const config::StationRegistry& stations);

// ALSA-backed engine for one mono route: captures from `captureFrom`'s
// capture device and plays it straight out on `playbackTo`'s playback
// device, on its own thread (see docs/ARCHITECTURE.md "Audio boundary").
//
// start() never throws on a device problem: if either device can't be
// opened or configured, it logs why and leaves isRunning() false -- as
// does an unrecoverable error on the audio thread later -- so the daemon
// keeps ringing the bell and reports audio=down rather than dying over
// audio alone.
//
// `rtPriority` > 0 runs the audio thread at that SCHED_FIFO priority, so
// a busy moment elsewhere on the Pi Zero's single core can't starve it
// into an audible dropout. Needs CAP_SYS_NICE or an RLIMIT_RTPRIO at
// least that high (systemd/intercomd.service sets LimitRTPRIO); without
// either it logs a warning and runs at normal priority. 0 leaves the
// thread's scheduling alone.
//
// Passing the same station twice routes its mic into its own speaker:
// fine for a bench test with headphones, acoustic feedback on the real
// handset/door unit.
std::unique_ptr<IEngine> makeAlsaEngine(const config::Station& captureFrom,
                                        const config::Station& playbackTo, int rtPriority = 0);

// The intercom itself: both directions between two stations at once, `a`'s
// mic to `b`'s speaker and `b`'s mic to `a`'s speaker, one route and one
// thread each. All or nothing: if either route fails to start, start()
// stops the other and isRunning() stays false. If one route later fails on
// its own, isRunning() turns false while the other keeps running.
std::unique_ptr<IEngine> makeAlsaIntercomEngine(const config::Station& a, const config::Station& b,
                                                int rtPriority = 0);

} // namespace icom::audio
