/* THE LOG NEVER MAKES THE GAME WAIT.
 *
 * Everything this port reports goes to stderr, and until now every line was
 * written on the game's own thread, synchronously, wherever stderr pointed.
 * Measured: a one-line report that fires once a second
 * cost 15 ms of that second, and the WH state report - several lines a second
 * in a data-sharing session - cost ~90 ms of every second on the host. A line
 * is a few bytes; the time was the WRITE, to whatever was on the other end.
 *
 * And the port is a console program, so a double-click opens a console window
 * and that is what is on the other end. A console write is slow, and a console
 * the player has clicked into (QuickEdit selection) does not return from a
 * write at all until the selection is cleared - a frozen game, from a click on
 * the wrong window.
 *
 * lsw2_asynclog_start() puts an in-memory pipe between the two: stderr writes
 * land in the pipe's buffer and return, and a background thread carries them
 * to the real destination at whatever pace it accepts. Nothing is dropped and
 * the order is kept - it is the same byte stream, delivered by another thread.
 * A 1 MiB buffer is many minutes of this port's logging, so the game thread
 * only ever waits if the destination stops accepting for that long.
 *
 * Host layer only. LSW2_SYNC_LOG=1 restores the direct write, for a crash hunt
 * where the last line before the crash must be on disk before the crash. */
#ifndef LSW2_ASYNCLOG_H
#define LSW2_ASYNCLOG_H

/* Idempotent. A no-op off Windows, with no stderr, or under LSW2_SYNC_LOG=1. */
void lsw2_asynclog_start();

#endif /* LSW2_ASYNCLOG_H */
