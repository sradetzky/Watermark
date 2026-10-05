param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer not found. Install Visual Studio Build Tools with Desktop development with C++.'
}
$toolchain = & $vswhere -latest -products '*' -version '[16.10,)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $toolchain) {
    throw 'No supported MSVC installation found. Install Visual Studio 2019 16.10 or newer with Desktop development with C++.'
}
$toolchain = $toolchain.Trim()
$vcvars = Join-Path $toolchain 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) { throw "Required tool missing: $vcvars" }
# Import the developer environment into this process; do not change the user's PATH permanently.
$environment = & $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not load the MSVC developer environment.' }
foreach ($entry in $environment) {
    if ($entry -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
function Find-BuildTool([string]$Name, [string]$BundledPath) {
    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    if (Test-Path -LiteralPath $BundledPath) { return $BundledPath }
    throw "Required tool missing: $Name. Install the C++ CMake tools for Windows component in Visual Studio Installer."
}
$cmake = Find-BuildTool 'cmake.exe' (Join-Path $toolchain 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
$ctest = Find-BuildTool 'ctest.exe' (Join-Path (Split-Path $cmake) 'ctest.exe')
$ninja = Find-BuildTool 'ninja.exe' (Join-Path $toolchain 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe')
$root = Split-Path $PSScriptRoot
$build = Join-Path $root "build\$Configuration"
& $cmake -S $root -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $cmake --build $build
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
& $ctest --test-dir $build --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
