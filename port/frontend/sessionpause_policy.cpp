/* See sessionpause_policy.h. */

#include "sessionpause_policy.h"

Lsw2PauseMsgAction lsw2_pause_message_policy(uint8_t type, int station, int local,
                                             bool local_menu_up, uint8_t* paused) {
    if (!paused || station < 0 || station > 3) return Lsw2PauseMsgAction::Arm;
    const uint8_t bit = uint8_t(1u << station);
    switch (type) {
    case kLsw2MsgPauseOpen:
        *paused = uint8_t(*paused | bit);
        if (station != local) return Lsw2PauseMsgAction::Drop;
        return local_menu_up ? Lsw2PauseMsgAction::Drop
                             : Lsw2PauseMsgAction::OpenLocal;
    case kLsw2MsgPauseResume:
        *paused = uint8_t(*paused & ~bit);
        if (station != local) return Lsw2PauseMsgAction::Drop;
        return local_menu_up ? Lsw2PauseMsgAction::Arm : Lsw2PauseMsgAction::Drop;
    case kLsw2MsgPauseQuit:
        *paused = uint8_t(*paused & ~bit);
        return (station == local || station == kLsw2PauseHostStation)
                   ? Lsw2PauseMsgAction::Arm
                   : Lsw2PauseMsgAction::Drop;
    default:
        return Lsw2PauseMsgAction::Arm;
    }
}

Lsw2PadState lsw2_pause_resume_edges(uint16_t held_now, uint16_t held_before) {
    Lsw2PadState s;
    s.held = held_now;
    s.prev = held_before;
    /* The sampler's own arithmetic (FUN_02059e64): new & (new ^ old), and
     * old & (new ^ old). */
    s.pressed = uint16_t(held_now & (held_now ^ held_before));
    s.released = uint16_t(held_before & (held_now ^ held_before));
    return s;
}
