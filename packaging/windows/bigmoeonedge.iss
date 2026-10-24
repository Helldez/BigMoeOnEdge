; The per-user Windows installer, built by the release workflow from the staged bmoe-desktop folder
; the archive also carries (bmoe-server.exe, its DLLs and backend modules, the C++ runtime, ui\,
; catalog\, the licenses and README.txt). No administrator rights: it installs under the user's
; local app data and adds a Start-menu entry that starts bmoe-server.exe, which opens the app in the
; browser when started without a terminal of its own (platform::started_by_double_click); closing
; its console window stops it. Settings and models stay in the server's data folder
; (%LOCALAPPDATA%\BigMoeOnEdge), which an uninstall leaves alone.
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
UninstallDisplayIcon={app}\bmoe-server.exe
UninstallDisplayName=BigMoeOnEdge
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; An update installs over a running server: the installer asks to close it first.
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; The staged folder is the whole app: files an older version had and this one does not go.
Type: filesandordirs; Name: "{app}\ui"
Type: filesandordirs; Name: "{app}\catalog"

[Icons]
Name: "{autoprograms}\BigMoeOnEdge"; Filename: "{app}\bmoe-server.exe"; WorkingDir: "{app}"; Comment: "Start BigMoeOnEdge and open it in the browser"
Name: "{autodesktop}\BigMoeOnEdge"; Filename: "{app}\bmoe-server.exe"; WorkingDir: "{app}"; Comment: "Start BigMoeOnEdge and open it in the browser"; Tasks: desktopicon

[Run]
Filename: "{app}\bmoe-server.exe"; Description: "Start BigMoeOnEdge now"; WorkingDir: "{app}"; Flags: postinstall nowait skipifsilent
