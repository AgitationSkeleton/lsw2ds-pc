# Entity capture: identify what each arena world object IS, not just where.
#
# The previous attempt at this produced 24 static actor positions and then
# could not say which was a turret and which a dispenser, because:
#
#   - the frame column was 0 on every row, so the trace could not be aligned
#     to anything (fixed: the counter no longer lives inside the periodic
#     capture block)
#   - the only RAM keyframes in the directory were written before the arena
#     level loaded, so the entity structs did not exist in them yet
#   - unbuffered per-row writes hung the emulator mid-match (fixed: 8 MB
#     buffer, plus MELONDS_TRACE_STRIDE to sample every Nth frame)
#
# This profile runs the trace and the RAM recording together, both stamped
# with the same frame index, so an entity seen sitting still in the trace can
# be read out of the RAM capture taken on a frame where it was definitely
# alive.
#
# Field 0x00 is traced alongside position: if entity actors carry a class or
# vtable pointer there, grouping actors by it names the types outright.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root 'out/work/gpudump'

# Deploy the freshly built binary to BOTH launch paths before starting.
#
# A capture was once lost because the build was deployed to
# tools/melonds_re/melonDS.exe while the emulator actually in use was
# tools/melonds_re/patched/melonDS.exe, three weeks older. It ran happily with
# the old field list and no frame counter, and nothing said so until the data
# was analysed. Copying both, from the build output, removes the possibility.
$built = Join-Path $root 'tools/vendor/melonds-src/build-ucrt/melonDS.exe'
$targets = @(
    (Join-Path $root 'tools/melonds_re/melonDS.exe'),
    (Join-Path $root 'tools/melonds_re/patched/melonDS.exe')
)
if (Get-Process melonDS -ErrorAction SilentlyContinue) {
    throw 'melonDS is already running. Close it first, or the running (possibly stale) build will be captured instead of the one just built.'
}
if (Test-Path $built) {
    foreach ($t in $targets) {
        if (Test-Path (Split-Path -Parent $t)) {
            Copy-Item $built $t -Force
            Write-Host "deployed -> $t"
        }
    }
} else {
    Write-Warning "no build at $built - launching whatever is already deployed"
}

# Never reuse a dirty directory. Stale captures from an earlier run were read
# as if they belonged to this one, which is what wasted the last session.
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force -Path $out | Out-Null

$env:MELONDS_GPUDUMP_DIR      = $out
$env:MELONDS_GPUDUMP_EVERY    = '10'        # RAM delta every 10 frames
$env:MELONDS_GPUDUMP_KEYFRAME = '300'       # full keyframe every 300 captures
$env:MELONDS_TRACE_PC         = '0x02017bcc'  # gravity - every actor, every frame
$env:MELONDS_TRACE_FIELDS     = '0x0,0xb0,0xb4,0xb8'   # class?, x, y, z
$env:MELONDS_TRACE_STRIDE     = '3'         # every 3rd frame: 20 Hz, plenty

Write-Host 'Entity capture armed.' -ForegroundColor Green
Write-Host "  output   $out"
Write-Host '  trace    0x02017bcc  fields 0x00,0xb0,0xb4,0xb8  stride 3'
Write-Host '  recording every 10 frames, keyframe every 300'
Write-Host ''
Write-Host 'Load save state 1 (arena lobby) before doing anything else.' -ForegroundColor Yellow

& (Join-Path $root 'tools/melonds_re/melonDS.exe')
