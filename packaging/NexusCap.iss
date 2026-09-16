; NexusCap — Inno Setup 6
; Compilar via tools\make_installer.ps1 (define MyAppVersion / StageDir / SetupIconFile).
;
; NÃO embute Lossless Scaling — Frame Gen é opcional (Steam do usuário).
; Instala em %LOCALAPPDATA%\NexusCap (gravável) — RenoDX precisa escrever ini/cfg/cache.

#ifndef MyAppVersion
  #define MyAppVersion "1.1.6"
#endif
#ifndef StageDir
  #define StageDir "..\dist\stage"
#endif
; Sempre relativo ao .iss — caminhos absolutos com \Users quebram o ISPP (\U = escape).
#define SetupIconFile "logo.ico"

#define MyAppName "NexusCap"
#define MyAppPublisher "NexusCap"
#define MyAppURL "https://github.com/lealvjo/NexusCap"
#define MyAppExeName "NexusCap.exe"

[Setup]
AppId={{A8C3E2F1-9B4D-4E6A-8F21-7C0D5B91E3A2}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
; Pasta do usuario: ReShade/RenoDX gravam ini, cfg e cache ao lado do exe.
DefaultDirName={localappdata}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
LicenseFile=
InfoBeforeFile=NOTICE.txt
OutputDir=..\dist
OutputBaseFilename=NexusCap-Setup-{#MyAppVersion}
SetupIconFile={#SetupIconFile}
UninstallDisplayIcon={app}\logo.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DisableProgramGroupPage=no
CloseApplications=yes
RestartApplications=no
MinVersion=10.0
UsePreviousAppDir=no

[Languages]
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Dirs]
; Se o usuario escolher Program Files (via dialogo elevacao), libera escrita.
Name: "{app}"; Permissions: users-modify
Name: "{app}\reshade-cache"; Permissions: users-modify

[Files]
; Staging limpo gerado por make_installer.ps1 (sem logs / dumps / LS).
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\logo.ico"
Name: "{group}\Desinstalar {#MyAppName}"; Filename: "{uninstallexe}"; IconFilename: "{app}\logo.ico"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\logo.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Abrir {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\reshade-cache"
Type: files; Name: "{app}\*.log"
Type: files; Name: "{app}\*.dmp"
