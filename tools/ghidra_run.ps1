# Run a Ghidra script headless, working around a broken external script path.
#
# THE WORKAROUND, AND WHY IT IS NEEDED. Passing -scriptPath a directory outside
# the Ghidra install fails in this setup with
#
#   Failed to get OSGi bundle containing script: ...\tools\ghidra_scripts\X.java
#
# for every script, including ones that had run before - so it is the bundle
# registration for external directories that is broken, not the scripts. Proven
# by copying the identical file into Ghidra's own ghidra_scripts directory,
# where it compiles and runs first time.
#
# So that is what this does: stage the script into the install, run it, and take
# it out again. tools/ghidra_scripts stays the place scripts are kept and
# versioned; the copy is transient.
#
#   powershell -File tools/ghidra_run.ps1 -Script DumpInstructionSet.java `
#       -Program arm9_section_0_02000000.bin -Args @("out.txt")
param(
    [Parameter(Mandatory = $true)][string]$Script,
    [Parameter(Mandatory = $true)][string]$Program,
    [string[]]$Args = @(),
    [switch]$Analyze,
    [string]$Ghidra = "ghidra_1212"
)

$root = Split-Path -Parent $PSScriptRoot
$headless = Join-Path $root "tools/vendor/$Ghidra/support/analyzeHeadless.bat"
$stage = Join-Path $root "tools/vendor/$Ghidra/Ghidra/Features/Base/ghidra_scripts"
$src = Join-Path $root "tools/ghidra_scripts/$Script"
$proj = Join-Path $root "out/ghidra"

if (-not (Test-Path $src)) { Write-Error "no such script: $src"; exit 1 }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item $src $stage -Force

try {
    $a = @($proj, "ARM9", "-process", $Program)
    if (-not $Analyze) { $a += "-noanalysis" }
    $a += @("-postScript", $Script) + $Args
    & $headless @a
} finally {
    Remove-Item (Join-Path $stage $Script) -Force -ErrorAction SilentlyContinue
}
