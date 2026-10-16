/* The string decoder, checked against a bank this file builds itself.
 *
 * WHY A SYNTHETIC BANK. The real one lives in rebuilt RAM from a capture, which
 * is a 4MB input that does not belong in the repository and cannot be produced
 * on a machine without the capture. A bank built here is small, exact, and
 * exercises the parts that are easy to get wrong - bit order, the child-offset
 * arithmetic, the terminator and the capacity rule.
 *
 * WHAT IT DOES NOT PROVE. That the algorithm matches the GAME. That claim rests
 * on tools/export_strings.py, which decoded 1333 strings from the real bank and
 * self-validated against names captured independently from framebuffers. This
 * file only proves the C++ says the same thing as that Python.
 *
 * Build and run:
 *   g++ -O2 -std=c++20 -Iport/frontend port/frontend/test_strings.cpp \
 *       port/frontend/strings.cpp -o test_strings && ./test_strings
 */

#include "strings.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("  ok   %s\n", what);
    } else {
        ++g_failures;
        std::printf("  FAIL %s\n", what);
    }
}

/* A tiny addressable memory, so the decoder's callback has something to read. */
std::map<uint32_t, uint8_t> g_mem;

uint8_t read8(uint32_t addr, void*) {
    auto it = g_mem.find(addr);
    return it == g_mem.end() ? 0u : it->second;
}

void put8(uint32_t a, uint8_t v) { g_mem[a] = v; }
void put16(uint32_t a, uint16_t v) { put8(a, uint8_t(v)); put8(a + 1, uint8_t(v >> 8)); }
void put32(uint32_t a, uint32_t v) { put16(a, uint16_t(v)); put16(a + 2, uint16_t(v >> 16)); }

/* Layout of the bank this test builds. Addresses are arbitrary but must look
 * like RAM, because that is what the real control block holds. */
constexpr uint32_t kCb = 0x02100000;
constexpr uint32_t kOffsets = 0x02110000;
constexpr uint32_t kData = 0x02120000;
constexpr uint32_t kTree = 0x02130000;

/* A Huffman tree, built the way the game stores one.
 *
 * Node n's children live at tree + (n - 0x100) * 4 and + 2, the root is 0x100,
 * and anything below 0x100 is a literal byte. Nodes are numbered from 0x100 up
 * as they are created. */
struct TreeBuilder {
    uint16_t next_node = kLsw2StringRootNode;
    /* Codes assigned per byte, low bit first - the order the decoder reads. */
    std::map<uint8_t, std::vector<int>> codes;

    uint16_t alloc() { return next_node++; }

    void set_child(uint16_t node, int bit, uint16_t child) {
        /* -0x3FE ALREADY INCLUDES THE +2 for the right child. The first version
         * of this added 2 as well, putting every right child two bytes past
         * where the decoder looks - and the test caught it, which is the point
         * of having one. */
        put16(kTree + uint32_t(node) * 4 - (bit ? 0x3FEu : 0x400u), child);
    }
};

/* Build a tree where each byte in `alphabet` gets the code
 * 1,1,...,1,0 (index ones then a zero) - a degenerate but perfectly valid
 * Huffman shape, and one whose codes are trivially checkable by hand. */
void build_bank(const std::vector<uint8_t>& alphabet,
                const std::map<uint32_t, std::string>& strings) {
    TreeBuilder tb;
    uint16_t node = tb.alloc();  /* the root, 0x100 */
    for (size_t i = 0; i < alphabet.size(); ++i) {
        const bool last = (i + 1 == alphabet.size());
        tb.set_child(node, 0, alphabet[i]);            /* 0 -> this byte */
        std::vector<int> code(i, 1);
        code.push_back(0);
        tb.codes[alphabet[i]] = code;
        if (!last) {
            const uint16_t nxt = tb.alloc();
            tb.set_child(node, 1, nxt);                /* 1 -> keep walking */
            node = nxt;
        } else {
            /* The last symbol also owns the 1 branch, so no code is undefined. */
            tb.set_child(node, 1, alphabet[i]);
        }
    }

    put32(kCb + 0, kOffsets);
    put32(kCb + 4, kData);
    put32(kCb + 8, kTree);

    uint32_t cursor = 0;
    for (const auto& [id, text] : strings) {
        put32(kOffsets + id * 4, cursor);
        /* Emit the bits of each byte then the terminator, low bit first. */
        std::vector<int> bits;
        std::string with_terminator = text;
        with_terminator.push_back('\0');
        for (char c : with_terminator)
            for (int b : tb.codes[uint8_t(c)]) bits.push_back(b);
        uint8_t byte = 0;
        int n = 0;
        for (int b : bits) {
            if (b) byte = uint8_t(byte | (1u << n));
            if (++n == 8) {
                put8(kData + cursor++, byte);
                byte = 0;
                n = 0;
            }
        }
        if (n) put8(kData + cursor++, byte);
    }
}

std::string decode(uint32_t id, uint32_t cap, Lsw2StringResult* result) {
    std::vector<char> buf(cap + 8, '\xCC');
    *result = lsw2_string_decode(kCb, id, buf.data(), cap, read8, nullptr);
    return std::string(buf.data());
}

}  // namespace

int main() {
    std::printf("string decoder\n");

    const std::map<uint32_t, std::string> strings = {
        {0, "Host Internet Game"},
        {1, "A"},
        {2, ""},
        {7, "Join by IP or Room Code"},
    };
    std::vector<uint8_t> alphabet;
    {
        std::map<uint8_t, bool> seen;
        for (const auto& [id, s] : strings)
            for (char c : s) seen[uint8_t(c)] = true;
        seen[0] = true;  /* the terminator needs a code like any other byte */
        for (const auto& [b, _] : seen) alphabet.push_back(b);
    }
    build_bank(alphabet, strings);

    Lsw2StringResult r;
    for (const auto& [id, expected] : strings) {
        const std::string got = decode(id, 64, &r);
        char what[160];
        std::snprintf(what, sizeof what, "id %u decodes to \"%s\"", id,
                      expected.c_str());
        check(r == Lsw2StringResult::Ok && got == expected, what);
    }

    /* THE CAPACITY RULE, which is the easiest thing here to get off by one.
     * "Host Internet Game" is 18 bytes, so 19 is exactly enough. */
    decode(0, 19, &r);
    check(r == Lsw2StringResult::Ok, "a buffer of exactly length+1 is enough");
    const std::string cut = decode(0, 10, &r);
    check(r == Lsw2StringResult::Truncated, "a short buffer reports Truncated");
    check(cut.size() == 9 && cut == "Host Inte",
          "and it is terminated within the buffer");

    /* NOT READY. The guest tests the DATA pointer, not the offset table. */
    put32(kCb + 4, 0);
    decode(0, 64, &r);
    check(r == Lsw2StringResult::NotReady, "a zero data pointer reports NotReady");
    put32(kCb + 4, kData);

    /* THE OVERRIDES, including that they answer before the bank is consulted. */
    lsw2_string_set_override(1, "Replaced");
    check(std::string(lsw2_string_override(1) ? lsw2_string_override(1) : "") ==
              "Replaced",
          "an override is installed");
    check(decode(1, 64, &r) == "Replaced" && r == Lsw2StringResult::Ok,
          "and the decoder returns it instead of the bank");

    put32(kCb + 4, 0);
    check(decode(1, 64, &r) == "Replaced" && r == Lsw2StringResult::Ok,
          "an override answers even when the bank is not loaded");
    put32(kCb + 4, kData);

    check(decode(0, 64, &r) == "Host Internet Game",
          "and a non-overridden id is unaffected");

    lsw2_string_set_override(1, nullptr);
    check(lsw2_string_override(1) == nullptr, "an override can be cleared");
    check(decode(1, 64, &r) == "A", "and the bank answers again");

    /* The table must not leak entries as ids are installed and cleared. */
    for (int i = 0; i < 200; ++i) {
        lsw2_string_set_override(uint32_t(i), "x");
        lsw2_string_set_override(uint32_t(i), nullptr);
    }
    lsw2_string_set_override(3, "still works");
    check(std::string(lsw2_string_override(3)) == "still works",
          "install and clear cycles do not exhaust the table");
    lsw2_string_set_override(3, nullptr);

    check(lsw2_string_lead_bytes_seen() == 0,
          "no >= 0xF0 lead byte was seen in this bank");

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (g_failures == 0) std::printf("all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
