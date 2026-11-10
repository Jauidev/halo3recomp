# Halo 3 phone GPU: which draws cost the GPU its time, found by leaving them out.
#
# One Arm Streamline capture (Mali counters) while the player stands still; during it this script
# switches groups of draws off and on again through the renderer's live control file
# (xnative_skip.txt, read by the game once a second: "vs <hash>", "ps <hash>", "cvar name=value"),
# one group per phase. Afterwards each phase's counters are read from the capture and divided by
# the phase's fps (the game's VblankThread log lines): GPU cycles PER FRAME, which do not depend
# on the GPU clock, so heat throttling during the run does not falsify the comparison. The group
# whose removal takes the most cycles off the frame is the one that costs them.
#
# The capture needs the debuggable APK. Streamline starts the game itself, so THE GAME MUST BE
# CLOSED when this starts; then there are -Preparacion seconds to load the save, reach the place
# and stand still. The first step blanks the picture for a few seconds (every draw off): that dip
# in the counters is how the phases are lined up with the capture's clock.
#
# What is switched off:
#   -Pruebas "vs 1443BC09285E7C08","ps 4EDC7354FD8F3628;ps 0B3B12AFAA757C3C"   one phase each
#            (";" separates lines). A cvar does not go back by itself: "cvar a=1 => cvar a=0"
#            sets the left side for the phase and the right side after it.
#   without -Pruebas: from the newest census (tools\halo3-gpu-censo.ps1, logs\gpu-censo\ultimo):
#            the depth-only vertex shaders together, the computed-fetch ones together, and the
#            -Top vertex shaders with the most vertex work one by one.
#
#   powershell -ExecutionPolicy Bypass -File tools\halo3-gpu-quitar.ps1
#   ... -Segundos 20 -Top 6 -Preparacion 150
#   ... -Analizar <carpeta de una sesion>     only redo the analysis of a finished session
#   ... -QuitarCapas                          switch off Vulkan debug layers left on for the game
param(
    [string[]]$Pruebas = @(),
    [int]$Top = 6,
    [int]$Segundos = 20,
    [int]$Margen = 5,
    [int]$Preparacion = 150,
    [string]$Censo = (Join-Path $PSScriptRoot "..\work\logs\gpu-censo\ultimo\censo_vs.csv"),
    [string]$Analizar = "",
    [switch]$QuitarCapas,
    [string]$Plantilla = "Mali-G615 - Valhall",
    [string]$Device = $env:ANDROID_SERIAL,
    [string]$Package = "com.xport.halo3"
)
# Continue: Windows PowerShell turns adb's stderr lines into errors; failures are checked
# explicitly.
$ErrorActionPreference = "Continue"
$inv = [Globalization.CultureInfo]::InvariantCulture
$adbDir = (Join-Path $env:LOCALAPPDATA "Android\Sdk\platform-tools")
if (Test-Path $adbDir) { $env:PATH = "$adbDir;$env:PATH" }
$StreamlineCli = "C:\Program Files\Arm\Arm Performance Studio 2026.3\streamline\Streamline-cli.exe"
$Remote = "/sdcard/Android/data/$Package/files"
$Root = (Join-Path $PSScriptRoot "..\work\logs\gpu-quitar")
$BlipSeconds = 6

function Fail([string]$text) { Write-Output ("ERROR: " + $text); exit 1 }
function Now-Epoch { return [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() / 1000.0 }
function N([double]$value, [string]$format = "0.0") {
    if ([double]::IsNaN($value) -or [double]::IsInfinity($value)) { return "n/d" }
    return $value.ToString($format, $inv)
}

# ---------------------------------------------------------------------------------------------
# The capture's timeline (same reader as tools\halo3-gpu-resumen.ps1).
$fieldPattern = New-Object System.Text.RegularExpressions.Regex('(?:"([^"]*)"|([^,]*))(?:,|$)')
function Split-Line([string]$line) {
    $fields = New-Object System.Collections.Generic.List[object]
    $position = 0
    while ($position -le $line.Length) {
        $m = $fieldPattern.Match($line, $position)
        if (-not $m.Success) { break }
        if ($m.Groups[1].Success) { $fields.Add(@($m.Groups[1].Value, $true)) }
        else { $fields.Add(@($m.Groups[2].Value, $false)) }
        if ($m.Length -eq 0) { break }
        $position = $m.Index + $m.Length
        if ($position -eq $line.Length -and -not $line.EndsWith(",")) { break }
    }
    return ,$fields
}
function Parse-Number($field) {
    $text = ([string]$field[0]).Trim()
    if (-not $text) { return [double]::NaN }
    if ($field[1] -or $text.Contains(",")) { $text = $text.Replace(".", "").Replace(",", ".").Replace(" ", "") }
    $value = 0.0
    if ([double]::TryParse($text, [Globalization.NumberStyles]::Float, $inv, [ref]$value)) { return $value }
    return [double]::NaN
}
function Read-Timeline([string]$csv) {
    $lines = [IO.File]::ReadAllLines($csv)
    $header = -1
    for ($i = 0; $i -lt [Math]::Min($lines.Length, 40); $i++) {
        if ($lines[$i].StartsWith("Index (s)") -or $lines[$i].StartsWith('"Index (s)"')) { $header = $i; break }
    }
    if ($header -lt 0) { throw "no encuentro la cabecera 'Index (s)' en $csv" }
    $names = @((Split-Line $lines[$header]) | ForEach-Object { ([string]$_[0]).Trim() })
    $rows = New-Object System.Collections.Generic.List[double[]]
    for ($i = $header + 1; $i -lt $lines.Length; $i++) {
        if (-not $lines[$i]) { continue }
        $fields = Split-Line $lines[$i]
        if ($fields.Count -lt 2) { continue }
        $row = New-Object double[] $names.Count
        for ($c = 0; $c -lt $names.Count; $c++) {
            if ($c -lt $fields.Count) { $row[$c] = Parse-Number $fields[$c] } else { $row[$c] = [double]::NaN }
        }
        if ([double]::IsNaN($row[0])) { continue }
        $rows.Add($row)
    }
    if ($rows.Count -eq 0) { throw "sin filas de datos en $csv" }
    return [pscustomobject]@{ Names = $names; Rows = $rows }
}
function Column-Index($timeline, [string]$name) {
    for ($c = 0; $c -lt $timeline.Names.Count; $c++) { if ($timeline.Names[$c] -eq $name) { return $c } }
    return -1
}
# Mean of a column over capture seconds [from, to); NaN when the column or the rows are missing.
function Mean-Between($timeline, [string]$name, [double]$from, [double]$to) {
    $c = Column-Index $timeline $name
    if ($c -lt 0) { return [double]::NaN }
    $sum = 0.0; $n = 0
    foreach ($row in $timeline.Rows) {
        if ($row[0] -ge $from -and $row[0] -lt $to -and -not [double]::IsNaN($row[$c])) { $sum += $row[$c]; $n++ }
    }
    if ($n -eq 0) { return [double]::NaN }
    return $sum / $n
}
function Median([double[]]$values) {
    $sorted = @($values | Where-Object { -not [double]::IsNaN($_) } | Sort-Object)
    if ($sorted.Count -eq 0) { return [double]::NaN }
    return $sorted[[int][Math]::Floor(($sorted.Count - 1) / 2)]
}

# ---------------------------------------------------------------------------------------------
# The analysis of a finished session folder: fases.csv + sync.txt + the capture.
function Analyze-Session([string]$session) {
    $phasesPath = Join-Path $session "fases.csv"
    $syncPath = Join-Path $session "sync.txt"
    if (-not (Test-Path -LiteralPath $phasesPath)) { Fail "falta $phasesPath" }
    $sync = @{}
    if (Test-Path -LiteralPath $syncPath) {
        foreach ($line in [IO.File]::ReadAllLines($syncPath)) {
            $parts = $line.Split("=", 2)
            if ($parts.Count -eq 2) { $sync[$parts[0].Trim()] = $parts[1].Trim() }
        }
    }
    $phases = New-Object System.Collections.ArrayList
    $lines = [IO.File]::ReadAllLines($phasesPath)
    for ($i = 1; $i -lt $lines.Length; $i++) {
        $f = $lines[$i].Split("`t")
        if ($f.Count -lt 5) { continue }
        [void]$phases.Add([pscustomobject]@{
            Name = $f[0]; Kind = $f[1]
            Start = [double]::Parse($f[2], $inv); End = [double]::Parse($f[3], $inv)
            Fps = $(if ($f[4]) { [double]::Parse($f[4], $inv) } else { [double]::NaN })
        })
    }
    if ($phases.Count -eq 0) { Fail "fases.csv no tiene fases" }

    $apc = Join-Path $session "captura.apc"
    if (-not (Test-Path -LiteralPath (Join-Path $apc "captured.xml"))) { Fail "no hay captura de Streamline en $apc" }
    $reportFolder = Join-Path $session "informe"
    $csv = Join-Path $reportFolder "timeline.csv"
    if (-not (Test-Path -LiteralPath $csv)) {
        if (-not (Test-Path -LiteralPath $StreamlineCli)) { Fail "falta Streamline-cli: $StreamlineCli" }
        New-Item -ItemType Directory -Force $reportFolder | Out-Null
        Write-Output "Generando el informe de la captura (tarda un rato)..."
        & $StreamlineCli -report -timeline -template $Plantilla -scale 1 -format csv -o $reportFolder $apc 2>&1 |
            Out-File -FilePath (Join-Path $reportFolder "informe.log") -Encoding utf8
        if (-not (Test-Path -LiteralPath $csv)) {
            $found = Get-ChildItem -LiteralPath $reportFolder -Recurse -Filter "timeline.csv" | Select-Object -First 1
            if (-not $found) { Fail "Streamline no escribio timeline.csv (mira $reportFolder\informe.log)" }
            $csv = $found.FullName
        }
    }
    $timeline = Read-Timeline $csv

    # The capture's clock. First guess: the capture's creation time (PC or phone clock); then the
    # dip the "every draw off" step left in the input primitives fixes it to the second.
    $created = [double]::NaN
    $capturedXml = [IO.File]::ReadAllText((Join-Path $apc "captured.xml"))
    $m = [regex]::Match($capturedXml, 'created="(\d+)"')
    if ($m.Success) { $created = [double]::Parse($m.Groups[1].Value, $inv) }
    $deviceOffset = 0.0
    if ($sync.ContainsKey("device_minus_pc")) { $deviceOffset = [double]::Parse($sync["device_minus_pc"], $inv) }
    $origin = [double]::NaN; $originNote = ""
    $primitives = "Mali Geometry Usage:Input primitives"
    if ($sync.ContainsKey("blip_start") -and $sync.ContainsKey("blip_end") -and (Column-Index $timeline $primitives) -ge 0 -and -not [double]::IsNaN($created)) {
        $blipStart = [double]::Parse($sync["blip_start"], $inv); $blipEnd = [double]::Parse($sync["blip_end"], $inv)
        $best = [double]::MaxValue; $bestOrigin = [double]::NaN
        foreach ($guess in @($created, ($created - $deviceOffset))) {
            for ($shift = -30; $shift -le 30; $shift++) {
                $candidate = $guess + $shift
                $inside = Mean-Between $timeline $primitives ($blipStart - $candidate + 2) ($blipEnd - $candidate)
                $before = Mean-Between $timeline $primitives ($blipStart - $candidate - 8) ($blipStart - $candidate - 1)
                $after = Mean-Between $timeline $primitives ($blipEnd - $candidate + 3) ($blipEnd - $candidate + 9)
                if ([double]::IsNaN($inside) -or [double]::IsNaN($before) -or [double]::IsNaN($after) -or $before -le 0 -or $after -le 0) { continue }
                $ratio = $inside / [Math]::Min($before, $after)
                if ($ratio -lt $best) { $best = $ratio; $bestOrigin = $candidate }
            }
        }
        if ($best -lt 0.4) {
            $origin = $bestOrigin
            $originNote = ("alineado con el apagon de sincronizacion (la geometria cae al {0} % en el)" -f (N (100 * $best) "0"))
        }
    }
    if ([double]::IsNaN($origin)) {
        if ([double]::IsNaN($created)) { Fail "no se puede situar la captura en el tiempo (sin apagon ni fecha de creacion)" }
        $origin = $created
        $originNote = "AVISO: no se ve el apagon de sincronizacion; se usa la hora de creacion de la captura (puede haber varios segundos de error: desconfia de las diferencias pequenas)"
    }

    $out = New-Object System.Collections.ArrayList
    function Say([string]$text) { [void]$out.Add($text); Write-Output $text }
    Say ("Sesion: {0}" -f $session)
    Say ("Reloj de la captura: {0}" -f $originNote)
    Say ""
    $results = New-Object System.Collections.ArrayList
    foreach ($phase in $phases) {
        $from = $phase.Start - $origin + $Margen
        $to = $phase.End - $origin - 1
        $gpu = Mean-Between $timeline "Mali GPU Usage:GPU active" $from $to
        $vq = Mean-Between $timeline "Mali GPU Usage:Vertex queue active" $from $to
        $fq = Mean-Between $timeline "Mali GPU Usage:Fragment queue active" $from $to
        $in = Mean-Between $timeline $primitives $from $to
        $pos = Mean-Between $timeline "Mali Geometry Threads:Position shading threads" $from $to
        $var = Mean-Between $timeline "Mali Geometry Threads:Varying shading threads" $from $to
        $px = Mean-Between $timeline "Mali Pixels:Pixels" $from $to
        $fps = $phase.Fps
        $per = 1.0
        if (-not [double]::IsNaN($fps) -and $fps -gt 0) { $per = $fps }
        [void]$results.Add([pscustomobject]@{
            Name = $phase.Name; Kind = $phase.Kind; Fps = $fps
            Gpu = $gpu / $per / 1e6; Vq = $vq / $per / 1e6; Fq = $fq / $per / 1e6
            Tris = $in / $per / 1e3; Verts = ($pos + $var) / $per / 1e3; Pixels = $px / $per / 1e6
            Mhz = $gpu / 1e6
        })
    }
    $withoutFps = @($results | Where-Object { [double]::IsNaN($_.Fps) -or $_.Fps -le 0 }).Count
    $unit = "por fotograma"
    if ($withoutFps -gt 0) {
        $unit = "POR SEGUNDO"
        Say "AVISO: hay fases sin lecturas de fps (lineas VblankThread): las cifras van POR SEGUNDO en todas."
        Say "       Con la GPU saturada eso esconde el ahorro (al quitar trabajo suben los fps); fiate solo de los fps."
        foreach ($r in $results) {
            if (-not [double]::IsNaN($r.Fps) -and $r.Fps -gt 0) {
                $r.Gpu *= $r.Fps; $r.Vq *= $r.Fps; $r.Fq *= $r.Fps; $r.Tris *= $r.Fps; $r.Verts *= $r.Fps; $r.Pixels *= $r.Fps
            }
        }
    }
    $bases = @($results | Where-Object { $_.Kind -eq "base" -and -not [double]::IsNaN($_.Gpu) })
    if ($bases.Count -eq 0) { Fail "ninguna fase base cae dentro de la captura (la captura termino antes de las fases?)" }
    function Average($items, [string]$property) {
        $sum = 0.0; $n = 0
        foreach ($item in $items) { if (-not [double]::IsNaN($item.$property)) { $sum += $item.$property; $n++ } }
        if ($n -eq 0) { return [double]::NaN }
        return $sum / $n
    }
    $base = [pscustomobject]@{
        Gpu = (Average $bases "Gpu"); Vq = (Average $bases "Vq"); Fq = (Average $bases "Fq")
        Tris = (Average $bases "Tris"); Verts = (Average $bases "Verts"); Fps = (Average $bases "Fps")
    }
    $noise = [double]::NaN
    if ($bases.Count -ge 2 -and $base.Gpu -gt 0) {
        $values = @($bases | ForEach-Object { $_.Gpu })
        $noise = 100 * ((($values | Measure-Object -Maximum).Maximum) - (($values | Measure-Object -Minimum).Minimum)) / $base.Gpu
    }
    Say ("Cifras {0}. Mcic = millones de ciclos de GPU. Ahorro = lo que baja respecto a la base al quitar ese grupo." -f $unit)
    Say ("  {0,-44} {1,5} {2,8} {3,8} {4,8} {5,8} {6,8} {7,8} {8,8}" -f "fase", "fps", "GPU Mcic", "vert Mcic", "frag Mcic", "tri mil", "vert mil", "ahorro", "ahorro v")
    foreach ($r in $results) {
        $saving = ""; $savingV = ""
        if ($r.Kind -ne "base" -and $base.Gpu -gt 0 -and -not [double]::IsNaN($r.Gpu)) {
            $saving = (N (100 * ($base.Gpu - $r.Gpu) / $base.Gpu) "0.0") + " %"
            if ($base.Vq -gt 0 -and -not [double]::IsNaN($r.Vq)) { $savingV = (N (100 * ($base.Vq - $r.Vq) / $base.Vq) "0.0") + " %" }
        }
        $name = $r.Name
        if ($name.Length -gt 44) { $name = $name.Substring(0, 41) + "..." }
        Say ("  {0,-44} {1,5} {2,8} {3,8} {4,8} {5,8} {6,8} {7,8} {8,8}" -f $name, (N $r.Fps), (N $r.Gpu), (N $r.Vq), (N $r.Fq), (N $r.Tris "0"), (N $r.Verts "0"), $saving, $savingV)
    }
    Say ""
    if (-not [double]::IsNaN($noise)) {
        Say ("Ruido: las fases base difieren un {0} % entre si; un ahorro menor que eso no significa nada." -f (N $noise))
    }
    $tests = @($results | Where-Object { $_.Kind -ne "base" -and -not [double]::IsNaN($_.Gpu) } | Sort-Object { $base.Gpu - $_.Gpu } -Descending)
    if ($tests.Count -gt 0 -and $base.Gpu -gt 0) {
        Say "DE MAS A MENOS COSTE:"
        foreach ($r in $tests) {
            Say ("  {0,6} % del fotograma de GPU ({1} % de la cola de vertices, {2} miles de triangulos menos, fps {3} -> {4})  {5}" -f `
                (N (100 * ($base.Gpu - $r.Gpu) / $base.Gpu)), (N (100 * ($base.Vq - $r.Vq) / [Math]::Max(0.000001, $base.Vq))),
                (N ($base.Tris - $r.Tris) "0"), (N $base.Fps), (N $r.Fps), $r.Name)
        }
    }
    [IO.File]::WriteAllLines((Join-Path $session "resultado.txt"), [string[]]$out)
    Write-Output ""
    Write-Output ("Guardado en: {0}" -f (Join-Path $session "resultado.txt"))
}

if ($Analizar) {
    if (-not (Test-Path -LiteralPath $Analizar)) { Fail "no existe $Analizar" }
    Analyze-Session (Resolve-Path -LiteralPath $Analizar).Path
    exit 0
}

# ---------------------------------------------------------------------------------------------
# The tests: {Name, Lines (during the phase), Restore (after it, may be empty)}.
$tests = New-Object System.Collections.ArrayList
function Add-Test([string]$name, [string[]]$lines, [string[]]$restore) {
    [void]$tests.Add([pscustomobject]@{ Name = $name; Lines = $lines; Restore = $restore })
}
if ($Pruebas.Count -gt 0) {
    foreach ($text in $Pruebas) {
        $sides = $text -split "=>", 2
        $lines = @($sides[0].Split(";") | ForEach-Object { $_.Trim() } | Where-Object { $_ })
        $restore = @()
        if ($sides.Count -eq 2) { $restore = @($sides[1].Split(";") | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
        if ($lines.Count -eq 0) { continue }
        if (@($lines | Where-Object { $_ -like "cvar *" }).Count -gt 0 -and $restore.Count -eq 0) {
            Fail ("la prueba '{0}' cambia un cvar y no dice como dejarlo despues: usa 'cvar a=1 => cvar a=0'" -f $text)
        }
        Add-Test ("sin: " + $sides[0].Trim()) $lines $restore
    }
} else {
    if (-not (Test-Path -LiteralPath $Censo)) {
        Fail "no hay pruebas (-Pruebas) ni censo en $Censo. Lanza antes tools\halo3-gpu-censo.ps1 con el juego en la zona."
    }
    $census = @(Import-Csv -LiteralPath $Censo)
    $depth = @($census | Where-Object { $_.only_depth -eq "1" } | ForEach-Object { $_.vs })
    $fetch = @($census | Where-Object { $_.computed_fetch -eq "1" } | ForEach-Object { $_.vs })
    if ($depth.Count -gt 0) { Add-Test ("sin los {0} VS de solo profundidad (sombras, prepase)" -f $depth.Count) @($depth | ForEach-Object { "vs $_" }) @() }
    if ($fetch.Count -gt 0) { Add-Test ("sin los {0} VS computed-fetch (agua, particulas)" -f $fetch.Count) @($fetch | ForEach-Object { "vs $_" }) @() }
    foreach ($row in ($census | Select-Object -First $Top)) {
        Add-Test ("sin VS {0} ({1} % de los vertices; {2})" -f $row.vs, $row.share, $row.kind) @("vs $($row.vs)") @()
    }
}
if ($tests.Count -eq 0) { Fail "ninguna prueba que hacer" }

# ---------------------------------------------------------------------------------------------
# Checks before anything starts.
if (-not (Test-Path -LiteralPath $StreamlineCli)) { Fail "falta Streamline-cli: $StreamlineCli" }
$state = (& adb -s $Device get-state 2>$null | Out-String).Trim()
if ($state -ne "device") { Fail "el movil $Device no esta conectado por adb" }
if ((& adb -s $Device shell pidof $Package 2>$null | Out-String).Trim()) {
    Fail "EL JUEGO ESTA ABIERTO: cierralo del todo. Streamline lo abre el mismo; con el juego ya abierto se queda la pantalla en negro."
}
$debuggable = (& adb -s $Device shell "run-as $Package id 2>&1" | Out-String)
if ($debuggable -match "not debuggable|unknown package") {
    Fail "el APK instalado no es depurable: Streamline no puede leer los contadores. Instala el APK '-depurable-'."
}

# Vulkan debug layers left on for the game (Arm Frame Advisor sets them to record the frame and
# does not always take them away): the game would run with a recording layer inside, slower, and
# the capture would measure that. -QuitarCapas switches them off (settings delete global ...).
$layersOn = (& adb -s $Device shell "settings get global enable_gpu_debug_layers" 2>$null | Out-String).Trim()
$layersApp = (& adb -s $Device shell "settings get global gpu_debug_app" 2>$null | Out-String).Trim()
if ($layersOn -eq "1" -and $layersApp -eq $Package) {
    $layers = (& adb -s $Device shell "settings get global gpu_debug_layers" 2>$null | Out-String).Trim()
    # Streamline's own layer, left behind by a capture that was cut short: Streamline sets it
    # again when it starts, so it is simply cleared here.
    $ownLayer = ($layers -eq "VK_LAYER_LGL_gpu_timeline")
    if ($QuitarCapas -or $ownLayer) {
        foreach ($key in @("enable_gpu_debug_layers", "gpu_debug_app", "gpu_debug_layers", "gpu_debug_layer_app")) {
            & adb -s $Device shell "settings delete global $key" 2>&1 | Out-Null
        }
        if (-not $ownLayer) { Write-Output ("Capas de depuracion de Vulkan quitadas del juego ({0})." -f $layers) }
    } else {
        Fail ("el movil tiene capas de depuracion de Vulkan activadas para el juego ({0}): el juego va mas lento con ellas y la medida no vale. Repite con -QuitarCapas para desactivarlas." -f $layers)
    }
}

$Session = Join-Path $Root (Get-Date -Format "yyyyMMdd_HHmmss")
New-Item -ItemType Directory -Force $Session | Out-Null
$phaseCount = $tests.Count + 2
$restoreCount = @($tests | Where-Object { $_.Restore.Count -gt 0 }).Count
$needed = $Preparacion + 45 + $BlipSeconds + 10 + $phaseCount * $Segundos + $restoreCount * 3 + 25
Write-Output ("Sesion: {0}" -f $Session)
Write-Output ("{0} pruebas + 2 bases de {1} s; captura de {2} s en total." -f $tests.Count, $Segundos, $needed)

function Set-Control([string[]]$lines) {
    if ($lines.Count -eq 0) {
        & adb -s $Device shell "rm -f $Remote/xnative_skip.txt" 2>&1 | Out-Null
        return
    }
    $local = Join-Path $Session "xnative_skip.txt"
    [IO.File]::WriteAllText($local, (($lines -join "`n") + "`n"))
    & adb -s $Device push $local "$Remote/xnative_skip.txt" 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "adb push del fichero de control ha fallado" }
}
& adb -s $Device shell "rm -f $Remote/xnative_skip.txt $Remote/xnative_drawn.txt" 2>&1 | Out-Null

# The phone's clock against this PC's (the capture may be stamped with either).
$pcBefore = Now-Epoch
$deviceText = (& adb -s $Device shell "date +%s.%N" 2>$null | Out-String).Trim()
$pcAfter = Now-Epoch
$deviceEpoch = 0.0
$deviceMinusPc = 0.0
if ([double]::TryParse($deviceText, [Globalization.NumberStyles]::Float, $inv, [ref]$deviceEpoch)) {
    $deviceMinusPc = $deviceEpoch - ($pcBefore + $pcAfter) / 2
}

# ---- The capture, in the background.
$sessionXml = Join-Path $Session "session.xml"
[IO.File]::WriteAllText($sessionXml, "<?xml version=`"1.0`" encoding=`"UTF-8`"?>`n<session version=`"1`" call_stack_unwinding=`"no`" buffer_mode=`"streaming`" sample_rate=`"normal`" duration=`"0`" live_rate=`"100`" target_address=`"adb:$Device`">`n</session>`n")
$apc = Join-Path $Session "captura.apc"
$emptyInput = Join-Path $Session "vacio.txt"
[IO.File]::WriteAllText($emptyInput, "")
$arguments = "-capture `"$sessionXml`" -target-address adb:$Device -package $Package -template `"$Plantilla`" -duration $needed -clean-gatord -o `"$apc`""
$captureStarted = Now-Epoch
$capture = Start-Process -FilePath $StreamlineCli -ArgumentList $arguments -PassThru -WindowStyle Hidden `
    -RedirectStandardInput $emptyInput -RedirectStandardOutput (Join-Path $Session "streamline.log") `
    -RedirectStandardError (Join-Path $Session "streamline_err.log")
function Stop-Capture([string]$why) {
    Set-Control @()
    if ($capture -and -not $capture.HasExited) { try { Stop-Process -Id $capture.Id -Force -ErrorAction Stop } catch {} }
    Fail $why
}

Write-Output "Streamline arrancando el juego..."
$gamePid = ""
for ($i = 0; $i -lt 90 -and -not $gamePid; $i++) {
    Start-Sleep -Seconds 1
    if ($capture.HasExited) {
        $tail = ""
        if (Test-Path (Join-Path $Session "streamline.log")) { $tail = (Get-Content (Join-Path $Session "streamline.log") -Tail 8) -join " | " }
        Fail ("Streamline ha terminado antes de empezar: {0}" -f $tail)
    }
    $gamePid = (& adb -s $Device shell pidof $Package 2>$null | Out-String).Trim()
}
if (-not $gamePid) { Stop-Capture "Streamline no ha abierto el juego en 90 s" }
Write-Output ("Juego abierto (pid {0}). TIENES {1} s: carga la partida, ve a la zona y QUEDATE QUIETO." -f $gamePid, $Preparacion)
$prepared = (Get-Date).AddSeconds($Preparacion)
while ((Get-Date) -lt $prepared) {
    $left = [int]($prepared - (Get-Date)).TotalSeconds
    if ($left -gt 0 -and ($left % 30) -eq 0) { Write-Output ("  quedan {0} s" -f $left) }
    Start-Sleep -Seconds 1
    if ($capture.HasExited) { Fail "Streamline ha terminado durante la preparacion (mira streamline.log en la sesion)" }
}
$gamePid = (& adb -s $Device shell pidof $Package 2>$null | Out-String).Trim()
if (-not $gamePid) { Stop-Capture "el juego se ha cerrado durante la preparacion" }

# ---- The shaders on screen now (also proves the game is drawing a level).
$request = [int]([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() % 1000000)
Set-Control @("dump $request")
$drawn = $null
for ($i = 0; $i -lt 30 -and -not $drawn; $i++) {
    Start-Sleep -Milliseconds 700
    $text = @(& adb -s $Device shell "cat $Remote/xnative_drawn.txt 2>/dev/null")
    if ($text.Count -gt 0 -and ($text | Select-Object -Last 1).Trim() -eq "end $request") { $drawn = $text }
}
if (-not $drawn) { Stop-Capture "el juego no responde al fichero de control (no ha escrito xnative_drawn.txt): no esta dibujando una partida?" }
[IO.File]::WriteAllLines((Join-Path $Session "drawn.txt"), [string[]]$drawn)
$drawnVs = @($drawn | Where-Object { $_ -like "vs *" } | ForEach-Object { ($_.Trim() -split "\s+")[1] })
if ($drawnVs.Count -eq 0) { Stop-Capture "el fotograma no tiene ningun vertex shader: no esta en una partida" }
Write-Output ("En pantalla: {0} vertex shaders. Empieza la medida: NO TOQUES NADA." -f $drawnVs.Count)

# ---- Sync: every draw off for a few seconds.
Set-Control @()
Start-Sleep -Seconds 4
$blipStart = Now-Epoch
Set-Control @($drawnVs | ForEach-Object { "vs $_" })
Start-Sleep -Seconds $BlipSeconds
$blipEnd = Now-Epoch
Set-Control @()
[IO.File]::WriteAllLines((Join-Path $Session "sync.txt"), [string[]]@(
    ("blip_start=" + $blipStart.ToString("0.000", $inv)), ("blip_end=" + $blipEnd.ToString("0.000", $inv)),
    ("device_minus_pc=" + $deviceMinusPc.ToString("0.000", $inv)), ("capture_started=" + $captureStarted.ToString("0.000", $inv))))
Start-Sleep -Seconds 10

# ---- The phases.
$phaseLines = New-Object System.Collections.ArrayList
[void]$phaseLines.Add("nombre`ttipo`tinicio`tfin`tfps")
function Run-Phase([string]$name, [string]$kind, [string[]]$lines) {
    Set-Control $lines
    $start = Now-Epoch
    Start-Sleep -Seconds $Margen
    # The phone's own clock for logcat -T.
    $stamp = (& adb -s $Device shell "date '+%m-%d %H:%M:%S.000'" 2>$null | Out-String).Trim()
    Start-Sleep -Seconds ([Math]::Max(1, $Segundos - $Margen))
    $end = Now-Epoch
    $fps = @()
    if ($stamp) {
        $fps = @(& adb -s $Device logcat -d --pid=$gamePid -T $stamp 2>$null |
                 Select-String -Pattern "VblankThread: \d+ swaps \(([\d.]+) fps\)" |
                 ForEach-Object { [double]::Parse($_.Matches[0].Groups[1].Value, $inv) })
    }
    $fpsText = ""
    if ($fps.Count -gt 0) { $fpsText = (($fps | Measure-Object -Average).Average).ToString("0.00", $inv) }
    [void]$phaseLines.Add(("{0}`t{1}`t{2}`t{3}`t{4}" -f $name, $kind, $start.ToString("0.000", $inv), $end.ToString("0.000", $inv), $fpsText))
    [IO.File]::WriteAllLines((Join-Path $Session "fases.csv"), [string[]]$phaseLines)
    Write-Output ("  {0,-60} {1} fps" -f $name, $(if ($fpsText) { $fpsText } else { "n/d" }))
}
try {
    Run-Phase "base (nada apagado) 1" "base" @()
    foreach ($test in $tests) {
        if ($capture.HasExited) { throw "la captura ha terminado antes que las pruebas" }
        Run-Phase $test.Name "prueba" $test.Lines
        if ($test.Restore.Count -gt 0) {
            Set-Control $test.Restore
            Start-Sleep -Seconds 3
        }
    }
    Run-Phase "base (nada apagado) 2" "base" @()
} catch {
    Set-Control @()
    Write-Output ("AVISO: {0}" -f $_.Exception.Message)
} finally {
    Set-Control @()
}

Write-Output "Pruebas terminadas; puedes moverte. Esperando a que Streamline cierre la captura..."
$waitUntil = (Get-Date).AddSeconds($needed + 180)
while (-not $capture.HasExited -and (Get-Date) -lt $waitUntil) { Start-Sleep -Seconds 2 }
if (-not $capture.HasExited) {
    Write-Output "AVISO: Streamline no termina; se analiza lo que haya."
}
Start-Sleep -Seconds 2
Analyze-Session $Session
