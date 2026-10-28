param([switch]$Package)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $projectRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Visual Studio C++ build tools are required.' }
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$envLines = & cmd.exe /d /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE) { throw 'Could not initialize the Visual Studio environment.' }
foreach ($line in $envLines) { if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') } }
$cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$qt = Join-Path $projectRoot '.deps\Qt\6.8.3\msvc2022_64'
if (-not (Test-Path "$qt\lib\cmake\Qt6")) { throw 'Install Qt as described in README.md first.' }
& $cmake -S . -B build -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$qt"
if ($LASTEXITCODE) { throw 'CMake configure failed.' }
# Fornaio doesn't use vcpkg; opt out of the machine-wide MSBuild integration.
& $cmake --build build --config Release --parallel 8 -- /p:VCPkgLocalAppDataDisabled=true
if ($LASTEXITCODE) { throw 'Build failed.' }
& "$qt\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime build\Release\Fornaio.exe
if ($LASTEXITCODE) { throw 'Qt deployment failed.' }
Copy-Item -LiteralPath "$qt\bin\Qt6Test.dll" -Destination build\Release -Force
# The optional private Python engine also needs the app-local MSVC runtime on clean Windows.
$redist = Get-ChildItem -LiteralPath (Join-Path $vsRoot 'VC\Redist\MSVC') -Directory | Where-Object Name -Match '^14\.' | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$crt = Join-Path $redist.FullName 'x64\Microsoft.VC143.CRT'
Get-ChildItem -LiteralPath $crt -Filter '*.dll' | Copy-Item -Destination build\Release -Force
if ($Package) { & "$PSScriptRoot\package.ps1" }
