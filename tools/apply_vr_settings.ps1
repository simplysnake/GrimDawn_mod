# Waits for Grim Dawn to exit (it rewrites options.txt on exit), backs up options.txt,
# applies VR-friendly settings, writes the mod's control.ini, then relaunches via gdvr_launch.
param([switch]$NoRelaunch)

$options = "$env:USERPROFILE\Documents\My Games\Grim Dawn\Settings\options.txt"
$control = "$env:LOCALAPPDATA\gdvr\control.ini"
$launcher = Join-Path $PSScriptRoot "..\bin\gdvr_launch.exe"

while (Get-Process "Grim Dawn" -ErrorAction SilentlyContinue) { Start-Sleep 1 }
Start-Sleep 2

$backup = "$options.pre-vr-$(Get-Date -Format yyyyMMdd-HHmmss)"
Copy-Item $options $backup
Write-Output "Backed up options to $backup"

# key = value pairs the game reads at start-up
$settings = [ordered]@{
  'resolution'        = '2560 1440'  # each eye renders at the game resolution: 4K per eye was too slow
  'screenMode'        = '1'          # windowed: fullscreen minimises (and stops rendering) when SteamVR takes focus
  'syncToRefresh'     = 'false'      # vsync off: the headset paces frames, not the 60 Hz monitor
  'depthOfField'      = 'false'      # screen-space blur is wrong per eye in VR
  'cameraShake'       = 'false'      # comfort
  'shadowQuality'     = 'medium'     # performance (scene is drawn twice)
  'reflectionQuality' = 'medium'     # performance
}
$lines = Get-Content $options
foreach ($key in $settings.Keys) {
  $pattern = "^\s*$key\s*="
  $new = "{0,-25} = {1}" -f $key, $settings[$key]
  if ($lines -match $pattern) { $lines = $lines -replace "$pattern.*", $new } else { $lines += $new }
}
Set-Content -Path $options -Value $lines -Encoding ascii
Write-Output "Applied VR settings to options.txt"

New-Item -ItemType Directory -Force (Split-Path $control) | Out-Null
@"
[exp]
offset_enabled=0
render_twice=0
[vr]
mode=2
world_scale=15
fov_deg=0
level_horizon=1
recompute_frustum=1
far_plane=70
"@ | Set-Content -Path $control -Encoding ascii
Write-Output "Wrote $control"

if (-not $NoRelaunch) { & $launcher }
