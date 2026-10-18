/* See instancemute.h. */

#include "instancemute.h"

#include "ui.h"

#include "state.h"   /* bus_device_read32 / write32 */

#include <cstdint>
#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

bool g_claimed = false;
bool g_secondary = false;

}  // namespace

void lsw2_instance_claim() {
    if (g_claimed) return;
    g_claimed = true;
#ifdef _WIN32
    /* Held for the life of the process and never released by hand: Windows
     * drops it when the process ends, crash or not, so a stale claim cannot
     * outlive the first window. */
    HANDLE m = CreateMutexA(nullptr, FALSE, "Local\\LSW2BountyHunterInstance");
    g_secondary = (m != nullptr && GetLastError() == ERROR_ALREADY_EXISTS);
#endif
    std::fprintf(stderr, "[instance] %s\n",
                 g_secondary ? "another copy of the game was already running - "
                               "this one is a second instance"
                             : "the first copy of the game");
}

bool lsw2_instance_is_secondary() { return g_secondary; }

bool lsw2_instance_mute_all_sound() {
    if (!g_secondary) return false;
    const bool on = lsw2_ui_mute_sound_second_instance();
    return on;
}

void lsw2_instance_mute_tick() {
    if (!g_secondary || !lsw2_ui_mute_music_second_instance()) return;
    const uint32_t base = kLsw2MusicVolume & ~3u;
    const uint32_t shift = (kLsw2MusicVolume & 3u) * 8u;
    const uint32_t w = bus_device_read32(0, base);
    if (((w >> shift) & 0xFFu) == 0u) return;
    bus_device_write32(0, base, w & ~(0xFFu << shift));
    static bool said = false;
    if (!said) {
        said = true;
        std::fprintf(stderr, "[instance] second instance: the music player's "
                     "volume is held at 0 (Debug: mute music)\n");
    }
}
