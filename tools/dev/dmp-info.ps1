# Résumé d'un minidump sans débogueur : code d'exception, adresse, module fautif (+offset),
# et les adresses de la pile du thread fautif qui tombent dans un module connu (pile brute, heuristique).
#   dmp-info.ps1 <fichier.dmp> [-StackWords 4096]
param([Parameter(Mandatory = $true, Position = 0)][string]$Path, [int]$StackWords = 4096)
$ErrorActionPreference = "Stop"
$b = [System.IO.File]::ReadAllBytes((Resolve-Path $Path))
function U32($o) { [BitConverter]::ToUInt32($b, $o) }
function U64($o) { [BitConverter]::ToUInt64($b, $o) }
if ((U32 0) -ne 0x504D444D) { throw "pas un minidump" }
$nStreams = U32 8; $dir = U32 12
$streams = @{}
for ($i = 0; $i -lt $nStreams; $i++) { $e = $dir + 12 * $i; $streams[[int](U32 $e)] = @{ size = (U32 ($e + 4)); rva = (U32 ($e + 8)) } }

function MinidumpString($rva) { $len = U32 $rva; [Text.Encoding]::Unicode.GetString($b, $rva + 4, $len) }

# Modules (stream 4)
$mods = @()
if ($streams.ContainsKey(4)) {
    $r = $streams[4].rva; $n = U32 $r
    for ($i = 0; $i -lt $n; $i++) {
        $m = $r + 4 + 108 * $i
        $mods += [pscustomobject]@{ Base = (U64 $m); Size = (U32 ($m + 8)); Name = [IO.Path]::GetFileName((MinidumpString (U32 ($m + 20)))) }
    }
}
function Where-Addr([uint64]$a) {
    foreach ($m in $mods) { if ($a -ge $m.Base -and $a -lt ($m.Base + $m.Size)) { return "{0}+0x{1:X}" -f $m.Name, ($a - $m.Base) } }
    return $null
}

# Exception (stream 6)
if (-not $streams.ContainsKey(6)) { "aucun flux d'exception"; return }
$x = $streams[6].rva
$tid = U32 $x
$code = U32 ($x + 8); $addr = U64 ($x + 24)
$nParams = U32 ($x + 32)
$params = @(); for ($i = 0; $i -lt [Math]::Min($nParams, 15); $i++) { $params += ("0x{0:X}" -f (U64 ($x + 40 + 8 * $i))) }
"exception : 0x{0:X8} à 0x{1:X} ({2})" -f $code, $addr, (Where-Addr $addr)
if ($params) { "paramètres : " + ($params -join ", ") }
$mainTid = if ($streams.ContainsKey(3)) { U32 ($streams[3].rva + 4) } else { 0 }
"thread : $tid$(if ($tid -eq $mainTid) { ' (thread principal du jeu)' } else { " (pas le thread principal $mainTid)" })"

# Pile du thread fautif : retrouver sa mémoire via la liste des threads (stream 3) et le RSP du contexte
$ctxRva = U32 ($x + 4 + 4 + 4 + 4 + 8 + 8 + 4 + 4 + 15 * 8 + 4)   # MINIDUMP_EXCEPTION_STREAM.ThreadContext.Rva
$rsp = U64 ($ctxRva + 0x98); $rip = U64 ($ctxRva + 0xF8)
"rip : 0x{0:X} ({1})" -f $rip, (Where-Addr $rip)
if ($streams.ContainsKey(3)) {
    $r = $streams[3].rva; $n = U32 $r
    for ($i = 0; $i -lt $n; $i++) {
        $t = $r + 4 + 48 * $i
        if ((U32 $t) -ne $tid) { continue }
        $stStart = U64 ($t + 24); $stSize = U32 ($t + 32); $stRva = U32 ($t + 36)
        $off = [int64]$rsp - [int64]$stStart
        if ($off -lt 0 -or $off -ge $stSize) { $off = 0 }
        "pile (adresses de retour probables) :"
        $count = 0
        for ($o = $off; $o + 8 -le $stSize -and $stRva + $o + 8 -le $b.Length -and $count -lt $StackWords; $o += 8) {
            $v = U64 ($stRva + $o); $w = Where-Addr $v
            if ($w) { "  rsp+0x{0:X} {1}" -f ($o - $off), $w }
            $count++
        }
    }
}
