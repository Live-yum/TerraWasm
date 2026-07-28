$ErrorActionPreference = "Stop"
$env:EMSDK_QUIET = 1
$ProjectDir = "C:\Users\depths\Desktop\Tdecoder\TerraWasm"
$BuildDir = Join-Path $ProjectDir "build"

# Source emsdk environment
Push-Location "D:\Tool\emsdk"
& .\emsdk_env.ps1 2>$null
Pop-Location

# Configure and build Node target
if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
New-Item -ItemType Directory -Path $BuildDir | Out-Null

Push-Location $ProjectDir
& emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=Release 2>&1
& cmake --build build --config Release --target terrax_world_wasm 2>&1
Pop-Location

$jsFile = Join-Path $BuildDir "terrax_world_wasm.js"
$wasmFile = Join-Path $BuildDir "terrax_world_wasm.wasm"
if ((Test-Path $jsFile) -and (Test-Path $wasmFile)) {
    Write-Host "=== Node build succeeded ===" -ForegroundColor Green
    # Run test
    Write-Host "=== Running thumbnail test ===" -ForegroundColor Cyan
    node (Join-Path $ProjectDir "tests\test_thumbnail.js")
} else {
    Write-Error "Build output not found"
}
