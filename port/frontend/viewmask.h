/* The one value everything about "how many players are in this session" is
 * derived from.
 *
 * WHY THIS FILE REPLACES A GUESS. Three separate port modules have written a
 * player count into the guest - `netcount.cpp` into `[0x021027F8+0x29]`, and
 * two abandoned attempts at `[0x020FEB88]` and `[0x02104D2C+6]`. All three are
 * DOWNSTREAM. The game computes every one of them, itself, from a single 16-bit
 * bitmask, and a port write to any of them is erased the next time the game
 * recomputes. That is why raising a count never turned the message layer on.
 *
 * THE DERIVATION, read out of the ROM rather than inferred:
 *
 *   0x020B0BDC   the refresh. Runs from the main-loop chain
 *                (0x02000BC0 -> 0x020013A4 -> 0x020B07E8 -> here), guarded only
 *                by a null check on the manager:
 *
 *                  020B0C08  r0 = [0x0210B348]        the session authority
 *                  020B0C10  r1 = [[r0] + 0x28]       its vtable slot 10
 *                  020B0C14  BLX r1                   -> THE MASK
 *                  020B0C1C  STRH r0, [r4 + 0x66]     into the view record
 *
 *   0x02071B9C   the derivation, from a state vtable (0x020FEB1C), so it runs
 *                while that state is active:
 *
 *                  020720D0  r9 = LDRH [record + 0x66]      the mask
 *                  020720E0  STRH r9, [0x02104D2C + 2]      cached
 *                  020720F4  TST r9, 1 << r8                per slot...
 *                  020720FC  ADDNE r7, r7, #1               ...popcount
 *                  02072110  STRB r7, [0x02104D2C + 6]      THE PLAYER COUNT
 *                  02072118  CMP r7, [r6 + 0xAC]            against the expected
 *                  0207211C  BNE  ...                       mismatch: stop here
 *                  02072128  BL 0x0205CE5C   -> [0x021027F8+0x29] = count
 *                  02072134  BL 0x02073054   -> [0x020FEB88]      = count
 *                                                (+ camera reconfiguration)
 *
 * So the byte `netcount.cpp` writes and the word the receive pump reads are the
 * SAME NUMBER, popcount of the same mask, written by the same function four
 * instructions apart. Neither is a lever. The mask is.
 *
 * WHAT THAT MEANS FOR THE FORK. The honest seam is the session authority's
 * vtable slot 10 - the call that returns the mask - because the port is what
 * replaces the wireless hardware that answers it. Overriding a service the port
 * already stands in for is not the same kind of act as poking a derived byte,
 * and it is the only one of the two the game will not immediately undo.
 *
 * THIS FILE DOES NOT DO THAT YET. It reads the mask and reports it, so that one
 * play session establishes what the arena actually presents before anything is
 * written. Measure first, then write.
 */

#ifndef LSW2_VIEWMASK_H
#define LSW2_VIEWMASK_H

#include <cstdint>

/* The view manager, and the record array inside it.
 *
 * FOUR RECORDS, then a FIFTH that is not part of the array. `0x020AF7D0` ends
 * the resolver with `ADDEQ r0, r10, #0x440` when the ordinal is not found - and
 * `0x240 + 4*0x70` is `0x400`, not `0x440`. The fallback is a distinct record
 * sitting past the array, not its fifth element, and computing it with the
 * stride would land 0x40 short of it. */
constexpr uint32_t kLsw2ViewManagerPtr = 0x021092F4u;
constexpr uint32_t kLsw2ViewArray = 0x240u;
constexpr uint32_t kLsw2ViewStride = 0x70u;
constexpr uint32_t kLsw2ViewFallback = 0x440u;
constexpr uint32_t kLsw2ViewMask = 0x66u;   /* the bitmask, in record 0 */

/* THE CROSS-PLAYER CONSISTENCY CHECK'S GATE, on the same object.
 *
 * `FUN_020af330` - the netgame step - can only report the failure that becomes
 * "Communication Error" while this byte is set. It compares each peer's
 * reported {sequence, checksum} against a local ring, where the sequence is a
 * byte of the shared random generator and the checksum is the sum of the
 * players' positions.
 *
 * WHY IT IS WRITTEN FROM HERE, and the ROM's own discipline for it. Every
 * cartridge site that DRAWS from the shared generator disarms as it draws -
 * `FUN_020b0cd4` is the clearest: draw, broadcast the new seed, then
 * `vtable+0x18(netgame, 0)`. It must, because a draw moves that console off the
 * seed the peer still holds, so the comparison is meaningless until the
 * broadcast lands and both re-seed. The re-arm is equally disciplined:
 * `FUN_02098b6c` re-seeds and arms in the same breath.
 *
 * The one arming site that does NOT seed is `FUN_0208c888`, reached on a level
 * load, and a write watch caught exactly that firing as the cantina finished
 * loading. In the retail flow that is harmless, because the route
 * from a level to the Game Settings screen goes through a message whose sender
 * disarms. The fork's route does not: its lobby IS a loaded level, and the
 * cantina console entity opens Game Settings by writing the state-request
 * record directly - which is why `request 54` never appears in a state log.
 *
 * So the check arrives at Start Game armed, the press draws without disarming,
 * and the window between the draw and the re-seed is fatal. Clearing the byte
 * as Game Settings opens restores the ROM's own invariant rather than inventing
 * one. */
constexpr uint32_t kLsw2NetCheckGate = 0x6F4u;
constexpr int kLsw2ViewRecords = 4;

/* WHAT IS ACTUALLY IN A RECORD, read rather than reasoned about. This map, and
 * the yaw below it, came from enhanced.h - where they sat under the name
 * "camera" beside a SECOND copy of the manager pointer, the array offset and
 * the stride. One record, described in two headers, and the camera copy had
 * quietly gone dead: enhanced.cpp resolves through lsw2_viewmask_resolve now,
 * so kLsw2CameraArray and kLsw2CameraStride had no consumer left while still
 * asserting a layout. A reader correcting the stride there would have changed
 * nothing at all.
 *
 * tools/reconstruct_ram.py rebuilds main RAM from the recorded sessions, so
 * this structure can be read without a live run. Across fourteen consecutive
 * captured frames of a walk through Jabba's Tower (gpudump_walk_jabba, frames
 * 11400..12180) and again in the arena capture, with the manager resolving to
 * 0x021DDD18:
 *
 *     +0x00 .. +0x5F   ZERO in every frame, all 0x60 bytes of it
 *     +0x60  u32       varies widely; not angle-shaped
 *     +0x64  u16       small power-of-two values - 0, 1, 16, 32, 64 - flags
 *     +0x66  u16       constant 3: the view mask, two players seated
 *     +0x68  u16       byte-range, changes irregularly and holds for stretches
 *     +0x6A  u16       THE YAW: ranges over a full circle
 *     +0x6C  u16       constant 0x100
 *     +0x6E  u16       steps by exactly 30 per captured frame - a counter
 *
 * That +0x66 is the same halfword this file's kLsw2ViewMask names, which is the
 * cleanest evidence the "camera record" and the "view record" were always one
 * structure: the mask the session count is derived from and the yaw the
 * enhanced controls turn are eight bytes apart in it.
 *
 * TWO THINGS THAT FOLLOW. Records 0 and 1 are populated and 2 and 3 are all
 * zero, which independently confirms a two-player capture - the HUD sprites
 * say the same, since only two portrait slots carry the occupied overlay.
 *
 * And there is no pitch here. That answers whether the enhanced controls can
 * offer an inverted up/down by measurement instead of by an unfinished search: only +0x6A ranges like an
 * angle, and the one field that cannot be dismissed on shape alone - +0x68's
 * low half - held IDENTICAL values in both view records of the arena capture,
 * which a per-view pitch would not. It does not prove the game has no pitch
 * anywhere; it proves the structure this port writes a yaw into has one angle
 * in it, so an invert-Y control has nothing to write. */
constexpr uint32_t kLsw2ViewYaw = 0x6Au;

/* Not found. `lsw2_viewmask_resolve` returns this for an ordinal the mask does
 * not reach, which the game answers with the fallback record. */
constexpr int kLsw2ViewNotFound = -1;

/* THE GAME'S OWN INDEX SEMANTICS, which are not array subscripting.
 * `0x020AF718` branches three ways on the sign of its argument:
 *
 *   index == 0   record 0, with no reference to the mask at all
 *   index <  0   "the active view" - the index comes from the session
 *                authority's vtable slot 11 (`0x020AF754`)
 *   index >  0   an ORDINAL: walk the bits of record 0's mask and take the
 *                (index+1)-th set one (`0x020AF780`-`0x020AF7CC`)
 *
 * So view 1 is "the second player present", not "record 1". They coincide only
 * when the set bits run contiguously from bit 0, which is why treating the
 * argument as a subscript looks right in a full four-player session and is
 * wrong in every session with a gap. */
int lsw2_viewmask_resolve(unsigned mask, int index);

/* Popcount as the game does it: bits `0 .. slots-1` only. The bound is
 * `[0x020FEB88 + 4]`, re-read every iteration at `0x02072100`. */
int lsw2_viewmask_count(unsigned mask, int slots);

/* The address of a resolved record, or the fallback for `kLsw2ViewNotFound`. */
uint32_t lsw2_viewmask_record(uint32_t manager, int resolved);

/* THE FIELD THE MASK ACTUALLY LIVES IN, and the layer that owns it.
 *
 * The view record's `+0x66` is itself a copy. `0x020B0C14` asks the session
 * authority for the mask through vtable slot 10, and that method is three
 * instructions:
 *
 *     020AD8CC  LDR  r0, =0x0210B320
 *     020AD8D0  LDRH r0, [r0, #0xE]      <- the mask, a plain field
 *     020AD8D4  BX   lr
 *
 * Slot 11 is the same shape over `+0x8`, the active view index. So the
 * authority is a STATIC OBJECT at `0x0210B320` and the mask is one halfword.
 *
 * WHO MAINTAINS IT. Seven writers, every one of them in the wireless connection
 * module, and two of them are a matched join/leave pair driven by an event
 * struct:
 *
 *     020AEAA8  LDRH r3, [obj + 0xE]        join
 *     020AEAAC  LDRH r1, [event + 0xA]      the station's slot
 *     020AEAB0  ORR  r1, r3, r2, LSL r1     set that station's bit
 *     020AEAB4  STRH r1, [obj + 0xE]
 *
 *     020AEAE8  LDRH r3, [obj + 0xE]        leave
 *     020AEAEC  MVN  r1, r2, LSL r1         clear that station's bit
 *     020AEAF4  STRH r1, [obj + 0xE]
 *
 * WHY THE FORK MAY WRITE IT, when it may not write the counts. This field
 * belongs to the DS wireless connection layer, and on a PC that layer is the
 * port - there is no radio, the handlers above never run, and nothing else in
 * the game writes this halfword. Setting a peer's bit here is the port doing
 * the job of the hardware it replaces, using the same slot numbering; every
 * value downstream is then computed by the game itself, correctly, including
 * the camera reconfiguration a direct write to `[0x020FEB88]` would have
 * skipped.
 *
 * That is the distinction that separates a fix from a patch-over: not "is it a
 * write" but "is it this layer's value to produce". The counts are the game's
 * conclusions. The mask is the device's report. */
/* THE WHOLE CHAIN, OBSERVED IN A NATIVE TWO-PLAYER SESSION.
 *
 * The argument above - the mask is the device's report, the counts are the
 * game's conclusions - was reasoning from disassembly. It can now be checked,
 * because tools/reconstruct_ram.py rebuilds main RAM from the recorded
 * sessions and gpudump_arena has BOTH machines: inst0 and inst1, two DS
 * instances in one wireless arena match.
 *
 * Reading the last captured frame of each:
 *
 *                                        inst0     inst1
 *     [0x0210B32E]  the device mask      0x0003    0x0003
 *     view   +0x66  the view mask             3         3
 *     [0x020FEB88]  the player count          2         2
 *     [0x020FEB8C]  the slot bound            4         4
 *
 * THAT THIRD ROW USED TO SAY "the game bound  2", AND IT WAS WRONG TWICE. The
 * word at +0 is the COUNT - the popcount 0x02072110 stores - and the bound is
 * the separate word at +4, which reads 4 and not 2. The mislabel had been sat
 * next to this file's own line 236, which puts the bound at `[0x020FEB88 + 4]`
 * correctly, for as long as both have existed. It was found by collapsing the
 * duplicate of this address in selectskip.cpp, which is the argument for the
 * whole duplicate-collapsing exercise in one example: reading two copies of a
 * fact side by side is what makes a contradiction visible.
 *
 * WHY THE WRONG VALUE MATTERED. "Bound 2" reads as "the bound counts the
 * players", so a fork session reaching bound 4 - which is what a CORRECT
 * session looks like - would have been read as broken. The bound is the slot
 * CAPACITY the popcount walks, and it is 4 in a two-player match.
 *
 * AND THE SEQUENCE ANSWERS THE OPEN QUESTION BELOW. Walking inst0 across the
 * session join, one reconstructed frame every 60:
 *
 *     frame 4500   mask 0x1   count -1   bound -1
 *     frame 4620   mask 0x1   count -1   bound  4     <- the bound, FIRST
 *     frame 4740   mask 0x3   count  2   bound  4     <- then mask, then count
 *
 * The bound is established while the mask still says one station and the count
 * is still -1, so it is NOT derived from the mask - it arrives ahead of it, on
 * the wireless path (message type 0x13 carries count and bound, bytes 4 and
 * 5). And the idle value is -1, not 0, which is still <= 0 and so still takes
 * the BLE that skips the popcount entirely.
 *
 * SO THE FORK MUST PUBLISH THE BOUND AS WELL AS THE MASK, and the answer is
 * measured rather than guessed: a mask written into a session whose bound is
 * still -1 produces a player count of zero, because the game never runs the
 * popcount. Publish 4 - the capacity - not the number of peers.
 *
 * ONE MORE FIELD, and it moves. The halfword at 0x0210B328 is 0x0000 on inst0
 * throughout, and on inst1 it is 0x0000 until the session forms and 0x0001
 * from the frame the mask reaches 0x3 onward. So it is written as the station
 * is seated rather than fixed at startup - but that is what BOTH readings
 * predict, a local station index and a host/client flag alike, so the reading
 * is still not claimed. What is now known is that it is not static. */
constexpr uint32_t kLsw2SessionAuthority = 0x0210B320u;
constexpr uint32_t kLsw2AuthorityMask = 0x0Eu;
constexpr uint32_t kLsw2AuthorityActiveView = 0x08u;

/* TWO MORE FIELDS OF THE SAME OBJECT, moved here so the wireless context has
 * one home. dispatch_overrides.cpp declared the base TWICE on its own - as
 * kWhContext near the top and kWhCtx some 780 lines later - and the 0x20
 * state offset twice with it, as kWhStateField and kWhCtxState. One file,
 * two spellings, far enough apart that a correction to one would never meet
 * the other.
 *
 * The values are pinned by the name table at 0x020FFD20: the state
 * is a WH_SYSSTATE_*, and 9 is ERROR, which is why the error code is only
 * written when the state is not already 9. */
constexpr uint32_t kLsw2WhSystemState = 0x20u;
constexpr uint32_t kLsw2WhErrorCode = 0x30u;

/* THE STEP SIGNAL - the field that says WHICH WH OPERATION LAST COMPLETED, and
 * the reason the wireless layer can be read as a story rather than guessed at.
 *
 * Every WH handler ends by stamping its own constant here: the data-sharing
 * entry writes 0x20, the connect callback 0x23, the event switch 0x2d, the
 * reset path 0x1c, and so on for about thirty ids. That alone would make it a
 * useful breadcrumb. What makes it the CENTRE of the layer is `FUN_020addf0`,
 * WH's own wait helper:
 *
 *     for (n = ctx[0x14]; ctx[0x48] != wanted && ctx[0x30] == 0 && n > 0; n--)
 *         yield();
 *
 * WH issues an asynchronous WM call, goes BUSY, and then spins HERE until the
 * completion callback stamps the id it is waiting for - or until the error
 * field moves, or the count runs out. So this is not decoration the cartridge
 * left behind; it is the handshake the whole state machine is built on.
 *
 * That matters for reading a stall. WH_SYSSTATE_BUSY on its own says only
 * "something is in flight", which is why the host sitting in BUSY for two
 * thousand log lines after the match-start teardown never named a culprit.
 * This field names the last step that actually finished, on each console
 * separately - and the two consoles do NOT agree, which a single state value
 * could never have shown. */
constexpr uint32_t kLsw2WhStepSignal = 0x48u;

/* Two more WH objects, ABSOLUTE, each named once here because three
 * files use them:
 *   - the shared dataset WH_StepDS fills and WH_GetSharedDataAdr points into
 *     (sDataSet; the host-level WH lays it out as AID * 124);
 *   - the WMBssDesc WH_ChildConnect copies the chosen host's record to, which
 *     the scan also fills while browsing. */
constexpr uint32_t kLsw2WhSharedData = 0x0210B520u;
constexpr uint32_t kLsw2WhBssDesc = 0x0210B460u;

/* The mask a session implies, from the stations the transport actually handed
 * out - never from the player count. A roster of stations 0 and 2 is mask 0x5,
 * and a count-derived 0x3 would seat a player who is not there while leaving a
 * player who is unseated. `peer_stations` may contain -1 for a row that has not
 * reported one, which is skipped. */
unsigned lsw2_viewmask_compose(int local_station, const int* peer_stations,
                               int peer_count);

/* --- the guest side, which needs the bus ------------------------------------ */

/* THE BOUND, which differs from the mask and can defeat it.
 *
 * The game's popcount at `0x020720E4` asks `0x02073044` - `[0x020FEB88 + 4]` -
 * for how many slots to walk, and:
 *
 *     020720E4  BL   0x02073044
 *     020720E8  CMP  r0, #0
 *     020720EC  BLE  0x0207210C      <- SKIPS THE POPCOUNT ENTIRELY
 *
 * so a bound of zero stores a player count of zero **no matter what the mask
 * says**. Publishing the mask is necessary and may not be sufficient.
 *
 * The cartridge sets this word from a received message: type 0x13 carries the
 * count in byte 4 and the bound in byte 5, handing them to `0x02073054` and
 * `0x020730D0`. So on real hardware the host tells the clients both.
 *
 * ANSWERED, and the answer is yes. This used to end "whether the
 * fork has to set it as well is exactly what one session shows, and writing it
 * first would be guessing" - one session has now been read out of the arena
 * capture, and the measurement is in the reference-values block above. The
 * idle value is -1 (which the BLE above skips on, exactly as feared), the
 * bound is set to 4 BEFORE the mask reaches its final value, and it is not
 * derived from the mask at all. The fork must publish the capacity.
 *
 * STILL REPORTED, NOT WRITTEN, because the publish belongs next to the mask's
 * publish and lsw2_viewmask_publish is where that goes - the question this
 * comment existed to hold open is what has closed, not the work. */
constexpr uint32_t kLsw2PumpCountWord = 0x020FEB88u;
/* THE OFFSET +4, whose value collides confusingly with the bound's own value:
 * the slot bound LIVES at +4 and READS 4 in a real match. This constant is the
 * offset. kLsw2PumpBoundValue below is the capacity to publish. */
constexpr uint32_t kLsw2PumpBound = 4u;      /* +4 is the slot bound */

/* THE CAPACITY THE CARTRIDGE PUBLISHES, measured at 4 on both machines of the
 * arena capture and constant from the moment the session forms. Four is also
 * kLsw2ViewRecords, and that is not a coincidence - the popcount walks bits
 * 0..bound-1 of a mask whose bits are the view records. */
constexpr int kLsw2PumpBoundValue = 4;

/* The game's own slot bound, or -1 when unreachable. */
int lsw2_viewmask_bound();

/* The live mask, or -1 when the manager is not allocated. */
int lsw2_viewmask_guest();

/* Once a frame from the fork's tick. Reports the mask when it changes, and
 * nothing otherwise. This is a MEASUREMENT, not a lever - see the header. */
void lsw2_viewmask_tick();

/* Put the fork session's mask where the cartridge's connection handler would
 * have put it, and take it back when the session ends. Does nothing offline,
 * nothing in a session of one, and nothing outside the fork build. */
void lsw2_viewmask_publish();

#endif /* LSW2_VIEWMASK_H */
