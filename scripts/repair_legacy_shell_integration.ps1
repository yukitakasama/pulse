# Repairs only the exact legacy residue captured in shell-integration-1339.json.
# Run as the affected user. Without -Apply this only checks the prerequisites.
[CmdletBinding()]
param(
    [switch]$Apply,
    [string]$BackupPath
)

$ErrorActionPreference = 'Stop'
$base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry64)
$groups = @(
    @{ name = 'Directory'; shell = 'Software\Classes\Directory\shell'; verb = 'open'; delegateAtVerb = $true },
    @{ name = 'Drive'; shell = 'Software\Classes\Drive\shell'; verb = 'open'; delegateAtVerb = $true },
    @{ name = 'WinE'; shell = 'Software\Classes\CLSID\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\shell'; verb = 'opennewwindow'; delegateAtVerb = $false },
    @{ name = 'ThisPc'; shell = 'Software\Classes\CLSID\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\shell'; verb = 'open'; delegateAtVerb = $false }
)
$targets = @()
foreach ($group in $groups) {
    $verb = $group.shell + '\' + $group.verb
    if ($group.name -ne 'WinE') {
        $targets += @{ path = $group.shell; name = ''; text = 'open' }
    }
    $delegatePath = $verb + '\command'
    if ($group.delegateAtVerb) { $delegatePath = $verb }
    $targets += @{ path = $delegatePath; name = 'DelegateExecute'; text = '' }
}

function Assert-Value {
    param([string]$Path, [string]$Name, [bool]$Exists, [string]$Text)
    $key = $base.OpenSubKey($Path, $false)
    try {
        $found = $null -ne $key -and $key.GetValueNames() -contains $Name
        if ($found -ne $Exists) { throw "State does not match the diagnosed residue: $Path [$Name]" }
        if ($found -and ($key.GetValueKind($Name) -ne [Microsoft.Win32.RegistryValueKind]::String -or
            $key.GetValue($Name) -cne $Text)) {
            throw "Value has changed; refusing to alter it: $Path [$Name]"
        }
    } finally {
        if ($null -ne $key) { $key.Dispose() }
    }
}

function Assert-DiagnosedState {
    foreach ($group in $groups) {
        $verb = $group.shell + '\' + $group.verb
        $command = $verb + '\command'
        Assert-Value $command '' $false ''
        # Existing snapshots or legacy backups require the normal restore path.
        $snapshot = $base.OpenSubKey('Software\Pulse\ShellIntegration\Backups\v1\' + $group.name, $false)
        if ($null -ne $snapshot) { $snapshot.Dispose(); throw "A restore snapshot now exists for $($group.name); refusing legacy repair." }
        foreach ($path in @($group.shell, $verb, $command)) {
            Assert-Value $path 'PulseBackup' $false ''
            Assert-Value $path 'PulseBackupDelegateExecute' $false ''
        }
        if ($group.delegateAtVerb) { Assert-Value $command 'DelegateExecute' $false '' }
        else { Assert-Value $verb 'DelegateExecute' $false '' }
        if ($group.name -eq 'WinE') { Assert-Value $group.shell '' $false '' }
    }
    foreach ($target in $targets) { Assert-Value $target.path $target.name $true $target.text }
}

function Remove-EmptyAncestors {
    param([string]$Path)
    while ($Path.StartsWith('Software\Classes\', [StringComparison]::OrdinalIgnoreCase)) {
        $key = $base.OpenSubKey($Path, $false)
        if ($null -eq $key) { $Path = $Path.Substring(0, $Path.LastIndexOf('\')); continue }
        try { $empty = $key.ValueCount -eq 0 -and $key.SubKeyCount -eq 0 }
        finally { $key.Dispose() }
        if (-not $empty) { break }
        # DeleteSubKey never recursively removes another program's keys.
        $base.DeleteSubKey($Path, $false)
        $Path = $Path.Substring(0, $Path.LastIndexOf('\'))
    }
}

try {
    Assert-DiagnosedState
    Write-Output 'Confirmed the diagnosed legacy residue: 7 values, no command or backup.'
    if (-not $Apply) {
        Write-Output 'Read-only check complete. Use -Apply -BackupPath <new-file.reg> to repair.'
        return
    }
    if ([string]::IsNullOrWhiteSpace($BackupPath)) { throw '-BackupPath is required with -Apply.' }
    $destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BackupPath)
    if ([IO.Path]::GetExtension($destination) -ine '.reg') { throw 'BackupPath must end in .reg.' }
    if (Test-Path -LiteralPath $destination) { throw "Backup already exists: $destination" }
    Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class PulseLegacyRepairNotify {
    [DllImport("shell32.dll")] public static extern void SHChangeNotify(uint e, uint f, System.IntPtr a, System.IntPtr b);
}
'@
    $lines = @('Windows Registry Editor Version 5.00', '')
    foreach ($target in $targets) {
        $name = '@'
        if ($target.name) { $name = '"' + $target.name + '"' }
        $lines += '[HKEY_CURRENT_USER\' + $target.path + ']'
        $lines += $name + '="' + $target.text + '"'
        $lines += ''
    }
    # CreateNew prevents overwriting a backup even if another process creates it.
    $stream = [IO.File]::Open($destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
    try {
        $writer = [IO.StreamWriter]::new($stream, [Text.Encoding]::Unicode)
        try { $writer.Write(($lines -join "`r`n")); $writer.Flush() }
        finally { $writer.Dispose() }
    } finally { $stream.Dispose() }
    Write-Output "Backup saved before changes: $destination"
    Assert-DiagnosedState
    $deleted = @()
    try {
        foreach ($target in $targets) {
            Assert-Value $target.path $target.name $true $target.text
            $key = $base.OpenSubKey($target.path, $true)
            try { $key.DeleteValue($target.name, $true); $deleted += $target }
            finally { $key.Dispose() }
        }
        foreach ($group in $groups) { Remove-EmptyAncestors ($group.shell + '\' + $group.verb + '\command') }
        foreach ($target in $targets) { Assert-Value $target.path $target.name $false '' }
    } catch {
        $failure = $_
        foreach ($target in $deleted) {
            $key = $null
            try {
                $key = $base.CreateSubKey($target.path)
                # A concurrent writer owns any new value; do not overwrite it.
                if ($key.GetValueNames() -notcontains $target.name) {
                    $key.SetValue($target.name, $target.text, [Microsoft.Win32.RegistryValueKind]::String)
                }
            } catch { Write-Warning "Could not roll back $($target.path). The .reg backup remains available." }
            finally { if ($null -ne $key) { $key.Dispose() } }
        }
        [PulseLegacyRepairNotify]::SHChangeNotify(0x08000000, 0x1000, [IntPtr]::Zero, [IntPtr]::Zero)
        throw $failure
    }
    [PulseLegacyRepairNotify]::SHChangeNotify(0x08000000, 0x1000, [IntPtr]::Zero, [IntPtr]::Zero)
    Write-Output 'Repair complete. Test This PC and Win+E, then capture a new diagnostic report.'
} finally {
    $base.Dispose()
}
