# Packages a verified, complete official 7-Zip x64 distribution; no installation.
param(
    [Parameter(Mandatory=$true)][string]$SourceDirectory,
    [Parameter(Mandatory=$true)][ValidatePattern('^\d+\.\d+$')][string]$Version,
    [string]$Work = 'build/archivepack'
)
$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
foreach ($name in @('7z.exe', '7z.dll', 'License.txt')) {
    if (-not (Test-Path -LiteralPath (Join-Path $source $name) -PathType Leaf)) { throw "Missing $name" }
}
foreach ($name in @('7z.exe', '7z.dll')) {
    $item = Get-Item -LiteralPath (Join-Path $source $name)
    if ($item.VersionInfo.ProductName -ne '7-Zip' -or $item.VersionInfo.FileVersion -ne $Version -or
        $item.VersionInfo.OriginalFilename -ne $name) { throw "Unexpected 7-Zip identity/version: $name" }
    $bytes = [IO.File]::ReadAllBytes($item.FullName)
    $pe = [BitConverter]::ToInt32($bytes, 60)
    if ($pe -lt 0 -or $pe + 6 -gt $bytes.Length -or [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664) {
        throw "Expected x64 PE: $name"
    }
}
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$workPath = (Resolve-Path -LiteralPath $Work).Path
$pack = Join-Path $workPath 'pack'
New-Item -ItemType Directory -Force -Path $pack | Out-Null
foreach ($name in @('7z.exe', '7z.dll', 'License.txt')) {
    Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $pack $name) -Force
}
@"
7-Zip $Version, Copyright Igor Pavlov.
License: GNU LGPL with BSD components and unRAR restriction; see License.txt.
Official source: https://github.com/ip7z/7zip/tree/$Version
Downloads and corresponding source: https://www.7-zip.org/download.html
Pulse invokes the listing command only. This pack is optional.
"@ | Set-Content -LiteralPath (Join-Path $pack 'NOTICE.txt') -Encoding utf8
# Use Process rather than an unbounded pipeline; validate the exact packaged pair.
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = Join-Path $pack '7z.exe'
$start.Arguments = 'i'
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = [Diagnostics.Process]::Start($start)
$stdout = $process.StandardOutput.ReadToEndAsync()
$stderr = $process.StandardError.ReadToEndAsync()
if (-not $process.WaitForExit(5000)) { $process.Kill(); throw '7-Zip validation timed out' }
$log = $stdout.GetAwaiter().GetResult()
if ($process.ExitCode -ne 0 -or $log -notmatch '7-Zip' -or $log -notmatch 'Rar' -or $log -notmatch '7z.dll') {
    throw "7-Zip format validation failed: $($stderr.GetAwaiter().GetResult())"
}
$process.Dispose()
Get-ChildItem -LiteralPath $pack -File | ForEach-Object {
    [ordered]@{ name=$_.Name; size=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $workPath 'files.json') -Encoding utf8
Write-Output "Validated archive pack $Version at $pack (not published)."
