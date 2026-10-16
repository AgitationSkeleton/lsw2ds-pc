/* See screenengine.h - the half that reads the register.
 *
 * Two lines, because the mapping they wrap lives in screenengine_policy.cpp
 * where a test can reach it without the framework. */

#include "screenengine.h"

#include "io.h"

int lsw2_screen_engine(int screen) {
    return lsw2_screen_engine_for(nds_powercontrol9(), screen);
}

uint32_t lsw2_screen_dispcnt(int screen) {
    return lsw2_screen_dispcnt_for(nds_powercontrol9(), screen);
}
