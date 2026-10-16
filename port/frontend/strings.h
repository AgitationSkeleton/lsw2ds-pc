/* The game's string bank, and our replacements for parts of it.
 *
 * WHY THE PORT NEEDS THIS AT ALL. The fork replaces the arena menu with
 * "Host Internet Game" / "Join by IP or Room Code" / "View Master Server", and
 * adds join and disconnect popups naming a player. None of those strings exist
 * in the ROM. The game draws menu text by asking a decoder for a string id, so
 * the cheapest place to supply new text is that decoder.
 *
 * WHY IT REIMPLEMENTS RATHER THAN WRAPS - AND THE REASON IT USED TO GIVE WAS
 * WRONG. This said "there is no way to call the lifted body and post-process
 * its result (the same limit was recorded for the music path)". There is:
 * gearapply.cpp's override sets `g_cpu.R[15]` and calls
 * `lsw2_arm9_FUN_020063d4()` - the game's own applier, complete - and then does
 * its fix-up on the result. lsw2_guest_call is the same capability with a
 * safety net around it, used from the tick in eight places.
 *
 * That music-path note is the same false limit that kept the host settings'
 * Music: RANDOM unbuilt for a long time, so it is worth naming here too: one
 * incorrect sentence, copied into a second file, blocking a second feature.
 *
 * THE REIMPLEMENTATION STILL STANDS, on its own merits rather than on an
 * impossibility. Wrapping the decoder would mean calling it for every id the
 * fork does not replace - thousands of calls a frame through the guest-call
 * path, to get back a pointer the port would then have to read out of guest
 * memory anyway. Decoding the bank directly is cheaper and is already
 * validated. That is a design choice now, not a constraint, and if a future
 * override needs the guest's own answer it can have it. */
/* So an override of the decoder owes the guest a correct answer for every id it
 * does not replace, and that means decoding the bank ourselves.
 *
 * THE ALGORITHM IS NOT GUESSED. config/strings.json's decoder was derived from
 * 0x0205CD20 and self-validated - it reproduces character names that
 * config/menu_screens.json captured independently from framebuffers, and it
 * decoded 1333 strings. This is a port of that validated reference, not a fresh
 * reading of the disassembly.
 */

#ifndef LSW2_STRINGS_H
#define LSW2_STRINGS_H

#include <cstdint>

/* The control block, filled at load. Reads zero in the static image, which is
 * why the exporter needs rebuilt RAM and why the decoder below reports NotReady
 * rather than producing nonsense when it is called too early.
 *
 *   [cb + 0] the offset table, indexed by id * 4
 *   [cb + 4] the compressed data base
 *   [cb + 8] the Huffman tree base
 */
constexpr uint32_t kLsw2StringControlBlock = 0x020FE5C8u;

/* The tree's root, and the boundary between a node and a literal byte. */
constexpr uint16_t kLsw2StringRootNode = 0x100u;

/* The guest's own return values, from 0x0205CD20. */
enum class Lsw2StringResult {
    Ok = 0,
    NotReady = 1,   /* the control block is not filled yet */
    Truncated = 2,  /* the caller's buffer filled before the terminator */
};

/* How the decoder reaches guest memory. A callback rather than a direct bus
 * call so the algorithm can be tested against a synthetic bank with no runner,
 * no ROM and no window - which is the only reason this file has a test at all. */
using Lsw2ReadU8 = uint8_t (*)(uint32_t addr, void* ctx);

/* Decode one string. `out` is always terminated when the result is Ok.
 *
 * `cap` counts the terminator, matching the guest: it compares the write index
 * against its own limit after storing each byte. */
Lsw2StringResult lsw2_string_decode(uint32_t control_block, uint32_t id,
                                    char* out, uint32_t cap,
                                    Lsw2ReadU8 read8, void* ctx);

/* Our replacements. Null when the id is the game's own.
 *
 * Deliberately a small fixed table rather than a map: the ids the fork replaces
 * are known at build time, and a lookup that cannot allocate is a lookup that
 * cannot fail inside a guest call. */
const char* lsw2_string_override(uint32_t id);

/* Install or clear one. Passing null clears. Text is NOT copied - it must
 * outlive the override, so string literals and long-lived buffers only. The
 * player-name join/disconnect popups are the reason this is settable at all: their text
 * is not known until someone joins. */
void lsw2_string_set_override(uint32_t id, const char* text);

/* How many bytes >= 0xF0 the decoder has emitted.
 *
 * NOT A DEFECT IN THIS DECODER - and the first version of this comment said it
 * was, which was wrong and worth correcting rather than quietly deleting.
 *
 * 0xF0 and above are the LEAD BYTE OF A TWO-BYTE GLYPH, and the place that
 * handles it has been found: the token substituter at 0x02066518 does
 * `CMP r0,#0xF0 / ADDCS r6,r6,#1`, stepping an extra byte so it does not read
 * the trail byte as a character. That is a CONSUMER's concern. The decoder's
 * job is to hand back the byte stream, and both bytes come out of the tree
 * correctly - which is why the validated Python reference reproduces all 1333
 * strings without knowing about any of this.
 *
 * AND IT IS ALSO THIS DECODER'S CONCERN, which the note above understated.
 * Re-reading 0x0205CDA0-0x0205CE0C with a fixed disassembler shows the guest
 * ends a string on `byte == 0 AND glyph == 0`, tracking the glyph across the
 * pair - so a zero byte that is the TRAIL of a two-byte glyph does not
 * terminate. This decoder stops at any zero byte and would truncate such a
 * string.
 *
 * It cannot happen in this cartridge: test_strings_real.cpp decoded all 1333
 * strings against the real bank and counted ZERO bytes >= 0xF0. So the
 * implementation is right here by measurement, not by construction - which is a
 * weaker guarantee than it first appeared, and exactly why this counter exists.
 *
 * A nonzero count means a string has appeared whose terminator rule differs
 * from ours, and the decode of it should not be trusted. */
uint64_t lsw2_string_lead_bytes_seen();

#endif /* LSW2_STRINGS_H */
