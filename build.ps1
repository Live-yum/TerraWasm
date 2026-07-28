# Build TerraWasm with Emscripten
param(
    [ValidateSet("node", "web", "all")]
    [string]$Target = "all",
    [switch]$Quick,
    [switch]$Test,
    [string]$EmsdkDir = "D:\Tool\emsdk",
    [string]$DeployDir
)

$ErrorActionPreference = "Stop"
$ProjectDir = $PSScriptRoot
$BuildDir = Join-Path $ProjectDir "build"

# --- Activate Emscripten ---
$env:EMSDK_QUIET = 1
$emsdkEnv = Join-Path $EmsdkDir "emsdk_env.ps1"
if (-not (Test-Path $emsdkEnv)) {
    Write-Error "Emscripten SDK not found at $EmsdkDir"
    exit 1
}
Write-Host "=== Activating Emscripten ===" -ForegroundColor Cyan
Push-Location $EmsdkDir
& $emsdkEnv 2>$null
Pop-Location

# --- Configure (skip in Quick mode) ---
if (-not $Quick) {
    Write-Host "=== Configuring CMake ===" -ForegroundColor Cyan
    if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
    New-Item -ItemType Directory -Path $BuildDir | Out-Null

    Push-Location $ProjectDir
    & emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=Release 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Error "CMake configure failed"
        Pop-Location
        exit 1
    }
    Pop-Location
}

# --- Build ---
function Build-Target {
    param([string]$Name)
    Write-Host "=== Building $Name ===" -ForegroundColor Cyan
    Push-Location $ProjectDir
    & cmake --build build --config Release --target $Name 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Error "$Name build failed"
        Pop-Location
        exit 1
    }
    Pop-Location

    $jsFile = Join-Path $BuildDir "$Name.js"
    $wasmFile = Join-Path $BuildDir "$Name.wasm"
    if ((Test-Path $jsFile) -and (Test-Path $wasmFile)) {
        $jsSize = [math]::Round((Get-Item $jsFile).Length / 1KB, 1)
        $wasmSize = [math]::Round((Get-Item $wasmFile).Length / 1MB, 2)
        Write-Host "=== $Name OK ($jsSize KB js, $wasmSize MB wasm) ===" -ForegroundColor Green
    } else {
        Write-Error "$Name output not found"
        exit 1
    }
}

$buildTargets = switch ($Target) {
    "node" { @("terrax_world_wasm") }
    "web"  { @("terrax_world_wasm_web") }
    "all"  { @("terrax_world_wasm", "terrax_world_wasm_web") }
}

foreach ($t in $buildTargets) {
    Build-Target -Name $t
}

# --- Optional explicit deployment ---
if ($DeployDir) {
    $boundaryPath = Join-Path (Split-Path -Parent $DeployDir) "terra-wasm.js"
    if (-not (Test-Path -LiteralPath $boundaryPath -PathType Leaf)) {
        throw "DeployDir must be the wasm directory next to terra-wasm.js"
    }
    if (-not (Test-Path -LiteralPath $DeployDir)) {
        New-Item -ItemType Directory -Path $DeployDir -Force | Out-Null
    }
    $deployTargets = @($buildTargets | Where-Object { $_ -eq "terrax_world_wasm_web" })
    if ($deployTargets.Count -eq 0) {
        throw "DeployDir requires the web or all target"
    }
    foreach ($t in $deployTargets) {
        $js = Join-Path $BuildDir "$t.js"
        $wasm = Join-Path $BuildDir "$t.wasm"
        Copy-Item -LiteralPath $js -Destination $DeployDir -Force
        Copy-Item -LiteralPath $wasm -Destination $DeployDir -Force
    }
    Write-Host "=== Copied to $DeployDir ===" -ForegroundColor Cyan
}

# --- Test ---
if ($Test) {
    if ($Target -eq "web") {
        throw "-Test requires the node or all target"
    }
    Write-Host "`n=== Running tests ===" -ForegroundColor Cyan
    Push-Location $ProjectDir
    node --test tests/test_memory_lifecycle.js tests/test_buffer_io.js tests/test_batch_update_thumbnail.js tests/test_build_contract.js tests/test_marker_outputs.js tests/test_pixel_art_bulk.js tests/test_pixel_art_indexed.js tests/test_section_mutators.js tests/test_sha256.js 2>&1
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "Node regression tests failed" }
    node tests/test_all.js 2>&1
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "Legacy operation suite failed" }
    Pop-Location
}
