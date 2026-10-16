"""The directory's event log.

The GUI shows a somewhat verbose console log: debug info, startup info, ports
and IP info, and player and session joins/leaves and lobby states - for example
a player from an IP connecting and starting a new bounty or co-op game.

There are TWO executables over one service, so the log cannot be `print`.
Both flavours want the same lines; they differ only in where the lines go - the
CLI writes them to stdout, the GUI appends them to a text pane. So the service
emits records to this object, and each front end attaches a sink.

WHAT "VERBOSE" MEANS HERE. Verbose is not the same as noisy. The default logger
in the stdlib http.server prints one line per request, which on a directory that
is heartbeated every few seconds by every lobby is a wall of identical text
that hides the lines a person is actually watching for. The categories below are
the events listed above, and routine heartbeats are DEBUG so a front end can
show or hide them without the service caring.

The ring buffer exists because the GUI is attached late: a window opened a minute
after the service started should still be able to show the startup and port
lines.
"""

from __future__ import annotations

import threading
import time
from collections import deque

DEBUG = 10
INFO = 20
WARN = 30
ERROR = 40

_NAMES = {DEBUG: "DEBUG", INFO: "INFO", WARN: "WARN", ERROR: "ERROR"}


class Record:
    __slots__ = ("when", "level", "category", "message")

    def __init__(self, when: float, level: int, category: str, message: str):
        self.when = when
        self.level = level
        self.category = category
        self.message = message

    @property
    def level_name(self) -> str:
        return _NAMES.get(self.level, str(self.level))

    def format(self) -> str:
        stamp = time.strftime("%H:%M:%S", time.localtime(self.when))
        return "[%s] %-5s %-9s %s" % (stamp, self.level_name, self.category,
                                      self.message)

    def __repr__(self) -> str:                       # pragma: no cover - debug aid
        return "Record(%r)" % self.format()


class EventLog:
    def __init__(self, capacity: int = 4000):
        self._lock = threading.Lock()
        self._records: deque[Record] = deque(maxlen=capacity)
        self._sinks: list = []

    # ---------------------------------------------------------------- sinks

    def add_sink(self, sink, replay: bool = True) -> None:
        """Attach a callable taking one Record.

        `replay` hands the new sink everything already buffered, which is what
        makes a GUI opened after startup still show the startup lines. It runs
        outside the lock so a slow sink cannot stall the service.
        """
        with self._lock:
            self._sinks.append(sink)
            backlog = list(self._records) if replay else []
        for record in backlog:
            self._safe(sink, record)

    def remove_sink(self, sink) -> None:
        with self._lock:
            if sink in self._sinks:
                self._sinks.remove(sink)

    @staticmethod
    def _safe(sink, record: Record) -> None:
        try:
            sink(record)
        except Exception:
            # A FRONT END THAT THROWS MUST NOT TAKE THE DIRECTORY WITH IT. The
            # GUI's sink marshals onto the Tk thread and that can fail while a
            # window is closing; a lobby list is worth more than a log line.
            pass

    # ----------------------------------------------------------------- emit

    def emit(self, level: int, category: str, message: str) -> Record:
        record = Record(time.time(), level, category, message)
        with self._lock:
            self._records.append(record)
            sinks = list(self._sinks)
        for sink in sinks:
            self._safe(sink, record)
        return record

    def debug(self, category, message): return self.emit(DEBUG, category, message)
    def info(self, category, message): return self.emit(INFO, category, message)
    def warn(self, category, message): return self.emit(WARN, category, message)
    def error(self, category, message): return self.emit(ERROR, category, message)

    # ----------------------------------------------------------------- read

    def tail(self, count: int = 200) -> list:
        with self._lock:
            if count <= 0 or count >= len(self._records):
                return list(self._records)
            return list(self._records)[-count:]


def stdout_sink(minimum: int = DEBUG):
    """A sink for the command-prompt flavour."""
    def sink(record: Record) -> None:
        if record.level >= minimum:
            print(record.format(), flush=True)
    return sink
