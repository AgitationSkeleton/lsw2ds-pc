/* The cartridge-roster policy: which slots are remote peers.
 *
 * The guest read is elsewhere; what is judgement, and testable without a ROM,
 * is which of the four name slots become peers given the local station and the
 * seat count. Getting this wrong shows another player as yourself, or misses a
 * peer the kick list needs, so it is pinned. */

#include "cartroster.h"

#include <cstdio>
#include <cstring>

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("  ok: %s\n", what);
    } else {
        ++g_failed;
        std::printf("  FAIL: %s\n", what);
    }
}

static Lsw2CartSlot slot(const char* name) {
    Lsw2CartSlot s{};
    s.present = (name && name[0]);
    if (s.present) std::snprintf(s.name, sizeof s.name, "%s", name);
    return s;
}

int main() {
    const char* out[3];
    int station[3];

    /* Host at slot 0, one joiner at slot 1, two seated. The joiner is the one
     * peer; the host's own slot is never a peer. */
    {
        Lsw2CartSlot s[4] = {slot("Maclunkey"), slot("Maclunkey"), slot(""), slot("")};
        const int n = lsw2_cartroster_remote_peers(s, /*local*/0, /*seats*/2, out, station);
        check(n == 1, "one remote peer with two seated");
        check(n == 1 && station[0] == 1, "and it is slot 1, not the local slot 0");
    }

    /* The JOINER's view: it is slot 1, the host is slot 0. The host is its one
     * peer. Symmetry - each console excludes its own slot. */
    {
        Lsw2CartSlot s[4] = {slot("Alice"), slot("Bob"), slot(""), slot("")};
        const int n = lsw2_cartroster_remote_peers(s, /*local*/1, /*seats*/2, out, station);
        check(n == 1 && station[0] == 0 && std::strcmp(out[0], "Alice") == 0,
              "the joiner's one peer is the host at slot 0");
    }

    /* Four players, local at slot 2: three peers, in slot order, skipping 2. */
    {
        Lsw2CartSlot s[4] = {slot("A"), slot("B"), slot("Me"), slot("D")};
        const int n = lsw2_cartroster_remote_peers(s, /*local*/2, /*seats*/4, out, station);
        check(n == 3, "three peers with four seated");
        check(n == 3 && station[0] == 0 && station[1] == 1 && station[2] == 3,
              "in slot order, the local slot 2 skipped");
    }

    /* A lone player: no peers, whatever the slots hold. */
    {
        Lsw2CartSlot s[4] = {slot("Solo"), slot(""), slot(""), slot("")};
        const int n = lsw2_cartroster_remote_peers(s, /*local*/0, /*seats*/1, out, station);
        check(n == 0, "one seat, no peers");
    }

    /* THE SEAT COUNT CAPS A LINGERING SLOT. A name slot that has not been
     * cleared yet, while the count already says one seat, is not a peer - the
     * count is the seat truth and the name is stale. */
    {
        Lsw2CartSlot s[4] = {slot("Host"), slot("GhostLeaver"), slot(""), slot("")};
        const int n = lsw2_cartroster_remote_peers(s, /*local*/0, /*seats*/1, out, station);
        check(n == 0, "a stale name slot with the count back to one is not a peer");
    }

    /* An empty slot between two full ones is skipped, not counted. */
    {
        Lsw2CartSlot s[4] = {slot("A"), slot(""), slot("C"), slot("")};
        const int n = lsw2_cartroster_remote_peers(s, /*local*/0, /*seats*/3, out, station);
        check(n == 1 && station[0] == 2,
              "an empty middle slot is skipped; only the full remote slot counts");
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
