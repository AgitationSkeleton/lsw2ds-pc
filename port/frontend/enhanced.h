/* The Enhanced control set's movement half: its one byte.
 *
 * The Enhanced control set gives non-tank arena movement. It defaults on, and
 * a client's Off overrides a host's Enhanced.
 *
 * RETRACTED - the byte below is the wrong byte for the arena. The
 * arena's tank controls are its per-station camera MODE (9, set by the arena's
 * script into kLsw2LocalCamera + 0x208), which 0x02007420 diverts on before the
 * view index is ever consulted; the view index reads 0/1 there, never 4, so the
 * rule below never fires in a match - the measurement quoted below said so and
 * was read as "nothing to correct". What the Enhanced set does now is in enhanced.cpp: the
 * game's own mode 4 (camera-relative movement, an offset-and-yaw camera),
 * switched through the game's own replicated setter so every console applies it
 * at one dataset, with the stick steering that camera's yaw and height and the
 * arena camera's wall pull run for it. Measured on two consoles:
 * movement follows the orbited camera, positions agree, no bus faults. The
 * view-index rule below is kept for an actor the arena has not seated; the
 * rest of this comment is the history of how the wrong byte was chosen.
 *
 * WHY THIS IS ONE BYTE AND NOT A CONTROL SCHEME. Movement
 * intent is `requested heading + view yaw offset`, and the offset comes from
 * `0x0204A5BC`, which REFUSES to compute one when the actor's view index -
 * `actor+0x5DA` - reads 4, writing zero instead. A zero offset is world-absolute
 * movement, which is the feel called tank controls here.
 *
 * WHAT THE ARENA ACTUALLY ASSIGNS, measured rather than assumed.
 * tools/reconstruct_ram.py rebuilt main RAM from
 * out/work/archive/gpudump_3matches - three recorded matches - and read
 * `actor+0x5DA` for each party slot at 0x0210930C + 0x4C + slot*4:
 *
 *     f020000   slot 0 view 0 (character 29, Jango Fett)   slot 1 view 1 (20, Greedo)
 *     f028000   slot 0 view 0 (29)                          slot 1 view 1 (20)
 *     f040000   slot 0 view 0 (34, Lando Jabba's Palace)    slot 1 view 1 (71, Zuckuss)
 *
 * Two different matches with different character pairs, and in every one both
 * players already hold a real view index. Nobody was left at the sentinel 4.
 *
 * That is the benign answer: where the arena seats a player itself, the
 * enhanced scheme is already what they get, and the byte-write has nothing to
 * correct. It does not make the write pointless - a slot the arena has NOT
 * seated is exactly where 4 would survive - but it does mean the common case
 * needs no help, which is worth knowing before treating a missing effect as a
 * bug.
 *
 * So the game contains both schemes and they are one byte apart. The choice
 * was to write the byte and let the guest do the rest - no override, no blast
 * radius, and the project's standing preference: fill in the game's own data and
 * let the game's own code act on it.
 *
 * THE OBJECTION RAISED, AND CLOSED. The byte feeds a second
 * consumer, `0x020AAE08`. For index 0 that function returns `[mgr + 0x208]`
 * either way - the substituted lookup on its last line exists precisely to give
 * a SENTINEL index something sensible to read, and a real index never needs it.
 *
 * WHY THIS SHIPS BEFORE THE MEASUREMENT, when an earlier analysis
 * deliberately did not.
 *
 * That analysis held the write back for two reasons and both have expired:
 *
 *   "matches are not reachable until the match flow is walkable end to end"
 *        - the match flow was built: lobby countdown, round-start message, selector,
 *          seed, match end, score screen, return.
 *
 *   "the measurement comes first" - which value the arena actually assigns.
 *        - THE POLICY BELOW IS CORRECT UNDER BOTH ANSWERS, so it no longer
 *          needs to be known first. If the arena passes 4, the write turns tank
 *          movement into camera-relative movement, which is the feature. If the
 *          arena already assigns a real per-station index, the policy returns
 *          "leave it alone" and nothing changes - which is exactly that analysis's
 *          own other branch: "then movement would already be camera-relative
 *          and there is nothing to change".
 *
 * That is the difference between guessing an unmeasured number and not needing
 * it. The guard is the reading, not an assumption about it.
 *
 * WHAT IS STILL NOT HERE. The right stick. Its settings persist and its UI is
 * real, but pointing a camera needs a yaw write into `0x021092F4[index] + 0x6A`
 * against a camera that recomputes it, and that is a measurement
 * rather than a decision. This file is deliberately only the half that is
 * settled - shipping the movement change alone is a control set that is better
 * than tank, not a promise that is half kept.
 */

#ifndef LSW2_ENHANCED_H
#define LSW2_ENHANCED_H

#include <cstdint>

/* THE SENTINEL: `0x0204A5BC` tests for exactly this and writes a
 * zero offset when it matches. Any other value sends it on to `0x020AAE4C`. */
constexpr int kLsw2ViewSentinel = 4;

/* THE LOCAL CAMERA - `0x020AAEB4` returns it, and the frame's camera update
 * `0x020A0440` runs on it at 0x0205ED88. Its angles are +0x40/+0x42/+0x44 (the
 * yaw is +0x42, published into this console's view record at 0x0205EDC0), and
 * +0x208 is the per-station movement MODE table the arena sets to 9 - see
 * enhanced.cpp, the Enhanced set's movement half in a session. whsession.h knew
 * the same object as the holder of a node table at +0x21C. */
constexpr uint32_t kLsw2LocalCamera = 0x02109C88u;

/* The view a local player borrows. With the split-view bit clear only
 * index 0 gets a yaw at all, and index 0 traces through the second
 * consumer to the same canonical answer. Every station in this fork is its own
 * machine, so the local player IS the first view. */
constexpr int kLsw2ViewLocal = 0;

/* THE PRECEDENCE RULE, which is easy to get backwards and which the
 * requirement states in one direction only: a client turning Enhanced OFF wins over a host
 * that has it on; a client turning it ON does not win over a host that has it
 * off. Both must agree, and the asymmetry is in who can be overruled - it is an
 * AND, and writing it as one is the honest reading rather than a coincidence. */
bool lsw2_enhanced_effective(bool host_allows, bool client_wants);

/* What to write into `actor+0x5DA`, or -1 for "leave the actor alone".
 *
 * `current` is what the byte reads NOW, and it is a parameter rather than an
 * assumption because no single value can be assumed for it. What a SEATED
 * player holds was measured - see the note above the camera
 * constants: view 0 and view 1 across two different matches, never the sentinel
 * 4. That says what the arena assigns; it does not say what this byte holds for
 * an actor the arena has not seated, which is the case this function exists
 * for. So it is still read rather than assumed.
 * `owned` is whether this port put the current value there, which is what makes
 * the undo safe: toggling Enhanced off mid-match restores tank movement, but
 * only to an actor whose index this port changed in the first place. */
int lsw2_enhanced_view_write(bool in_arena, bool enhanced, int current,
                             bool owned);

/* THE ENHANCED CONTROL SET'S CAMERA HALF, unblocked.
 *
 * Every previous pass recorded this as needing "a yaw write through a virtual
 * call the port cannot make". The call is real:
 * `0x020AAE4C` does `BLX [[manager]]` to turn an index into a camera object and
 * then reads `LDRH [r0, #0x6A]`.
 *
 * BUT THE CALL DOES NOTHING THE PORT CANNOT DO. Vtable `0x02100668` slot 0 -
 * `0x020AF718` - is a plain address computation:
 *
 *     MLA r0, r1, r0, r2     with r2 = manager + 0x240, r0 = 0x70
 *     => camera = manager + 0x240 + index * 0x70
 *
 * CORRECTED. The sentence that stood here - "the
 * non-zero-index arm does the identical arithmetic on a station index it
 * fetches first, it is an ARRAY, so the port reaches any camera with two reads"
 * - described one arm and generalised it over three. `0x020AF718` branches on
 * the SIGN of its argument:
 *
 *     index == 0   record 0 outright, the mask never consulted
 *     index <  0   the ACTIVE view, index fetched from the session authority
 *                  at `0x020AF754` - this is the arm that "fetches first"
 *     index >  0   an ORDINAL, resolved at `0x020AF780` by walking the set bits
 *                  of record 0's mask at `+0x66` and taking the (index+1)-th
 *
 * So view 1 means "the second player present", not "record 1", and the two
 * agree only when the mask's bits run contiguously from zero. The array and the
 * stride are real; the subscript was not. `lsw2_viewmask_resolve` in
 * viewmask.h now does the resolution, and this file's reader goes through it.
 *
 * There is also a fifth record at `manager + 0x440` for an ordinal the mask
 * does not reach - and `0x240 + 4*0x70` is `0x400`, so it is NOT the array's
 * fifth element.
 *
 * READ FIRST, WRITE LATER, DELIBERATELY. This exposes only the read. Whether a
 * written yaw survives the camera's own update is the next question and it is
 * cheap to answer once the read is proven - but writing a value derived from
 * arithmetic nobody has yet seen produce a sane number is how this project has
 * gone wrong before. The DIAG line is what proves the address. */
/* THE RECORD ITSELF LIVES IN viewmask.h, along with its field map and the
 * yaw this file turns. It used to be described here too - manager pointer,
 * array, stride and yaw under "camera" names - which made one structure look
 * like two, and left kLsw2CameraArray and kLsw2CameraStride behind as dead
 * constants once the reader below started resolving through
 * lsw2_viewmask_resolve. The camera IS the view. */

/* The view yaw for a camera index, or -1 if the manager is not allocated.
 *
 * THE MANAGER OUTLIVES THE MATCH, and this comment used to say the opposite -
 * "the multi-view bit clear, which is most of the time outside a match". That
 * was a reasonable reading of the guard at 0x0205ED8C and it is not what the
 * recordings show. Sampling 464 frames across every gpudump session and
 * reading the level id ([0x0210930C + 0x68] + 0x479) beside [0x021092F4]:
 * 149 frames sit in the arena LOBBY, level 9, and 31 of those have a LIVE
 * manager. Within a session the pattern is null early and allocated later, so
 * it is created for a match and not torn down on the way back.
 *
 * WHY THAT MATTERED. A -1 from this function was being used as "not in a
 * match" by lsw2_enhanced_camera_step, which is how the rule that the Enhanced
 * set applies to arena gameplay only (the lobby keeps the usual controls) got
 * lost on the camera half while
 * the movement half had it right all along. Both now gate on
 * lsw2_cheats_in_arena_match(). This function stays a plain read - a
 * measurement of the lobby's camera is worth having, and diagnostics.cpp
 * takes it. */
int lsw2_enhanced_camera_yaw(int index);

/* THE ONE-SHOT WRITE PROBE, and why it is a probe rather than the feature.
 *
 * Reading the yaw proves the address. What it does NOT prove is whether a
 * WRITTEN yaw survives - the camera may recompute it from its follow logic every
 * frame, which is an objection raised early and never tested.
 *
 * That is one experiment: write a known offset, read it back on the next frame,
 * report whether it held. Doing it as an opt-in one-shot rather than as
 * continuous control means the answer costs one button press in the same run
 * that proves the read, instead of a second round trip afterwards.
 *
 * REFUSES UNLESS THE READ LOOKS SANE. If the manager is unallocated the read is
 * -1 and there is nothing to write to; writing anyway would be writing into
 * whatever `[0x021092F4] + 0x240` happens to be, which is exactly the mistake
 * the read-first discipline exists to prevent. */
void lsw2_enhanced_probe_camera();

/* Called once a frame from the fork's tick. Silent in the baseline build and
 * outside an arena. */
/* The Enhanced control set's camera step: apply the right stick's accumulated yaw delta.
 * Called from the fork tick, after the guest frame - see the comment on the
 * definition for why that timing is what makes the write survive. */
void lsw2_enhanced_camera_step();

/* THE HEADING THE ORBIT CAMERA LOOKS ALONG, for `actor`: true, with the heading
 * in `*heading`, only while this console's own station is in the Enhanced
 * orbit (mode 4, framed as ours) and that camera follows `actor`. It is the
 * camera's yaw + 0x8000 - the inverse of frame_orbit, which places the camera
 * at heading + 0x8000, behind the player. For the radar (radarview.h), which
 * the game turns by the actor's heading because its own cameras always look
 * along it. */
bool lsw2_enhanced_orbit_heading(uint32_t actor, uint16_t* heading);

void lsw2_enhanced_tick();

/* THERE IS DELIBERATELY NO lsw2_enhanced_last_write().
 *
 * One was written - "what the tick last did, for LSW2_DIAG" - and nothing ever
 * called it, which is this project's most-repeated defect and was caught here in
 * code minutes old. Wiring it would have been the wrong fix as well as the easy
 * one: LSW2_DIAG already reports view_index by READING THE GUEST BYTE, which is
 * the truth. A port-side echo of what the port MEANT to write is a second source
 * for one fact, and the only interesting case is the one where the two disagree -
 * exactly the case an echo cannot report.
 */

#endif /* LSW2_ENHANCED_H */
