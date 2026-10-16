/* The slot walk, against a fake buffer - see netmsglog.h for the derivation. */

#include "netmsglog.h"

#include <cstdio>
#include <vector>

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) std::printf("  ok: %s\n", what);
    else { ++g_failed; std::printf("  FAIL: %s\n", what); }
}

namespace {

struct Fake {
    std::vector<uint32_t> slots;
    std::vector<int> seen_slot, seen_type, seen_seq;
};

uint32_t rd(uint32_t addr, void* ctx) {
    Fake* f = static_cast<Fake*>(ctx);
    const uint32_t i = addr / 4u;
    return i < f->slots.size() ? f->slots[i] : 0u;
}

void collect(int slot, int type, int seq, void* ctx) {
    Fake* f = static_cast<Fake*>(ctx);
    f->seen_slot.push_back(slot);
    f->seen_type.push_back(type);
    f->seen_seq.push_back(seq);
}

/* delta | type << 8 | seq << 16 - the layout at 0x020B0470/0x0208048C. */
uint32_t entry(int delta, int type, int seq) {
    return uint32_t(delta & 0xFF) | (uint32_t(type & 0xFF) << 8) |
           (uint32_t(seq & 0xFFFF) << 16);
}

}  // namespace

int main() {
    /* --- an empty list ------------------------------------------------------ */
    {
        Fake f; f.slots = {0u};
        check(lsw2_netmsg_walk(0, rd, &f, collect) == 0,
              "a slot with delta 0 is an empty list");
        check(f.seen_type.empty(), "...and reports nothing");
    }

    /* --- one message -------------------------------------------------------- */
    {
        Fake f; f.slots = {entry(1, 0x15, 7), 0u};
        check(lsw2_netmsg_walk(0, rd, &f, collect) == 1, "one entry is walked");
        check(f.seen_type.size() == 1 && f.seen_type[0] == 0x15,
              "...and its type is byte 1 - 0x15, the host message");
        check(f.seen_seq[0] == 7, "...with the sequence from the high halfword");
    }

    /* --- the delta is a stride, not a step ---------------------------------- */
    {
        Fake f;
        f.slots = {entry(2, 0x20, 1), 0u, entry(1, 0x2D, 2), 0u};
        check(lsw2_netmsg_walk(0, rd, &f, collect) == 2,
              "a delta of 2 skips the slot between");
        check(f.seen_slot.size() == 2 && f.seen_slot[0] == 0 &&
                  f.seen_slot[1] == 2,
              "...landing on slots 0 and 2");
        check(f.seen_type[1] == 0x2D, "...and reads the second type");
    }

    /* --- A CYCLE MUST NOT HANG ----------------------------------------------
     * The game's own loop has no guard because the list is its own data. The
     * port is reading guest memory that may be anything, and a frame that
     * hangs is worse than a number that is wrong. */
    {
        Fake f; f.slots = {entry(0xFF, 1, 1)};   /* wraps back to itself */
        const int n = lsw2_netmsg_walk(0, rd, &f, collect);
        check(n > 0 && n <= 16, "a cycle stops at the visit cap");
    }

    /* --- a null reader is refused rather than crashed ----------------------- */
    check(lsw2_netmsg_walk(0, nullptr, nullptr, nullptr) == 0,
          "no reader means no walk");

    /* --- a null sink still walks -------------------------------------------- */
    {
        Fake f; f.slots = {entry(1, 9, 1), 0u};
        check(lsw2_netmsg_walk(0, rd, &f, nullptr) == 1,
              "the walk counts even with nothing to report to");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
