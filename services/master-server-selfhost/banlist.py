"""The directory's IP ban list.

The GUI offers "Kick" and "Ban IP" on any server or player, in case of
malicious behaviour exploiting the service. The policy: bans are permanent, by
IP, stored in a plain text file one address per line, they survive restarts,
and they are cleared only by editing that file by hand.

So: one address per line, plain text, no expiry, and **the file is the truth**.

THE FILE BEING THE TRUTH IS THE WHOLE DESIGN, and it is why this reloads rather
than caching. Clearing a ban by editing the text file only works if a
running service notices those changes - otherwise unbanning someone would mean
restarting the directory and dropping every lobby on it, which is a worse
outcome than the ban. The mtime is checked on each query; a file nobody edits
costs one stat.

Comments and blank lines are allowed even though the policy does not need
them. A ban list is edited by a human under pressure, and being able to write
down WHY an address is on it is the difference between a list that can be
maintained and one that only grows.
"""

from __future__ import annotations

import os
import threading


class BanList:
    def __init__(self, path: str):
        self.path = path
        self._lock = threading.Lock()
        self._entries: set[str] = set()
        self._mtime: float | None = None
        self.reload()

    # ------------------------------------------------------------------ read

    def reload(self) -> None:
        """Read the file. A missing file is an empty list, not an error - the
        common case is a service that has never banned anyone."""
        with self._lock:
            self._reload_locked()

    def _reload_locked(self) -> None:
        try:
            stat = os.stat(self.path)
        except OSError:
            self._entries = set()
            self._mtime = None
            return

        self._mtime = stat.st_mtime
        entries: set[str] = set()
        try:
            with open(self.path, "r", encoding="utf-8") as fh:
                for line in fh:
                    entry = line.split("#", 1)[0].strip()
                    if entry:
                        entries.add(entry)
        except OSError:
            # Unreadable is not the same as empty, and guessing "empty" would
            # silently lift every ban. Keep what we had.
            return
        self._entries = entries

    def _refresh_locked(self) -> None:
        try:
            mtime = os.stat(self.path).st_mtime
        except OSError:
            if self._mtime is not None:
                self._entries = set()
                self._mtime = None
            return
        if self._mtime is None or mtime != self._mtime:
            self._reload_locked()

    def is_banned(self, address: str) -> bool:
        if not address:
            return False
        with self._lock:
            self._refresh_locked()
            return address in self._entries

    def entries(self) -> list[str]:
        with self._lock:
            self._refresh_locked()
            return sorted(self._entries)

    # ----------------------------------------------------------------- write

    def add(self, address: str, note: str = "") -> bool:
        """Append an address. Returns False if it was already listed, so a
        caller can say "already banned" rather than writing a duplicate."""
        address = (address or "").strip()
        if not address:
            return False

        with self._lock:
            self._refresh_locked()
            if address in self._entries:
                return False

            line = address if not note else "%s  # %s" % (address, note)
            try:
                # Append rather than rewrite: a rewrite would discard comments
                # and ordering a human put there, which is exactly what
                # clearing bans by hand-editing relies on surviving.
                needs_nl = os.path.exists(self.path) and \
                    os.path.getsize(self.path) > 0
                with open(self.path, "a", encoding="utf-8") as fh:
                    if needs_nl:
                        with open(self.path, "rb") as probe:
                            probe.seek(-1, os.SEEK_END)
                            if probe.read(1) not in (b"\n", b"\r"):
                                fh.write("\n")
                    fh.write(line + "\n")
            except OSError:
                return False

            self._entries.add(address)
            try:
                self._mtime = os.stat(self.path).st_mtime
            except OSError:
                self._mtime = None
            return True
