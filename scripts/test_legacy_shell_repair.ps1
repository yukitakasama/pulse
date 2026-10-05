[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PulseRepairTestRegistry {
    [DllImport("advapi32.dll")] public static extern int RegOverridePredefKey(IntPtr key, IntPtr replacement);
}
'@
$original = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,
    [Microsoft.Win32.RegistryView]::Registry64)
$parent = $original.CreateSubKey('Software\PulseTest')
$name = 'LegacyRepair-' + [Guid]::NewGuid().ToString('N')
$root = $parent.CreateSubKey($name)
$hkcu = [IntPtr]::new(-2147483647)
$backup = Join-Path ([IO.Path]::GetTempPath()) ($name + '.reg')
$repair = Join-Path $PSScriptRoot 'repair_legacy_shell_integration.ps1'
$failures = 0
function Check([bool]$Ok, [string]$Label) {
    if ($Ok) { Write-Output "[PASS] $Label" }
    else { Write-Output "[FAIL] $Label"; $script:failures++ }
}
function Set-Value([string]$Path, [string]$Name, [string]$Text) {
    $key = $root.CreateSubKey($Path)
    try { $key.SetValue($Name, $Text, [Microsoft.Win32.RegistryValueKind]::String) }
    finally { $key.Dispose() }
}
$directory = 'Software\Classes\Directory\shell'
$pc = 'Software\Classes\CLSID\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\shell'
$win = 'Software\Classes\CLSID\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\shell\opennewwindow\command'
function Setup {
    $root.DeleteSubKeyTree('Software', $false)
    foreach ($group in @('Directory', 'Drive')) {
        Set-Value ('Software\Classes\' + $group + '\shell') '' 'open'
        Set-Value ('Software\Classes\' + $group + '\shell\open') 'DelegateExecute' ''
    }
    Set-Value $pc '' 'open'
    Set-Value ($pc + '\open\command') 'DelegateExecute' ''
    Set-Value $win 'DelegateExecute' ''
    Set-Value ($directory + '\OtherApp\command') '' 'other application'
}
function Has-Value([string]$Path, [string]$Name) {
    $key = $root.OpenSubKey($Path)
    try { return $null -ne $key -and $key.GetValueNames() -contains $Name }
    finally { if ($null -ne $key) { $key.Dispose() } }
}
$redirected = $false
try {
    $status = [PulseRepairTestRegistry]::RegOverridePredefKey($hkcu, $root.Handle.DangerousGetHandle())
    if ($status -ne 0) { throw "Registry isolation failed: $status" }
    $redirected = $true
    Setup
    & $repair | Out-Null
    Check (Has-Value $pc '') 'dry run retains diagnosed values'
    $rejected = $false
    try { & $repair -Apply -BackupPath (Join-Path $backup 'missing.reg') | Out-Null }
    catch { $rejected = $true }
    Check ($rejected -and (Has-Value $pc '') -and (Has-Value $win 'DelegateExecute')) 'backup creation failure leaves all associations unchanged'
    Set-Value ($pc + '\open\command') '' 'other command'
    $rejected = $false
    try { & $repair -Apply -BackupPath $backup | Out-Null } catch { $rejected = $true }
    Check ($rejected -and (Has-Value $directory '') -and -not (Test-Path -LiteralPath $backup)) 'foreign command aborts before backup or mutations'
    Setup
    Set-Value $win 'DelegateExecute' 'foreign delegate'
    $rejected = $false
    try { & $repair -Apply -BackupPath $backup | Out-Null } catch { $rejected = $true }
    Check ($rejected -and (Has-Value $pc '')) 'foreign delegate aborts entire repair'
    Setup
    $protected = $root.OpenSubKey($win, [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
        [System.Security.AccessControl.RegistryRights]::FullControl)
    $originalSecurity = $protected.GetAccessControl().GetSecurityDescriptorBinaryForm()
    $deniedSecurity = $protected.GetAccessControl()
    $deny = [System.Security.AccessControl.RegistryAccessRule]::new(
        [System.Security.Principal.WindowsIdentity]::GetCurrent().User,
        [System.Security.AccessControl.RegistryRights]::SetValue,
        [System.Security.AccessControl.AccessControlType]::Deny)
    $deniedSecurity.AddAccessRule($deny)
    try {
        $protected.SetAccessControl($deniedSecurity)
        $rejected = $false
        try { & $repair -Apply -BackupPath $backup | Out-Null } catch { $rejected = $true }
        Check ($rejected -and (Has-Value $directory '') -and
            (Has-Value ($directory + '\open') 'DelegateExecute') -and (Has-Value $pc '')) 'mid-repair write failure rolls back deleted values'
    } finally {
        $restoredSecurity = [System.Security.AccessControl.RegistrySecurity]::new()
        $restoredSecurity.SetSecurityDescriptorBinaryForm($originalSecurity,
            [System.Security.AccessControl.AccessControlSections]::Access)
        $protected.SetAccessControl($restoredSecurity)
        $protected.Dispose()
        if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup }
    }
    Setup
    & $repair -Apply -BackupPath $backup | Out-Null
    Check ((Test-Path -LiteralPath $backup) -and -not (Has-Value $pc '') -and
        -not (Has-Value $win 'DelegateExecute') -and -not (Has-Value $directory '') -and
        (Has-Value ($directory + '\OtherApp\command') '')) 'repair backs up residue, removes seven overrides, preserves unrelated menu'
    $backupText = [IO.File]::ReadAllText($backup)
    Check (([regex]::Matches($backupText, '\[HKEY_CURRENT_USER\\')).Count -eq 7) 'backup contains every removed value'
    $rejected = $false
    try { & $repair -Apply -BackupPath $backup | Out-Null } catch { $rejected = $true }
    Check ($rejected -and (Has-Value ($directory + '\OtherApp\command') '') -and
        [IO.File]::ReadAllText($backup) -ceq $backupText) 'second run makes no changes or backup overwrite'
} catch {
    Write-Output $_.ScriptStackTrace
    throw
} finally {
    if ($redirected) { [void][PulseRepairTestRegistry]::RegOverridePredefKey($hkcu, [IntPtr]::Zero) }
    $root.Dispose()
    try { $parent.DeleteSubKeyTree($name, $false) }
    catch { Write-Warning "Isolated fixture cleanup failed for $name : $($_.Exception.Message)"; $failures++ }
    $parent.Dispose()
    $original.Dispose()
    if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup }
}
if ($failures) { exit 1 }
