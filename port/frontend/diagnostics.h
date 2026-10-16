/* One run, every outstanding measurement.
 *
 * WHY THIS EXISTS. Six features are blocked on a reading from a running game,
 * and every one of them had its own switch, its own log format, and its own
 * place to look:
 *
 *   Enhanced controls  which view index the arena assigns
 *   pause menu         which script opcode the pause menu raises
 *   Settings           whether the cartridge's Settings screen persists
 *   battle music       which music slots are the battle tracks
 *   arena HUD          the arena HUD's source rectangles
 *   lobby console      what the console entity's script actually enqueues
 *
 * Each is one line of output. Scattered across five switches, answering them
 * means five separate runs and remembering what to turn on for each; together
 * they are ONE run and one log. Play the fork; the questions get answered.
 *
 *     LSW2_DIAG=1 ./lsw2_bountyhunter
 *
 * WATCHING, NEVER REPLACING. Everything here reads guest memory and prints.
 * That distinction matters: the first plan for the
 * music question was to override the request function and log from inside it,
 * and the seam REPLACES a function rather than wrapping it - so any slip in the
 * reimplementation would have produced a table of what the port does instead of
 * a table of what the game does. Reading cannot make that mistake.
 *
 * ON CHANGE, NOT ON FRAME. Every value here is stable for seconds at a time and
 * the loop runs at 60Hz. Printing unconditionally would bury the six lines that
 * matter under a hundred thousand that do not, which is the same failure the
 * string decoder's cache was added to fix.
 */

#ifndef LSW2_DIAGNOSTICS_H
#define LSW2_DIAGNOSTICS_H

/* Is the diagnostics pass on? Also true when a specific LSW2_TRACE_* switch is
 * set, so the older switches keep working on their own. */
bool lsw2_diag_enabled();

/* Once per frame, after the guest has stepped. Cheap when off: one cached bool
 * and a return. */
void lsw2_diag_tick();

/* Printed once, at startup, when the pass is on: what is being watched and what
 * to do to make each line appear. A log nobody knows how to read is a log
 * nobody reads. */
void lsw2_diag_announce();

#endif /* LSW2_DIAGNOSTICS_H */
