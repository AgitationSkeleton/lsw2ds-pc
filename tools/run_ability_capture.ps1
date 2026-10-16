# Capture profile for the ability test plan.
#
# Everything needed in one run, so the session does not have to be repeated:
#
#   events.csv   named routine hits - who did what, where, on which frame
#   input.csv    both players' buttons and stylus, every frame
#   trace.csv    the per-actor gravity trace, for entity positions
#   inst0/       main RAM every 10 frames, for anything decoded later
#
# The event set is deliberately wide. Tracing a routine that turns out to be
# irrelevant costs a few columns; missing one costs the whole session.
#
#   0x020075EC  tank scheme - runs once per player per frame with the player
#               actor in r0. This is what names player 1 and player 2, and everything else
#               keys off it. Without this the log cannot attribute anything.
#   0x02007408  free-movement scheme - the other control path
#   six handlers from the controller-mode table: the ability candidates. Which
#               is which is exactly what this session determines
#   0x02047ED8  the targeting cone test - fires when something is aimed at
#   0x02012744  base world-object tick - every buildable delegates to it
#   0x020265E4  flag turret tick      0x020216E0  grapple tick
#   0x0201F6F0  lever tick            0x0201D1E0  the animated 2-instance class
#
# Fields dereferenced from r0 on every event: the character index at +0x6BB and
# the position triple at +0xB0/B4/B8. So each row already says who and where,
# with no join needed.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root 'out/work/gpudump_abilities'

if (Get-Process melonDS -ErrorAction SilentlyContinue) {
    throw 'melonDS is already running. Close it first, or the running build gets captured instead of the one just built.'
}

# Deploy the current build to BOTH launch paths. A previous session was lost
# because only one was updated and the stale binary ran with an old field list.
$built = Join-Path $root 'tools/vendor/melonds-src/build-ucrt/melonDS.exe'
if (-not (Test-Path $built)) { throw "no build at $built" }
foreach ($t in @('tools/melonds_re/melonDS.exe','tools/melonds_re/patched/melonDS.exe')) {
    $p = Join-Path $root $t
    if (Test-Path (Split-Path -Parent $p)) { Copy-Item $built $p -Force }
}

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force -Path $out | Out-Null

$fields = '0x6bb,0xb0,0xb4,0xb8'
$events = @(
    "0x020075ec:PLAYER_TICK:$fields",
    "0x02007408:FREE_SCHEME:$fields",
    "0x02046d84:ABILITY_A:$fields",
    "0x02046e80:ABILITY_B:$fields",
    "0x02047008:ABILITY_C:$fields",
    "0x020470d0:ABILITY_D:$fields",
    "0x020470e4:ABILITY_E:$fields",
    "0x020473a8:ABILITY_F:$fields",
    "0x02047ed8:TARGETING:$fields",
    "0x02012744:ENTITY_TICK:0xbe,0xc1,0xb0,0xb4",
    "0x020265e4:TURRET_TICK:0xbe,0xc1,0xb0,0xb4",
    "0x020216e0:GRAPPLE_TICK:0xbe,0xc1,0xb0,0xb4",
    "0x0201f6f0:LEVER_TICK:0xbe,0xc1,0xb0,0xb4",
    "0x0201d1e0:ANIM_TICK:0xbe,0xc1,0xb0,0xb4"
) -join ';'

$env:MELONDS_GPUDUMP_DIR      = $out
$env:MELONDS_GPUDUMP_EVERY    = '10'
$env:MELONDS_GPUDUMP_KEYFRAME = '300'
$env:MELONDS_INPUT_LOG        = '1'
$env:MELONDS_EVENTS           = $events
# The gravity trace still runs, at a stride, so entity positions keep coming.
$env:MELONDS_TRACE_PC         = '0x02017bcc'
$env:MELONDS_TRACE_FIELDS     = '0x0,0xb0,0xb4,0xb8'
$env:MELONDS_TRACE_STRIDE     = '3'

Write-Host 'Ability capture armed - all logs enabled.' -ForegroundColor Green
Write-Host "  output    $out"
Write-Host '  events    14 routines: player schemes, 6 ability candidates, targeting, 5 entity ticks'
Write-Host '  input     both instances, buttons + stylus, every frame'
Write-Host '  trace     gravity, stride 3, with vtable and position'
Write-Host '  RAM       every 10 frames, keyframe every 300'
Write-Host ''
Write-Host 'Follow the ability test plan:' -ForegroundColor Yellow
Write-Host '  one ability at a time, 3 seconds still between each, player 1 then player 2.'
Write-Host ''

# Launch from patched/, which is the only path holding the 34 Qt and SDL
# runtime DLLs. tools/melonds_re/melonDS.exe has none beside it and exits
# immediately with STATUS_DLL_NOT_FOUND (0xC0000135) - which looks exactly like
# a crash and wasted a launch attempt before this was pinned down.
#
# ONE process only. Local wireless works exclusively between instances INSIDE
# one process: LocalMP's queue and lock are members of a per-process
# object, and the MAC is derived from the instance id, so two separately
# launched processes are both instance 0, share a MAC, and can never see each
# other. The second DS has to be created from the menu instead.
$exe = Join-Path $root 'tools/melonds_re/patched/melonDS.exe'
$rom = Get-ChildItem (Join-Path $root 'rom') -Filter '*.nds' | Select-Object -First 1
if (-not $rom) { throw 'no .nds found in rom/' }

# ROM as a positional argument and --loadstate 1, both handled by melonDS
# itself. --loadstate was added for this: it triggers the existing Load State
# menu action 2.5 seconds after boot, which is long enough for the machine to
# be up and reuses the checks the menu path already does.
# --mp 2 brings up the second DS inside this same process, which is the only
# arrangement local wireless works in, and gives it the same ROM and save state.
Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe) `
    -ArgumentList @("`"$($rom.FullName)`"", '--loadstate', '1', '--mp', '2')

Write-Host ''
Write-Host "ROM auto-loading with save state 1: $($rom.Name)" -ForegroundColor Cyan
Write-Host 'ONE process - deliberate. Local wireless only links instances inside one process.' -ForegroundColor Cyan
Write-Host ''
Write-Host 'Both DS windows come up automatically with the ROM and save state 1.' -ForegroundColor Yellow
Write-Host 'The second appears a few seconds after the first - it is staggered so the'
Write-Host 'ROM load and state restore each land on a machine that has settled.'
Write-Host ''
Write-Host 'All that is left by hand:' -ForegroundColor Yellow
Write-Host '  host the wireless game in one window, browse for it in the other.'
Write-Host ''
Write-Host 'Both instances log separately as inst0 and inst1.' -ForegroundColor Cyan
