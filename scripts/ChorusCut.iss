; Inno Setup script for the ChorusCut Windows package. package_windows.sh builds
; dist/stage/ChorusCut first, then invokes this; CHORUSCUT_VERSION carries the
; release version (default keeps local builds obviously non-release).
#define AppVersion GetEnv('CHORUSCUT_VERSION') != '' ? GetEnv('CHORUSCUT_VERSION') : '0.1.0'

[Setup]
AppId={{081B6C28-D4E6-419C-A19F-7B8E4FC5D4F0}
AppName=ChorusCut
AppVersion={#AppVersion}
AppPublisher=drphero
DefaultDirName={autopf}\ChorusCut
DefaultGroupName=ChorusCut
OutputDir=..\dist
OutputBaseFilename=ChorusCut-{#AppVersion}-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
UninstallDisplayIcon={app}\bin\ChorusCut.exe
PrivilegesRequired=admin

[Files]
Source: "..\dist\stage\ChorusCut\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked

[Icons]
; The exe lives in bin\ because MLT resolves its modules at <exedir>\..\lib\mlt.
Name: "{autoprograms}\ChorusCut"; Filename: "{app}\bin\ChorusCut.exe"; WorkingDir: "{app}\bin"; AppUserModelID: "ChorusCut"
Name: "{autodesktop}\ChorusCut"; Filename: "{app}\bin\ChorusCut.exe"; WorkingDir: "{app}\bin"; Tasks: desktopicon; AppUserModelID: "ChorusCut"
