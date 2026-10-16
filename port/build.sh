#!/usr/bin/env bash
# Build the LSW2 PC port from scratch: ROM -> configs -> banks -> runnable exe.
#
# Run from Git Bash. One command, no manual steps, no paths outside this repo
# except the framework checkout and your own BIOS dumps.
#
#   port/build.sh              build everything that is out of date
#   port/build.sh clean        wipe generated output first
#   port/build.sh run          build, then launch interactively
#
# WHY A SCRIPT AND NOT NOTES IN A README. Every stage here was got wrong at
# least once by hand - the wrong compiler, the wrong path flavour, a stale bank,
# an exit code hidden behind a pipe. A script that fails loudly is the only
# version of this that stays true a month from now.

set -u
set -o pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="$REPO/port"
WORK="$REPO/out/work/ndsrecomp"
BANKS="$WORK/generated"       # title banks (globbed by NDS_TITLE_BANK_DIR)
BIOSB="$WORK/gen_bios"        # BIOS banks (NDS_GENERATED_DIR)
BUILD="$REPO/out/work/port-build"

# The framework: RetroPortingToolKit/ndsrecomp at the commit
# tools/fetch_ndsrecomp.sh pins, with port/patches/ applied on top. In order:
# NDSRECOMP_ROOT if set; else the path written in .ndsrecomp_root at the repo
# root (one line, untracked - for a checkout kept elsewhere); else
# third_party/ndsrecomp, where the fetch script puts it.
FRAMEWORK="${NDSRECOMP_ROOT:-}"
if [ -z "$FRAMEWORK" ] && [ -f "$REPO/.ndsrecomp_root" ]; then
    FRAMEWORK="$(head -1 "$REPO/.ndsrecomp_root" | tr -d '\r')"
fi
[ -n "$FRAMEWORK" ] || FRAMEWORK="$REPO/third_party/ndsrecomp"

# THE TOOLCHAIN IS NOT NEGOTIABLE. Where devkitPro is installed, its g++ can
# come first on PATH, and it has no C++ standard library headers - it compiles a bare
# main() so cmake's compiler check passes, then dies on the first real file.
# msys2 ucrt64 is the one with a complete C++20 stdlib.
UCRT="${UCRT64_BIN:-/c/msys64/ucrt64/bin}"
export PATH="$UCRT:/c/msys64/usr/bin:$PATH"

ROM_SHA1="7c1afa070797945ba5a2577c7ddfded5cf2fc443"

die() { echo "" >&2; echo "BUILD FAILED: $*" >&2; exit 1; }
step() { echo ""; echo "=== $* ==="; }

# Never `cmd | head` and then read $? - that reports head's status. Every
# command here writes a log and its exit code is read directly.
run() {
    local log="$1"; shift
    "$@" > "$log" 2>&1
    local rc=$?
    if [ $rc -ne 0 ]; then
        echo "--- last 25 lines of $(basename "$log") ---" >&2
        tail -25 "$log" >&2
        die "$1 exited $rc"
    fi
}

[ "${1:-}" = "clean" ] && { step "clean"; rm -rf "$BANKS" "$BIOSB" "$BUILD" "$WORK/generated_stage"; shift || true; }

step "preflight"
[ -f "$FRAMEWORK/recompiler/CMakeLists.txt" ] || die "no ndsrecomp at $FRAMEWORK
Run tools/fetch_ndsrecomp.sh to download the pinned version into third_party/,
or set NDSRECOMP_ROOT to a checkout of it."
[ -x "$UCRT/g++.exe" ] || die "no ucrt64 g++ at $UCRT (set UCRT64_BIN)"
ROM="$(ls "$REPO"/rom/*.nds 2>/dev/null | head -1)"
[ -n "$ROM" ] || die "no ROM in $REPO/rom/"
# THE CARTRIDGE HASH, and why a mismatch is not automatically fatal.
#
# Refusing anything but the stock cartridge is right for a normal build: the
# banks are generated from this image and registering them for a different game
# corrupts guest execution before the runtime can fall back.
#
# It is WRONG for a modded cartridge, and that made mod support impossible. An
# asset mod (tools/build_asset_rom.py) rewrites NitroFS data and leaves the ARM9
# and ARM7 images untouched - the recompiler verifies those two by their own
# hashes when it emits, and prints "identity sha1: ... (verified)" for each. So
# the banks stay exactly as valid as they were; only the whole-file hash moved.
#
# So: a mismatch is refused by default, and LSW2_ALLOW_MODDED_ROM=1 proceeds
# with the modded image, gating the banks on ITS hash so they still register.
# The recompiler's per-image verification is what keeps that honest - if a mod
# had touched executable code, emitting the banks would fail there rather than
# here.
got="$(sha1sum "$ROM" | cut -d' ' -f1)"
if [ "$got" != "$ROM_SHA1" ]; then
    if [ "${LSW2_ALLOW_MODDED_ROM:-0}" = "1" ]; then
        echo "   NOTE: cartridge hash is $got, not the stock $ROM_SHA1."
        echo "         Building for the modded image (LSW2_ALLOW_MODDED_ROM=1)."
        echo "         The ARM9/ARM7 images are still verified by the recompiler"
        echo "         when it emits each bank; if a mod touched code, that will"
        echo "         fail rather than silently mis-register."
        ROM_SHA1="$got"
    else
        die "ROM sha1 mismatch: got $got, want $ROM_SHA1

If this is a MODDED cartridge from tools/build_asset_rom.py, rebuild with
    LSW2_ALLOW_MODDED_ROM=1 bash port/build.sh
which gates the title banks on the modded image's hash so they still
register. Without that the game runs entirely in the interpreter."
    fi
fi
for f in biosnds9.rom biosnds7.rom; do
    [ -f "$PORT/bios/$f" ] || die "missing $PORT/bios/$f - see port/README.md"
done
echo "ROM, BIOS dumps and toolchain all present."

mkdir -p "$BANKS" "$BIOSB" "$WORK/generated_stage" "$REPO/out/work/logs"
LOGS="$REPO/out/work/logs"
REC="$BUILD/recompiler/nds_recompile.exe"

# THE ROM'S PROGRAMS AND FONTS, cut out of your ROM once (tools/extract_rom.py).
# Nothing extracted is committed - it is the game. A Windows CPython runs it,
# not MSYS2's: the font assets are expanded by the game's own decompressor under
# unicorn, which has no MSYS2 build.
RAW="$REPO/out/raw"
missing=0
for f in arm7.bin arm7_wram_037f8000.bin arm9_section_0_02000000.bin \
         arm9_section_1_01ff8000.bin; do
    [ -f "$RAW/$f" ] || missing=1
done
ls "$RAW"/archive/entries/3442.* > /dev/null 2>&1 || missing=1
if [ $missing = 1 ]; then
    step "extract the programs and fonts from your ROM"
    if command -v py > /dev/null 2>&1; then PYWIN=(py -3); else PYWIN=(python); fi
    run "$LOGS/extract.log" "${PYWIN[@]}" "$REPO/tools/extract_rom.py" --rom "$ROM" --output "$RAW"
    sed 's/^/   /' "$LOGS/extract.log" | grep -v '^   *$' | tail -6
fi

# PATCHES WE APPLY TO THE FRAMEWORK, and why they live here rather than there.
#
# ndsrecomp is a dependency we build against, not a tree we own; a change made
# by hand in it would be invisible to this repo and lost the next time it was
# re-downloaded. Keeping the change as a patch in port/patches/ means the diff
# is reviewable, is version-controlled with the port, and is re-applied on any
# machine that builds this.
#
# Idempotent by construction: a patch that is already applied reverse-applies
# cleanly, and that is the test used to skip it. Re-running this script is
# therefore safe, which matters because it runs on every build.
step "apply framework patches"
for patch in "$PORT"/patches/*.patch; do
    [ -e "$patch" ] || continue
    name="$(basename "$patch")"
    if patch -d "$FRAMEWORK" -p1 -R --dry-run -s -f < "$patch" >/dev/null 2>&1; then
        echo "  $name already applied"
    elif patch -d "$FRAMEWORK" -p1 --dry-run -s -f < "$patch" >/dev/null 2>&1; then
        patch -d "$FRAMEWORK" -p1 -s < "$patch" || die "$name failed to apply"
        echo "  $name applied"
    else
        # Neither direction is clean: the framework has moved under the patch.
        # Stopping is right - building on a half-patched tree would produce a
        # binary nobody could reason about.
        die "$name does not apply to $FRAMEWORK and is not already applied"
    fi
done

step "build the recompiler"
run "$LOGS/rec-cfg.log" cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -B "$BUILD/recompiler" "$FRAMEWORK/recompiler"
run "$LOGS/rec-build.log" cmake --build "$BUILD/recompiler" -j "$(nproc)"
[ -x "$REC" ] || die "recompiler did not produce $REC"

# THE LIFT CONFIGS, and why a copy of them is tracked in port/lift/.
#
# The four program configs - every function entry point, its instruction mode
# and its name - are generated from the Ghidra corpus under out/ghidra. That
# corpus is decompiled game code: it stays in the research checkout and is not
# part of the public source (tools/publish_public.py). The configs it yields
# are only addresses, modes and names, so they ARE published, as port/lift/,
# and a checkout without the corpus builds from them.
#
# Refreshed on every build that has the corpus, so the tracked copy cannot go
# stale quietly: a change shows up in `git status` as a diff in port/lift/.
LABELS="lsw2_arm9 lsw2_arm7 lsw2_arm7_wram lsw2_arm9_itcm"
if [ -d "$REPO/out/ghidra/decomp/c" ]; then
    step "generate program configs from the Ghidra corpus"
    run "$LOGS/gen-config.log" python "$REPO/tools/gen_ndsrecomp_config.py"
    grep -E "entries|covered|dropped" "$LOGS/gen-config.log" || true
    mkdir -p "$PORT/lift"
    for label in $LABELS; do
        cmp -s "$WORK/$label.toml" "$PORT/lift/$label.toml" ||
            cp -f "$WORK/$label.toml" "$PORT/lift/$label.toml"
    done
else
    step "program configs from port/lift (no Ghidra corpus in this checkout)"
    for label in $LABELS; do
        [ -f "$PORT/lift/$label.toml" ] || die "missing $PORT/lift/$label.toml"
        cp -f "$PORT/lift/$label.toml" "$WORK/$label.toml"
    done
    echo "   $(cat "$PORT"/lift/*.toml | grep -c '^\[\[entry_point\]\]') entry points"
fi

step "extract and verify the FreeBIOS images"
run "$LOGS/freebios.log" python "$REPO/tools/extract_freebios.py" "$FRAMEWORK"

step "emit BIOS banks (identity-verified against your dumps)"
emit_bios() {  # <config> <bin> <bank>
    run "$LOGS/emit-$3.log" "$REC" --config "$1" --bin "$2" --out "$BIOSB" --bank "$3"
    grep -E "verified|\[emit\]" "$LOGS/emit-$3.log" | sed 's/^/   /'
}
emit_bios "$FRAMEWORK/bios/biosnds9.toml" "$PORT/bios/biosnds9.rom"           arm9_bios
emit_bios "$FRAMEWORK/bios/biosnds7.toml" "$PORT/bios/biosnds7.rom"           arm7_bios
emit_bios "$FRAMEWORK/bios/freebios9.toml" "$WORK/freebios/drastic_bios_arm9.bin" freebios_arm9
emit_bios "$FRAMEWORK/bios/freebios7.toml" "$WORK/freebios/drastic_bios_arm7.bin" freebios_arm7

step "emit LSW2 title banks"
# Sharding is not cosmetic: unsharded, the ARM9 bank is a single 242 MB
# translation unit. The runner globs body shards as *_[0-9][0-9].c, so a bank
# emitted without --shards is not picked up at all.
#
# --validate-live-bytes on the two RAM banks: that code is COPIED into RAM at
# runtime, so dispatching on address alone would be trusting bytes that may not
# be there yet. The immutable cartridge banks do not need it.
# EMIT TO A STAGING DIRECTORY AND COPY ONLY WHAT CHANGED.
#
# The recompiler rewrites every bank source on every run. The CONTENT is
# identical when the ROM and configs have not changed - which is every build
# where only the shell was edited - but the mtimes are new, so ninja recompiled
# all 33,000 lifted functions each time. That is roughly half an hour per
# iteration to rebuild code that did not change, and it made editing frontend.cpp
# cost the same as changing the ROM.
#
# Staging and copying only differing files keeps the unchanged ones' mtimes, so
# ninja skips them. `cmp` over ~250 MB is disk-bound and takes seconds against
# the half hour it saves.
#
# Correctness is preserved by construction: a file whose content differs by even
# one byte is copied, and a file that disappears from the staging directory is
# deleted from the bank directory, so the two are always identical in content
# after this runs. It is a timestamp optimisation, not a decision to skip work.
STAGE="$WORK/generated_stage"
emit_title() {  # <label> <bin> <shards> [extra...]
    local label="$1" bin="$2" shards="$3"; shift 3
    run "$LOGS/emit-$label.log" "$REC" --config "$WORK/$label.toml" --bin "$bin" \
        --out "$STAGE" --bank "$label" --shards "$shards" --stable-address-shards "$@"
    grep -E "\[emit\]" "$LOGS/emit-$label.log" | sed 's/^/   /'
}

sync_banks() {
    mkdir -p "$BANKS"
    local copied=0 kept=0 removed=0 f name
    for f in "$STAGE"/*; do
        [ -f "$f" ] || continue
        name="$(basename "$f")"
        if [ -f "$BANKS/$name" ] && cmp -s "$f" "$BANKS/$name"; then
            kept=$((kept + 1))
        else
            cp -f "$f" "$BANKS/$name"
            copied=$((copied + 1))
        fi
    done
    # A bank that is no longer emitted must not linger: the runner GLOBS this
    # directory, so a stale shard would be compiled and linked into the build.
    for f in "$BANKS"/*; do
        [ -f "$f" ] || continue
        name="$(basename "$f")"
        if [ ! -f "$STAGE/$name" ]; then
            rm -f "$f"
            removed=$((removed + 1))
        fi
    done
    echo "   banks: $kept unchanged, $copied updated, $removed removed"
    [ "$copied" = 0 ] && echo "   (nothing to recompile - only the shell changed)"
    return 0
}
emit_title lsw2_arm9      "$REPO/out/raw/arm9_section_0_02000000.bin" 48
emit_title lsw2_arm7      "$REPO/out/raw/arm7.bin"                     8
emit_title lsw2_arm7_wram "$REPO/out/raw/arm7_wram_037f8000.bin"       4 --validate-live-bytes
emit_title lsw2_arm9_itcm "$REPO/out/raw/arm9_section_1_01ff8000.bin"  2 --validate-live-bytes
# POST-LIFT: guard the netgame tick's transient state-38 request so a 2nd
# child can join without the host tearing down. Runtime-gated by LSW2_TICK_TOLERATE
# (default off), so 2-player is unaffected whether or not this applies. Non-fatal.
python "$PORT/postlift_net38.py" "$STAGE" 2>&1 | sed 's/^/   /' || true
sync_banks

step "build the runner"
# NDS_GENERATED_DIR keeps BIOS banks in THIS repo; without it the runner reads
# them from inside the framework checkout.
# NDS_BOOTSTRAP_FIRMWARE drops the firmware-RAM-bank requirement, which a
# build generally cannot satisfy: the banks were captured against one specific
# firmware image, and a dump from any other console does not match it.
run "$LOGS/runner-cfg.log" cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DNDS_GENERATED_DIR="$BIOSB" \
    -DNDS_BOOTSTRAP_FIRMWARE=ON \
    -DNDS_TITLE_BANK_DIR="$BANKS" \
    -DNDS_TITLE_ROM_SHA1="$ROM_SHA1" \
    -B "$BUILD/runner" "$FRAMEWORK/runner"
run "$LOGS/runner-build.log" cmake --build "$BUILD/runner" -j "$(nproc)"

EXE="$BUILD/runner/nds_runner.exe"
[ -x "$EXE" ] || die "runner did not produce $EXE"

# THE SEAM. The framework's runner is one executable whose main() does the
# setup and then calls nds_run_interactive_frontend(). That function owns the
# SDL windows and the present loop and exposes no hook, so we replace it: link
# every runner object EXCEPT src/frontend.cpp.obj and supply our own.
#
# We keep all of main.cpp's setup - CLI, config, ROM, BIOS, bank registration,
# direct boot - and own the loop, which is where imgui, viewport policy and the
# floating touch panel have to live.
IMGUI="$PORT/frontend/vendor/imgui"
[ -f "$IMGUI/imgui.cpp" ] || die "imgui is missing from $IMGUI

It is vendored in the repository. If this is a fresh clone with submodules or
large files skipped, restore it before building the shell."

# Include paths are held in an array because the framework path can contain
# spaces, and an unquoted \$INC string word-splits into
# nonsense arguments that g++ reports as missing input files.
SHELL_INC=(-I"$FRAMEWORK/runner/src"
           -I"$FRAMEWORK/recompiler/armv4t"
           -I"$IMGUI" -I"$IMGUI/backends"
           -I"$PORT/frontend"
           -I"$UCRT/../include/SDL2")

step "build imgui"
mkdir -p "$BUILD/imgui"
IMGUI_OBJS=""
for src in "$IMGUI"/imgui.cpp "$IMGUI"/imgui_draw.cpp "$IMGUI"/imgui_tables.cpp \
           "$IMGUI"/imgui_widgets.cpp "$IMGUI"/imgui_demo.cpp \
           "$IMGUI"/backends/imgui_impl_sdl2.cpp \
           "$IMGUI"/backends/imgui_impl_sdlrenderer2.cpp \
           "$IMGUI"/backends/imgui_impl_opengl3.cpp; do
    obj="$BUILD/imgui/$(basename "${src%.cpp}").o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
        run "$LOGS/imgui-cc.log" g++ -O2 -std=c++20 -c "${SHELL_INC[@]}" \
            "$src" -o "$obj"
    fi
    IMGUI_OBJS="$IMGUI_OBJS $obj"
done
echo "   imgui built"

step "build our shell"
# EVERY .cpp IN port/frontend/, discovered rather than listed.
#
# This used to name ui.cpp and frontend.cpp explicitly, and so it silently fell
# behind every time the shell gained a file - widescreen.cpp, glpresent.cpp and
# cheats.cpp were all added without it, and the one-command build had been
# failing to link for some time while hand-built binaries kept working. A list
# that has to be maintained by hand is a list that will be wrong.
SHELL_OBJS=""
for src in "$PORT"/frontend/*.cpp; do
    # test_*.cpp carry their own main() and must stay out of the link, the same
    # rule the net loop below uses. Added when strings.cpp gained a test.
    case "$(basename "$src")" in test_*) continue ;; esac
    obj="$BUILD/shell-$(basename "${src%.cpp}").o"
    run "$LOGS/shell-cc.log" g++ -O2 -std=c++20 -c "${SHELL_INC[@]}" \
        "$src" -o "$obj"
    SHELL_OBJS="$SHELL_OBJS $obj"
done

# THE NET LAYER, minus its tests.
#
# port/net/ was built only by its own test binaries until the data-sharing
# overrides needed it linked into the game. The test_*.cpp files each carry a
# main() and must stay out, which is the one reason this is not the same blind
# glob as the loop above.
for src in "$PORT"/net/*.cpp; do
    case "$(basename "$src")" in test_*) continue ;; esac
    obj="$BUILD/net-$(basename "${src%.cpp}").o"
    run "$LOGS/shell-cc.log" g++ -O2 -std=c++20 -c "${SHELL_INC[@]}" "$src" -o "$obj"
    SHELL_OBJS="$SHELL_OBJS $obj"
done
echo "   shell sources:$(echo "$SHELL_OBJS" | tr ' ' '\n' | grep -c '\.o$') objects"

RUNNER_OBJS=$(find "$BUILD/runner/CMakeFiles/nds_runner.dir" -name "*.obj" \
              | grep -v "src/frontend.cpp.obj" | tr '\n' ' ')
[ -n "$RUNNER_OBJS" ] || die "no runner objects found to link the shell against"
run "$LOGS/shell-link.log" g++ -O2 -o "$BUILD/lsw2.exe" \
    $RUNNER_OBJS $SHELL_OBJS $IMGUI_OBJS \
    "$BUILD/runner/libnds_banks.a" \
    "$BUILD/runner/libnds_support.a" \
    "$BUILD/runner/libnds_armv4t.a" \
    "$BUILD/runner/vendor/melonds/net/libslirp/libslirp.a" \
    -L"$UCRT/../lib" -lSDL2main -lSDL2 -lws2_32 -liphlpapi -lopengl32
[ -x "$BUILD/lsw2.exe" ] || die "shell link produced no executable"

# PUT THE THING YOU RUN SOMEWHERE YOU CAN FIND IT. out/work/port-build/ is where
# the build system wants its objects; it is a silly place to go looking for a
# game. Copy the executable and the one DLL it needs into build/ at the top of
# the repo.
OUTDIR="$REPO/build"
mkdir -p "$OUTDIR"
cp -f "$BUILD/lsw2.exe" "$OUTDIR/lsw2.exe"
# THE FORK, BESIDE IT. Same binary under a second name: the executable picks
# its save from its own filename, so a copy called lsw2_bountyhunter.exe runs
# the fork's save and anything else runs the baseline's. One binary means the
# two cannot drift apart while they are still the same code; when the fork
# grows behaviour of its own this becomes a real second build.
cp -f "$BUILD/lsw2.exe" "$OUTDIR/lsw2_bountyhunter.exe"
cp -f "$BUILD/runner/nds_runner.exe" "$OUTDIR/nds_runner.exe" 2>/dev/null || true
# EVERY DLL IT NEEDS, not just SDL2. Launched from port/launch.sh these are
# found through PATH, so the shortfall is invisible - but double-clicking
# build/lsw2.exe with only SDL2.dll beside it exits 127 and the console window
# closes too fast to read. ldd names the real list.
for dll in SDL2.dll libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
    [ -f "$UCRT/$dll" ] && cp -f "$UCRT/$dll" "$OUTDIR/$dll"
done

# AND IT NEEDS ARGUMENTS. Run bare, it prints usage and exits, which as a
# double-click looks like "a cmd window flashed and nothing happened". Copy in
# a launcher that supplies them and PAUSES on failure so the reason is readable.
# It lives as a file rather than a heredoc because a .bat is full of the
# characters a heredoc mangles.
# No space in the name: a space invites quoting bugs in every script
# and shell that later refers to it.
# THE GAME'S OWN FONT, which the fork's in-game menus are drawn with.
#
# font_18 was matched to six row widths read out of a live pause menu, to
# the pixel. The port needs it beside the executable or its menus fall back to
# nothing - so the export runs here rather than being a step someone remembers.
step "export the game font"
run "$LOGS/fonts.log" python "$REPO/tools/export_custom_fonts.py"
mkdir -p "$OUTDIR/assets/fonts"
for f in "$REPO"/out/exported/godot_ui/fonts/*.lsw2font; do
    [ -e "$f" ] && cp -f "$f" "$OUTDIR/assets/fonts/"
done
echo "   fonts: $(ls "$OUTDIR/assets/fonts" | wc -l) installed"

cp -f "$PORT/frontend/PlayLSW2.bat.in" "$OUTDIR/PlayLSW2.bat"

# THE SELF-HOST MASTER SERVER, BESIDE THE GAME. Whoever hosts the lobby list
# runs one of these two exes (the server comes in a CLI flavour and a GUI flavour);
# shipping them in build/ means the host does not go hunting for a Python
# install. They are PACKAGED SEPARATELY by services/master-server-selfhost/
# build.py (it needs PyInstaller, which the recomp toolchain does not), so this
# copy is GUARDED: a missing dist exe prints a note and the recomp build carries
# on. The service still runs from source (python master_server.py) regardless.
MASTER_DIST="$REPO/services/master-server-selfhost/dist"
for exe in lsw2-master.exe lsw2-master-gui.exe; do
    if [ -f "$MASTER_DIST/$exe" ]; then
        cp -f "$MASTER_DIST/$exe" "$OUTDIR/$exe"
    else
        echo "   note: $exe not packaged (run services/master-server-selfhost/build.py); skipping"
    fi
done

echo "   -> $OUTDIR/lsw2.exe                (double-click it)"
echo "   -> $OUTDIR/lsw2_bountyhunter.exe   (the fork's save)"
echo "   -> $OUTDIR/lsw2-master.exe         (lobby list host, console)"
echo "   -> $OUTDIR/lsw2-master-gui.exe     (lobby list host, window)"
# THE TESTS, AS A BUILD STEP RATHER THAN AS A THING SOMEONE REMEMBERS.
#
# tools/run_unit_tests.sh has sixty-five entries, and this script used to run
# none of them. That is exactly the
# defect the suite's own sweeps exist to catch, one level up: sweep_test_only
# fails a function whose only caller is a test, and sweep_call_reachable fails
# one whose caller is itself dead - but nothing was asking the same question
# about the suite itself. A check that nothing invokes is not a check.
#
# IT RUNS HERE, AT THE END, AND NOT EARLIER. sweep_exe_reachable reads
# build/lsw2.exe and asks whether it is newer than the source. Run before the
# link, it would judge the PREVIOUS build's binary and fail - a false red, and
# the fastest way to teach everyone to skip the step.
#
# LSW2_SKIP_TESTS=1 skips it, and says so loudly. The suite compiles forty
# translation units, which is minutes on top of a build that is already long,
# so an escape hatch is honest - but it is opt-OUT, because the default has to
# be right for anyone who is not deliberately iterating.
step "unit tests"
if [ "${LSW2_SKIP_TESTS:-0}" = "1" ]; then
    echo "   SKIPPED (LSW2_SKIP_TESTS=1) - this build is NOT verified"
else
    bash "$REPO/tools/run_unit_tests.sh" > "$LOGS/unit.log" 2>&1
    unit_rc=$?
    tail -2 "$LOGS/unit.log" | sed 's/^/   /'
    if [ $unit_rc -ne 0 ]; then
        echo "--- failures from unit.log ---" >&2
        grep -E "FAILED|BUILD FAILED" "$LOGS/unit.log" >&2
        die "unit suite failed (full log: out/work/logs/unit.log)"
    fi
fi

step "done"
echo "$EXE"

if [ "${1:-}" = "run" ]; then
    step "launching"
    cd "$PORT"
    exec "$EXE" "$PORT/bios" --rom "$ROM" --config "$PORT/game.toml" \
        --save-path "$PORT/lsw2.sav" --boot direct --generated-firmware \
        --interactive --screen-layout separate
fi
