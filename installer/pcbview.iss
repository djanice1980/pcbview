; Inno Setup script for pcbview. Build after `cmake --build build --config
; Release --target deploy` has staged everything into build\Release:
;   ISCC.exe installer\pcbview.iss
; Output: installer\Output\pcbview-<version>-setup.exe

#define AppVersion "1.13"

[Setup]
AppId={{7E1F7A2C-9B7D-4A63-B7B1-52D1C0B4D6E1}
AppName=pcbview
AppVersion={#AppVersion}
AppPublisher=David Janice
AppPublisherURL=https://github.com/djanice1980/pcbview
DefaultDirName={autopf}\pcbview
DefaultGroupName=pcbview
LicenseFile=..\LICENSE
OutputDir=Output
OutputBaseFilename=pcbview-{#AppVersion}-setup
SetupIconFile=..\assets\pcbview.ico
UninstallDisplayIcon={app}\pcbview.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Per-user install works too; let the user pick (admin -> Program Files).
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
; dxcompiler/dxil are dragged in by windeployqt but unused by pcbview.
Source: "..\build\Release\*"; DestDir: "{app}"; Excludes: "dxcompiler.dll,dxil.dll"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\pcbview"; Filename: "{app}\pcbview.exe"
Name: "{group}\Uninstall pcbview"; Filename: "{uninstallexe}"
Name: "{autodesktop}\pcbview"; Filename: "{app}\pcbview.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\pcbview.exe"; Description: "Launch pcbview"; Flags: nowait postinstall skipifsilent
