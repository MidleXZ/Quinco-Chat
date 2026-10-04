#ifndef AppVersion
#define AppVersion "0.1.0"
#endif

[Setup]
AppId={{C9C3A11E-1D91-47B3-8886-1FB73BC9001C}
AppName=Quinco Chat Server
AppVersion={#AppVersion}
DefaultDirName={autopf}\Quinco Chat Server
DefaultGroupName=Quinco Chat Server
OutputDir=..\..\build\release\dist
OutputBaseFilename=QuincoChat-setup
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
PrivilegesRequired=admin
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Files]
Source: "..\..\build\release\quinco-chat-server.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\public\*"; DestDir: "{app}\public"; Flags: ignoreversion recursesubdirs createallsubdirs

[Dirs]
Name: "{userappdata}\Quinco Chat Server"

[Icons]
Name: "{group}\Quinco Chat Server"; Filename: "{app}\quinco-chat-server.exe"; Parameters: "--data ""{userappdata}\Quinco Chat Server\data"""; WorkingDir: "{userappdata}\Quinco Chat Server"