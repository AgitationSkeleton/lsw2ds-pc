/* THE RADAR TURNS WITH THE ENHANCED CAMERA.
 *
 * THE SYMPTOM: with Enhanced controls, the touch screen's radar - and the copy
 * of it in the arena HUD - turned with the character, not with the camera, so
 * "up" on the radar was wherever the player happened to be facing.
 *
 * WHY. The radar places each blip with FUN_0209b27c, which rotates the offset
 * from the local player by that player's heading: party[local station]
 * (0x02109358[*(0x021027F8+0x2A)]), heading u16 at actor+0xCA, through the
 * sine table 0x020F88C4 (pool words 0x0209B600/04/0C, read on a live match).
 * In the game's own arena camera (mode 9) the camera always looks along that
 * heading, so the two never differed. The Enhanced orbit (enhanced.h, mode 4)
 * turns the camera on its own.
 *
 * SO THE GAME'S RADAR IS GIVEN THE CAMERA'S HEADING, for the length of the
 * touch HUD's update and no longer. The update (0x0209A3CC, the override
 * touchportrait.cpp already hosts) is entered with lr pointing back at itself;
 * the real heading is put back when it returns, recognised by its stack
 * pointer - the same "return into the override" the gear hop uses on that
 * host. Within the update, FUN_0209b27c is the only reader of +0xCA (its
 * callees were checked), and the value is restored only while it still reads
 * what was written, so a heading the game itself changed is never undone. */

#ifndef LSW2_RADARVIEW_H
#define LSW2_RADARVIEW_H

/* First thing in the touch HUD update's override: true when this entry is the
 * update returning - the heading has been put back and the override must
 * return at once, to the caller now in r15. */
bool lsw2_radarview_returned();

/* Last thing before the override runs the game's update: substitute the
 * heading when the orbit is active, and point lr back at the host. */
void lsw2_radarview_enter();

#endif /* LSW2_RADARVIEW_H */
