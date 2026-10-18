# Changelog

Versions follow [semantic versioning](https://semver.org) loosely while the
port is pre-1.0: a minor version adds features, and a patch version only fixes
bugs. Each release's section here becomes its GitHub release notes
([RELEASING.md](RELEASING.md)). New entries go under **Unreleased** as they
land.

## Unreleased

- Fixed: a host could crash when a second player joined its lobby, if the
  joining player wore a hat over a helmet-class character. The head re-attach
  that caused it never actually showed the head, so it has been removed for
  now.
- Fixed: hosting stuttered every few seconds. The master server's heartbeat
  now runs in the background instead of on the game's frame.
- Fixed: players on the same network as the host saw its lobby twice, and
  joining through the master server's listing or a room code could fail. They
  are now given the host's local address.
- Enhanced controls: new camera turn and height speed settings in the Input
  tab, with faster defaults. The camera now starts moving smoothly just past
  the deadzone.

## 0.1.0 - 2026-10-16

The first public release, and an early one:

- **Tested on one machine only.** All testing so far ran on a single Windows
  PC.
- **Online play is not thoroughly playtested.** Two to four players reached
  matches at 60 fps, with several game instances on that one machine. Play
  between separate PCs over the internet has had little testing, and so have
  routers and NAT, joining in progress and disconnects. Expect desyncs and
  dropped sessions, and please report them with the newest file from
  `build\logs\`.

### Bounty Hunter (`lsw2_bountyhunter.exe`)
- 2–4 players online at 60 fps over a deterministic TCP lockstep, with a
  host-level stand-in for the DS wireless layer.
- The arena lobby as a walk-around space. Its console holds Start Game, the host
  settings and the kick list.
- Host settings: visibility (master list / LAN), roster (Hunters, All, Random
  Hunter, Random All, or One chosen character), Extras, controls (Tank /
  Enhanced), customization (Off / Hats / All), battle music (Default / Random,
  drawn from the battle tracks and the arena's own), lobby music (Hunter /
  Cantina / Random) and lobby timer (Off / 30 s / 60 s / 5 min).
- A public lobby list with four-letter room codes, LAN discovery and UPnP.
  Players can join a match in progress.
- Everything unlocked from the first start. Customization and settings persist.
- Enhanced controls: a right-stick camera with inversion options.

### Both programs
- First start asks for the ROM with a file picker, verifies its SHA-1 and copies
  it into `rom/`.
- No console window. Every session writes a log to `build\logs\`.
- FreeBIOS and a generated firmware: no BIOS files needed to play.
- The master server moved to Cloudflare at `lsw2server.viosarcade.xyz`. Hosting,
  joins, leaves, game starts (with settings) and disbanded lobbies are logged
  to Discord, in one channel for co-op and one for Bounty Hunter.
