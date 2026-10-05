param([string]$VcpkgRoot = 'C:\vcpkg', [string]$Work = '', [switch]$SkipVcpkg)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..')).Path
if (-not $Work) { $Work = Join-Path $repo 'build\rawpack' }
New-Item -ItemType Directory -Force $Work | Out-Null
$Work = (Resolve-Path $Work).Path
$triplet = 'x64-windows-rawpack'
$prefix = Join-Path $Work "installed\$triplet"
$env:VCPKG_DISABLE_METRICS = '1'
$env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $Work 'binarycache'
New-Item -ItemType Directory -Force $env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null
if (-not $SkipVcpkg) {
    & "$VcpkgRoot\vcpkg.exe" install --x-manifest-root="$here" --overlay-triplets="$here\triplets" --triplet $triplet --host-triplet x64-windows-release --x-install-root="$Work\installed" --x-buildtrees-root="$Work\buildtrees" --x-packages-root="$Work\packages" --downloads-root="$Work\downloads" --clean-after-build
    if ($LASTEXITCODE) { throw 'vcpkg install failed' }
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    cmd /c "`"$repo\scripts\vcvars.bat`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
    }
}
cmake --fresh -G Ninja -S $here -B "$Work\cmake" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$prefix" "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot\scripts\buildsystems\vcpkg.cmake" "-DVCPKG_INSTALLED_DIR=$Work\installed" "-DVCPKG_TARGET_TRIPLET=$triplet" -DVCPKG_MANIFEST_INSTALL=OFF
if ($LASTEXITCODE) { throw 'configure failed' }
cmake --build "$Work\cmake" --target pulse-rawpack
if ($LASTEXITCODE) { throw 'build failed' }
# Stage to a new/reusable directory without deleting any user supplied path.
$pack = Join-Path $Work 'pack'
New-Item -ItemType Directory -Force $pack | Out-Null
Copy-Item "$Work\cmake\pulse-rawpack.exe" $pack -Force
Copy-Item "$prefix\bin\raw_r.dll" $pack -Force
# Both helper and replaceable LibRaw DLL use static CRT; verify the staged binaries.
Get-ChildItem $pack -File | Where-Object { $_.Extension -in '.exe', '.dll' } | ForEach-Object {
    $imports = & dumpbin /nologo /dependents $_.FullName
    if ($LASTEXITCODE) { throw "Cannot inspect imports of $($_.Name)" }
    $dlls = $imports | Where-Object { $_ -match '^\s+[^\s]+\.dll\s*$' }
    Write-Output "Imports for $($_.Name): $($dlls.Trim() -join ', ')"
    if ($dlls -match '(?i)msvcp[0-9]|vcruntime|ucrtbase|api-ms-win-crt-') {
        throw "Unexpected dynamic VC runtime dependency in $($_.Name)"
    }
}
$notices = [System.Text.StringBuilder]::new()
[void]$notices.AppendLine('Pulse RAW preview pack; LibRaw is a replaceable LGPL/CDDL library. https://www.libraw.org/')
Get-ChildItem "$prefix\share" -Directory | Sort-Object Name | ForEach-Object {
    $license = Join-Path $_.FullName 'copyright'
    if (Test-Path $license) { [void]$notices.AppendLine($_.Name); [void]$notices.AppendLine((Get-Content $license -Raw)) }
}
[IO.File]::WriteAllText((Join-Path $pack 'THIRD-PARTY-NOTICES.txt'), $notices.ToString())
& "$here\test.ps1" -Exe "$pack\pulse-rawpack.exe" -Work "$Work\test"
if ($LASTEXITCODE) { throw 'rawpack test failed' }
Write-Output "RAW pack staged at $pack"




