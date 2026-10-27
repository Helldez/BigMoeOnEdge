; The per-user Windows installer, built by the release workflow from its staged app folder
; (bmoe-desktop.exe, bmoe-server.exe, their DLLs and backend modules, the C++ runtime, ui\, catalog\,
; skills\ when there is one, the licenses). No administrator rights: it installs under the user's
; local app data and adds a Start-menu entry that starts bmoe-desktop.exe, the app's window, which
; starts bmoe-server.exe beside it and keeps an icon in the notification area. Settings, models and
; backend packs stay in the server's data folder (%LOCALAPPDATA%\BigMoeOnEdge), which an uninstall
; leaves alone. The app's update runs this same installer over the installed copy.
;
;   ISCC.exe /DAppVersion=0.35.1 /DSourceDir=<staged folder> /DOutputDir=<dir>
;            /DOutputBaseName=BigMoeOnEdge-<tag>-<target>-setup /DArch=x64compatible|arm64
;            packaging\windows\bigmoeonedge.iss

#ifndef AppVersion
  #error Pass /DAppVersion=X.Y.Z
#endif
#ifndef SourceDir
  #error Pass /DSourceDir=<staged bmoe-desktop folder>
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
#ifndef OutputBaseName
  #define OutputBaseName "BigMoeOnEdge-setup"
#endif
#ifndef Arch
  #define Arch "x64compatible"
#endif

[Setup]
AppId=BigMoeOnEdge
AppName=BigMoeOnEdge
AppVersion={#AppVersion}
AppVerName=BigMoeOnEdge {#AppVersion}
AppPublisher=BigMoeOnEdge
AppPublisherURL=https://github.com/Helldez/BigMoeOnEdge
AppSupportURL=https://github.com/Helldez/BigMoeOnEdge/issues
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Programs\BigMoeOnEdge
DisableDirPage=auto
DefaultGroupName=BigMoeOnEdge
DisableProgramGroupPage=yes
ArchitecturesAllowed={#Arch}
ArchitecturesInstallIn64BitMode={#Arch}
LicenseFile={#SourceDir}\LICENSE
UninstallDisplayIcon={app}\bmoe-desktop.exe
UninstallDisplayName=BigMoeOnEdge
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; An update installs over a running app: the installer asks to close it (and its server) first.
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; The staged folder is the whole app: files an older version had and this one does not go.
Type: filesandordirs; Name: "{app}\ui"
Type: filesandordirs; Name: "{app}\catalog"
Type: filesandordirs; Name: "{app}\skills"

[Icons]
Name: "{autoprograms}\BigMoeOnEdge"; Filename: "{app}\bmoe-desktop.exe"; WorkingDir: "{app}"; Comment: "Run mixture-of-experts language models on this machine"
Name: "{autodesktop}\BigMoeOnEdge"; Filename: "{app}\bmoe-desktop.exe"; WorkingDir: "{app}"; Comment: "Run mixture-of-experts language models on this machine"; Tasks: desktopicon

[Run]
Filename: "{app}\bmoe-desktop.exe"; Description: "Start BigMoeOnEdge now"; WorkingDir: "{app}"; Flags: postinstall nowait skipifsilent
