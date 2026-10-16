# Decompile the WHOLE ARM9 - BOTH sections - to disk, once, so later lookups
# are a grep rather than a Ghidra run.
#
# WHY. Every ROM question has cost a full headless invocation - import, analyse,
# decompile a few addresses - taking minutes and having to happen inside whatever
# investigation is asking. Running this once turns all of that into a file read.
#
#   powershell -ExecutionPolicy Bypass -File tools/ghidra_decompile_all.ps1
#   powershell ... -File tools/ghidra_decompile_all.ps1 -Reimport   # re-analyse
#   powershell ... -File tools/ghidra_decompile_all.ps1 -NoFill     # skip gaps
#
# BOTH SECTIONS. A first pass covered only section 0 and left ITCM entirely
# absent - 0 functions over 21,952 bytes at 0x01FF8000 - even though real code
# lives there: the animation-event opcode dispatcher at 0x01FFAE50 and the
# step-up helper at 0x01FF9188 were both read out of it by hand. ITCM is
# imported as its own program in the same project and decompiled to its own
# output directory.
#
# GAP FILLING. Plain auto-analysis left 89,779 bytes of section 0 unclaimed -
# code nothing referenced, or that a data island broke the flow into.
# FillGaps.java disassembles 4-byte-aligned runs of undefined bytes and creates
# functions where that produced instructions. It does not guess at data: a run
# that fails to disassemble is left alone, which is right for a literal pool.
#
# WHY THE RAW IMAGES RATHER THAN THE .nds. NTRGhidra loads a cartridge but ASKS
# (a "Choose CPU" dialog for ARM7 vs ARM9) and cannot be answered headlessly;
# left alone the import picks ARM:LE:32:v4t, the ARM7. The game code is ARM9,
# ARMv5TE. Loading the extracted sections raw at their real bases means every
# ROM address this project records lines up 1:1 with what Ghidra shows.

param(
    [switch] $Reimport,
    [switch] $NoFill,
    [string] $Out = "out/ghidra/decomp"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$env:JAVA_HOME = Join-Path $root "tools/vendor/jdk21"
$env:PATH = "$env:JAVA_HOME\bin;$env:PATH"

$headless = Join-Path $root "tools/vendor/ghidra_1204/support/analyzeHeadless.bat"
$proj     = Join-Path $root "out/ghidra"
$scripts  = Join-Path $root "tools/ghidra_scripts"

if (-not (Test-Path $headless)) { throw "ghidra not found at $headless" }
New-Item -ItemType Directory -Force $proj | Out-Null

$projectName = "ARM9"

# name, image, base, processor, output subdirectory
#
# THE ARM7 IS A DIFFERENT PROCESSOR AND WAS MISSING ENTIRELY. 165,536 bytes at
# 0x02380000 with zero coverage - the SPU driver, the touch and key sampling,
# and the far side of the IPC the ARM9 talks to through FUN_020D12B0 and
# FUN_020D1338. It is an ARM7TDMI, so it decodes as ARM:LE:32:v4t; loading it
# as v5t like the ARM9 would mis-decode every ARMv5 instruction it does not
# have and, worse, silently accept them.
#
# Section 2 (0x027E0000, 672 bytes) is DTCM initialisation data, not code, and
# is deliberately not imported.
$sections = @(
    @{ file = "arm9_section_0_02000000.bin"; base = "0x02000000"; proc = "ARM:LE:32:v5t"; out = $Out },
    @{ file = "arm9_section_1_01ff8000.bin"; base = "0x01FF8000"; proc = "ARM:LE:32:v5t"; out = "$Out`_itcm" },
    @{ file = "arm7.bin";                    base = "0x02380000"; proc = "ARM:LE:32:v4t"; out = "$Out`_arm7" }
)

foreach ($s in $sections) {
    $image = Join-Path $root "out/raw/$($s.file)"
    if (-not (Test-Path $image)) {
        Write-Host "skipping $($s.file) - not present"
        continue
    }
    $outDir = Join-Path $root $s.out
    New-Item -ItemType Directory -Force $outDir | Out-Null

    # Ghidra names the program after the file, and -process matches that name.
    $programName = $s.file
    $alreadyImported = $false
    $repDir = Join-Path $proj "$projectName.rep"
    if (Test-Path $repDir) {
        $alreadyImported = (Get-ChildItem $repDir -Recurse -Filter "*$programName*" `
            -ErrorAction SilentlyContinue | Measure-Object).Count -gt 0
    }

    $post = @()
    if (-not $NoFill) { $post += @("-preScript", "FillGaps.java") }
    $post += @("-postScript", "DecompileAll.java", $s.out)

    if ($Reimport -or -not $alreadyImported) {
        Write-Host "importing and analysing $programName at $($s.base)"
        $ghArgs = @(
            $proj, $projectName,
            "-import", $image,
            "-processor", $s.proc,
            "-loader", "BinaryLoader",
            "-loader-baseAddr", $s.base,
            "-scriptPath", $scripts
        ) + $post + @("-overwrite")
    } else {
        Write-Host "reusing the analysed program $programName"
        $ghArgs = @(
            $proj, $projectName,
            "-process", $programName,
            "-noanalysis",
            "-scriptPath", $scripts
        ) + $post
    }

    $started = Get-Date
    & $headless @ghArgs
    $elapsed = (Get-Date) - $started

    $cDir = Join-Path $outDir "c"
    $count = 0
    if (Test-Path $cDir) {
        $count = (Get-ChildItem $cDir -Filter *.c -ErrorAction SilentlyContinue).Count
    }
    Write-Host ("  {0}: {1} function file(s) in {2:mm\:ss}" -f $programName, $count, $elapsed)
    Write-Host ""
}

Write-Host "outputs:"
Write-Host "  section 0   $Out/"
Write-Host "  ITCM        $Out`_itcm/"
Write-Host "  ARM7        $Out`_arm7/"
Write-Host ""
Write-Host "query without running Ghidra:"
Write-Host "  python tools/ghidra_lookup.py 0x0203764C -c"
Write-Host "  python tools/ghidra_lookup.py --callers 0x02007268"
Write-Host "  python tools/ghidra_lookup.py --stats"
