/* The level-script command queue, reproduced from the guest and pinned.
 *
 * WHY THIS IS ITS OWN FILE. `dispatch_overrides.cpp` replaces `0x02060084`,
 * which is the ONE function that writes this queue - every scripted command in
 * the game goes through it. A hand-transcribed replacement for something on that
 * path is the highest-blast-radius change in the port, and it arrived with no
 * test at all: the only evidence it was right was that the disassembly had been
 * read carefully. This project has a standing record of careful readings that
 * were wrong (a phantom offset, a slot-0 miscount), and the
 * defence that works is a test against the semantics rather than another
 * reading.
 *
 * So the body lives here as a pure function over an abstract memory, the
 * override supplies the guest bus, and `test_scriptqueue.cpp` supplies a map.
 * Same shape as `strings.cpp` and `test_strings.cpp`.
 *
 * THE FORMAT, from `0x0205FFB8`'s consumer loop:
 *
 *     count  at  obj + 0xD4 + index*4        words currently queued
 *     queue  at  obj + index*0x20 + 0x94     the words themselves
 *
 * and one command is a packed word followed by its arguments inline:
 *
 *     word = opcode | (argc << 16)
 *     then argc words
 *
 * which is why the consumer advances `r8 += r6` - it steps over the arguments
 * using the count packed into the high half.
 */

#ifndef LSW2_SCRIPTQUEUE_H
#define LSW2_SCRIPTQUEUE_H

#include <cstdint>

/* Offsets on the script object. Read off the guest's own arithmetic:
 *     0x02060088  ADD r5, r0, #0xD4          the count base
 *     0x0206009C  ADD lr, r0, r1, LSL #5     the per-index stride, 0x20
 *     0x020600B4  STR r2, [r0, #+0x94]       the queue base
 */
constexpr uint32_t kLsw2ScriptCountBase = 0xD4u;
constexpr uint32_t kLsw2ScriptQueueBase = 0x94u;
constexpr uint32_t kLsw2ScriptQueueStride = 0x20u;

/* The command the fork cares about: opcode 26 shows a message popup, and its
 * first argument is the string id (dispatcher case at 0x0208D12C). */
constexpr uint32_t kLsw2ScriptOpShowMessage = 26u;

struct Lsw2ScriptIo {
    uint32_t (*read32)(uint32_t addr, void* ctx);
    void (*write32)(uint32_t addr, uint32_t value, void* ctx);
    void* ctx;
};

/* THE BODY, with the argument source left open.
 *
 * Two callers want this and they differ in exactly one way. The override
 * replacing `0x02060084` reads its arguments from GUEST memory, because that is
 * where the guest put them. The fork raising its own popup has the argument in
 * HOST memory, because it just made it up. Everything else - the count, the
 * header word, the order of the writes, the reload of the count inside the loop
 * - has to be identical, and writing it twice is how those two copies start
 * disagreeing six months from now.
 *
 * `fetch(i)` returns argument i. Templated rather than a callback because this
 * is on the path of every scripted command in the game. */
template <typename Fetch>
void lsw2_script_enqueue_with(uint32_t obj, uint32_t index, uint32_t packed,
                              uint32_t argc, Fetch fetch,
                              const Lsw2ScriptIo& io) {
    const uint32_t count_addr = obj + kLsw2ScriptCountBase + index * 4u;
    const uint32_t base = obj + index * kLsw2ScriptQueueStride;

    /* The guest ORs the count into the high half of whatever it was passed
     * rather than masking the opcode first (`0x020600A4`), so this does too. */
    const uint32_t first = io.read32(count_addr, io.ctx);
    io.write32(count_addr, first + 1u, io.ctx);
    io.write32(base + kLsw2ScriptQueueBase + first * 4u,
               packed | (argc << 16), io.ctx);

    /* EACH ARGUMENT RE-READS THE COUNT. The guest's loop reloads it every
     * iteration - `LDR r0, [r12, #0xD4]` at 0x020600C4 sits inside the loop,
     * not above it. Carrying it in a local would be identical for any call that
     * runs to completion and would stop being identical the moment anything
     * else wrote the count in between. Reproducing the guest is the job. */
    for (uint32_t i = 0; i < argc; ++i) {
        const uint32_t value = fetch(i);
        const uint32_t slot = io.read32(count_addr, io.ctx);
        io.write32(count_addr, slot + 1u, io.ctx);
        io.write32(base + kLsw2ScriptQueueBase + slot * 4u, value, io.ctx);
    }
}

/* `0x02060084`, faithfully. `args` is the address of the argument array, which
 * the guest receives as its fifth argument on the stack. */
void lsw2_script_enqueue(uint32_t obj, uint32_t index, uint32_t packed,
                         uint32_t argc, uint32_t args, const Lsw2ScriptIo& io);

/* (lsw2_script_enqueue_values - the same with the arguments in host memory -
 * was the fork popups' way in, and went with them on 2026-09-23: they were
 * queued into a script object nobody ran. dispatch_overrides.cpp.) */

#endif /* LSW2_SCRIPTQUEUE_H */
