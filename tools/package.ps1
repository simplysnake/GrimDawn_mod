# Builds a release of Grim Dawn VR and puts it in dist\GrimDawnVR-<version>\ (+ a .zip next to it).
#
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1
#
# Release builds leave out the development extras (logs in the repo, memory dumps, hard-coded game path)
# and are built in build-release\, so the development build in bin\ is untouched.
param([string]$CMake = "")

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
if (-not $CMake) {
  $CMake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
  if (-not $CMake) {
    $CMake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  }
}
$build = Join-Path $repo "build-release"

& $CMake -S $repo -B $build -A x64 -DGDVR_DEV=OFF | Out-Host
if ($LASTEXITCODE) { throw "cmake configure failed" }
& $CMake --build $build --config Release --target gdvr gdvr_launch gdvr_proxy | Out-Host
if ($LASTEXITCODE) { throw "build failed" }

$version = (Select-String -Path (Join-Path $repo "CMakeLists.txt") -Pattern 'project\(gdvr VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$name = "GrimDawnVR-$version"
$dist = Join-Path $repo "dist"
$out = Join-Path $dist $name
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out, "$out\skybox", "$out\licenses" | Out-Null

$bin = Join-Path $build "out"
Copy-Item "$bin\gdvr_launch.exe", "$bin\gdvr.dll", "$bin\dinput8.dll" $out
Copy-Item "$repo\skybox\*" "$out\skybox"
Copy-Item "$repo\LICENSE.md", "$repo\THIRD_PARTY_NOTICES.md" $out
Copy-Item "$repo\docs\INSTALL.md" "$out\README.md"

$deps = Join-Path $build "_deps"
Copy-Item "$deps\minhook-src\LICENSE.txt" "$out\licenses\MinHook.txt"
Copy-Item "$deps\imgui-src\LICENSE.txt" "$out\licenses\DearImGui.txt"
Copy-Item "$deps\openxr-src\LICENSE" "$out\licenses\OpenXR-SDK.txt"
Copy-Item "$deps\openxr-src\src\external\jsoncpp\LICENSE" "$out\licenses\jsoncpp.txt"

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $out -DestinationPath $zip
Write-Output "Release: $out"
Write-Output "Zip:     $zip"

# Installer (Inno Setup 6: winget install JRSoftware.InnoSetup)
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($iscc) {
  & $iscc /Q "/DAppVersion=$version" "/DSourceDir=$out" "/DOutputDir=$dist" (Join-Path $repo "installer\GrimDawnVR.iss") | Out-Host
  if ($LASTEXITCODE) { throw "installer build failed" }
  Write-Output "Setup:   $(Join-Path $dist "GrimDawnVR-Setup-$version.exe")"
} else {
  Write-Output "Inno Setup not found: no installer built (winget install JRSoftware.InnoSetup)"
}
