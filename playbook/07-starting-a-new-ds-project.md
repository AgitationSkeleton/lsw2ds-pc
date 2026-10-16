# Starting a new DS decomp+recomp: what transfers, and day-one order

For the next project (LEGO Battles is the intended one). This is the concrete
version of the rest of the playbook: what to copy, what to rewrite, and the
order to do it in.

## What transfers from this repo essentially unchanged

These are game-agnostic. Copy them first.

| tool | what it does |
| --- | --- |
| `tools/ghidra_scripts/FixVoidReturns.java` | retypes `void` functions whose ARM returns a value, and re-decompiles. 107 fixed here, 0 failures |
| `tools/ghidra_scripts/DumpDataTypes.java` | dumps Ghidra's stored types, so you can see what it actually believes |
| `tools/gen_dat_perfile.py` | shapes `DAT_` symbols, with the address-of-vs-bitwise-and rule and element-width overrides already worked out |
| `tools/gen_trace_wrappers.py` | call counting without touching a call site |
| `tools/gen_call_patches.py`, `tools/gen_irq_patches.py` | recover dropped call arguments by dataflow, and refuse when they cannot prove a value |
| `tools/host_pc_to_function.py` | host program counters in a stuck run back to DS function names |
| `tools/extract_arm7_sections.py` | splits the ARM7 binary, including the measurement-based path for a relocated block with no autoload table |
| `tools/fix_split_literals.py` | repairs C string literals mangled by shell heredocs |
| `tools/report_closure_progress.py` | progress against a frozen denominator, with translated and verified kept separate |

The host layer transfers in *shape* but not in content: address-space mapping,
MMIO trapping via `PAGE_NOACCESS` plus a vectored exception handler, indirect
call dispatch through non-executable pages, fibers for the thread system, and
the `LSW2_ARMCALL` / `LSW2_HOSTCALL` convention split. All of that is DS-generic
and only the specifics change.

## Day one

1. **Extract the ROM's sections and find every relocated block.** ARM9 static,
   ITCM, DTCM, ARM7 static, and any block the loader copies. Check for a
   module-params structure first; if there is none, correlate against a
   savestate (see [05](05-the-oracle.md)). *Do not proceed until every block is
   decompiled at its runtime address* - unresolved `thunk_EXT_*` targets are
   code you do not have, and finding that out in month two is expensive.
2. **Check that Ghidra headless scripting actually works** before relying on it.
   It is the difference between whole-corpus retyping passes and doing things
   by hand.
3. **Run the retyping passes** while re-exporting is still cheap: void returns
   first, then a survey of `DAT_` symbols used in arithmetic that resolve into
   the MMIO range.
4. **Freeze the closure.** Decide what set of functions the project needs and
   write it to a file. Report against it from the first week, so "progress" is
   never a feeling.
5. **Set up the build**: 32-bit `i686-w64-mingw32`, `-mregparm=3`, `-O0`,
   `-fpermissive`. Put the msys mingw32 bin directory on PATH or gcc fails in
   confusing ways - cc1 cannot load its DLLs and the error does not say so.
6. **Get a savestate of the real game running** and a helper that turns
   "read address X" into a number. You will use it every session.

## The pre-flight checks worth scripting immediately

Each of these caught a real bug here, and each is a short script:

- **Side-effecting reads.** Build the list of MMIO addresses whose reads have
  side effects (FIFOs first) and grep the corpus for every one. Check each
  against the disassembly for reads hoisted above their guard.
- **MMIO-adjacent `DAT_` arithmetic.** Any symbol used with `+` or `-` that
  resolves into the register range is a width bug waiting to happen.
- **Dropped-argument survey.** Count call sites where the caller sets a
  register the callee's signature does not mention. The count alone tells you
  how big the problem is; here one function had 169 such sites.
- **`void`-typed functions whose ARM sets r0.**
- **Unresolved externals.** Any `thunk_EXT_*` or missing symbol is a section
  you have not mapped.

## Things that will cost you a day each if you skip them

- Building 64-bit. A DS pointer is four bytes. Decide this before anything else.
- Optimising. Ghidra's output has no `volatile`; `-O0` is not negotiable.
- Calling Win32 or `stderr` from a regparm translation unit. Isolate OS calls
  in host-convention files from the start and write the reason at the top.
- Treating a halt count as a frame count. Name counters after what they
  measure.
- Hand-editing exported C "just this once".

## A note on shell heredocs

Writing C string literals through a shell heredoc mangles backslash escapes -
`\n` becomes a real newline and the build fails somewhere unrelated. This cost
four build failures here before it was recognised. Use a file-writing tool
rather than a heredoc for anything containing C escapes, and keep
`tools/fix_split_literals.py` around for when it happens anyway.

## And the habit that matters most

When translated code misbehaves, **disassemble before theorising**. Every
significant bug on this project was information Ghidra dropped that the
instructions still contained. Reading the ARM is the fast path.
