/* The decoder, checked against the REAL string bank.
 *
 * test_strings.cpp proves the algorithm is self-consistent against a bank it
 * builds itself. This proves it agrees with the GAME, which is the claim that
 * actually matters before the decoder is allowed to replace the guest's.
 *
 * The fixture comes from tools/dump_string_bank_fixture.py - rebuilt RAM plus
 * the expected strings from config/strings.json, which the exporter produced
 * and self-validated against character names captured independently from
 * framebuffers.
 *
 *   python tools/dump_string_bank_fixture.py
 *   g++ -O2 -std=c++20 -Iport/frontend port/frontend/test_strings_real.cpp \
 *       port/frontend/strings.cpp -o test_strings_real
 *   ./test_strings_real out/work/string_fixture
 *
 * SKIPS RATHER THAN FAILS when the fixture is absent, because the capture it
 * needs is not in the repository. A check that cannot run must say so; it must
 * not report success.
 */

#include "strings.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> g_ram;
constexpr uint32_t kMainRamBase = 0x02000000;

uint8_t read8(uint32_t addr, void*) {
    const uint32_t off = addr - kMainRamBase;
    return off < g_ram.size() ? g_ram[off] : 0u;
}

bool read_file(const std::string& path, std::vector<uint8_t>* out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out->resize(size_t(n < 0 ? 0 : n));
    const size_t got = out->empty() ? 0 : std::fread(out->data(), 1, out->size(), f);
    std::fclose(f);
    return got == out->size();
}

uint32_t u32_at(const std::vector<uint8_t>& v, size_t off) {
    return uint32_t(v[off]) | (uint32_t(v[off + 1]) << 8) |
           (uint32_t(v[off + 2]) << 16) | (uint32_t(v[off + 3]) << 24);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "out/work/string_fixture";

    std::vector<uint8_t> expected;
    if (!read_file(dir + "/ram.bin", &g_ram) ||
        !read_file(dir + "/expected.bin", &expected)) {
        std::printf("SKIPPED - no fixture in %s\n", dir.c_str());
        std::printf("  build it with: python tools/dump_string_bank_fixture.py\n");
        std::printf("  (it needs the capture in out/work/session_live_01)\n");
        return 0;
    }

    std::printf("string decoder against the real bank\n");
    std::printf("  %zu bytes of RAM\n", g_ram.size());

    /* THE CONTROL BLOCK MUST BE FILLED. It reads zero in the static image, so a
     * fixture built from the wrong frame would make every decode return
     * NotReady and the run would look like a pass with zero comparisons. Check
     * it explicitly rather than trusting the count at the end. */
    char probe[8];
    if (lsw2_string_decode(kLsw2StringControlBlock, 0x28E, probe, sizeof probe,
                           read8, nullptr) == Lsw2StringResult::NotReady) {
        std::printf("FAILED - the control block reads empty in this fixture\n");
        return 1;
    }

    const uint32_t count = u32_at(expected, 0);
    size_t off = 4;
    uint32_t checked = 0, mismatched = 0, truncated = 0;
    std::string first_bad;

    for (uint32_t i = 0; i < count && off + 8 <= expected.size(); ++i) {
        const uint32_t id = u32_at(expected, off);
        const uint32_t len = u32_at(expected, off + 4);
        off += 8;
        if (off + len > expected.size()) break;
        const std::string want(reinterpret_cast<const char*>(&expected[off]), len);
        off += len;

        char got[1024];
        const Lsw2StringResult r =
            lsw2_string_decode(kLsw2StringControlBlock, id, got, sizeof got,
                               read8, nullptr);
        ++checked;
        if (r == Lsw2StringResult::Truncated) { ++truncated; continue; }
        if (want != got) {
            ++mismatched;
            if (first_bad.empty()) {
                char buf[512];
                std::snprintf(buf, sizeof buf,
                              "id 0x%03X: want \"%s\", got \"%s\"",
                              id, want.c_str(), got);
                first_bad = buf;
            }
        }
    }

    std::printf("  %u strings compared\n", checked);
    std::printf("  %u mismatched, %u truncated\n", mismatched, truncated);
    std::printf("  %llu two-byte glyph lead bytes seen\n",
                (unsigned long long)lsw2_string_lead_bytes_seen());

    if (checked != count) {
        std::printf("FAILED - expected %u strings, compared %u\n", count, checked);
        return 1;
    }
    if (mismatched) {
        std::printf("FAILED - first mismatch: %s\n", first_bad.c_str());
        return 1;
    }
    std::printf("\nall %u strings match the exporter\n", checked);
    return 0;
}
