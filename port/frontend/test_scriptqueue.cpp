/* Pin the script-command queue against the format its CONSUMER requires.
 *
 * The override in dispatch_overrides.cpp replaces the one function that writes
 * this queue, so a mistake here breaks every scripted event in the game. The
 * reading it came from is careful; so were the readings that produced an
 * earlier phantom offset and a slot-0 miscount.
 *
 * WHAT MAKES THIS MORE THAN A RESTATEMENT. The checks below are not written
 * against the producer's disassembly - that would only prove the transcription
 * matches itself. They are written against the CONSUMER at 0x0205FFB8, which is
 * a different function that was read separately:
 *
 *     r2 = queue[r8]
 *     r7 = r2 & 0xFFFF        the opcode
 *     r6 = r2 >> 16           the argument count
 *     r9 = &queue[r8 + 1]     the arguments, inline
 *     r8 += r6                step over them
 *
 * So `decode_and_walk` below is the consumer's arithmetic, and the tests assert
 * that what the producer writes is what that arithmetic reads back. If either
 * reading is wrong, they disagree.
 */

#include "scriptqueue.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

/* A word-addressed memory that faults loudly on an unwritten read, because the
 * guest's own count starts at a value someone put there and reading rubbish
 * would silently pass. */
struct Memory {
    std::map<uint32_t, uint32_t> words;
    bool read_missing = false;
};

uint32_t mem_read(uint32_t addr, void* ctx) {
    Memory* m = static_cast<Memory*>(ctx);
    auto it = m->words.find(addr);
    if (it == m->words.end()) {
        m->read_missing = true;
        return 0;
    }
    return it->second;
}

void mem_write(uint32_t addr, uint32_t value, void* ctx) {
    static_cast<Memory*>(ctx)->words[addr] = value;
}

constexpr uint32_t kObj = 0x02200000u;
constexpr uint32_t kArgs = 0x02300000u;

uint32_t count_addr(uint32_t index) {
    return kObj + kLsw2ScriptCountBase + index * 4u;
}

uint32_t queue_addr(uint32_t index, uint32_t slot) {
    return kObj + index * kLsw2ScriptQueueStride + kLsw2ScriptQueueBase +
           slot * 4u;
}

struct Command {
    uint32_t opcode;
    std::vector<uint32_t> args;
};

/* THE CONSUMER'S ARITHMETIC, from 0x0205FFB8. Walks the queue the way the game
 * does and returns what it would dispatch. */
std::vector<Command> decode_and_walk(Memory& m, uint32_t index) {
    std::vector<Command> out;
    const uint32_t total = m.words.count(count_addr(index))
                               ? m.words[count_addr(index)] : 0u;
    uint32_t i = 0;
    while (i < total) {
        const uint32_t word = m.words[queue_addr(index, i)];
        Command c;
        c.opcode = word & 0xFFFFu;
        const uint32_t argc = word >> 16;
        for (uint32_t a = 0; a < argc; ++a)
            c.args.push_back(m.words[queue_addr(index, i + 1u + a)]);
        out.push_back(c);
        /* The guest steps r8 += 1 (the header, added by the loop's own ++) and
         * then += argc, which is `r8 += r6` after the increment at 0x02060024. */
        i += 1u + argc;
    }
    return out;
}

Lsw2ScriptIo io_for(Memory& m) {
    return Lsw2ScriptIo{mem_read, mem_write, &m};
}

void put_args(Memory& m, const std::vector<uint32_t>& args) {
    for (size_t i = 0; i < args.size(); ++i)
        m.words[kArgs + uint32_t(i) * 4u] = args[i];
}

void test_no_arguments() {
    Memory m;
    m.words[count_addr(0)] = 0;
    lsw2_script_enqueue(kObj, 0, 12u, 0u, kArgs, io_for(m));

    check(!m.read_missing, "argc 0: nothing unwritten was read");
    check(m.words[count_addr(0)] == 1, "argc 0: the count advanced by one");
    check(m.words[queue_addr(0, 0)] == 12u, "argc 0: the header is the opcode");

    const auto cmds = decode_and_walk(m, 0);
    check(cmds.size() == 1 && cmds[0].opcode == 12u && cmds[0].args.empty(),
          "argc 0: the consumer reads back one bare command");
}

void test_one_argument() {
    Memory m;
    m.words[count_addr(0)] = 0;
    put_args(m, {0x2D7u});
    lsw2_script_enqueue(kObj, 0, kLsw2ScriptOpShowMessage, 1u, kArgs, io_for(m));

    check(m.words[count_addr(0)] == 2, "argc 1: the count advanced by two");
    const auto cmds = decode_and_walk(m, 0);
    check(cmds.size() == 1, "argc 1: exactly one command");
    check(cmds[0].opcode == kLsw2ScriptOpShowMessage, "argc 1: opcode survives");
    check(cmds[0].args.size() == 1 && cmds[0].args[0] == 0x2D7u,
          "argc 1: the message id is argument zero");
}

void test_many_arguments_in_order() {
    Memory m;
    m.words[count_addr(0)] = 0;
    put_args(m, {11u, 22u, 33u, 44u});
    lsw2_script_enqueue(kObj, 0, 5u, 4u, kArgs, io_for(m));

    check(m.words[count_addr(0)] == 5, "argc 4: the count advanced by five");
    const auto cmds = decode_and_walk(m, 0);
    check(cmds.size() == 1 && cmds[0].args.size() == 4, "argc 4: one command");
    check(cmds[0].args[0] == 11 && cmds[0].args[1] == 22 &&
              cmds[0].args[2] == 33 && cmds[0].args[3] == 44,
          "argc 4: arguments keep their order");
}

void test_appends_after_existing() {
    /* THE COUNT IS NOT RESET. The producer appends at whatever the count says,
     * which is what makes several commands per frame work at all - and getting
     * this wrong would overwrite the first command of every frame with the
     * second, a bug that looks like the script skipping steps. */
    Memory m;
    m.words[count_addr(0)] = 0;
    put_args(m, {0x2D7u});
    lsw2_script_enqueue(kObj, 0, kLsw2ScriptOpShowMessage, 1u, kArgs, io_for(m));
    put_args(m, {7u, 8u});
    lsw2_script_enqueue(kObj, 0, 3u, 2u, kArgs, io_for(m));

    check(m.words[count_addr(0)] == 5, "append: two commands total five words");
    const auto cmds = decode_and_walk(m, 0);
    check(cmds.size() == 2, "append: the consumer sees both commands");
    check(cmds[0].opcode == kLsw2ScriptOpShowMessage && cmds[0].args.size() == 1,
          "append: the first command is intact");
    check(cmds[1].opcode == 3u && cmds[1].args.size() == 2 &&
              cmds[1].args[0] == 7u && cmds[1].args[1] == 8u,
          "append: the second command follows it");
}

void test_indices_are_separate_queues() {
    /* Per-player. Index 1's queue must not touch index 0's, and the stride that
     * separates them (0x20 words apart in the base, 4 bytes in the count) comes
     * from two different instructions - so a mistake in either shows up here. */
    Memory m;
    m.words[count_addr(0)] = 0;
    m.words[count_addr(1)] = 0;
    put_args(m, {111u});
    lsw2_script_enqueue(kObj, 0, 1u, 1u, kArgs, io_for(m));
    put_args(m, {222u});
    lsw2_script_enqueue(kObj, 1, 2u, 1u, kArgs, io_for(m));

    const auto a = decode_and_walk(m, 0);
    const auto b = decode_and_walk(m, 1);
    check(a.size() == 1 && a[0].opcode == 1u && a[0].args[0] == 111u,
          "indices: player 0 kept its own command");
    check(b.size() == 1 && b[0].opcode == 2u && b[0].args[0] == 222u,
          "indices: player 1 kept its own command");
    check(m.words[count_addr(0)] == 2 && m.words[count_addr(1)] == 2,
          "indices: the counts are independent");
}

void test_header_matches_the_consumers_split() {
    /* The single most load-bearing claim: the consumer splits the header at bit
     * 16. If the producer packed them the other way round, every command would
     * dispatch as an enormous opcode with a tiny argument count. */
    Memory m;
    m.words[count_addr(0)] = 0;
    put_args(m, {0xAAAAu, 0xBBBBu, 0xCCCCu});
    lsw2_script_enqueue(kObj, 0, kLsw2ScriptOpShowMessage, 3u, kArgs, io_for(m));

    const uint32_t header = m.words[queue_addr(0, 0)];
    check((header & 0xFFFFu) == kLsw2ScriptOpShowMessage,
          "header: the opcode is the low halfword");
    check((header >> 16) == 3u, "header: the count is the high halfword");
}

}  // namespace

int main() {
    test_no_arguments();
    test_one_argument();
    test_many_arguments_in_order();
    test_appends_after_existing();
    test_indices_are_separate_queues();
    test_header_matches_the_consumers_split();

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
