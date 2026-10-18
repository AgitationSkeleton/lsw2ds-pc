/* See touchportrait.h. */

#include "touchportrait.h"

#include "cheats.h"        /* kLsw2PartyActors, kLsw2ActorCharacter */
#include "gearhop.h"
#include "levels.h"        /* kLsw2LevelRootAddr */
#include "ui.h"

#include "runtime_arm.h"
#include "state.h"

#include <cstdio>

extern "C" void lsw2_arm9_FUN_0209a3cc(void);

namespace {

/* The init's own numbers - touchportrait.h has the disassembly they come from. */
constexpr uint32_t kHudFaces = 0x94u;            /* hud + i*4: slot i's face sprite */
constexpr uint32_t kCharTable = 0x020EF57Cu;     /* 0x18 bytes a character */
constexpr uint32_t kCharStride = 0x18u;
constexpr uint32_t kCharFrames = 0x08u;          /* the entry's frame set */
constexpr uint32_t kCharCount = 0x54u;           /* FUN_0208ea58's own bound */
constexpr uint32_t kSpriteFrames = 0x20u;        /* sprite -> its frame set */
constexpr uint32_t kSprite2dVtable = 0x020FD0F0u;
constexpr uint32_t kSpriteFactory = 0x0201AD50u;
constexpr uint32_t kSpriteManagerPool = 0x0209A3A4u;  /* -> -> the manager */
constexpr uint32_t kFaceX = 0x8000u;             /* 8.0 */
constexpr uint32_t kFacePitch = 0x30u, kFaceTop = 0x08u;
constexpr uint32_t kFacePaletteBase = 8u;

enum Step { kDestroy = 0, kBuild = 1 };

/* One rebuild at a time: which HUD and slot, and the character it is for. */
bool g_busy = false;
uint32_t g_hud = 0;
int g_slot = -1;
uint8_t g_char = 0;
int g_said_odd = 0;

bool ram(uint32_t a) { return a >= 0x02000000u && a < 0x02400000u; }
uint32_t r32(uint32_t a) { return bus_device_read32(0, a); }
uint8_t r8(uint32_t a) { return uint8_t(r32(a & ~3u) >> ((a & 3u) * 8u)); }

uint32_t frames_of(uint8_t c) { return kCharTable + uint32_t(c) * kCharStride + kCharFrames; }

/* Which character a face was built for, from its frame set; -1 when the
 * pointer is not one of the table's entries. */
int built_for(uint32_t sprite) {
    const uint32_t f = r32(sprite + kSpriteFrames);
    if (f < frames_of(0) || f >= frames_of(uint8_t(kCharCount))) return -1;
    if ((f - frames_of(0)) % kCharStride != 0u) return -1;
    return int((f - frames_of(0)) / kCharStride);
}

void check(uint32_t hud) {
    if (g_busy || !ram(hud)) return;
    for (int i = 0; i < 4; ++i) {
        const uint32_t sprite = r32(hud + kHudFaces + uint32_t(i) * 4u);
        const uint32_t actor = r32(kLsw2LevelRootAddr + kLsw2PartyActors + uint32_t(i) * 4u);
        if (!ram(sprite) || !ram(actor)) continue;
        const uint8_t c = r8(actor + kLsw2ActorCharacter);
        if (c >= kCharCount) continue;
        const int was = built_for(sprite);
        if (was == int(c)) continue;
        if (was < 0 || r32(sprite) != kSprite2dVtable) {
            if (g_said_odd < 4) {
                ++g_said_odd;
                std::fprintf(stderr,
                             "[fork] touch portrait: slot %d's face %08X (vtable "
                             "%08X, frames %08X) is not a sprite this knows how to "
                             "rebuild - left alone\n",
                             i, unsigned(sprite), unsigned(r32(sprite)),
                             unsigned(r32(sprite + kSpriteFrames)));
            }
            continue;
        }
        /* THE FACE'S OWN DELETING DESTRUCTOR, through its vtable. */
        const uint32_t dtor = r32(r32(sprite) + 4u);
        if (lsw2_gearhop_request_call7_at(dtor, sprite, 0u, 0u, 0u, 0u, 0u, 0u,
                                          kDestroy, kLsw2TouchHudUpdate)) {
            g_busy = true;
            g_hud = hud;
            g_slot = i;
            g_char = c;
            std::fprintf(stderr,
                         "[fork] touch portrait: slot %d is character %u, its face "
                         "was built for %d - rebuilding\n", i, unsigned(c), was);
        }
        return;
    }
}

}  // namespace

void lsw2_touchportrait_update() {
    /* THE HOP'S TURN FIRST: a step may be taken here, or one returning here, and
     * either way r0 is not the HUD until the hook has put the host back. */
    if (lsw2_gearhop_hook(kLsw2TouchHudUpdate)) return;
    if (ui_fork_build()) check(g_cpu.R[0]);
    g_cpu.R[15] = kLsw2TouchHudUpdate;
    lsw2_arm9_FUN_0209a3cc();
}

bool lsw2_touchportrait_step_still_live(int step, uint32_t a0, uint32_t hud) {
    if (!g_busy || hud != g_hud || g_slot < 0) return false;
    const uint32_t slot = g_hud + kHudFaces + uint32_t(g_slot) * 4u;
    if (step == kDestroy) return r32(slot) == a0;
    return a0 == slot && r32(slot) == 0u;
}

void lsw2_touchportrait_step_dropped(int step) {
    std::fprintf(stderr,
                 "[fork] touch portrait: step %d for slot %d dropped - the HUD "
                 "it was for is gone\n", step, g_slot);
    g_busy = false;
    g_slot = -1;
}

void lsw2_touchportrait_step_returned(int step, uint32_t result) {
    if (!g_busy || g_slot < 0) return;
    const uint32_t slot = g_hud + kHudFaces + uint32_t(g_slot) * 4u;
    if (step == kDestroy) {
        /* THE SLOT IS CLEARED ONLY NOW that the destructor has run, as the
         * cartridge clears a slot after deleting what it held. */
        bus_device_write32(0, slot, 0u);
        const uint32_t i = uint32_t(g_slot);
        const uint32_t manager = r32(r32(kSpriteManagerPool));
        if (!lsw2_gearhop_request_call7_at(
                kSpriteFactory, slot, i + kFacePaletteBase, frames_of(g_char),
                kFaceX, (kFacePitch * i + kFaceTop) << 12, 0u, manager, kBuild,
                kLsw2TouchHudUpdate)) {
            std::fprintf(stderr,
                         "[fork] touch portrait: slot %d's build could not be "
                         "queued - its face stays empty\n", g_slot);
            g_busy = false;
            g_slot = -1;
        }
        return;
    }
    std::fprintf(stderr,
                 "[fork] touch portrait: slot %d rebuilt as character %u (face "
                 "%08X)\n", g_slot, unsigned(g_char), unsigned(r32(slot)));
    (void)result;
    g_busy = false;
    g_slot = -1;
}
