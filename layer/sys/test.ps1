<#
.SYNOPSIS
  Builds any360::sys and runs sys_io_tests against the reference runtime, with Visual Studio's clang++ (the toolchain
  the titles use). The reference SDK install (include/, lib/, bin/ of a ReXGlue SDK) comes from -ReferenceSdk or from
  the first titles/<title>/pipeline.json that names "reference_sdk".
  When the Vulkan validation layer is installed (the Vulkan SDK), the GPU tests run once more under it, on each GPU
  of -Gpus (a substring of the device name; '' is the one the tests choose), and any validation message fails the
  run: what one driver lets through another refuses (AMD's refused a descriptor pool NVIDIA's did not).
.EXAMPLE
  .\layer\sys\test.ps1
  .\layer\sys\test.ps1 -ReferenceSdk F:\...\tools\rexglue\win-amd64 -Trials 200
  .\layer\sys\test.ps1 -Gpus NVIDIA,Radeon
#>
param(
    [string]$ReferenceSdk,
    [int]$Trials = 64,
    [string]$BuildDir,
    [string[]]$Gpus = @(''),
    [switch]$NoValidation
)
$ErrorActionPreference = 'Stop'
$Gpus = @($Gpus | ForEach-Object { $_ -split ',' })  # -File passes NVIDIA,Radeon as one string
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
    & (Join-Path $BuildDir 'sys_net_tests.exe') | Out-Host
    if ($LASTEXITCODE) { throw "sys_net_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'xdk/xdk_crt_tests.exe') | Out-Host
    if ($LASTEXITCODE) { throw "xdk_crt_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'xdk/xdk_xapi_tests.exe') | Out-Host
    if ($LASTEXITCODE) { throw "xdk_xapi_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'xdk/xdk_xaudio_tests.exe') | Out-Host
    if ($LASTEXITCODE) { throw "xdk_xaudio_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'xdk/xdk_d3d_tests.exe') | Out-Host
    if ($LASTEXITCODE) { throw "xdk_d3d_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'xdk/xdk_d3d_state_tests.exe') | Out-Host
    if ($LASTEXITCODE) { throw "xdk_d3d_state_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'gfx/gfx_cp_tests.exe') | Out-Host
    if ($LASTEXITCODE) { throw "gfx_cp_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'gfx/gfx_write_back_tests.exe') | Out-Host
    if ($LASTEXITCODE) { throw "gfx_write_back_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'gfx/gfx_shader_tests.exe') | Out-Host
    if ($LASTEXITCODE) { throw "gfx_shader_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'gfx/gfx_vulkan_tests.exe') | Out-Host
    if ($LASTEXITCODE) { throw "gfx_vulkan_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'gfx/gfx_render_tests.exe') | Out-Host
    if ($LASTEXITCODE) { throw "gfx_render_tests failed ($LASTEXITCODE)" }
    $capture = Join-Path $BuildDir 'gfx/gfx_render_tests.a360cap'
    & (Join-Path $BuildDir 'gfx/gfx_render_tests.exe') --copy --capture $capture | Out-Host
    if ($LASTEXITCODE) { throw "gfx_render_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'gfx/gfx_replay.exe') $capture | Out-Host
    if ($LASTEXITCODE) { throw "gfx_replay failed ($LASTEXITCODE)" }
    # The GPU tests under the Vulkan validation layer, on each GPU asked for: no validation message allowed.
    $layers = @()
    $key = 'HKLM:\SOFTWARE\Khronos\Vulkan\ExplicitLayers'
    if (Test-Path $key) { $layers = @((Get-Item $key).Property | Where-Object { $_ -like '*VkLayer_khronos_validation.json' }) }
    if (-not $NoValidation -and $layers.Count) {
        $env:VK_INSTANCE_LAYERS = 'VK_LAYER_KHRONOS_validation'
        try {
            foreach ($gpu in $Gpus) {
                foreach ($test in 'gfx_vulkan_tests', 'gfx_render_tests', 'gfx_resource_tests') {
                    $test_args = @(); if ($gpu) { $test_args += $gpu }
                    $out = & (Join-Path $BuildDir "gfx/$test.exe") @test_args 2>&1 | ForEach-Object { "$_" }
                    if ($LASTEXITCODE) { $out | Out-Host; throw "$test failed under validation on '$gpu' ($LASTEXITCODE)" }
                    $errors = @($out | Select-String -SimpleMatch 'Validation Error')
                    $device = ($out | Select-String -Pattern '(NVIDIA|AMD|Intel|Radeon)[^,(]*' | Select-Object -First 1).Matches.Value
                    "$($test): validation layer on $(if ($device) { $device } elseif ($gpu) { $gpu } else { 'the default GPU' }), $($errors.Count) validation messages" | Out-Host
                    if ($errors.Count) { ($out | Select-String -SimpleMatch 'VUID' | Select-Object -First 5) | Out-Host; throw "$($test): validation errors on '$gpu'" }
                }
            }
        } finally {
            Remove-Item Env:VK_INSTANCE_LAYERS -ErrorAction SilentlyContinue
        }
    }
    & (Join-Path $BuildDir 'sys_io_tests.exe') --trials $Trials | Out-Host
    if ($LASTEXITCODE) { throw "sys_io_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'sys_runtime_tests.exe') --trials $Trials | Out-Host
    if ($LASTEXITCODE) { throw "sys_runtime_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'host/rexglue/host_rexglue_tests.exe') | Out-Host
    if ($LASTEXITCODE) { throw "host_rexglue_tests failed ($LASTEXITCODE)" }
    & (Join-Path $BuildDir 'host/rexglue/host_system_tests.exe') | Out-Host
    if ($LASTEXITCODE) { throw "host_system_tests failed ($LASTEXITCODE)" }
} finally {
    foreach ($name in @([Environment]::GetEnvironmentVariables().Keys)) {
        if (-not $saved.Contains($name)) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
    }
    foreach ($name in $saved.Keys) { Set-Item -LiteralPath "Env:$name" -Value $saved[$name] }
}
