$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$godot = Join-Path $root 'tools\vendor\godot\Godot_v4.7.1-stable_win64.exe'
$work = Join-Path $root 'out\work'
$renders = Join-Path $root 'renders'
$approvedCharacters = Join-Path $renders 'characters'
$candidates = Join-Path $work 'candidate_character_previews'
$taskAppData = Join-Path $work 'godot_appdata'
New-Item -ItemType Directory -Force -Path $work, $renders, $approvedCharacters, $candidates, $taskAppData | Out-Null
$env:APPDATA = $taskAppData
$env:LOCALAPPDATA = $taskAppData

function Invoke-GodotPreview {
    param([string]$Name, [string]$Project, [string[]]$UserArguments)
    $arguments = @(
        '--display-driver', 'windows', '--rendering-driver', 'opengl3', '--audio-driver', 'Dummy',
        '--path', $Project, '--log-file', (Join-Path $work "godot_safe_$Name.log"), '--'
    ) + $UserArguments
    $process = Start-Process -FilePath $godot -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Godot preview '$Name' failed with exit code $($process.ExitCode)" }
}

function Invoke-StaticPreview {
    param([string]$Name, [string]$InputPath, [string]$OutputPath)
    Invoke-GodotPreview $Name (Join-Path $root 'tools\godot_preview_v2') @(
        "--input=$InputPath", "--output=$OutputPath"
    )
}

# Arena outputs are approved scene-geometry references.
Invoke-StaticPreview 'jabbas_tower' `
    (Join-Path $root 'out\exported\levels\jabbas_tower\glb\BattleArena1_Mai.glb') `
    (Join-Path $renders 'jabbas_tower.png')
Invoke-StaticPreview 'mos_eisley_streets' `
    (Join-Path $root 'out\exported\levels\mos_eisley_streets\glb\BattleArena2_Roo.glb') `
    (Join-Path $renders 'mos_eisley_streets.png')

# Zuckuss is the only currently approved complete character render. Keep the
# intact reference pose; do not bind the collapsed converted idle animation.
Invoke-StaticPreview 'zuckuss' `
    (Join-Path $root 'out\exported\characters\zuckuss\static_glb\ZuckussMiniKit.glb') `
    (Join-Path $approvedCharacters 'zuckuss.png')

# Greedo is a runtime-assembly candidate. Runtime appearance 20 consists of
# part 16 (head), part 65 (legs asset 2527), and part 128 (torso asset 2535).
# It deliberately renders outside the approved directory pending visual review.
python (Join-Path $root 'tools\decode_ttga_companion.py') `
    (Join-Path $root 'out\raw\archive\entries\2535.bin') `
    (Join-Path $work 'greedo_torso_asset_2535.png')
if ($LASTEXITCODE -ne 0) { throw 'Greedo torso decode failed' }
python (Join-Path $root 'tools\build_humanoid_candidate.py') greedo `
    --legs-texture 'out/exported/humanoid_appearances/actor_065_asset_2527.png' `
    --torso-texture 'out/work/greedo_torso_asset_2535.png'
if ($LASTEXITCODE -ne 0) { throw 'Greedo candidate build failed' }
Invoke-GodotPreview 'greedo_candidate' (Join-Path $root 'tools\godot_modular_preview') @(
    "--body=$(Join-Path $root 'out\work\humanoid_candidates\greedo\Human_Model.glb')",
    "--head=$(Join-Path $root 'out\exported\characters\greedo\playable_glb\GreedoHead.glb')",
    "--weapon=$(Join-Path $root 'out\exported\characters\greedo\playable_glb\Pistol.glb')",
    "--output=$(Join-Path $candidates 'greedo.png')",
    '--animation=BlstrIdle'
)

python (Join-Path $root 'tools\validate_render_approval.py')
if ($LASTEXITCODE -ne 0) { throw 'Canonical render approval validation failed' }
