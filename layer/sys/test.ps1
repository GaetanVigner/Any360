<#
.SYNOPSIS
  Builds any360::sys and runs sys_io_tests against the reference runtime, with Visual Studio's clang++ (the toolchain
  the titles use). The reference SDK install (include/, lib/, bin/ of a ReXGlue SDK) comes from -ReferenceSdk or from
  the first titles/<title>/pipeline.json that names "reference_sdk".
.EXAMPLE
  .\layer\sys\test.ps1
  .\layer\sys\test.ps1 -ReferenceSdk F:\...\tools\rexglue\win-amd64 -Trials 200
#>
param(
    [string]$ReferenceSdk,
    [int]$Trials = 64,
    [string]$BuildDir
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $BuildDir) { $BuildDir = Join-Path $Root 'out\sys' }
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
    & $cmake --build $BuildDir | Out-Host
    if ($LASTEXITCODE) { throw "Build failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'sys_object_tests.exe') | Out-Host
    if ($LASTEXITCODE) { throw "sys_object_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'gfx/gfx_cp_tests.exe') | Out-Host
    if ($LASTEXITCODE) { throw "gfx_cp_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'gfx/gfx_shader_tests.exe') | Out-Host
    if ($LASTEXITCODE) { throw "gfx_shader_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'gfx/gfx_vulkan_tests.exe') | Out-Host
    if ($LASTEXITCODE) { throw "gfx_vulkan_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'gfx/gfx_render_tests.exe') | Out-Host
    if ($LASTEXITCODE) { throw "gfx_render_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'gfx/gfx_render_tests.exe') --copy | Out-Host
    if ($LASTEXITCODE) { throw "gfx_render_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'sys_io_tests.exe') --trials $Trials | Out-Host
    if ($LASTEXITCODE) { throw "sys_io_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'sys_runtime_tests.exe') --trials $Trials | Out-Host
    if ($LASTEXITCODE) { throw "sys_runtime_tests failed ($LASTEXITCODE)" }
} finally {
    foreach ($name in @([Environment]::GetEnvironmentVariables().Keys)) {
        if (-not $saved.Contains($name)) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
    }
    foreach ($name in $saved.Keys) { Set-Item -LiteralPath "Env:$name" -Value $saved[$name] }
}
