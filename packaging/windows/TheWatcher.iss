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
; and Administrators only. It runs from PrepareToInstall, before any file is
; touched, so a failure aborts the install cleanly. SIDs are used so it works on any Windows language.
Name: "{commonappdata}\TheWatcher"

[UninstallRun]
Filename: "{app}\agent\TheWatcherAgent.exe"; Parameters: "--uninstall-service"; Flags: runhidden waituntilterminated
Filename: "{app}\server\TheWatcherServer.exe"; Parameters: "--uninstall-service"; Flags: runhidden waituntilterminated

[Icons]
Name: "{group}\TheWatcher Dashboard"; Filename: "{app}\dashboard\index.html"; Check: DashboardExists

[Code]
var
  RegistrationError: String;
  RegistrationFailureText: String;

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
  // An unreadable legacy config aborts the install: carrying on would let the
  // server start on a missing .conf and generate a new keypair.
  if not LoadStringsFromFile(FileName, Lines) then
    RaiseException('Could not read ' + FileName + '; the upgrade cannot preserve the server config.');
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

// True when the service is registered. sc query exits 1060
// (ERROR_SERVICE_DOES_NOT_EXIST) for a missing service.
function ServiceExists(const Name: String): Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(ExpandConstant('{sys}\sc.exe'), 'query ' + Name, '', SW_HIDE,
                 ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

// (Re)registers a service so its command line points at ConfPath. Service
// registration lives here instead of [Run] because [Run] cannot check exit
// codes. Any failure aborts the install with a message.
// - An existing registration (an upgrade, possibly still pointing at the old
//   server.json) is removed first. A missing service is not an error: the
//   uninstall is skipped. After removing it we wait up to 30 seconds for the
//   service to disappear, since a service with open handles (for example
//   services.msc) is only marked for deletion.
// - Registration is retried 5 times, 2 seconds apart, because it fails while
//   the old service is still marked for deletion (ERROR_SERVICE_MARKED_FOR_
//   DELETE, 1072; the binary reports that as a non-zero exit code, not 1072).
procedure RegisterService(const Name, Exe, ConfPath: String);
var
  ResultCode, I: Integer;
  Registered: Boolean;
begin
  if ServiceExists(Name) then
  begin
    if not Exec(Exe, '--uninstall-service', '', SW_HIDE, ewWaitUntilTerminated, ResultCode)
       or (ResultCode <> 0) then
      RaiseException('Could not remove the existing ' + Name + ' service (exit code ' + IntToStr(ResultCode) + ').');
    for I := 1 to 30 do
    begin
      if not ServiceExists(Name) then
        Break;
      Sleep(1000);
    end;
    if ServiceExists(Name) then
      RaiseException('The old ' + Name + ' service is still marked for deletion. Close services.msc and run the installer again.');
  end;

  Registered := False;
  for I := 1 to 5 do
  begin
    if Exec(Exe, '--install-service --config "' + ConfPath + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode)
       and (ResultCode = 0) then
    begin
      Registered := True;
      Break;
    end;
    Sleep(2000);
  end;
  if not Registered then
    RaiseException('Could not register the ' + Name + ' service (exit code ' + IntToStr(ResultCode) + ').');
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
  if not ForceDirectories(Dir) then
    RaiseException('Could not create ' + Dir);
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

// Steps that must not fail silently run here, before any file is touched:
// Inno Setup catches exceptions raised from [Dirs] AfterInstall and from
// CurStepChanged and carries on, but a non-empty result from PrepareToInstall
// aborts the install cleanly. Order: stop the services, create and lock down
// the config directory, migrate the legacy server config. Migration must
// complete before the service is registered, otherwise the server could start
// on a missing TheWatcherServer.conf and generate a new CURVE keypair, which
// breaks every enrolled agent.
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := StopServiceIfPresent('TheWatcherAgent');
  if Result = '' then
    Result := StopServiceIfPresent('TheWatcherServer');
  if Result = '' then
  begin
    try
      SecureConfigDir();
    except
      Result := 'Could not prepare the configuration directory: ' + GetExceptionMessage();
    end;
  end;
end;

// Registration needs the installed executables, so it runs after the files are
// copied (ssPostInstall) and cannot abort the install. Failures are recorded,
// shown (SuppressibleMsgBox, so silent installs log them), reflected in the
// finished page and returned as the setup exit code.
procedure RegisterAll();
var
  Msg: String;
begin
  try
    RegisterService('TheWatcherServer', ExpandConstant('{app}\server\TheWatcherServer.exe'),
                    ExpandConstant('{commonappdata}\TheWatcher\TheWatcherServer.conf'));
  except
    RegistrationError := GetExceptionMessage();
  end;
  try
    RegisterService('TheWatcherAgent', ExpandConstant('{app}\agent\TheWatcherAgent.exe'),
                    ExpandConstant('{commonappdata}\TheWatcher\TheWatcherAgent.conf'));
  except
    if RegistrationError <> '' then
      RegistrationError := RegistrationError + #13#10;
    RegistrationError := RegistrationError + GetExceptionMessage();
  end;
  if RegistrationError <> '' then
  begin
    Msg := 'The files were installed but service registration failed:' + #13#10 + RegistrationError + #13#10#13#10 +
           'Fix the problem, then run as Administrator:' + #13#10 +
           ExpandConstant('"{app}\server\TheWatcherServer.exe" --install-service --config "{commonappdata}\TheWatcher\TheWatcherServer.conf"') + #13#10 +
           ExpandConstant('"{app}\agent\TheWatcherAgent.exe" --install-service --config "{commonappdata}\TheWatcher\TheWatcherAgent.conf"');
    RegistrationFailureText := Msg;
    SuppressibleMsgBox(Msg, mbError, MB_OK, IDOK);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    RegisterAll();
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and (RegistrationError <> '') then
    WizardForm.FinishedLabel.Caption := RegistrationFailureText;
end;

// Non-zero when service registration failed, so silent installs report it.
function GetCustomSetupExitCode(): Integer;
begin
  if RegistrationError <> '' then
    Result := 10
  else
    Result := 0;
end;
