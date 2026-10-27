# Windows acceptance test (scripts/test.sh on Linux): tools/fake-sfu.js against build\out\vesktop_media.node under
# Electron. The cases that need Linux (PulseAudio soundshare, PipeWire screens) or the ffmpeg CLI are not run.
param([string]$Module = 'build\out\vesktop_media.node')
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)
$Electron = if ($env:ELECTRON) { $env:ELECTRON } else { (Resolve-Path '..\..\node_modules\electron\dist\electron.exe').Path }
$Module = (Resolve-Path $Module).Path
$Tools = (Resolve-Path 'tools').Path
$Work = Join-Path ([IO.Path]::GetTempPath()) ("vesktop_media_test_" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $Work | Out-Null
$env:ELECTRON_RUN_AS_NODE = '1'

# Runs one tool under Electron in $dir with a time limit; returns its exit code (124 on timeout), output in $dir\out
function Invoke-Tool([string]$dir, [int]$seconds, [string[]]$arguments) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    $p = Start-Process -FilePath $Electron -ArgumentList $arguments -WorkingDirectory $dir -NoNewWindow -PassThru `
        -RedirectStandardOutput "$dir\out" -RedirectStandardError "$dir\err"
    $null = $p.Handle # without it ExitCode stays empty for a process started with -NoNewWindow
    if (-not $p.WaitForExit($seconds * 1000)) {
        $p.Kill()
        return 124
    }
    Get-Content "$dir\err" | Add-Content "$dir\out"
    return $p.ExitCode
}

$script:case = 0
# check <pattern> <fake-sfu args>: prints the matching lines, fails when fake-sfu reports a failure
function Check([string]$pattern, [string[]]$arguments) {
    $script:case++
    $dir = Join-Path $Work "case$script:case"
    $status = Invoke-Tool $dir 60 (@("`"$Tools\fake-sfu.js`"", "`"$Module`"") + $arguments)
    Select-String -Path "$dir\out" -Pattern $pattern | ForEach-Object { Write-Host $_.Line }
    if ($status -ne 0) {
        Write-Host "FAILED ($arguments), exit $status, full output:"
        Get-Content "$dir\out" | ForEach-Object { Write-Host $_ }
        exit 1
    }
    return "$dir\out"
}

try {
    Write-Host '== exit with a connection that was not destroyed (Node''s teardown finalizes it)'
    if ((Invoke-Tool "$Work\exit" 20 @("`"$Tools\exit-with-connection.js`"", "`"$Module`"", "`"$Work\exit`"")) -ne 0) { Write-Host 'FAILED (exit with a connection)'; exit 1 }
    Write-Host '== two environments in one process: the callbacks of one go on after the other one ended'
    if ((Invoke-Tool "$Work\two" 30 @("`"$Tools\two-environments.js`"", "`"$Module`"", "`"$Work\two`"")) -ne 0) { Write-Host 'FAILED (two environments)'; exit 1 }

    Write-Host '== noise models (RNNoise by default, DeepFilterNet, Standard = libwebrtc''s suppression)'
    $env:VESKTOP_MEDIA_FAKE_MIC = 'speech'
    $status = Invoke-Tool "$Work\noise" 60 @("`"$Tools\noise-models.js`"", "`"$Module`"", "`"$Work\noise`"")
    Remove-Item env:VESKTOP_MEDIA_FAKE_MIC
    Get-Content "$Work\noise\out" | ForEach-Object { Write-Host $_ }
    if ($status -ne 0) { Write-Host 'FAILED (noise models)'; exit 1 }

    Write-Host '== speaking'
    $out = Check 'onConnect|inbound|summary' @('--seconds', '4')
    if (-not (Select-String -Path $out -Pattern 'speaking \["222",1' -Quiet)) { Write-Host 'FAILED: no speaking callback for the remote user'; exit 1 }
    if (-not (Select-String -Path $out -Pattern 'ping callback \[' -Quiet)) { Write-Host 'FAILED: no ping callback'; exit 1 }
    Write-Host '== silent (expect 10 Opus silence frames, then nothing)'
    Check 'summary' @('--seconds', '3', '--vad', '0') | Out-Null
    Write-Host '== DAVE turned off by a transition'
    Check 'dave|secure frames|summary' @('--seconds', '2', '--dave-reset') | Out-Null

    # video receive: a test clip as a remote camera must come out of the video sink (the clips are encoded with the
    # ffmpeg.exe of scripts/build-ffmpeg.ps1; the module decodes H.264 / H.265 with the FFmpeg DLLs next to it)
    $ffmpegTools = Join-Path (Split-Path -Parent $PSScriptRoot) 'third_party\ffmpeg-win\tools'
    if (Test-Path "$ffmpegTools\ffmpeg.exe") {
        $env:Path = "$ffmpegTools;$env:Path"
        foreach ($codec in 'VP8', 'H264', 'H265') {
            Write-Host "== receive $codec video"
            Check 'inbound video|video sink' @('--seconds', '3', '--recv-video', $codec) | Out-Null
        }
        Write-Host '== receive H264 video joined mid-stream (needs a PLI for the key frame)'
        Check 'summary|video sink' @('--seconds', '4', '--recv-video', 'H264', '--late-join') | Out-Null
    } else {
        Write-Host '== video receive skipped (no ffmpeg.exe in third_party\ffmpeg-win\tools)'
    }

    Write-Host '== send camera video (VP8, looped back)'
    Check 'outbound video|video sink' @('--seconds', '3', '--video', 'VP8') | Out-Null
    Write-Host '== send camera video (H264, x264 through the FFmpeg DLLs, looped back)'
    Check 'outbound video|video sink' @('--seconds', '3', '--video', 'H264') | Out-Null
    Write-Host '== send camera video the way a real call does: layer reported active, encoder idle until viewers ask'
    Check 'late wants|viewers want|outbound video|video sink' @('--seconds', '4', '--video', 'VP8', '--wants-late') | Out-Null
    Write-Host '== same, with the layer created inactive by JS (as Discord does)'
    Check 'late wants|viewers want|outbound video|video sink' @('--seconds', '4', '--video', 'VP8', '--wants-late', '--js-inactive') | Out-Null
    Write-Host '== send camera video with 2 simulcast layers (lower layer looped back)'
    Check 'outbound video|video sink' @('--seconds', '3', '--video', 'VP8', '--layers', '2') | Out-Null
    Write-Host '== the selected camera changed during a call'
    Check 'camera switch|outbound video|video sink' @('--seconds', '4', '--video', 'VP8', '--switch-camera') | Out-Null
    Write-Host '== Go Live with a device (setGoLiveDevices, fake camera)'
    Check 'outbound video|video sink' @('--seconds', '3', '--video', 'VP8', '--golive-camera') | Out-Null
    # soundshare: a 440 Hz tone played by a process of its own, shared by its pid (process loopback). Needs an audio
    # output device (the test VM has a silent virtual one)
    if (Get-CimInstance Win32_SoundDevice | Where-Object Status -eq 'OK') {
        Write-Host '== Go Live audio (soundshare of one process)'
        $wav = Join-Path $Work 'tone.wav'
        $samples = 48000 * 12
        $writer = New-Object IO.BinaryWriter([IO.File]::Create($wav))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $writer.Write([int](36 + $samples * 4))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt ')); $writer.Write([int]16); $writer.Write([int16]1); $writer.Write([int16]2)
        $writer.Write([int]48000); $writer.Write([int](48000 * 4)); $writer.Write([int16]4); $writer.Write([int16]16)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('data')); $writer.Write([int]($samples * 4))
        for ($i = 0; $i -lt $samples; $i++) {
            $v = [int16](8000 * [Math]::Sin($i * 2 * [Math]::PI * 440 / 48000))
            $writer.Write($v); $writer.Write($v)
        }
        $writer.Close()
        $player = Start-Process powershell -WindowStyle Hidden -PassThru -ArgumentList '-NoProfile', '-Command', "(New-Object Media.SoundPlayer '$wav').PlaySync()"
        Start-Sleep -Milliseconds 1500
        try {
            Check 'soundshare|inbound from' @('--seconds', '3', '--video', 'VP8', '--golive-camera', '--soundshare', "$($player.Id)") | Out-Null
            Write-Host '== Go Live audio, entire system (everything except Vesktop''s own processes)'
            Check 'soundshare|inbound from' @('--seconds', '3', '--video', 'VP8', '--golive-camera', '--soundshare', '1') | Out-Null
        } finally {
            Stop-Process -Id $player.Id -Force -ErrorAction SilentlyContinue
        }
    } else {
        Write-Host '== Go Live audio skipped (no audio output device)'
    }

    # Screens and windows can only be captured from a desktop session (not over SSH, session 0)
    if ((Get-Process -Id $PID).SessionId -ne 0) {
        Write-Host '== Go Live from the first screen'
        Check 'outbound video|video sink' @('--seconds', '3', '--video', 'VP8', '--screen', 'screen:0') | Out-Null
        Write-Host '== Go Live from a window, closed mid-stream: the stream must end, not freeze'
        $notepad = Start-Process notepad.exe -PassThru
        $hwnd = 0
        for ($i = 0; $i -lt 50 -and $hwnd -eq 0; $i++) { Start-Sleep -Milliseconds 100; $notepad.Refresh(); $hwnd = [int64]$notepad.MainWindowHandle }
        Check 'desktop source ended|capture ended' @('--seconds', '3', '--video', 'VP8', '--screen', "window:$hwnd", '--kill', "$($notepad.Id)", '--expect-source-end') | Out-Null
    } else {
        Write-Host '== Go Live from screens and windows skipped (not a desktop session)'
    }
    Write-Host '== ok'
} finally {
    Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
}
