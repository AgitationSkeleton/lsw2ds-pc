"""Freeze the two flavours into executables.

The server ships as an executable in two flavours: a command-prompt instance
and a GUI.

    python build.py                 both
    python build.py --cli           just lsw2-master.exe
    python build.py --gui           just lsw2-master-gui.exe

WHY POINT THIS AT PYINSTALLER. Nothing required the executables to be built
from any particular language, so this keeps the service that already has a
passing test suite and wraps it, rather than rewriting a working
directory in another language for the sake of a file extension. The two binaries
are the same code with different entry points, which is the property that
matters: a behaviour can only differ between the windows if this file is wrong.

THE CONSOLE FLAG IS THE WHOLE DIFFERENCE. The CLI build keeps a console because
it IS the console; the GUI build drops it so double-clicking does not leave a
black window behind the real one. Everything else - the modules bundled, the
service inside - is identical.

NO EXTERNAL RUNTIME DEPENDENCY IS ADDED. PyInstaller is needed to BUILD, never
to run, and the service still starts with `python master_server.py` on any
machine with Python and no build step at all. That fallback is deliberate: a
frozen binary is a convenience for the person hosting, not a requirement.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Everything the two entry points import. Listed rather than discovered because
# PyInstaller follows imports well but a missing one only shows up when a user
# runs the binary, which is the worst place to find out.
MODULES = ("master_server.py", "master_server_gui.py", "banlist.py",
           "eventlog.py", "filelog.py")

TARGETS = {
    "cli": {
        "entry": "master_server.py",
        "name": "lsw2-master",
        "console": True,
        "what": "command prompt instance",
    },
    "gui": {
        "entry": "master_server_gui.py",
        "name": "lsw2-master-gui",
        "console": False,
        "what": "windowed console",
    },
}


def check_sources() -> bool:
    missing = [m for m in MODULES if not os.path.exists(os.path.join(HERE, m))]
    if missing:
        print("missing source files: %s" % ", ".join(missing))
        return False
    return True


def have_pyinstaller() -> bool:
    try:
        subprocess.run([sys.executable, "-m", "PyInstaller", "--version"],
                       check=True, capture_output=True)
        return True
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False


def build(key: str) -> bool:
    target = TARGETS[key]
    print("building %s (%s)" % (target["name"], target["what"]))

    cmd = [sys.executable, "-m", "PyInstaller",
           "--noconfirm", "--clean", "--onefile",
           "--name", target["name"],
           "--distpath", os.path.join(HERE, "dist"),
           "--workpath", os.path.join(HERE, "build"),
           "--specpath", os.path.join(HERE, "build"),
           "--console" if target["console"] else "--windowed"]

    # The siblings the entry point imports by plain name. --paths rather than
    # --hidden-import so PyInstaller resolves them the way Python does when the
    # service is run from this directory.
    cmd += ["--paths", HERE]
    for module in MODULES:
        if module != target["entry"]:
            cmd += ["--hidden-import", os.path.splitext(module)[0]]

    if not target["console"]:
        # tkinter's dynamic imports are not all visible to the analyser.
        cmd += ["--hidden-import", "tkinter",
                "--hidden-import", "tkinter.ttk",
                "--hidden-import", "tkinter.messagebox"]

    cmd.append(os.path.join(HERE, target["entry"]))

    result = subprocess.run(cmd, cwd=HERE)
    if result.returncode != 0:
        print("  FAILED")
        return False

    suffix = ".exe" if os.name == "nt" else ""
    produced = os.path.join(HERE, "dist", target["name"] + suffix)
    print("  -> %s" % produced)
    return os.path.exists(produced)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", action="store_true", help="build only the CLI flavour")
    ap.add_argument("--gui", action="store_true", help="build only the GUI flavour")
    args = ap.parse_args(argv)

    wanted = []
    if args.cli:
        wanted.append("cli")
    if args.gui:
        wanted.append("gui")
    if not wanted:
        wanted = ["cli", "gui"]

    if not check_sources():
        return 1

    if not have_pyinstaller():
        print("PyInstaller is not installed.\n"
              "    pip install pyinstaller\n\n"
              "It is only needed to BUILD. The service runs without it:\n"
              "    python master_server.py          (command prompt)\n"
              "    python master_server_gui.py      (window)")
        return 1

    ok = all(build(key) for key in wanted)
    if ok:
        print("\ndone. Both flavours read the same flags:")
        print("    lsw2-master --port 28553 --bans bans.txt --logs logs")
        print("\nThe ban file and the logs directory sit next to wherever the")
        print("binary is RUN, not where it was built - pass absolute paths if")
        print("that matters.")
    shutil.rmtree(os.path.join(HERE, "build"), ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
