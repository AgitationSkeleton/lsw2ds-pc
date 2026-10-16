/* Recording and replaying a session's input, exactly.
 *
 *     lsw2_bountyhunter.exe --record run.lsw2input
 *     lsw2_bountyhunter.exe --replay run.lsw2input
 *
 * WHY THIS EXISTS. Every unverified thing in this project is waiting on the
 * same scarce resource: somebody sitting down and walking the game to a
 * specific screen. A recording turns one walk into an asset - the same eleven runbook steps can
 * be replayed after every build, and a fix that needs "boot, Cantina, console,
 * pause, wireless row" stops costing a person's attention each time.
 *
 * WHAT IS CAPTURED, and it is the whole of what the guest can see:
 *
 *   - the 12-bit key mask, active-high (A B Select Start Right Left Up Down
 *     R L X Y), taken AFTER the menu host has decided which presses the guest
 *     is allowed to see, so a replay reproduces what the game got rather than
 *     what the keyboard did;
 *   - the touch point and pen state, in DS screen pixels.
 *
 * TOUCH IS EXACT, not approximate. `nds_set_touch` stores `x << 4` into the
 * 12-bit ADC domain, so a screen pixel survives the round trip unchanged -
 * recording pixels loses nothing. And because the coordinates are recorded
 * AFTER the window-to-screen mapping, a session recorded in a 4x window
 * replays identically in a 1x one. That is the property that makes a recording
 * worth keeping: it is a record of what the DS saw, not of where the mouse was.
 *
 * INDEXED BY GUEST VBLANK, not by host loop iteration. The host loop can run
 * fast, slow, or skip; the guest's vblank count cannot. Replaying against the
 * vblank counter means a recording made at 60fps still lands on the right frame
 * during a stutter, and a turbo replay stays aligned. Determinism holds
 * because the guest tick is preserved rather than free-running, which is
 * what makes this indexable at all.
 *
 * DELTAS, NOT FRAMES. Only changes are written, with the vblank they take
 * effect on. A minute of walking is a few hundred lines rather than 3,600, and
 * the file stays readable - which matters because the whole point is to be able
 * to check, by eye, that a recording contains the walk it claims to.
 */

#ifndef LSW2_INPUTLOG_H
#define LSW2_INPUTLOG_H

#include <cstdint>

/* The input the guest is about to be given for one frame. */
struct Lsw2InputFrame {
    uint16_t keys = 0;      /* active-high, bits 0..11                      */
    uint16_t touch_x = 0;   /* 0..255, DS screen pixels                     */
    uint16_t touch_y = 0;   /* 0..191                                       */
    bool touch_down = false;
};

enum class Lsw2InputMode { Off = 0, Record, Replay };

/* Open a recording or a replay. Either path may be null; passing both is
 * rejected, because a file cannot be its own source and destination and doing
 * it by accident would truncate the recording being replayed. Returns false
 * and leaves the mode Off if the file cannot be opened. */
bool lsw2_inputlog_open(const char* record_path, const char* replay_path);

/* Flush and close. Safe to call when nothing is open. */
void lsw2_inputlog_close();

Lsw2InputMode lsw2_inputlog_mode();

/* Once per frame, immediately before the input is published to the guest.
 *
 * Recording: writes a line when `f` differs from the previous frame.
 * Replaying: OVERWRITES `f` with what was recorded for this vblank.
 *
 * `vblank` is the guest's vblank count - a monotonic frame index the host
 * cannot skew. */
void lsw2_inputlog_frame(Lsw2InputFrame& f, uint64_t vblank);

/* True once a replay has passed its last recorded event. The caller decides
 * what that means - the shell prints a line and keeps running, so a replay
 * that ends mid-menu leaves the game where it got to rather than closing and
 * taking the evidence with it. */
bool lsw2_inputlog_replay_finished();

/* ------------------------------------------------------------------ parsing
 *
 * Split out so `test_inputlog.cpp` can pin the format without a file, a
 * window, or a guest - the same split customization_policy.cpp and
 * enhanced_policy.cpp make, and for the same reason. */

/* Parse one recorded line into `f` and its vblank. Returns false for blank
 * lines, comments, and anything malformed - a corrupt line is skipped rather
 * than aborting the replay, because losing one frame of a long walk is better
 * than losing the walk. */
bool lsw2_inputlog_parse(const char* line, Lsw2InputFrame& f, uint64_t& vblank);

/* Render one event as it is written. `buf` takes at least 64 bytes. */
void lsw2_inputlog_format(char* buf, unsigned n, const Lsw2InputFrame& f,
                          uint64_t vblank);

#endif /* LSW2_INPUTLOG_H */
