# 生成 pvz-a11y 僵尸提示音（对齐 PvZA11y 的 NAudio 音调参数）：
# - 入场提示：正弦 100ms，freq = 1000 - row*500/height，右声道；
# - 快速僵尸：锯齿 3x50ms（0/100/200ms 起始），同一频率，右声道。
# - 巨人举锤：锯齿 2x50ms（0/100ms 起始），同一行频率，双声道居中音源。
# - 跨行浏览：三角波 100ms，同一行频率，双声道居中音源，运行时按最左僵尸定位。
# 输出 22050Hz 16bit 立体声 PCM wav；入场/快速僵尸左声道静音，
# 举锤和跨行音由播放时的 pan 按僵尸横向位置定位。
param([string]$OutDir = "$PSScriptRoot\..\sounds")

$rate = 22050
$amp = 0.8
$fadeMs = 3.0

function Write-CueWav([string]$Path, [double]$Freq, [string]$Kind) {
    $beepMs = if ($Kind -in @("entry", "row")) { 100.0 } else { 50.0 }
    $totalMs = if ($Kind -in @("entry", "row")) { 100.0 } elseif ($Kind -eq "hammer") { 150.0 } else { 250.0 }
    $starts = if ($Kind -in @("entry", "row")) { @(0.0) } elseif ($Kind -eq "hammer") { @(0.0, 100.0) } else { @(0.0, 100.0, 200.0) }
    $total = [int]($rate * $totalMs / 1000)
    $samples = New-Object 'double[]' $total
    $fade = [int]($rate * $fadeMs / 1000)
    foreach ($startMs in $starts) {
        $offset = [int]($rate * $startMs / 1000)
        $len = [int]($rate * $beepMs / 1000)
        for ($i = 0; $i -lt $len; $i++) {
            $t = $i / $rate
            $v = if ($Kind -eq "entry") {
                [Math]::Sin(2 * [Math]::PI * $Freq * $t)
            } elseif ($Kind -eq "row") {
                $phase = ($Freq * $t) % 1.0
                1.0 - 4.0 * [Math]::Abs($phase - 0.5)
            } else {
                $phase = ($Freq * $t) % 1.0
                2.0 * $phase - 1.0
            }
            $gain = 1.0
            if ($i -lt $fade) { $gain = $i / [double]$fade }
            elseif ($i -ge $len - $fade) { $gain = ($len - $i) / [double]$fade }
            $samples[$offset + $i] = $v * $amp * $gain
        }
    }
    $fs = [System.IO.File]::Create($Path)
    $bw = New-Object System.IO.BinaryWriter($fs)
    $dataSize = $total * 4
    $bw.Write([System.Text.Encoding]::ASCII.GetBytes("RIFF"))
    $bw.Write([int](36 + $dataSize))
    $bw.Write([System.Text.Encoding]::ASCII.GetBytes("WAVEfmt "))
    $bw.Write([int]16); $bw.Write([int16]1); $bw.Write([int16]2)
    $bw.Write([int]$rate); $bw.Write([int]($rate * 4)); $bw.Write([int16]4); $bw.Write([int16]16)
    $bw.Write([System.Text.Encoding]::ASCII.GetBytes("data"))
    $bw.Write([int]$dataSize)
    foreach ($s in $samples) {
        $v = [int16]([Math]::Max(-1.0, [Math]::Min(1.0, $s)) * 32767)
        if ($Kind -in @("hammer", "row")) { $bw.Write($v) } else { $bw.Write([int16]0) }
        $bw.Write($v)
    }
    $bw.Close(); $fs.Close()
}

foreach ($height in @(5, 6)) {
    for ($row = 0; $row -lt $height; $row++) {
        $freq = 1000.0 - ($row * 500.0 / $height)
        Write-CueWav (Join-Path $OutDir ("entry_h{0}_r{1}.wav" -f $height, $row)) $freq "entry"
        Write-CueWav (Join-Path $OutDir ("fast_h{0}_r{1}.wav" -f $height, $row)) $freq "fast"
        Write-CueWav (Join-Path $OutDir ("hammer_h{0}_r{1}.wav" -f $height, $row)) $freq "hammer"
        Write-CueWav (Join-Path $OutDir ("row_h{0}_r{1}.wav" -f $height, $row)) $freq "row"
    }
}

# 植物就绪琶音（对齐 PvZA11y PlantReadyCueVolume）：698.46/880/1046.5 Hz 正弦
# 三音叠加，190/170/150ms，起始 0/20/40ms，双声道。
function Write-ReadyWav([string]$Path) {
    $tones = @(
        @{ Freq = 698.46; StartMs = 0.0;  LenMs = 190.0 },
        @{ Freq = 880.0;  StartMs = 20.0; LenMs = 170.0 },
        @{ Freq = 1046.5; StartMs = 40.0; LenMs = 150.0 }
    )
    $total = [int]($rate * 190 / 1000)
    $samples = New-Object 'double[]' $total
    $fade = [int]($rate * $fadeMs / 1000)
    foreach ($tone in $tones) {
        $offset = [int]($rate * $tone.StartMs / 1000)
        $len = [int]($rate * $tone.LenMs / 1000)
        for ($i = 0; $i -lt $len; $i++) {
            $t = $i / $rate
            $gain = 1.0
            if ($i -lt $fade) { $gain = $i / [double]$fade }
            elseif ($i -ge $len - $fade) { $gain = ($len - $i) / [double]$fade }
            $samples[$offset + $i] += [Math]::Sin(2 * [Math]::PI * $tone.Freq * $t) * 0.35 * $gain
        }
    }
    $fs = [System.IO.File]::Create($Path)
    $bw = New-Object System.IO.BinaryWriter($fs)
    $dataSize = $total * 4
    $bw.Write([System.Text.Encoding]::ASCII.GetBytes("RIFF"))
    $bw.Write([int](36 + $dataSize))
    $bw.Write([System.Text.Encoding]::ASCII.GetBytes("WAVEfmt "))
    $bw.Write([int]16); $bw.Write([int16]1); $bw.Write([int16]2)
    $bw.Write([int]$rate); $bw.Write([int]($rate * 4)); $bw.Write([int16]4); $bw.Write([int16]16)
    $bw.Write([System.Text.Encoding]::ASCII.GetBytes("data"))
    $bw.Write([int]$dataSize)
    foreach ($s in $samples) {
        $v = [int16]([Math]::Max(-1.0, [Math]::Min(1.0, $s)) * 32767)
        $bw.Write($v); $bw.Write($v)  # 双声道
    }
    $bw.Close(); $fs.Close()
}
Write-ReadyWav (Join-Path $OutDir "ready.wav")

# 下一波僵尸刷新倒计时方波提示音（短促方波 60ms，以 C5=523.25Hz 为基准，C 大调自然音阶上行）
# 倒计时音调从高到低，每次递减一个音，最终到达 C5。
# 音符索引 1..15 对应 1..15 秒倒计时：
# 1s: C5 (523.25 Hz)
# 2s: D5 (587.33 Hz)
# 3s: E5 (659.25 Hz)
# 4s: F5 (698.46 Hz)
# 5s: G5 (783.99 Hz)  -> 5秒倒计时即 G F E D C
# 6s: A5 (880.00 Hz)
# 7s: B5 (987.77 Hz)
# 8s: C6 (1046.50 Hz)
# 9s: D6 (1174.66 Hz)
# 10s: E6 (1318.51 Hz)
# 11s: F6 (1396.91 Hz)
# 12s: G6 (1567.98 Hz)
# 13s: A6 (1760.00 Hz)
# 14s: B6 (1975.53 Hz)
# 15s: C7 (2093.00 Hz)
function Write-SquareWav([string]$Path, [double]$Freq, [double]$DurationMs = 60.0) {
    $total = [int]($rate * $DurationMs / 1000)
    $samples = New-Object 'double[]' $total
    $fade = [int]($rate * $fadeMs / 1000)
    $len = $total
    for ($i = 0; $i -lt $len; $i++) {
        $t = $i / $rate
        $phase = ($Freq * $t) % 1.0
        $v = if ($phase -lt 0.5) { 1.0 } else { -1.0 }
        $gain = 1.0
        if ($i -lt $fade) { $gain = $i / [double]$fade }
        elseif ($i -ge $len - $fade) { $gain = ($len - $i) / [double]$fade }
        $samples[$i] = $v * 0.45 * $gain
    }
    $fs = [System.IO.File]::Create($Path)
    $bw = New-Object System.IO.BinaryWriter($fs)
    $dataSize = $total * 4
    $bw.Write([System.Text.Encoding]::ASCII.GetBytes("RIFF"))
    $bw.Write([int](36 + $dataSize))
    $bw.Write([System.Text.Encoding]::ASCII.GetBytes("WAVEfmt "))
    $bw.Write([int]16); $bw.Write([int16]1); $bw.Write([int16]2)
    $bw.Write([int]$rate); $bw.Write([int]($rate * 4)); $bw.Write([int16]4); $bw.Write([int16]16)
    $bw.Write([System.Text.Encoding]::ASCII.GetBytes("data"))
    $bw.Write([int]$dataSize)
    foreach ($s in $samples) {
        $v = [int16]([Math]::Max(-1.0, [Math]::Min(1.0, $s)) * 32767)
        $bw.Write($v); $bw.Write($v)  # 双声道立体声居中
    }
    $bw.Close(); $fs.Close()
}

$countdownFreqs = @(
    523.25, # 1: C5
    587.33, # 2: D5
    659.25, # 3: E5
    698.46, # 4: F5
    783.99, # 5: G5
    880.00, # 6: A5
    987.77, # 7: B5
    1046.50, # 8: C6
    1174.66, # 9: D6
    1318.51, # 10: E6
    1396.91, # 11: F6
    1567.98, # 12: G6
    1760.00, # 13: A6
    1975.53, # 14: B6
    2093.00  # 15: C7
)
for ($idx = 1; $idx -le $countdownFreqs.Length; $idx++) {
    $f = $countdownFreqs[$idx - 1]
    Write-SquareWav (Join-Path $OutDir ("countdown_{0}.wav" -f $idx)) $f 60.0
}

Write-Host "cues written to $OutDir"
