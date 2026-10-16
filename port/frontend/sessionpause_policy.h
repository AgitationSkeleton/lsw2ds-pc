/* The online pause's decisions, with no guest in them - see sessionpause.h for
 * the mechanism and the measurements. Split out so the two rules every console
 * must apply identically (which stations are paused, and what a pause-menu
 * message does here) are pinned by a test that needs no ROM. */

#ifndef LSW2_SESSIONPAUSE_POLICY_H
#define LSW2_SESSIONPAUSE_POLICY_H

#include <cstdint>

/* The cartridge's three pause-menu messages:
 *   0x2B  sent by 0x0208CEF8 when Start is pressed with the multi-view bit up;
 *         arm 0x020B2770 -> FUN_0208ce8c: freeze + request state 53.
 *   0x2C  the pause menu's Resume row (0x0208EADC); arm 0x020B2784: unpause and
 *         ask for the screen state the pause replaced (manager+0x28).
 *   0x42  the confirmed Quit row (0x0208EF70); arm 0x020B2E7C: end the session,
 *         the sender to the front end, everyone else to an offline lobby. */
constexpr uint8_t kLsw2MsgPauseOpen = 0x2Bu;
constexpr uint8_t kLsw2MsgPauseResume = 0x2Cu;
constexpr uint8_t kLsw2MsgPauseQuit = 0x42u;

/* The session's host is AID 0 on the host-level WH. */
constexpr int kLsw2PauseHostStation = 0;

/* What this console's dispatcher does with the message. */
enum class Lsw2PauseMsgAction {
    Arm,        /* the cartridge's own handler runs, unchanged           */
    Drop,       /* consumed: nothing on this console but the mask        */
    OpenLocal,  /* this console's own pause: state 53 with no freeze     */
};

/* ONE PAUSE-MENU MESSAGE, AS EVERY CONSOLE SEES IT AT ONE DISPATCH.
 *
 * `paused` (a bit per station) is updated identically on every console - the
 * message arrived in the same dataset everywhere - and says whose input the
 * world must not see. The returned action is this console's own:
 *
 *   0x2B from s  s is paused. Only s's console opens its menu (OpenLocal, or
 *                Drop if its menu is already up); every other console Drops.
 *   0x2C from s  s is not. s's console runs the arm if its menu is still up -
 *                if a transition already replaced it, the arm would ask for
 *                the pause state back, so it Drops. Others Drop.
 *   0x42 from s  s is leaving: its own console quits (Arm). The HOST's quit is
 *                "Shut Down Lobby" and ends the session for everyone (Arm on
 *                all). A client's is that client's alone: others Drop, and
 *                the host counts the player out when the departure arrives.
 *
 * Anything else is not the pause's (Arm). `local_menu_up` is whether this
 * console's screen slot holds the pause state, or is about to. */
Lsw2PauseMsgAction lsw2_pause_message_policy(uint8_t type, int station, int local,
                                             bool local_menu_up, uint8_t* paused);

/* One station's input word as the sampler keeps it (FUN_02059e64, the 10-byte
 * records at 0x021027F8): held, the previous held, and the two edges. */
struct Lsw2PadState {
    uint16_t held;
    uint16_t prev;
    uint16_t pressed;
    uint16_t released;
};

/* THE FIRST FRAME AFTER A PAUSE ENDS. While a station is paused every console
 * zeroes its input, so the sampler's next "previous" is 0 and every key the
 * player is still holding would read as a fresh press - a jump or a shot on the
 * frame the menu closes. These are the edges the sampler would have computed
 * against the keys held on the last paused frame instead. */
Lsw2PadState lsw2_pause_resume_edges(uint16_t held_now, uint16_t held_before);

#endif /* LSW2_SESSIONPAUSE_POLICY_H */
