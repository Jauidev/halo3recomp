# Halo 3 phone GPU: where the frame's triangles and vertices come from, per shader.
#
# Asks the running game for a geometry census through the renderer's live control file
# (graphics_system.cpp, "census <n> <frames>" in xnative_skip.txt; needs an APK built with the
# census, 2026-11-09 or later): over the next whole frames the renderer counts, per vertex shader
# + pixel shader + pass + primitive type, the draws, the elements sent to the GPU and, for
# indexed draws, the vertices their indices SPAN (max - min + 1) and the DISTINCT vertices they
# name. The game keeps running; nothing is restarted and no draw is changed.
#
# Why the span matters: a GPU that shades the whole index range of a draw works on "span"
# vertices where the draw only needs "distinct". span / distinct = wasted vertex work.
#
# Output (screen, and logs\gpu-censo\<date>\): the frame's totals, the split by kind of draw, the
# ranking of vertex shaders by vertices the GPU works on, and censo.csv (every row, per frame),
# which tools\halo3-gpu-quitar.ps1 reads to choose what to switch off.
#
#   powershell -ExecutionPolicy Bypass -File tools\halo3-gpu-censo.ps1
#   ... -Fotogramas 40          frames counted (default 20)
#   ... -Top 30                 rows of the ranking (default 25)
#   ... -Fichero <txt>          read a census file already pulled instead of asking the game
param(
    [int]$Fotogramas = 20,
    [int]$Top = 25,
    [string]$Fichero = "",
    [string]$Out = "",
    [int]$Espera = 60,
    [string]$Device = $env:ANDROID_SERIAL,
    [string]$Package = "com.xport.halo3"
)
# Continue: Windows PowerShell turns adb's stderr lines into errors; failures are checked with
# $LASTEXITCODE or by the result and thrown explicitly.
$ErrorActionPreference = "Continue"
$inv = [Globalization.CultureInfo]::InvariantCulture
$adbDir = (Join-Path $env:LOCALAPPDATA "Android\Sdk\platform-tools")
if (Test-Path $adbDir) { $env:PATH = "$adbDir;$env:PATH" }
$Remote = "/sdcard/Android/data/$Package/files"
if (-not $Out) {
    $Out = Join-Path (Join-Path $PSScriptRoot "..\work\logs\gpu-censo") (Get-Date -Format "yyyyMMdd_HHmmss")
}
New-Item -ItemType Directory -Force $Out | Out-Null
$report = New-Object System.Collections.ArrayList
function Say([string]$text) { [void]$report.Add($text); Write-Output $text }
function Fail([string]$text) { Write-Output ("ERROR: " + $text); exit 1 }

# ---- 1. The census text: from a file, or from the running game.
$censusPath = Join-Path $Out "xnative_census.txt"
if ($Fichero) {
    if (-not (Test-Path -LiteralPath $Fichero)) { Fail "no existe $Fichero" }
    Copy-Item -LiteralPath $Fichero -Destination $censusPath -Force
} else {
    $state = (& adb -s $Device get-state 2>$null | Out-String).Trim()
    if ($state -ne "device") { Fail "el movil $Device no esta conectado por adb" }
    $gamePid = (& adb -s $Device shell pidof $Package 2>$null | Out-String).Trim()
    if (-not $gamePid) { Fail "el juego no esta abierto: abrelo, entra en la mision y vuelve a lanzar esto" }
    $layersOn = (& adb -s $Device shell "settings get global enable_gpu_debug_layers" 2>$null | Out-String).Trim()
    $layersApp = (& adb -s $Device shell "settings get global gpu_debug_app" 2>$null | Out-String).Trim()
    $layersNow = (& adb -s $Device shell "settings get global gpu_debug_layers" 2>$null | Out-String).Trim()
    if ($layersOn -eq "1" -and $layersApp -eq $Package -and $layersNow -ne "VK_LAYER_LGL_gpu_timeline") {
        Write-Output "AVISO: el movil tiene capas de depuracion de Vulkan activadas para el juego (va mas lento). El censo cuenta geometria y sigue valiendo; para quitarlas: tools\halo3-gpu-quitar.ps1 -QuitarCapas."
    }
    # Lines already in the control file (cvars, skips) stay as they are: the census line is added
    # for the request and the file is put back afterwards.
    $previous = @(& adb -s $Device shell "cat $Remote/xnative_skip.txt 2>/dev/null" | Where-Object { $_ -and ($_ -notmatch "^\s*census\s") })
    $request = [int]([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() % 1000000000)
    $local = Join-Path $Out "xnative_skip.txt"
    [IO.File]::WriteAllText($local, ((@($previous) + @("census $request $Fotogramas")) -join "`n") + "`n")
    & adb -s $Device shell "rm -f $Remote/xnative_census.txt" | Out-Null
    & adb -s $Device push $local "$Remote/xnative_skip.txt" 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "adb push del fichero de control ha fallado" }
    Write-Output ("Censo pedido ({0} fotogramas); no muevas la camara..." -f $Fotogramas)
    $text = $null
    $deadline = (Get-Date).AddSeconds($Espera)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 800
        $lines = @(& adb -s $Device shell "cat $Remote/xnative_census.txt 2>/dev/null")
        if ($lines.Count -gt 0 -and ($lines | Select-Object -Last 1).Trim() -eq "end $request") { $text = $lines; break }
    }
    # The control file as it was (without the census line).
    if ($previous.Count -gt 0) {
        [IO.File]::WriteAllText($local, (($previous -join "`n") + "`n"))
        & adb -s $Device push $local "$Remote/xnative_skip.txt" 2>&1 | Out-Null
    } else {
        & adb -s $Device shell "rm -f $Remote/xnative_skip.txt" | Out-Null
    }
    if (-not $text) {
        Fail ("el juego no ha escrito el censo en {0} s. Causas: el APK instalado no lleva el censo (hace falta uno 'c0censo' o posterior), o el juego no esta dibujando una partida (menu, pausa, pantalla apagada)." -f $Espera)
    }
    [IO.File]::WriteAllLines($censusPath, [string[]]$text)
}

# ---- 2. Parse.
$frames = 0; $milliseconds = 0
$rows = New-Object System.Collections.ArrayList
$complete = $false
foreach ($line in [IO.File]::ReadAllLines($censusPath)) {
    $w = @($line.Trim() -split "\s+")
    if ($w[0] -eq "census" -and $w.Count -ge 6) { $frames = [int]$w[3]; $milliseconds = [double]$w[5] }
    elseif ($w[0] -eq "end") { $complete = $true }
    elseif ($w[0] -eq "d" -and $w.Count -ge 11) {
        [void]$rows.Add([pscustomobject]@{
            Vs = $w[1]; Ps = $w[2]; Pass = $w[3]; Primitive = [int]$w[4]; Flags = [int]$w[5]
            Draws = [double]$w[6]; Elements = [double]$w[7]; SpanDraws = [double]$w[8]
            Span = [double]$w[9]; Distinct = [double]$w[10]
        })
    }
}
if (-not $complete) { Fail "el fichero del censo esta incompleto (no termina en 'end')" }
if ($frames -le 0) { Fail "el censo no dice cuantos fotogramas cubre" }
if ($rows.Count -eq 0) { Fail "el censo no tiene ningun dibujado" }

# Triangles of a row, from the guest primitive type and the HOST element count (quad lists and
# rectangles reach the GPU already as triangles). An estimate: strips with cuts and fans count
# a little high or low. Xenos types: 1 points, 2 lines, 3 line strip, 4 triangle list,
# 5 triangle fan, 6 triangle strip, 8 rectangle list, 13 quad list.
function Triangles($row) {
    $e = $row.Elements; $d = $row.Draws
    switch ($row.Primitive) {
        1 { return 0 }
        2 { return 0 }
        3 { return 0 }
        6 { return [Math]::Max(0, $e - 2 * $d) }
        5 { if (($e % 3) -eq 0) { return $e / 3 } else { return [Math]::Max(0, $e - 2 * $d) } }
        8 { if (($row.Flags -band 1) -ne 0) { return $e / 3 } else { return $e / 2 } }
        default { return $e / 3 }
    }
}
# Vertices the GPU works on for a row: the index span when known; otherwise the elements (a
# non-indexed draw shades each vertex it is given; an indexed draw without a known span cannot
# shade more distinct vertices than it has indices).
function Worked($row) {
    if ($row.SpanDraws -gt 0) {
        $known = $row.Span
        if ($row.SpanDraws -lt $row.Draws) { $known += $row.Elements * (1 - $row.SpanDraws / $row.Draws) }
        return $known
    }
    return $row.Elements
}
function Needed($row) {
    if ($row.SpanDraws -gt 0) {
        $known = $row.Distinct
        if ($row.SpanDraws -lt $row.Draws) { $known += $row.Elements * (1 - $row.SpanDraws / $row.Draws) }
        return $known
    }
    return $row.Elements
}
function Kind($row) {
    $f = $row.Flags
    if (($f -band 2) -ne 0) { return "computed-fetch (agua, particulas)" }
    if (($f -band 4) -eq 0) { return "solo profundidad (sombras, prepase)" }
    if (($f -band 16) -ne 0) { return "con mezcla (transparentes, luces)" }
    return "opaco con color"
}
function PrimitiveName([int]$type) {
    switch ($type) {
        1 { "puntos" } 2 { "lineas" } 3 { "tira de lineas" } 4 { "lista de triangulos" } 5 { "abanico" }
        6 { "tira de triangulos" } 8 { "rectangulos" } 13 { "cuadrilateros" } default { "tipo $type" }
    }
}
foreach ($row in $rows) {
    $row | Add-Member -NotePropertyName Tris -NotePropertyValue (Triangles $row)
    $row | Add-Member -NotePropertyName Worked -NotePropertyValue (Worked $row)
    $row | Add-Member -NotePropertyName Needed -NotePropertyValue (Needed $row)
    $row | Add-Member -NotePropertyName Kind -NotePropertyValue (Kind $row)
}
function Sum($items, [string]$property) {
    $total = 0.0
    foreach ($item in $items) { $total += $item.$property }
    return $total
}
function PerFrame([double]$value) { return $value / $frames }
function N([double]$value, [string]$format = "0") { return $value.ToString($format, $inv) }

# ---- 3. Totals.
$allDraws = Sum $rows "Draws"; $allTris = Sum $rows "Tris"; $allElements = Sum $rows "Elements"
$allWorked = Sum $rows "Worked"; $allNeeded = Sum $rows "Needed"
$spanRows = @($rows | Where-Object { $_.SpanDraws -gt 0 })
$allSpan = Sum $spanRows "Span"; $allDistinct = Sum $spanRows "Distinct"
$fps = 0.0
if ($milliseconds -gt 0) { $fps = $frames * 1000.0 / $milliseconds }
Say ("Censo de {0} fotogramas en {1} ms ({2} fps mientras se contaba)" -f $frames, (N $milliseconds), (N $fps "0.0"))
Say ""
Say "POR FOTOGRAMA"
Say ("  dibujados                              {0,10}" -f (N (PerFrame $allDraws)))
Say ("  triangulos (estimado)                  {0,10}" -f (N (PerFrame $allTris)))
Say ("  indices / vertices enviados            {0,10}" -f (N (PerFrame $allElements)))
Say ("  vertices distintos que hacen falta     {0,10}" -f (N (PerFrame $allNeeded)))
Say ("  vertices del rango (lo que recorre una GPU que procesa el rango entero) {0,10}" -f (N (PerFrame $allWorked)))
if ($allDistinct -gt 0) {
    Say ("  rango / distintos en los dibujados con indices: x{0}  ({1} % de los dibujados con indices medidos)" -f `
        (N ($allSpan / $allDistinct) "0.00"), (N (100 * (Sum $spanRows "SpanDraws") / [Math]::Max(1, (Sum (@($rows | Where-Object { ($_.Flags -band 1) -ne 0 })) "Draws"))) "0"))
}
if ($allTris -gt 0) { Say ("  vertices distintos por triangulo       {0,10}" -f (N ($allNeeded / $allTris) "0.00")) }
Say ""

# ---- 4. By kind of draw and by primitive type.
Say "POR TIPO DE DIBUJADO (por fotograma)"
Say ("  {0,-38} {1,9} {2,11} {3,11} {4,11} {5,7}" -f "", "dibujados", "triangulos", "distintos", "rango", "% rango")
foreach ($group in ($rows | Group-Object Kind | Sort-Object { -(Sum $_.Group "Worked") })) {
    $worked = Sum $group.Group "Worked"
    Say ("  {0,-38} {1,9} {2,11} {3,11} {4,11} {5,6} %" -f $group.Name, (N (PerFrame (Sum $group.Group "Draws"))),
        (N (PerFrame (Sum $group.Group "Tris"))), (N (PerFrame (Sum $group.Group "Needed"))), (N (PerFrame $worked)),
        (N (100 * $worked / [Math]::Max(1, $allWorked)) "0.0"))
}
Say ""
Say "POR PRIMITIVA (por fotograma)"
foreach ($group in ($rows | Group-Object Primitive | Sort-Object { -(Sum $_.Group "Elements") })) {
    Say ("  {0,-38} {1,9} {2,11} {3,11}" -f (PrimitiveName ([int]$group.Name)), (N (PerFrame (Sum $group.Group "Draws"))),
        (N (PerFrame (Sum $group.Group "Tris"))), (N (PerFrame (Sum $group.Group "Elements"))))
}
$passes = @($rows | Group-Object Pass)
if ($passes.Count -gt 1) {
    Say ""
    Say "POR PASE DEL JUEGO (por fotograma)"
    foreach ($group in ($passes | Sort-Object { -(Sum $_.Group "Worked") } | Select-Object -First 16)) {
        $name = $group.Name
        if ($name -eq "FFFFFFFF") { $name = "sin pase" }
        Say ("  {0,-38} {1,9} {2,11} {3,11} {4,11}" -f $name, (N (PerFrame (Sum $group.Group "Draws"))),
            (N (PerFrame (Sum $group.Group "Tris"))), (N (PerFrame (Sum $group.Group "Needed"))), (N (PerFrame (Sum $group.Group "Worked"))))
    }
}
Say ""

# ---- 5. Ranking of vertex shaders.
$byVs = @($rows | Group-Object Vs | ForEach-Object {
    $g = $_.Group
    $worked = Sum $g "Worked"; $needed = Sum $g "Needed"; $tris = Sum $g "Tris"
    $kinds = @($g | Group-Object Kind | Sort-Object { -(Sum $_.Group "Worked") } | ForEach-Object { $_.Name })
    [pscustomobject]@{
        Vs = $_.Name; Draws = (Sum $g "Draws"); Tris = $tris; Needed = $needed; Worked = $worked
        Waste = $(if ($needed -gt 0) { $worked / $needed } else { 0 })
        PerTri = $(if ($tris -gt 0) { $needed / $tris } else { 0 })
        PixelShaders = @($g | ForEach-Object { $_.Ps } | Sort-Object -Unique).Count
        Kind = $kinds[0]
        OnlyDepth = (@($g | Where-Object { ($_.Flags -band 4) -ne 0 }).Count -eq 0)
        ComputedFetch = (@($g | Where-Object { ($_.Flags -band 2) -ne 0 }).Count -gt 0)
    }
} | Sort-Object Worked -Descending)
Say ("VERTEX SHADERS POR TRABAJO DE VERTICES (por fotograma; {0} en total, se listan {1})" -f $byVs.Count, [Math]::Min($Top, $byVs.Count))
Say ("  {0,-16} {1,6} {2,9} {3,9} {4,9} {5,6} {6,6} {7,6} {8,3}  {9}" -f "vertex shader", "dibuj.", "triang.", "distintos", "rango", "x", "v/tri", "% tot", "PS", "tipo principal")
$accumulated = 0.0
foreach ($item in ($byVs | Select-Object -First $Top)) {
    $accumulated += $item.Worked
    Say ("  {0,-16} {1,6} {2,9} {3,9} {4,9} {5,6} {6,6} {7,5} % {8,3}  {9}" -f $item.Vs, (N (PerFrame $item.Draws)),
        (N (PerFrame $item.Tris)), (N (PerFrame $item.Needed)), (N (PerFrame $item.Worked)), (N $item.Waste "0.00"),
        (N $item.PerTri "0.00"), (N (100 * $item.Worked / [Math]::Max(1, $allWorked)) "0.0"), $item.PixelShaders, $item.Kind)
}
Say ("  los {0} primeros suman el {1} % del trabajo de vertices" -f [Math]::Min($Top, $byVs.Count), (N (100 * $accumulated / [Math]::Max(1, $allWorked)) "0.0"))
Say ""
Say "Columnas: 'distintos' = vertices que el dibujado necesita; 'rango' = vertices entre el indice menor y el"
Say "mayor (lo que procesa una GPU que recorre el rango entero); 'x' = rango / distintos (1.00 = nada"
Say "desperdiciado); 'v/tri' = vertices distintos por triangulo (0.5-1 es una malla bien compartida)."

# ---- 6. Files.
$csv = New-Object System.Collections.ArrayList
[void]$csv.Add("vs,ps,pass,primitive,flags,kind,draws_frame,tris_frame,elements_frame,needed_frame,worked_frame,span_draws")
foreach ($row in ($rows | Sort-Object Worked -Descending)) {
    [void]$csv.Add(("{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11}" -f $row.Vs, $row.Ps, $row.Pass, $row.Primitive, $row.Flags,
        $row.Kind, (N (PerFrame $row.Draws) "0.###"), (N (PerFrame $row.Tris) "0.###"), (N (PerFrame $row.Elements) "0.###"),
        (N (PerFrame $row.Needed) "0.###"), (N (PerFrame $row.Worked) "0.###"), (N $row.SpanDraws)))
}
[IO.File]::WriteAllLines((Join-Path $Out "censo.csv"), [string[]]$csv)
$vsCsv = New-Object System.Collections.ArrayList
[void]$vsCsv.Add("vs,draws_frame,tris_frame,needed_frame,worked_frame,waste,share,only_depth,computed_fetch,kind")
foreach ($item in $byVs) {
    [void]$vsCsv.Add(("{0},{1},{2},{3},{4},{5},{6},{7},{8},{9}" -f $item.Vs, (N (PerFrame $item.Draws) "0.###"), (N (PerFrame $item.Tris) "0.###"),
        (N (PerFrame $item.Needed) "0.###"), (N (PerFrame $item.Worked) "0.###"), (N $item.Waste "0.###"),
        (N (100 * $item.Worked / [Math]::Max(1, $allWorked)) "0.###"), [int]$item.OnlyDepth, [int]$item.ComputedFetch, $item.Kind))
}
[IO.File]::WriteAllLines((Join-Path $Out "censo_vs.csv"), [string[]]$vsCsv)
[IO.File]::WriteAllLines((Join-Path $Out "censo.txt"), [string[]]$report)
# The newest census, where halo3-gpu-quitar.ps1 looks for it.
$latest = Join-Path (Split-Path $Out -Parent) "ultimo"
New-Item -ItemType Directory -Force $latest | Out-Null
foreach ($name in @("censo.csv", "censo_vs.csv", "censo.txt", "xnative_census.txt")) {
    Copy-Item -LiteralPath (Join-Path $Out $name) -Destination (Join-Path $latest $name) -Force
}
Write-Output ""
Write-Output ("Guardado en: {0}" -f $Out)
