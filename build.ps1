# Build TerraWasm with Emscripten
param(
    [ValidateSet("node", "web", "all")]
    [string]$Target = "all",
    [ValidateSet("all", "wld", "plr")]
    [string]$Features = "all",
    [switch]$Quick,
    [switch]$Test,
    [switch]$AllowDirty,
    [string]$EmsdkDir = "D:\Tool\emsdk",
    [ValidateSet("-O0", "-O1", "-O2", "-O3", "-Os", "-Oz")]
    [string]$OptimizeFlag,
    [switch]$EnableLto
)

$ErrorActionPreference = "Stop"
$ProjectDir = $PSScriptRoot
$BuildDir = Join-Path $ProjectDir "build"
$SourceCommit = (& git -C $ProjectDir rev-parse HEAD).Trim()
$DirtyOutput = (& git -C $ProjectDir status --porcelain -- . ':(exclude)build')
$SourceState = $DirtyOutput -join "`n"
$Dirty = -not [string]::IsNullOrWhiteSpace($SourceState)
$DirtyFlag = if ($Dirty) { "true" } else { "false" }
# Match CMake's measured all-only compact profile. Callers can override either
# setting explicitly, including -EnableLto:$false for controlled comparisons.
if (-not $OptimizeFlag) { $OptimizeFlag = if ($Features -eq "all") { "-Oz" } else { "-O3" } }
if (-not $PSBoundParameters.ContainsKey("EnableLto")) { $EnableLto = ($Features -eq "all") }
$EnableLtoFlag = if ($EnableLto) { "ON" } else { "OFF" }
$CmakeOptions = [ordered]@{
    TERRAX_BUILD_COMMIT = $SourceCommit
    TERRAX_BUILD_DIRTY = $DirtyFlag
    TERRAX_BUILD_COMPILER = "emscripten"
    TERRAWASM_FEATURE_SET = $Features
    TERRAX_ENABLE_LTO = $EnableLtoFlag
}
if ($OptimizeFlag) { $CmakeOptions.TERRAX_OPTIMIZE_FLAG = $OptimizeFlag }

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

# Quick mode is safe only when the cached build identity exactly matches this invocation.
if ($Quick) {
    $cachePath = Join-Path $BuildDir "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
        throw "Quick build requires an existing CMake cache"
    }
    $cache = Get-Content -LiteralPath $cachePath -Raw
    foreach ($option in $CmakeOptions.GetEnumerator()) {
        $expected = "(?m)^$([regex]::Escape($option.Key)):[^=]+=$([regex]::Escape($option.Value))`r?$"
        if ($cache -notmatch $expected) {
            throw "Quick build cache identity does not match the current source/options; rerun without -Quick"
        }
    }
}

# --- Configure (skip in Quick mode) ---
if (-not $Quick) {
    Write-Host "=== Configuring CMake ===" -ForegroundColor Cyan
    if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
    New-Item -ItemType Directory -Path $BuildDir | Out-Null

    Push-Location $ProjectDir
    $configureArgs = @($CmakeOptions.GetEnumerator() | ForEach-Object { "-D$($_.Key)=$($_.Value)" })
    & emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=Release @configureArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Error "CMake configure failed"
        Pop-Location
        exit 1
    }
    Pop-Location
}

# Both targets belong to one release identity; CMake's default target builds the pair.
& cmake --build $BuildDir --config Release 2>&1
if ($LASTEXITCODE -ne 0) { throw "WASM build failed" }

# Refuse artifacts if the source changed while compilation was in flight.
$CurrentSourceCommit = (& git -C $ProjectDir rev-parse HEAD).Trim()
$CurrentSourceState = ((& git -C $ProjectDir status --porcelain -- . ':(exclude)build') -join "`n")
if ($CurrentSourceCommit -ne $SourceCommit -or $CurrentSourceState -ne $SourceState) {
    throw "TerraWasm source changed during build; discard the artifacts and rebuild"
}

# --- Generate and validate the source/artifact identity manifest ---
Write-Host "=== Generating artifact manifest ===" -ForegroundColor Cyan
Push-Location $ProjectDir
$manifestArgs = @("scripts/generate-manifest.mjs", "--root", $ProjectDir,
    "--output", "build/terra.manifest.json", "--source-commit", $SourceCommit, "--dirty", $DirtyFlag)
if ($AllowDirty) { $manifestArgs += "--allow-dirty" }
& node @manifestArgs
$manifestExitCode = $LASTEXITCODE
Pop-Location
if ($manifestExitCode -ne 0) { throw "Artifact manifest validation failed" }
if ($Target -ne "node") {
    Push-Location $ProjectDir
    if ($AllowDirty) {
        & node scripts/check-artifact-size.mjs build/terra.manifest.json --allow-dirty
    } else {
        & node scripts/check-artifact-size.mjs build/terra.manifest.json
    }
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "Web artifact size gate failed" }
    Pop-Location
}

# --- Test ---
if ($Test) {
    if ($Target -eq "web") {
        throw "-Test requires the node or all target"
    }
    Write-Host "`n=== Running $Features feature tests ===" -ForegroundColor Cyan
    $commonTests = @(
        "tests/test_build_identity.js",
        "tests/test_manifest_contract.js",
        "tests/test_artifact_size_contract.js",
        "tests/test_feature_set.js",
        "tests/test_stream_abi_manifest.mjs"
    )
    $wldTests = @(
        "tests/test_memory_lifecycle.js",
        "tests/test_map_streaming_contract.js",
        "tests/test_stream_lease.js",
        "tests/test_stream_world.js",
        "tests/test_stream_operations.js",
        "tests/test_buffer_io.js",
        "tests/test_batch_update_thumbnail.js",
        "tests/test_commands.js",
        "tests/test_marker_outputs.js",
        "tests/test_removed_api_contract.js",
        "tests/test_icon_atlas_bridge.js",
        "tests/test_open_task.js",
        "tests/test_future_readonly.js",
        "tests/test_wld_legacy.js",
        "tests/test_wld_header_versions.js",
        "tests/test_wld_legacy_api.js",
        "tests/test_reader_safety.js",
        "tests/test_pixel_art_bulk.js",
        "tests/test_pixel_art_indexed.js",
        "tests/test_pixel_art_materials.js",
        "tests/test_pixel_art_semantics.js",
        "tests/test_section_mutators.js",
        "tests/test_signs.js",
        "tests/test_sha256.js",
        "tests/test_circuit.js",
        "tests/test_fixture_contract.js"
    )
    $regressionTests = @($commonTests)
    if ($Features -ne "plr") { $regressionTests += $wldTests }
    if ($Features -ne "wld") { $regressionTests += @("tests/test_plr.js", "tests/test_plr_real_fixture.js") }

    Push-Location $ProjectDir
    & node --test @regressionTests 2>&1
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "Node regression tests failed" }
    if ($Features -ne "plr") {
        & node tests/test_all.js 2>&1
        if ($LASTEXITCODE -ne 0) { Pop-Location; throw "Legacy operation suite failed" }
        & node bench/map_8400x2400.js 2>&1
        if ($LASTEXITCODE -ne 0) { Pop-Location; throw "8400x2400 MAP benchmark failed" }
    }
    Pop-Location
}
