/* The host settings menu's "Music: <DEFAULT/RANDOM>", finally buildable.
 *
 * Setting it to Random selects a random 'battle' music track from the game's
 * soundtrack upon round start (not in the lobby) - one of the tracks that
 * levels switch to during combat. Which tracks toggle like this in gameplay
 * had to be determined.
 *
 * THAT TURNED OUT NOT TO NEED WATCHING GAMEPLAY. Two earlier analyses both
 * concluded the track set could only come from a play test, and this option sat
 * inert for that reason. The set is in the ROM:
 *
 *     0x0208FD24  LDRB r0,[r0,#0x479]   the level index
 *                 BL   0x02093F6C       level -> music slot
 *                 BL   0x020B3ABC       set it on the music record
 *                 BL   0x020B3718       and request variation 0
 *
 * and 0x02093F6C is `[[0x020FF674 + level*4] + 1]`. tools/export_music_levels.py
 * dumps it: 64 levels onto 22 distinct slots, chapters sharing one each.
 *
 * WHAT THAT SETTLES, and why the option's shape follows from it rather than
 * from taste:
 *
 *   LEVELS 4, 5 AND 9 ALL USE SLOT 2 - the two arena maps and the arena lobby.
 *   The arena has ONE track, shared with its own lobby. So "a random battle
 *   track" cannot mean another arena track; there is no other. It means one
 *   borrowed from elsewhere, which is what "from the game's soundtrack" says.
 *
 *   ~~AND "CALM AND BATTLE" IS NOT TWO SLOTS ... The calm/battle pair is the
 *   PARAM axis on one slot ... This feature therefore changes the SLOT and
 *   leaves the variation alone.~~ RETRACTED after testing in play.
 *   The byte in a level record is not a slot: it is a MUSIC SET, an
 *   index into 0x020F02BC (8 bytes a set). 0x020B37C0 maps the music record's
 *   variation to a slot through it - variation 0 (calm) takes +0 and +1,
 *   variation 3 takes +2 and +3 - and variation 3 is what the record's update
 *   asks for while +0x34 is non-zero (0x020B36AC), a count the AI bumps each
 *   frame an engaged actor's target is a player (0x0202D2A0). So calm and
 *   battle ARE two slots, one set's +0 and +2, and the level path's variation
 *   0 always played the CALM one. The pool includes only the 'action' or
 *   battle tracks from the levels. See kLsw2MusicBattleSlots.
 *
 * WHY THE SEED IS THE ROUND'S, NOT rand(). Every machine must hear the same
 * track: the host chose RANDOM, and four players in one arena hearing four
 * different songs is not what the setting means. `lsw2_netmsg_start_round`
 * already carries a seed to every peer for exactly this class of decision, so
 * the pick is a pure function of it.
 *
 * NOT THE LOBBY. The pick is made upon round start, not in the lobby,
 * and the lobby shares the arena's slot, so picking there would
 * also change the music the player is standing in while they wait.
 */

#ifndef LSW2_MUSICPICK_H
#define LSW2_MUSICPICK_H

#include <cstdint>

/* The music-set table (0x020F02BC, 8 bytes a set: calm slot, mode, battle
 * slot, mode, then variations 4 and 5), and the sets the game's levels use -
 * 0..21, from config/music_levels.json. */
constexpr uint32_t kLsw2MusicSetTable = 0x020F02BCu;
constexpr uint32_t kLsw2MusicSetStride = 8u;
constexpr uint32_t kLsw2MusicSetCalm = 0u;
constexpr uint32_t kLsw2MusicSetBattle = 2u;
constexpr int kLsw2MusicSlotCount = 22;

/* The arena's own set, which levels 4, 5 and 9 share: calm and battle are both
 * slot 17, the battle track eight other level sets use as well. */
constexpr int kLsw2MusicArenaSlot = 2;

/* THE LEVELS' BATTLE TRACKS: every slot that is some level set's battle slot
 * where it differs from that set's calm slot - the tracks a level switches TO
 * when fighting starts - read off the table. lsw2_music_tick checks the
 * running game's table still says this before it uses any of them.
 *
 * 17, THE ARENA'S OWN, IS IN THE POOL: the arena's own default track is one
 * of the random draws. It was left out on the reasoning that a random pick
 * which sometimes changes nothing reads as broken; it belongs in the pool
 * because it is a battle track in its own right - eight level sets switch to
 * it in combat. */
constexpr int kLsw2MusicBattleSlots[] = {1, 3, 4, 5, 7, 12, 13, 17};
constexpr int kLsw2MusicBattleCount =
    int(sizeof kLsw2MusicBattleSlots / sizeof kLsw2MusicBattleSlots[0]);

/* THE SET A RANDOM ROUND PLAYS: 22, which no level record names. Its calm and
 * battle slots are both set to the pick, so the arena plays that battle track
 * from its first frame whichever variation the game asks for, and the lobby's
 * set (2) is never touched. */
constexpr int kLsw2MusicSpareSet = 22;

/* Which battle slot a given round seed picks. Pure, so it can be tested and so
 * every machine agrees without exchanging anything but the seed. */
int lsw2_music_pick(uint16_t seed);

/* Once a frame from the fork's tick: keeps the two arena maps' level records
 * naming the music the next round should play - the spare set carrying the
 * picked battle track while a live session has RANDOM and an agreed round
 * seed, the arena's own set otherwise - so the game's own level constructor
 * plays it when the arena loads. See the note in musicpick.cpp for why this
 * writes bytes rather than calling the selector.
 *
 * THE SEED IS THE SHARED ONE. `Lsw2ForkSession::match_seed()` is adopted by
 * every machine at round start, so four players hear one song; the host's own
 * send (lsw2_netmsg_start_round) is not the place, because only the host runs
 * it.
 *
 * Silent in the baseline, and leaves the records exactly as the ROM loaded
 * them until RANDOM is first chosen. */
void lsw2_music_tick();

/* LOBBY MUSIC: [HUNTER/CANTINA/RANDOM]. The arena
 * lobby's level record names its music set like the arena maps' do, so this
 * keeps it naming the set the setting wants - the lobby's own (HUNTER), the
 * Cantina level's (CANTINA), or kLsw2MusicLobbySpareSet carrying a random one of
 * the levels' tracks (RANDOM, a new draw each time the lobby is left: a round
 * in a session, which every console counts alike, or a visit offline). The
 * next lobby load plays it; a change made while standing in the lobby is
 * applied at once through the game's own music start (0x020B3718) on the
 * guest's thread. The setting is the host's while in a session. */
/* THE GAME'S MUSIC CONTEXT: +9 the slot recorded as playing (the request
 * function 0x020B90E4 skips a request for it), +0xA its flags - 0x01 paused,
 * 0x02 stale (restart on the next request), 0x20 music suppressed. */
constexpr uint32_t kLsw2MusicContext = 0x0210DB14u;
constexpr int kLsw2MusicLobbySpareSet = 23;
void lsw2_lobby_music_tick();

/* THE MUSIC, AS THE LOG SEES IT. Once a frame, both builds: the slot the music
 * context says is playing (0x0210DB14 + 9), its flags byte (+0xA, bit 0x20
 * suppresses music) and the music player's volume (0x0210DC44), with the state
 * and level, logged whenever any of them changes. Read-only - it cannot change
 * the music, only say when and where it changed - so a player's report of the
 * music cutting out arrives with the frame it happened on. */
void lsw2_music_watch_tick();

#endif /* LSW2_MUSICPICK_H */
