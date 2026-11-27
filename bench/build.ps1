<#
.SYNOPSIS
  Builds any360_bench (docs/BENCH.md) with Visual Studio's clang++, as layer\sys\test.ps1 builds the layers, into its own
  build folder. The reference SDK comes from -ReferenceSdk or the first titles/<title>/pipeline.json naming one.
.EXAMPLE
  .\bench\build.ps1
  .\bench\build.ps1 -BuildDir F:\work\out-bench -Jobs 4
#>
param(
    [string]$ReferenceSdk,
    [string]$BuildDir,
    [int]$Jobs = 8,
    [string]$Target = 'any360_bench'
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $BuildDir) { $BuildDir = Join-Path $Root 'out\bench' }
if (-not $ReferenceSdk) {
    foreach ($cfg in Get-ChildItem (Join-Path $Root 'titles\*\pipeline.json')) {
        $json = Get-Content $cfg -Raw | ConvertFrom-Json
        if ($json.reference_sdk -and (Test-Path $json.reference_sdk)) { $ReferenceSdk = $json.reference_sdk; break }
    }
}
if (-not $ReferenceSdk) { throw 'No reference SDK: pass -ReferenceSdk or set "reference_sdk" in a titles/<title>/pipeline.json' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$query = @('-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', 'Microsoft.VisualStudio.Component.VC.Llvm.Clang')
$vsRoot = "$(& $vswhere @query -property installationPath)".Trim()
if (-not $vsRoot) { throw 'Visual Studio with the C++ x64 tools and C++ Clang tools is required.' }
$llvm = Join-Path $vsRoot 'VC\Tools\Llvm\x64\bin'
$cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$saved = [Environment]::GetEnvironmentVariables()
try {
    $env:PATH = (($env:PATH -split ';') | Where-Object { $_ -and $_ -notmatch 'msys64' }) -join ';'
    $env:PATH = "$env:PATH;$(Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer')"
    foreach ($line in (cmd.exe /c "`"$(Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat')`" -arch=x64 -host_arch=x64 -no_logo >nul && set")) {
        if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
    }
    $env:PATH = "$llvm;$(Split-Path $cmake);$(Split-Path $ninja);$env:PATH"
    & $cmake -S (Join-Path $Root 'layer\sys') -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
        "-DCMAKE_CXX_COMPILER:FILEPATH=$(Join-Path $llvm 'clang++.exe')" "-DCMAKE_LINKER:FILEPATH=$(Join-Path $llvm 'lld-link.exe')" `
        "-DCMAKE_MAKE_PROGRAM:FILEPATH=$ninja" "-DANY360_REFERENCE_SDK=$ReferenceSdk" | Out-Host
    if ($LASTEXITCODE) { throw "CMake configure failed ($LASTEXITCODE)" }
    & $cmake --build $BuildDir --target $Target -- -j $Jobs | Out-Host
    if ($LASTEXITCODE) { throw "Build failed ($LASTEXITCODE)" }
} finally {
    foreach ($name in @([Environment]::GetEnvironmentVariables().Keys)) {
        if (-not $saved.Contains($name)) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
    }
    foreach ($name in $saved.Keys) { Set-Item -LiteralPath "Env:$name" -Value $saved[$name] }
}
