#define MyAppName "Berlenga"
#define MyAppVersion "4.8"
#define MyAppPublisher "Berlenga contributors"
#define MyAppURL "https://berlenga.canilho.net/"
#define MyAppExeName "godot.exe"

[Setup]
AppId={{FC00E23C-9975-4441-B506-22F4717829CB}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
; Don't add "version {version}" to the installed app name in the Add/Remove Programs
; dialog as it's redundant with the Version field in that same dialog.
AppVerName={#MyAppName}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
AppComments=Godot Engine editor
ChangesEnvironment=yes
DefaultDirName={localappdata}\Godot
DefaultGroupName=Godot Engine
AllowNoIcons=yes
UninstallDisplayIcon={app}\{#MyAppExeName}
#ifdef App32Bit
  OutputBaseFilename=godot-setup-x86
#else
  OutputBaseFilename=godot-setup-x86_64
  ArchitecturesAllowed=x64
  ArchitecturesInstallIn64BitMode=x64
#endif
Compression=lzma
SolidCompression=yes
PrivilegesRequired=lowest

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "modifypath"; Description: "Add Godot to PATH environment variable"

[Files]
Source: "{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{commondesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[Code]
const
    ModPathName = 'modifypath';
    ModPathType = 'user';

function ModPathDir(): TArrayOfString;
begin
    setArrayLength(Result, 1)
    Result[0] := ExpandConstant('{app}');
end;

#include "modpath.pas"
