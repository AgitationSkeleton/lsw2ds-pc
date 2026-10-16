# How the game works, and how the port works

These documents explain LEGO Star Wars II: The Original Trilogy for the
Nintendo DS from the inside. They cover what the game's code does and how this
port runs it on a PC. They were written from the project's reverse-engineering
findings, and each states how sure it is: a point is measured, read from the
code, or inferred.

Every address is a guest address in the USA cartridge
(SHA-1 `7c1afa070797945ba5a2577c7ddfded5cf2fc443`).

| document | what it covers |
|---|---|
| [game-internals.md](game-internals.md) | the programs and memory map, the main loop and 30 Hz simulation, the state and screen manager, saves and unlocks, the RNG, the string bank, the asset formats, actors and customization, the Bounty Hunter rules, the HUD and menus |
| [multiplayer.md](multiplayer.md) | the game's wireless stack, data sharing and its desync check, the message protocol, the lobby and match flow, and the port's replacement for all of it |
| [the-port.md](the-port.md) | static recompilation with ndsrecomp, overrides and guest calls, the shell, the Bounty Hunter build's features, and how the port is verified |

Also useful:

- [../port/ARCHITECTURE.md](../port/ARCHITECTURE.md): the base build and the
  Bounty Hunter build, and which layer owns what.
- [../playbook/README.md](../playbook/README.md): the method, written for anyone
  bringing up a DS game on a recompiler.
- [../BUILDING.md](../BUILDING.md): building from source.
