# Halo 3 phone GPU: what an Arm Streamline capture (Mali counters) says, in one table.
#
# Input: a capture (.apc folder), a report folder with timeline.csv, or a timeline.csv. A capture
# without a report gets one made here (Streamline-cli -report -timeline, 1 bin per second, into
# "informe_<capture name>" next to the capture).
#
# The seconds read: -Desde/-Hasta, or (default) the seconds with real geometry: those whose input
# primitives reach half of the capture's busy level (loading screens and menus are left out).
#
# Every counter is looked up by its column NAME; a column the capture does not have prints "n/d".
# Ratios are computed from the sums over the window, not as a mean of per-second ratios.
#
#   powershell -ExecutionPolicy Bypass -File tools\halo3-gpu-resumen.ps1 -Captura <apc|carpeta|csv>
#   ... -Desde 119 -Hasta 419        only those seconds
#   ... -Fps 17.2                    adds the per-frame figures (the capture has no fps of its own)
#   ... -Comparar <otra> [-DesdeB .. -HastaB .. -FpsB ..]   second capture, with the change in %
#   ... -Todo                        also the mean of every counter of the window
param(
    [Parameter(Mandatory = $true)][string]$Captura,
    [double]$Desde = -1,
    [double]$Hasta = -1,
    [double]$Fps = 0,
    [string]$Comparar = "",
    [double]$DesdeB = -1,
    [double]$HastaB = -1,
    [double]$FpsB = 0,
    [string]$Plantilla = "Mali-G615 - Valhall",
    [switch]$Todo
)
$ErrorActionPreference = "Stop"
$inv = [Globalization.CultureInfo]::InvariantCulture
$StreamlineCli = "C:\Program Files\Arm\Arm Performance Studio 2026.3\streamline\Streamline-cli.exe"

# The timeline.csv of a capture, a report folder or a csv path (making the report when missing).
function Resolve-Timeline([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { throw "no existe: $path" }
    $item = Get-Item -LiteralPath $path
    if (-not $item.PSIsContainer) { return $item.FullName }
    $direct = Join-Path $item.FullName "timeline.csv"
    if (Test-Path -LiteralPath $direct) { return $direct }
    if (-not (Test-Path -LiteralPath (Join-Path $item.FullName "captured.xml"))) {
        throw "ni timeline.csv ni captura de Streamline en: $path"
    }
    $name = [IO.Path]::GetFileNameWithoutExtension($item.Name)
    $report = Join-Path $item.Parent.FullName ("informe_" + $name)
    $csv = Join-Path $report "timeline.csv"
    if (Test-Path -LiteralPath $csv) { return $csv }
    if (-not (Test-Path -LiteralPath $StreamlineCli)) { throw "falta Streamline-cli: $StreamlineCli" }
    New-Item -ItemType Directory -Force $report | Out-Null
    Write-Host ("Generando el informe de {0} (tarda un rato)..." -f $item.Name)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $StreamlineCli -report -timeline -template $Plantilla -scale 1 -format csv -o $report $item.FullName 2>&1 |
        Out-File -FilePath (Join-Path $report "informe.log") -Encoding utf8
    $ErrorActionPreference = $previous
    if (-not (Test-Path -LiteralPath $csv)) {
        # Several captures at once go to a subfolder named after the capture; one may too.
        $found = Get-ChildItem -LiteralPath $report -Recurse -Filter "timeline.csv" | Select-Object -First 1
        if ($found) { return $found.FullName }
        throw "Streamline no escribio timeline.csv (mira $report\informe.log)"
    }
    return $csv
}

# One CSV line into fields; a quoted field keeps its commas. Returns {text, quoted} pairs.
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

# Streamline writes decimals in the PC's locale: quoted "1.234,5 " here. An unquoted field without
# a comma is a plain number (digits, maybe a decimal point).
function Parse-Number($field) {
    $text = ([string]$field[0]).Trim()
    if (-not $text) { return [double]::NaN }
    if ($field[1] -or $text.Contains(",")) { $text = $text.Replace(".", "").Replace(",", ".").Replace(" ", "") }
    $value = 0.0
    if ([double]::TryParse($text, [Globalization.NumberStyles]::Float, $inv, [ref]$value)) { return $value }
    return [double]::NaN
}

# {Names: column names, Rows: list of double[] (index 0 = second)}.
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
    return [pscustomobject]@{ Names = $names; Rows = $rows; Path = $csv }
}

function Column-Index($timeline, [string]$name) {
    for ($c = 0; $c -lt $timeline.Names.Count; $c++) { if ($timeline.Names[$c] -eq $name) { return $c } }
    return -1
}

# The rows of the window: explicit seconds, or the seconds with real geometry.
function Select-Window($timeline, [double]$from, [double]$to) {
    $rows = $timeline.Rows
    if ($from -ge 0 -or $to -ge 0) {
        if ($from -lt 0) { $from = 0 }
        if ($to -lt 0) { $to = [double]::MaxValue }
        $picked = @($rows | Where-Object { $_[0] -ge $from -and $_[0] -le $to })
        return [pscustomobject]@{ Rows = $picked; Note = ("segundos {0}-{1}" -f $from, $(if ($to -eq [double]::MaxValue) { "fin" } else { $to })) }
    }
    $c = Column-Index $timeline "Mali Geometry Usage:Input primitives"
    if ($c -lt 0) { return [pscustomobject]@{ Rows = @($rows); Note = "toda la captura (sin columna de primitivas)" } }
    $values = @($rows | ForEach-Object { $_[$c] } | Where-Object { -not [double]::IsNaN($_) } | Sort-Object)
    if ($values.Count -eq 0) { return [pscustomobject]@{ Rows = @($rows); Note = "toda la captura" } }
    $busy = $values[[int][Math]::Floor(($values.Count - 1) * 0.9)]
    $threshold = $busy * 0.5
    $picked = @($rows | Where-Object { $_[$c] -ge $threshold -and $_[$c] -gt 0 })
    if ($picked.Count -eq 0) { $picked = @($rows) }
    $first = $picked[0][0]; $last = $picked[$picked.Count - 1][0]
    return [pscustomobject]@{ Rows = $picked; Note = ("automatico: {0} segundos con geometria, entre el {1} y el {2}" -f $picked.Count, $first, $last) }
}

# Mean of a column over the window; NaN when the column is missing.
function Mean($timeline, $rows, [string]$name) {
    $c = Column-Index $timeline $name
    if ($c -lt 0) { return [double]::NaN }
    $sum = 0.0; $n = 0
    foreach ($row in $rows) { if (-not [double]::IsNaN($row[$c])) { $sum += $row[$c]; $n++ } }
    if ($n -eq 0) { return [double]::NaN }
    return $sum / $n
}
function Ratio([double]$a, [double]$b) {
    if ([double]::IsNaN($a) -or [double]::IsNaN($b) -or $b -eq 0) { return [double]::NaN }
    return $a / $b
}

# The figures of one window: ordered list of {Name, Value, Format, Unit}.
function Measure-Window($timeline, $rows, [double]$fps) {
    $m = @{}
    foreach ($pair in @(
        @("gpu", "Mali GPU Usage:GPU active"), @("vq", "Mali GPU Usage:Vertex queue active"),
        @("fq", "Mali GPU Usage:Fragment queue active"), @("cq", "Mali GPU Usage:Compute queue active"),
        @("vqu", "Mali GPU Utilization:Vertex queue utilization"), @("fqu", "Mali GPU Utilization:Fragment queue utilization"),
        @("cqu", "Mali GPU Utilization:Compute queue utilization"),
        @("rd", "Mali Memory Bandwidth Usage:Read bytes"), @("wr", "Mali Memory Bandwidth Usage:Write bytes"),
        @("l0", "Mali Memory Read Latency:0-127 cycles"), @("l1", "Mali Memory Read Latency:128-191 cycles"),
        @("l2", "Mali Memory Read Latency:192-255 cycles"), @("l3", "Mali Memory Read Latency:256-319 cycles"),
        @("l4", "Mali Memory Read Latency:320-383 cycles"), @("l5", "Mali Memory Read Latency:384+ cycles"),
        @("in", "Mali Geometry Usage:Input primitives"), @("vis", "Mali Geometry Usage:Visible primitives"),
        @("frus", "Mali Geometry Culling Rate:Frustum culled primitive rate"),
        @("face", "Mali Geometry Culling Rate:Facing culled primitive rate"),
        @("samp", "Mali Geometry Culling Rate:Sample culled primitive rate"),
        @("pos", "Mali Geometry Threads:Position shading threads"), @("var", "Mali Geometry Threads:Varying shading threads"),
        @("px", "Mali Pixels:Pixels"), @("over", "Mali Overdraw:Fragments/pixel"),
        @("nfw", "Mali Core Warps:Non-fragment warps"),
        @("cynf", "Mali Core Throughput:Cycles/non-fragment thread"), @("cyf", "Mali Core Throughput:Cycles/fragment thread"),
        @("exec", "Mali Core Utilization:Execution core utilization"),
        @("nfu", "Mali Core Utilization:Non-fragment utilization"), @("fu", "Mali Core Utilization:Fragment utilization"),
        @("arith", "Mali Core Unit Utilization:Arithmetic unit utilization"),
        @("lsu", "Mali Core Unit Utilization:Load/store unit utilization"),
        @("texu", "Mali Core Unit Utilization:Texture unit utilization"),
        @("fr", "Mali Load/Store Usage:Full read"), @("pr", "Mali Load/Store Usage:Partial read"),
        @("fw", "Mali Load/Store Usage:Full write"), @("pw", "Mali Load/Store Usage:Partial write"),
        @("xls", "Mali Core External Memory Reads:Load/store unit bytes"),
        @("xtex", "Mali Core External Memory Reads:Texture unit bytes"),
        @("wls", "Mali Core Memory Writes:Load/store unit bytes"), @("wtile", "Mali Core Memory Writes:Tile unit bytes"))) {
        $m[$pair[0]] = Mean $timeline $rows $pair[1]
    }
    $threads = $m.pos + $m.var
    $latencyAll = $m.l0 + $m.l1 + $m.l2 + $m.l3 + $m.l4 + $m.l5
    $out = New-Object System.Collections.ArrayList
    function Add([string]$name, [double]$value, [string]$format, [string]$unit) {
        [void]$out.Add([pscustomobject]@{ Name = $name; Value = $value; Format = $format; Unit = $unit })
    }
    Add "GPU: ciclos activos (= MHz si esta al 100 %)" ($m.gpu / 1e6) "0" "M/s"
    Add "Cola de vertices ocupada" $m.vqu "0.0" "%"
    Add "Cola de fragmentos ocupada" $m.fqu "0.0" "%"
    Add "Cola de computo ocupada" $m.cqu "0.0" "%"
    Add "Triangulos enviados" ($m.in / 1e6) "0.00" "M/s"
    Add "  visibles" (100 * (Ratio $m.vis $m.in)) "0.0" "%"
    Add "  descartados: no tocan ningun pixel" $m.samp "0.0" "%"
    Add "  descartados: fuera de pantalla" $m.frus "0.0" "%"
    Add "  descartados: de espaldas" $m.face "0.0" "%"
    Add "Vertices procesados (posicion)" ($m.pos / 1e6) "0.00" "M/s"
    Add "Vertices procesados (resto de datos)" ($m.var / 1e6) "0.00" "M/s"
    Add "  vertices de posicion por triangulo enviado" (Ratio $m.pos $m.in) "0.00" ""
    Add "  vertices completos por triangulo visible" (Ratio $m.var $m.vis) "0.00" ""
    Add "Ciclos por vertice (contador de Mali)" $m.cynf "0.0" "ciclos"
    Add "  ciclos de cola de vertices por vertice" (Ratio $m.vq $threads) "0.0" "ciclos"
    Add "  vertices por tanda (warp, maximo 16)" (Ratio $threads $m.nfw) "0.0" ""
    Add "Nucleos: con trabajo dentro" $m.exec "0.0" "%"
    Add "  calculando (aritmetica)" $m.arith "0.0" "%"
    Add "  leyendo/escribiendo memoria" $m.lsu "0.0" "%"
    Add "  texturas" $m.texu "0.0" "%"
    Add "Lecturas de memoria por vertice (como mucho)" (Ratio ($m.fr + $m.pr) $threads) "0.00" ""
    Add "  completas (16 bytes)" (Ratio $m.fr $threads) "0.00" ""
    Add "  parciales" (Ratio $m.pr $threads) "0.00" ""
    Add "Escrituras de memoria por vertice (como mucho)" (Ratio ($m.fw + $m.pw) $threads) "0.00" ""
    Add "Bytes escritos por vertice completo" (Ratio $m.wls $m.var) "0" "B"
    Add "Lecturas que salen a la RAM por tanda de vertices" (Ratio ($m.xls / 64) $m.nfw) "0.0" "lineas"
    Add "Latencia de la RAM: lecturas de mas de 128 ciclos" (100 * (Ratio ($latencyAll - $m.l0) $latencyAll)) "0.0" "%"
    Add "  de mas de 384 ciclos" (100 * (Ratio $m.l5 $latencyAll)) "0.0" "%"
    Add "Memoria leida" ($m.rd / 1e9) "0.00" "GB/s"
    Add "  por texturas" ($m.xtex / 1e9) "0.00" "GB/s"
    Add "Memoria escrita" ($m.wr / 1e9) "0.00" "GB/s"
    Add "Pixeles" ($m.px / 1e6) "0" "M/s"
    Add "  veces que se pinta cada pixel" $m.over "0.00" ""
    Add "  ciclos por fragmento" $m.cyf "0.00" "ciclos"
    if ($fps -gt 0) {
        Add ("Por fotograma (a {0} fps): triangulos" -f $fps.ToString("0.0", $inv)) ($m.in / $fps / 1e3) "0" "miles"
        Add "  vertices procesados" ($threads / $fps / 1e3) "0" "miles"
        Add "  ciclos de GPU" ($m.gpu / $fps / 1e6) "0.0" "M"
        Add "  ciclos de cola de vertices" ($m.vq / $fps / 1e6) "0.0" "M"
        Add "  ciclos de cola de fragmentos" ($m.fq / $fps / 1e6) "0.0" "M"
    }
    return [pscustomobject]@{ Figures = $out; Raw = $m; Threads = $threads }
}

function Format-Value($figure) {
    if ([double]::IsNaN($figure.Value) -or [double]::IsInfinity($figure.Value)) { return "n/d" }
    $text = $figure.Value.ToString($figure.Format, $inv)
    if ($figure.Unit) { return "$text $($figure.Unit)" }
    return $text
}

# What limits the GPU, from the window's figures.
function Diagnose($measure) {
    $m = $measure.Raw
    $notes = New-Object System.Collections.ArrayList
    if (-not [double]::IsNaN($m.vqu) -and -not [double]::IsNaN($m.fqu)) {
        if ($m.vqu -ge 60 -and $m.fqu -lt 50) { [void]$notes.Add("LIMITE: la geometria (cola de vertices mucho mas ocupada que la de fragmentos). Bajar la resolucion no ayuda.") }
        elseif ($m.fqu -ge 60 -and $m.vqu -lt 50) { [void]$notes.Add("LIMITE: los pixeles (cola de fragmentos). Bajar la resolucion si ayuda.") }
        elseif ($m.vqu -ge 60 -and $m.fqu -ge 60) { [void]$notes.Add("LIMITE: geometria y pixeles a la vez.") }
        else { [void]$notes.Add("La GPU no esta saturada en este tramo.") }
    }
    if (-not [double]::IsNaN($m.exec) -and -not [double]::IsNaN($m.arith) -and $m.exec -ge 90 -and $m.arith -lt 35) {
        [void]$notes.Add(("Los nucleos estan ocupados ({0:0} %) pero calculan poco ({1:0} %): esperan datos, no les falta potencia." -f $m.exec, $m.arith))
    }
    $all = $m.l0 + $m.l1 + $m.l2 + $m.l3 + $m.l4 + $m.l5
    if (-not [double]::IsNaN($all) -and $all -gt 0 -and (($all - $m.l0) / $all) -ge 0.5) {
        [void]$notes.Add(("La RAM responde lenta: {0:0} % de las lecturas tardan mas de 128 ciclos." -f (100 * ($all - $m.l0) / $all)))
    }
    if (-not [double]::IsNaN($m.samp) -and $m.samp -ge 40) {
        [void]$notes.Add(("{0:0} % de los triangulos no tocan ningun pixel (demasiado pequenos o finos): se procesan para nada." -f $m.samp))
    }
    return $notes
}

$timelineA = Read-Timeline (Resolve-Timeline $Captura)
$windowA = Select-Window $timelineA $Desde $Hasta
$measureA = Measure-Window $timelineA $windowA.Rows $Fps
Write-Output ("A: {0}" -f $timelineA.Path)
Write-Output ("   tramo {0}" -f $windowA.Note)
$measureB = $null
if ($Comparar) {
    $timelineB = Read-Timeline (Resolve-Timeline $Comparar)
    $windowB = Select-Window $timelineB $DesdeB $HastaB
    $measureB = Measure-Window $timelineB $windowB.Rows $FpsB
    Write-Output ("B: {0}" -f $timelineB.Path)
    Write-Output ("   tramo {0}" -f $windowB.Note)
}
Write-Output ""
if ($measureB) {
    Write-Output ("{0,-52} {1,16} {2,16} {3,9}" -f "", "A", "B", "cambio")
    $byName = @{}
    foreach ($figure in $measureB.Figures) { $byName[($figure.Name -replace "\(a [\d.]+ fps\)", "")] = $figure }
    foreach ($figure in $measureA.Figures) {
        $other = $byName[($figure.Name -replace "\(a [\d.]+ fps\)", "")]
        $otherText = "n/d"; $change = ""
        if ($other) {
            $otherText = Format-Value $other
            if (-not [double]::IsNaN($figure.Value) -and -not [double]::IsNaN($other.Value) -and $figure.Value -ne 0) {
                $change = ((($other.Value - $figure.Value) / [Math]::Abs($figure.Value)) * 100).ToString("+0.0;-0.0", $inv) + " %"
            }
        }
        Write-Output ("{0,-52} {1,16} {2,16} {3,9}" -f $figure.Name, (Format-Value $figure), $otherText, $change)
    }
} else {
    foreach ($figure in $measureA.Figures) { Write-Output ("{0,-52} {1,16}" -f $figure.Name, (Format-Value $figure)) }
}
Write-Output ""
foreach ($note in (Diagnose $measureA)) { Write-Output ("A> " + $note) }
if ($measureB) { foreach ($note in (Diagnose $measureB)) { Write-Output ("B> " + $note) } }

if ($Todo) {
    Write-Output ""
    Write-Output "Media de cada contador en el tramo A:"
    for ($c = 1; $c -lt $timelineA.Names.Count; $c++) {
        $value = Mean $timelineA $windowA.Rows $timelineA.Names[$c]
        if (-not [double]::IsNaN($value) -and $value -ne 0) {
            Write-Output ("  {0,-70} {1,18}" -f $timelineA.Names[$c], $value.ToString("0.###", $inv))
        }
    }
}
