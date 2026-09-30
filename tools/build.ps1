param([string]$Keil = 'C:\Keil_v5\UV4\UV4.exe')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$project = Join-Path $root 'MDK-ARM\LiteRadio_Plus.uvprojx'
$output = Join-Path $root 'artifacts'
New-Item -ItemType Directory -Force $output | Out-Null
$log = Join-Path $output 'elrs3-build.log'
$started = Get-Date
$arguments = '-r "{0}" -t LiteRadio_Plus_SX1280 -o "{1}"' -f $project, $log
$process = Start-Process -FilePath $Keil -ArgumentList $arguments -WorkingDirectory $root -WindowStyle Hidden -Wait -PassThru
if (-not (Test-Path -LiteralPath $log)) { throw 'Keil did not produce a build log.' }
Get-Content -LiteralPath $log | Select-Object -Last 12
if ($process.ExitCode -gt 1 -or -not (Select-String -LiteralPath $log -Pattern ' - 0 Error\(s\)')) {
    throw "Keil build failed (exit $($process.ExitCode)); see $log"
}
$files = @{
    'MDK-ARM\LiteRadio_Plus.bin' = 'LiteRadio_2_SE_V2_ELRS3_USB.bin'
    'MDK-ARM\LiteRadio_Plus\LiteRadio_Plus.hex' = 'LiteRadio_2_SE_V2_ELRS3_USB.hex'
    'MDK-ARM\LiteRadio_Plus\LiteRadio_Plus.axf' = 'LiteRadio_2_SE_V2_ELRS3_USB.axf'
    'MDK-ARM\LiteRadio_Plus\LiteRadio_Plus.map' = 'LiteRadio_2_SE_V2_ELRS3_USB.map'
}
foreach ($source in $files.Keys) {
    $path = Join-Path $root $source
    if ((Get-Item -LiteralPath $path).LastWriteTime -lt $started) { throw "Stale artifact: $path" }
    Copy-Item -LiteralPath $path -Destination (Join-Path $output $files[$source])
}
Get-ChildItem -LiteralPath $output -Filter 'LiteRadio_2_SE_V2_ELRS3_USB.*' |
    Get-FileHash -Algorithm SHA256 |
    ForEach-Object { '{0}  {1}' -f $_.Hash.ToLower(), (Split-Path -Leaf $_.Path) } |
    Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt')
Write-Output "Firmware and checksums: $output"
