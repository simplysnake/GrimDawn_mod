; Grim Dawn VR installer (Inno Setup 6). Built by tools\package.ps1, which passes:
;   /DAppVersion=<x.y.z>  /DSourceDir=<dist\GrimDawnVR-x.y.z>  /DOutputDir=<dist>
;
; Installs per user (no administrator rights needed), optionally adds "Grim Dawn VR" to Steam as a VR game
; (Steam has to be closed for that) and optionally adds the two Windows Defender exclusions (asks for
; administrator permission). The uninstaller removes the dinput8.dll stand-in from the game folder, puts the
; normal game settings back and removes the Steam entry.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #error SourceDir must point at the packaged release folder
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{6E1B7C3A-4D2F-4B8E-9A51-3C7D2E9F0A14}
AppName=Grim Dawn VR
AppVersion={#AppVersion}
AppVerName=Grim Dawn VR {#AppVersion}
AppPublisher=Brian Alvarez (artalva.com)
AppPublisherURL=https://artalva.com
AppSupportURL=https://github.com/simplysnake/GrimDawn_mod
DefaultDirName={localappdata}\Programs\GrimDawnVR
DefaultGroupName=Grim Dawn VR
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
LicenseFile={#SourceDir}\LICENSE.md
OutputDir={#OutputDir}
OutputBaseFilename=GrimDawnVR-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName=Grim Dawn VR
UninstallDisplayIcon={app}\gdvr_launch.exe

[Tasks]
Name: "steam"; Description: "Add ""Grim Dawn VR"" to my Steam library (listed with the VR games; Steam is closed for a moment)"
Name: "defender"; Description: "Add Windows Defender exclusions for the mod's folders (asks for administrator permission; see the README)"; Flags: unchecked
Name: "desktopicon"; Description: "Desktop shortcut"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{autoprograms}\Grim Dawn VR"; Filename: "{app}\gdvr_launch.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\Grim Dawn VR - Read me"; Filename: "{app}\README.md"
Name: "{autodesktop}\Grim Dawn VR"; Filename: "{app}\gdvr_launch.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\README.md"; Description: "Open the README (controls, settings, troubleshooting)"; Flags: postinstall shellexec skipifsilent unchecked
; Updates started by the launcher (/RELAUNCH=1): start the game again when done
Filename: "{app}\gdvr_launch.exe"; WorkingDir: "{app}"; Flags: nowait; Check: Relaunch

[Messages]
FinishedLabel=Grim Dawn VR is installed. Start it from Steam ("Grim Dawn VR") or the Start menu.%n%nImportant: in Steam, open the Properties of "Grim Dawn VR", Controller, and set Steam Input to Disabled. Otherwise Steam turns the VR controllers into a desktop gamepad on top of the mod.

[Code]
const
  SteamExe = 'steam.exe';

function Relaunch(): Boolean;
begin
  Result := ExpandConstant('{param:RELAUNCH|0}') = '1';
end;

function SteamRunning(): Boolean;
var
  ResultCode: Integer;
begin
  { tasklist prints the image name only when the process exists }
  Result := Exec(ExpandConstant('{cmd}'), '/C tasklist /FI "IMAGENAME eq ' + SteamExe + '" | find /I "' + SteamExe + '"',
                 '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

function SteamDir(): String;
begin
  if not RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', Result) then Result := '';
  StringChangeEx(Result, '/', '\', True);
end;

{ Asks Steam to exit and waits for it (up to 30 s). }
function CloseSteam(): Boolean;
var
  ResultCode, I: Integer;
begin
  if SteamDir() <> '' then
    Exec(SteamDir() + '\steam.exe', '-shutdown', '', SW_HIDE, ewNoWait, ResultCode);
  for I := 1 to 60 do begin
    if not SteamRunning() then Break;
    Sleep(500);
  end;
  Sleep(1000);
  Result := not SteamRunning();
end;

{ Steam rewrites its shortcut list when it exits, so it has to be closed while ours is added/removed. }
function SteamClosedFor(const What: String): Boolean;
begin
  Result := True;
  if not SteamRunning() then Exit;
  if SuppressibleMsgBox('Steam is running. To ' + What + ', Steam has to be closed for a moment.' + #13#10#13#10 +
                        'Close Steam now? (No = skip this step)', mbConfirmation, MB_YESNO, IDNO) = IDYES then begin
    Result := CloseSteam();
    if not Result then
      SuppressibleMsgBox('Steam did not close, so this step was skipped.', mbError, MB_OK, IDOK);
  end else
    Result := False;
end;

function RunLauncher(const Params: String): Integer;
begin
  if not Exec(ExpandConstant('{app}\gdvr_launch.exe'), Params, ExpandConstant('{app}'), SW_HIDE,
              ewWaitUntilTerminated, Result) then
    Result := -1;
end;

procedure AddToSteam();
var
  Code: Integer;
begin
  if not SteamClosedFor('add Grim Dawn VR to your Steam library') then Exit;
  Code := RunLauncher('--add-to-steam');
  if Code = 3 then
    SuppressibleMsgBox('Steam was not found. Add gdvr_launch.exe to Steam yourself (Games, Add a Non-Steam Game).',
                       mbInformation, MB_OK, IDOK)
  else if Code <> 0 then
    SuppressibleMsgBox('Grim Dawn VR could not be added to Steam (see %LOCALAPPDATA%\gdvr\install.log). ' +
                       'Add gdvr_launch.exe yourself: Games, Add a Non-Steam Game.', mbError, MB_OK, IDOK);
end;

procedure AddDefenderExclusions();
var
  Cmd: String;
  ResultCode: Integer;
begin
  Cmd := '-NoProfile -ExecutionPolicy Bypass -Command "Add-MpPreference -ExclusionPath ''' + ExpandConstant('{app}') +
         ''',''' + ExpandConstant('{localappdata}\gdvr') + '''"';
  if ShellExec('runas', 'powershell.exe', Cmd, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
    SaveStringToFile(ExpandConstant('{app}\defender-exclusions.txt'),
                     ExpandConstant('{app}') + #13#10 + ExpandConstant('{localappdata}\gdvr') + #13#10, False)
  else
    SuppressibleMsgBox('The Defender exclusions were not added. The README explains how to add them yourself.',
                       mbInformation, MB_OK, IDOK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    if WizardIsTaskSelected('steam') then AddToSteam();
    if WizardIsTaskSelected('defender') then AddDefenderExclusions();
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Code, ResultCode: Integer;
  Cmd: String;
begin
  if CurUninstallStep <> usUninstall then Exit;
  { Before the files go: the launcher removes the game folder's dinput8.dll, puts the normal options.txt back
    and removes the Steam entry (only with Steam closed). }
  if SteamRunning() and
     (SuppressibleMsgBox('Steam is running. Close it so the "Grim Dawn VR" entry can be removed from your library?' + #13#10 +
                         '(No = leave the entry; remove it in Steam yourself)', mbConfirmation, MB_YESNO,
                         IDNO) = IDYES) then
    CloseSteam();
  Code := RunLauncher('--uninstall');
  if Code = 2 then
    SuppressibleMsgBox('The "Grim Dawn VR" entry is still in your Steam library: right-click it in Steam, Manage, ' +
                       'Remove non-Steam game from your library.', mbInformation, MB_OK, IDOK);
  if FileExists(ExpandConstant('{app}\defender-exclusions.txt')) then begin
    Cmd := '-NoProfile -ExecutionPolicy Bypass -Command "Remove-MpPreference -ExclusionPath ''' +
           ExpandConstant('{app}') + ''',''' + ExpandConstant('{localappdata}\gdvr') + '''"';
    ShellExec('runas', 'powershell.exe', Cmd, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    DeleteFile(ExpandConstant('{app}\defender-exclusions.txt'));
  end;
  if DirExists(ExpandConstant('{localappdata}\gdvr')) and
     (SuppressibleMsgBox('Also delete the mod''s settings, logs and save backups (%LOCALAPPDATA%\gdvr)?',
                         mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES) then
    DelTree(ExpandConstant('{localappdata}\gdvr'), True, True, True);
end;
