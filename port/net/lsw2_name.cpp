/* See lsw2_name.h. */

#include "lsw2_name.h"

namespace {

/* PRINTABLE ASCII, tested as an unsigned value.
 *
 * `char` is signed on this toolchain, so a byte of 0xC3 - the lead of a UTF-8
 * accented character, and exactly the kind of thing a name from the wire
 * carries - arrives as -61. Comparing that against 0x20 without the cast says
 * "below space, drop it", which is the right answer by luck; comparing against
 * 0x7E says "below 0x7E, keep it", which is the wrong one. Casting first makes
 * both comparisons mean what they read as. */
bool printable(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return u >= 0x20u && u <= 0x7Eu;
}

}  // namespace

size_t lsw2_sanitise_name(const char* in, char* out, size_t cap,
                          const char* fallback) {
    if (!out || cap == 0) return 0;

    size_t written = 0;
    if (in) {
        bool pending_space = false;
        for (const char* p = in; *p && written + 1 < cap; ++p) {
            if (!printable(*p)) continue;
            if (*p == ' ') {
                /* Collapse runs, and never lead with one. A name of forty
                 * spaces would otherwise fill the slot and draw as nothing,
                 * which reads as a corrupted row rather than as a bad name. */
                pending_space = written > 0;
                continue;
            }
            if (pending_space && written + 2 < cap) {
                out[written++] = ' ';
            }
            pending_space = false;
            out[written++] = *p;
        }
    }
    out[written] = '\0';

    if (written == 0 && fallback) {
        size_t i = 0;
        for (; fallback[i] && i + 1 < cap; ++i) out[i] = fallback[i];
        out[i] = '\0';
        return i;
    }
    return written;
}
