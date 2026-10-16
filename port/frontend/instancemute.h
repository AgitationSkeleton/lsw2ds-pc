/* THE DEBUG MUTES FOR A SECOND COPY OF THE GAME.
 *
 * A debug setting disables music when another instance is running, so music is
 * not doubled when testing multiple clients; a second setting does the same for
 * sound.
 *
 * WHICH COPY IS SECOND. The first process to start claims a named mutex; a
 * process that finds it already claimed is a second instance, for its whole
 * life. The answer is taken once, at startup (lsw2_instance_claim), so the
 * first window keeps its sound when a later one is closed and reopened.
 *
 * MUSIC ONLY, THE GAME'S OWN WAY. The music player's output volume is the byte
 * at 0x0210DB84 + 0xC0 (0x0210DC44), which the game's music-volume setter
 * FUN_020bb4e0 writes and the sequencer reads for every note; measured with a
 * write watch while stepping the Settings page's "Music Volume" (95 -> 94 ->
 * 93) and at boot, where FUN_020b8d80 restores the saved value. Holding that
 * byte at 0 silences the music within a note and leaves the SAVED option
 * (0x02104364 + 0xA, FUN_020b8f48's store) and the Settings page alone, so a
 * shared save file never learns a zero. It is sound-engine output state: the
 * simulation never reads it, so a session's lockstep is not touched.
 *
 * ALL SOUND is simpler: the output samples are zeroed, as the boot's are. */

#ifndef LSW2_INSTANCEMUTE_H
#define LSW2_INSTANCEMUTE_H

/* Claim the instance mutex, once, as early as the process can. */
void lsw2_instance_claim();

/* Another copy of the game was already running when this one started. */
bool lsw2_instance_is_secondary();

/* Whether this copy's audio output should be silenced (the Sound setting on a
 * second instance). */
bool lsw2_instance_mute_all_sound();

/* Once a frame: hold the music player's volume at 0 when the Music setting is
 * on and this is a second instance. */
void lsw2_instance_mute_tick();

#endif /* LSW2_INSTANCEMUTE_H */
