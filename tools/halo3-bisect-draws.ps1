# Finds the shader whose draws make a visible artifact (Halo 3 "stretched sheets"), on the phone,
# without restarting the game: the player stands still looking at the artifact, and this script
# leaves out halves of the frame's vertex (or pixel) shaders through the renderer's live skip file
# (graphics_system.cpp, xnative_skip.txt in the app's files folder), takes a screenshot of each
# half, and keeps the half whose removal takes the streaks away, down to one shader.
#
# The measure: share of 32x32-pixel blocks crossed by long parallel streaks (strong gradients in a
# single direction: structure tensor coherence). Every screenshot is kept so a person can check.
#
#   powershell -ExecutionPolicy Bypass -File tools\halo3-bisect-draws.ps1
#   ... -PixelShaders            bisect pixel shaders instead of vertex shaders
#   ... -Image <png>             only measure one screenshot (to check the measure)
#   ... -Roi "0.10,0.05,0.90,0.95"   region measured (fractions of width and height)
param(
    [string]$Device = $env:ANDROID_SERIAL,
    [string]$Package = "com.xport.halo3",
    [string]$Out = "",
    [double]$Settle = 3.0,
    [string]$Roi = "0.10,0.05,0.90,0.95",
    [double]$Threshold = 0.6,
    [double]$MinCoherence = 0.75,
    [double]$MinEnergy = 25,
    [switch]$PixelShaders,
    [switch]$Each,
    [string]$Exclude = "",
    [string]$Candidates = "",
    [string]$Reference = "",
    [string]$Image = ""
)
# Continue: Windows PowerShell turns adb's stderr lines into errors ("No such file" while the game
# has not written its list yet); failures are checked with $LASTEXITCODE and thrown explicitly.
$ErrorActionPreference = "Continue"

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class SheetMeter {
    // Share of the reference's streaky blocks that are still streaky, in the same direction
    // (within 20 degrees), in another screenshot: the reference's streaks still on screen.
    public static double Persist(double[] reference, double[] current) {
        int streaky = 0, kept = 0;
        for (int i = 0; i + 1 < reference.Length && i + 1 < current.Length; i += 2) {
            if (reference[i] < 0) continue;
            streaky++;
            if (current[i] < 0) continue;
            double difference = Math.Abs(reference[i] - current[i]) % Math.PI;
            if (difference > Math.PI / 2) difference = Math.PI - difference;
            if (difference < Math.PI / 9) kept++;
        }
        return streaky > 0 ? (double)kept / streaky : 0;
    }
    // Per block: {streak angle in radians, or -1 when the block is not streaky, energy}.
    public static double[] Blocks(string path, double fx0, double fy0, double fx1, double fy1,
                                  double minCoherence, double minEnergy) {
        double[] m = Run(path, fx0, fy0, fx1, fy1, minCoherence, minEnergy, true);
        return m;
    }
    // {streaky blocks, textured blocks (enough gradient energy), measured blocks}
    public static double[] Measure(string path, double fx0, double fy0, double fx1, double fy1,
                                   double minCoherence, double minEnergy) {
        return Run(path, fx0, fy0, fx1, fy1, minCoherence, minEnergy, false);
    }
    static double[] Run(string path, double fx0, double fy0, double fx1, double fy1,
                        double minCoherence, double minEnergy, bool perBlock) {
        System.Collections.Generic.List<double> blocks = new System.Collections.Generic.List<double>();
        using (Bitmap bitmap = new Bitmap(path)) {
            int w = bitmap.Width, h = bitmap.Height;
            BitmapData data = bitmap.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly,
                                              PixelFormat.Format24bppRgb);
            int stride = data.Stride;
            byte[] pixels = new byte[stride * h];
            Marshal.Copy(data.Scan0, pixels, 0, pixels.Length);
            bitmap.UnlockBits(data);
            // Half resolution luminance.
            int sw = w / 2, sh = h / 2;
            float[] g = new float[sw * sh];
            for (int y = 0; y < sh; y++) {
                for (int x = 0; x < sw; x++) {
                    float sum = 0;
                    for (int dy = 0; dy < 2; dy++) {
                        for (int dx = 0; dx < 2; dx++) {
                            int o = (2 * y + dy) * stride + (2 * x + dx) * 3;
                            sum += 0.114f * pixels[o] + 0.587f * pixels[o + 1] + 0.299f * pixels[o + 2];
                        }
                    }
                    g[y * sw + x] = sum * 0.25f;
                }
            }
            const int block = 16;
            int x0 = Math.Max(1, (int)(fx0 * sw)), y0 = Math.Max(1, (int)(fy0 * sh));
            int x1 = Math.Min(sw - 1, (int)(fx1 * sw)), y1 = Math.Min(sh - 1, (int)(fy1 * sh));
            int total = 0, streaky = 0, textured = 0;
            for (int by = y0; by + block <= y1; by += block) {
                for (int bx = x0; bx + block <= x1; bx += block) {
                    double jxx = 0, jyy = 0, jxy = 0;
                    for (int y = by; y < by + block; y++) {
                        for (int x = bx; x < bx + block; x++) {
                            float a = g[(y - 1) * sw + x - 1], b = g[(y - 1) * sw + x], c = g[(y - 1) * sw + x + 1];
                            float d = g[y * sw + x - 1], f = g[y * sw + x + 1];
                            float p = g[(y + 1) * sw + x - 1], q = g[(y + 1) * sw + x], r = g[(y + 1) * sw + x + 1];
                            double gx = (c + 2 * f + r) - (a + 2 * d + p);
                            double gy = (p + 2 * q + r) - (a + 2 * b + c);
                            jxx += gx * gx; jyy += gy * gy; jxy += gx * gy;
                        }
                    }
                    total++;
                    double energy = (jxx + jyy) / (block * block);
                    double angle = -1;
                    if (energy >= minEnergy) {
                        textured++;
                        double coherence = Math.Sqrt((jxx - jyy) * (jxx - jyy) + 4 * jxy * jxy) / (jxx + jyy);
                        if (coherence >= minCoherence) {
                            streaky++;
                            angle = 0.5 * Math.Atan2(2 * jxy, jxx - jyy) + Math.PI;
                        }
                    }
                    blocks.Add(angle);
                    blocks.Add(energy);
                }
            }
            if (perBlock) return blocks.ToArray();
            return new double[] { streaky, textured, total };
        }
    }
}
"@

$roiParts = $Roi.Split(",") | ForEach-Object { [double]$_ }
# {Streaks: % of the textured blocks crossed by streaks, Drawn: % of the blocks that are textured}.
# Leaving out half the scene empties part of the screen: Streaks ignores the empty blocks, and a
# screenshot that keeps too little of the reference's texture does not decide anything.
function Measure-Sheets([string]$path) {
    $m = [SheetMeter]::Measure($path, $roiParts[0], $roiParts[1], $roiParts[2], $roiParts[3],
                               $MinCoherence, $MinEnergy)
    $streaks = if ($m[1] -gt 0) { [math]::Round($m[0] / $m[1] * 100, 1) } else { 0 }
    $drawn = if ($m[2] -gt 0) { [math]::Round($m[1] / $m[2] * 100, 1) } else { 0 }
    return [pscustomobject]@{ Streaks = $streaks; Drawn = $drawn }
}

if ($Image) {
    $score = Measure-Sheets (Resolve-Path $Image).Path
    Write-Output ("{0}: rayas {1} % de la zona con dibujo (con dibujo {2} %)" -f $Image, $score.Streaks, $score.Drawn)
    if ($Reference) {
        $r = [SheetMeter]::Blocks((Resolve-Path $Reference).Path, $roiParts[0], $roiParts[1], $roiParts[2],
                                  $roiParts[3], $MinCoherence, $MinEnergy)
        $c = [SheetMeter]::Blocks((Resolve-Path $Image).Path, $roiParts[0], $roiParts[1], $roiParts[2],
                                  $roiParts[3], $MinCoherence, $MinEnergy)
        Write-Output ("  quedan {0} % de las rayas de {1}" -f [math]::Round([SheetMeter]::Persist($r, $c) * 100, 1), $Reference)
    }
    exit 0
}

$remote = "/sdcard/Android/data/$Package/files"
if (-not $Out) {
    $Out = Join-Path (Join-Path $PSScriptRoot "..\work\logs\capturas") ("bisect_" + (Get-Date -Format "HHmmss"))
}
New-Item -ItemType Directory -Force $Out | Out-Null
$logPath = Join-Path $Out "bisect.txt"
function Log([string]$text) {
    Write-Output $text
    Add-Content -Path $logPath -Value $text -Encoding UTF8
}

function Set-Skip([string[]]$lines) {
    $local = Join-Path $Out "xnative_skip.txt"
    [IO.File]::WriteAllText($local, (($lines -join "`n") + "`n"))
    & adb -s $Device push $local "$remote/xnative_skip.txt" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "adb push failed" }
}

# A screenshot's blocks, and how many of the reference's streaks it keeps (% Kept; the reference
# itself sets $script:baseBlocks).
$script:baseBlocks = $null
function Capture([string]$name) {
    & adb -s $Device shell screencap -p /sdcard/xnative_bisect.png
    $path = Join-Path $Out "$name.png"
    & adb -s $Device pull /sdcard/xnative_bisect.png $path | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "adb pull failed" }
    $score = Measure-Sheets $path
    $blocks = [SheetMeter]::Blocks($path, $roiParts[0], $roiParts[1], $roiParts[2], $roiParts[3],
                                   $MinCoherence, $MinEnergy)
    if (-not $script:baseBlocks) { $script:baseBlocks = $blocks }
    $kept = [math]::Round([SheetMeter]::Persist($script:baseBlocks, $blocks) * 100, 1)
    $score | Add-Member -NotePropertyName Kept -NotePropertyValue $kept
    return $score
}

function Skip-Lines([string[]]$hashes) {
    $kind = if ($PixelShaders) { "ps" } else { "vs" }
    return @($hashes | ForEach-Object { "$kind $_" })
}

# 1. The shaders drawn in the current frame (or the -Candidates list, to continue a search).
$kind = if ($PixelShaders) { "ps" } else { "vs" }
$excluded = @($Exclude.Split(",") | Where-Object { $_ } | ForEach-Object { $_.Trim().ToUpper() })
if ($Candidates) {
    $list = @(($Candidates -split "[,; ]+") | Where-Object { $_ } | ForEach-Object { $_.Trim().ToUpper() })
} else {
    $request = [int]([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() % 1000000)
    Set-Skip @("dump $request")
    $drawn = $null
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 700
        $text = & adb -s $Device shell "cat $remote/xnative_drawn.txt 2>/dev/null"
        if ($text -and ($text | Select-Object -Last 1) -eq "end $request") { $drawn = $text; break }
    }
    if (-not $drawn) { throw "the game wrote no xnative_drawn.txt (is the liveskip APK running, in game?)" }
    $drawn | Set-Content -Path (Join-Path $Out "drawn.txt") -Encoding UTF8
    $list = @($drawn | Where-Object { $_ -like "$kind *" } | ForEach-Object { ($_ -split " ")[1] })
}
$list = @($list | Where-Object { $excluded -notcontains $_ })
Log ("Shaders {0} candidatos: {1}" -f $kind, $list.Count)
if ($list.Count -lt 1) { throw "no shader to search" }

# 2. Reference: nothing left out. Its streaky blocks are the ones followed afterwards.
Set-Skip @()
Start-Sleep -Seconds $Settle
$base = Capture "00_base"
Log ("Referencia (nada apagado): rayas {0} % (con dibujo {1} %)" -f $base.Streaks, $base.Drawn)

# A half holds the culprit when leaving it out keeps few of the reference's streaks (Kept) while
# leaving out the other half keeps clearly more.
# -Each: leave out each candidate alone (one screenshot each) instead of halves.
if ($Each) {
    foreach ($hash in $list) {
        Set-Skip (Skip-Lines @($hash))
        Start-Sleep -Seconds $Settle
        $score = Capture ("solo_sin_{0}" -f $hash)
        Log ("Sin {0}: quedan {1} % de las rayas, dibujo {2} %" -f $hash, $score.Kept, $score.Drawn)
    }
    Set-Skip @()
    Log ("Capturas y registro en: {0}" -f $Out)
    exit 0
}

$found = $true
$step = 1
while ($list.Count -gt 1) {
    $half = [math]::Ceiling($list.Count / 2)
    $a = @($list[0..($half - 1)])
    $b = @($list[$half..($list.Count - 1)])
    Set-Skip (Skip-Lines $a)
    Start-Sleep -Seconds $Settle
    $scoreA = Capture ("{0:D2}_A_sin_{1}" -f $step, $a.Count)
    Set-Skip (Skip-Lines $b)
    Start-Sleep -Seconds $Settle
    $scoreB = Capture ("{0:D2}_B_sin_{1}" -f $step, $b.Count)
    Log ("Paso {0}: sin A ({1}) quedan {2} % de las rayas, dibujo {3} % | sin B ({4}) quedan {5} %, dibujo {6} %" -f
         $step, $a.Count, $scoreA.Kept, $scoreA.Drawn, $b.Count, $scoreB.Kept, $scoreB.Drawn)
    # A screenshot left blank (a full-screen pass left out: white or black screen) has no streaks
    # either: it decides nothing. Such a shader goes in -Exclude.
    $blankA = $scoreA.Drawn -lt $base.Drawn * 0.5
    $blankB = $scoreB.Drawn -lt $base.Drawn * 0.5
    if ($blankA -or $blankB) {
        Log ("  la mitad {0} deja la pantalla vacia (una pasada de pantalla completa): ponerla en -Exclude" -f $(if ($blankA) { "A" } else { "B" }))
    }
    if (-not $blankA -and $scoreA.Kept -le 40 -and $scoreA.Kept -le $scoreB.Kept * 0.6) {
        $list = $a
    } elseif (-not $blankB -and $scoreB.Kept -le 40 -and $scoreB.Kept -le $scoreA.Kept * 0.6) {
        $list = $b
    } else {
        Log ("Paso {0}: ninguna mitad quita claramente las rayas: hacen falta dibujos de las dos, o la medida no las ve. Revisar las capturas; quedan: {1}" -f $step, ($list -join ","))
        $found = $false
        break
    }
    Log ("  quedan {0}: {1}" -f $list.Count, ($list -join ","))
    $step++
}

# 3. Check the result alone, then restore everything.
if ($found) {
    Set-Skip (Skip-Lines $list)
    Start-Sleep -Seconds $Settle
    $final = Capture "99_solo_sin_resultado"
    Log ("Resultado: {0} {1} (sin el quedan {2} % de las rayas)" -f $kind, ($list -join ","), $final.Kept)
}
Set-Skip @()
Start-Sleep -Seconds $Settle
$after = Capture "99_todo_otra_vez"
Log ("Con todo otra vez: quedan {0} % de las rayas" -f $after.Kept)
Log ("Capturas y registro en: {0}" -f $Out)
