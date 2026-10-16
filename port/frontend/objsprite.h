/* Decoding the DS object attribute table.
 *
 * The arena HUD needs the touch screen's HUD sprites WITHOUT its background:
 * the starfield should not be visible behind the other three players' scores.
 *
 * Removing it is not difficult once the pieces are separated, because they were never
 * mixed. `config/bounty_hud_layout.json` records the in-match bottom screen as
 * **nineteen hardware sprites** over background layers - the portraits, the
 * counters and the radar are OBJ; the starfield is BG. Reading the sprites and
 * not the backgrounds removes the starfield by construction. No colour-keying,
 * no masking, nothing to tune.
 *
 * That correction also settles a second thing the previous implementation had
 * wrong. It cropped the touch FRAMEBUFFER, which is BG and OBJ already merged -
 * so the starfield was not an accident of the crop rectangle, it was
 * unavoidable that way round.
 *
 * WHY A DECODER AND A TEST RATHER THAN INLINE BIT-TWIDDLING. Every field here is
 * a bitfield at a hand-counted offset, and this project's record with those is
 * poor: one LDRH mask took three attempts and returned an empty set twice
 * while looking exactly like "there is nothing there". An OAM decode that is
 * subtly wrong does not fail - it draws the wrong tile at the wrong place in the
 * wrong colours, which reads as a rendering bug anywhere but here.
 */

#ifndef LSW2_OBJSPRITE_H
#define LSW2_OBJSPRITE_H

#include <cstdint>

/* One OAM entry, decoded. Fields are named for what they mean rather than for
 * which attribute word they came out of. */
struct Lsw2ObjSprite {
    int x = 0;              /* screen left, already sign-corrected  */
    int y = 0;              /* screen top,  already wrapped          */
    int width = 0;
    int height = 0;
    int tile = 0;           /* index into OBJ character memory       */
    int palette = 0;        /* 16-colour bank, or 0 in 256-colour    */
    int priority = 0;       /* 0 is nearest                          */
    bool colour256 = false;
    bool disabled = false;  /* off, or an affine-disabled entry      */
    bool affine = false;    /* rotation/scaling in use               */
};

constexpr int kLsw2OamEntries = 128;
constexpr int kLsw2OamEntryBytes = 8;

/* Decode entry `index` from a 1 KiB OAM image. Returns false when `oam` is null
 * or the index is out of range; a disabled sprite still decodes, with
 * `disabled` set, because callers want to skip it rather than mistake it for a
 * missing entry. */
bool lsw2_obj_decode(const uint8_t* oam, int index, Lsw2ObjSprite* out);

/* The OBJ dimension table, exposed because it is the part most worth checking
 * against a reference: shape and size are two bits each and the mapping is not
 * derivable, it is a table in the hardware documentation. */
void lsw2_obj_dimensions(int shape, int size, int* width, int* height);

#endif /* LSW2_OBJSPRITE_H */
