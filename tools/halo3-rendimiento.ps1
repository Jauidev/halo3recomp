# Halo 3 phone performance: what the running game spends its time on, without restarting it.
#
# Steps (-Paso):
#   medir     -Segundos N (default 12): per-thread CPU (% of one core, from /proc tick deltas),
#             game fps (the VblankThread log lines), page faults that hit storage (swap), memory,
#             core frequencies and temperatures. Writes a report to the session folder.
#   gpu       the renderer's own profiler (cvars xport_profile_native_*, set live through
#             xnative_skip.txt): N sampled frames with GPU time per pass and CPU time per phase,
#             pulled from files/xnative/Diagnostics and summarised. Once per game process.
#   perf      simpleperf --app (the shell cannot attach with -p, and this phone has no cycle
#             counter: cpu-clock) for -Segundos: hottest functions per thread.
#   relanzar  CLOSES THE GAME and starts it again with the native profiler category on
#             (XNATIVE_DIAG=logging,native-profiler) and its autostart off, so "gpu" can arm the
#             one capture of the process later, in the level. The diagnostics categories are
#             read once at startup: "gpu" does nothing in a game launched without this.
#   ab        live A/B of what costs fps: the base, then each -Quitar line alone in
#             xnative_skip.txt ("vs <hash>", "ps <hash>", "cvar name=value"...), then the base again,
#             -Segundos of VblankThread readings each (the player stands still meanwhile).
#
#   powershell -ExecutionPolicy Bypass -File tools\halo3-rendimiento.ps1 -Paso medir
param(
    [ValidateSet("medir", "gpu", "perf", "relanzar", "ab")]
    [string]$Paso = "medir",
    [int]$Segundos = 12,
    [int]$Muestras = 120,
    [string[]]$Quitar = @(),
    [string]$Device = $env:ANDROID_SERIAL,
    [string]$Package = "com.xport.halo3"
)
$ErrorActionPreference = "Continue"
$Root = (Join-Path $PSScriptRoot "..\work\logs\rendimiento")
$Session = Join-Path $Root (Get-Date -Format "yyyyMMdd_HHmmss")
New-Item -ItemType Directory -Force $Session | Out-Null
$Remote = "/sdcard/Android/data/$Package/files"

function Sh([string]$command) { return @(& adb -s $Device shell $command) }
if ($Paso -eq "relanzar") {
    # runtime/os/android_app.cpp sets these XNATIVE_CVARS before the intent's env list, which
    # replaces the whole variable: repeat them and add the autostart switch.
    $cvars = "xnative_merge_up_max=64,xnative_merge_indexed_draws=true,xnative_early_fragment_tests=false," +
             "xnative_fs_relaxed_precision=false,xnative_lazy_resolves=true,xnative_resolve_swap=true," +
             "xnative_gpu_detile=true,xnative_pipelined_recording=true,xnative_async_command_retire=true," +
             "xport_profile_native_autostart=false"
    $envList = "XNATIVE_DIAG=logging,native-profiler XNATIVE_CVARS=$cvars"
    & adb -s $Device shell am force-stop $Package | Out-Null
    # A control file left behind would set the capture trigger at the first frame and spend the
    # process's one capture on the loading screen.
    & adb -s $Device shell rm -f "$Remote/xnative_skip.txt" | Out-Null
    Start-Sleep -Seconds 2
    & adb -s $Device shell "am start -n $Package/com.xport.MainActivity --es env '$envList'" | Out-Null
    Write-Output "Juego relanzado con el perfilador. Entra en la partida, ve a la zona y luego: -Paso gpu"
    exit 0
}
$GamePid = ((Sh "pidof $Package") -join "").Trim()
if (-not $GamePid) { throw "el juego no esta abierto" }

$report = New-Object System.Collections.ArrayList
function Say([string]$text) { [void]$report.Add($text); Write-Output $text }

# One snapshot of every thread: "tid utime stime majflt cpu name" (fields after the ")" of stat).
$SnapshotScript = 'for t in /proc/' + $GamePid + '/task/*; do s=$(cat $t/stat 2>/dev/null) || continue; r=${s##*) }; set -- $r; echo "${t##*/} ${12} ${13} ${10} ${37} $(cat $t/comm)"; done'
function Thread-Snapshot {
    $map = @{}
    foreach ($line in (Sh $SnapshotScript)) {
        $p = $line.Trim() -split " ", 6
        if ($p.Count -lt 6) { continue }
        $map[$p[0]] = @{ Tid = $p[0]; Ticks = [int64]$p[1] + [int64]$p[2]; Major = [int64]$p[3]; Cpu = $p[4]; Name = $p[5] }
    }
    return $map
}

switch ($Paso) {
"medir" {
    Say ("== Rendimiento {0} (pid {1}, {2} s) ==" -f (Get-Date -Format "HH:mm:ss"), $GamePid, $Segundos)
    $a = Thread-Snapshot
    $t0 = Get-Date
    $logStart = (Get-Date).ToString("MM-dd HH:mm:ss.000")
    Start-Sleep -Seconds $Segundos
    $b = Thread-Snapshot
    $elapsed = ((Get-Date) - $t0).TotalSeconds
    # Game fps: the VblankThread lines of the window.
    $fpsLines = @(& adb -s $Device logcat -d --pid=$GamePid -T $logStart 2>$null | Select-String -Pattern "VblankThread: \d+ swaps \(([\d.]+) fps\)")
    $fps = @($fpsLines | ForEach-Object { [double]::Parse($_.Matches[0].Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture) })
    if ($fps.Count) {
        $avg = ($fps | Measure-Object -Average).Average
        Say ("FPS del juego: media {0:0.0}, min {1:0.0}, max {2:0.0} ({3} lecturas): {4}" -f $avg, ($fps | Measure-Object -Minimum).Minimum, ($fps | Measure-Object -Maximum).Maximum, $fps.Count, (($fps | ForEach-Object { "{0:0.#}" -f $_ }) -join " "))
    } else { Say "FPS del juego: sin lineas VblankThread en la ventana" }
    # Threads: % of one core over the window (100 ticks per second).
    $rows = foreach ($tid in $b.Keys) {
        if (-not $a.ContainsKey($tid)) { continue }
        $pct = 100.0 * ($b[$tid].Ticks - $a[$tid].Ticks) / (100.0 * $elapsed)
        $major = $b[$tid].Major - $a[$tid].Major
        [pscustomobject]@{ Hilo = $b[$tid].Name; Tid = $tid; Cpu = [math]::Round($pct, 1); Nucleo = $b[$tid].Cpu; FallosDisco = $major }
    }
    $rows = @($rows | Sort-Object Cpu -Descending)
    $total = ($rows | Measure-Object Cpu -Sum).Sum
    Say ("CPU total del juego: {0:0} % de un nucleo ({1:0.0} nucleos de 8)" -f $total, ($total / 100))
    Say "Hilos (% de un nucleo; 100 = un nucleo entero ocupado):"
    foreach ($r in ($rows | Where-Object { $_.Cpu -ge 1 -or $_.FallosDisco -gt 0 } | Select-Object -First 24)) {
        Say ("  {0,-22} {1,6:0.0} %  nucleo {2}  tid {3}{4}" -f $r.Hilo, $r.Cpu, $r.Nucleo, $r.Tid, $(if ($r.FallosDisco) { "  fallos de pagina a disco/swap: $($r.FallosDisco)" } else { "" }))
    }
    $majorTotal = ($rows | Measure-Object FallosDisco -Sum).Sum
    Say ("Fallos de pagina que leen de disco/swap: {0} en {1:0} s ({2:0} por segundo)" -f $majorTotal, $elapsed, ($majorTotal / $elapsed))
    # Memory.
    $mem = Sh "cat /proc/meminfo" | Where-Object { $_ -match "^(MemTotal|MemAvailable|SwapTotal|SwapFree):" }
    Say ("Memoria del movil: {0}" -f (($mem | ForEach-Object { ($_ -replace "\s+", " ").Trim() }) -join ", "))
    $status = Sh "cat /proc/$GamePid/status" | Where-Object { $_ -match "^(VmRSS|VmSwap):" }
    Say ("Memoria del juego: {0}" -f (($status | ForEach-Object { ($_ -replace "\s+", " ").Trim() }) -join ", "))
    # Cores and temperatures.
    $freqs = Sh 'for c in /sys/devices/system/cpu/cpu[0-9]*; do echo "${c##*/} $(cat $c/cpufreq/scaling_cur_freq 2>/dev/null) $(cat $c/cpufreq/cpuinfo_max_freq 2>/dev/null)"; done'
    Say ("Frecuencias (MHz actual/max): {0}" -f (($freqs | ForEach-Object { $p = $_.Trim() -split " "; if ($p.Count -ge 3 -and $p[1]) { "{0} {1:0}/{2:0}" -f $p[0], ([double]$p[1] / 1000), ([double]$p[2] / 1000) } }) -join ", "))
    $temps = Sh 'for z in /sys/class/thermal/thermal_zone*; do t=$(cat $z/temp 2>/dev/null) && echo "$(cat $z/type) $t"; done'
    $hot = @($temps | ForEach-Object { $p = $_.Trim() -split " "; if ($p.Count -eq 2 -and $p[1] -match "^\d+$" -and [int]$p[1] -gt 1000) { [pscustomobject]@{ Zona = $p[0]; C = [int]$p[1] / 1000.0 } } } | Sort-Object C -Descending | Select-Object -First 6)
    if ($hot.Count) { Say ("Temperaturas mas altas: {0}" -f (($hot | ForEach-Object { "{0} {1:0.0}C" -f $_.Zona, $_.C }) -join ", ")) }
    $path = Join-Path $Session "medir.txt"
    [IO.File]::WriteAllLines($path, [string[]]$report)
    Write-Output "Informe: $path"
}
"gpu" {
    # The live control file sets the profiler's cvars; changing the trigger arms one capture.
    $trigger = [int]((Get-Date).ToFileTime() % 100000) + 1
    $lines = @("cvar xport_profile_native_samples=$Muestras", "cvar xport_profile_native_interval=2",
               "cvar xport_profile_native_capture_trigger=$trigger")
    $local = Join-Path $Session "xnative_skip.txt"
    [IO.File]::WriteAllText($local, (($lines -join "`n") + "`n"))
    $before = (Sh "stat -c %Y $Remote/xnative/Diagnostics/native-performance-latest.csv 2>/dev/null") -join ""
    & adb -s $Device push $local "$Remote/xnative_skip.txt" | Out-Null
    Say ("Perfilador de GPU armado: {0} muestras, una cada 2 fotogramas. Juega con normalidad..." -f $Muestras)
    $done = $false
    for ($i = 0; $i -lt 150; $i++) {
        Start-Sleep -Seconds 2
        $now = (Sh "stat -c %Y $Remote/xnative/Diagnostics/native-performance-latest.csv 2>/dev/null") -join ""
        if ($now -and $now -ne $before) { Start-Sleep -Seconds 3; $done = $true; break }
    }
    & adb -s $Device shell rm -f "$Remote/xnative_skip.txt" | Out-Null
    if (-not $done) {
        $why = @(& adb -s $Device logcat -d --pid=$GamePid -t 3000 2>$null | Select-String -Pattern "xnative-perf|profile-capture" | Select-Object -Last 5 | ForEach-Object { $_.Line })
        Say "El perfil no llego. Ultimas lineas del perfilador:"; $why | ForEach-Object { Say "  $_" }
        break
    }
    foreach ($name in @("native-performance-latest.log", "native-performance-latest.csv", "native-performance-frames.csv", "native-performance-meta.json", "native-performance-gpu-passes.csv", "native-performance-gpu-slices.csv", "native-performance-cpu.csv", "native-performance-cpu-shaders.csv", "native-performance-trace.json")) {
        & adb -s $Device pull "$Remote/xnative/Diagnostics/$name" (Join-Path $Session $name) 2>$null | Out-Null
    }
    Say ("Perfil descargado en {0}" -f $Session)
    $log = Join-Path $Session "native-performance-latest.log"
    if (Test-Path $log) { Get-Content $log -TotalCount 80 | ForEach-Object { Say $_ } }
}
"ab" {
    $phases = @("") + $Quitar + @("")
    $results = New-Object System.Collections.ArrayList
    foreach ($line in $phases) {
        if ($line) {
            $local = Join-Path $Session "xnative_skip.txt"
            [IO.File]::WriteAllText($local, (($line -replace ";", "`n") + "`n"))
            & adb -s $Device push $local "$Remote/xnative_skip.txt" | Out-Null
        } else {
            & adb -s $Device shell rm -f "$Remote/xnative_skip.txt" | Out-Null
        }
        Start-Sleep -Seconds 3
        $start = (Get-Date).ToString("MM-dd HH:mm:ss.000")
        Start-Sleep -Seconds $Segundos
        $fps = @(& adb -s $Device logcat -d --pid=$GamePid -T $start 2>$null |
                 Select-String -Pattern "VblankThread: \d+ swaps \(([\d.]+) fps\)" |
                 ForEach-Object { [double]::Parse($_.Matches[0].Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture) })
        $avg = if ($fps.Count) { ($fps | Measure-Object -Average).Average } else { 0 }
        $name = if ($line) { "sin: $line" } else { "base" }
        [void]$results.Add([pscustomobject]@{ Prueba = $name; Fps = $avg })
        Say ("{0,-48} {1,5:0.0} fps  ({2})" -f $name, $avg, (($fps | ForEach-Object { "{0:0.#}" -f $_ }) -join " "))
    }
    & adb -s $Device shell rm -f "$Remote/xnative_skip.txt" | Out-Null
    [IO.File]::WriteAllLines((Join-Path $Session "ab.txt"), [string[]]$report)
}
"perf" {
    $out = "/data/local/tmp/halo3-perf.data"
    Say ("simpleperf {0} s en el pid {1}..." -f $Segundos, $GamePid)
    & adb -s $Device shell "simpleperf record -e cpu-clock --app $Package --duration $Segundos -f 2000 -o $out" 2>&1 | Select-Object -Last 3 | ForEach-Object { Say "  $_" }
    $perfText = @(Sh "simpleperf report -i $out --sort comm,dso,symbol --percent-limit 0.7 2>&1")
    [IO.File]::WriteAllLines((Join-Path $Session "perf.txt"), [string[]]$perfText)
    $perfText | Select-Object -First 70 | ForEach-Object { Say $_ }
}
}
