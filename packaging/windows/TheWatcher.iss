#define AppName "TheWatcher"
#define AppVersion GetEnv("THEWATCHER_VERSION")
#if AppVersion == ""
#define AppVersion "0.9.0"
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
; Upgrades: a service registered by an older installer still points at
; server.json and CreateService does not update an existing registration, so
; remove the old registration first. This fails harmlessly on a fresh install.
Filename: "{app}\server\TheWatcherServer.exe"; Parameters: "--uninstall-service"; Flags: runhidden waituntilterminated
Filename: "{app}\server\TheWatcherServer.exe"; Parameters: "--install-service --config ""{commonappdata}\TheWatcher\TheWatcherServer.conf"""; Flags: runhidden waituntilterminated
Filename: "{app}\agent\TheWatcherAgent.exe"; Parameters: "--install-service --config ""{commonappdata}\TheWatcher\TheWatcherAgent.conf"""; Flags: runhidden waituntilterminated

[UninstallRun]
Filename: "{app}\agent\TheWatcherAgent.exe"; Parameters: "--uninstall-service"; Flags: runhidden waituntilterminated
Filename: "{app}\server\TheWatcherServer.exe"; Parameters: "--uninstall-service"; Flags: runhidden waituntilterminated

[Icons]
Name: "{group}\TheWatcher Dashboard"; Filename: "{app}\dashboard\index.html"; Check: DashboardExists

[Code]
// Upgrades: stop the running services before any file is replaced, so the
// executables are not locked and the service registration can be deleted and
// recreated immediately afterwards.
// - A service that does not exist (fresh install) is skipped: sc query fails
//   with 1060 (ERROR_SERVICE_DOES_NOT_EXIST).
// - Any other failure to query or stop an existing service aborts the install
//   with a message, so files are never replaced under a running service.
// `net stop` is only the request; the loop waits up to 30 seconds for sc query
// to report STOPPED. `net stop`'s own exit code is not used: it is also
// non-zero when the service was already stopped.
function StopServiceIfPresent(const Name: String): String;
var
  ResultCode, I: Integer;
begin
  Result := '';
  if not Exec(ExpandConstant('{sys}\sc.exe'), 'query ' + Name, '', SW_HIDE,
              ewWaitUntilTerminated, ResultCode) then
  begin
    Result := 'Could not run sc.exe to check the ' + Name + ' service.';
    Exit;
  end;
  if ResultCode = 1060 then
    Exit;
  if ResultCode <> 0 then
  begin
    Result := 'Could not query the ' + Name + ' service (sc.exe exit code ' + IntToStr(ResultCode) + ').';
    Exit;
  end;

  Exec(ExpandConstant('{sys}\net.exe'), 'stop ' + Name, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  for I := 1 to 30 do
  begin
    if Exec(ExpandConstant('{sys}\cmd.exe'), '/C sc query ' + Name + ' | find "STOPPED" >nul', '', SW_HIDE,
            ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
      Exit;
    Sleep(1000);
  end;
  Result := 'The ' + Name + ' service did not stop. Stop it manually and run the installer again.';
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := StopServiceIfPresent('TheWatcherAgent');
  if Result = '' then
    Result := StopServiceIfPresent('TheWatcherServer');
end;

// True when FileName has a non-comment KEY=VALUE line whose trimmed key is
// SERVER_SECRET_KEY and whose value is not empty. This mirrors the parsing
// rules of the server (trimmed keys and values, '#' comments, lines without
// '=' skipped), so a JSON file that merely mentions the key does not match.
function HasServerSecretKey(const FileName: String): Boolean;
var
  Lines: TArrayOfString;
  I, P: Integer;
  Line: String;
begin
  Result := False;
  if not LoadStringsFromFile(FileName, Lines) then
    Exit;
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    Line := Trim(Lines[I]);
    if (Line = '') or (Line[1] = '#') then
      Continue;
    P := Pos('=', Line);
    if P = 0 then
      Continue;
    if (Trim(Copy(Line, 1, P - 1)) = 'SERVER_SECRET_KEY') and (Trim(Copy(Line, P + 1, Length(Line))) <> '') then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

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

// Older installers registered the server service with server.json. The server
// wrote a KEY=VALUE file there (the same format as TheWatcherServer.conf), so
// on upgrade the file is COPIED to TheWatcherServer.conf. If that were not
// done the service would start on a missing .conf and load_or_create would
// generate a new CURVE keypair, breaking every enrolled agent.
// - An existing TheWatcherServer.conf is never overwritten.
// - server.json is left in place as a backup.
// - A server.json that does not look like a KEY=VALUE config (for example one
//   hand-written as JSON) is not converted; see the upgrade notes in
//   docs/installation.md.
procedure MigrateServerConfig(const Dir: String);
var
  OldConf, NewConf: String;
begin
  OldConf := AddBackslash(Dir) + 'server.json';
  NewConf := AddBackslash(Dir) + 'TheWatcherServer.conf';
  if FileExists(NewConf) or not FileExists(OldConf) then
    Exit;
  if not HasServerSecretKey(OldConf) then
  begin
    Log('server.json is not a KEY=VALUE server config; not migrated');
    Exit;
  end;
  if not FileCopy(OldConf, NewConf, True) then
    RaiseException('Could not copy ' + OldConf + ' to ' + NewConf);
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

  // After the ACL reset so the copied file inherits the restricted ACL.
  MigrateServerConfig(Dir);
end;
