# Import LSW2's ARM9 image into Ghidra and answer a batch of address questions.
#
# WHY THE RAW IMAGE RATHER THAN THE .nds
# --------------------------------------
# NTRGhidra loads a full cartridge but it ASKS - a "Choose CPU" dialog for ARM7
# vs ARM9 - which cannot be answered headlessly, and left to itself the headless
# import picks ARM:LE:32:v4t, i.e. the ARM7. The game code is on the ARM9
# (ARMv5TE).
#
# out/raw/arm9_section_0_02000000.bin is already the extracted ARM9 section, and
# loading it raw at its real base is deterministic, prompt-free, and - the part
# that matters - every address in the project's ROM lookup index lines up 1:1 with
# what Ghidra shows, because it is the same image this project has been
# disassembling all along.
#
#   powershell -ExecutionPolicy Bypass -File tools/ghidra_arm9.ps1
#   powershell ... -File tools/ghidra_arm9.ps1 -Addrs "0209B88C,0202E8E4"
#
# -Reimport forces a fresh import and re-analysis (slow); without it an existing
# project is reused and only the decompile pass runs.
# NOTE the interface: -Addrs takes ONE COMMA-SEPARATED STRING, not an array.
# Invoking a script with `powershell -File x.ps1 -Addrs $array` flattens the
# array and binds only its FIRST element, which silently turned a 24-address
# batch into a single function and looked like a decompiler failure twice.
param(
    [string] $Addrs = "",
    [switch] $Reimport,
    [string] $Out = "out/ghidra/decompiled.c"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$env:JAVA_HOME = Join-Path $root "tools/vendor/jdk21"
$env:PATH = "$env:JAVA_HOME\bin;$env:PATH"

$headless = Join-Path $root "tools/vendor/ghidra_1204/support/analyzeHeadless.bat"
$proj     = Join-Path $root "out/ghidra_attack"
$image    = Join-Path $root "out/raw/arm9_section_0_02000000.bin"
$scripts  = Join-Path $root "tools/ghidra_scripts"
$outFile  = Join-Path $root $Out

if (-not (Test-Path $headless)) { throw "ghidra not found at $headless" }
if (-not (Test-Path $image))    { throw "arm9 image not found at $image" }
New-Item -ItemType Directory -Force $proj | Out-Null
New-Item -ItemType Directory -Force (Split-Path -Parent $outFile) | Out-Null

# A path with spaces or parentheses breaks analyzeHeadless.bat's own argument
# parsing - the cartridge filename "... (USA).nds" fails outright - so the
# project lives somewhere clean.
# NOT $args - that is a RESERVED PowerShell automatic variable, and assigning
# to it silently truncated the argument list so only the first address ever
# reached the post-script.
$ghArgs = @($proj, "ARM9")
$existing = Join-Path $proj "ARM9.gpr"
if ($Reimport -or -not (Test-Path $existing)) {
    Write-Host "importing and analysing $image (this takes a while)"
    $ghArgs += @(
        "-import", $image,
        "-processor", "ARM:LE:32:v5t",
        "-loader", "BinaryLoader",
        "-loader-baseAddr", "0x02000000",
        "-overwrite")
} else {
    Write-Host "reusing existing project $existing"
    $ghArgs += @("-process", "arm9_section_0_02000000.bin", "-noanalysis")
}

$addrList = @()
if ($Addrs -ne "") {
    $addrList = $Addrs.Split(",") | ForEach-Object { $_.Trim() } |
        Where-Object { $_ -ne "" }
}

if ($addrList.Count -gt 0) {
    # The address list goes through a FILE. analyzeHeadless does not reliably
    # forward a long trailing argument list to a post-script - only the first
    # extra argument ever arrived, which silently truncated a 20-address batch
    # to one and looked like a decompiler failure.
    $listFile = Join-Path $proj "addrs.txt"
    $addrList | Set-Content -Path $listFile -Encoding ascii
    $ghArgs += @("-scriptPath", $scripts,
               "-postScript", "DecompileAddrs.java", $listFile, $outFile)
}

& $headless @ghArgs 2>&1 | Where-Object {
    $_ -match "ERROR|WARN|Import|analysis|wrote|Decompil|elapsed"
} | Select-Object -Last 20

if ($addrList.Count -gt 0 -and (Test-Path $outFile)) {
    Write-Host "`n--- $Out ---"
    Get-Content $outFile | Select-Object -First 60
}
