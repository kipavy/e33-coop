# Prévoit les tirages de gameplay dérivés (D10) pour une graine, sans lancer le jeu.
# Même calcul que native/sync/coop.cpp (derived_uniform, fnv32).
#   draw-preview.ps1 -Seed <n> -Stream "<Classe:Fonction:Tirage#e>" [-Turns "3,6"] [-Rank 0] [-Min 0 -Max 3]
#     #e = emplacement de combat de l ennemi appelant (D11). Ciblage : -Stream "Targeting:GetRandomAliveHero#e"
#     -Min 0 -Max (héros vivants - 1) donne l index du héros visé parmi les vivants triés par emplacement.
#   draw-preview.ps1 -FindFrom 1 -FindCount 200 -Stream ... -Turns "3,6" -Min 0 -Max 3 -Want 3
param(
    [string]$Seed = "0",
    [Parameter(Mandatory = $true)][string]$Stream,
    [string]$Turns = "3",          # liste séparée par des virgules (passage via -File)
    [int]$Rank = 0,
    [int]$Min = 0, [int]$Max = 0,
    [int]$FindFrom = -1, [int]$FindCount = 100, [int]$Want = -1
)
$M = [System.Numerics.BigInteger]::Pow(2, 64)
function U64([System.Numerics.BigInteger]$x) { $r = $x % $M; if ($r -lt 0) { $r += $M }; return $r }
function SplitMix([System.Numerics.BigInteger]$x) {
    $x = U64 ($x + [System.Numerics.BigInteger]::Parse("11400714819323198485"))      # 0x9E3779B97F4A7C15
    $x = U64 (($x -bxor ($x / [System.Numerics.BigInteger]::Pow(2, 30))) * [System.Numerics.BigInteger]::Parse("13787848793156543929"))   # 0xBF58476D1CE4E5B9
    $x = U64 (($x -bxor ($x / [System.Numerics.BigInteger]::Pow(2, 27))) * [System.Numerics.BigInteger]::Parse("10723151780598845931"))   # 0x94D049BB133111EB
    return $x -bxor ($x / [System.Numerics.BigInteger]::Pow(2, 31))
}
function Fnv32([string]$s) {
    $h = [uint64]2166136261
    foreach ($c in [Text.Encoding]::UTF8.GetBytes($s)) { $h = (($h -bxor $c) * 16777619) % 4294967296 }
    return $h
}
function Uniform([System.Numerics.BigInteger]$seed, [int]$turn, [uint64]$key, [int]$rank) {
    $a = SplitMix ([System.Numerics.BigInteger]$turn * [System.Numerics.BigInteger]::Pow(2, 32) + $key)
    $b = SplitMix ([System.Numerics.BigInteger]::Parse("1592590336") + $rank)       # 0x5EED0000 + rang
    $h = (U64 $seed) -bxor $a -bxor $b
    $h = SplitMix $h
    return [double]($h / [System.Numerics.BigInteger]::Pow(2, 11)) / 9007199254740992.0
}
function Value([double]$u) { if ($Max -ge $Min -and ($Max -ne 0 -or $Min -ne 0)) { $range = $Max - $Min + 1; return $Min + [Math]::Min([Math]::Floor($u * $range), $range - 1) } return $u }

$key = Fnv32 $Stream
$turnList = @($Turns -split "," | ForEach-Object { [int]$_ })
$seeds = if ($FindFrom -ge 0) { $FindFrom..($FindFrom + $FindCount - 1) } else { @($Seed) }
foreach ($sd in $seeds) {
    $sv = [System.Numerics.BigInteger]::Parse("$sd")
    $vals = foreach ($t in $turnList) { Value (Uniform $sv $t $key $Rank) }
    if ($Want -ge 0 -and @($vals | Where-Object { $_ -ne $Want }).Count -gt 0) { continue }
    "graine $sd : " + (($turnList | ForEach-Object -Begin { $i = 0 } -Process { "tour $_ = $($vals[$i])"; $i++ }) -join ", ")
}
