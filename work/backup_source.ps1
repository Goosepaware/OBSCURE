# Backup OBScure source to a location outside the project drive.
# Keeps the most recent 14 archives.

$ErrorActionPreference = 'Stop'

$root = 'D:\program\OBScure'
$dest = 'C:\Users\34887\Documents\OBScure_backups'
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$zip = Join-Path $dest "OBScure_source_$stamp.zip"

New-Item -ItemType Directory -Force -Path $dest | Out-Null

$items = @(
    (Join-Path $root 'src'),
    (Join-Path $root 'shaders'),
    (Join-Path $root 'docs'),
    (Join-Path $root 'tools'),
    (Join-Path $root 'work\build_cli.bat'),
    (Join-Path $root 'work\build_opencv.bat'),
    (Join-Path $root 'work\build_player.bat'),
    (Join-Path $root 'CMakeLists.txt'),
    (Join-Path $root 'README.md'),
    (Join-Path $root 'AGENTS.md'),
    (Join-Path $root '.gitignore')
) | Where-Object { Test-Path -LiteralPath $_ }

Compress-Archive -LiteralPath $items -DestinationPath $zip -Force
Write-Output "backup: $zip"

$trash = Join-Path 'D:\_trash' ("old_backups_" + $stamp)
New-Item -ItemType Directory -Force -Path $trash | Out-Null
Get-ChildItem $dest -Filter 'OBScure_source_*.zip' |
    Sort-Object LastWriteTime -Descending |
    Select-Object -Skip 14 |
    ForEach-Object {
        Move-Item -LiteralPath $_.FullName -Destination (Join-Path $trash $_.Name) -Force -ErrorAction SilentlyContinue
    }
