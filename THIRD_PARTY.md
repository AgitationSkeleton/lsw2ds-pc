# Third-party components

This lists what the port is built from, and under which terms. It is a record,
not legal advice.

## The port

Everything in this repository not listed below is the port's own work and is
licensed under the **GNU General Public License v3.0 or later**
([LICENSE](LICENSE)).

## In the executables

| component | where | license |
|---|---|---|
| [ndsrecomp](https://github.com/RetroPortingToolKit/ndsrecomp) (the recompiler and runtime) | fetched by `tools/fetch_ndsrecomp.sh`, patched by `port/patches/` | MIT |
| melonDS GPU3D and Wi-Fi code, vendored by ndsrecomp's runner | inside ndsrecomp | **GPL-3.0-or-later** |
| FreeBIOS (the DraStic BIOS replacement), embedded by ndsrecomp | inside ndsrecomp | BSD-2-Clause |
| libslirp, vendored by ndsrecomp | inside ndsrecomp | BSD-3-Clause |
| toml++, downloaded by ndsrecomp's CMake | inside ndsrecomp | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | `port/frontend/vendor/imgui/` | MIT |
| [Roboto](https://github.com/googlefonts/roboto) Medium, the settings menu's font (the copy in Dear ImGui's `misc/fonts/`) | compiled in from `port/frontend/uifont.cpp` | Apache-2.0 ([text](port/frontend/vendor/imgui/misc/fonts/LICENSE-Roboto.txt)) |
| [SDL2](https://libsdl.org) | `SDL2.dll`, from MSYS2 | zlib |
| GCC runtime (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`) | from MSYS2 UCRT64 | GPL-3.0 with the GCC Runtime Library Exception |
| winpthreads (`libwinpthread-1.dll`) | from MSYS2 UCRT64 | MIT and BSD-3-Clause |

**The executables as a whole are GPL-3.0-or-later**, because they link melonDS
code. Anyone who distributes them must also offer the corresponding source.
This repository is that source, together with the pinned ndsrecomp commit and
`port/patches/`. A release zip names the commit it was built from, and carries
this file and the license texts in its `licenses` folder.

## Not included, and never redistributed

- **The game.** LEGO Star Wars II: The Original Trilogy is © its owners. You
  supply your own cartridge dump. The build reads it to lift the code and
  extract the fonts. Neither the dump nor anything decompiled from it is
  committed here.
- **Nintendo's BIOS and firmware.** The build uses your own BIOS dumps if you
  have them, and the executables run on FreeBIOS and a generated firmware image.

## The master server

`services/master-server/` is a Cloudflare Worker, built with
[wrangler](https://github.com/cloudflare/workers-sdk) (MIT/Apache-2.0, a
development dependency only).
