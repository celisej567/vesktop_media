# FFmpeg for Windows (scripts/build-ffmpeg.sh builds our own on Linux): a pinned GPL shared build by BtbN
# (FFmpeg 8.1 with x264, x265 and NVENC), checked against its SHA-256. avcodec, avutil and the DLLs they import go to
# third_party\ffmpeg-win\bin and ship next to the module, which loads them from there only (video/ffmpeg.cc).
# A DLL binds its imports to the DLL it names, so these never mix with Electron's own ffmpeg.dll.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Root = Split-Path -Parent $PSScriptRoot
$Dir = Join-Path $Root 'third_party\ffmpeg-win'

$Release = 'autobuild-2026-10-26-14-25'
$Name = 'ffmpeg-n8.1.3-24-g92611900bb-win64-gpl-shared-8.1'
$Sha256 = '40daf4793f3adcf13ed6658c775c10380188e99059a286a12ad984ca5992a34d'

if (Test-Path "$Dir\bin\.stamp") {
    if ((Get-Content "$Dir\bin\.stamp") -eq $Name) {
        Write-Host "== FFmpeg already there: $Dir\bin"
        exit 0
    }
}
Write-Host "== FFmpeg $Name"
New-Item -ItemType Directory -Force $Dir | Out-Null
$zip = "$Dir\$Name.zip"
if (-not (Test-Path $zip) -or (Get-FileHash $zip -Algorithm SHA256).Hash -ne $Sha256) {
    & curl.exe -fL -o $zip "https://github.com/BtbN/FFmpeg-Builds/releases/download/$Release/$Name.zip"
    if ($LASTEXITCODE -ne 0) { throw "cannot download $Name (BtbN keeps autobuilds for a while only: pick a newer one)" }
}
if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $Sha256) { throw "$Name.zip: SHA-256 does not match" }
Remove-Item -Recurse -Force "$Dir\x", "$Dir\bin", "$Dir\tools" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$Dir\x" | Out-Null
& tar.exe -xf $zip -C "$Dir\x"
if ($LASTEXITCODE -ne 0) { throw "cannot unpack $Name.zip" }
New-Item -ItemType Directory -Force "$Dir\bin" | Out-Null
# avcodec imports avutil and swresample; the programs, avformat, avfilter… are not needed
foreach ($pattern in 'avcodec-*.dll', 'avutil-*.dll', 'swresample-*.dll') {
    Copy-Item "$Dir\x\$Name\bin\$pattern" "$Dir\bin\"
}
Copy-Item "$Dir\x\$Name\LICENSE.txt" "$Dir\bin\FFMPEG-LICENSE.txt" -ErrorAction SilentlyContinue
# the whole build, ffmpeg.exe included, for scripts/test.ps1 (it encodes the clips of the video receive cases); not shipped
Move-Item "$Dir\x\$Name\bin" "$Dir\tools"
Remove-Item -Recurse -Force "$Dir\x"
Set-Content "$Dir\bin\.stamp" $Name
Get-ChildItem "$Dir\bin" -Filter *.dll | ForEach-Object { "$($_.Name) $($_.Length)" }
Write-Host "== done: $Dir\bin"
