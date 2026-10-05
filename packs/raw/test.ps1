param([Parameter(Mandatory=$true)][string]$Exe, [string]$Work = "$PSScriptRoot\..\..\build\rawpack-test")
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
New-Item -ItemType Directory -Force $Work | Out-Null
$formats = & $Exe formats
if ($LASTEXITCODE -ne 0 -or $formats -notmatch '\.cr3' -or $formats -notmatch '\.nef') { throw 'formats failed' }
Write-Output '[PASS] RAW format inventory'
$bad = Join-Path (Resolve-Path $Work).Path 'corrupt.dng'
[IO.File]::WriteAllBytes($bad, [byte[]](0..31))
& $Exe decode $bad 0 | Out-Null
if ($LASTEXITCODE -ne 1) { throw 'invalid cap was accepted' }
Write-Output '[PASS] invalid cap rejected'
& $Exe decode $bad 256 | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'corrupt RAW was accepted' }
Write-Output '[PASS] corrupt RAW rejected'
& $Exe probe "$bad.missing" | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'missing RAW was accepted' }
Write-Output '[PASS] missing RAW rejected'
python "$PSScriptRoot/test_dng.py" $Exe $Work
if ($LASTEXITCODE) { throw "synthetic DNG integration failed" }
exit 0

