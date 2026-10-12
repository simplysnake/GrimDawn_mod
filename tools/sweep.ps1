# Sweeps one [vr] setting in control.ini and reports average perf for each value.
#   sweep.ps1 -Key far_plane -Values 0,90,70,50 [-Seconds 6]
param([string]$Key, [string[]]$Values, [int]$Seconds = 6)

$control = "$env:LOCALAPPDATA\gdvr\control.ini"
$log = "$env:LOCALAPPDATA\gdvr\gdvr.log"

function Set-Key($k, $v) {
  $lines = Get-Content $control
  if ($lines -match "^$k=") { $lines = $lines -replace "^$k=.*", "$k=$v" }
  else { $lines = $lines -replace '^\[vr\]$', "[vr]`r`n$k=$v" }
  Set-Content $control $lines -Encoding ascii
}

foreach ($v in $Values) {
  Set-Key $Key $v
  Start-Sleep 2                      # settle (control.ini is polled once a second)
  $start = Get-Date
  Start-Sleep $Seconds
  $lines = Get-Content $log | Select-String '^\[(\d\d:\d\d:\d\d)\.\d+\] perf: ' | Where-Object {
    [datetime]::ParseExact($_.Matches[0].Groups[1].Value, 'HH:mm:ss', $null).TimeOfDay -ge $start.TimeOfDay }
  $num = { param($re) ($lines | ForEach-Object { if ($_.Line -match $re) { [double]$matches[1] } } | Measure-Object -Average).Average }
  "{0}={1,-6} n={2}  fps {3,5:N1}  frame {4,5:N1} ms  eye cpu {5,4:N1}+{6,4:N1}  gpu eye {7,4:N1}+{8,4:N1}" -f `
    $Key, $v, $lines.Count, (& $num 'perf: (\d+) fps'), (& $num 'cpu frame ([\d.]+)'),
    (& $num 'eye cpu ([\d.]+) \+'), (& $num 'eye cpu [\d.]+ \+ ([\d.]+)'),
    (& $num 'gpu eye ([\d.]+) \+'), (& $num 'gpu eye [\d.]+ \+ ([\d.]+)')
}
