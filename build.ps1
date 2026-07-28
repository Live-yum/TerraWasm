# Build TerraWasm with Emscripten
param(
    [ValidateSet("node", "web", "all")]
    [string]$Target = "all",
    [switch]$Quick,
    [switch]$Test,
    [switch]$AllowDirty,
    [string]$EmsdkDir = "D:\Tool\emsdk",
    [string]$DeployDir
)

$ErrorActionPreference = "Stop"
$ProjectDir = $PSScriptRoot
$BuildDir = Join-Path $ProjectDir "build"
$SourceCommit = (& git -C $ProjectDir rev-parse HEAD).Trim()
$DirtyOutput = (& git -C $ProjectDir status --porcelain -- . ':(exclude)build')
$Dirty = -not [string]::IsNullOrWhiteSpace(($DirtyOutput -join "`n"))
$DirtyFlag = if ($Dirty) { "true" } else { "false" }

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
    & emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=Release `
        "-DTERRAX_BUILD_COMMIT=$SourceCommit" `
        "-DTERRAX_BUILD_DIRTY=$DirtyFlag" `
        "-DTERRAX_BUILD_COMPILER=emscripten" `
        "-DTERRAX_BUILD_FLAGS=-O3" 2>&1
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

# --- Generate and validate the source/artifact identity manifest ---
Write-Host "=== Generating artifact manifest ===" -ForegroundColor Cyan
Push-Location $ProjectDir
& node scripts/generate-manifest.mjs --root $ProjectDir --output build/terra.manifest.json --compiler emscripten --flags '-O3'
$manifestExitCode = $LASTEXITCODE
Pop-Location
if (-not (Test-Path (Join-Path $BuildDir "terra.manifest.json"))) {
    throw "Artifact manifest was not generated"
}
$manifest = Get-Content (Join-Path $BuildDir "terra.manifest.json") -Raw | ConvertFrom-Json
if ($manifestExitCode -ne 0 -and -not $AllowDirty) {
    throw "Dirty TerraWasm builds are not publishable; use a clean checkout or -AllowDirty for local diagnostics"
}
if ($manifest.dirty -and -not $AllowDirty) {
    throw "Dirty TerraWasm builds are not publishable; use a clean checkout or -AllowDirty for local diagnostics"
}

# --- Optional explicit deployment ---
if ($DeployDir) {
    if ($manifest.dirty) {
        throw "DeployDir requires a clean TerraWasm source tree"
    }
    $boundaryDir = Split-Path -Parent $DeployDir
    $boundaryPath = @(
        (Join-Path $boundaryDir "terra-wasm.ts"),
        (Join-Path $boundaryDir "terra-wasm.js")
    ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }
    if ($boundaryPath.Count -eq 0) {
        throw "DeployDir must be the generated directory next to terra-wasm.ts"
    }
    if (-not (Test-Path -LiteralPath $DeployDir)) {
        New-Item -ItemType Directory -Path $DeployDir -Force | Out-Null
    }
    $webArtifacts = @($manifest.artifacts)
    if ($webArtifacts.Count -ne 2) {
        throw "Manifest does not contain exactly two Web deployment artifacts"
    }
    $deployManifest = $manifest | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $viewerRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $DeployDir))
    $useViewerRelativePaths = Test-Path -LiteralPath (Join-Path $viewerRoot "package.json")
    foreach ($artifact in $webArtifacts) {
        $source = Join-Path $ProjectDir $artifact.path
        $name = Split-Path -Leaf $artifact.path
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Manifest artifact is missing: $($artifact.path)"
        }
        Copy-Item -LiteralPath $source -Destination (Join-Path $DeployDir $name) -Force
        $manifestPath = if ($useViewerRelativePaths) { "infrastructure/wasm/generated/$name" } else { $name }
        $artifact.path = $manifestPath
        $deployArtifact = @($deployManifest.artifacts | Where-Object { $_.role -eq $artifact.role })[0]
        $deployArtifact.path = $manifestPath
        $deployWebArtifact = @($deployManifest.targets.web.artifacts | Where-Object { $_.role -eq $artifact.role })[0]
        $deployWebArtifact.path = $manifestPath
    }
    $deployManifest | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $DeployDir "terra.manifest.json") -Encoding utf8
    Write-Host "=== Copied to $DeployDir ===" -ForegroundColor Cyan
}

# --- Test ---
if ($Test) {
    if ($Target -eq "web") {
        throw "-Test requires the node or all target"
    }
    Write-Host "`n=== Running tests ===" -ForegroundColor Cyan
    Push-Location $ProjectDir
    node --test tests/test_memory_lifecycle.js tests/test_buffer_io.js tests/test_batch_update_thumbnail.js tests/test_build_contract.js tests/test_commands.js tests/test_manifest_contract.js tests/test_marker_outputs.js tests/test_open_task.js tests/test_reader_safety.js tests/test_pixel_art_bulk.js tests/test_pixel_art_indexed.js tests/test_section_mutators.js tests/test_sha256.js 2>&1
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "Node regression tests failed" }
    node tests/test_all.js 2>&1
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "Legacy operation suite failed" }
    Pop-Location
}
