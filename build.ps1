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
    [string]$OptimizeFlag = "-O3",
    [switch]$EnableLto,
    [string]$DeployDir
)

$ErrorActionPreference = "Stop"
$ProjectDir = $PSScriptRoot
$BuildDir = Join-Path $ProjectDir "build"
$SourceCommit = (& git -C $ProjectDir rev-parse HEAD).Trim()
$DirtyOutput = (& git -C $ProjectDir status --porcelain -- . ':(exclude)build')
$SourceState = $DirtyOutput -join "`n"
$Dirty = -not [string]::IsNullOrWhiteSpace($SourceState)
$DirtyFlag = if ($Dirty) { "true" } else { "false" }
$EnableLtoFlag = if ($EnableLto) { "ON" } else { "OFF" }
$NodeInitialMemory = 134217728
$NodeMaximumMemory = 536870912
$WebInitialMemory = 67108864
$WebMaximumMemory = 167772160
switch ($Features) {
    "all" {
        $NodeExportsFile = "exports.txt"
        $WebExportsFile = "exports.web.txt"
    }
    "wld" {
        $NodeExportsFile = "exports.wld.txt"
        $WebExportsFile = "exports.wld.web.txt"
    }
    "plr" {
        $NodeExportsFile = "exports.plr.txt"
        $WebExportsFile = "exports.plr.txt"
    }
}
$CommonFlags = @(
    $OptimizeFlag,
    "-fno-exceptions",
    "-fno-rtti"
)
if ($Features -ne "plr") {
    $CommonFlags += "-sUSE_ZLIB=1"
}
$CommonFlags += @(
    "-sALLOW_MEMORY_GROWTH=1",
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','stringToUTF8','lengthBytesUTF8','getValue','setValue','HEAPU8','HEAPU32','HEAP32','HEAPF32','HEAPF64','FS','stackAlloc','stackSave','stackRestore','wasmMemory']",
    "-sMODULARIZE=1",
    "-sERROR_ON_UNDEFINED_SYMBOLS=1",
    "--no-entry"
)
if ($EnableLto) {
    $CommonFlags += "-flto"
}
$NodeFlags = @(
    "-sEXPORTED_FUNCTIONS=@exported_functions_node.json",
    "-sINITIAL_MEMORY=$NodeInitialMemory",
    "-sMAXIMUM_MEMORY=$NodeMaximumMemory",
    "-sEXPORT_NAME='TerraWorldWasm'",
    "-sENVIRONMENT=node",
    "-sNODERAWFS=1",
    "-sFILESYSTEM=1"
)
$WebFlags = @(
    "-sEXPORTED_FUNCTIONS=@exported_functions_web.json",
    "-sINITIAL_MEMORY=$WebInitialMemory",
    "-sMAXIMUM_MEMORY=$WebMaximumMemory",
    "-sEXPORT_NAME='TerraWorldWasmWeb'",
    "-sENVIRONMENT=web,worker",
    "-sFILESYSTEM=1"
)

function Add-FlagArgs {
    param(
        [System.Collections.Generic.List[string]]$Arguments,
        [string]$Option,
        [string[]]$Values
    )
    foreach ($value in $Values) {
        $Arguments.Add($Option)
        $Arguments.Add($value)
    }
}

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
    foreach ($expected in @(
        "TERRAX_BUILD_COMMIT:STRING=$SourceCommit",
        "TERRAX_BUILD_DIRTY:STRING=$DirtyFlag",
        "TERRAWASM_FEATURE_SET:STRING=$Features",
        "TERRAX_OPTIMIZE_FLAG:STRING=$OptimizeFlag",
        "TERRAX_ENABLE_LTO:BOOL=$EnableLtoFlag",
        "TERRAX_NODE_INITIAL_MEMORY:STRING=$NodeInitialMemory",
        "TERRAX_NODE_MAXIMUM_MEMORY:STRING=$NodeMaximumMemory",
        "TERRAX_WEB_INITIAL_MEMORY:STRING=$WebInitialMemory",
        "TERRAX_WEB_MAXIMUM_MEMORY:STRING=$WebMaximumMemory"
    )) {
        if (-not $cache.Contains($expected)) {
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
    & emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=Release `
        "-DTERRAX_BUILD_COMMIT=$SourceCommit" `
        "-DTERRAX_BUILD_DIRTY=$DirtyFlag" `
        "-DTERRAX_BUILD_COMPILER=emscripten" `
        "-DTERRAWASM_FEATURE_SET=$Features" `
        "-DTERRAX_OPTIMIZE_FLAG=$OptimizeFlag" `
        "-DTERRAX_ENABLE_LTO=$EnableLtoFlag" `
        "-DTERRAX_NODE_INITIAL_MEMORY=$NodeInitialMemory" `
        "-DTERRAX_NODE_MAXIMUM_MEMORY=$NodeMaximumMemory" `
        "-DTERRAX_WEB_INITIAL_MEMORY=$WebInitialMemory" `
        "-DTERRAX_WEB_MAXIMUM_MEMORY=$WebMaximumMemory" 2>&1
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

# The provenance manifest records both targets, even when the caller only
# consumes one of them. Always refresh the pair so a clean build never reuses
# a missing or stale sibling artifact.
$buildTargets = @("terrax_world_wasm", "terrax_world_wasm_web")

foreach ($t in $buildTargets) {
    Build-Target -Name $t
}

# Refuse artifacts if the source changed while compilation was in flight.
$CurrentSourceCommit = (& git -C $ProjectDir rev-parse HEAD).Trim()
$CurrentSourceState = ((& git -C $ProjectDir status --porcelain -- . ':(exclude)build') -join "`n")
if ($CurrentSourceCommit -ne $SourceCommit -or $CurrentSourceState -ne $SourceState) {
    throw "TerraWasm source changed during build; discard the artifacts and rebuild"
}

# --- Generate and validate the source/artifact identity manifest ---
Write-Host "=== Generating artifact manifest ===" -ForegroundColor Cyan
Push-Location $ProjectDir
$manifestArgs = [System.Collections.Generic.List[string]]::new()
$manifestArgs.Add("scripts/generate-manifest.mjs")
$manifestArgs.Add("--root")
$manifestArgs.Add($ProjectDir)
$manifestArgs.Add("--output")
$manifestArgs.Add("build/terra.manifest.json")
$manifestArgs.Add("--compiler")
$manifestArgs.Add("emscripten")
$manifestArgs.Add("--feature-set")
$manifestArgs.Add($Features)
$manifestArgs.Add("--node-exports-file")
$manifestArgs.Add($NodeExportsFile)
$manifestArgs.Add("--web-exports-file")
$manifestArgs.Add($WebExportsFile)
$manifestArgs.Add("--source-commit")
$manifestArgs.Add($SourceCommit)
$manifestArgs.Add("--dirty")
$manifestArgs.Add($DirtyFlag)
if ($AllowDirty) { $manifestArgs.Add("--allow-dirty") }
Add-FlagArgs -Arguments $manifestArgs -Option "--common-flag" -Values $CommonFlags
Add-FlagArgs -Arguments $manifestArgs -Option "--node-flag" -Values $NodeFlags
Add-FlagArgs -Arguments $manifestArgs -Option "--web-flag" -Values $WebFlags
& node @manifestArgs
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
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    $deployJson = $deployManifest | ConvertTo-Json -Depth 20
    [System.IO.File]::WriteAllText(
        (Join-Path $DeployDir "terra.manifest.json"),
        "$deployJson`n",
        $utf8NoBom
    )
    if ($useViewerRelativePaths) {
        $browserManifestPath = Join-Path $boundaryDir "terra-manifest-browser.mjs"
        $browserJson = $deployManifest | ConvertTo-Json -Depth 20 -Compress
        $browserModule = "const manifest = $browserJson`n`nexport default manifest`n"
        [System.IO.File]::WriteAllText($browserManifestPath, $browserModule, $utf8NoBom)
    }
    Write-Host "=== Copied to $DeployDir ===" -ForegroundColor Cyan
}

# --- Test ---
if ($Test) {
    if ($Target -eq "web") {
        throw "-Test requires the node or all target"
    }
    Write-Host "`n=== Running $Features feature tests ===" -ForegroundColor Cyan
    $commonTests = @(
        "tests/test_build_contract.js",
        "tests/test_build_identity.js",
        "tests/test_ci_contract.js",
        "tests/test_manifest_contract.js",
        "tests/test_artifact_size_contract.js",
        "tests/test_feature_set.js"
    )
    $wldTests = @(
        "tests/test_memory_lifecycle.js",
        "tests/test_map_streaming_contract.js",
        "tests/test_buffer_io.js",
        "tests/test_batch_update_thumbnail.js",
        "tests/test_commands.js",
        "tests/test_marker_outputs.js",
        "tests/test_icon_atlas_bridge.js",
        "tests/test_open_task.js",
        "tests/test_reader_safety.js",
        "tests/test_pixel_art_bulk.js",
        "tests/test_pixel_art_indexed.js",
        "tests/test_section_mutators.js",
        "tests/test_signs.js",
        "tests/test_sha256.js",
        "tests/test_fixture_contract.js"
    )
    $regressionTests = @($commonTests)
    if ($Features -ne "plr") { $regressionTests += $wldTests }
    if ($Features -ne "wld") { $regressionTests += "tests/test_plr.js" }

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
