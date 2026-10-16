/* A WINDOW DRAG MUST NOT STOP THE GAME. Moving the window froze the game for
 * the whole drag, which in an online match can cause desyncs or disconnects.
 *
 * WHY IT FROZE. On Windows, pressing on a title bar or a border hands the
 * mouse to DefWindowProc's own MOVE/SIZE loop, which runs until the button is
 * released. That loop is entered from inside SDL_PollEvent (SDL dispatches the
 * message to the window procedure), so the port's frame loop - emulation, the
 * fork's networking, presenting - stops for the whole drag. In a session the
 * other consoles then wait on this one's datasets.
 *
 * THE FIX: the drag is done here instead, without the system's loop. The game
 * windows are subclassed; a press on the caption or a sizing border captures
 * the mouse and records where it started, each mouse move while captured
 * places the window with SetWindowPos, and the release ends it. SDL_PollEvent
 * returns at once throughout, so the frame loop keeps running. Aero Snap is
 * not applied to these drags; a maximised window is left to the system (the
 * drag that restores it). Windows only - a no-op elsewhere. */

#ifndef LSW2_WINDRAG_H
#define LSW2_WINDRAG_H

struct SDL_Window;

/* Subclass this window so its moves and resizes do not block the frame loop. */
void lsw2_windrag_attach(SDL_Window* window);

#endif /* LSW2_WINDRAG_H */
