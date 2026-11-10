# Halo 3 phone monitor: what the running game really uses, sampled every -Intervalo seconds for
# -Segundos, from the PC through adb (nothing in the game changes, no restart):
#   fps        the game's VblankThread log lines
#   CPU        the game's cores in use (/proc/<pid>/stat), the whole phone's busy %, the busiest
#              threads (% of one core, 100 = one core fully busy), core frequencies
#   GPU        busy % of the game's uid: GPU active time from the driver's power/gpu_work_period
#              accounting (dumpsys gpu --gpuwork, Android 14), over wall time; the GPU memory the
#              game holds (dumpsys gpu). The Mali frequency and load nodes are root-only here.
#   RAM        the game's resident and swapped memory, the phone's free memory and swap in use,
#              page faults that read storage/swap per second
#   heat       the GPU's thermal brake: the Mali devfreq cooling device's state from the thermal HAL
#              (dumpsys thermalservice; 0 = not throttled, higher = a lower maximum GPU clock), the
#              HAL's GPU and skin temperatures, and the battery temperature
#   also       the fastest CPU core frequency (the kernel's thermal zones and the Mali clock are
#              root-only on this phone)
# One line per sample on screen; monitor.csv and resumen.txt (means, the threads over the whole
# run, and which of CPU, GPU or memory limits the fps) in logs\monitor\<date>.
#
#   powershell -ExecutionPolicy Bypass -File tools\halo3-monitor.ps1 -Segundos 120
param(
    [int]$Segundos = 60,
    [double]$Intervalo = 2,
    [string]$Etiqueta = "",
    [string]$Device = $env:ANDROID_SERIAL,
    [string]$Package = "com.xport.halo3"
)
$ErrorActionPreference = "Continue"
$inv = [Globalization.CultureInfo]::InvariantCulture
$adbDir = (Join-Path $env:LOCALAPPDATA "Android\Sdk\platform-tools")
if (Test-Path $adbDir) { $env:PATH = "$adbDir;$env:PATH" }
$Root = (Join-Path $PSScriptRoot "..\work\logs\monitor")
$name = Get-Date -Format "yyyyMMdd_HHmmss"
if ($Etiqueta) { $name += "_" + $Etiqueta }
$Session = Join-Path $Root $name
New-Item -ItemType Directory -Force $Session | Out-Null

$GamePid = ((& adb -s $Device shell pidof $Package) -join " ").Trim()
if (-not $GamePid) { Write-Output "El juego no esta en marcha ($Package): abrelo y vuelve a lanzar el monitor."; exit 1 }
$Uid = ""
foreach ($line in (& adb -s $Device shell pm list packages -U $Package)) {
    if ($line -match "^package:$([regex]::Escape($Package)) uid:(\d+)") { $Uid = $Matches[1] }
}
$Cores = [int]((& adb -s $Device shell "ls -d /sys/devices/system/cpu/cpu[0-9]* | wc -l") -join "").Trim()

# One adb call per sample; every line starts with a tag.
# Shell builtins only per thread (read, not cat): a fork per file made one sample take seconds.
# The thermal zones are root-only on this phone: the battery is the only temperature.
$Probe = @'
P=__PID__
read -r u x < /proc/uptime; echo "@U $u"
read -r l < /proc/stat; echo "@S $l"
read -r s 2>/dev/null < /proc/$P/stat || { echo "@X"; exit 0; }
r=${s##*) }; set -- $r; echo "@P ${10} ${12} ${13}"
for t in /proc/$P/task/*; do read -r s 2>/dev/null < $t/stat || continue; read -r c 2>/dev/null < $t/comm; r=${s##*) }; set -- $r; echo "@T ${t##*/} ${12} ${13} ${37} $c"; done
while read -r k v rest; do case $k in VmRSS:|VmSwap:) echo "@V $k $v";; esac; done < /proc/$P/status
while read -r k v rest; do case $k in MemTotal:|MemAvailable:|SwapTotal:|SwapFree:) echo "@M $k $v";; esac; done < /proc/meminfo
dumpsys gpu --gpuwork | grep -E "^[0-9]+ __UID__ " | sed "s/^/@G /"
dumpsys gpu | grep "Proc $P total" | sed "s/^/@H /"
for c in /sys/devices/system/cpu/cpu[0-9]*; do read -r f 2>/dev/null < $c/cpufreq/scaling_cur_freq && echo "@F ${c##*/} $f"; done
dumpsys battery | grep -m1 temperature | sed "s/^/@B /"
dumpsys thermalservice | sed -n '/^Current temperatures from HAL/,/^Temperature static/p' | grep -E "mName=(GPU|SKIN),|mali" | sed "s/^/@K /"
'@
$Probe = $Probe.Replace("__PID__", $GamePid).Replace("__UID__", $Uid).Replace("`r", "")
# Windows PowerShell mangles double quotes in native arguments: the probe goes as a file.
$probeLocal = Join-Path $Session "sonda.sh"
[IO.File]::WriteAllText($probeLocal, $Probe)
$ProbeRemote = "/data/local/tmp/halo3_monitor_probe.sh"
& adb -s $Device push $probeLocal $ProbeRemote 2>$null | Out-Null

function Sample {
    $s = @{ Threads = @{}; GpuActive = [int64]0; GpuSeen = $false; Freq = @()
            GpuBrake = [double]::NaN; TempGpu = [double]::NaN; TempSkin = [double]::NaN }
    foreach ($line in (& adb -s $Device shell sh $ProbeRemote)) {
        $p = $line.Trim() -split "\s+"
        switch ($p[0]) {
            "@X" { $s.Gone = $true }
            "@U" { $s.Uptime = [double]::Parse($p[1], $inv) }
            "@S" { $v = @($p[2..($p.Count - 1)] | ForEach-Object { [int64]$_ }); $s.CpuTotal = ($v | Measure-Object -Sum).Sum; $s.CpuIdle = $v[3] + $v[4] }
            "@P" { $s.Major = [int64]$p[1]; $s.Ticks = [int64]$p[2] + [int64]$p[3] }
            "@T" { $parts = $line.Trim() -split " ", 6; if ($parts.Count -eq 6) { $s.Threads[$parts[1]] = @{ Ticks = [int64]$parts[2] + [int64]$parts[3]; Core = $parts[4]; Name = $parts[5] } } }
            "@V" { if ($p[1] -eq "VmRSS:") { $s.RssKb = [int64]$p[2] } elseif ($p[1] -eq "VmSwap:") { $s.SwapKb = [int64]$p[2] } }
            "@M" { $s[$p[1].TrimEnd(":")] = [int64]$p[2] }
            "@G" { $s.GpuActive += [int64]$p[3]; $s.GpuSeen = $true }
            "@H" { $s.GpuMem = [int64]$p[4] }
            "@F" { if ($p.Count -ge 3) { $s.Freq += ,@($p[1], [int64]$p[2]) } }
            "@B" { if ($line -match "temperature:\s*(\d+)") { $s.Battery = [double]$Matches[1] / 10 } }
            "@K" {
                if ($line -match "CoolingDevice\{mValue=(\d+),.*mali") { $s.GpuBrake = [double]$Matches[1] }
                elseif ($line -match "Temperature\{mValue=([\d.]+), mType=\d+, mName=GPU,") { $s.TempGpu = [double]::Parse($Matches[1], $inv) }
                elseif ($line -match "Temperature\{mValue=([\d.]+), mType=\d+, mName=SKIN,") { $s.TempSkin = [double]::Parse($Matches[1], $inv) }
            }
        }
    }
    return $s
}

# fps: a background logcat of the game's VblankThread lines, read as it grows.
$fpsFile = Join-Path $Session "vblank.log"
$logcat = Start-Process -FilePath "adb" -ArgumentList @("-s", $Device, "logcat", "--pid=$GamePid", "-T", "1", "-e", "VblankThread") -RedirectStandardOutput $fpsFile -NoNewWindow -PassThru
$fpsRead = 0
function New-Fps {
    $values = @()
    if (Test-Path $fpsFile) {
        $lines = @(Get-Content $fpsFile -ErrorAction SilentlyContinue)
        for ($i = $script:fpsRead; $i -lt $lines.Count; ++$i) {
            if ($lines[$i] -match "VblankThread: \d+ swaps \(([\d.]+) fps\)") { $values += [double]::Parse($Matches[1], $inv) }
        }
        $script:fpsRead = $lines.Count
    }
    return $values
}

$header = "hora,fps,cpu_juego_nucleos,cpu_movil_pct,gpu_ocupada_pct,gpu_mem_mb,ram_juego_mb,swap_juego_mb,movil_libre_mb,movil_swap_usado_mb,fallos_disco_s,cpu_frec_max_mhz,bateria_c,freno_gpu,temp_gpu_c,temp_piel_c,hilos"
$csv = New-Object System.Collections.Generic.List[string]
$csv.Add($header)
$rows = New-Object System.Collections.Generic.List[object]
Write-Output ("Monitor de Halo 3: pid {0}, uid {1}, {2} nucleos, {3} s cada {4} s -> {5}" -f $GamePid, $Uid, $Cores, $Segundos, $Intervalo, $Session)
if (-not $Uid) { Write-Output "  (sin uid: no hay lectura de GPU)" }
Write-Output "hora      fps  | CPU juego (nucleos) movil% | GPU ocupada  mem | RAM juego  swap | movil libre swap | fallos/s | CPU MHz bat C | freno GPU, T GPU, T piel | hilos mas ocupados (% de un nucleo)"

$first = Sample
$prev = $first
$gpuWarned = $false
$deadline = (Get-Date).AddSeconds($Segundos)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds ([int]($Intervalo * 1000))
    $cur = Sample
    if ($cur.Gone) { Write-Output "El juego se ha cerrado."; break }
    $dt = $cur.Uptime - $prev.Uptime
    if ($dt -le 0) { continue }
    $fps = @(New-Fps)
    $fpsMean = if ($fps.Count) { ($fps | Measure-Object -Average).Average } else { [double]::NaN }
    $gameCores = ($cur.Ticks - $prev.Ticks) / 100.0 / $dt
    $phonePct = 100.0 * (1 - ($cur.CpuIdle - $prev.CpuIdle) / [math]::Max(1, $cur.CpuTotal - $prev.CpuTotal))
    $gpuPct = if ($cur.GpuSeen -and $prev.GpuSeen) { 100.0 * ($cur.GpuActive - $prev.GpuActive) / ($dt * 1e9) } else { [double]::NaN }
    $major = ($cur.Major - $prev.Major) / $dt
    $threads = @(foreach ($tid in $cur.Threads.Keys) {
        if (-not $prev.Threads.ContainsKey($tid)) { continue }
        [pscustomobject]@{ Name = $cur.Threads[$tid].Name; Pct = 100.0 * ($cur.Threads[$tid].Ticks - $prev.Threads[$tid].Ticks) / 100.0 / $dt }
    }) | Sort-Object Pct -Descending | Select-Object -First 4
    $top = ($threads | Where-Object { $_.Pct -ge 5 } | ForEach-Object { "{0} {1:0}" -f $_.Name, $_.Pct }) -join ", "
    $freqMax = ($cur.Freq | ForEach-Object { $_[1] } | Measure-Object -Maximum).Maximum / 1000.0
    $row = [pscustomobject]@{
        Hora = (Get-Date -Format "HH:mm:ss"); Fps = $fpsMean; CpuJuego = $gameCores; CpuMovil = $phonePct; Gpu = $gpuPct
        GpuMem = $cur.GpuMem / 1MB; Rss = $cur.RssKb / 1024.0; Swap = $cur.SwapKb / 1024.0
        Libre = $cur.MemAvailable / 1024.0; SwapUsado = ($cur.SwapTotal - $cur.SwapFree) / 1024.0
        Fallos = $major; FreqMax = $freqMax; Bateria = $cur.Battery; Hilos = $top
        Freno = $cur.GpuBrake; TGpu = $cur.TempGpu; TPiel = $cur.TempSkin }
    $rows.Add($row)
    Write-Output ("{0} {1,5:0.0} | {2,5:0.00} {3,5:0}% | {4,5:0}% {5,5:0}M | {6,6:0}M {7,5:0}M | {8,6:0}M {9,5:0}M | {10,6:0} | {11,5:0} {12,4:0.0} | {14,4:0} {15,5:0.0}C {16,5:0.0}C | {13}" -f $row.Hora, $row.Fps, $row.CpuJuego, $row.CpuMovil, $row.Gpu, $row.GpuMem, $row.Rss, $row.Swap, $row.Libre, $row.SwapUsado, $row.Fallos, $row.FreqMax, $row.Bateria, $row.Hilos, $row.Freno, $row.TGpu, $row.TPiel)
    if (-not $cur.GpuSeen -and -not $gpuWarned) { Write-Output "  (dumpsys gpu --gpuwork no da linea para el uid ${Uid}: sin lectura de GPU)"; $gpuWarned = $true }
    $csv.Add([string]::Format($inv, "{0},{1:0.0},{2:0.000},{3:0.0},{4:0.0},{5:0},{6:0},{7:0},{8:0},{9:0},{10:0},{11:0},{12:0.0},{14:0},{15:0.0},{16:0.0},""{13}""",
        $row.Hora, $row.Fps, $row.CpuJuego, $row.CpuMovil, $row.Gpu, $row.GpuMem, $row.Rss, $row.Swap, $row.Libre, $row.SwapUsado, $row.Fallos, $row.FreqMax, $row.Bateria, $row.Hilos, $row.Freno, $row.TGpu, $row.TPiel))
    $prev = $cur
}
try { Stop-Process -Id $logcat.Id -Force -ErrorAction Stop } catch {}
[IO.File]::WriteAllLines((Join-Path $Session "monitor.csv"), $csv.ToArray())

# Summary over the whole run.
$report = New-Object System.Collections.Generic.List[string]
function Stat([string]$label, [object[]]$values, [string]$unit, [string]$format = "0.0") {
    $v = @($values | Where-Object { $_ -is [double] -and -not [double]::IsNaN($_) })
    if (-not $v.Count) { return "{0,-26} sin datos" -f $label }
    $m = $v | Measure-Object -Average -Minimum -Maximum
    return ("{0,-26} media {1,8} {4}  min {2,8}  max {3,8}" -f $label, $m.Average.ToString($format), $m.Minimum.ToString($format), $m.Maximum.ToString($format), $unit)
}
$report.Add(("Monitor {0}: {1} muestras, pid {2}" -f $name, $rows.Count, $GamePid))
$report.Add((Stat "FPS" ($rows | ForEach-Object { [double]$_.Fps }) ""))
$report.Add((Stat "CPU del juego" ($rows | ForEach-Object { [double]$_.CpuJuego }) ("nucleos de {0}" -f $Cores) "0.00"))
$report.Add((Stat "CPU de todo el movil" ($rows | ForEach-Object { [double]$_.CpuMovil }) "%" "0"))
$report.Add((Stat "GPU ocupada por el juego" ($rows | ForEach-Object { [double]$_.Gpu }) "%" "0"))
$report.Add((Stat "Memoria GPU del juego" ($rows | ForEach-Object { [double]$_.GpuMem }) "MB" "0"))
$report.Add((Stat "RAM del juego (residente)" ($rows | ForEach-Object { [double]$_.Rss }) "MB" "0"))
$report.Add((Stat "RAM del juego en swap" ($rows | ForEach-Object { [double]$_.Swap }) "MB" "0"))
$report.Add((Stat "RAM libre del movil" ($rows | ForEach-Object { [double]$_.Libre }) "MB" "0"))
$report.Add((Stat "Swap usado del movil" ($rows | ForEach-Object { [double]$_.SwapUsado }) "MB" "0"))
$report.Add((Stat "Fallos a disco/swap" ($rows | ForEach-Object { [double]$_.Fallos }) "por s" "0"))
$report.Add((Stat "Frecuencia CPU max" ($rows | ForEach-Object { [double]$_.FreqMax }) "MHz" "0"))
$report.Add((Stat "Bateria" ($rows | ForEach-Object { [double]$_.Bateria }) "C"))
$report.Add((Stat "Freno termico de la GPU" ($rows | ForEach-Object { [double]$_.Freno }) "(0 = sin freno)" "0"))
$report.Add((Stat "Temperatura GPU" ($rows | ForEach-Object { [double]$_.TGpu }) "C"))
$report.Add((Stat "Temperatura piel del movil" ($rows | ForEach-Object { [double]$_.TPiel }) "C"))
# Frame rate and GPU load at each brake state: whether the fps fall when the brake rises.
$byBrake = @($rows | Where-Object { -not [double]::IsNaN($_.Freno) } | Group-Object Freno)
if ($byBrake.Count) {
    $report.Add("FPS segun el freno termico de la GPU:")
    foreach ($g in ($byBrake | Sort-Object { [double]$_.Name })) {
        $f = ($g.Group | Where-Object { -not [double]::IsNaN($_.Fps) } | Measure-Object Fps -Average).Average
        $gp = ($g.Group | Where-Object { -not [double]::IsNaN($_.Gpu) } | Measure-Object Gpu -Average).Average
        $report.Add(("  freno {0,3}: {1,3} muestras, fps medio {2,5:0.0}, GPU ocupada {3,3:0} %" -f $g.Name, $g.Count, $f, $gp))
    }
}
$span = $prev.Uptime - $first.Uptime
if ($span -gt 0) {
    $report.Add("Hilos en toda la medida (% de un nucleo; 100 = un nucleo entero):")
    $all = @(foreach ($tid in $prev.Threads.Keys) {
        if (-not $first.Threads.ContainsKey($tid)) { continue }
        [pscustomobject]@{ Name = $prev.Threads[$tid].Name; Tid = $tid; Pct = 100.0 * ($prev.Threads[$tid].Ticks - $first.Threads[$tid].Ticks) / 100.0 / $span }
    }) | Sort-Object Pct -Descending | Select-Object -First 15
    foreach ($t in $all) { if ($t.Pct -ge 1) { $report.Add(("  {0,-22} {1,6:0.0} %  tid {2}" -f $t.Name, $t.Pct, $t.Tid)) } }
    # What limits the fps.
    $gpuMean = ($rows | Where-Object { -not [double]::IsNaN($_.Gpu) } | Measure-Object Gpu -Average).Average
    $faultMean = ($rows | Measure-Object Fallos -Average).Average
    $busiest = $all | Select-Object -First 1
    $verdict = @()
    if ($gpuMean -ge 85) { $verdict += ("GPU: ocupada el {0:0} % del tiempo, es el limite" -f $gpuMean) }
    if ($busiest -and $busiest.Pct -ge 85) { $verdict += ("CPU: el hilo {0} usa {1:0} % de un nucleo, es un limite" -f $busiest.Name, $busiest.Pct) }
    if ($faultMean -ge 200) { $verdict += ("RAM: {0:0} lecturas de swap/disco por segundo, el juego espera a la memoria" -f $faultMean) }
    $brakeMean = ($rows | Where-Object { -not [double]::IsNaN($_.Freno) } | Measure-Object Freno -Average).Average
    if ($brakeMean -gt 0) { $verdict += ("Calor: el sistema frena la GPU (estado medio {0:0}; 0 = sin freno), rinde menos aunque este ocupada" -f $brakeMean) }
    if (-not $verdict.Count) { $verdict += "Nada saturado: ni la GPU, ni un hilo, ni la memoria (mirar el ritmo de fotogramas o esperas entre hilos)" }
    $report.Add("Que limita: " + ($verdict -join "; "))
}
[IO.File]::WriteAllLines((Join-Path $Session "resumen.txt"), $report.ToArray())
Write-Output ""
$report | ForEach-Object { Write-Output $_ }
Write-Output "Ficheros: $Session"
