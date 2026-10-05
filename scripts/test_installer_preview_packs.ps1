# Uninstall handling of the preview packs (installer/PulseSetup.iss): the
# packs.json "remove_on_uninstall" switch, data cleanup keeping kept packs, and
# upgrades never touching them. The real Pascal code runs against a sandbox
# directory; only ExpandConstant is mocked, and it refuses paths outside it.
param(
    [string]$Iscc = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$source = Get-Content -LiteralPath (Join-Path $repo 'installer/PulseSetup.iss') -Raw -Encoding UTF8
$out = Join-Path $repo 'build/installer-packs-test'
$sandbox = Join-Path $out 'sandbox'
New-Item -ItemType Directory -Force -Path $sandbox | Out-Null
$code = [regex]::Match($source, '(?s)function EndsWith\(.*?(?=function InitializeUninstall:)').Value
if (!$code -or $code -notmatch 'procedure RemovePreviewPacksIfWanted' -or $code -notmatch 'procedure CleanupPulseData') {
    throw 'Installer procedure boundaries changed; update the harness.'
}
$code = $code -replace '\bExpandConstant\b', 'MockExpandConstant'
if ($code -match '(?<!Mock)ExpandConstant' -or $code -match '\b(Reg\w+|Exec|ShellExec)\s*\(') {
    throw 'Unmocked external operation in extracted installer code.'
}
$report = (Join-Path $out 'result.txt').Replace("'", "''")
$root = $sandbox.Replace("'", "''")
$prefix = @"
[Setup]
AppName=Pulse preview pack uninstall harness
AppVersion=1
DefaultDirName={tmp}\PulsePacksHarnessNeverInstalled
PrivilegesRequired=lowest
Uninstallable=no
CreateAppDir=no
OutputBaseFilename=installer-packs-harness
Compression=none
[Code]
const
  SandboxRoot = '$root';
var
  UninstallIndexPath, UpgradeParam, Report: String;
  Failures: Integer;

function MockExpandConstant(const Value: String): String;
begin
  Result := Value;
  StringChangeEx(Result, '{localappdata}', SandboxRoot + '\local', True);
  StringChangeEx(Result, '{commonappdata}', SandboxRoot + '\common', True);
  StringChangeEx(Result, '{param:PULSEUPGRADE|0}', UpgradeParam, True);
  if Pos('{', Result) > 0 then
    RaiseException('Unmocked constant: ' + Value);
  if (Value <> '{param:PULSEUPGRADE|0}') and (Pos(Lowercase(SandboxRoot + '\'), Lowercase(Result)) <> 1) then
    RaiseException('Outside the sandbox: ' + Result);
end;
"@
$tests = @'
procedure Check(Condition: Boolean; const LabelText: String);
begin
  if Condition then Report := Report + '[PASS] ' + LabelText + #13#10
  else begin Failures := Failures + 1; Report := Report + '[FAIL] ' + LabelText + #13#10; end;
end;
procedure Put(const Rel: String);
begin
  ForceDirectories(ExtractFileDir(SandboxRoot + '\' + Rel));
  SaveStringToFile(SandboxRoot + '\' + Rel, 'x', False);
end;
function Has(const Rel: String): Boolean;
begin
  Result := FileExists(SandboxRoot + '\' + Rel) or DirExists(SandboxRoot + '\' + Rel);
end;
procedure Layout(const PacksJson: String);
begin
  DelTree(SandboxRoot + '\local', True, True, True);
  DelTree(SandboxRoot + '\common', True, True, True);
  Put('local\Pulse\app.json');
  Put('local\Pulse\thumbs\a.bin');
  Put('local\Pulse\packs\ffmpeg\7.1.1\ffmpeg.exe');
  Put('local\Pulse\packs\ffmpeg\installed.json');
  Put('common\Pulse\index-config.json');
  if PacksJson <> '' then
    SaveStringToFile(SandboxRoot + '\local\Pulse\packs\packs.json', PacksJson, False);
end;

function InitializeSetup: Boolean;
begin
  UninstallIndexPath := '';
  UpgradeParam := '0';
  Check(not ReadJsonBool('{"remove_on_uninstall": false}', 'remove_on_uninstall', True), 'json false');
  Check(ReadJsonBool('{' + #13#10 + '  "remove_on_uninstall":true}', 'remove_on_uninstall', False), 'json true');
  Check(ReadJsonBool('{}', 'remove_on_uninstall', True), 'missing key keeps the default');

  Layout('');
  CleanupPulseData;
  Check(not Has('local\Pulse') and not Has('common\Pulse'), 'data cleanup removes packs by default');

  Layout('{"ffmpeg_enabled": true, "remove_on_uninstall": false}');
  CleanupPulseData;
  Check(Has('local\Pulse\packs\ffmpeg\7.1.1\ffmpeg.exe') and Has('local\Pulse\packs\packs.json'),
    'kept packs survive data cleanup');
  Check(not Has('local\Pulse\app.json') and not Has('local\Pulse\thumbs') and not Has('common\Pulse'),
    'the rest of the data is still removed');

  Layout('{"remove_on_uninstall": true}');
  RemovePreviewPacksIfWanted;
  Check(not Has('local\Pulse\packs') and Has('local\Pulse\app.json'),
    'uninstall keeping data still removes packs');

  UpgradeParam := '1';
  Layout('');
  RemovePreviewPacksIfWanted;
  Check(Has('local\Pulse\packs\ffmpeg\7.1.1\ffmpeg.exe'), 'upgrade never removes packs');

  UpgradeParam := '0';
  Layout('{"remove_on_uninstall": false}');
  RemovePreviewPacksIfWanted;
  Check(Has('local\Pulse\packs\ffmpeg\7.1.1\ffmpeg.exe'), 'packs kept when the setting is off');

  DelTree(SandboxRoot + '\local', True, True, True);
  DelTree(SandboxRoot + '\common', True, True, True);
  { Returning False prevents installation, file deployment, and uninstall registration. }
  Result := False;
'@
$harness = $prefix + "`r`n" + $code + "`r`n" + $tests + "`r`n  SaveStringToFile('$report', Report, False);`r`nend;`r`n"
$iss = Join-Path $out 'harness.iss'
Set-Content -LiteralPath $iss -Value $harness -Encoding utf8
& $Iscc "/Q" "/O$out" $iss
if ($LASTEXITCODE -ne 0) { throw 'Pascal harness compilation failed.' }
$exe = Join-Path $out 'installer-packs-harness.exe'
if (Test-Path -LiteralPath (Join-Path $out 'result.txt')) { Remove-Item -LiteralPath (Join-Path $out 'result.txt') }
$process = Start-Process -FilePath $exe -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -WindowStyle Hidden -Wait -PassThru
if (!(Test-Path -LiteralPath (Join-Path $out 'result.txt'))) { throw "Harness produced no report (exit $($process.ExitCode))." }
$result = Get-Content -LiteralPath (Join-Path $out 'result.txt') -Raw
Write-Output $result
if ($result -match '\[FAIL\]') { throw 'Preview pack uninstall regression failed.' }
if (($result -split '\[PASS\]').Count -ne 10) { throw 'Unexpected number of completed assertions.' }
