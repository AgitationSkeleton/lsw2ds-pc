/* The first host-initiated guest call in this port, behind a button.
 *
 * WHY A PROBE AND NOT THE FEATURE. It was established that the port CAN call
 * a lifted guest function: they are plain C symbols, the generated code's own
 * call idiom is copyable, `runtime_call_push_return` accepts a synthetic return
 * address, and `0x020063D4`'s transform argument is optional - `0x0200641C`
 * takes the actor's own when it is NULL, and two of the game's call sites
 * already pass zero.
 *
 * All of that is read out of the framework and the ROM, and none of it has been
 * executed. Character customization needs six of these calls per character
 * build; doing six on the
 * strength of a document is how the last four attempts went. This does ONE, on
 * demand, and reports what happened.
 *
 * WHAT IT ANSWERS, in one press:
 *
 *   - does a host-initiated call return cleanly, with R[15] back at the
 *     synthetic return and the runtime not unwinding
 *   - does the actor's slot record at `actor + 0x6E8 + slot*2` actually change
 *     to the part that was passed
 *
 * If both hold, the customization implementation is mechanical. If either fails, it fails
 * here, once, with a log line - rather than six times a frame inside a feature.
 *
 * WHERE IT RUNS. The fork tick, which `frontend.cpp` calls *between frames,
 * with the guest settled* - the same safe point the cheats tab uses. A guest
 * call from anywhere else would nest inside guest execution. */

#ifndef LSW2_PARTAPPLY_H
#define LSW2_PARTAPPLY_H

/* Arm the probe. From the imgui button; takes effect on the next fork tick. */
void lsw2_partapply_arm();

/* Once a frame from the fork tick. Does nothing unless armed. */
void lsw2_partapply_tick();

#endif /* LSW2_PARTAPPLY_H */
