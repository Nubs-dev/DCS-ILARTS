; DCS ILARTS - Windows installer (Inno Setup 6). Built by deploy\package.ps1, which passes
;   /DVersion=1.0.27 /DStage=<the staged Saved Games tree> /DOutDir=<dist>
; Installs into DCS's Saved Games folder (per user, no administrator rights) the same files the mod-manager zips
; carry, then runs "ILARTS.exe --repair" (hook, autoexec.cfg permissions, old hook removed). The virtual camera
; component is optional (Windows 11; asks for administrator rights for that step only). Running the installer
; again repairs; so does Start menu > DCS ILARTS - Repair.

#ifndef Version
  #define Version "0.0.0"
#endif
#ifndef Stage
  #define Stage "..\dist\stage"
#endif
#ifndef OutDir
  #define OutDir "..\dist"
#endif
#define Bin "{app}\Mods\tech\ILARTS\bin"

[Setup]
AppId={{8E0C6A2B-5B7E-4C51-9C4B-2E9A61F0A3D7}
AppName=DCS ILARTS
AppVersion={#Version}
AppVerName=DCS ILARTS {#Version}
AppPublisher=DCS ILARTS
DefaultDirName={code:DefaultSavedGames}
AppendDefaultDirName=no
DirExistsWarning=no
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutDir}
OutputBaseFilename=DCS-ILARTS-Setup-{#Version}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\assets\ILARTS-light.ico
UninstallDisplayName=DCS ILARTS
UninstallDisplayIcon={#Bin}\ILARTS.exe
CloseApplications=yes
UninstallFilesDir={app}\Mods\tech\ILARTS\uninstall

[Messages]
SelectDirLabel3=DCS ILARTS goes into DCS's Saved Games folder.
SelectDirBrowseLabel=Usually Saved Games\DCS. Click Next to continue, or Browse to choose another (for example Saved Games\DCS.openbeta).

[Tasks]
Name: "cameras"; Description: "Virtual cameras for Discord, Teams and OBS (Windows 11, administrator rights)"; Flags: unchecked; Check: IsWindows11
Name: "desktopicon"; Description: "Desktop shortcut"; Flags: unchecked

[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{autoprograms}\DCS ILARTS"; Filename: "{#Bin}\ILARTS.exe"; WorkingDir: "{#Bin}"
Name: "{autoprograms}\DCS ILARTS - Repair"; Filename: "{#Bin}\ILARTS.exe"; Parameters: "--repair --report"; WorkingDir: "{#Bin}"
Name: "{autoprograms}\DCS ILARTS - User Guide"; Filename: "{app}\Mods\tech\ILARTS\DCS-ILARTS-Guide.pdf"
Name: "{autodesktop}\DCS ILARTS"; Filename: "{#Bin}\ILARTS.exe"; WorkingDir: "{#Bin}"; Tasks: desktopicon

[Run]
Filename: "{#Bin}\ILARTS.exe"; Parameters: "--repair"; StatusMsg: "Checking the hook and DCS's permissions..."; Flags: runhidden waituntilterminated
Filename: "{#Bin}\ILARTS.exe"; Parameters: "--install-cameras"; Verb: "runas"; StatusMsg: "Installing the virtual cameras..."; Flags: shellexec waituntilterminated runhidden; Tasks: cameras
Filename: "{#Bin}\ILARTS.exe"; Description: "Start DCS ILARTS"; WorkingDir: "{#Bin}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; autoexec.cfg: the block REPAIR added comes out again (anything else in the file stays; the backup goes once the
; file matches it again)
Filename: "{#Bin}\ILARTS.exe"; Parameters: "--remove-permissions"; Flags: runhidden waituntilterminated; RunOnceId: "RemovePermissions"
Filename: "{#Bin}\ILARTS.exe"; Parameters: "--uninstall-cameras"; Verb: "runas"; Flags: shellexec waituntilterminated runhidden; RunOnceId: "RemoveCameras"; Check: CamerasInstalled

[Code]
// Saved Games\DCS: the one that already has ILARTS, else DCS, else DCS.openbeta.
function DefaultSavedGames(Param: String): String;
var
  Base: String;
begin
  Base := ExpandConstant('{%USERPROFILE}\Saved Games\');
  if DirExists(Base + 'DCS\Mods\tech\ILARTS') then Result := Base + 'DCS'
  else if DirExists(Base + 'DCS.openbeta\Mods\tech\ILARTS') then Result := Base + 'DCS.openbeta'
  else if DirExists(Base + 'DCS') then Result := Base + 'DCS'
  else if DirExists(Base + 'DCS.openbeta') then Result := Base + 'DCS.openbeta'
  else Result := Base + 'DCS';
end;

function IsWindows11: Boolean;
var
  V: TWindowsVersion;
begin
  GetWindowsVersionEx(V);
  Result := V.Build >= 22000;
end;

function CamerasInstalled: Boolean;
begin
  Result := RegKeyExists(HKLM, 'SOFTWARE\Classes\CLSID\{6C1A5E70-3B1D-4E8A-9C27-1F0D5A4B7E00}\InprocServer32');
end;

// DCS holds ILARTS.dll while it runs.
function InitializeSetup: Boolean;
begin
  Result := True;
  while Result and ((FindWindowByWindowName('DCS') <> 0) or (FindWindowByWindowName('Digital Combat Simulator') <> 0)) do
    Result := MsgBox('DCS is running. Close DCS, then click OK to continue (Cancel quits).', mbInformation, MB_OKCANCEL) = IDOK;
end;
