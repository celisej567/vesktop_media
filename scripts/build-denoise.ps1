# Windows build of the noise suppression libraries (scripts/build-denoise.sh on Linux): RNNoise (C, BSD) as
# vesktop_rnnoise.dll and DeepFilterNet (Rust, MIT / Apache-2.0) as vesktop_deepfilter.dll, into
# third_party\denoise\prefix\lib. Loaded from the module's own directory only (see audio/rnnoise.cc).
# Runs inside scripts/build.ps1 (the MSVC environment and LLVM are set up there). Needs git, curl, tar, cargo.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Root = Split-Path -Parent $PSScriptRoot
$TP = Join-Path $Root 'third_party\denoise'
$Prefix = Join-Path $TP 'prefix\lib'

$RnnoiseTag = 'v0.2'
$DeepfilterTag = 'v0.5.6'

function Run([string]$exe) {
    & $exe @args
    if ($LASTEXITCODE -ne 0) { throw "$exe $args failed ($LASTEXITCODE)" }
}

# our changes to DeepFilterNet's C API (scripts/patches): a library built from other ones is built again
$DfPatch = Join-Path $Root 'scripts\patches\deepfilter-capi.patch'
$DfStamp = "deepfilter $DeepfilterTag " + (Get-FileHash $DfPatch -Algorithm SHA256).Hash
$DfDll = Join-Path $Prefix 'vesktop_deepfilter.dll'
$RnDll = Join-Path $Prefix 'vesktop_rnnoise.dll'
if ((Test-Path $DfDll) -and ((Get-Content "$Prefix\.deepfilter-stamp" -ErrorAction SilentlyContinue) -ne $DfStamp)) { Remove-Item $DfDll }
if ((Test-Path $RnDll) -and (Test-Path $DfDll)) {
    Write-Host "== denoise libraries already built: $Prefix"
    exit 0
}
New-Item -ItemType Directory -Force "$TP\src", "$TP\build", $Prefix | Out-Null

function Fetch([string]$dir, [string]$url, [string]$tag) {
    if (Test-Path "$TP\src\$dir\.git") { return }
    # without line-ending conversion: the patch (and the sources' own scripts) have LF line endings
    Run git -c core.autocrlf=false clone -q --depth 1 --branch $tag $url "$TP\src\$dir"
}

if (-not (Test-Path $RnDll)) {
    Write-Host "== RNNoise $RnnoiseTag"
    Fetch rnnoise https://github.com/xiph/rnnoise.git $RnnoiseTag
    $rn = "$TP\src\rnnoise"
    # the model (autogen.sh's job on Linux): the tarball holds src/rnnoise_data.{c,h}
    $hash = (Get-Content "$rn\model_version" -Raw).Trim()
    $model = "$rn\rnnoise_data-$hash.tar.gz"
    if (-not (Test-Path $model)) { Run curl.exe -fsSL -o $model "https://media.xiph.org/rnnoise/models/rnnoise_data-$hash.tar.gz" }
    if (-not (Test-Path "$rn\src\rnnoise_data.c") -or -not (Select-String -Path "$rn\src\rnnoise_data.c" -Pattern 'rnnoise_arrays' -Quiet)) {
        Run tar.exe -xzf $model -C $rn
    }
    # the portable C code (no x86 run-time dispatch); only rnnoise_* is exported (RNNOISE_EXPORT)
    $sources = 'denoise', 'rnn', 'pitch', 'kiss_fft', 'celt_lpc', 'nnet', 'nnet_default', 'parse_lpcnet_weights',
        'rnnoise_data', 'rnnoise_tables' | ForEach-Object { "$rn\src\$_.c" }
    $obj = "$TP\build\rnnoise"
    New-Item -ItemType Directory -Force $obj | Out-Null
    Run clang-cl /nologo /O2 /MT /c /DWIN32 /DRNNOISE_BUILD /DDLL_EXPORT /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS `
        "/I$rn\include" "/I$rn\src" "/clang:-ffile-prefix-map=$TP\=" -Wno-everything "/Fo$obj\" @sources
    Run lld-link /nologo /dll /out:$RnDll /opt:ref /opt:icf (Get-ChildItem "$obj\*.obj" | ForEach-Object FullName)
    Remove-Item "$Prefix\vesktop_rnnoise.lib", "$Prefix\vesktop_rnnoise.exp" -ErrorAction SilentlyContinue
}

if (-not (Test-Path $DfDll)) {
    Write-Host "== DeepFilterNet $DeepfilterTag"
    Fetch DeepFilterNet https://github.com/Rikorose/DeepFilterNet.git $DeepfilterTag
    $df = "$TP\src\DeepFilterNet"
    # the C API, patched (see build-denoise.sh): df_create(NULL) loads the built-in model, panics return NULL / NaN
    # through cmd: PowerShell would take git's message on stderr for an error
    cmd /c "git -C `"$df`" apply --reverse --check `"$DfPatch`" 2>nul"
    if ($LASTEXITCODE -ne 0) {
        Run git -C $df checkout -q -- libDF/src/capi.rs
        Run git -C $df apply $DfPatch
    }
    $cargoHome = if ($env:CARGO_HOME) { $env:CARGO_HOME } else { Join-Path $env:USERPROFILE '.cargo' }
    $sysroot = (& rustc --print sysroot).Trim()
    $env:RUSTFLAGS = "--remap-path-prefix=$df=deepfilter --remap-path-prefix=$cargoHome=cargo --remap-path-prefix=$sysroot=rust -C strip=symbols -C target-feature=+crt-static"
    $env:CARGO_TARGET_DIR = "$TP\build\deepfilter"
    Push-Location $df
    try {
        # the locked `time` 0.3.x does not compile with Rust 1.80 or newer: the first release that does
        Run cargo update -q -p time --precise 0.3.36
        Run cargo rustc --release --locked -p deep_filter --lib --crate-type cdylib --no-default-features --features capi
    } finally {
        Pop-Location
        Remove-Item env:RUSTFLAGS, env:CARGO_TARGET_DIR
    }
    Copy-Item "$TP\build\deepfilter\release\df.dll" $DfDll -Force
    Set-Content "$Prefix\.deepfilter-stamp" $DfStamp
}
Get-ChildItem $Prefix -Filter *.dll | ForEach-Object { "$($_.Name) $($_.Length)" }
Write-Host "== done: $Prefix"
