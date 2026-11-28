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
const
  // The Z85 alphabet of common/crypto.hpp; a CURVE key is 40 of these.
  Z85Alphabet = '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-:+=^!/*?&<>()[]{}@%$#';

var
  RegistrationError: String;
  // Services that were running before the upgrade but did not start again.
  StartError: String;
  PostInstallFailureText: String;
  // Set by PrepareToInstall for each service it stopped, so the service is
  // started again after the upgrade, or when the install is aborted.
  AgentWasRunning, ServerWasRunning: Boolean;
  // True once the files are installed (ssPostInstall). DeinitializeSetup uses
  // it to tell an aborted install from a finished one.
  PostInstallReached: Boolean;

// True when sc query reports the service as STOPPED.
function ServiceIsStopped(const Name: String): Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(ExpandConstant('{sys}\cmd.exe'), '/C sc query ' + Name + ' | find "STOPPED" >nul', '', SW_HIDE,
                 ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

// Upgrades: stop the running services before any file is replaced, so the
// executables are not locked and the service registration can be deleted and
// recreated immediately afterwards.
// - A service that does not exist (fresh install) is skipped: sc query fails
//   with 1060 (ERROR_SERVICE_DOES_NOT_EXIST). So is one that is already
//   stopped.
// - Any other failure to query or stop an existing service aborts the install
//   with a message, so files are never replaced under a running service.
// `net stop` is only the request; the loop waits up to 30 seconds for sc query
// to report STOPPED. `net stop`'s own exit code is not used: it is also
// non-zero when the service was already stopped.
// WasRunning is set when the service existed and was not already stopped, so
// it can be started again after the upgrade or an aborted install.
function StopServiceIfPresent(const Name: String; var WasRunning: Boolean): String;
var
  ResultCode, I: Integer;
begin
  Result := '';
  WasRunning := False;
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
  if ServiceIsStopped(Name) then
    Exit;

  WasRunning := True;
  Exec(ExpandConstant('{sys}\net.exe'), 'stop ' + Name, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  for I := 1 to 30 do
  begin
    if ServiceIsStopped(Name) then
      Exit;
    Sleep(1000);
  end;
  Result := 'The ' + Name + ' service did not stop. Stop it manually and run the installer again.';
end;

// `net start` waits until the service reports RUNNING, or fails.
function StartServiceAgain(const Name: String): Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(ExpandConstant('{sys}\net.exe'), 'start ' + Name, '', SW_HIDE, ewWaitUntilTerminated, ResultCode)
            and (ResultCode = 0);
end;

// Starts a service again after an aborted install, if StopServiceIfPresent
// stopped it and it is still stopped. Returns a sentence for the abort
// message.
function RestartStoppedService(const Name: String; WasRunning: Boolean): String;
begin
  Result := '';
  if not WasRunning or not ServiceIsStopped(Name) then
    Exit;
  if StartServiceAgain(Name) then
    Result := ' The ' + Name + ' service was started again.'
  else
    Result := ' The ' + Name + ' service could not be started again; start it manually.';
end;

// Reads a KEY=VALUE config. An unreadable file aborts the install: carrying
// on would let the server start on a missing .conf and generate a new
// keypair.
procedure LoadConfigLines(const FileName: String; var Lines: TArrayOfString);
begin
  if not LoadStringsFromFile(FileName, Lines) then
    RaiseException('Could not read ' + FileName + '; the upgrade cannot preserve the server config.');
end;

// The value of Key, or '' when it is absent or empty. This mirrors the server
// parser (server/config.cpp): lines are trimmed, blank and '#' lines and
// lines without '=' are skipped, keys and values are trimmed, and a key that
// appears twice keeps its last value. A JSON file that merely mentions a key
// therefore does not match.
function ConfigValue(const Lines: TArrayOfString; const Key: String): String;
var
  I, P: Integer;
  Line: String;
begin
  Result := '';
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    Line := Trim(Lines[I]);
    if (Line = '') or (Line[1] = '#') then
      Continue;
    P := Pos('=', Line);
    if (P > 0) and (Trim(Copy(Line, 1, P - 1)) = Key) then
      Result := Trim(Copy(Line, P + 1, Length(Line)));
  end;
end;

// True when Value has the form of a CURVE key: 40 Z85 characters, which is
// what z85_decode in common/crypto.hpp accepts.
function IsZ85Key(const Value: String): Boolean;
var
  I: Integer;
begin
  Result := Length(Value) = 40;
  if not Result then
    Exit;
  for I := 1 to Length(Value) do
    if Pos(Copy(Value, I, 1), Z85Alphabet) = 0 then
    begin
      Result := False;
      Exit;
    end;
end;

function KeyProblem(const Lines: TArrayOfString; const Key: String): String;
var
  Value: String;
begin
  Result := '';
  Value := ConfigValue(Lines, Key);
  if Value = '' then
    Result := Key + ' is missing or empty'
  else if not IsZ85Key(Value) then
    Result := Key + ' is not a 40-character Z85 key';
end;

// Why the config in FileName cannot keep the server's CURVE identity, or ''
// when it can. Both keys are required: with only SERVER_SECRET_KEY the server
// loads an empty public key, runs without CURVE and sends no server key on
// enrollment, so agents that pinned the old key reject it. The message never
// contains a key value.
function ServerKeyProblem(const FileName: String): String;
var
  Lines: TArrayOfString;
  PublicProblem, SecretProblem: String;
begin
  LoadConfigLines(FileName, Lines);
  PublicProblem := KeyProblem(Lines, 'SERVER_PUBLIC_KEY');
  SecretProblem := KeyProblem(Lines, 'SERVER_SECRET_KEY');
  if (PublicProblem <> '') and (SecretProblem <> '') then
    Result := PublicProblem + ' and ' + SecretProblem
  else
    Result := PublicProblem + SecretProblem;
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
// - A server.json without both CURVE keys as KEY=VALUE lines (for example one
//   hand-written as JSON, or one with an empty SERVER_PUBLIC_KEY) is not
//   converted; see the upgrade notes in docs/installation.md.
// Pre-flight check, run before anything is stopped or changed. Returns an
// error message when a legacy server.json exists, there is no
// TheWatcherServer.conf yet, and server.json cannot be converted. Carrying on
// would register the server without its keypair: it would generate a new one,
// or run without CURVE, and every enrolled agent would have to be re-enrolled.
// Returns '' when the upgrade can proceed.
function CheckLegacyServerConfig(): String;
var
  Dir, OldConf, NewConf, Problem: String;
begin
  Result := '';
  Dir := ExpandConstant('{commonappdata}\TheWatcher');
  OldConf := AddBackslash(Dir) + 'server.json';
  NewConf := AddBackslash(Dir) + 'TheWatcherServer.conf';
  if FileExists(NewConf) or not FileExists(OldConf) then
    Exit;
  try
    Problem := ServerKeyProblem(OldConf);
    if Problem <> '' then
      Result := OldConf + ' cannot be migrated automatically: ' + Problem + '. ' +
                'Copy its settings, including SERVER_PUBLIC_KEY and SERVER_SECRET_KEY, into ' + NewConf +
                ' and run the installer again. Nothing has been changed.';
  except
    Result := GetExceptionMessage() + ' Nothing has been changed.';
  end;
end;

procedure MigrateServerConfig(const Dir: String);
var
  OldConf, NewConf, Problem: String;
begin
  OldConf := AddBackslash(Dir) + 'server.json';
  NewConf := AddBackslash(Dir) + 'TheWatcherServer.conf';
  if FileExists(NewConf) or not FileExists(OldConf) then
    Exit;
  // CheckLegacyServerConfig already refused a server.json that cannot be
  // converted; check again in case the file changed since.
  Problem := ServerKeyProblem(OldConf);
  if Problem <> '' then
    RaiseException(OldConf + ' was not migrated: ' + Problem);
  // NewConf did not exist above and the services are stopped, so whatever is
  // there after a failed or short copy (for example a full disk) is ours. It
  // is removed: left in place, the next run would skip the migration and the
  // server would start on a config without its keypair.
  if not FileCopy(OldConf, NewConf, True) then
  begin
    DeleteFile(NewConf);
    RaiseException('Could not copy ' + OldConf + ' to ' + NewConf);
  end;
  try
    Problem := ServerKeyProblem(NewConf);
  except
    Problem := GetExceptionMessage();
  end;
  if Problem <> '' then
  begin
    DeleteFile(NewConf);
    RaiseException('The copy of ' + OldConf + ' is incomplete (' + Problem + '); ' + NewConf + ' was removed');
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
// aborts the install cleanly. Order: check that a legacy server config can be
// migrated (before anything is stopped, so a refusal leaves the running
// install untouched), stop the services, create and lock down the config
// directory, migrate the legacy server config. Migration must complete before
// the service is registered, otherwise the server could start on a missing
// TheWatcherServer.conf and generate a new CURVE keypair, which breaks every
// enrolled agent.
// The services stopped here are started again in every outcome:
// - a later step here fails: no file has been replaced and the old
//   registrations still point at the old configs, so they are started here;
// - the file copy fails or is cancelled: DeinitializeSetup starts them;
// - the install finishes: RegisterAll starts them on the new registrations.
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  AgentWasRunning := False;
  ServerWasRunning := False;
  Result := CheckLegacyServerConfig();
  if Result = '' then
    Result := StopServiceIfPresent('TheWatcherAgent', AgentWasRunning);
  if Result = '' then
    Result := StopServiceIfPresent('TheWatcherServer', ServerWasRunning);
  if Result = '' then
  begin
    try
      SecureConfigDir();
    except
      Result := 'Could not prepare the configuration directory: ' + GetExceptionMessage();
    end;
  end;
  // The server first, so the agent finds it when it reconnects. The flags are
  // cleared so DeinitializeSetup does not try again.
  if Result <> '' then
  begin
    Result := Result + RestartStoppedService('TheWatcherServer', ServerWasRunning) +
              RestartStoppedService('TheWatcherAgent', AgentWasRunning);
    ServerWasRunning := False;
    AgentWasRunning := False;
  end;
end;

procedure AddStartError(const Name: String);
begin
  if StartError <> '' then
    StartError := StartError + ', ';
  StartError := StartError + Name;
end;

// Shows registration and start failures (SuppressibleMsgBox, so silent installs
// log them) and keeps the text for the finished page.
procedure ReportPostInstallProblems();
var
  Msg: String;
begin
  Msg := '';
  if RegistrationError <> '' then
    Msg := 'The files were installed but service registration failed:' + #13#10 + RegistrationError + #13#10#13#10 +
           'Fix the problem, then run as Administrator:' + #13#10 +
           ExpandConstant('"{app}\server\TheWatcherServer.exe" --install-service --config "{commonappdata}\TheWatcher\TheWatcherServer.conf"') + #13#10 +
           ExpandConstant('"{app}\agent\TheWatcherAgent.exe" --install-service --config "{commonappdata}\TheWatcher\TheWatcherAgent.conf"');
  if StartError <> '' then
  begin
    if Msg <> '' then
      Msg := Msg + #13#10#13#10;
    Msg := Msg + 'These services were running before the upgrade but did not start again: ' + StartError + '.' +
           #13#10 + 'Check the service log and the Windows event log, then start them with: sc.exe start <name>';
  end;
  if Msg <> '' then
  begin
    PostInstallFailureText := Msg;
    SuppressibleMsgBox(Msg, mbError, MB_OK, IDOK);
  end;
end;

// Registration needs the installed executables, so it runs after the files are
// copied (ssPostInstall) and cannot abort the install. The services that were
// running before the upgrade are then started on their new registrations; a
// fresh install starts nothing, as before. Failures are recorded, reported
// and returned as the setup exit code.
procedure RegisterAll();
var
  ServerRegistered, AgentRegistered: Boolean;
begin
  ServerRegistered := False;
  AgentRegistered := False;
  try
    RegisterService('TheWatcherServer', ExpandConstant('{app}\server\TheWatcherServer.exe'),
                    ExpandConstant('{commonappdata}\TheWatcher\TheWatcherServer.conf'));
    ServerRegistered := True;
  except
    RegistrationError := GetExceptionMessage();
  end;
  try
    RegisterService('TheWatcherAgent', ExpandConstant('{app}\agent\TheWatcherAgent.exe'),
                    ExpandConstant('{commonappdata}\TheWatcher\TheWatcherAgent.conf'));
    AgentRegistered := True;
  except
    if RegistrationError <> '' then
      RegistrationError := RegistrationError + #13#10;
    RegistrationError := RegistrationError + GetExceptionMessage();
  end;

  // The server first, so the agent finds it when it reconnects. A service whose
  // registration failed is not started: the message above covers it.
  if ServerWasRunning and ServerRegistered and not StartServiceAgain('TheWatcherServer') then
    AddStartError('TheWatcherServer');
  if AgentWasRunning and AgentRegistered and not StartServiceAgain('TheWatcherAgent') then
    AddStartError('TheWatcherAgent');

  ReportPostInstallProblems();
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    PostInstallReached := True;
    RegisterAll();
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and (PostInstallFailureText <> '') then
    WizardForm.FinishedLabel.Caption := PostInstallFailureText;
end;

// Called whenever Setup exits. If the install stopped after PrepareToInstall
// stopped the services but before the files were installed (a file copy
// error, or Cancel), start them again: their registrations were not touched
// and still point at the old configs. There is no wizard page left, so the
// outcome goes to the setup log (/LOG).
procedure DeinitializeSetup();
var
  Note: String;
begin
  if PostInstallReached then
    Exit;
  Note := RestartStoppedService('TheWatcherServer', ServerWasRunning) +
          RestartStoppedService('TheWatcherAgent', AgentWasRunning);
  if Note <> '' then
    Log('The install was aborted after the services were stopped.' + Note);
end;

// Non-zero when the files were installed but a service could not be
// registered or started again, so silent installs report it.
function GetCustomSetupExitCode(): Integer;
begin
  if (RegistrationError <> '') or (StartError <> '') then
    Result := 10
  else
    Result := 0;
end;
