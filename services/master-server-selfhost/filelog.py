"""Writing the event log to disk.

The master server keeps its logs in a logs directory that rotates every 31
days, so the logs do not bloat.

HOW THAT IS READ. A directory that "rotates every 31 days to avoid bloat" is a
rolling 31-day window: one file per day, named by date, and days older than 31
are deleted. The alternative reading - a single file replaced every 31st day -
gives one enormous file and then throws away a month of history in one step,
which is worse on both counts: size and history. Daily files also mean "what
happened on the 14th" is one `type` away instead of a search through a month.

    logs/
      master-2026-08-27.log      <- today, appended to
      master-2026-08-26.log
      ...                        <- 31 of these at most

APPEND, NEVER TRUNCATE. The service can be restarted several times in a day and
each run's lines belong in the same day's file; a run boundary is a log line, not
a new file. This matters more than it sounds: the reason to read these logs is
usually "the service died", and truncating on start would destroy the evidence
at exactly the moment it is wanted.

Failures here are swallowed. A directory that cannot write its log is still a
directory, and taking lobbies offline because a disk filled up would be the
wrong trade.
"""

from __future__ import annotations

import os
import threading
import time

import eventlog

# The retention window: 31 days. Files whose date is older than this many days
# are removed when the sink rolls over to a new day.
RETAIN_DAYS = 31

PREFIX = "master-"
SUFFIX = ".log"


def _day_of(when: float) -> str:
    return time.strftime("%Y-%m-%d", time.localtime(when))


class FileSink:
    """An eventlog sink that appends to logs/master-YYYY-MM-DD.log.

    Attach with `log.add_sink(FileSink(dir), replay=True)` - the replay is
    wanted, so a log file started mid-run still contains the startup lines.
    """

    def __init__(self, directory: str, minimum: int = eventlog.DEBUG,
                 retain_days: int = RETAIN_DAYS):
        self.directory = directory
        self.minimum = minimum
        self.retain_days = retain_days
        self._lock = threading.Lock()
        self._handle = None
        self._day = None
        self.last_error = None

    # --------------------------------------------------------------- sink

    def __call__(self, record) -> None:
        if record.level < self.minimum:
            return
        line = record.format()
        with self._lock:
            handle = self._handle_for(_day_of(record.when))
            if handle is None:
                return
            try:
                handle.write(line + "\n")
                handle.flush()
            except OSError as exc:
                # Flush every line rather than buffer: an unclean exit is the
                # case these logs exist for, and a buffered tail would be lost
                # exactly then.
                self.last_error = str(exc)

    def close(self) -> None:
        with self._lock:
            if self._handle is not None:
                try:
                    self._handle.close()
                except OSError:
                    pass
                self._handle = None
                self._day = None

    # ------------------------------------------------------------ internals

    def path_for(self, day: str) -> str:
        return os.path.join(self.directory, PREFIX + day + SUFFIX)

    def _handle_for(self, day: str):
        if self._handle is not None and self._day == day:
            return self._handle

        if self._handle is not None:
            try:
                self._handle.close()
            except OSError:
                pass
            self._handle = None

        try:
            os.makedirs(self.directory, exist_ok=True)
            self._handle = open(self.path_for(day), "a", encoding="utf-8")
            self._day = day
        except OSError as exc:
            self.last_error = str(exc)
            self._handle = None
            self._day = None
            return None

        # THE SWEEP HAPPENS ON ROLLOVER, not on a timer. A service running
        # across midnight rolls and sweeps then; a service started fresh sweeps
        # on its first line. Either way there is no background thread to leak,
        # which is the same reasoning the directory's row expiry uses.
        self.prune()
        return self._handle

    def prune(self, now: float | None = None) -> list:
        """Delete day files older than the retention window. Returns what was
        removed, which is what the test asserts on and what the service logs."""
        now = time.time() if now is None else now
        cutoff = now - self.retain_days * 86400
        removed = []
        try:
            names = os.listdir(self.directory)
        except OSError:
            return removed

        for name in names:
            if not (name.startswith(PREFIX) and name.endswith(SUFFIX)):
                continue
            day = name[len(PREFIX):-len(SUFFIX)]
            try:
                stamp = time.mktime(time.strptime(day, "%Y-%m-%d"))
            except ValueError:
                # Not one of ours despite the name. Leaving a file alone is
                # always the safe half of a delete decision.
                continue
            if stamp >= cutoff:
                continue
            try:
                os.remove(os.path.join(self.directory, name))
                removed.append(name)
            except OSError:
                pass
        return removed
