param(
    [int[]]$Module = (0..28),
    [string]$Output = 'out\snd\music_midi',
    [int]$SampleRate = 44100
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$fluidSynth = Join-Path $PSScriptRoot 'vendor\fluidsynth-2.4.6\bin\fluidsynth.exe'
$soundfontRoot = Join-Path $repoRoot 'out\snd\soundfonts'
$sequenceRoot = Join-Path $repoRoot 'out\snd\sequences'
$outputRoot = if ([IO.Path]::IsPathRooted($Output)) {
    $Output
} else {
    Join-Path $repoRoot $Output
}

if (-not (Test-Path -LiteralPath $fluidSynth -PathType Leaf)) {
    throw 'FluidSynth is missing; run tools\install_vendor.ps1 first'
}
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

foreach ($moduleIndex in $Module) {
    if ($moduleIndex -lt 0 -or $moduleIndex -gt 28) {
        throw 'Module index must be in the range 0..28'
    }
    $stem = 'module_{0:D2}' -f $moduleIndex
    $soundfont = Join-Path $soundfontRoot ($stem + '.sf2')
    $sequence = Join-Path $sequenceRoot ($stem + '.mid')
    $wave = Join-Path $outputRoot ($stem + '.wav')
    if (-not (Test-Path -LiteralPath $soundfont -PathType Leaf)) {
        throw 'Missing soundfont: ' + $soundfont
    }
    if (-not (Test-Path -LiteralPath $sequence -PathType Leaf)) {
        throw 'Missing sequence: ' + $sequence
    }
    & $fluidSynth -ni -q -F $wave -r $SampleRate $soundfont $sequence
    if ($LASTEXITCODE -ne 0) {
        throw 'FluidSynth failed for module ' + $moduleIndex
    }
    $size = (Get-Item -LiteralPath $wave).Length
    Write-Host ($stem + ': ' + $size + ' bytes')
}
