# config/

Deployment configuration for a Pi running icom2000. See
[`../docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md) "Configuration" for
the full rationale; this is just where each file goes and where it comes
from.

| File | Installs to | Source |
|---|---|---|
| `icom2000.conf` | `/etc/icom2000.conf` | tracked in this repo, edit directly |
| `asound.conf` | `/etc/asound.conf` | tracked in this repo, edit directly |
| `codec-zero-intercom.state` | `/etc/codec-zero-intercom.state` | **not in this repo** -- generated on real hardware, see below |

## The alsactl state file

`codec-zero-intercom.state` is not shipped here and shouldn't be
fabricated by hand or by a tool that hasn't seen the actual codec's mixer
controls -- it's a dump of the DA7212's specific control names and values
(the crossbar routing between "door" and "inside", levels, switches),
captured from a live system that already has the mixer configured the way
you want it (`alsamixer` / `amixer`), via:

```sh
sudo alsactl store -f /etc/codec-zero-intercom.state
```

`systemd/alsa-restore-codec-zero.service` (in the repo root's `systemd/`
directory) restores from exactly that path at boot -- see its comments,
and docs/ARCHITECTURE.md "Centralized mixer state" for why intercomd
itself never touches mixer controls at runtime.
