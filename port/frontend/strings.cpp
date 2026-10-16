/* See strings.h. A port of the validated reference in tools/export_strings.py. */

#include "strings.h"

#include <cstring>

namespace {

uint64_t g_lead_bytes = 0;

/* The override table.
 *
 * Sized rather than grown. Sixteen is well past what the fork needs - three arena
 * menu entries, two popups and the host submenu titles - and a fixed array
 * keeps the lookup allocation-free, which matters because it runs inside a
 * guest call where there is nowhere to report a failure to. */
constexpr int kMaxOverrides = 16;
struct Override {
    uint32_t id = 0xFFFFFFFFu;
    const char* text = nullptr;
};
Override g_overrides[kMaxOverrides];

uint16_t read_u16(uint32_t addr, Lsw2ReadU8 read8, void* ctx) {
    return uint16_t(uint16_t(read8(addr, ctx)) |
                    (uint16_t(read8(addr + 1, ctx)) << 8));
}

uint32_t read_u32(uint32_t addr, Lsw2ReadU8 read8, void* ctx) {
    return uint32_t(read8(addr, ctx)) | (uint32_t(read8(addr + 1, ctx)) << 8) |
           (uint32_t(read8(addr + 2, ctx)) << 16) |
           (uint32_t(read8(addr + 3, ctx)) << 24);
}

}  // namespace

Lsw2StringResult lsw2_string_decode(uint32_t control_block, uint32_t id,
                                    char* out, uint32_t cap,
                                    Lsw2ReadU8 read8, void* ctx) {
    if (!out || cap == 0 || !read8) return Lsw2StringResult::NotReady;

    /* OUR REPLACEMENTS FIRST, and this is the whole point of the override.
     * Checked before the control block so a replacement works even before the
     * bank is loaded - which is exactly when a boot-time menu would ask. */
    if (const char* text = lsw2_string_override(id)) {
        const size_t len = std::strlen(text);
        if (len + 1 > cap) {
            std::memcpy(out, text, cap - 1);
            out[cap - 1] = '\0';
            return Lsw2StringResult::Truncated;
        }
        std::memcpy(out, text, len + 1);
        return Lsw2StringResult::Ok;
    }

    const uint32_t offsets = read_u32(control_block + 0, read8, ctx);
    const uint32_t data = read_u32(control_block + 4, read8, ctx);
    const uint32_t tree = read_u32(control_block + 8, read8, ctx);

    /* THE GUEST'S OWN EMPTINESS TEST, and it tests the DATA pointer rather than
     * the offset table - 0x0205CD2C loads [cb+4] and compares it against zero
     * before anything else. Matching that exactly matters: a decoder that
     * declared itself ready one field earlier would read an offset table that
     * is not there yet. */
    if (data == 0) return Lsw2StringResult::NotReady;

    uint32_t pointer = data + read_u32(offsets + id * 4u, read8, ctx);
    uint8_t current = read8(pointer++, ctx);
    int bit = 0;
    uint32_t written = 0;

    for (;;) {
        /* Walk from the root until the value drops below it, which is what
         * makes it a literal byte rather than a node. */
        uint16_t node = kLsw2StringRootNode;
        while (node >= kLsw2StringRootNode) {
            const uint32_t slot = tree + uint32_t(node) * 4u;
            /* Low bit first, and a fresh byte every eight - 0x0205CD8C. */
            node = ((current >> bit) & 1)
                       ? read_u16(slot - 0x3FEu, read8, ctx)   /* right child */
                       : read_u16(slot - 0x400u, read8, ctx);  /* left child  */
            if (++bit == 8) {
                current = read8(pointer++, ctx);
                bit = 0;
            }
        }

        const uint8_t byte = uint8_t(node & 0xFFu);
        if (byte == 0) {
            out[written] = '\0';
            return Lsw2StringResult::Ok;
        }
        /* The gap, counted. See strings.h - the guest treats these as the lead
         * of a two-byte sequence and we do not. */
        if (byte >= 0xF0u) ++g_lead_bytes;

        out[written++] = char(byte);
        if (written + 1 > cap) {
            out[cap - 1] = '\0';
            return Lsw2StringResult::Truncated;
        }
    }
}

const char* lsw2_string_override(uint32_t id) {
    for (const Override& o : g_overrides)
        if (o.id == id && o.text) return o.text;
    return nullptr;
}

void lsw2_string_set_override(uint32_t id, const char* text) {
    for (Override& o : g_overrides) {
        if (o.id == id) {
            o.text = text;
            if (!text) o.id = 0xFFFFFFFFu;
            return;
        }
    }
    if (!text) return;
    for (Override& o : g_overrides) {
        if (!o.text) {
            o.id = id;
            o.text = text;
            return;
        }
    }
    /* Full. Deliberately silent rather than asserting: this can be reached from
     * a guest call, and the table is sized well past what the fork needs, so
     * overflowing it means a caller is installing per-frame - a bug to find in
     * the caller, not a crash to cause here. */
}

uint64_t lsw2_string_lead_bytes_seen() { return g_lead_bytes; }
