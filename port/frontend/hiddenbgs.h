/* ONE KEEPER FOR PATCH 0016'S LAYER MASK.
 *
 * nds_gpu2d_set_hidden_bgs(engine, mask) leaves an engine's named BGs out of
 * the picture it renders - presentation only, the guest's registers untouched
 * (gpu2d.h). It holds ONE mask per engine and each call replaces it, so two
 * parts of the port using it would undo each other: the stock join screens'
 * text (whsession.cpp, engine B's BG0 in states 34/35/39) and the starfield
 * behind the match HUD (arenahud.cpp). Each owner states its own engine and
 * mask here, and the engine gets the union. */

#ifndef LSW2_HIDDENBGS_H
#define LSW2_HIDDENBGS_H

#include <cstdint>

enum class Lsw2HiddenBgOwner : int {
    StockJoinText = 0,   /* whsession.cpp */
    ArenaStarfield = 1,  /* arenahud.cpp  */
    Count
};

/* This owner wants `mask` (bit n = BGn) hidden on `engine` (0 = A, 1 = B), or
 * nothing hidden anywhere when engine is -1. Cheap to call every frame: the
 * framework is only told when an engine's union changes. */
void lsw2_hidden_bgs_set(Lsw2HiddenBgOwner owner, int engine, uint8_t mask);

#endif /* LSW2_HIDDENBGS_H */
