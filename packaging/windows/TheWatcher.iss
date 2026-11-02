#define AppName "TheWatcher"
#define AppVersion GetEnv("THEWATCHER_VERSION")
#if AppVersion == ""
#define AppVersion "0.8.0"
#endif

[Setup]
AppId={{B0EA81D4-B2A5-4F11-8D68-0D282E9BE3C3}
AppName={#AppName}
AppVersion={#AppVersion}
DefaultDirName={autopf}\TheWatcher
DefaultGroupName=TheWatcher
OutputBaseFilename=TheWatcher-{#AppVersion}-windows
OutputDir=dist
ArchitecturesInstallIn64BitMode=x64
PrivilegesRequired=admin
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Files]
Source: "..\..\builddir-release\server\TheWatcherServer.exe"; DestDir: "{app}\server"; Flags: ignoreversion
Source: "..\..\builddir-release\agent\TheWatcherAgent.exe"; DestDir: "{app}\agent"; Flags: ignoreversion
Source: "..\..\dashboard\dist\*"; DestDir: "{app}\dashboard"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist

[Dirs]
; SEC-45: the config directory holds SERVER_SECRET_KEY, the Postgres DSN and
; AGENT_SECRET_KEY. Do NOT grant Users modify here — previous installers did,
; which let any local user read both CURVE secrets and edit configs consumed
; by SYSTEM services. The [Run] entry below resets the ACL to SYSTEM and
; Administrators only (SIDs are used so it works on any Windows language).
Name: "{commonappdata}\TheWatcher"

[Run]
; SEC-45: strip inherited ACEs and grant full control to Administrators
; (*S-1-5-32-544) and SYSTEM (*S-1-5-18) only. Must run before the service
; installers so the config files they create inherit the restricted ACL.
Filename: "{sys}\icacls.exe"; Parameters: """{commonappdata}\TheWatcher"" /inheritance:r /grant:r *S-1-5-32-544:(OI)(CI)F *S-1-5-18:(OI)(CI)F"; Flags: runhidden
Filename: "{app}\server\TheWatcherServer.exe"; Parameters: "--install-service --config ""{commonappdata}\TheWatcher\server.json"""; Flags: runhidden waituntilterminated
Filename: "{app}\agent\TheWatcherAgent.exe"; Parameters: "--install-service --config ""{commonappdata}\TheWatcher\TheWatcherAgent.conf"""; Flags: runhidden waituntilterminated

[UninstallRun]
Filename: "{app}\agent\TheWatcherAgent.exe"; Parameters: "--uninstall-service"; Flags: runhidden waituntilterminated
Filename: "{app}\server\TheWatcherServer.exe"; Parameters: "--uninstall-service"; Flags: runhidden waituntilterminated

[Icons]
Name: "{group}\TheWatcher Dashboard"; Filename: "{app}\dashboard\index.html"; Check: DashboardExists

[Code]
function DashboardExists(): Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\dashboard\index.html'));
end;
