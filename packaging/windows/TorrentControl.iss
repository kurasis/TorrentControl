#ifndef StageRoot
  #error StageRoot is required
#endif
#ifndef OutputRoot
  #error OutputRoot is required
#endif
#ifndef RuntimeSource
  #error RuntimeSource is required
#endif
#ifndef RuntimeSHA256
  #error RuntimeSHA256 is required
#endif
#ifndef RuntimeMinimum
  #error RuntimeMinimum is required
#endif
#ifndef InstallMutex
  #error InstallMutex is required
#endif
#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef PackageMode
  #error PackageMode is required
#endif

[Setup]
AppId={{75613C0D-60D8-4B53-B7F7-01DE3D5E37FE}
AppName=TorrentControl (development)
AppVersion={#AppVersion}
AppVerName=TorrentControl {#AppVersion} (unsigned development build)
AppSupportURL=https://github.com/kurasis/TorrentControl
DefaultDirName={localappdata}\Programs\TorrentControl
DefaultGroupName=TorrentControl
PrivilegesRequired=lowest
#if PackageMode == "offline"
; This bundle carries the x64 standalone Runtime, not the ARM64 Runtime.
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
#else
; The online bootstrapper selects the Runtime for the actual operating system.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif
MinVersion=10.0.19045
AppMutex={#InstallMutex}
UninstallDisplayIcon={app}\TorrentControl.exe
DisableProgramGroupPage=yes
CloseApplications=no
RestartApplications=no
OutputDir={#OutputRoot}
OutputBaseFilename=TorrentControl-{#AppVersion}-dev-unsigned-windows-x64-{#PackageMode}-setup
Compression=lzma2/fast
SolidCompression=yes
WizardStyle=modern
SetupLogging=yes
VersionInfoVersion={#AppVersion}

[Files]
Source: "{#StageRoot}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RuntimeSource}"; DestDir: "{tmp}"; DestName: "WebView2Prerequisite.exe"; Flags: dontcopy nocompression

[Icons]
Name: "{group}\TorrentControl"; Filename: "{app}\TorrentControl.exe"; WorkingDir: "{app}"
Name: "{group}\Uninstall TorrentControl"; Filename: "{uninstallexe}"

[Code]
const
  RuntimeKey = 'Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}';

function CompatibleVersion(const Text: String): Boolean;
var
  Current, Minimum: Int64;
  I, Dots: Integer;
begin
  Result := False;
  Dots := 0;
  for I := 1 to Length(Text) do begin
    if Text[I] = '.' then Dots := Dots + 1
    else if (Text[I] < '0') or (Text[I] > '9') then Exit;
  end;
  if Dots <> 3 then Exit;
  if not StrToVersion(Text, Current) then Exit;
  if not StrToVersion('{#RuntimeMinimum}', Minimum) then Exit;
  Result := ComparePackedVersion(Current, Minimum) >= 0;
end;

function RuntimeCompatible: Boolean;
var
  Version: String;
begin
  Result := (RegQueryStringValue(HKCU32, RuntimeKey, 'pv', Version) and CompatibleVersion(Version)) or
    (RegQueryStringValue(HKLM32, RuntimeKey, 'pv', Version) and CompatibleVersion(Version)) or
    (RegQueryStringValue(HKCU64, RuntimeKey, 'pv', Version) and CompatibleVersion(Version)) or
    (RegQueryStringValue(HKLM64, RuntimeKey, 'pv', Version) and CompatibleVersion(Version));
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  FileName: String;
  ExitCode: Integer;
begin
  Result := '';
  if RuntimeCompatible and (ExpandConstant('{param:INSTALLRUNTIME|0}') <> '1') then begin
    Log('TC_RUNTIME compatible-existing');
    Exit;
  end;
  try
    ExtractTemporaryFile('WebView2Prerequisite.exe');
    FileName := ExpandConstant('{tmp}\WebView2Prerequisite.exe');
    if CompareText(GetSHA256OfFile(FileName), '{#RuntimeSHA256}') <> 0 then begin
      Log('TC_RUNTIME hash-mismatch');
      Result := 'The WebView2 installer failed its integrity check. Download TorrentControl again.';
      Exit;
    end;
    Log('TC_RUNTIME executing-{#PackageMode}');
    if not Exec(FileName, '/silent /install', '', SW_HIDE, ewWaitUntilTerminated, ExitCode) then begin
      Log('TC_RUNTIME launch-failed');
      Result := 'The Microsoft WebView2 Runtime installer could not start.';
      Exit;
    end;
    Log(Format('TC_RUNTIME exit=%d', [ExitCode]));
    if (ExitCode <> 0) and (ExitCode <> 3010) then begin
      Result := Format('Microsoft WebView2 Runtime installation failed (code %d). Check disk space and internet access for the online package, then retry.', [ExitCode]);
      Exit;
    end;
    NeedsRestart := ExitCode = 3010;
    if not RuntimeCompatible then begin
      Log('TC_RUNTIME still-incompatible');
      Result := 'Microsoft WebView2 Runtime is still missing or too old. Restart Windows if required, then retry installation.';
      Exit;
    end;
    Log('TC_RUNTIME compatible-after-install');
  except
    Log('TC_RUNTIME exception: ' + GetExceptionMessage);
    Result := 'WebView2 prerequisite preparation failed: ' + GetExceptionMessage;
  end;
end;
