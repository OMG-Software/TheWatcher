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
; by SYSTEM services. SecureConfigDir (see [Code]) resets the ACL to SYSTEM
; and Administrators only and aborts the install if that fails. It runs here,
; during the file-copy phase, so it always completes before the [Run] entries
; that install the services. SIDs are used so it works on any Windows language.
Name: "{commonappdata}\TheWatcher"; AfterInstall: SecureConfigDir

[Run]
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

// SEC-45: run icacls and abort the install when it fails.
procedure RunIcacls(const Params: String);
var
  ResultCode: Integer;
begin
  if not Exec(ExpandConstant('{sys}\icacls.exe'), Params, '', SW_HIDE,
              ewWaitUntilTerminated, ResultCode) then
    RaiseException('Could not start icacls.exe: ' + SysErrorMessage(ResultCode));
  if ResultCode <> 0 then
    RaiseException(Format('icacls.exe %s failed with exit code %d', [Params, ResultCode]));
end;

function DirHasEntries(const Dir: String): Boolean;
var
  FindRec: TFindRec;
begin
  Result := False;
  if FindFirst(AddBackslash(Dir) + '*', FindRec) then
  begin
    try
      repeat
        Result := (FindRec.Name <> '.') and (FindRec.Name <> '..');
      until Result or not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

// SEC-45: restrict the config directory to Administrators (*S-1-5-32-544)
// and SYSTEM (*S-1-5-18).
// - /inheritance:r drops ACEs inherited from ProgramData.
// - /remove:g drops explicit grants that /grant:r does not replace. Installers
//   before 0.8.0 granted Users (S-1-5-32-545) modify; Authenticated Users
//   (S-1-5-11) and Everyone (S-1-1-0) are removed for the same reason.
// - Existing files and subfolders (upgrades) are reset so that they only
//   inherit the restricted ACL from the directory.
// The resulting ACLs are then checked in SDDL form, which does not depend on
// the Windows display language.
procedure SecureConfigDir();
var
  Dir, SddlFile, Sddl: String;
  Raw: AnsiString;
begin
  Dir := ExpandConstant('{commonappdata}\TheWatcher');
  RunIcacls('"' + Dir + '" /inheritance:r /remove:g *S-1-5-32-545 *S-1-5-11 *S-1-1-0 ' +
            '/grant:r *S-1-5-32-544:(OI)(CI)F *S-1-5-18:(OI)(CI)F');
  if DirHasEntries(Dir) then
    RunIcacls('"' + AddBackslash(Dir) + '*" /reset /T /C');

  SddlFile := ExpandConstant('{tmp}\thewatcher-acl.txt');
  RunIcacls('"' + Dir + '" /save "' + SddlFile + '" /T /C');
  if not LoadStringFromFile(SddlFile, Raw) then
    RaiseException('Could not read the saved ACL of ' + Dir);
  // icacls /save writes UTF-16; drop the NUL bytes before searching.
  Sddl := String(Raw);
  StringChangeEx(Sddl, #0, '', True);
  // SDDL trustee aliases: BU = Users, AU = Authenticated Users, WD = Everyone.
  if (Pos(';BU)', Sddl) > 0) or (Pos(';AU)', Sddl) > 0) or (Pos(';WD)', Sddl) > 0) then
    RaiseException('The ACL of ' + Dir + ' still grants access to non-administrators');
end;
