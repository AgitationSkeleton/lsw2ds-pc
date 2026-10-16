/* The host's customization policy - the pure half of customization.h.
 *
 * SEPARATE FROM customization.cpp SO IT CAN BE TESTED. That file reads and
 * writes guest memory through the framework's bus and reaches into the network
 * session; a unit test has no business linking either to check a membership
 * test and a switch. This is the part that is pure decision-making, so it lives
 * where a test can reach it.
 */

#include "customization.h"

namespace {

/* THE HEADWEAR SLOT, by part id range - CORRECTED.
 *
 * This was a membership test against EIGHT ids, because when it was written
 * which slot held headwear was unknown and guessing would have been invention.
 * Later analysis settled it: the part id ranges ARE the slots, and slot 3 is headwear
 * - **ids 177 to 231, fifty-five parts**.
 *
 * The eight are real and are kept below, but they are the hats the EXTRAS modes
 * assign, not the set a player can wear. Testing membership against them made
 * the host's "Hats" policy strip 47 of the 55 as though they were not hats.
 *
 * That is the shape this project keeps meeting: a fact that was true of the
 * evidence available and false of the game, left in place after better evidence
 * arrived. The comment that justified it was careful and still wrong. */
constexpr uint16_t kHeadwearFirst = 177u;
constexpr uint16_t kHeadwearLast = 231u;

/* The eight that used to BE this test - BaseballCap, CowboyHat, Crown,
 * KnightHelmet, Moustache, PirateHat, TopHat, WizardHat, ids 178, 187, 188,
 * 202, 212, 214, 228, 229 - are the hats the EXTRAS modes assign, and they all
 * fall inside the range above. They are named in
 * config/customization_modes.json with the mode each belongs to, which is where
 * that knowledge belongs; a second copy here with no caller would be an orphan
 * rather than a record. */

}  // namespace

bool lsw2_customization_is_headwear(uint16_t part) {
    return part >= kHeadwearFirst && part <= kHeadwearLast;
}

void lsw2_customization_filter(const uint16_t* saved, uint16_t* out,
                               Lsw2CustomPolicy policy) {
    if (!saved || !out) return;
    for (int i = 0; i < kLsw2CustomSlots; ++i) {
        switch (policy) {
        case Lsw2CustomPolicy::All:
            out[i] = saved[i];
            break;
        case Lsw2CustomPolicy::Hats:
            /* "stock hunters are applied" for everything that is not a hat, and
             * the game's own word for stock is 0xFA - read off the
             * two apply loops, which skip a slot holding it. Writing the game's
             * sentinel rather than a zero is what makes the guest treat the
             * slot as empty instead of as part id 0. */
            out[i] = lsw2_customization_is_headwear(saved[i]) ? saved[i]
                                                              : kLsw2CustomNone;
            break;
        case Lsw2CustomPolicy::Off:
            out[i] = kLsw2CustomNone;
            break;
        }
    }
}
