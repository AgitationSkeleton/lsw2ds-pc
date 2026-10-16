/* See scriptqueue.h. */

#include "scriptqueue.h"

void lsw2_script_enqueue(uint32_t obj, uint32_t index, uint32_t packed,
                         uint32_t argc, uint32_t args, const Lsw2ScriptIo& io) {
    lsw2_script_enqueue_with(
        obj, index, packed, argc,
        [&](uint32_t i) { return io.read32(args + i * 4u, io.ctx); }, io);
}
