/* Customization that survives a character switch.
 *
 * The behaviour: customize Greedo with, say, the helmet and the purple
 * lightsaber, switch to Lando Palace or Jango Fett, and the customizations
 * auto-apply to the new character immediately - a frame after the switch, so
 * it is hardly noticeable.
 *
 * The scope: persistent across characters, across level and arena
 * transitions, cleared by the customization menu's own reset button, and **only
 * in the fork**.
 *
 * WHERE THE STATE IS. Five halfword part slots at 0x02107890 + 0xA6
 * through +0xAE, each fed to the gear applier 0x020063D4. `0xFA` is the game's
 * own "no part" sentinel, so the port has an empty value it did not invent.
 *
 * WHAT IS NOT ESTABLISHED, and why this is written to fail visibly rather than
 * silently: that five slots are the WHOLE customization. The same reading found
 * two further part sources - a per-character list and a 64-byte-record loop -
 * and declined to claim the five are everything. If a switch loses something this
 * does not carry, that is the first place to look.
 */

#ifndef LSW2_CUSTOMIZATION_H
#define LSW2_CUSTOMIZATION_H

#include <cstdint>

/* The slots (six - see below), and the sentinel. */
constexpr int kLsw2CustomSlots = 6;

/* WHERE THE CUSTOMIZE SCREEN'S CHOICES LIVE, and the offset of each slot in
 * that block. ONE COPY, because there were three: customization.cpp,
 * partapply.cpp and partslot.cpp each declared their own `kCustomBase` at
 * 0x02107890, and two of them their own copy of the offsets. They agreed, which
 * is the only reason it had not bitten - the same duplicated-constant shape the
 * gear applier's address was collapsed out of. */
constexpr uint32_t kLsw2CustomBase = 0x02107890u;
constexpr uint32_t kLsw2CustomSlotOffsets[kLsw2CustomSlots] =
    {0xA6, 0xA8, 0xAA, 0xAC, 0xAE, 0xB0};

/* SIX, NOT FIVE - a correction to the earlier reading, which recorded five.
 *
 * The five it found are the ones fed to the gear applier 0x020063D4. There is a
 * SIXTH halfword at +0xB0, and it belongs to the same record:
 *
 *   - all three writers of this block write six in one run
 *     (0x02012ED0, 0x02016FA0, 0x02050B60), from six consecutive source bytes;
 *   - both readers read six (0x02001F24, 0x02002BC8);
 *   - it is gated by the same customization-unlocked flag - `+0x4518` bit 15,
 *     through the same accessor 0x020867EC.
 *
 * What made it invisible is that it goes to a DIFFERENT applier, 0x02004404,
 * which switches over nine values (0x11 to 0x19) rather than taking a part id.
 * A survey that started from the gear applier's callers would find five and stop
 * - which is what happened.
 *
 * The consequence of counting five: a player sets six things, the fork remembers
 * five, and the sixth silently reverts on every character switch and every
 * restart. Persistence, reset and the host policy filter all loop over this
 * constant, so widening it fixes all three.
 *
 * `kLsw2CustomNone` (0xFA) is a safe "none" for the sixth too: it is outside the
 * 0x11..0x19 range the applier switches on, so the switch falls through and
 * nothing is applied - which is exactly what none should mean. */

/* WHAT THE HOST ALLOWS, which is the host's decision: the host chooses whether
 * customization is allowed in matches, and if not, customizations are entirely
 * disabled and stock hunters are applied - Greedo with no special hat or custom
 * weapon, for example.
 *
 * The host settings menu has carried a "Customization: <OFF/HATS/ALL>" row
 * since it was written and nothing read it - the same built-and-unconsumed
 * shape as "Visible to LAN" before LAN discovery existed.
 *
 * Kept as its own enum rather than reusing Lsw2HostCustomization so that this
 * file does not depend on the host menu; the caller maps one to the other. */
enum class Lsw2CustomPolicy { Off = 0, Hats, All };

/* Filter a saved part set through a policy. `out` may alias `saved`.
 *
 * HATS IS DECIDED BY PART ID - and the id range now IS the slot.
 *
 * This paragraph used to say that which slot held headwear was unknown, so
 * "hats only" had to be a membership test against eight known ids. That was
 * true of the evidence then. It is now established that the categoriser at
 * 0x0208DA74 partitions the id space BY SLOT, and slot 3 - headwear - is ids
 * 177 to 231, fifty-five parts.
 *
 * So the test is a range, and the eight are what they always were: the hats the
 * Extras modes assign, a subset. Keeping them as the whole set made the host's
 * "Hats" policy strip 47 of the 55 as though they were not hats. */
void lsw2_customization_filter(const uint16_t* saved, uint16_t* out,
                               Lsw2CustomPolicy policy);

/* True when `part` is headwear - slot 3, ids 177..231. */
bool lsw2_customization_is_headwear(uint16_t part);

constexpr uint16_t kLsw2CustomNone = 0x00FAu;

/* THE CUSTOMIZE MENU'S EXACT WEAPON SET, from the ROM (RE 2026-09-18). The
 * FreePlay "Character Customize" (Mix N Match) menu offers 17 weapon rows - its
 * gate-1 row table values - not the nine-wide 0x11..0x19 the earlier version
 * assumed. That narrow range missed every blaster-family gun (the "Classic
 * Blaster" among them), which is why a chosen weapon often did not persist. The
 * 0x11..0x19 block is the coloured-lightsaber cluster (rainbow among them); the
 * rest are the blasters. A weapon NOT in this set is not something the menu can
 * pick, so the fork neither captures nor applies it - it is treated as none and
 * the character's own default weapon stands. */
inline bool lsw2_is_custom_weapon(uint16_t w) {
    switch (w) {
        case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0E: case 0x0F:
        case 0x11: case 0x12: case 0x13: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1D: case 0x1E: case 0x21:
            return true;
        default:
            return false;
    }
}

/* THE CUSTOMIZE MENU'S EXACT HEADGEAR SET, from the ROM (RE 2026-09-18). The
 * menu's slot-3 (headgear) options are exactly these nine ids - its gate-2 row
 * values. These are the ONLY headwear the menu can set; every other headgear id
 * in the 0xB1..0xE7 table is a character-specific part (Boba's helmet, Boushh's
 * helmet, ...). This is the whole fix for "a character's stock helmet/head/body
 * follows to the next character": the fork captures and applies ONLY a headgear
 * in this set - a stock part can never be one, so it is never carried forward. */
inline bool lsw2_is_customize_headgear(uint16_t p) {
    switch (p) {
        case 178: case 187: case 188: case 202: case 205:
        case 208: case 214: case 228: case 229:
            return true;
        default:
            return false;
    }
}

/* Once per frame, before the guest runs. Does nothing unless the build is the
 * fork - the baseline's customization behaves exactly as the cartridge's. */
void lsw2_customization_tick();

/* The remembered set, for the imgui readout and for the settings file. */
const uint16_t* lsw2_customization_saved();

/* Record a customize-panel part choice into the saved set the instant the
 * game's applier is called with it - see the definition. Called from the
 * gear-applier override; a no-op outside the fork, outside the lobby, or for a
 * part that is not a wearable customize part. */
void lsw2_customization_observe_panel_apply(uint32_t part);

/* THE ARENA CUSTOMIZE MENU'S COMMIT (override on 0x0207AEF4). Runs the game's
 * own row-commit and then records ONLY the menu's whitelisted options (slot-3
 * headgear + weapon). The one capture path: fires only on a menu action, so a
 * character's stock is never captured; Reset clears the set naturally. */
void lsw2_customization_menu_commit();

/* Load and store across runs. Persistence between characters and between level
 * and arena game transitions outlives one process. */
/* The set to actually apply: the player's raw choices filtered by what the
 * host allows. `out` takes kLsw2CustomSlots entries - five parts then the
 * weapon - and a slot the host disallowed comes back as kLsw2CustomNone.
 *
 * Exposed for gearapply.cpp, which dresses the actor with the game's own
 * applier rather than through the customize panel's staging halfwords. */
void lsw2_customization_effective(uint16_t* out);

void lsw2_customization_load(const uint16_t* slots);

/* THE TWO OVERLAY EXTRAS the customize menu can toggle - a moustache and
 * sunglasses. They are not part slots: the menu's commit sets bit
 * 0x400 / 0x800 of the actor's flags word and the actor tick reads those bits
 * per frame to show/hide the overlay (RE 2026-09-18). Persisted and saved like
 * the slots, so a chosen moustache follows a character switch. Getters for the
 * settings writer; the setter is the settings loader's way in and does not mark
 * the set dirty. */
bool lsw2_customization_moustache();
bool lsw2_customization_sunglasses();
void lsw2_customization_set_overlays(bool moustache, bool sunglasses);

/* Whether the saved set has changed since it was loaded or last marked clean.
 *
 * THIS EXISTS BECAUSE mark_clean() DID WITHOUT IT. The flag was set on every
 * adoption and cleared on load and on marking, and nothing could ask - so the
 * settings writer stored all five slots and the weapon on every save whether
 * or not the player had touched the customize screen, and called mark_clean()
 * on a flag no decision depended on. A module that advertises change tracking
 * and cannot answer the question is worse than one that never claimed to.
 *
 * Safe to gate a save on, because `g_saved` is written in exactly two places -
 * the adoption in lsw2_customization_tick() and lsw2_customization_load() -
 * and both maintain this. It cannot change behind the flag's back. */
bool lsw2_customization_dirty();

void lsw2_customization_mark_clean();

#endif /* LSW2_CUSTOMIZATION_H */
