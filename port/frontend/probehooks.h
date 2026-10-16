/* WHICH GUEST ADDRESSES ACTUALLY RUN EVERY FRAME - measured, not guessed.
 *
 * WHY THIS EXISTS, and it is an admission. Re-applying customization after a
 * character switch needs a per-frame hook on the guest's thread to run the gear
 * applier from (it was measured that nothing else can work: during a host
 * guest-call the machine's event queue is EMPTY, so the interrupt the applier
 * waits for can never be raised).
 * Finding that hook has now cost SIX candidates across three iterations, every
 * one of them picked by reading code and every one refuted by measurement:
 *
 *   script_command 0x02055E98      fires, rare
 *   state_message  0x0208CFEC      fires, rare
 *   flag_test_array_c 0x02086204   fires, rare
 *   0x0208CF08  gameplay +0x10     NEVER FIRES
 *   0x020877A0  state 48 +0x10     NEVER FIRES
 *   0x02088014  state 48 +0x14     NEVER FIRES
 *
 * The last of those also breaks the rule inferred from the first five, that
 * "slot +0x14 dispatches and +0x10 does not", generalised from
 * state_message. State 48's own +0x14 does not dispatch, so the rule is wrong:
 * 0x0208CFEC dispatches for some reason particular to IT, not to its slot.
 *
 * SIX GUESSES IS ENOUGH. This asks the machine instead. The framework's branch
 * ring already records what the guest actually executes, and whsession.cpp uses
 * it twice; this arms it for a couple of frames of ordinary play and histograms
 * what comes back. Addresses that appear once per frame, every frame, are the
 * candidate hooks - and unlike a reading of the code, the list is complete.
 *
 * OFF UNLESS LSW2_PROBE_FRAMEPCS=1. Deep trace records both cores and
 * runtime_arm.h warns it stops the run if left on, so the window is a fixed
 * handful of frames and it disarms itself.
 */

#ifndef LSW2_PROBEHOOKS_H
#define LSW2_PROBEHOOKS_H

/* Call once per fork tick. Inert unless the switch is set; arms once the lobby
 * is up and settled, dumps a histogram a few frames later, then never again. */
void lsw2_probe_frame_pcs();

/* COUNT HOW OFTEN A HOOK FIRES, off unless LSW2_PROBE_HOOKRATE=1.
 *
 * Call it at the top of any override with a literal tag. It prints once to say
 * it is live - so its SILENCE IS NEVER AMBIGUOUS, which is a rule this project
 * paid for: a conclusion was published from this counter before it had ever
 * printed a line, and "no hook fires often" and "the probe was never switched
 * on" look identical in a log. It then prints the first call for each tag, and
 * a running count every 60 calls, which is about once a second at 60fps.
 *
 * IT LIVES HERE RATHER THAN IN dispatch_overrides.cpp because headfix.cpp needs
 * it too - the gear applier's override is in that file, and whether IT fires on
 * a character switch is the question re-applying customization now turns on. */
void lsw2_probe_hook_rate(const char* tag);

#endif /* LSW2_PROBEHOOKS_H */
