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

// ALSA-backed engine, first step (see docs/ARCHITECTURE.md "Audio
// boundary"): one mono route, capturing from `captureFrom`'s capture
// device and playing it straight out on `playbackTo`'s playback device, on
// its own thread. Not yet used by the daemon, which still runs
// makeNullEngine().
//
// start() never throws on a device problem: if either device can't be
// opened or configured, it logs why and leaves isRunning() false -- as
// does an unrecoverable error on the audio thread later -- so the daemon
// keeps ringing the bell and reports audio=down rather than dying over
// audio alone.
//
// Passing the same station twice routes its mic into its own speaker:
// fine for a bench test with headphones, acoustic feedback on the real
// handset/door unit.
std::unique_ptr<IEngine> makeAlsaEngine(const config::Station& captureFrom,
                                        const config::Station& playbackTo);

} // namespace icom::audio
