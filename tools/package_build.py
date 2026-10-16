"""Build the test-build zip: what a tester unzips and double-clicks.

    py -3.13 tools/package_build.py            # after bash port/build.sh
    py -3.13 tools/package_build.py --version v0.9.0

WHAT GOES IN, and only that. The layout is the one the executable expects when
it is run with no arguments (patch 0005): build/ beside port/ and rom/.

    LSW2DS-PC-<version>/
      README.txt                     how to play, for testers
      build/lsw2_bountyhunter.exe    the Bounty Hunter build (its own save)
      build/lsw2.exe                 the base port, co-op story
      build/*.dll                    the MinGW runtime and SDL2
      build/assets/fonts/            the game's own fonts (made by the build)
      port/game.toml                 the title config, with the ROM's SHA-1
      port/saves/unlocked-template.sav   seeds the Bounty Hunter save
      rom/PUT-YOUR-ROM-HERE.txt      first run asks for the ROM and copies it here

WHAT NEVER GOES IN: a ROM, BIOS or firmware dumps (FreeBIOS and a generated
firmware are built in), anyone's saves, settings or logs. The script refuses
to write the zip if a staged file looks like any of them.

NO CONSOLE WINDOW. The dev build links as a console program, which is what the
harness and launch.sh want. The copies here are switched to the Windows GUI
subsystem in their PE header - the same code, no console behind the game. The
game still keeps its log: asynclog.cpp writes build/logs/lsw2-*.log whenever
stderr goes nowhere, and that file is what a tester sends back with a bug.
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
DIST = ROOT / "dist"

EXES = ["lsw2_bountyhunter.exe", "lsw2.exe"]
DLLS = ["SDL2.dll", "libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"]
FORBIDDEN_SUFFIXES = {".nds", ".srl", ".dsi", ".rom", ".bin", ".sav", ".log",
                      ".sqlite3", ".json", ".ml1", ".ml2", ".ml3", ".ml4", ".ml5"}
ALLOWED_EXCEPTIONS = {"port/saves/unlocked-template.sav"}

SOURCE_URL = "https://github.com/AgitationSkeleton/lsw2ds-pc"

README = """\
LEGO Star Wars II: The Original Trilogy (DS) - PC port
Build {version}
=====================================================================

This is a native PC port of the Nintendo DS game, with online play for
up to four players. It is a TEST build: thank you for playing it, and
please report anything that goes wrong (see "Reporting a bug" below).


WHAT YOU NEED
-------------
* Windows 10 or 11, 64-bit.
* Your own dump of LEGO Star Wars II: The Original Trilogy (USA) for the
  Nintendo DS, as a .nds file. This download does not include the game.
  The right dump has this SHA-1:
      {sha1}

Nothing else - no emulator, no BIOS files.


FIRST START
-----------
1. Unzip this folder anywhere you can write to (Desktop, Documents, ...).
   Not inside Program Files.
2. Open the "build" folder and double-click  lsw2_bountyhunter.exe
3. The first time, it asks for your ROM. Choose your .nds file: it is
   checked and copied into the "rom" folder, so you are only asked once.
   (You can also just put the .nds in the "rom" folder yourself.)

Windows SmartScreen may warn about an unrecognised app, because the
build is not signed. Choose "More info", then "Run anyway".


WHICH EXE
---------
  lsw2_bountyhunter.exe   Bounty Hunter: arena battles for 2-4 players,
                          everything unlocked. This is the one to test.
  lsw2.exe                The original game (story, co-op) with its own
                          separate save.


PLAYING ONLINE
--------------
Host:  choose to host a game. Your lobby appears in the online list and
       gets a four-letter ROOM CODE - send it to your friends.
Join:  pick the lobby from the list, or enter the room code.

The host's computer must be reachable on TCP port 28550. The game asks
your router to open it automatically (UPnP). If friends cannot join you,
forward TCP 28550 to your PC in your router, or let someone else host.


CONTROLS AND SETTINGS
---------------------
Press Esc (or F1) for the port's menu: controls, video, online and more.
Close the window to quit.
Settings are saved in port\\settings.json. Your game save is in
port\\saves\\.


REPORTING A BUG
---------------
Say what you were doing and what went wrong, and attach the newest file
from the build\\logs folder - every session writes one. If the game
crashed, Windows also keeps a crash dump in
    %LOCALAPPDATA%\\CrashDumps
(lsw2_bountyhunter.exe.*.dmp) - attach that too if there is one.


SOURCE CODE AND LICENSE
-----------------------
This program is free software under the GNU General Public License,
version 3 or later. Its complete source is at
    {source}
and this build was made from commit {commit}.
It contains no game code or data: those come from your own ROM.
"""

ROM_NOTE = """\
Your LEGO Star Wars II: The Original Trilogy (USA) .nds goes here.

You do not have to put it here yourself: the first time the game starts
it asks for the file and copies it into this folder.
"""


def git_version() -> str:
    try:
        out = subprocess.run(["git", "describe", "--tags", "--always", "--dirty"],
                             cwd=ROOT, capture_output=True, text=True, check=True)
        return out.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "dev"


def git_commit() -> str:
    try:
        out = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                             capture_output=True, text=True, check=True)
        return out.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def rom_sha1() -> str:
    in_game = False
    for line in (ROOT / "port" / "game.toml").read_text(encoding="utf-8").splitlines():
        t = line.strip()
        if t.startswith("["):
            in_game = t == "[game]"
        elif in_game and t.startswith("sha1"):
            return t.split('"')[1]
    raise SystemExit("port/game.toml names no [game] sha1")


def to_gui_subsystem(path: Path) -> None:
    """Set IMAGE_OPTIONAL_HEADER.Subsystem to WINDOWS_GUI (2)."""
    data = bytearray(path.read_bytes())
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit(f"{path.name}: not a PE image")
    subsystem_at = pe + 24 + 68
    current = struct.unpack_from("<H", data, subsystem_at)[0]
    if current not in (2, 3):
        raise SystemExit(f"{path.name}: unexpected subsystem {current}")
    struct.pack_into("<H", data, subsystem_at, 2)
    path.write_bytes(bytes(data))


def stage(version: str) -> Path:
    name = f"LSW2DS-PC-{version}"
    out = DIST / name
    if out.exists():
        shutil.rmtree(out)
    (out / "build" / "assets" / "fonts").mkdir(parents=True)
    (out / "port" / "saves").mkdir(parents=True)
    (out / "rom").mkdir()

    for exe in EXES:
        src = BUILD / exe
        if not src.is_file():
            raise SystemExit(f"{src} is missing - run bash port/build.sh first")
        shutil.copy2(src, out / "build" / exe)
        to_gui_subsystem(out / "build" / exe)
    for dll in DLLS:
        shutil.copy2(BUILD / dll, out / "build" / dll)
    fonts = sorted((BUILD / "assets" / "fonts").glob("*.lsw2font"))
    if not fonts:
        raise SystemExit("build/assets/fonts has no fonts - the build's font export did not run")
    for f in fonts:
        shutil.copy2(f, out / "build" / "assets" / "fonts" / f.name)

    shutil.copy2(ROOT / "port" / "game.toml", out / "port" / "game.toml")
    shutil.copy2(ROOT / "port" / "saves" / "unlocked-template.sav",
                 out / "port" / "saves" / "unlocked-template.sav")
    (out / "rom" / "PUT-YOUR-ROM-HERE.txt").write_text(ROM_NOTE.replace("\n", "\r\n"),
                                                        encoding="utf-8")
    (out / "README.txt").write_text(
        README.format(version=version, sha1=rom_sha1(), source=SOURCE_URL,
                      commit=git_commit()).replace("\n", "\r\n"),
        encoding="utf-8")
    return out


def audit(out: Path) -> None:
    bad = []
    for p in out.rglob("*"):
        if not p.is_file():
            continue
        rel = p.relative_to(out).as_posix()
        if rel in ALLOWED_EXCEPTIONS:
            continue
        if p.suffix.lower() in FORBIDDEN_SUFFIXES or "bios" in rel.lower() or "settings" in rel.lower():
            bad.append(rel)
    if bad:
        raise SystemExit("refusing to package - these must not be distributed:\n  " +
                         "\n  ".join(bad))


def zip_up(out: Path) -> Path:
    archive = DIST / f"{out.name}.zip"
    if archive.exists():
        archive.unlink()
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(out.rglob("*")):
            rel = Path(out.name) / p.relative_to(out)
            if p.is_dir():
                z.writestr(rel.as_posix() + "/", "")  # keep rom/ even when empty
            else:
                z.write(p, rel.as_posix())
    return archive


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--version", default=None, help="label (default: git describe)")
    ap.add_argument("--no-zip", action="store_true", help="stage only")
    args = ap.parse_args()
    version = args.version or git_version()

    out = stage(version)
    audit(out)
    print(f"staged  {out}")
    if args.no_zip:
        return 0
    archive = zip_up(out)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    print(f"zip     {archive}  ({archive.stat().st_size / 1e6:.1f} MB)")
    print(f"sha256  {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
