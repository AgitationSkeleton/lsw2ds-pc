$ErrorActionPreference = 'Stop'

$toolRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$vendorRoot = Join-Path $toolRoot 'vendor'
$pythonRoot = Join-Path $vendorRoot 'python'
$tempRoot = Join-Path (Split-Path -Parent $toolRoot) 'out\work\tmp'

New-Item -ItemType Directory -Force -Path $vendorRoot, $pythonRoot, $tempRoot | Out-Null
$env:TEMP = (Resolve-Path $tempRoot).Path
$env:TMP = $env:TEMP

python -m pip install --upgrade --target $pythonRoot `
    'git+https://github.com/RoadrunnerWMC/ndspy.git@629c18e3b555dd33da71c741912fc4b814ef7492' `
    'capstone==5.0.9' `
    'unicorn==2.1.4'

$apiculaZip = Join-Path $vendorRoot 'apicula-latest-windows.zip'
$apiculaRoot = Join-Path $vendorRoot 'apicula'
$apiculaUrl = 'https://github.com/scurest/apicula/releases/download/continuous/apicula-latest-windows.zip'
$expectedSha256 = '2BAA745DE84F299086713B3F52392D3D5D5B100279D4C0FC745E9556722C32DB'

Invoke-WebRequest -Uri $apiculaUrl -OutFile $apiculaZip
$actualSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $apiculaZip).Hash
if ($actualSha256 -ne $expectedSha256) {
    throw "Unexpected apicula archive SHA-256: $actualSha256"
}
New-Item -ItemType Directory -Force -Path $apiculaRoot | Out-Null
Expand-Archive -LiteralPath $apiculaZip -DestinationPath $apiculaRoot -Force

& (Join-Path $apiculaRoot 'apicula.exe') -V

$fluidSynthZip = Join-Path $vendorRoot 'fluidsynth-2.4.6-win10-x64.zip'
$fluidSynthRoot = Join-Path $vendorRoot 'fluidsynth-2.4.6'
$fluidSynthUrl = 'https://github.com/FluidSynth/fluidsynth/releases/download/v2.4.6/fluidsynth-2.4.6-win10-x64.zip'
$fluidSynthExpectedSha256 = 'F8344FDE598C2B5C43E2DDA6446E7B27E403268FB1816F4B3515CB644E6DD7B0'

Invoke-WebRequest -Uri $fluidSynthUrl -OutFile $fluidSynthZip
$fluidSynthActualSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $fluidSynthZip).Hash
if ($fluidSynthActualSha256 -ne $fluidSynthExpectedSha256) {
    throw 'Unexpected FluidSynth archive SHA-256: ' + $fluidSynthActualSha256
}
New-Item -ItemType Directory -Force -Path $fluidSynthRoot | Out-Null
Expand-Archive -LiteralPath $fluidSynthZip -DestinationPath $fluidSynthRoot -Force

& (Join-Path $fluidSynthRoot 'bin\fluidsynth.exe') --version
