# Windows build of vesktop_media into build\out\{index.js,vesktop_media.node} (scripts/build.sh on Linux).
# Needs: Visual Studio 2022 or its Build Tools (C++ desktop workload, Windows SDK), LLVM 22 for Windows (clang-cl,
# lld-link), CMake, Ninja, Git, pnpm. Downloads the prebuilt libwebrtc and the libc++ headers on the first run.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-Location (Split-Path -Parent $PSScriptRoot)
$Root = (Get-Location).Path
$TP = Join-Path $Root 'third_party'
$Build = if ($env:BUILD_DIR) { $env:BUILD_DIR } else { Join-Path $Root 'build' }
$TC = Join-Path $Root 'cmake\toolchain.cmake'

$WebrtcBuild = 'm156.8078.5.1'
$LibcxxCommit = '97b436da4c33663581d394f4ee0a5977fc38c2f4'
$BuildtoolsCommit = '61df5d8b18e317ea9fbf5e55d86e865388200fca'
$LibdaveRepo = 'https://github.com/discord/libdave'
$MlsppCommit = '1cc50a124a3bc4e143a787ec934280dc70c1034d'
$NlohmannVersion = 'v3.12.0'

function Run([string]$exe) {
    & $exe @args
    if ($LASTEXITCODE -ne 0) { throw "$exe $args failed ($LASTEXITCODE)" }
}

# The MSVC environment (headers, libraries, Windows SDK) for clang-cl and lld-link, from the newest Visual Studio
if (-not $env:VCToolsInstallDir) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio with the C++ desktop workload was not found' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] }
    }
}
# LLVM for Windows ahead of Visual Studio's own (older) clang: the libc++ headers of libwebrtc's revision need a
# current clang
$llvm = Join-Path $env:ProgramFiles 'LLVM\bin'
if (Test-Path (Join-Path $llvm 'clang-cl.exe')) { $env:Path = "$llvm;$env:Path" }

if (-not (Test-Path node_modules\node-addon-api) -and -not (Test-Path ..\..\node_modules\node-addon-api)) {
    Run pnpm install --ignore-workspace
}

if (-not (Test-Path "$TP\webrtc\lib\webrtc.lib")) {
    Write-Host "== fetching prebuilt libwebrtc $WebrtcBuild"
    New-Item -ItemType Directory -Force "$TP\webrtc-dl" | Out-Null
    $zip = "$TP\webrtc-dl\webrtc.zip"
    Run curl.exe -fL -o $zip "https://github.com/shiguredo-webrtc-build/webrtc-build/releases/download/$WebrtcBuild/webrtc.windows_x86_64.zip"
    Remove-Item -Recurse -Force "$TP\webrtc-x", "$TP\webrtc" -ErrorAction SilentlyContinue
    Run tar.exe -xf $zip -C (New-Item -ItemType Directory -Force "$TP\webrtc-x").FullName
    Move-Item "$TP\webrtc-x\webrtc" "$TP\webrtc"
    Remove-Item -Recurse -Force "$TP\webrtc-x", $zip
}

function Fetch([string]$dir, [string]$url, [string]$commit) {
    if (Test-Path "$TP\$dir\.git") { return }
    New-Item -ItemType Directory -Force "$TP\$dir" | Out-Null
    Run git -C "$TP\$dir" init -q
    Run git -C "$TP\$dir" fetch -q --depth 1 $url $commit
    Run git -C "$TP\$dir" checkout -q FETCH_HEAD
}
# libc++ only: on Windows libc++ sits on the MSVC runtime, without libc++abi
Fetch libcxx https://chromium.googlesource.com/external/github.com/llvm/llvm-project/libcxx.git $LibcxxCommit
Fetch mlspp https://github.com/cisco/mlspp $MlsppCommit
if (-not (Test-Path "$TP\libdave\.git")) { Run git clone -q --depth 1 $LibdaveRepo "$TP\libdave" }

if (-not (Test-Path "$TP\libcxx-config\include\__config_site")) {
    New-Item -ItemType Directory -Force "$TP\libcxx-config\include" | Out-Null
    foreach ($f in '__config_site', '__assertion_handler') {
        $b64 = (Invoke-WebRequest -UseBasicParsing "https://chromium.googlesource.com/chromium/src/buildtools/+/$BuildtoolsCommit/third_party/libc++/$f`?format=TEXT").Content
        if ($b64 -is [byte[]]) { $b64 = [Text.Encoding]::ASCII.GetString($b64) }
        [IO.File]::WriteAllBytes("$TP\libcxx-config\include\$f", [Convert]::FromBase64String($b64))
    }
}
# nlohmann/json: Linux uses the system's, Windows has none. Installed into build\deps with its CMake package (mlspp
# looks for that)
Fetch nlohmann https://github.com/nlohmann/json $NlohmannVersion

# A CMake build directory remembers the absolute source path: after the project moved, start it over
function Fresh([string]$dir, [string]$src) {
    $cache = Join-Path $dir 'CMakeCache.txt'
    if (-not (Test-Path $cache)) { return }
    $want = (Resolve-Path $src).Path.Replace('\', '/')
    if (-not (Select-String -Path $cache -SimpleMatch "CMAKE_HOME_DIRECTORY:INTERNAL=$want" -Quiet)) {
        Write-Host "== $dir was configured elsewhere, starting it over"
        Remove-Item -Recurse -Force $dir
    }
}

$Deps = Join-Path $Build 'deps'
if (-not (Test-Path "$Deps\include\nlohmann\json.hpp")) {
    Fresh "$Build\nlohmann" "$TP\nlohmann"
    Run cmake -S "$TP\nlohmann" -B "$Build\nlohmann" -G Ninja -DJSON_BuildTests=OFF -DJSON_Install=ON "-DCMAKE_INSTALL_PREFIX=$Deps"
    Run cmake --install "$Build\nlohmann"
}
if (-not (Test-Path "$Deps\lib\libdave.lib")) {
    Write-Host '== building mlspp + libdave'
    $env:CXXFLAGS = '/FIctime'
    $thirdParty = Join-Path $Root 'cmake\third_party_win.cmake'
    Fresh "$Build\mlspp" "$TP\mlspp"
    Fresh "$Build\libdave" "$TP\libdave\cpp"
    Run cmake -S "$TP\mlspp" -B "$Build\mlspp" -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_BUILD_TYPE=Release `
        "-DCMAKE_PREFIX_PATH=$Deps" "-DCMAKE_PROJECT_INCLUDE=$thirdParty" -DDISABLE_GREASE=ON -DMLS_CXX_NAMESPACE=mlspp -DREQUIRE_BORINGSSL=ON "-DCMAKE_INSTALL_PREFIX=$Deps"
    Run cmake --build "$Build\mlspp"
    Run cmake --install "$Build\mlspp"
    Run cmake -S "$TP\libdave\cpp" -B "$Build\libdave" -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_BUILD_TYPE=Release `
        "-DCMAKE_PREFIX_PATH=$Deps" "-DCMAKE_PROJECT_INCLUDE=$thirdParty" -DPERSISTENT_KEYS=ON -DREQUIRE_BORINGSSL=ON "-DCMAKE_INSTALL_PREFIX=$Deps"
    Run cmake --build "$Build\libdave"
    Run cmake --install "$Build\libdave"
    Remove-Item env:CXXFLAGS
}

# FFmpeg (H.264 / H.265, x264 / x265 / NVENC) and RNNoise / DeepFilterNet, shipped next to the module
Run powershell -NoProfile -ExecutionPolicy Bypass -File "$Root\scripts\build-ffmpeg.ps1"
Run powershell -NoProfile -ExecutionPolicy Bypass -File "$Root\scripts\build-denoise.ps1"

Write-Host '== building vesktop_media'
Fresh "$Build\main" $Root
$type = if ($env:BUILD_TYPE) { $env:BUILD_TYPE } else { 'Release' }
Run cmake -S . -B "$Build\main" -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$TC" "-DCMAKE_BUILD_TYPE=$type" "-DVV_DEPS_DIR=$Deps"
Run cmake --build "$Build\main" '--' -k 0
New-Item -ItemType Directory -Force "$Build\out" | Out-Null
# A loaded .node cannot be overwritten on Windows, but it can be renamed: move the old one aside first
$files = @(Get-Item "$Build\main\out\vesktop_media.node", "$Build\main\out\index.js") +
    @(Get-ChildItem "$TP\ffmpeg-win\bin\*.dll") + @(Get-ChildItem "$TP\denoise\prefix\lib\*.dll")
foreach ($file in $files) {
    $dst = Join-Path "$Build\out" $file.Name
    if ((Test-Path $dst) -and (Get-FileHash $dst).Hash -eq (Get-FileHash $file.FullName).Hash) { continue }
    if (Test-Path $dst) {
        Remove-Item "$dst.old" -Force -ErrorAction SilentlyContinue
        try { Move-Item $dst "$dst.old" -Force } catch { }
    }
    Copy-Item $file.FullName $dst -Force
}
Write-Host "== done: $Build\out"
