/* See hiddenbgs.h. */

#include "hiddenbgs.h"

#include "gpu2d.h"   /* nds_gpu2d_set_hidden_bgs (patch 0016) */

namespace {

constexpr int kOwners = int(Lsw2HiddenBgOwner::Count);

int g_engine[kOwners] = {-1, -1};
uint8_t g_mask[kOwners] = {0, 0};
uint8_t g_applied[2] = {0, 0};

}  // namespace

void lsw2_hidden_bgs_set(Lsw2HiddenBgOwner owner, int engine, uint8_t mask) {
    const int o = int(owner);
    if (o < 0 || o >= kOwners) return;
    g_engine[o] = (engine == 0 || engine == 1) ? engine : -1;
    g_mask[o] = uint8_t(mask & 0x0Fu);

    for (int e = 0; e < 2; ++e) {
        uint8_t want = 0;
        for (int i = 0; i < kOwners; ++i)
            if (g_engine[i] == e) want = uint8_t(want | g_mask[i]);
        if (want == g_applied[e]) continue;
        nds_gpu2d_set_hidden_bgs(e, want);
        g_applied[e] = want;
    }
}
