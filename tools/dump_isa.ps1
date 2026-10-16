# Ask Ghidra which functions are Thumb. Nothing else in the export records it,
# and it cannot be inferred from the address - see DumpInstructionSet.java.
$root = Split-Path -Parent $PSScriptRoot
$headless = Join-Path $root "tools/vendor/ghidra_1204/support/analyzeHeadless.bat"
$proj = Join-Path $root "out/ghidra"
$scripts = Join-Path $root "tools/ghidra_scripts"
$programs = @(
    @{ name = "arm9_section_0_02000000.bin"; out = "out/work/isa/main.txt" },
    @{ name = "arm9_section_1_01ff8000.bin"; out = "out/work/isa/itcm.txt" }
)
foreach ($p in $programs) {
    $outPath = Join-Path $root $p.out
    Write-Host "dumping instruction sets for $($p.name)"
    & $headless $proj "ARM9" -process $p.name -noanalysis `
        -scriptPath $scripts -postScript "DumpInstructionSet.java" $outPath
}
