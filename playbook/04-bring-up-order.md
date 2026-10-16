# The order to bring a DS recompilation up in

The order matters because each stage produces the evidence the next one needs.
Taken out of order you end up debugging the renderer before the game has
reached the code that programs it.

This is the order this project arrived at, with hindsight applied - some of
these were learned by doing them too late.

## 0. Decide the target and freeze the closure

Work out which functions the project actually needs and write the list to a
file. "The whole game" is not a target you can measure against. This project's
target is one mode, and the closure is 1,555 functions for the frame path and
741 for the logic floor. Every progress report is a count against that file.

## 1. Get the corpus complete *before* writing any host code

Missing code is the most expensive thing to discover late, because by then you
have theories built on its absence.

- Split every relocated block and decompile it at its **runtime** address -
  ITCM, DTCM, and any block the loader copies. See section 7 of
  [01](01-what-ghidra-silently-drops.md); 475 functions were missing here
  because one 64KB ARM7 block was decompiled at the wrong address.
- Do the whole-corpus retyping passes now (void returns, symbol widths) while
  re-exporting is still cheap.
- Confirm there are no unresolved `thunk_EXT_*` targets left. Each one is code
  you do not have.

## 2. Map the address space natively

Reserve every DS region at its real address in the host process, so a DS
pointer *is* a host pointer. No translation layer, no address arithmetic on
every access. This forces a 32-bit build (`i686-w64-mingw32`), because a DS
pointer is four bytes, and that constraint propagates to everything - accept it
on day one rather than discovering it in month two.

## 3. Fix the calling convention, and fix the boundary

`-mregparm=3` reproduces the ARM r0-r3 convention. `-O0` is mandatory, because
Ghidra's output has no `volatile` and an optimiser will happily delete your
hardware accesses.

Then define exactly two conventions and make the boundary explicit:

```
LSW2_ARMCALL    regparm(3)     translated game code
LSW2_HOSTCALL   regparm(0)     host layer
```

**Win32 API calls and stderr cannot be made from a regparm translation unit.**
This is not a style preference. `CreateFiber` called from a regparm TU returned
a garbage handle and corrupted the stack, turning a function *return* into a
jump to a stack address. Put anything that calls the OS in its own
host-convention file and document why at the top of the file.

## 4. Get to the reset vector, then handle traps

Run the ARM9 from its reset vector into translated code. Then, in this order:

1. **MMIO trapping.** Reserve the register range as `PAGE_NOACCESS` and decode
   the faulting instruction in a vectored exception handler. Now every hardware
   access is visible and nothing is silently ignored.
2. **Indirect calls.** Function pointers in translated code hold DS addresses.
   Point them at non-executable pages and dispatch in the same handler. Note
   the failure mode: if an indirect call lands somewhere *executable* - a
   loaded DLL - the handler never sees it and the frame chain is gone by the
   time it faults. Trace to narrow that down.
3. **The BIOS system region.** The firmware leaves data in RAM that the game
   reads: the ROM header, chip IDs, the boot indicator. Publish all of it from
   a savestate readout, not from the parts you think are needed - this project
   shipped one of the two chip ID copies and the game shut itself down as if
   the cartridge had been removed.

## 5. The two processors, then interrupts, then threads

- **IPC FIFO and IPC SYNC** before anything that waits. The ARM9 blocks on the
  ARM7 constantly.
- **Interrupts.** IME/IE/IF, with `IF` as write-one-to-clear. Get this exactly
  right; the failure mode is an interrupt handler that runs hundreds of
  thousands of times.
- **DMA.** Trigger on the **0 -> 1 edge** of the enable bit, never on level.
  Level-triggering re-runs a channel from zeroed addresses. Range-check every
  transfer and report refusals rather than faulting.
- **Threads.** The ARM context save/restore is untranslatable, so the scheduler
  needs a host implementation - fibers work well. Read the saved-PC offset out
  of the *disassembly*, not the decompiled C: here it is block+0x40, not the
  +0x3C the C implies, because `LDR r1,[r0],#4` post-increments the base.

Note that a port that cannot preempt has to deliver interrupts at a polling
point. That is a real divergence from the hardware. Flag it in the code that
implements it, so it is visible where it matters.

## 6. Only now, the renderer

By this point the game has actually programmed the hardware, so you can read
real register values instead of guessing at a configuration. Measure the
feature set the game uses before implementing anything - this project measured
which 3D features the game touches and found it needed 4x4 compressed textures
and W-buffering, and did not need fog, edge marking, alpha test, A3I5 or direct
colour. That is a large amount of work correctly not done.

Implement the parts the game uses; **count** the parts it does not, so an
unimplemented feature shows up as a number in the report rather than as a
visual artifact you have to notice.

## 7. Verification, continuously and from early on

See [05-the-oracle.md](05-the-oracle.md). The mistake to avoid is treating
verification as a phase at the end. "Translated" and "verified" are different
counts and the gap between them is the real state of the project.

## A note on the licence boundary

If you consult an emulator for hardware behaviour, know its licence. Reading a
GPLv3 emulator's *source* to learn what a register does is research; copying
its rasteriser into your project is a licence problem. This project wrote its
3D rasteriser from the hardware specification and says so explicitly in the
file, and vendored the emulator source only for savestate *layout*. Decide this
deliberately and write the decision down in the code.
