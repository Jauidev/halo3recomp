# Halo 3 water watch: why the water vanishes (or looks wrong) on the phone, without restarting the
# game. It drives the renderer's live control file (xnative_skip.txt in the app's files folder,
# graphics_system.cpp LiveSkip): "watch <hash prefix>" makes the renderer log every draw of the
# water shaders (and the resolves of the textures they read) to xnative_watch.txt, frame by frame:
# whether it was drawn or why not, its fixed state, booleans, textures and their host images; plus
# the tessellated-draw hook's draws sent/rejected (a rejected water draw never reaches the renderer).
#
# Steps (-Paso):
#   vigilar            start the watch (water shaders + ripple chain, -Extra adds hash prefixes)
#   vivo               one line per second of what the water draws do (-Segundos, default 30)
#   marcar -Nombre X   save a state: screenshot, the last seconds of the log, two full snapshots
#                      (all constants, textures, streams). Usual names: fuera (water visible, nobody
#                      in it) and dentro (player in the water, water gone)
#   comparar           what changes between two states (-A fuera -B dentro) and a first diagnosis
#   probar             player standing still in the water: try each suspect live (booleans that
#                      changed, new shaders left out, renderer options) and say for each whether the
#                      water comes back (screenshots compared with the water left out)
#   texturas [-Nombre X] one snapshot with the host images dumped (the ripple maps, scene color and
#                      depth the water reads, the ripple sprites...): .bin files pulled, turned into
#                      PNGs (one per channel too) and per-channel statistics; says whether the ripple
#                      map the water's vertex shader reads (t21) is neutral (0.5 = no displacement)
#   parar              remove the control file (normal play)
#
#   powershell -ExecutionPolicy Bypass -File tools\halo3-agua.ps1 -Paso vigilar
param(
    [ValidateSet("vigilar", "vivo", "marcar", "comparar", "probar", "texturas", "parar")]
    [string]$Paso = "vivo",
    [string]$Nombre = "",
    [string]$A = "fuera",
    [string]$B = "dentro",
    [string]$Extra = "",
    [int]$Segundos = 30,
    [double]$Ventana = 3.0,
    [double]$Settle = 3.0,
    [string]$Roi = "0.03,0.08,0.97,0.92",
    [string]$Sesion = "",
    [string]$Device = $env:ANDROID_SERIAL,
    [string]$Package = "com.xport.halo3"
)
# adb writes harmless lines to stderr; failures are checked with $LASTEXITCODE.
$ErrorActionPreference = "Continue"

# Water: tessellated water VS / PS. Ripple chain: sprite and slope shaders (PS or VS, prefixes from
# the 2026-11-06 traces). 7FF28317: the tessellation-factor exporter, skipped on purpose.
$WaterPrefixes = @("55002F08", "45571CD3")
$RipplePrefixes = @("CCD5CC9D", "A0325633", "371E75ED", "D35A7E07", "7FF28317")
# Renderer options to try live: name, value to try, value to put back (the phone's own).
$CvarTests = @(
    @("xnative_lazy_resolves", "false", "true"),
    @("xnative_skip_invisible_draws", "false", "true"),
    @("xnative_fetch_only_textures", "false", "true"),
    @("xnative_shader_fetch_filters", "false", "true"),
    @("xnative_strip_cut_without_reset", "false", "true"),
    @("xnative_merge_up_draws", "false", "true"),
    @("xnative_resolve_swap", "false", "true")
)

$Remote = "/sdcard/Android/data/$Package/files"
$Root = (Join-Path $PSScriptRoot "..\work\logs\agua")
New-Item -ItemType Directory -Force $Root | Out-Null
$LastSession = Join-Path $Root "ultima.txt"

function Get-Session([bool]$create) {
    if ($Sesion) { $path = $Sesion }
    elseif ($create) { $path = Join-Path $Root (Get-Date -Format "yyyyMMdd_HHmmss") }
    elseif (Test-Path $LastSession) { $path = (Get-Content $LastSession -Raw).Trim() }
    else { throw "no hay sesion: empieza con -Paso vigilar" }
    New-Item -ItemType Directory -Force $path | Out-Null
    if ($create) { Set-Content -Path $LastSession -Value $path -Encoding UTF8 }
    return $path
}

function Watch-Lines {
    $prefixes = @($WaterPrefixes + $RipplePrefixes)
    $prefixes += @($Extra -split "[,; ]+" | Where-Object { $_ } | ForEach-Object { $_.Trim().ToUpper() })
    return @($prefixes | ForEach-Object { "watch $_" })
}

function Set-Control([string]$session, [string[]]$lines) {
    $local = Join-Path $session "xnative_skip.txt"
    [IO.File]::WriteAllText($local, (($lines -join "`n") + "`n"))
    & adb -s $Device push $local "$Remote/xnative_skip.txt" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "adb push ha fallado (movil conectado?)" }
}

function Get-Remote([string]$command) {
    $text = & adb -s $Device exec-out $command
    return @($text)
}

# ---- Parsing ---------------------------------------------------------------------------------
function Parse-Line([string]$line) {
    $tokens = $line.Trim() -split " "
    $o = @{ Kind = $tokens[0]; Raw = $line }
    $start = 1
    if ($tokens[0] -eq "D" -or $tokens[0] -eq "R" -or $tokens[0] -eq "C") {
        $o.Frame = [int64]$tokens[1]; $o.Cmd = [int]$tokens[2]; $start = 3
    } elseif ($tokens[0] -eq "F") {
        $o.Frame = [int64]$tokens[1]; $start = 2
    }
    for ($i = $start; $i -lt $tokens.Count; $i++) {
        $eq = $tokens[$i].IndexOf("=")
        if ($eq -gt 0) { $o[$tokens[$i].Substring(0, $eq)] = $tokens[$i].Substring($eq + 1) }
    }
    return $o
}

# The log lines of the last $seconds: each D/R line belongs to the F line after it (its part).
function Select-Recent([string[]]$lines, [double]$seconds) {
    $parsed = @($lines | Where-Object { $_ -match "^[DRCF] " } | ForEach-Object { Parse-Line $_ })
    if ($parsed.Count -eq 0) { return @() }
    $lastT = $null
    for ($i = $parsed.Count - 1; $i -ge 0; $i--) {
        if ($parsed[$i].Kind -eq "F") { $lastT = [int64]$parsed[$i].t; break }
    }
    if ($null -eq $lastT) { return @() }
    $from = $lastT - [int64]($seconds * 1000)
    $kept = New-Object System.Collections.ArrayList
    $current = $null
    for ($i = $parsed.Count - 1; $i -ge 0; $i--) {
        $p = $parsed[$i]
        if ($p.Kind -eq "F") { $current = [int64]$p.t }
        if ($null -eq $current) { continue }   # lines after the last F: part not finished yet
        if ($current -lt $from) { break }
        $p.T = $current
        [void]$kept.Insert(0, $p)
    }
    return @($kept)
}

function Short([string]$hash) { if ($hash.Length -ge 8) { return $hash.Substring(0, 8) } return $hash }
function Group-Key($d) { return "{0}/{1}" -f (Short $d.vs), (Short $d.ps) }
function Is-Water($d) {
    foreach ($p in $WaterPrefixes) { if ($d.vs.StartsWith($p) -or $d.ps.StartsWith($p)) { return $true } }
    return $false
}
function Boolean-Name([int]$word, [int]$bit) {
    switch ($word) { 0 { return "b$bit" } 1 { return "b$(128 + $bit)" } 2 { return "b$(96 + $bit)" } default { return "b$(224 + $bit)" } }
}
function Most-Common([object[]]$values) {
    if (-not $values -or $values.Count -eq 0) { return "" }
    return ($values | Group-Object | Sort-Object Count -Descending | Select-Object -First 1).Name
}

# A state's numbers: per presented frame, per shader pair (outcomes, booleans, textures), resolves.
function Summarize($entries) {
    $frames = @($entries | Where-Object { $_.Kind -eq "F" -and $_.Frame -gt 0 }).Count
    if ($frames -lt 1) { $frames = 1 }
    $fl = @($entries | Where-Object { $_.Kind -eq "F" })
    $sent = 0; $rejected = 0; $lastRej = ""
    foreach ($f in $fl) {
        $parts = $f.tess -split "/"
        $sent += [int]$parts[0]; $rejected += [int]$parts[1]
        if ($f.rej) { $lastRej = $f.rej }
    }
    $groups = @{}
    foreach ($d in @($entries | Where-Object { $_.Kind -eq "D" })) {
        $key = Group-Key $d
        if (-not $groups.ContainsKey($key)) {
            $groups[$key] = @{ Key = $key; Vs = $d.vs; Ps = $d.ps; Draws = New-Object System.Collections.ArrayList }
        }
        [void]$groups[$key].Draws.Add($d)
    }
    $resolves = @{}
    foreach ($r in @($entries | Where-Object { $_.Kind -eq "R" })) {
        if (-not $resolves.ContainsKey($r.dst)) { $resolves[$r.dst] = New-Object System.Collections.ArrayList }
        [void]$resolves[$r.dst].Add($r.out)
    }
    return @{ Frames = $frames; Sent = $sent; Rejected = $rejected; LastRej = $lastRej;
              Groups = $groups; Resolves = $resolves }
}

function Outcome-Text($draws) {
    return (($draws | ForEach-Object { $_.out } | Group-Object | Sort-Object Count -Descending |
             ForEach-Object { "{0} x{1}" -f $_.Name, $_.Count }) -join ", ")
}

function Texture-Map($d) {
    $map = @{}
    foreach ($k in $d.Keys) { if ($k -match "^t\d+$") { $map[$k] = $d[$k] } }
    return $map
}

# ---- Screenshots -----------------------------------------------------------------------------
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class WaterMeter {
    static float[] Luma(string path, out int w, out int h) {
        using (Bitmap bitmap = new Bitmap(path)) {
            int fw = bitmap.Width, fh = bitmap.Height;
            BitmapData data = bitmap.LockBits(new Rectangle(0, 0, fw, fh), ImageLockMode.ReadOnly,
                                              PixelFormat.Format24bppRgb);
            int stride = data.Stride;
            byte[] pixels = new byte[stride * fh];
            Marshal.Copy(data.Scan0, pixels, 0, pixels.Length);
            bitmap.UnlockBits(data);
            w = fw / 2; h = fh / 2;
            float[] g = new float[w * h];
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    float sum = 0;
                    for (int dy = 0; dy < 2; dy++)
                        for (int dx = 0; dx < 2; dx++) {
                            int o = (2 * y + dy) * stride + (2 * x + dx) * 3;
                            sum += 0.114f * pixels[o] + 0.587f * pixels[o + 1] + 0.299f * pixels[o + 2];
                        }
                    g[y * w + x] = sum * 0.25f;
                }
            return g;
        }
    }
    // Percentage of 8x8 blocks (half resolution) whose mean luminance difference exceeds threshold.
    public static double BlockDiff(string a, string b, double fx0, double fy0, double fx1, double fy1,
                                   double threshold) {
        int wa, ha, wb, hb;
        float[] ga = Luma(a, out wa, out ha);
        float[] gb = Luma(b, out wb, out hb);
        if (wa != wb || ha != hb) return 100;
        const int block = 8;
        int x0 = (int)(fx0 * wa), y0 = (int)(fy0 * ha), x1 = (int)(fx1 * wa), y1 = (int)(fy1 * ha);
        int total = 0, changed = 0;
        for (int by = y0; by + block <= y1; by += block)
            for (int bx = x0; bx + block <= x1; bx += block) {
                double sum = 0;
                for (int y = by; y < by + block; y++)
                    for (int x = bx; x < bx + block; x++) sum += Math.Abs(ga[y * wa + x] - gb[y * wa + x]);
                total++;
                if (sum / (block * block) > threshold) changed++;
            }
        return total > 0 ? 100.0 * changed / total : 0;
    }
}
"@
$RoiParts = $Roi.Split(",") | ForEach-Object { [double]$_ }
function Diff([string]$a, [string]$b) {
    return [math]::Round([WaterMeter]::BlockDiff($a, $b, $RoiParts[0], $RoiParts[1], $RoiParts[2], $RoiParts[3], 10), 1)
}
function Capture([string]$folder, [string]$name) {
    # Nothing but the path may reach the output: adb's own lines would turn it into an array.
    & adb -s $Device shell screencap -p /sdcard/xnative_agua.png | Out-Null
    $path = Join-Path $folder "$name.png"
    & adb -s $Device pull /sdcard/xnative_agua.png $path | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "adb pull de la captura ha fallado" }
    return [string]$path
}

function Request-Snap([string]$session, [string[]]$base, [string]$path) {
    $request = [int]([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() % 100000000)
    Set-Control $session (@($base) + @("snap $request"))
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 400
        $text = Get-Remote "cat $Remote/xnative_watch_snap.txt 2>/dev/null"
        if ($text.Count -gt 0 -and ($text | Select-Object -Last 1).Trim() -eq "end $request") {
            [IO.File]::WriteAllLines($path, [string[]]$text)
            return $true
        }
    }
    return $false
}

# The control file's current lines (the watch and whatever else the session set).
function Base-Lines([string]$session) {
    $local = Join-Path $session "base.txt"
    if (Test-Path $local) { return @(Get-Content $local) }
    return @(Watch-Lines)
}

# ---- Steps -----------------------------------------------------------------------------------
switch ($Paso) {
"vigilar" {
    $session = Get-Session $true
    $lines = Watch-Lines
    Set-Content -Path (Join-Path $session "base.txt") -Value $lines -Encoding ASCII
    Set-Control $session $lines
    Start-Sleep -Seconds 2
    $head = Get-Remote "head -n 1 $Remote/xnative_watch.txt 2>/dev/null"
    if ($head.Count -eq 0) {
        Write-Output "El juego aun no ha escrito xnative_watch.txt: esta abierto y en partida? (APK con el vigilante del agua)"
    } else {
        Write-Output ("Vigilando: {0}" -f (($lines | ForEach-Object { $_.Substring(6) }) -join ", "))
    }
    Write-Output "Sesion: $session"
}
"vivo" {
    $until = (Get-Date).AddSeconds($Segundos)
    while ((Get-Date) -lt $until) {
        $text = Get-Remote "tail -n 600 $Remote/xnative_watch.txt 2>/dev/null"
        $recent = Select-Recent $text 1.0
        $s = Summarize $recent
        $parts = @()
        foreach ($g in ($s.Groups.Values | Sort-Object { -$_.Draws.Count })) {
            $water = if (Is-Water $g.Draws[0]) { "AGUA " } else { "" }
            $b = Most-Common @($g.Draws | ForEach-Object { $_.b })
            $parts += ("{0}{1} {2:0.#}/fot [{3}] b={4}" -f $water, $g.Key, ($g.Draws.Count / $s.Frames),
                       (Outcome-Text $g.Draws), $b)
        }
        if ($parts.Count -eq 0) { $parts = @("ningun dibujo vigilado") }
        $tess = "teselado {0:0.#} enviados/fot, {1} rechazados" -f ($s.Sent / $s.Frames), $s.Rejected
        if ($s.LastRej) { $tess += " (ultimo rechazado: $($s.LastRej))" }
        Write-Output ("{0} | {1} | {2}" -f (Get-Date -Format "HH:mm:ss"), $tess, ($parts -join " | "))
        Start-Sleep -Milliseconds 1000
    }
}
"marcar" {
    if (-not $Nombre) { throw "falta -Nombre (por ejemplo fuera o dentro)" }
    $session = Get-Session $false
    $folder = Join-Path $session $Nombre
    New-Item -ItemType Directory -Force $folder | Out-Null
    $base = Base-Lines $session
    $shot = Capture $folder "captura"
    $text = Get-Remote "tail -n 6000 $Remote/xnative_watch.txt 2>/dev/null"
    $recent = Select-Recent $text $Ventana
    [IO.File]::WriteAllLines((Join-Path $folder "watch.txt"), [string[]]@($recent | ForEach-Object { $_.Raw }))
    $ok1 = Request-Snap $session $base (Join-Path $folder "snap1.txt")
    Start-Sleep -Milliseconds 600
    $ok2 = Request-Snap $session $base (Join-Path $folder "snap2.txt")
    Set-Control $session $base
    $s = Summarize $recent
    Write-Output ("Estado '{0}' guardado en {1}" -f $Nombre, $folder)
    Write-Output ("  {0} fotogramas en {1} s; teselado {2:0.#} enviados/fot, {3} rechazados" -f $s.Frames, $Ventana, ($s.Sent / $s.Frames), $s.Rejected)
    foreach ($g in ($s.Groups.Values | Sort-Object { -$_.Draws.Count })) {
        Write-Output ("  {0} {1:0.#}/fot: {2}" -f $g.Key, ($g.Draws.Count / $s.Frames), (Outcome-Text $g.Draws))
    }
    if (-not ($ok1 -and $ok2)) { Write-Output "  AVISO: la instantanea completa no llego (el juego no dibujo ningun fotograma?)" }
}
"comparar" {
    $session = Get-Session $false
    $fa = Join-Path $session $A; $fb = Join-Path $session $B
    if (-not (Test-Path $fa) -or -not (Test-Path $fb)) { throw "faltan los estados '$A' o '$B' (usa -Paso marcar)" }
    $report = New-Object System.Collections.ArrayList
    function Say([string]$text) { [void]$report.Add($text); Write-Output $text }
    $sa = Summarize @(Get-Content (Join-Path $fa "watch.txt") | ForEach-Object { Parse-Line $_ })
    $sb = Summarize @(Get-Content (Join-Path $fb "watch.txt") | ForEach-Object { Parse-Line $_ })
    Say ("== Comparacion '{0}' (A) contra '{1}' (B) ==" -f $A, $B)
    Say ("Teselado (el enganche que manda el agua al renderizador): A {0:0.#} enviados/fot, {1} rechazados | B {2:0.#} enviados/fot, {3} rechazados" -f
         ($sa.Sent / $sa.Frames), $sa.Rejected, ($sb.Sent / $sb.Frames), $sb.Rejected)
    if ($sb.LastRej) { Say ("  ultimo dibujo teselado rechazado en B: {0}" -f $sb.LastRej) }
    $keys = @($sa.Groups.Keys + $sb.Groups.Keys | Sort-Object -Unique)
    $waterA = 0.0; $waterB = 0.0; $drawnA = 0.0; $drawnB = 0.0; $failB = @()
    foreach ($key in $keys) {
        $ga = $sa.Groups[$key]; $gb = $sb.Groups[$key]
        $da = if ($ga) { @($ga.Draws) } else { @() }
        $db = if ($gb) { @($gb.Draws) } else { @() }
        $sample = if ($ga) { $ga.Draws[0] } else { $gb.Draws[0] }
        $water = Is-Water $sample
        $label = if ($water) { "AGUA $key" } else { $key }
        Say ""
        Say ("[{0}] vs={1} ps={2}" -f $label, $sample.vs, $sample.ps)
        Say ("  A: {0:0.##}/fot  {1}" -f ($da.Count / $sa.Frames), $(if ($da.Count) { Outcome-Text $da } else { "no se pide" }))
        Say ("  B: {0:0.##}/fot  {1}" -f ($db.Count / $sb.Frames), $(if ($db.Count) { Outcome-Text $db } else { "no se pide" }))
        if ($water) {
            $waterA += $da.Count / $sa.Frames; $waterB += $db.Count / $sb.Frames
            $drawnA += @($da | Where-Object { $_.out -eq "drawn" }).Count / $sa.Frames
            $drawnB += @($db | Where-Object { $_.out -eq "drawn" }).Count / $sb.Frames
            $failB += @($db | Where-Object { $_.out -ne "drawn" } | ForEach-Object { $_.out })
        }
        if ($da.Count -and $db.Count) {
            # Booleans: bits that differ between the usual values.
            $ba = Most-Common @($da | ForEach-Object { $_.b }); $bb = Most-Common @($db | ForEach-Object { $_.b })
            if ($ba -ne $bb -and $ba -and $bb) {
                $wa = $ba -split ":"; $wb = $bb -split ":"; $bits = @()
                for ($w = 0; $w -lt 4; $w++) {
                    $x = [Convert]::ToUInt32($wa[$w], 16) -bxor [Convert]::ToUInt32($wb[$w], 16)
                    for ($bit = 0; $bit -lt 32; $bit++) {
                        if ($x -band ([uint32]1 -shl $bit)) {
                            $va = ([Convert]::ToUInt32($wa[$w], 16) -shr $bit) -band 1
                            $bits += ("{0} {1}->{2}" -f (Boolean-Name $w $bit), $va, (1 - $va))
                        }
                    }
                }
                Say ("  booleanos que cambian: {0}" -f ($bits -join ", "))
            }
            foreach ($field in @("rt", "ds", "z", "st", "cull", "cwm", "bl", "at", "m", "prim", "n")) {
                $va = Most-Common @($da | ForEach-Object { $_[$field] }); $vb = Most-Common @($db | ForEach-Object { $_[$field] })
                if ($va -ne $vb) { Say ("  {0}: {1} -> {2}" -f $field, $va, $vb) }
            }
            $ta = Texture-Map $da[$da.Count - 1]; $tb = Texture-Map $db[$db.Count - 1]
            foreach ($t in @($ta.Keys + $tb.Keys | Sort-Object -Unique)) {
                $xa = $ta[$t]; $xb = $tb[$t]
                # Compare without the generation (changes every frame for GPU-made textures).
                $na = if ($xa) { $xa -replace ":g\d+:", ":g:" -replace ":c\d+:", ":c:" -replace "res@\d+", "res" } else { "(nada)" }
                $nb = if ($xb) { $xb -replace ":g\d+:", ":g:" -replace ":c\d+:", ":c:" -replace "res@\d+", "res" } else { "(nada)" }
                if ($na -ne $nb) { Say ("  textura {0}: {1} -> {2}" -f $t, $na, $nb) }
                if ($xb -and ($xb -match ":(undef|noimg)$")) { Say ("  textura {0} en B: imagen {1} (nunca escrita / sin imagen)" -f $t, $Matches[1]) }
            }
        }
    }
    # Resolves of the textures watched draws read.
    $dsts = @($sa.Resolves.Keys + $sb.Resolves.Keys | Sort-Object -Unique)
    if ($dsts.Count) {
        Say ""
        Say "Resolves de las texturas que leen esos dibujos:"
        foreach ($dst in $dsts) {
            $ra = if ($sa.Resolves[$dst]) { ($sa.Resolves[$dst] | Group-Object | ForEach-Object { "{0} x{1}" -f $_.Name, $_.Count }) -join ", " } else { "ninguno" }
            $rb = if ($sb.Resolves[$dst]) { ($sb.Resolves[$dst] | Group-Object | ForEach-Object { "{0} x{1}" -f $_.Name, $_.Count }) -join ", " } else { "ninguno" }
            if ($ra -ne $rb) { Say ("  {0}: A {1} | B {2}" -f $dst, $ra, $rb) }
        }
    }
    # Full snapshots: registers that change between the states but stay put within each state.
    function Read-Snap([string]$path) {
        $draws = @{}; $current = $null
        if (-not (Test-Path $path)) { return $draws }
        foreach ($line in Get-Content $path) {
            if ($line -match "^D ") {
                $d = Parse-Line $line; $key = Group-Key $d
                if ($draws.ContainsKey($key)) { $current = $null; continue }
                $current = @{ Line = $d; Regs = @{}; Other = @{} }; $draws[$key] = $current
            } elseif ($current -and $line -match "^\s+([vp]c\d+) = (.+)$") {
                $current.Regs[$Matches[1]] = $Matches[2]
            } elseif ($current -and $line -match "^\s+(\S+?)[ =](.*)$") {
                $current.Other[$Matches[1]] = $Matches[2]
            } elseif ($line -notmatch "^\s") { $current = $null }
        }
        return $draws
    }
    $a1 = Read-Snap (Join-Path $fa "snap1.txt"); $a2 = Read-Snap (Join-Path $fa "snap2.txt")
    $b1 = Read-Snap (Join-Path $fb "snap1.txt"); $b2 = Read-Snap (Join-Path $fb "snap2.txt")
    foreach ($key in @($a1.Keys | Where-Object { $b1.ContainsKey($_) })) {
        $x = $a1[$key]; $y = $b1[$key]
        $moving = @{}
        foreach ($pair in @(@($a1, $a2), @($b1, $b2))) {
            if ($pair[0].ContainsKey($key) -and $pair[1].ContainsKey($key)) {
                $p = $pair[0][$key].Regs; $q = $pair[1][$key].Regs
                foreach ($r in @($p.Keys + $q.Keys)) { if ($p[$r] -ne $q[$r]) { $moving[$r] = $true } }
            }
        }
        $changed = @(); $animated = 0
        foreach ($r in @($x.Regs.Keys + $y.Regs.Keys | Sort-Object -Unique)) {
            if ($x.Regs[$r] -ne $y.Regs[$r]) {
                if ($moving[$r]) { $animated++ } else { $changed += ("{0}: {1} -> {2}" -f $r, $(if ($x.Regs[$r]) { $x.Regs[$r] } else { "0" }), $(if ($y.Regs[$r]) { $y.Regs[$r] } else { "0" })) }
            }
        }
        $otherChanged = @()
        foreach ($k in @($x.Other.Keys + $y.Other.Keys | Sort-Object -Unique)) {
            $ox = ($x.Other[$k] -replace "used\d+", "used" -replace "res(True|False)@\d+", "res`$1")
            $oy = ($y.Other[$k] -replace "used\d+", "used" -replace "res(True|False)@\d+", "res`$1")
            if ($ox -ne $oy) { $otherChanged += ("{0}: {1}  ->  {2}" -f $k, $x.Other[$k], $y.Other[$k]) }
        }
        if ($changed.Count -or $otherChanged.Count) {
            Say ""
            Say ("Instantanea completa [{0}]: {1} registros cambian entre A y B sin moverse dentro de cada estado ({2} se mueven solos: camara/animacion)" -f $key, $changed.Count, $animated)
            $changed | Select-Object -First 40 | ForEach-Object { Say "  $_" }
            $otherChanged | Select-Object -First 20 | ForEach-Object { Say "  $_" }
        }
    }
    # First diagnosis.
    Say ""
    Say "== Diagnostico =="
    if ($waterA -lt 0.5) {
        Say "En A el agua tampoco se pide: A no es un estado con el agua visible (o el vigilante no ve sus shaders)."
    } elseif ($waterB -lt 0.1 * $waterA) {
        if ($sb.Rejected -gt 0) {
            Say ("Al pisar el agua el enganche del teselado RECHAZA su dibujo ({0}): el renderizador ni lo ve. Arreglo: aceptar ese caso en sub_825A6DE0 (d3d_hooks.cpp)." -f $sb.LastRej)
        } else {
            Say "Al pisar el agua el JUEGO deja de pedir su dibujo (no llega ni al enganche ni al renderizador): el codigo del juego decide no dibujarla (lado CPU/ARM64, o un dibujo distinto que no se vigila)."
        }
    } elseif ($drawnB -lt 0.5 * $drawnA) {
        Say ("El juego pide el agua pero el renderizador NO la dibuja en B: {0}" -f ((($failB | Group-Object | Sort-Object Count -Descending | ForEach-Object { "{0} x{1}" -f $_.Name, $_.Count }) -join ", ")))
    } else {
        Say "El agua se pide y se dibuja tambien en B: lo que cambia es lo que el shader recibe (booleanos, texturas, constantes de arriba). Siguiente paso: -Paso probar con el jugador quieto dentro del agua."
    }
    [IO.File]::WriteAllLines((Join-Path $session ("comparacion_{0}_{1}.txt" -f $A, $B)), [string[]]$report)
    Write-Output ""
    Write-Output ("Informe guardado en {0}" -f (Join-Path $session ("comparacion_{0}_{1}.txt" -f $A, $B)))
}
"probar" {
    $session = Get-Session $false
    $folder = Join-Path $session ("pruebas_" + (Get-Date -Format "HHmmss"))
    New-Item -ItemType Directory -Force $folder | Out-Null
    $log = Join-Path $folder "pruebas.txt"
    function Note([string]$text) { Write-Output $text; Add-Content -Path $log -Value $text -Encoding UTF8 }
    $base = Base-Lines $session
    # The water's full hashes and the state now (the player is in the water).
    Set-Control $session $base
    Start-Sleep -Seconds 1
    $now = Summarize (Select-Recent (Get-Remote "tail -n 3000 $Remote/xnative_watch.txt 2>/dev/null") 2.0)
    $waterGroups = @($now.Groups.Values | Where-Object { Is-Water $_.Draws[0] })
    if ($waterGroups.Count -eq 0) { Note "AVISO: ahora no se pide ningun dibujo del agua: dejarla fuera no cambia nada; las pruebas siguen, compara las capturas a ojo." }
    $skipWater = @()
    foreach ($g in $waterGroups) { $skipWater += "vs $($g.Vs)" }
    # Experiments: booleans that changed (from the comparison), new shaders, renderer options.
    $tests = New-Object System.Collections.ArrayList
    $comparison = Join-Path $session ("comparacion_{0}_{1}.txt" -f $A, $B)
    if (Test-Path $comparison) {
        foreach ($line in Get-Content $comparison) {
            if ($line -match "booleanos que cambian: (.+)$") {
                foreach ($item in ($Matches[1] -split ", ")) {
                    if ($item -match "^b(\d+) (\d)->(\d)$") {
                        [void]$tests.Add(@{ Name = "bool b$($Matches[1]) a $($Matches[2]) (como en '$A')"; Lines = @("bool $($Matches[1]) $($Matches[2])"); Restore = @() })
                    }
                }
            }
        }
        $sa = Summarize @(Get-Content (Join-Path (Join-Path $session $A) "watch.txt") | ForEach-Object { Parse-Line $_ })
        foreach ($g in $now.Groups.Values) {
            if (-not $sa.Groups.ContainsKey($g.Key) -and -not (Is-Water $g.Draws[0])) {
                [void]$tests.Add(@{ Name = "sin $($g.Key) (solo aparece al pisar)"; Lines = @("vs $($g.Vs)"); Restore = @() })
            }
        }
    }
    if (-not ($tests | Where-Object { $_.Name -like "bool b229*" })) {
        [void]$tests.Add(@{ Name = "bool b229 a 0 (ondas de pisadas apagadas)"; Lines = @("bool 229 0"); Restore = @() })
    }
    foreach ($c in $CvarTests) {
        [void]$tests.Add(@{ Name = "$($c[0])=$($c[1])"; Lines = @("cvar $($c[0])=$($c[1])"); Restore = @("cvar $($c[0])=$($c[2])") })
    }
    Note ("Pruebas: {0}. El jugador tiene que estar QUIETO dentro del agua, sin mover la camara." -f $tests.Count)
    # References: two normal screenshots (noise) and one without the water draw.
    Set-Control $session $base
    Start-Sleep -Seconds $Settle
    $n1 = Capture $folder "00_normal_1"
    Start-Sleep -Milliseconds 700
    $n2 = Capture $folder "00_normal_2"
    $noise = Diff $n1 $n2
    if ($skipWater.Count) {
        Set-Control $session (@($base) + $skipWater)
        Start-Sleep -Seconds $Settle
        $without = Capture $folder "00_sin_agua"
        Set-Control $session $base
    } else { $without = $n1 }
    $gone = Diff $n1 $without
    Note ("Referencias: entre dos capturas normales cambia {0} % (ruido); normal contra sin-agua {1} % (el agua que se ve ahora)" -f $noise, $gone)
    $limit = [math]::Max($noise, $gone) * 2 + 2
    $index = 1
    foreach ($t in $tests) {
        Set-Control $session (@($base) + $t.Lines)
        Start-Sleep -Seconds $Settle
        $shot = Capture $folder ("{0:D2}_{1}" -f $index, ($t.Name -replace "[^A-Za-z0-9_=-]+", "_"))
        $d = Diff $shot $without
        $verdict = if ($d -gt $limit) { "EL AGUA VUELVE A VERSE" } else { "nada" }
        Note ("{0:D2}. {1}: {2} % distinto de sin-agua -> {3}" -f $index, $t.Name, $d, $verdict)
        if ($t.Restore.Count) { Set-Control $session (@($base) + $t.Restore); Start-Sleep -Milliseconds 1200 }
        $index++
    }
    Set-Control $session $base
    Note ("Capturas y registro en {0}" -f $folder)
}
"texturas" {
    $session = Get-Session $false
    $label = if ($Nombre) { $Nombre } else { "texturas" }
    $folder = Join-Path $session ("{0}_{1}" -f $label, (Get-Date -Format "HHmmss"))
    New-Item -ItemType Directory -Force $folder | Out-Null
    $base = Base-Lines $session
    & adb -s $Device shell "rm -f $Remote/xnative_tex_*.bin $Remote/xnative_tex_done.txt" | Out-Null
    $shot = Capture $folder "captura"
    $request = [int]([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() % 100000000)
    Set-Control $session (@($base) + @("tex 1", "snap $request"))
    $snapOk = $false; $manifest = @()
    for ($i = 0; $i -lt 75; $i++) {
        Start-Sleep -Milliseconds 400
        if (-not $snapOk) {
            $text = Get-Remote "cat $Remote/xnative_watch_snap.txt 2>/dev/null"
            if ($text.Count -gt 0 -and ($text | Select-Object -Last 1).Trim() -eq "end $request") {
                [IO.File]::WriteAllLines((Join-Path $folder "snap.txt"), [string[]]$text)
                $snapOk = $true
            }
        }
        $done = Get-Remote "cat $Remote/xnative_tex_done.txt 2>/dev/null"
        if ($done.Count -gt 0 -and ($done | Select-Object -Last 1).Trim() -eq "end $request") {
            $manifest = @($done | Where-Object { $_ -match "^xnative_tex_" }); break
        }
    }
    Set-Control $session $base
    if (-not $snapOk) { Write-Output "AVISO: la instantanea no llego (el juego esta en partida y el agua a la vista?)" }
    if ($manifest.Count -eq 0) { throw "no llego ningun volcado de texturas (APK con 'tex 1'? el agua se dibuja?)" }
    [IO.File]::WriteAllLines((Join-Path $folder "volcados.txt"), [string[]]$manifest)
    foreach ($m in $manifest) {
        $name = ($m -split " ")[0]
        & adb -s $Device pull "$Remote/$name" (Join-Path $folder $name) | Out-Null
    }
    & adb -s $Device shell "rm -f $Remote/xnative_tex_*.bin" | Out-Null
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.IO;
using System.Text;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class TexDump {
    static float Half(int h) {
        int s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
        float v;
        if (e == 0) v = (float)(m * Math.Pow(2, -24));
        else if (e == 31) v = m != 0 ? float.NaN : float.PositiveInfinity;
        else v = (float)((1 + m / 1024.0) * Math.Pow(2, e - 15));
        return s != 0 ? -v : v;
    }
    static float Small(int bits, int mantissa) {
        int e = bits >> mantissa, m = bits & ((1 << mantissa) - 1);
        if (e == 0) return (float)(m * Math.Pow(2, -14 - mantissa));
        if (e == 31) return m != 0 ? float.NaN : float.PositiveInfinity;
        return (float)((1 + m / (double)(1 << mantissa)) * Math.Pow(2, e - 15));
    }
    static void Save(string path, int w, int h, Func<int, int, int> rgb) {
        using (Bitmap bitmap = new Bitmap(w, h, PixelFormat.Format24bppRgb)) {
            BitmapData data = bitmap.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
            byte[] pixels = new byte[data.Stride * h];
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    int c = rgb(x, y), o = y * data.Stride + x * 3;
                    pixels[o] = (byte)(c & 255); pixels[o + 1] = (byte)((c >> 8) & 255); pixels[o + 2] = (byte)((c >> 16) & 255);
                }
            Marshal.Copy(pixels, 0, data.Scan0, pixels.Length);
            bitmap.UnlockBits(data);
            bitmap.Save(path, ImageFormat.Png);
        }
    }
    // Returns lines: "info ..." then one "cN min max mean nonfinite p_low p_mid p_high" per channel.
    public static string[] Convert(string path, string prefix) {
        byte[] all = File.ReadAllBytes(path);
        if (all.Length < 64 || BitConverter.ToUInt32(all, 0) != 0x58455458u) return new[] { "error no es XTEX" };
        uint[] hd = new uint[16];
        for (int i = 0; i < 16; i++) hd[i] = BitConverter.ToUInt32(all, i * 4);
        int vk = (int)hd[4], w = (int)hd[7], h = (int)hd[8], bpt = (int)hd[9];
        int n = w * h, off = 64;
        if (all.Length < off + (long)n * bpt) return new[] { "error archivo corto" };
        int channels; bool unit = true; float[][] c;
        Func<int, int, float> rd8 = (i, k) => all[off + i * bpt + k] / 255f;
        Func<int, int, float> rs8 = (i, k) => Math.Max((sbyte)all[off + i * bpt + k] / 127f, -1f);
        Func<int, int, int> u16 = (i, k) => BitConverter.ToUInt16(all, off + i * bpt + k * 2);
        Func<int, int, float> f32 = (i, k) => BitConverter.ToSingle(all, off + i * bpt + k * 4);
        Func<int, uint> u32 = i => BitConverter.ToUInt32(all, off + i * bpt);
        switch (vk) {
            case 9: case 16: case 37: case 43: case 44: case 50:
                channels = vk == 9 ? 1 : vk == 16 ? 2 : 4; break;
            case 10: case 17: case 38: channels = vk == 10 ? 1 : vk == 17 ? 2 : 4; unit = false; break;
            case 76: case 83: case 97: channels = vk == 76 ? 1 : vk == 83 ? 2 : 4; unit = false; break;
            case 70: case 77: case 91: channels = vk == 70 ? 1 : vk == 77 ? 2 : 4; break;
            case 100: case 126: case 130: channels = 1; unit = false; break;
            case 103: case 109: channels = vk == 103 ? 2 : 4; unit = false; break;
            case 124: case 125: case 129: channels = 1; break;
            case 64: case 122: channels = vk == 64 ? 4 : 3; unit = vk == 64; break;
            default: return new[] { "error formato vk" + vk + " sin decodificar" };
        }
        c = new float[channels][];
        for (int k = 0; k < channels; k++) c[k] = new float[n];
        for (int i = 0; i < n; i++) {
            for (int k = 0; k < channels; k++) {
                float v;
                switch (vk) {
                    case 9: case 16: case 37: case 43: case 50: v = rd8(i, k); break;
                    case 44: v = rd8(i, k < 3 ? 2 - k : 3); break;
                    case 10: case 17: case 38: v = rs8(i, k); break;
                    case 76: case 83: case 97: v = Half(u16(i, k)); break;
                    case 70: case 77: case 91: v = u16(i, k) / 65535f; break;
                    case 100: case 126: case 130: case 103: case 109: v = f32(i, k); break;
                    case 124: v = u16(i, 0) / 65535f; break;
                    case 125: case 129: v = (u32(i) & 0xFFFFFF) / 16777215f; break;
                    case 64: { uint p = u32(i); v = k < 3 ? ((p >> (10 * k)) & 1023) / 1023f : (p >> 30) / 3f; break; }
                    default: { uint p = u32(i); v = k == 0 ? Small((int)(p & 2047), 6) : k == 1 ? Small((int)((p >> 11) & 2047), 6) : Small((int)(p >> 22), 5); break; }
                }
                c[k][i] = v;
            }
        }
        StringBuilder text = new StringBuilder();
        var lines = new System.Collections.Generic.List<string>();
        lines.Add(string.Format("info handle={0:X8} vk={1} {2}x{3} mip={4} frame={5} cmd={6} fmt={7} swz={8}",
                                hd[2], vk, w, h, hd[6], hd[10], hd[11], hd[12], System.Convert.ToString(hd[13], 8).PadLeft(4, '0')));
        float[] lo = new float[channels], hi = new float[channels];
        for (int k = 0; k < channels; k++) {
            double sum = 0; int bad = 0, low = 0, mid = 0, high = 0, ok = 0;
            float mn = float.MaxValue, mx = float.MinValue;
            for (int i = 0; i < n; i++) {
                float v = c[k][i];
                if (float.IsNaN(v) || float.IsInfinity(v)) { bad++; continue; }
                ok++; sum += v; if (v < mn) mn = v; if (v > mx) mx = v;
                if (v < 0.45f) low++; else if (v <= 0.55f) mid++; else high++;
            }
            if (ok == 0) { mn = 0; mx = 0; }
            lo[k] = mn; hi[k] = mx;
            lines.Add(string.Format(System.Globalization.CultureInfo.InvariantCulture,
                "c{0} {1:0.#####} {2:0.#####} {3:0.#####} {4} {5:0.#} {6:0.#} {7:0.#}", k, mn, mx, ok > 0 ? sum / ok : 0, bad,
                100.0 * low / n, 100.0 * mid / n, 100.0 * high / n));
        }
        // Pictures: values in 0..1 as they are, others stretched from min to max.
        Func<int, int, int> level = (k, i) => {
            float v = c[k][i];
            if (float.IsNaN(v) || float.IsInfinity(v)) return 255;
            float t = (unit || (lo[k] >= 0 && hi[k] <= 1)) ? v : (hi[k] > lo[k] ? (v - lo[k]) / (hi[k] - lo[k]) : 0.5f);
            return Math.Max(0, Math.Min(255, (int)Math.Round(t * 255)));
        };
        int shown = Math.Min(channels, 3);
        Save(prefix + ".png", w, h, (x, y) => {
            int i = y * w + x;
            int r = level(0, i), g = shown > 1 ? level(1, i) : r, b = shown > 2 ? level(2, i) : (shown > 1 ? 0 : r);
            return (r << 16) | (g << 8) | b;
        });
        if (channels > 1)
            for (int k = 0; k < channels; k++) {
                int kk = k;
                Save(prefix + "_c" + k + ".png", w, h, (x, y) => { int v = level(kk, y * w + x); return (v << 16) | (v << 8) | v; });
            }
        return lines.ToArray();
    }
}
"@
    # Which handle the water's vertex shader reads at t21 (the ripple map) and its PS at t1.
    $rippleHandle = ""; $rippleSwz = ""
    if ($snapOk) {
        $snapText = Get-Content (Join-Path $folder "snap.txt")
        $waterLine = $snapText | Where-Object { $_ -match "^D .*vs=55002F08" } | Select-Object -First 1
        if ($waterLine -and $waterLine -match " t21=([0-9A-F]{8}):") { $rippleHandle = $Matches[1] }
        $t21Detail = $snapText | Where-Object { $_ -match "^\s+t21 handle=" } | Select-Object -First 1
        if ($t21Detail -and $t21Detail -match " swz=(\d+) ") { $rippleSwz = $Matches[1] }
    }
    $report = New-Object System.Collections.ArrayList
    function Say([string]$text) { [void]$report.Add($text); Write-Output $text }
    Say ("Volcados de texturas ({0}) en {1}" -f $manifest.Count, $folder)
    foreach ($file in (Get-ChildItem $folder -Filter "xnative_tex_*.bin" | Sort-Object Name)) {
        $prefix = Join-Path $folder ([IO.Path]::GetFileNameWithoutExtension($file.Name))
        $lines = [TexDump]::Convert($file.FullName, $prefix)
        if ($lines[0] -like "error*") { Say ("  {0}: {1}" -f $file.Name, $lines[0]); continue }
        Say ("  {0}" -f $lines[0].Substring(5))
        foreach ($l in $lines[1..($lines.Count - 1)]) {
            $p = $l -split " "
            Say ("     {0}: min {1} max {2} media {3} no-finitos {4} | <0.45 {5}% , 0.45-0.55 {6}% , >0.55 {7}%" -f $p[0], $p[1], $p[2], $p[3], $p[4], $p[5], $p[6], $p[7])
        }
        if ($rippleHandle -and $lines[0] -match "handle=$rippleHandle " -and $lines[0] -match "vk=37 ") {
            $x = if ($rippleSwz) { [Convert]::ToInt32($rippleSwz, 8) -band 7 } else { 0 }
            $stats = ($lines | Where-Object { $_ -like "c$x *" } | Select-Object -First 1) -split " "
            if ($stats.Count -ge 8) {
                $mean = [double]::Parse($stats[3], [Globalization.CultureInfo]::InvariantCulture)
                $neutral = [double]::Parse($stats[6], [Globalization.CultureInfo]::InvariantCulture)
                $h = 2 * $mean - 1
                Say ("     -> MAPA DE ONDAS que lee el vertex shader del agua (t21, lectura .x = c{0}): media {1:0.###} => altura h = {2:0.###} (0 = sin desplazar); {3}% de texeles neutros (0.45-0.55)" -f $x, $mean, $h, $neutral)
                if ([math]::Abs($h) -gt 0.2) {
                    Say ("        NO NEUTRO: el agua se desplaza unos {0:0.###} unidades (0.1*h) en su normal: con h<0 se hunde bajo el suelo y desaparece" -f (0.1 * $h))
                } else {
                    Say "        neutro: este mapa no hunde el agua (mirar el resto: datos por vertice, ondas del pixel shader)"
                }
            }
        }
    }
    [IO.File]::WriteAllLines((Join-Path $folder "informe.txt"), [string[]]$report)
    Write-Output ("Informe: {0}" -f (Join-Path $folder "informe.txt"))
}
"parar" {
    & adb -s $Device shell rm -f "$Remote/xnative_skip.txt"
    Write-Output "Control en vivo quitado: el juego vuelve a lo normal (las opciones cambiadas con cvar siguen hasta reiniciar el juego)."
}
}
