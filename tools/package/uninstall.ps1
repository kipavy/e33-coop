# E33 Coop - uninstall: removes dwmapi.dll and the ue4ss folder from the game's Win64 folder (saves are not touched).
# Remember to clear the Steam launch options afterwards.
param([string]$GameDir = "")
$ErrorActionPreference = "Stop"
if (-not $GameDir) {
    $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    $libs = @($steam)
    $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) { foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) { $libs += $m.Groups[1].Value.Replace("\\", "\") } }
    foreach ($l in $libs) { $g = Join-Path $l "steamapps\common\Expedition 33"; if (Test-Path $g) { $GameDir = $g } }
}
$win64 = Join-Path $GameDir "Sandfall\Binaries\Win64"
if (Get-Process "SandFall-Win64-Shipping" -ErrorAction SilentlyContinue) { throw "Close the game first." }
foreach ($p in @((Join-Path $win64 "dwmapi.dll"), (Join-Path $win64 "ue4ss"))) { if (Test-Path $p) { Remove-Item $p -Recurse -Force } }
Write-Output "E33 Coop removed from $win64. Clear the Steam launch options too."
