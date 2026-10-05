param([Parameter(Mandatory = $true)][string]$BuildDir, [switch]$RequireStaticRuntime)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ redistributables not found.' }
$redistRoot = Join-Path $vs 'VC/Redist/MSVC'
$version = Get-ChildItem -LiteralPath $redistRoot -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $version) { throw 'MSVC redistributable version not found.' }
$crt = Get-ChildItem -LiteralPath (Join-Path $version.FullName 'x64') -Directory |
    Where-Object { $_.Name -match '^Microsoft\.VC\d+\.CRT$' } | Select-Object -First 1
if (-not $crt) { throw 'x64 CRT redistributables not found.' }
# Inspect regular and delay-load imports, then follow CRT dependencies recursively.
$toolVersion = Get-ChildItem -LiteralPath (Join-Path $vs 'VC/Tools/MSVC') -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$dumpbin = Join-Path $toolVersion.FullName 'bin/Hostx64/x64/dumpbin.exe'
if (-not (Test-Path -LiteralPath $dumpbin)) { throw 'x64 dumpbin not found.' }
$runtimeFiles = @{}
Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll' -File | ForEach-Object {
    $runtimeFiles[$_.Name] = $_.FullName
}
$pending = [Collections.Generic.Queue[string]]::new()
# Keep in sync with the application payload in installer/PulseSetup.iss.
foreach ($binary in @('pulse.exe', 'Pulse.Index.exe', 'Pulse.Document.exe',
                      'Pulse.Preview.exe', 'pulse_shell.exe', 'pulse_elevated.exe', 'pulse_integration.exe', 'lumatext.dll', 'pdfium.dll')) {
    $pending.Enqueue((Join-Path $BuildDir $binary))
}
$required = @{}
while ($pending.Count -gt 0) {
    $binary = $pending.Dequeue()
    if (-not (Test-Path -LiteralPath $binary)) { throw "Missing payload: $binary" }
    $imports = & $dumpbin /nologo /dependents $binary
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect imports: $binary" }
    foreach ($line in $imports) {
        if ($line -notmatch '^\s+([^\s]+\.dll)\s*$') { continue }
        $name = $Matches[1].ToLowerInvariant()
        if ($runtimeFiles.ContainsKey($name)) {
            if (-not $required.ContainsKey($name)) {
                $required[$name] = $runtimeFiles[$name]
                $pending.Enqueue($runtimeFiles[$name])
            }
        } elseif ($name -match '^(msvcp|vcruntime|concrt|vccorlib)\d') {
            throw "Required MSVC runtime unavailable: $name ($binary)"
        }
    }
}
$buildRoot = (Resolve-Path -LiteralPath $BuildDir).Path
if ($RequireStaticRuntime -and $required.Count -gt 0) {
    throw "Release payload still requires MSVC runtime DLLs: $($required.Keys -join ', '). Rebuild all payloads, including LumaText, with /MT."
}
$destination = Join-Path $buildRoot 'msvc-runtime'
New-Item -ItemType Directory -Path $destination -Force | Out-Null
# Remove only stale DLLs in this dedicated staging folder, never application files.
Get-ChildItem -LiteralPath $destination -Filter '*.dll' -File | ForEach-Object {
    if (-not $required.ContainsKey($_.Name)) { Remove-Item -LiteralPath $_.FullName }
}
$manifest = @(foreach ($name in ($required.Keys | Sort-Object)) {
    $target = Join-Path $destination $name
    Copy-Item -LiteralPath $required[$name] -Destination $target -Force
    $stream = [IO.File]::OpenRead($target)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { $hash = [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '') }
    finally { $stream.Dispose(); $algorithm.Dispose() }
    [pscustomobject]@{ name = $name; sha256 = $hash }
})
ConvertTo-Json -InputObject @($manifest) | Set-Content -LiteralPath (Join-Path $buildRoot 'installer-runtime-manifest.json') -Encoding UTF8
Write-Output "Staged $($required.Count) required x64 MSVC runtime DLLs: $($required.Keys -join ', ')"
