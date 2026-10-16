# Verifying a port, and not trusting your own tools

Lessons from the LSW2 shell that generalise to any DS port. Every one of them
cost real time on this project, and every one is cheap to avoid.

## Run the command a user would run, early and often

Everything in a long session gets verified piece by piece with hand-run compiler
invocations, because that is the fast inner loop. **That is not the same as
verifying the thing you ship.**

`port/build.sh` had been failing to link for hours. The shell had gained three
source files and an imgui backend; the script named its sources explicitly and
had not been updated for any of them. Hand-built binaries kept working the whole
time, so nothing surfaced it. A fresh clone would not have built.

- **Run the entry point on a schedule**, not only at the end. Once per feature
  is about right.
- **Discover inputs, do not list them.** `for src in port/frontend/*.cpp` cannot
  fall behind. A list maintained by hand is a list that will be wrong. Keep an
  explicit list only where a glob would pull in things you do not want - imgui's
  backends directory holds one file per renderer, so that one stays explicit.

## Measure the claim; do not reason about it

A prominent warning shipped in the UI for days saying the widened viewport would
cull scenery at the original edges. The reasoning was plausible - the game clips
against its own projection, the host widens the raster without touching it. It
was wrong, and one measurement showed it:

  - **pause the game** so the scene is frozen
  - resize the window between captures (the raster width follows the window)
  - compare

The wide frame held strictly more world at the same scale. Ten minutes of
measurement against days of a false warning that also trained the reader to
ignore warnings.

**Anything asserted in a comment or a UI string is a claim.** If it describes
observable behaviour, observe it.

## Two readings are only a cross-check if they measure the same thing

The cheats tab read the current level two ways and warned in red when they
disagreed. They were not two views of one fact - one was the request the state
machine builds from, the other the state object the ordinary entry path
installs. A warp writes the first and not the second, so the alarm fired during
normal use.

Before treating agreement as evidence, state precisely what each side measures.
"Different code path" is not the same as "independent measurement of the same
quantity".

## Know what your debug channel can and cannot do atomically

The framework's debug surface executes **one command per frame**, on the frontend
thread between frames. That makes each command safe and makes a SEQUENCE of them
unsafe: a five-word state edit is spread over five frames, and the guest runs
with a half-written structure for four of them. Scripted warps crashed the guest
reliably; the same edit performed inside the shell, all five words in one frame,
never did.

**If several words must change together, do it inside the program, not over the
wire.** No amount of retry logic fixes a design that cannot be atomic.

## Handshakes: a timeout must invalidate the request, not just abandon it

The same surface waited 30s for the frontend and then answered "busy". It left
the request live, so a slow reply could arrive after the next request had been
posted - and be returned as that request's answer. From then on every reply was
one behind, permanently. The symptom looked like a client bug: a response of the
wrong shape, then timeouts.

Stamp requests; discard replies whose stamp has moved on. And **never set a
client timeout equal to the server's** - the client then gives up at the exact
moment the server decides to tell it something useful.

## A log line's position is not a timestamp

Twice I concluded a crash happened "at boot" because the first bad line was line
39 of the log. Line 39 was simply the first line after a 38-line boot banner. The
crashes were minutes later. That produced a confident false correlation with an
environment variable, which eight controlled launches then demolished.

If when something happened matters, timestamp it. Ordering in a file tells you
ordering, nothing more.

## You will violate your own written rule

playbook/02 says: do not write C through a heredoc, escapes get mangled. I did
it anyway, put a literal NUL byte into a C++ source, and it compiled. It broke
one unrelated-looking command, and I spent several cycles suspecting the game,
the save file and thread-safety before checking the file.

Rules in a playbook do not enforce themselves. Where a rule exists because a
class of mistake is easy, **make the tool refuse** rather than trusting yourself
to remember - or at minimum check the artifact (`file x.cpp` saying "binary" is
a fast tell) before believing the program is at fault.

## A debug command that accepts your call and does nothing

The worst failure mode a control surface has is not an error. It is silence.

Two examples from one session, both of which produced confident wrong
conclusions before they were caught:

**`run_cycles` does not advance the scheduler.** A drive loop ran 1,200
"frames" against the headless surface and reported that the game never got
anywhere. The registers were bit-identical before and after - PC unchanged, no
cycles consumed. `run_rounds` is the command that runs. `run_cycles` returned
success the whole time.

**`keys` is a no-op while the interactive frontend is running.** It calls
`nds_set_key_mask` directly, and the frontend's frame loop rewrites that mask
every frame from its own sources. An injected mask therefore survives less than
one frame. The frontend's own injection path - `frontend_input` - feeds a
variable the loop ORs in, and works. A RAM diff across "press Start" showed
**zero** changed words with `keys` and eighteen with `frontend_input`, which is
what finally made it obvious.

Both look exactly like a game that is stuck, and a stuck game is a much more
interesting hypothesis than a broken tool, so that is where the time goes.

**The check that would have caught both in seconds:** before trusting a stepping
or injection command, prove it moves something you can already see. Read a
register, step, read it again. If PC has not moved, the command did nothing -
whatever it returned.

**And prefer the path the program itself uses.** Both working commands went
through the frontend's own machinery; both broken ones reached past it into the
emulator's state, where the frontend promptly overwrote them. On a port that
owns its own loop, "inject at the layer that owns the value" is the rule.
