; AuraLite-Defrag Inno Setup 6 Installer Script (Apache-2.0)
#define AppName "AuraLite-Defrag"
#define AppVersion "1.0.0"
#define AppPublisher "AuraLite-Defrag Contributors"
#define AppExeName "AuraLiteDefrag.exe"

[Setup]
AppId={{8E3C4F92-6A1B-4C7D-9E55-1204A8F1C901}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\..\LICENSE
OutputDir=..\..\dist
OutputBaseFilename=AuraLite-Defrag-{#AppVersion}-x64-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
UninstallDisplayIcon={app}\{#AppExeName}

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "contextmenu"; Description: "Add 'Defragment with AuraLite-Defrag' to Explorer context menu"; Flags: unchecked

[Files]
Source: "..\..\build\Release\AuraLiteDefrag.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\build\Release\auralite-cli.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\NOTICE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\THIRD-PARTY.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Classes\*\shell\AuraLiteDefrag"; ValueType: string; ValueData: "Defragment with AuraLite-Defrag"; Tasks: contextmenu; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\*\shell\AuraLiteDefrag\command"; ValueType: string; ValueData: """{app}\auralite-cli.exe"" file --target ""%1"""; Tasks: contextmenu
Root: HKCU; Subkey: "Software\Classes\Directory\shell\AuraLiteDefrag"; ValueType: string; ValueData: "Defragment with AuraLite-Defrag"; Tasks: contextmenu; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\Directory\shell\AuraLiteDefrag\command"; ValueType: string; ValueData: """{app}\auralite-cli.exe"" folder --target ""%1"""; Tasks: contextmenu

[UninstallDelete]
Type: files; Name: "{app}\*.log"
