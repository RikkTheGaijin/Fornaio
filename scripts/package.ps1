$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $projectRoot
$destination = Join-Path $projectRoot 'dist\Fornaio'
New-Item -ItemType Directory -Force -Path $destination | Out-Null
foreach ($name in @('Fornaio.exe','fornaio_cli.exe','bake.ptx','transfer.ptx')) { Copy-Item -LiteralPath "build\Release\$name" -Destination $destination -Force }
Get-ChildItem -LiteralPath build\Release -Filter '*.dll' | Where-Object Name -NotIn @('dxcompiler.dll','dxil.dll','Qt6Test.dll') | Copy-Item -Destination $destination -Force
foreach ($name in @('platforms','imageformats','styles','generic','networkinformation','tls')) {
    if (Test-Path "build\Release\$name") { Copy-Item -LiteralPath "build\Release\$name" -Destination $destination -Recurse -Force }
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$redist = Get-ChildItem -LiteralPath (Join-Path $vsRoot 'VC\Redist\MSVC') -Directory | Where-Object Name -Match '^14\.' | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$crt = Join-Path $redist.FullName 'x64\Microsoft.VC143.CRT'
Get-ChildItem -LiteralPath $crt -Filter '*.dll' | Copy-Item -Destination $destination -Force
foreach ($name in @('README.md','THIRD_PARTY_NOTICES.md','VALIDATION.md')) { Copy-Item -LiteralPath $name -Destination $destination -Force }
New-Item -ItemType Directory -Force -Path "$destination\ai" | Out-Null
foreach ($name in @('manifest.json','AI_NOTICES.md','PROJECTION_BLENDING.md','ai_worker.py','ai_extract.ps1')) {
    Copy-Item -LiteralPath "build\Release\ai\$name" -Destination "$destination\ai" -Force
}
Copy-Item -LiteralPath samples -Destination $destination -Recurse -Force
Copy-Item -LiteralPath licenses -Destination $destination -Recurse -Force
# Fail closed if an old staging directory accidentally contains downloaded AI components.
foreach ($file in Get-ChildItem -LiteralPath $destination -File -Recurse) {
    if ($file.Extension -in @('.whl','.safetensors','.gguf','.pt','.pth') -or
        $file.Name -match '^python[0-9w.]*\.(exe|dll|zip)$' -or $file.Length -gt 100MB) {
        throw "Large/optional AI component must not be packaged: $($file.FullName)"
    }
}
Compress-Archive -Path $destination -DestinationPath dist\Fornaio-Windows-x64.zip -Force
# Include only source and required third-party files, without development environments or repository metadata.
$sourceRoot = Join-Path $projectRoot 'dist\Fornaio-Source'
New-Item -ItemType Directory -Force -Path $sourceRoot | Out-Null
foreach ($name in @('src','tests','scripts','samples','licenses','ai')) { Copy-Item -LiteralPath $name -Destination $sourceRoot -Recurse -Force }
foreach ($name in @('CMakeLists.txt','.clang-format','.gitignore','README.md','THIRD_PARTY_NOTICES.md','VALIDATION.md')) { Copy-Item -LiteralPath $name -Destination $sourceRoot -Force }
foreach ($lib in @('ufbx','stb','optix','mikktspace')) { New-Item -ItemType Directory -Force -Path "$sourceRoot\third_party\$lib" | Out-Null }
foreach ($name in @('ufbx.c','ufbx.h','LICENSE')) { Copy-Item -LiteralPath "third_party\ufbx\$name" -Destination "$sourceRoot\third_party\ufbx" -Force }
foreach ($name in @('stb_image.h','LICENSE')) { Copy-Item -LiteralPath "third_party\stb\$name" -Destination "$sourceRoot\third_party\stb" -Force }
foreach ($name in @('mikktspace.c','mikktspace.h')) { Copy-Item -LiteralPath "third_party\mikktspace\$name" -Destination "$sourceRoot\third_party\mikktspace" -Force }
Copy-Item -LiteralPath third_party\optix\include -Destination "$sourceRoot\third_party\optix" -Recurse -Force
Copy-Item -LiteralPath third_party\optix\LICENSE.txt -Destination "$sourceRoot\third_party\optix" -Force
# Test/import caches are local artifacts, never source-distribution inputs.
$sourcePrefix = [IO.Path]::GetFullPath($sourceRoot).TrimEnd('\') + '\'
foreach ($cache in Get-ChildItem -LiteralPath $sourceRoot -Directory -Filter '__pycache__' -Recurse) {
    $cachePath = [IO.Path]::GetFullPath($cache.FullName)
    if (-not $cachePath.StartsWith($sourcePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Cache cleanup escaped source staging: $cachePath"
    }
    Remove-Item -LiteralPath $cachePath -Recurse -Force
}
Compress-Archive -Path $sourceRoot -DestinationPath dist\Fornaio-Source.zip -Force
Get-FileHash dist\Fornaio-Windows-x64.zip,dist\Fornaio-Source.zip -Algorithm SHA256 | Format-Table -AutoSize
