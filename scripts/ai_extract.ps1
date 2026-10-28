# Extract verified release archives into a private staging directory. No package manager is run.
param([Parameter(Mandatory=$true)][string]$Archive,
      [Parameter(Mandatory=$true)][string]$Destination)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$extractRoot = [IO.Path]::GetFullPath($Destination).TrimEnd('\') + '\'
[IO.Directory]::CreateDirectory($extractRoot) | Out-Null
$zip = [IO.Compression.ZipFile]::OpenRead($Archive)
try {
    foreach ($entry in $zip.Entries) {
        $entryName = $entry.FullName.Replace('/', '\')
        if ($entryName.Contains(':')) { throw 'Invalid archive entry' }
        $targetPath = [IO.Path]::GetFullPath([IO.Path]::Combine($extractRoot, $entryName))
        if (-not $targetPath.StartsWith($extractRoot, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Archive entry escapes installation directory'
        }
        if (($entry.ExternalAttributes -band 0xF0000000) -eq 0xA0000000) { throw 'Archive symlinks are not allowed' }
        if ($entryName.EndsWith('\')) { [IO.Directory]::CreateDirectory($targetPath) | Out-Null; continue }
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($targetPath)) | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $targetPath, $true)
    }
} finally { $zip.Dispose() }
