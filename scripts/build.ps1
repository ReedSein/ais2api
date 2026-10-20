# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/windows',
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Configuration = 'Release',
    [ValidateRange(1, 256)][int]$Jobs = 2,
    [switch]$Package,
    [switch]$SkipTests,
    [switch]$Help
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($Help) {
    Write-Output 'Usage: ./scripts/build.ps1 [-BuildDirectory build/windows] [-VcpkgRoot PATH] [-Configuration Release] [-Jobs 2] [-Package] [-SkipTests]'
    Write-Output 'Requires Visual Studio C++ tools, Git and CMake/Ninja. New dependencies go under build/deps; existing .tools is not changed.'
    return
}
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Native command failed: $Program (exit $LASTEXITCODE)" }
}
$project = Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $project $BuildDirectory }
if ([string]::IsNullOrEmpty($VcpkgRoot)) { $VcpkgRoot = Join-Path $project 'build/deps/vcpkg' }
if (-not [IO.Path]::IsPathRooted($VcpkgRoot)) { $VcpkgRoot = Join-Path $project $VcpkgRoot }
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue) -or [string]::IsNullOrEmpty($env:INCLUDE)) {
    $locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $locator)) { throw 'Install Visual Studio C++ build tools or use Developer PowerShell.' }
    $installation = & $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'Visual Studio x64 C++ tools were not found.' }
    & (Join-Path $installation 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
}
foreach ($tool in @('cl.exe', 'git.exe', 'cmake.exe', 'ctest.exe', 'ninja.exe')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "Missing build tool: $tool" }
}
$baseline = (Get-Content -Raw -LiteralPath (Join-Path $project 'vcpkg.json') | ConvertFrom-Json).'builtin-baseline'
if ($baseline -notmatch '^[0-9a-f]{40}$') { throw 'vcpkg builtin-baseline must be a full commit SHA.' }
if (-not (Test-Path -LiteralPath $VcpkgRoot)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $VcpkgRoot) | Out-Null
    Invoke-Checked git.exe @('clone', '--filter=blob:none', '--no-checkout', 'https://github.com/microsoft/vcpkg.git', $VcpkgRoot)
    Invoke-Checked git.exe @('-C', $VcpkgRoot, 'checkout', '--detach', $baseline)
}
$revision = & git.exe -C $VcpkgRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $revision -ne $baseline) { throw 'VcpkgRoot must match the manifest baseline; existing checkout was left unchanged.' }
if (-not (Test-Path -LiteralPath (Join-Path $VcpkgRoot 'vcpkg.exe'))) {
    Invoke-Checked (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') @('-disableMetrics')
}
$testing = if ($SkipTests) { 'OFF' } else { 'ON' }
Invoke-Checked cmake.exe @('-S', $project, '-B', $BuildDirectory, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Configuration", "-DBUILD_TESTING=$testing", "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake", '-DVCPKG_TARGET_TRIPLET=x64-windows-static', "-DCMAKE_INSTALL_PREFIX=$BuildDirectory/install")
Invoke-Checked cmake.exe @('--build', $BuildDirectory, '--parallel', "$Jobs")
if (-not $SkipTests) { Invoke-Checked ctest.exe @('--test-dir', $BuildDirectory, '--output-on-failure') }
Invoke-Checked cmake.exe @('--install', $BuildDirectory)
if ($Package) { Invoke-Checked cmake.exe @('--build', $BuildDirectory, '--target', 'package', '--parallel', "$Jobs") }
