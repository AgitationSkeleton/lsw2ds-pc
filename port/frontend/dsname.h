/* THE PLAYER'S NAME, GIVEN TO THE CARTRIDGE THE WAY THE CARTRIDGE TAKES IT.
 *
 * THE GOAL, IN PLAIN ENGLISH. The player's name is set on the imgui menu in
 * a Multiplayer tab, defaulting to 'Maclunkey', and that name is what the
 * master server list shows, what the join and disconnect popups say, and what
 * the host's kick list offers. The BASELINE gets the same tab and the
 * same name.
 *
 * WHERE THE NAME WAS NOT GOING. On the preferred route -
 * the cartridge's own wireless, which every verified match has used - the
 * cartridge never saw the imgui name at all. Its beacon read "ndsrecomp -
 * Coop" (read out of the host's context in UTF-16), because the
 * cartridge builds its game name, its browse-list row and its four per-slot
 * peer names (`settingsObject + 0x51 + 24*slot`, i.e. "the
 * firmware username") from the DS USER SETTINGS, which the runner's generated
 * firmware fills with its own default. The fork's name lived only in the
 * fork's TCP transport and the directory.
 *
 * THE CARTRIDGE'S OWN READER, from the image. The SDK routine at 0x020CE938
 * keeps the literal 0x027FFC80 in its pool and copies from that block:
 *
 *     020CE980  ldrb r3,[r12,#0x1a]      the nickname's LENGTH
 *     020CE984  strh r3,[r4,#0x18]
 *     ...       bl   memcpy(dst, 027FFC80+6, 0x14)   ten UTF-16 characters
 *     020CE994  ldr  r0,[0x20ce9ac]      = 027FFC80
 *     020CE998  add  r1,r4,#0x1a
 *     020CE99C  add  r0,r0,#0x1c         the "message" field, 0x34 bytes
 *
 * That is the DS user-settings block the firmware loader leaves in main RAM:
 * nickname at +0x06 as ten UTF-16 code units, its length at +0x1A. Nothing
 * rewrites it after boot, and the wireless layer's readers (0x020D6BC4,
 * 0x020D6F08, 0x020D6F98, 0x020DC414 call the small copy at 0x020CE91C) take
 * it fresh when a session starts.
 *
 * SO THIS WRITES THE IMGUI NAME THERE, in both builds, and lets the cartridge
 * carry it everywhere it already carries a name. No string override, no
 * second name store, no fork-drawn substitute: the same bytes the firmware
 * would have supplied, supplied by the player instead. The session object's
 * `player_name()` is the source, so the directory row and the beacon can never
 * disagree.
 *
 * TEN CHARACTERS, ASCII. The DS nickname holds ten UTF-16 code units; the
 * fork's sanitiser (lsw2_name.h) already restricts a name to printable ASCII,
 * so the encoding is a widening and a cap, and the cap is the cartridge's, not
 * ours. A name that survives as nothing becomes "Player", the sanitiser's own
 * fallback, so a blank row never appears in anyone's list. */

#ifndef LSW2_DSNAME_H
#define LSW2_DSNAME_H

#include <cstdint>

/* The DS user-settings block in main RAM, as the cartridge's own reader
 * addresses it (literal at 0x020CE9AC). */
constexpr uint32_t kLsw2DsUserSettings = 0x027FFC80u;
constexpr uint32_t kLsw2DsNicknameOffset = 0x06u;      /* ten UTF-16 units */
constexpr uint32_t kLsw2DsNicknameLenOffset = 0x1Au;   /* u16, in units    */
constexpr int kLsw2DsNicknameChars = 10;

/* THE PURE HALF, so the cap and the fallback can be pinned without a guest.
 * Encodes `name` as UTF-16 code units into `out` (always all ten written,
 * unused ones zero) and returns how many carry a character. */
int lsw2_dsname_encode(const char* name, uint16_t out[kLsw2DsNicknameChars]);

/* Once a frame from the fork tick, BOTH builds. Writes the block when it does
 * not already hold the encoded name - which is the first frame, and again if
 * anything in the guest ever puts the firmware's name back. Says so on the
 * log each time it writes, so a name that reverts is visible as a count. */
void lsw2_dsname_tick();

#endif /* LSW2_DSNAME_H */
