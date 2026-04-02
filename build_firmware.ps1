$ErrorActionPreference = 'Stop'

$fqbn = 'esp32:esp32:twatchs3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,Revision=Radio_SX1262'
$outputDir = 'build/esp32.esp32.twatchs3'
$projectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$cli = 'C:\Users\Herbert\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$statsFile = Join-Path $projectDir 'build/.compile_stats_seconds.txt'

function Format-Duration([int]$seconds) {
    if ($seconds -lt 0) { $seconds = 0 }
    $h = [math]::Floor($seconds / 3600)
    $m = [math]::Floor(($seconds % 3600) / 60)
    $s = $seconds % 60
    if ($h -gt 0) { return ('{0:00}:{1:00}:{2:00}' -f $h, $m, $s) }
    return ('{0:00}:{1:00}' -f $m, $s)
}

function Get-Expected-Seconds([string]$path) {
    if (-not (Test-Path $path)) { return 0 }
    $vals = Get-Content $path | ForEach-Object {
        $n = 0
        if ([int]::TryParse($_, [ref]$n) -and $n -gt 0) { $n }
    }
    if (-not $vals -or $vals.Count -eq 0) { return 0 }
    return [int][math]::Round(($vals | Measure-Object -Average).Average)
}

function Save-Compile-Seconds([string]$path, [int]$seconds) {
    $existing = @()
    if (Test-Path $path) {
        $existing = Get-Content $path | ForEach-Object {
            $n = 0
            if ([int]::TryParse($_, [ref]$n) -and $n -gt 0) { $n }
        }
    }
    $updated = @($existing + $seconds)
    if ($updated.Count -gt 12) {
        $updated = $updated[($updated.Count - 12)..($updated.Count - 1)]
    }
    $updated | Set-Content $path
}

if (-not (Test-Path $cli)) {
    Write-Error "arduino-cli not found at: $cli"
    exit 2
}

Push-Location $projectDir
try {
    if (-not (Test-Path $outputDir)) {
        New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
    }

    $expectedSeconds = Get-Expected-Seconds $statsFile

    Write-Output "[BUILD] Running clean compile..."
    if ($expectedSeconds -gt 0) {
        Write-Output ("[ETA] Based on recent builds: ~" + (Format-Duration $expectedSeconds))
    } else {
        Write-Output "[ETA] No history yet, collecting timing for next runs."
    }

    $args = @(
        'compile',
        '--clean',
        '--fqbn',
        $fqbn,
        '--output-dir',
        $outputDir,
        '.'
    )

    $proc = Start-Process -FilePath $cli -ArgumentList $args -NoNewWindow -PassThru
    $sw = [System.Diagnostics.Stopwatch]::StartNew()

    while (-not $proc.HasExited) {
        $elapsed = [int][math]::Floor($sw.Elapsed.TotalSeconds)
        if ($expectedSeconds -gt 0) {
            $remaining = [math]::Max(0, $expectedSeconds - $elapsed)
            $pct = [math]::Min(99, [int](($elapsed * 100) / [math]::Max(1, $expectedSeconds)))
            Write-Progress -Activity 'Compiling firmware' -Status ("Elapsed " + (Format-Duration $elapsed) + " | ETA " + (Format-Duration $remaining)) -PercentComplete $pct
        } else {
            Write-Progress -Activity 'Compiling firmware' -Status ("Elapsed " + (Format-Duration $elapsed) + " | ETA learning...") -PercentComplete 0
        }
        Start-Sleep -Seconds 1
    }

    $sw.Stop()
    Write-Progress -Activity 'Compiling firmware' -Completed

    if ($proc.ExitCode -ne 0) {
        Write-Error "Compile failed with exit code $($proc.ExitCode)"
        exit $proc.ExitCode
    }

    $durationSeconds = [int][math]::Ceiling($sw.Elapsed.TotalSeconds)
    Save-Compile-Seconds $statsFile $durationSeconds

    $bin = Join-Path $outputDir 'WatchOS.ino.bin'
    if (-not (Test-Path $bin)) {
        Write-Error "Compile finished but BIN not found: $bin"
        exit 3
    }

    $item = Get-Item $bin
    Write-Output "[OK] Firmware ready"
    Write-Output ("[DURATION] " + (Format-Duration $durationSeconds))
    Write-Output ("[BIN] " + $item.FullName)
    Write-Output ("[TIME] " + $item.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))
    Write-Output ("[SIZE] " + $item.Length)
}
finally {
    Pop-Location
}
