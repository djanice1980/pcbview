; Inno Setup script for pcbview. Build after `cmake --build build --config
; Release --target deploy` has staged everything into build\Release:
;   ISCC.exe installer\pcbview.iss
; Output: installer\Output\pcbview-<version>-setup.exe

#define AppVersion "1.20.0"

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
; Stable name in Add/Remove Programs. Inno's default folds the version into
; DisplayName ("pcbview version 1.17.0"), which changes every release and so
; cannot be matched on by a deployment tool; the version is already published
; separately as DisplayVersion.
UninstallDisplayName=pcbview
; The setup.exe's OWN version resource. Without these Inno leaves FileVersion
; blank, so a deployment tool that reads the installer's version -- rather than
; the version it eventually registers -- sees nothing.
VersionInfoVersion={#AppVersion}
VersionInfoProductVersion={#AppVersion}
VersionInfoProductName=pcbview
VersionInfoCompany=David Janice
VersionInfoDescription=pcbview installer
VersionInfoCopyright=Copyright (C) David Janice

; Authenticode signing. Unsigned binaries make Windows show "Unknown publisher"
; on the UAC prompt and give SmartScreen nothing to build reputation against --
; for an unsigned file that reputation is tracked per FILE HASH, so every
; release starts from zero and the warning never goes away. With a certificate
; it accrues to the certificate instead and carries across releases.
;
; To sign, configure a tool named "pcbsign" in the Inno Setup IDE
; (Tools > Configure Sign Tools) with a command line such as:
;   signtool.exe sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 /f "cert.pfx" /p "$p" $f
; then build with:  ISCC.exe /Spcbsign="..." installer\pcbview.iss
; and uncomment the two directives below. SignedUninstaller matters because the
; uninstaller is generated at install time and is otherwise unsigned even when
; the installer is.
;SignTool=pcbsign
;SignedUninstaller=yes

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
