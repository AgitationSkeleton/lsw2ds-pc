/* See guestcall.h. */

#include "guestcall.h"

#include "state.h"
#include "runtime_arm.h"
/* nds_cpu_halted / nds_dma_cpu_stalled - two of runtime_should_yield()'s own
 * terminal-halt conditions, so the seam can name the one that refused a call
 * instead of inferring it from the symptom. */
#include "io.h"

#include <cstdio>
#include <cstdlib>

namespace {

/* OUTSIDE GUEST ADDRESS SPACE, deliberately - a value the guest can never
 * legitimately return to cannot collide with a real return further up the
 * call-return stack. */
constexpr uint32_t kHostReturn = 0x0FFFFFF0u;

/* A handful of reports, not one per frame - the callers already rate-limit
 * themselves and this is the seam underneath them. */
int g_reports = 0;

/* Filled in when the event pump gives up - see the stall branch. */
bool g_stall_pumps_left = false;
unsigned long long g_stall_now = 0;
unsigned long long g_stall_next = 0;
constexpr int kReports = 8;

/* See lsw2_guest_ready. Totals, not rates - a rate hides a condition that is
 * true almost always. */
unsigned long long g_ready_calls = 0;
unsigned long long g_ready_not_arm9 = 0;
unsigned long long g_ready_halted = 0;
unsigned long long g_ready_dma = 0;
unsigned long long g_ready_terminal = 0;

/* LSW2_CALL_BUDGET raises it for one experiment; 200,000 is the shipping
 * value. Hoisted out of the call so the resume loop below can renew it - a
 * slice yield that is not renewed trips again on the very next backward
 * branch, which would make resuming pointless. */
const unsigned long long kCallBudget = [] {
    if (const char* s = std::getenv("LSW2_CALL_BUDGET")) {
        const unsigned long long n = std::strtoull(s, nullptr, 0);
        if (n) return n;
    }
    return 200000ull;
}();

/* HOW MANY TIMES A PREEMPTED CALL MAY BE RESUMED, and how much DMA to advance
 * per attempt.
 *
 * The chunk is one scheduler quantum's worth: enough that a portrait's worth
 * of tiles moves in a few passes, small enough that the loop keeps checking
 * whether the transfer is done rather than running the machine forward blindly.
 * The resume count then bounds the whole thing - sixty-four passes is far more
 * than any of these calls needs and is still a bounded failure. */
/* RAISED FROM 64, because 64 was not a failure - it was the bound.
 *
 * The WH session start reported `resumes=64 RESUME LIMIT` with zero unmapped
 * reads and the layer's state moved STOP -> BUSY on the way, which is a call
 * doing real work and running out of permission rather than a call going
 * wrong. Four thousand is generous for anything the port asks for and is still
 * a bounded frame.
 *
 * AND A CALL THAT IS NOT MOVING IS STOPPED EARLY, which matters more than the
 * ceiling: a guest spin re-dispatches at the same PC forever and would burn the
 * whole allowance looking busy. kStallLimit consecutive dispatches at one PC,
 * with no DMA advancing, is a spin - and saying so is the difference between
 * "needs more resumes" and "waiting for something that will never come". */
constexpr int kMaxResumes = 4096;

/* TWO, NOT THIRTY-TWO. The first version waited for thirty-two consecutive
 * dispatches at one PC before pumping the event queue, and a card read needs a
 * pump per data word - so it spent 32 wasted re-dispatches buying one word and
 * ran out of resumes with the transfer unfinished (`resumes=4096 events=127`).
 * The PC repeating even once already means the guest is waiting rather than
 * working; there is nothing to gain by watching it wait thirty more times. */
constexpr int kStallLimit = 2;
constexpr unsigned long long kDmaChunkCycles = 4096;

/* AND A SPIN IS NOT ALWAYS THE GUEST'S FAULT - IT IS USUALLY OURS.
 *
 * MEASURED on the customization gear applier. Applying a chosen hat reported
 *
 *     [guestcall] 020063D4 did not return: r15=020D5ABC resumes=37
 *                 dma_chunks=0  SPINNING AT ONE PC
 *
 * and that address disassembles to the DS cartridge read loop:
 *
 *     020D5AB4  str r0, [r2]            ; the command, into ROMCTRL
 *     020D5ABC  ldr r3, [r2]
 *     020D5AC0  tst r3, #0x800000       ; bit 23 - a data word is ready
 *     020D5AD8  tst r3, #0x80000000     ; bit 31 - the block is still busy
 *     020D5ADC  bne 0x20d5abc           ; ... so go round again
 *
 * The applier has to LOAD THE PART'S MODEL OFF THE CART, and io.cpp advances
 * that transfer from a SCHEDULED EVENT - `schedule_card_event` - which the
 * emulator's own loop services between instructions. A host call runs between
 * frames, where that loop is not running, so the busy bit never clears and the
 * guest spins on it forever.
 *
 * Which is the DMA finding again with a different device, and the framework
 * offers the same remedy: `nds_run_system_events` and
 * `nds_next_system_event_time` are both public in io.h, and scheduler.cpp
 * drives every device through exactly that pair. So on a spin the seam runs
 * the machine's own event queue forward to the next event and re-dispatches -
 * the card completes, the applier returns, and a genuine deadlock is still
 * caught because the pump count is bounded.
 *
 * THE HORIZON KEEPS IT HONEST. Only events already due, or due within a short
 * window, are run: `nds_next_system_event_time` returns something enormous when
 * the queue is empty, and running to that would fire every future event at
 * once. A spin with no event pending really is a deadlock and is reported as
 * one. */
constexpr int kMaxEventPumps = 4096;
constexpr unsigned long long kEventHorizon = 1u << 20;

}  // namespace

bool lsw2_guest_ready() {
    /* The three of runtime_should_yield()'s terminal-halt conditions the port
     * can observe. The others - an insn/event break, self-modified code under
     * the active function, a break PC - are debugger and tooling states that do
     * not occur in a normal run, and the cycle cap is granted by the call
     * itself just below. See guestcall.h. */
    /* AND THE ARM9 MUST BE THE ACTIVE CORE.
     *
     * MEASURED 2026-09-02. The seam's own failure report has printed
     * `active_cpu` since it was added, and it caught this:
     *
     *     [guestcall] 0205E80C did not return: unwinding=1
     *                 r15=0FFFFFF0 (wanted 0FFFFFF0) active_cpu=ARM7
     *
     * That call is the cartridge's ARM9 allocator. It reached its return pad -
     * r15 is the pad - while the ARM7 was the active core, which means every
     * instruction in it ran against the ARM7's memory map. runtime_arm.h is
     * explicit that the map branches on this: "The bus view (TCM, per-CPU I/O)
     * and the exception-vector base branch on this."
     *
     * The report that named it also predicted the symptom: "a call that reaches
     * its return pad, followed by [bus] ARM7 unmapped read at addresses that
     * are not addresses". A dispatch miss at pc=0x99054788 was logged in the
     * same run.
     *
     * THIS CHECK WAS THE MISSING HALF. Asking whether the ARM9 is halted and
     * then running on whichever core the scheduler last left active are two
     * different questions, and only the first was ever asked - so every host
     * call in the port has been rolling this dice, not only the new one that
     * exposed it. Refusing costs a frame; not refusing runs ARM9 code on the
     * wrong machine. */
    const bool arm9 = g_nds_active == NDS_ARM9;
    const bool halted = nds_cpu_halted(NDS_ARM9);
    const bool stalled = nds_dma_cpu_stalled(NDS_ARM9);

    /* COUNTED, BECAUSE THIS PREDICATE GATES EVERY HOST CALL IN THE PORT.
     *
     * The ARM9-active condition was added on 2026-09-02 for a real hazard - the
     * seam had been running ARM9 code while the ARM7 was current - but it
     * refuses more often than the check it replaced, and a chooser gate logged
     * `guest ready 0` five times in a row. The customization dressing, the
     * customization weapon and the party-portrait refresh all come through
     * here, so "how often is this true, and which condition says no" is not a
     * curiosity; it decides whether three required features are quietly not
     * running. Four counters and no formatting. */
    ++g_ready_calls;
    if (!arm9) ++g_ready_not_arm9;
    if (halted) ++g_ready_halted;
    if (stalled) ++g_ready_dma;
    if (g_nds_terminal) ++g_ready_terminal;

    return arm9 && !halted && !stalled && !g_nds_terminal;
}

void lsw2_guest_ready_report() {
    /* ONCE A SECOND WHILE ANYTHING IS ASKING, and silent when nothing is - a
     * run that makes no host calls should not be told about it sixty times a
     * minute. */
    static unsigned long long last_calls = 0;
    static int countdown = 0;
    if (g_ready_calls == last_calls) return;
    if (--countdown > 0) return;
    countdown = 60;

    const unsigned long long asked = g_ready_calls - last_calls;
    last_calls = g_ready_calls;
    std::fprintf(stderr,
                 "[guestcall] ready: %llu asked in total, %llu since the last "
                 "line; refused by not-ARM9 %llu, halted %llu, dma %llu, "
                 "terminal %llu" "\n",
                 g_ready_calls, asked, g_ready_not_arm9, g_ready_halted,
                 g_ready_dma, g_ready_terminal);
}

bool lsw2_guest_call(Lsw2LiftedFn fn, uint32_t target,
                     uint32_t r0, uint32_t r1, uint32_t r2,
                     uint32_t r3, int stack_words,
                     uint32_t stack0, uint32_t stack1) {
    if (!fn) return false;

    /* REFUSED BEFORE IT WAS EVEN TRIED, and this guard is where three required
     * features have been dying without saying so.
     *
     * The callers print "DID NOT RETURN", which reads as "the call was made
     * and went wrong". Measured 2026-08-31: the diagnostic further down never
     * printed at all, so the failure is HERE - the call is never attempted,
     * and the message the player-facing log carries is describing something
     * that did not happen.
     *
     * runtime_unwinding() being true on ENTRY means the fork tick is running
     * while the runtime is mid slice-yield unwind. That is a fact about WHEN
     * the tick is called, not about the target, which is why all three targets
     * fail identically and why the target-by-target investigation kept finding
     * nothing wrong with any of them. */
    /* A HOST-INITIATED CALL IS A FRESH ENTRY, so it clears the unwind flag the
     * same way the scheduler does before its own dispatch.
     *
     * THIS GUARD WAS REFUSING EVERY CALL. Measured 2026-08-31: three targets -
     * the gear applier 0x020063D4, the weapon setter 0x02004404 and the party
     * refresh 0x020691B4 - all reported "DID NOT RETURN", and the diagnostic
     * further down never printed once, which means the call was never made.
     * The customization dressing, the customization weapon and the party
     * portrait were all failing here, at
     * a `return false` two lines into the function, while their callers logged
     * messages describing a call that had gone wrong.
     *
     * WHY THE FLAG IS SET AT ALL. `runtime_unwinding()` is armed by the
     * scheduler's cooperative slice-yield at a backward branch, and it is
     * cleared in exactly one place - scheduler.cpp, immediately before each
     * dispatch, under the comment "a fresh dispatch is a real entry, not an
     * unwind". The fork tick runs BETWEEN frames, after a slice ended and
     * before the next dispatch clears it, so the flag is true every single
     * time. guestcall.h's "with the guest settled" was right about the timing
     * and wrong about what the flag means there.
     *
     * WHY CLEARING IT HERE IS SAFE, and this is the part worth checking rather
     * than assuming. The scheduler's loop is clear -> dispatch -> restore:
     *
     *     nds_clear_unwinding();
     *     runtime_dispatch(pc | t);
     *     nds_restore_unwind_state();
     *
     * so the preserved state's whole lifetime is inside one dispatch, and by
     * the time a between-frames tick runs, `nds_restore_unwind_state()` has
     * already consumed it. There is no pending unwind to destroy - only a
     * stale flag. And the scheduler clears it again before its next dispatch
     * regardless, so leaving it cleared changes nothing about the guest.
     *
     * nds_clear_unwinding() is the framework's own sanctioned accessor,
     * declared extern "C" in state.h, not a reach into its internals. */
    if (runtime_unwinding()) nds_clear_unwinding();

    /* AND GIVE THE CALL A CYCLE BUDGET, or it yields on its first loop.
     *
     * Clearing the flag above got the call ATTEMPTED and it still failed, with
     * the diagnostic naming the reason exactly:
     *
     *     [guestcall] 02004404 did not return: unwinding=1 r15=02004404
     *
     * R15 back at the target's own entry is the slice-yield contract working
     * as designed - "R15 is a dispatch entry, so re-dispatch is clean". The
     * function entered, hit a backward branch, and was preempted.
     *
     * WHY IMMEDIATELY. runtime_slice_yield trips on
     * `g_runtime_cycles >= g_cycle_cap`, and between frames that is already
     * true - reaching the cap is WHY the slice ended and why the fork tick is
     * running at all. So every host-initiated call was being preempted at its
     * first loop top, forever, no matter what it was calling. That is the one
     * cause behind all three targets, and it is why nothing was ever wrong
     * with any of them.
     *
     * BOUNDED, AND THE SCHEDULER TAKES IT BACK. nds_slice_begin is the
     * framework's own extern "C" entry point and the scheduler calls it at the
     * top of every slice, so this raise is undone on the next one without
     * needing to be restored here. The budget is deliberately finite: a guest
     * function that spins would otherwise hang the frame instead of being
     * preempted, which is the failure this whole mechanism exists to prevent.
     * Dressing an actor is a few thousand cycles; this is generous by two
     * orders of magnitude and still under a millisecond of guest time. */
    /* RAISED 2026-09-01 FROM 200,000, because a real call outgrew it.
     *
     * The party portrait's chain is party_refresh then redraw_page. The refresh returns; the
     * REDRAW does not:
     *
     *     [guestcall] 02068D74 did not return: unwinding=1 r15=01FFD584
     *     [fork] redraw page DID NOT RETURN - not calling it again
     *
     * unwinding=1 after the call is the slice yield firing again, which at this
     * seam means the budget ran out mid-call. Redrawing a page of portraits is
     * more work than dressing an actor - it touches VRAM and walks eight
     * entries - so 200,000 was sized from the wrong example.
     *
     * STILL FINITE, and that is the point of having a number at all: a guest
     * function that spins must be preempted rather than hang the frame. Two
     * million cycles is about 60ms of guest time at the ARM9's clock - far more
     * than any of these calls should need, and still a bounded failure rather
     * than an unbounded one.
     *
     * AND IT WAS NOT THE CAUSE. Raised to 2,000,000 and re-run: the redraw
     * failed identically, same r15 to the digit -
     *
     *     [guestcall] 02068D74 did not return: unwinding=1 r15=01FFD584
     *
     * Ten times the budget changing nothing at all is what rules a budget out,
     * so the number goes back to what was justified rather than staying raised
     * on the strength of a theory that failed. 200,000 is still generous for
     * every call that DOES return - dressing an actor and setting a weapon both
     * complete well inside it.
     *
     * THE REMAINING THREAD is that r15. 0x01FFD584 is below main RAM, in ITCM,
     * and it is identical across runs, so the redraw ends up somewhere the
     * return check cannot recognise rather than being preempted at a loop top -
     * a slice yield leaves r15 at a dispatch entry in guest RAM, and this is
     * not one. No dispatch miss is logged, so "unlifted code" is not confirmed
     * either. That is where the party-portrait redraw stands. */
    /* LSW2_CALL_BUDGET raises it for one experiment. 200,000 is the shipping
     * value and is generous for the calls that return today - dressing an
     * actor, setting a weapon, refreshing a party. The round-start dispatch is
     * a different animal: it enters the arena flow and seeds the RNG, and it
     * was measured being preempted at 0x02062A40 with real progress made, which
     * is a budget question rather than a refusal. This makes that measurable
     * without guessing a new constant into the shipping path. */
    nds_slice_begin(g_runtime_cycles + kCallBudget);

    /* WHICH TERMINAL-HALT CONDITION IS PENDING, named before the call.
     *
     * MEASURED 2026-09-01, from the generated code rather than guessed. Every
     * lifted function begins:
     *
     *     L_02073054:
     *         g_cpu.R[15] = 0x02073054u;
     *         if (runtime_should_yield()) return;
     *
     * so a call that comes straight back with r15 STILL AT THE ENTRY - which is
     * exactly what the count bootstrap's three setters did - never executed an
     * instruction. It was refused at the door by runtime_should_yield(), and
     * that is a DIFFERENT signal from the slice yield this function budgets
     * for. runtime_arm.h calls it the "per-instruction unwind: terminal halts
     * only"; nds_slice_begin above cannot clear it and never could.
     *
     * Its conditions are, in order: an insn/event break, the CPU being HALTED
     * or DMA-STALLED, self-modified code under the active function, a break PC,
     * the cycle cap, and g_nds_terminal. Two of those are ordinary and likely
     * here: the fork tick runs BETWEEN FRAMES, and between frames the guest is
     * typically sitting in a HALT waiting for VBlank - which would refuse every
     * host call made from the tick while leaving calls made from inside a
     * dispatch override working, which is exactly the pattern this port sees.
     *
     * So the port asks the framework which one, instead of inferring it from
     * the symptom for a fourth time. */
    if (nds_cpu_halted(NDS_ARM9) || nds_dma_cpu_stalled(NDS_ARM9) ||
        g_nds_terminal) {
        static int said = 0;
        if (said < 4) {
            ++said;
            std::fprintf(stderr,
                         "[guestcall] %08X refused before entry: halted=%d "
                         "dma_stalled=%d terminal=%d\n",
                         unsigned(target), int(nds_cpu_halted(NDS_ARM9)),
                         int(nds_dma_cpu_stalled(NDS_ARM9)),
                         int(g_nds_terminal));
        }
    }

    const ArmCpuState saved = g_cpu;

    g_cpu.R[0] = r0;
    g_cpu.R[1] = r1;
    g_cpu.R[2] = r2;
    g_cpu.R[3] = r3;

    /* ARGUMENTS FIVE AND SIX GO ON THE STACK, below the callee's frame, which
     * is where the ARM ABI puts them and where the callee will look. r13 is
     * part of the saved state restored at the end, so this borrows the guest's
     * stack for the duration of one call and gives it back. */
    if (stack_words > 0) {
        const uint32_t words[2] = {stack0, stack1};
        for (int i = stack_words - 1; i >= 0; --i) {
            g_cpu.R[13] -= 4u;
            bus_device_write32(0, g_cpu.R[13], words[i]);
        }
    }

    g_cpu.R[14] = kHostReturn;
    g_cpu.R[15] = target;

    runtime_call_push_return(kHostReturn);
    fn();

    /* A CALL THAT STARTED A DMA IS NOT FINISHED, IT IS WAITING.
     *
     * MEASURED 2026-09-02, and the address is the whole finding. Two unrelated
     * host calls - the party-portrait redraw and the WH session start - both stop
     * at exactly r15 = 0x01FFD584, in ITCM. Read out of a running instance and
     * disassembled, that is one instruction past a DMA kick:
     *
     *     01FFD564  mul lr, r0, ip        ; channel * 12
     *     01FFD570  str r1, [lr, #0xb0]   ; 0x040000B0 - DMAxSAD
     *     01FFD578  str r2, [ip, #4]      ; DMAxDAD
     *     01FFD580  str r3, [ip, #8]      ; DMAxCNT - the transfer starts here
     *     01FFD584  ldr r1, [r2]          ; <- and here the CPU is stalled
     *
     * io.h says why: "An enabled transfer stalls that CPU while the scheduler
     * advances DMA bus units in place of guest instructions." The fork tick
     * runs BETWEEN frames, so that scheduler is not running, so the transfer
     * never advances and the call unwinds forever. It was never a budget - a
     * 250x raise changed nothing, twice, for two different callees.
     *
     * SO THE SEAM DOES WHAT THE SCHEDULER DOES. scheduler.cpp's run_slice is
     * four lines:
     *
     *     if (nds_dma_cpu_stalled(cpu)) { switch_to(cpu);
     *         nds_dma_run(cpu, cycles + quantum); return; }
     *
     * and nds_dma_run is public in io.h. Advance the transfer, clear the
     * unwind, re-dispatch at the saved PC - runtime_arm.h guarantees that is
     * clean, because "the call-return stack is saved/restored per-CPU across
     * the preemption, so the guest return survives".
     *
     * BOUNDED, because a call that will never finish must not hang the frame,
     * which is the whole reason the preemption exists. */
    /* ARM THE BRANCH RING AROUND THIS CALL, off unless
     * LSW2_TRACE_GUESTCALL=1. Deep trace records both cores and is expensive
     * enough that runtime_arm.h warns it stops the run if left on, so it is
     * armed here and switched off the moment the call is over - a window of one
     * guest call rather than a session. */
    static int trace_arm = -1;
    if (trace_arm < 0) {
        const char* v = std::getenv("LSW2_TRACE_GUESTCALL");
        trace_arm = (v && *v == '1') ? 1 : 0;
    }
    if (trace_arm == 1) {
        runtime_set_deep_trace(1);
        runtime_trace_reset();
    }

    int resumes = 0;
    int dma_chunks = 0;
    int pumps = 0;
    bool hit_terminal = false;
    bool hit_stall = false;
    uint32_t last_pc = 0;
    int same_pc = 0;
    for (int resume = 0; resume < kMaxResumes; ++resume) {
        if (g_cpu.R[15] == kHostReturn) break;

        /* NOT `while (unwinding)`, AND THE DIFFERENCE COST A DIAGNOSIS.
         *
         * runtime_dispatch returns at a NATURAL BOUNDARY as well as on a
         * preemption - runtime_arm.h says so in as many words - and at a
         * natural boundary the unwind flag is false. A loop that stopped there
         * left the call parked mid-chain with r15 in the middle of the guest's
         * own code, which reads exactly like a fault and is not one.
         *
         * scheduler.cpp does not make that mistake: run_slice's condition is
         * `while (g_runtime_cycles < cap && !halted)` - it keeps dispatching
         * until the work is done or the budget is gone, and the unwind flag
         * never appears in it. This now matches. The bound is the resume count
         * and the terminal check below, which are the two real stopping
         * conditions. */

        /* A TERMINAL HALT IS NOT A PREEMPTION, AND MUST NOT BE RESUMED.
         *
         * runtime_should_yield arms the same unwind flag for both, so the loop
         * cannot tell them apart from the flag alone - and nds_slice_begin
         * CLEARS g_nds_terminal as part of arming a cap. The first version of
         * this loop therefore erased a real fault and re-dispatched into it,
         * which is strictly worse than the stall it was written to fix:
         * a dispatch miss or an unlowered op would have been silently retried
         * sixty-four times.
         *
         * Checked BEFORE nds_slice_begin, because after it the evidence is
         * gone. */
        if (g_nds_terminal) { hit_terminal = true; break; }

        ++resumes;

        if (nds_dma_cpu_stalled(NDS_ARM9)) {
            nds_dma_run(NDS_ARM9, g_runtime_cycles + kDmaChunkCycles);
            ++dma_chunks;
            /* Still moving bytes - give it another chunk rather than
             * re-dispatching into a CPU that is still stalled. */
            if (nds_dma_cpu_stalled(NDS_ARM9)) continue;
        }

        /* NOT A DMA STALL, so this is the ordinary slice yield and the cap has
         * to be renewed or the very next backward branch trips it again. */
        /* THE WAY THE SCHEDULER DOES IT, to the letter. scheduler.cpp:
         *
         *     uint32_t pc = g_cpu.R[15];
         *     uint32_t t  = (g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u;
         *     nds_clear_unwinding();
         *     runtime_dispatch(pc | t);
         *     nds_restore_unwind_state();
         *
         * THE THUMB BIT IS NOT OPTIONAL. Dispatching a Thumb PC without it
         * decodes Thumb code as ARM, and the first version of this loop
         * omitted it - which is the likeliest reason a resumed call kept
         * ending at addresses that were not its return pad. The restore
         * afterwards is the other half of the same contract: it puts back the
         * state a nested unwind captured, so the observer lands on the true
         * PC rather than on a host frame's idea of it. */
        const uint32_t pc = g_cpu.R[15];

        /* THE SAME PC, OVER AND OVER, WITH NOTHING MOVING, IS A SPIN. Reported
         * rather than run out the allowance on, because the two need different
         * answers: a spin means the guest is waiting for something this port
         * does not provide, and no number of resumes will change that. */
        if (pc == last_pc) {
            if (++same_pc >= kStallLimit) {
                /* THE MACHINE'S OWN EVENT QUEUE, run forward. See the note on
                 * kEventHorizon: a guest spinning on a device register is
                 * waiting for a scheduled event, and between frames nothing
                 * else will deliver it. */
                const unsigned long long sys_now =
                    (g_nds_active == NDS_ARM9) ? (g_runtime_cycles >> 1)
                                               : g_runtime_cycles;
                const unsigned long long next = nds_next_system_event_time();
                if (pumps < kMaxEventPumps && next <= sys_now + kEventHorizon) {
                    nds_run_system_events(next > sys_now ? next : sys_now + 1);
                    ++pumps;
                    same_pc = 0;
                } else {
                    /* SAY WHY THE PUMP STOPPED, because the two reasons need
                     * different fixes and the report has never told them apart:
                     * the pump BUDGET running out means the guest is waiting on
                     * something that keeps rescheduling, while NOTHING DUE
                     * means the interrupt it is waiting for is not scheduled at
                     * all - no amount of pumping can deliver it. */
                    g_stall_pumps_left = pumps < kMaxEventPumps;
                    g_stall_now = sys_now;
                    g_stall_next = next;
                    hit_stall = true;
                    break;
                }
            }
        } else {
            last_pc = pc;
            same_pc = 0;
        }

        const uint32_t thumb = (g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u;
        nds_slice_begin(g_runtime_cycles + kCallBudget);
        nds_clear_unwinding();
        runtime_dispatch(pc | thumb);
        nds_restore_unwind_state();
    }

    /* WHICH OF THE TWO FAILED, because they are different events and this file
     * has been reporting them as one.
     *
     * Three separate targets - the gear applier 0x020063D4, the weapon setter
     * 0x02004404 and the party refresh 0x020691B4 - all report "DID NOT
     * RETURN", which is three required features (the customization dressing,
     * the customization weapon, the party portrait) failing through one seam. A single cause is far more
     * likely than three, and the two conditions below point at very different
     * ones:
     *
     *   UNWINDING is the scheduler's cooperative slice yield, armed at loop
     *   tops so the other core can run. runtime_arm.h is explicit that this is
     *   NOT a failure - "the call-return stack is saved/restored per-CPU across
     *   the preemption, so the guest return survives". A function with a loop
     *   in it can be preempted this way through no fault of the caller.
     *
     *   R15 NOT AT THE PAD is a real miss: the guest returned somewhere else,
     *   or a dispatch landed on another entry.
     *
     * Until now both produced the same false and the same cancel. Counting
     * them separately is one line and it decides whether the fix is in this
     * file or in the callers. */
    const bool unwinding = runtime_unwinding();
    const bool at_pad = g_cpu.R[15] == kHostReturn;
    const bool returned = !unwinding && at_pad;

    if (!returned && g_reports < kReports) {
        ++g_reports;
        /* WHICH CORE WAS ACTIVE, because it decides what the call even means.
         *
         * runtime_arm.h: "The bus view (TCM, per-CPU I/O) and the
         * exception-vector base branch on this." So running an ARM9 lifted
         * function while g_nds_active is ARM7 executes ARM9 code against the
         * ARM7's memory map, and the symptom would be exactly what the round
         * start produced - a call that reaches its return pad, followed by
         * "[bus] ARM7 unmapped read" at addresses that are not addresses.
         *
         * This seam has never checked. lsw2_guest_ready() asks whether ARM9 is
         * halted and then the call runs on whichever core the scheduler last
         * left active, which is a different question. Printing it is the
         * cheapest way to find out whether that is what has been happening. */
        if (trace_arm == 1) runtime_set_deep_trace(0);
        std::fprintf(stderr,
                     "[guestcall] %08X did not return: unwinding=%d "
                     "r15=%08X (wanted %08X) active_cpu=%s resumes=%d "
                     "dma_chunks=%d events=%d%s\n",
                     unsigned(target), int(unwinding), unsigned(g_cpu.R[15]),
                     unsigned(kHostReturn),
                     g_nds_active == NDS_ARM9 ? "ARM9" : "ARM7",
                     resumes, dma_chunks, pumps,
                     hit_terminal ? "  TERMINAL HALT"
                         : hit_stall ? "  SPINNING AT ONE PC"
                         : (resumes >= kMaxResumes ? "  RESUME LIMIT" : ""));
        if (hit_stall)
            std::fprintf(stderr,
                         "[guestcall]   the pump stopped because %s: now=%llu "
                         "next_event=%llu (horizon %llu)\n",
                         g_stall_pumps_left ? "NOTHING IS DUE"
                                            : "the pump budget ran out",
                         g_stall_now, g_stall_next,
                         (unsigned long long)kEventHorizon);

        /* AND WHERE IT WENT, not just where it stopped - off unless
         * LSW2_TRACE_GUESTCALL=1.
         *
         * WHY THIS EXISTS. The gear applier's failure has been recorded for
         * several iterations as "hangs at 020CEA8C, a wait-for-interrupt
         * helper", which reads like a benign stall. Disassembling it says
         * otherwise:
         *
         *     020CEA74  STM sp!, {r3,lr}
         *     020CEA78  BL 0x020CE804      <- OS_DisableInterrupts
         *     020CEA7C  BL 0x020CEA84      <- MCR p15,0,r0,c7,c0,0 = halt
         *     020CEA80  B  0x020CEA78      <- forever
         *
         * Disable interrupts, halt, loop - that is NitroSDK's OS_Terminate
         * shape, and it has 25 callers spread through the SDK. So the guest is
         * not WAITING for anything; it has ABORTED and parked. The interesting
         * address is therefore not where it stopped but which of those 25 sites
         * sent it there, and the branch ring is the only thing that knows.
         *
         * runtime_trace_copy_recent is public and whsession.cpp already uses it
         * twice; this is the same call at the one moment it matters. */
        {
            static int trace_on = -1;
            if (trace_on < 0) {
                const char* v = std::getenv("LSW2_TRACE_GUESTCALL");
                trace_on = (v && *v == '1') ? 1 : 0;
            }
            if (trace_on == 1) {
                static RuntimeTraceEntry ring[64];
                const uint32_t n = runtime_trace_copy_recent(ring, 64);
                std::fprintf(stderr,
                             "[guestcall] the last %u branches before it "
                             "parked, oldest first:\n", unsigned(n));
                for (uint32_t i = 0; i < n; ++i)
                    std::fprintf(stderr, "[guestcall]   %2u  pc=%08X\n",
                                 unsigned(i), unsigned(ring[i].pc));
            }
        }
    }

    /* DO NOT CANCEL A RETURN THE RUNTIME IS PRESERVING. On a slice yield the
     * push is deliberately kept - cancelling it here would drop a return the
     * scheduler intends to resume through, which is worse than the failure
     * being reported. runtime_arm.h's own words: the BL/BLX return-check
     * "uses this to PRESERVE the pending return-push instead of cancelling
     * it". This seam is a BL and owes it the same treatment. */
    if (!returned && !unwinding) runtime_call_cancel_return(kHostReturn);

    g_cpu = saved;
    return returned;
}
