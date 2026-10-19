# Changelog

Versions follow [semantic versioning](https://semver.org) loosely while the
port is pre-1.0: a minor version adds features, and a patch version only fixes
bugs. Each release's section here becomes its GitHub release notes
([RELEASING.md](RELEASING.md)). New entries go under **Unreleased** as they
land.

## Unreleased

- Fixed (a bug in the original game): in kill-based modes, blowing yourself up
  with your own explosive (Bossk's, Leia in Boushh disguise's, 4-LOM's)
  counted as a kill. A death you cause yourself now scores like a fall: a
  death and nothing else. New option under Enhancements > Gameplay, "Suicides
  don't score", on by default. It's a match rule, so the host's setting
  applies to everyone.
- Fixed: after a capture or a kill reordered the score column, a player who
  then respawned as a new character (random characters) had their portrait
  drawn on another player's row, so the scorer appeared beside that player's
  score of 0.
- The top screen now shows your own score above your hearts, where the stud
  counter sits in story mode: kills and deaths, or captures in Capture. The
  other players' scores were already in the column at the lower left.
- Online play needs everyone on this version: the host's match rules gained a
  field, and older versions won't accept them.

## 0.3.1 - 2026-10-18

Still an early pre-release, and **not thoroughly tested**: these fixes were
checked on a single PC only, with two players. **Online play is still not
thoroughly playtested** - play between separate PCs over the internet, and
matches with three or four players, have had little testing.

- Fixed: "Random All" and "Random Hunter" dealt the same characters in the
  same order every session, and random music followed a fixed order too. Each
  session now starts from a random seed.
- Fixed: with Enhanced controls, the radar turned with the character instead
  of the camera. Up on the radar is now the direction the camera faces.
- The touch screen now shows as a floating panel inside the game window by
  default, instead of in a separate window. Settings saved with the old
  default switch over once; you can turn the separate window back on in
  Settings > Touch Screen.

## 0.3.0 - 2026-10-18

Still an early pre-release, and **not thoroughly tested**. These changes were
checked on a single PC only, with two players. **Online play is still not
thoroughly playtested**: play between separate PCs over the internet, and
matches with three or four players, have had little testing. Please report
problems with the newest file from `build\logs\`.

- New settings menu layout: the F1 menu is now a full-window menu with
  sections along the top (Settings, Enhancements, Multiplayer, Debug), pages
  down the side and a search box that finds any option. The game keeps running,
  dimmed, behind it. Saved settings carry over unchanged. Some options moved:
  the arena camera and portrait options are under Enhancements, and so are the
  lobby character select and arena HUD options.
- Fixed: choosing Quit after a match sent every player to single player and
  left them stuck in the lobby, with cutscene bars and a squashed HUD. The row
  is now called "Return to Lobby", and it returns everyone to the online lobby,
  where they can see each other and the host can start the next match.
- The match results screen's "Continue" now reads "New Match on Same Map".
- Fixed: with random music on, a new match on the same map kept the previous
  track, and a match could start with the lobby's track (such as the cantina
  music). Each match now draws a fresh track, and never the one that's
  playing.
- Fixed: with random characters on, player 2's top-screen ring turned from
  green to blue after their first respawn.
- Fixed: with random characters on, the HUD portraits of the other players
  stayed on their first character all match. They now follow each respawn.
- Fixed: during match notices (the countdown and the time warnings) the picture
  briefly dropped to the original resolution, with black bars at the sides.
- Fixed: "Reset to defaults" in the button bindings cleared the four Camera
  keys instead of restoring J, L, I and K.

## 0.2.0 - 2026-10-17

Still an early pre-release. All testing so far ran on a single PC; play between
separate PCs over the internet is largely untested.

- Fixed: a host could crash when a second player joined its lobby, if the
  joining player wore a hat over a helmet-class character. The head re-attach
  that caused it never actually showed the head, so it has been removed for
  now.
- Fixed: hosting stuttered every few seconds. The master server's heartbeat
  now runs in the background instead of on the game's frame.
- Fixed: players on the same network as the host saw its lobby twice, and
  joining through the master server's listing or a room code could fail. They
  are now given the host's local address.
- Fixed: the lobby music stopped for good when a player joined. The join
  screens stop the track in a way the game doesn't record, and the lobby that
  reloads afterwards asks for the same track, so the game skipped it. The
  track is now marked for restart, and the lobby plays it again.
- The self-hosted master server (`lsw2-master.exe`, `lsw2-master-gui.exe`)
  gives same-network players the host's local address too, matching the
  public one.
- Logs now record every music change. Setting `LSW2_AUDIO_LEVEL=1` also logs
  the output level once a second.
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
