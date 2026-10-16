# Automation, and the rule that makes it possible

## The rule

**Never hand-edit exported decompiler output.**

Every correction is expressed as data in a config file, applied by a generator
that runs as part of the build. The exported corpus stays exactly as Ghidra
produced it, so it can be re-exported at any time - after retyping a function,
after fixing a symbol, after a Ghidra upgrade - without losing a single fix.

This sounds like extra ceremony on day one. It is the difference between a
project you can still improve in month three and one where re-exporting means
redoing weeks of work, so you stop re-exporting, so the corpus rots.

## The shape that works

```
out/ghidra/decomp/c/FUN_xxxxxxxx.c     exported, never edited
config/*.txt, config/*.json            the corrections, as data
tools/gen_*.py                         generators that apply them
out/work/patched/FUN_xxxxxxxx.c        the result, disposable
```

Every generator: reads config, reads the corpus, writes to `out/work/patched/`,
and prints a count of what it applied, what was already applied, and **what it
refused**. That last number is the one that matters.

## Config files that earned their place here

| file | what it overrides |
| --- | --- |
| `config/dat_element_types.txt` | true element width of a data symbol, with the disassembly that proves it |
| `config/host_overrides.txt` | functions the host layer provides instead |
| `config/unfaithful_wrapped.txt` | functions that compile but read storage Ghidra could not account for |
| `config/excluded_functions.txt` | functions deliberately not built |
| `config/traced_functions.txt` | functions to count calls to, at runtime |
| `config/recomp_inventory.json` | the closure - the denominator for all progress reporting |

## Trace without touching a call site

You will constantly want to know "is this function ever reached?". The cheap
way that does not modify any decompiled code:

1. Compile the target under a renamed symbol with `-DFUN_xxx=FUN_xxx_traced`.
2. Generate a wrapper with the real name that increments a counter and tail-calls
   the renamed one.

Call sites are untouched, nothing in the corpus changes, and turning a trace
off is deleting a line from a text file. `tools/gen_trace_wrappers.py`.

The same mechanism handles "this function reads a register we cannot account
for" - wrap it, count it, and report at exit how many of the unfaithful set
were actually reached. On this project that turned "239 functions might be
wrong" into "4 were reached", which is a completely different problem.

## Make the tools honest, and make them stay honest

Two failure modes that cost real time here:

**A stale generated file.** A generator that only ran when its input changed
left a stale `unfaithful_wrappers.c` behind, producing a duplicate symbol at
link after a 40-minute build. Generators are cheap; run them every build.

**A tool that cries wolf.** The SWI patcher used one entry's marker to test
whether *both* entries were applied, so it reported "not applied 1" on every
build after the first, while the fix was in fact present. A tool that reports a
false problem every run is a tool whose output you stop reading - and then it
cannot tell you about the real one. Give each check its own marker, and treat a
persistent spurious warning as a bug in the tool, not as noise to live with.

**A failed build that leaves a runnable binary.** The host layer failed to
compile, the link never ran, and the previous `run_game.exe` was still on disk.
The command chain went on to run it, and the report that came back described
the *old* build with nothing to indicate it. The build had printed its error
and exited non-zero; that was not enough, because the next command did not
check.

Two fixes, and you want both:

- Delete the output binary at the *start* of the build. A broken build then
  produces a missing file, which fails loudly, rather than an old file, which
  lies quietly.
- Chain with `&&`, not `;`, so a run never follows a failed build.

The general principle: **make the failure mode be "nothing happened", never
"something plausible happened with stale inputs".** Stale-input results are
the most expensive kind of wrong answer, because they are internally
consistent and you have no reason to doubt them.

**A trace wrapper must have the traced function's own signature.** The obvious
implementation is one generic shape for every function —

```c
undefined4 f(undefined4 a, undefined4 b, undefined4 c, undefined4 d) {
    hits[i]++;
    return ((any_fn)f_traced)(a, b, c, d);   /* four arguments, always */
}
```

— and it is not transparent. It forwards four arguments whatever the real arity
is, and its return type is `undefined4` whatever the function actually returns.
On this project, adding three traces moved the port's failure from a read fault
at 0x1C to an execute fault at 0, and the first thing that had to be ruled out
was the instrument rather than the program.

The decompiler already wrote the signature. Copy it, rename the definition,
forward the parameters by name, and the wrapper is transparent by construction:

```c
bool FUN_020d28c0(int *param_1, char *param_2, int param_3, int param_4) {
    hits[22]++;
    return FUN_020d28c0_traced(param_1, param_2, param_3, param_4);
}
```

The general rule: **an instrument that can perturb what it measures will
eventually be blamed for a real bug, or excuse one.** Make it structurally
incapable of changing behaviour, and when it cannot find the information it
needs to do that, make it say so rather than falling back silently.

**Do not combine measurements from different builds.** Obvious stated plainly,
easy to do by accident. On this project the trace counts came from one build
and the memory dumps from another, and reasoning that used both at once was
about to conclude something specific about which function touched which
structure. The two builds were not the same program — tracing perturbs this
port — so the combined picture was of a machine that never existed.

Prefer **one run that answers the whole question**. If you need a call count
and a structure's contents, put both in the same build even if that costs a
rebuild; the rebuild is cheaper than a conclusion drawn across two.

And prefer measurements that **cannot** perturb. Dumping memory the game itself
maintains — a device object, a request block, a handle — changes nothing about
control flow, and on this project those dumps settled three questions that
traces and code reading had left open.

**Mark unreliable diagnostics as unreliable, in their own output.** The crash
reporter here walks the frame chain and, when there is none, scans the stack —
printing "some entries may be stale" as it does. That warning is load-bearing:
a scanned "caller" sent an investigation down a path through a function that a
later trace showed had never been called at all. Diagnostics that guess are
worth having; diagnostics that guess *silently* are not.

**Your own disassembler will be wrong, and it is the worst place to be wrong.**
This project's whole method is "when the C does not explain it, read the
instructions". The hand-written ARM disassembler used for that decoded

```
E0020293   as   AND r0, r2, r3, LSL #5      (wrong)
                MUL r2, r3, r2               (right)
E7B02002   as   LDR r2, [r0, #+0x2]!         (wrong)
                LDR r2, [r0, r2]!            (right)
```

Two ordinary encoding mistakes: multiplies have bits 27-25 = 000 and fall into
the data-processing case unless you test bits 7 and 4 first, and load/store
ignores bit 25 which selects a register offset. The second one mattered
enormously - the register-offset reading is what reveals that the writeback
leaves r0 holding the computed address, which *is* the argument the callee
receives.

**Test a disassembler against instructions whose meaning you already know**,
and re-run that check after every change to it. Here the regression test is the
card-removal check, whose `LDRH` and `SUBEQ #0x410` the project already depends
on; if those still decode correctly the fix did not break an existing
conclusion. Without such a check you cannot tell a fix from a new bug.

**A known trap that keeps recurring should be repaired by the build, not by
you.** Writing C through a shell heredoc mangles `\n` inside string literals
into real newlines. Knowing about it did not stop it happening a fifth time.
The build now runs `tools/fix_split_literals.py` over the host sources before
compiling and announces anything it rejoined - which heals the mistake without
hiding it. If a mistake has cost you three rebuilds, stop resolving to be
careful and put a repair in the pipeline.

## Instrument the port so a stuck run names its own location

The single highest-leverage piece of infrastructure on this project. When the
port hangs, you want the report to say *which function*, not to start a
debugging session.

What earned its keep:

- **A sampling watchdog.** A second thread suspends the game thread at
  intervals and records its instruction pointer. A spin lands on the same few
  addresses every sample, naming the loop exactly. Needs nothing installed and
  works on the shipping build.
- **Caller capture at the wait.** At the halt, record
  `__builtin_return_address(1..4)`. Level 1 is whoever called the halt; the
  levels above name the code that *decided* to halt, which is the useful part.
- **A host-PC to function mapper.** Those return addresses are raw host
  addresses that move every rebuild. `tools/host_pc_to_function.py` resolves
  them against the executable's symbol table and prints DS function names. This
  was being done by hand - run nm, sort, eyeball - once per address per
  rebuild. Automating it took ten minutes and paid for itself the same day.
- **A wrapping event trace.** A fixed-size ring of the last N interrupt or
  dispatch events, printed on exit. Fixed size means it costs nothing to leave
  on permanently.

**Name your counters honestly.** A counter here was called `frames` and its
report announced that the game loop was turning over. It was counting halts,
and a fatal halt loop halts exactly like a frame boundary does - so 601 halts
in a shutdown loop were reported as 601 frames of a running game, while tracing
showed neither frame function had ever been entered. The counter now says
`halt(s)` and the report says, in the output itself, "A HALT IS NOT A FRAME."

The general form of that mistake: **a metric that measures the thing you want
to see, rather than the thing that happened, will eventually tell you what you
want to hear.** Write the caveat into the program's own output, where it cannot
be forgotten, rather than into a document you will not reread.

## Build a "what does this project already do about X?" command

The machinery accumulates: veneer forwarders, argument patches, element-type
overrides, host replacements, unfaithful wrappers, trace wrappers, exclusions,
transcriptions. Each is a good idea. Together they become more than anyone
remembers, and then you rediscover a defect somebody already fixed — and only
find out after a build.

On this project that happened with `FUN_020CCCDC`. It looked like a textbook
dropped-argument-at-an-indirect-call bug: Ghidra writes `(*DAT_020ccce8)()`
while the instructions pass r0–r2 through and force r3 = 0. It was already in
`config/veneers.txt`, the generated forwarder was correct, and it is the
**worked example in the veneer generator's own docstring**.

So `tools/whats_handled.py FUN_020cccdc` now prints every config that names a
function, every generated wrapper for it, what that wrapper does, and whether a
patched source exists — ending with a plain verdict. One command, before the
fix rather than after.

```
LISTED in config/veneers.txt
HAS a generated veneer forwarder in recomp/decomp/src/veneers.c
   | /* FUN_020cccdc -> 0x020CCC90, forcing r3=0x0 */
   | undefined4 FUN_020cccdc(undefined4 a, undefined4 b, undefined4 c, undefined4 d) {
   |     d = 0x0u;  /* the veneer sets this itself */
ALREADY COVERED in at least one way.
```

Cheap to write, and the value grows with the project. Add one early, and add a
line to it every time you add a new kind of override — the tool is only as good
as its list of places to look.

## Report progress against a fixed denominator

Decide early what set of functions the project actually needs - the closure -
and freeze it in a file. Then report progress as counts against it, in states
that mean something:

```
translated      compiled and linked
unfaithful      compiled, but reads storage that could not be accounted for
stubbed         no usable body; aborts if ever called
host-replaced   deliberately provided by the host layer
failed          in the corpus and does not compile
```

And keep **verified** strictly separate. Translated means it compiled. Verified
means it agreed with the emulator on real input. Conflating the two is the
easiest way to believe a port is nearly done when it has never been checked.
`tools/report_closure_progress.py` prints both and refuses to merge them.

## Never put a pipe between a command and its exit code

`cmd | head` then `echo $?` reports **head's** status, not `cmd`'s. Head almost
always succeeds, so the failure disappears.

This happened three times in one session here, and twice it turned a *failed*
build into an apparent success — the log was even sitting there with the error
in it, unread, because the exit code said there was nothing to look at.

```sh
cmd > log 2>&1; echo "EXIT=$?"; tail -20 log      # right
cmd 2>&1 | head -20; echo "EXIT=$?"               # lies
```

The same rule covers `| tee`, `| grep`, and `| tail`. If a pipeline is genuinely
wanted, use `set -o pipefail` or read `${PIPESTATUS[0]}` — but redirecting to a
file and reading it afterwards is simpler and keeps the whole log.

This belongs next to "a failed build must not leave a runnable executable": both
are cases where the *absence* of a visible error was taken as evidence of
success.

## Use a second, known-clean subject as a control before believing a histogram

Auditing the LSW2 ARM9 through a lifter gave 2.3M instructions decoded across a
1.05MB image — 8.8× the image size. The obvious reading was "it must be decoding
data as code". **That reading was wrong.** The ARM7 came out at 6.9× with *zero*
undefined encodings, so the multiplier is just per-function body overlap and says
nothing at all about data.

What did work was comparing opcode histograms with the clean image as a control,
scaled by the size ratio: `swi` should have appeared ~57 times in the ARM9 and
appeared 82,946 times; `cdp` should have been absent and appeared 12,630 times;
`and` — opcode `0000`, what arbitrary data decodes into — ran 5.7× over its
expected share while `mov`, `ldr` and `bl` sat at or below theirs.

The general habit: **a suspicious number is not evidence until you know what the
number looks like when nothing is wrong.** Two images from the same ROM, one
known clean, is about the cheapest control available on a DS title, and it turns
"this looks high" into a ratio you can defend.

## Do not write code containing escape sequences through a heredoc

This has now bitten three times in one session, and the third time cost four
failed compiles.

Piping a Python patch script into `python - <<'EOF'` looks safe — the quoted
delimiter means the shell does no expansion. But a `\n` inside a string literal
in that script still has to survive being written to disk, and in this
environment it does not: it arrives as a **real newline**, splitting the C string
across two lines.

```c
    std::fprintf(stderr, "[shell] audio %d Hz stereo
", got.freq);
```

The compiler says `missing terminating " character`, which reads like a typo
rather than a tooling artefact, so the instinct is to fix that one line — and the
next one fails the same way.

**Rules:**

- Any file content containing `\n`, `\t` or `\` goes through the editor tools
  (Write/Edit), never a heredoc.
- If you have already done it, **grep before compiling**:
  `grep -nE '"[^"]*$' file.c` finds string literals with no closing quote on the
  same line.
- Repairing them by regex is a trap of its own. The obvious pattern misses when
  the file has **CRLF** endings, because `\n"` does not match `\r\n"` — which is
  how "rejoined 3 split literals / remaining suspicious: 3" happens. Fix them
  with the editor instead of getting clever.

## SDL_WINDOWPOS_* are magic constants, not coordinates

`SDL_WINDOWPOS_CENTERED` is `0x2FFF0000`, and `SDL_WINDOWPOS_CENTERED_DISPLAY(n)`
ORs the display index into the low bits. They are sentinels the window manager
interprets, not positions.

So this, written to nudge two windows apart, is nonsense:

```c
SDL_WINDOWPOS_CENTERED_DISPLAY(0) + (index ? 40 : -300)
```

Subtracting 300 corrupts the sentinel into either a different sentinel or a wild
coordinate. The window is created, appears in the taskbar, and cannot be brought
to the foreground because it is not on any display. **The failure looks like a
window-manager problem, not an arithmetic one**, which is what makes it expensive.

Compute real coordinates instead — `SDL_GetDisplayUsableBounds()` gives the area
to lay out in — and clamp so nothing can land outside it on a smaller display.

The general form is worth remembering beyond SDL: **an API constant that is
"just an int" may still be a sentinel, and arithmetic on it is undefined even
though it compiles.**
