; PulseX CV - the installer (Inno Setup 6, https://jrsoftware.org/isinfo.php). 2026-10-02.
; For people who do not want to unpack a zip and find the right exe: one file, next, next, done.
;
; What goes in is this repository's own published files: bin\ (the two programs and the icon), lang\, examples\, the
; license texts and the README. Nothing else - no model, no personal data, no drafts.
;
; * Per user, no administrator: the program writes its drafts to work\ NEXT TO ITSELF, so it has to live in a folder
;   the user may write to. {autopf} is %LOCALAPPDATA%\Programs when PrivilegesRequired=lowest.
; * ARM64 only: the binaries are built for Snapdragon X.
; * The user's own data (Documents\pulse_cv: details, merits, model list, keys) is never touched - not by the
;   installer and not by the uninstaller. The uninstaller removes the program and its work\ drafts.
; * No model is installed. The window says so at first start and shows where to choose one.
;
;   ISCC.exe installer\pulsex_cv.iss                 from a clone: packs the clone, writes installer\output\
;   ISCC.exe /DSrc=<folder> /DOut=<folder> /DAppVersion=0.2 installer\pulsex_cv.iss

#ifndef Src
  #define Src SourcePath + "\.."
#endif
#ifndef Out
  #define Out SourcePath + "\output"
#endif
#ifndef AppVersion
  #define AppVersion "0.2"
#endif
#define AppName "PulseX CV"

[Setup]
AppId={{177A53F8-30E7-434E-8110-EB1E8464A035}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=PulseCore
AppPublisherURL=https://github.com/Erik-matrix/pulsex-cv
AppSupportURL=https://github.com/Erik-matrix/pulsex-cv
AppUpdatesURL=https://github.com/Erik-matrix/pulsex-cv/releases/latest
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
AllowNoIcons=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
LicenseFile={#Src}\LICENSE
OutputDir={#Out}
OutputBaseFilename=PulseX-CV-Setup-{#AppVersion}
SetupIconFile={#Src}\bin\pulse_cv.ico
UninstallDisplayIcon={app}\pulse_cv.ico
UninstallDisplayName={#AppName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
VersionInfoVersion={#AppVersion}.0.0
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} Setup

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "swedish"; MessagesFile: "compiler:Languages\Swedish.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Dirs]
Name: "{app}\work"

[Files]
Source: "{#Src}\bin\pulse_cv_gui.exe";    DestDir: "{app}"; Flags: ignoreversion
Source: "{#Src}\bin\pulse_cv_tailor.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Src}\bin\pulse_cv.ico";        DestDir: "{app}"; Flags: ignoreversion
Source: "{#Src}\lang\*.json";             DestDir: "{app}\lang"; Flags: ignoreversion
Source: "{#Src}\examples\*";              DestDir: "{app}\examples"; Flags: ignoreversion
Source: "{#Src}\LICENSE";                 DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "{#Src}\THIRD_PARTY_NOTICES.md";  DestDir: "{app}"; Flags: ignoreversion
Source: "{#Src}\README.md";               DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\pulse_cv_gui.exe"; WorkingDir: "{app}"; IconFilename: "{app}\pulse_cv.ico"
Name: "{autodesktop}\{#AppName}";  Filename: "{app}\pulse_cv_gui.exe"; WorkingDir: "{app}"; IconFilename: "{app}\pulse_cv.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\pulse_cv_gui.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; the drafts of the last letter (the program writes them next to itself); Documents\pulse_cv is the user's and stays
Type: filesandordirs; Name: "{app}\work"
