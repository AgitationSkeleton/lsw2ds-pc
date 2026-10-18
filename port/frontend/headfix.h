/* THE HEAD-REVEAL ENHANCEMENT - the Enhancement that stops a customize hat
 * hiding the head.
 *
 * THE DEFECT: some characters whose heads are hidden by their own headgear,
 * although a head exists, do not show it when a customize hat is applied -
 * namely Jango Fett and Boba Fett, whose helmets are swapped for the chosen hat
 * over an invisible head. The fix is an Enhancement setting, offered by the
 * base recomp and by the Bounty Hunter fork alike.
 *
 * THE AFFECTED SET IS NINE CHARACTERS, NOT TWO. The property that
 * matters is "this character's STOCK hat covers the head" - `headgear + 0x28`,
 * a flag on 9 of the 55 hats - and nine characters have one: **7 (Boba), 11,
 * 15, 29 (Jango), 60, 61, 63, 64, 65**. Boba and Jango are the two worth playing
 * in the arena, which is why they are the two the defect was noticed on.
 *
 * Descriptor byte 4 (the class) selects **exactly** Boba and Jango out of 79 and
 * nothing else, so keying this to `class == 9` would have matched the two
 * noticed characters perfectly and left SEVEN characters broken - 11, 15, 60, 61, 63, 64 and 65,
 * which are classes 2, 2, 6, 6, 3, 3 and 3. `config/head_coverage.json` carries
 * the derived set so the predicate is data and not a guess.
 *
 * RE-DERIVED FROM THE CARTRIDGE, not taken from an earlier derivation a second
 * time. Reading
 * `out/raw/arm9_section_0_02000000.bin` directly:
 *
 *   - the headgear table holds 55 entries and every `+0x28` in 177..231 is 0 or
 *     1, so it really is a flag and not a field being read too narrowly;
 *   - exactly nine are 1: parts 180, 189, 193, 201, 222, 224, 225, 226, 227;
 *   - walking the stock table for characters wearing one gives 7, 11, 15, 29,
 *     60, 61, 63, 64, 65 - the set below, with the same hat for each;
 *   - `0200668C` is `E5D40028`, which is `LDRB r0,[r4,#0x28]`, and `020066B4` is
 *     `EBFFFF29`, which branches to `0x02006360`;
 *   - `0x02006360` is `MOV r1,#1; STRB r1,[r0,#0x6F3]; BX lr` - three
 *     instructions, and the only thing it does is set the flag;
 *   - `0x0200636C` reads it back with `LDRB r0,[r4,#0x6F3]`.
 *
 * Part 226 is a covering hat that no character wears as stock, which is why the
 * nine hats and the nine characters are nine by coincidence rather than by
 * pairing: character 60 and character 64 both wear hat 225.
 *
 * THE MECHANISM (corrected - see the sub-part-list note further down
 * for the full derivation). The head is one SUB-PART of the character's
 * MAIN MODEL, always modelled; a bitmask decides whether it is drawn. When a
 * character's stock hat COVERS, the build clears the head's visibility bit and
 * draws the helmet in its place - so for the nine, the head is present but
 * hidden. Swapping to a non-covering customize hat removes the helmet but leaves
 * that bit clear, so nothing is drawn where the head should be: a bare neck.
 *
 * WHAT THIS DOES. After the gear applier runs, if the enhancement is on and a
 * non-covering hat was just applied to one of the nine, it SETS THE HEAD
 * SUB-PART'S VISIBILITY BIT in the main model - the same bit the game's own head
 * builder (FUN_0201c858) sets when it shows a head. The head model is already
 * there, so this reveals it. (An earlier version set a re-attach flag at +0x6F3
 * and called 0x0200636C; that reached a different subsystem and never touched
 * what is drawn - superseded, see below.)
 *
 * BOTH CALL SITES REACH IT. The applier override handles a Mix N Match hat; the
 * gear-hop completion (gearapply.cpp/gearhop.cpp) handles the fork re-applying a
 * saved hat on a character switch, which never goes through the override. So the
 * head is revealed however the non-covering hat arrives, in BOTH builds -
 * `0x020063D4`'s dispatching callers include the baseline's customize panel. */

#ifndef LSW2_HEADFIX_H
#define LSW2_HEADFIX_H

#include <cstdint>

/* WHERE THE HEAD ACTUALLY LIVES, corrected by disassembly of the
 * applier and a live measurement. The head is one SUB-PART of the MAIN MODEL, not
 * a separately-attached node. The applier stores the main model behind
 * `actor + 0xE0`, and its sub-part id/visibility list begins at +0x14 of that:
 * u16[0] is a visibility bitmask (bit N = sub-part N is drawn), u16[1..] are the
 * sub-part model ids. Sub-part 0 is the head - measured to hold model 0x0E31 for
 * Boba/Jango whether the helmet OR a hat is worn - so the head geometry is
 * present the whole time; only bit 0 of the bitmask decides if it is drawn. The
 * covering-helmet build clears bit 0 (the helmet is drawn there instead), and
 * swapping to a non-covering hat removes the helmet but never re-shows the head.
 * Setting bit 0 is exactly what the game's own head builder (FUN_0201c858) does.
 *
 * THE OLD APPROACH WAS THE WRONG SUBSYSTEM. It set a re-attach flag at +0x6F3
 * and called 0x0200636C, which builds DAT_020063d0[actor+0x180] and attaches via
 * the actor+0xD4 vtable - a bespoke-minifig-head path whose model array does not
 * even contain 0x0E31, so it never showed these heads (its earlier "verified"
 * result was only a one-frame animation shift). Superseded by setting bit 0. */
/* The re-attach flag the game's own covering path sets (STRB r1,[r0,#0x6F3] at
 * 0x02006364). The reveal does NOT use it - the head is a main-model sub-part,
 * not the +0x6F3/0x0200636C subsystem - but it is a real ROM fact this header
 * describes, kept pinned (rom_constants). The main-model offsets (actor+0xE0,
 * +0x14) are inline literals in the .cpp: their tiny values collide with
 * unrelated offsets and the dup-constants baseline is kept minimal. */
constexpr uint32_t kLsw2HeadReattach = 0x6F3u;

/* headgear descriptors: 0x020F2B1C, stride 0x2C, indexed by part - 177.
 * `+0x28` is the "this hat covers the head" flag. */
constexpr uint32_t kLsw2HeadgearTable = 0x020F2B1Cu;
constexpr uint32_t kLsw2HeadgearStride = 0x2Cu;
constexpr uint32_t kLsw2HeadgearFirst = 177u;
constexpr uint32_t kLsw2HeadgearLast = 231u;
constexpr uint32_t kLsw2HeadgearCovers = 0x28u;

/* The stock part table - five bytes per character, index 3 is the headgear. */
constexpr uint32_t kLsw2StockParts = 0x020EEBB8u;
constexpr uint32_t kLsw2StockStride = 5u;
constexpr uint32_t kLsw2StockHeadgear = 3u;



/* The correction itself: call after the gear applier has run with `part`
 * applied to `actor`. Does nothing unless the enhancement is on, `part` is a
 * non-covering hat, and `actor` is one of the nine.
 *
 * IT IS SEPARATE FROM THE OVERRIDE ON PURPOSE. `lsw2_guest_call` invokes the
 * LIFTED FUNCTION by symbol, which does not go through the dispatch table and
 * therefore does not run the override - so the fork's own dressing in
 * `gearapply.cpp` would apply a customize hat and skip this correction, which
 * is precisely the case the defect was observed in. Both paths call this. */
void lsw2_headfix_after_apply(uint32_t actor, uint32_t part);

/* The override registered on 0x020063D4, for the game's own applications -
 * the baseline's customize panel among them. Delegates to the lifted applier
 * and then calls the correction above. */
void lsw2_headfix_gear_applier();

/* Per-frame reveal, called from the fork tick. Reveals the head sub-part of one
 * of the nine covered-head characters whenever it wears non-covering headwear -
 * the robust counterpart to lsw2_headfix_after_apply, catching the first apply
 * (whose timing after_apply can miss) and any switch. A no-op otherwise. */
void lsw2_headfix_tick();

#endif /* LSW2_HEADFIX_H */
