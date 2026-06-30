; VRto3D Installer
; Inno Setup 6.4+ script. Build with: iscc installer\VRto3D-Installer.iss

#define AppName       "VRto3D"
#define AppPublisher  "oneup03"
#define AppURL        "https://github.com/oneup03/VRto3D"
#define VRto3DReleaseUrl    "https://github.com/oneup03/VRto3D/releases/latest/download/vrto3d.zip"
#define VRto3DPreReleaseUrl "https://github.com/oneup03/VRto3D/releases/download/latest/VRto3D.zip"
#define WWZipUrl      "https://github.com/user-attachments/files/29488165/WibbleWobbleBeta10.zip"
#define SteamVRAppId  "250820"

[Setup]
AppId={{A6F4D9E2-7C3B-4A2E-9F1B-1B3E2D7C8F40}
AppName={#AppName}
AppVersion=1.0
AppVerName={#AppName}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
DefaultDirName={autopf}\VRto3D
DisableWelcomePage=no
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
Uninstallable=no
PrivilegesRequired=admin
OutputDir=Output
OutputBaseFilename=VRto3D-Installer
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
SolidCompression=yes
Compression=lzma2
ArchiveExtraction=full
ShowLanguageDialog=no
CloseApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "install";              Description: "Install / update VRto3D driver";                                   GroupDescription: "Actions:";              Flags: checkedonce
Name: "install\release";      Description: "Latest official release";                                          Flags: exclusive
Name: "install\prerelease";   Description: "Latest pre-release";                                               Flags: exclusive unchecked
Name: "install\local";        Description: "Local VRto3D.zip (next to installer)";                             Flags: exclusive unchecked
Name: "cleanreshade";         Description: "Remove legacy ReShade from SteamVR\bin\win64";                    GroupDescription: "Cleanup (recommended):"
Name: "cleandrivers";         Description: "Remove third-party SteamVR drivers and reset steamvr.vrsettings"; GroupDescription: "Cleanup (recommended):"
Name: "wibblewobble";         Description: "Install WibbleWobble for Frame Sequential 3D";                    GroupDescription: "Optional:"; Flags: unchecked
Name: "addleiasrpath";        Description: "Fix LeiaSR library loading (requires restart)";                  GroupDescription: "Optional:"; Flags: unchecked
Name: "launchsteamvr";        Description: "Launch SteamVR when finished";                                    GroupDescription: "Finish:"

[Code]
var
  SteamPath:        String;
  SteamVRPath:      String;
  ReleaseTag:       String;
  ReleaseTitle:     String;
  PreReleaseTag:    String;
  PreReleaseTitle:  String;
  LocalZipNote:     String;
  LogPath:          String;

  SteamPage:     TInputDirWizardPage;
  WWPage:        TInputDirWizardPage;

{ ============================================================ }
{ Logging                                                       }
{ ============================================================ }

procedure LogLine(const S: String);
var
  Line: AnsiString;
begin
  if LogPath = '' then Exit;
  Line := AnsiString(GetDateTimeString('yyyy-mm-dd hh:nn:ss', '-', ':') + '  ' + S + #13#10);
  SaveStringToFile(LogPath, Line, FileExists(LogPath));
end;

{ ============================================================ }
{ Path / string helpers                                         }
{ ============================================================ }

function NormalizeSlashes(const S: String): String;
var
  I: Integer;
  R: String;
begin
  R := S;
  for I := 1 to Length(R) do
    if R[I] = '/' then
      R[I] := '\';
  Result := R;
end;

function PathCombine(const A, B: String): String;
begin
  if A = '' then
    Result := B
  else if A[Length(A)] = '\' then
    Result := A + B
  else
    Result := A + '\' + B;
end;

{ Pull the second quoted string out of a VDF "key" "value" line.
  Returns empty string if no such pair exists. }
function VdfLineValue(const Line: String): String;
var
  Q1, Q2, Q3, Q4: Integer;
  S: String;
begin
  Result := '';
  S := Line;
  Q1 := Pos('"', S);
  if Q1 = 0 then Exit;
  Q2 := Pos('"', Copy(S, Q1 + 1, MaxInt));
  if Q2 = 0 then Exit;
  Q2 := Q2 + Q1;
  Q3 := Pos('"', Copy(S, Q2 + 1, MaxInt));
  if Q3 = 0 then Exit;
  Q3 := Q3 + Q2;
  Q4 := Pos('"', Copy(S, Q3 + 1, MaxInt));
  if Q4 = 0 then Exit;
  Q4 := Q4 + Q3;
  Result := Copy(S, Q3 + 1, Q4 - Q3 - 1);
end;

function VdfLineKey(const Line: String): String;
var
  Q1, Q2: Integer;
  S: String;
begin
  Result := '';
  S := Line;
  Q1 := Pos('"', S);
  if Q1 = 0 then Exit;
  Q2 := Pos('"', Copy(S, Q1 + 1, MaxInt));
  if Q2 = 0 then Exit;
  Q2 := Q2 + Q1;
  Result := LowerCase(Copy(S, Q1 + 1, Q2 - Q1 - 1));
end;

function SplitLines(const Text: String): TArrayOfString;
var
  I, J, N: Integer;
begin
  N := 0;
  SetArrayLength(Result, 0);
  J := 1;
  for I := 1 to Length(Text) do
  begin
    if Text[I] = #10 then
    begin
      SetArrayLength(Result, N + 1);
      Result[N] := Copy(Text, J, I - J);
      if (Length(Result[N]) > 0) and (Result[N][Length(Result[N])] = #13) then
        Result[N] := Copy(Result[N], 1, Length(Result[N]) - 1);
      N := N + 1;
      J := I + 1;
    end;
  end;
  if J <= Length(Text) then
  begin
    SetArrayLength(Result, N + 1);
    Result[N] := Copy(Text, J, Length(Text) - J + 1);
  end;
end;

{ Parse libraryfolders.vdf and return the library root that contains app SteamVRAppId.
  Returns empty string if the app isn't listed in any library. }
function FindSteamVRLibrary(const VdfPath: String): String;
var
  Raw:       AnsiString;
  Lines:     TArrayOfString;
  I:         Integer;
  Trimmed:   String;
  CurPath:   String;
  Key:       String;
  InApps:    Boolean;
  AppsDepth: Integer;
begin
  Result := '';
  if not FileExists(VdfPath) then Exit;
  if not LoadStringFromFile(VdfPath, Raw) then Exit;
  Lines := SplitLines(String(Raw));

  CurPath := '';
  InApps := False;
  AppsDepth := 0;

  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    Trimmed := Trim(Lines[I]);
    if Trimmed = '' then Continue;

    Key := VdfLineKey(Trimmed);

    if (not InApps) and (Key = 'path') then
    begin
      CurPath := VdfLineValue(Trimmed);
      StringChangeEx(CurPath, '\\', '\', True);
      CurPath := NormalizeSlashes(CurPath);
      Continue;
    end;

    if (not InApps) and (Key = 'apps') then
    begin
      InApps := True;
      AppsDepth := 0;
      Continue;
    end;

    if InApps then
    begin
      if Trimmed = '{' then
      begin
        AppsDepth := AppsDepth + 1;
        Continue;
      end;
      if Trimmed = '}' then
      begin
        AppsDepth := AppsDepth - 1;
        if AppsDepth <= 0 then
          InApps := False;
        Continue;
      end;
      if Key = '{#SteamVRAppId}' then
      begin
        if CurPath <> '' then
        begin
          Result := CurPath;
          Exit;
        end;
      end;
    end;
  end;
end;

{ ============================================================ }
{ Steam path detection                                          }
{ ============================================================ }

function DetectSteamPath: String;
var
  S: String;
begin
  Result := '';
  if RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', S) then
    Result := NormalizeSlashes(S);
end;

function DetectSteamVRPath: String;
var
  Lib, Candidate: String;
begin
  Result := '';
  if SteamPath = '' then Exit;

  Lib := FindSteamVRLibrary(PathCombine(SteamPath, 'steamapps\libraryfolders.vdf'));
  if Lib <> '' then
  begin
    Candidate := PathCombine(Lib, 'steamapps\common\SteamVR');
    if DirExists(Candidate) then
    begin
      Result := Candidate;
      Exit;
    end;
  end;

  Candidate := PathCombine(SteamPath, 'steamapps\common\SteamVR');
  if DirExists(Candidate) then
    Result := Candidate;
end;

{ ============================================================ }
{ GitHub release lookup                                         }
{ ============================================================ }

function ExtractJsonString(const Body, Field: String): String;
var
  Key: String;
  P, Q1, Q2: Integer;
  Out_: String;
begin
  Result := '';
  Key := '"' + Field + '"';
  P := Pos(Key, Body);
  if P = 0 then Exit;
  P := P + Length(Key);
  while (P <= Length(Body)) and ((Body[P] = ' ') or (Body[P] = ':') or (Body[P] = #9)) do
    P := P + 1;
  if (P > Length(Body)) or (Body[P] <> '"') then Exit;
  Q1 := P + 1;
  Q2 := Q1;
  while Q2 <= Length(Body) do
  begin
    if Body[Q2] = '\' then
    begin
      Q2 := Q2 + 2;
      Continue;
    end;
    if Body[Q2] = '"' then Break;
    Q2 := Q2 + 1;
  end;
  Out_ := Copy(Body, Q1, Q2 - Q1);
  StringChangeEx(Out_, '\"', '"', True);
  StringChangeEx(Out_, '\\', '\', True);
  StringChangeEx(Out_, '\n', #10, True);
  Result := Out_;
end;

procedure FetchReleaseInfo(const Endpoint: String; out Tag, Title: String);
var
  WinHttp: Variant;
  Body: String;
  Status: Integer;
begin
  Tag := '';
  Title := '';
  LogLine('FetchReleaseInfo: GET ' + Endpoint);
  try
    WinHttp := CreateOleObject('WinHttp.WinHttpRequest.5.1');
    WinHttp.SetTimeouts(10000, 10000, 10000, 10000);
    WinHttp.Open('GET', 'https://api.github.com/repos/oneup03/VRto3D' + Endpoint, False);
    WinHttp.SetRequestHeader('User-Agent', 'VRto3D-Installer');
    WinHttp.SetRequestHeader('Accept', 'application/vnd.github+json');
    WinHttp.Send('');
    Status := WinHttp.Status;
    LogLine('FetchReleaseInfo: HTTP ' + IntToStr(Status));
    if Status = 200 then
    begin
      Body := WinHttp.ResponseText;
      Tag := ExtractJsonString(Body, 'tag_name');
      Title := ExtractJsonString(Body, 'name');
      LogLine('FetchReleaseInfo: tag="' + Tag + '" title="' + Title + '"');
    end;
  except
    LogLine('FetchReleaseInfo error: ' + GetExceptionMessage);
  end;
end;

procedure FetchLatestRelease;
begin
  FetchReleaseInfo('/releases/latest', ReleaseTag, ReleaseTitle);
  FetchReleaseInfo('/releases/tags/latest', PreReleaseTag, PreReleaseTitle);
end;

{ ============================================================ }
{ File / folder helpers                                         }
{ ============================================================ }

function DeleteFolderRecursive(const Path: String): Boolean;
begin
  Result := True;
  if not DirExists(Path) then Exit;
  Result := DelTree(Path, True, True, True);
  if Result then
    LogLine('Deleted folder: ' + Path)
  else
    LogLine('Failed to delete folder: ' + Path);
end;

function DeleteFileLogged(const Path: String): Boolean;
begin
  Result := True;
  if not FileExists(Path) then Exit;
  Result := DeleteFile(Path);
  if Result then
    LogLine('Deleted file: ' + Path)
  else
    LogLine('Failed to delete file: ' + Path);
end;

{ Copy Src directory tree to Dest (Dest's parent must exist). }
function CopyFolderRecursive(const Src, Dest: String): Boolean;
var
  ResultCode: Integer;
  Cmd: String;
begin
  ForceDirectories(ExtractFilePath(RemoveBackslashUnlessRoot(Dest)));
  Cmd := '/C xcopy "' + Src + '" "' + Dest + '" /E /I /Y /Q';
  if not Exec(ExpandConstant('{cmd}'), Cmd, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    LogLine('Exec(xcopy) failed for ' + Src + ' -> ' + Dest);
    Result := False;
    Exit;
  end;
  Result := DirExists(Dest);
  if Result then
    LogLine('Copied: ' + Src + ' -> ' + Dest)
  else
    LogLine('xcopy returned ' + IntToStr(ResultCode) + ' but dest missing: ' + Dest);
end;

{ Conservative ReShade detection - look for the literal "ReShade" byte string anywhere
  in the file content. Reliable for ReShade DLLs (which embed it in their .rsrc data)
  and won't match Valve's own dxgi/d3d* DLLs. }
function IsReShadeDll(const Path: String): Boolean;
var
  Raw: AnsiString;
  I, N: Integer;
begin
  Result := False;
  if not FileExists(Path) then Exit;
  if not LoadStringFromFile(Path, Raw) then Exit;
  N := Length(Raw);
  if N < 7 then Exit;
  for I := 1 to N - 6 do
  begin
    if (Raw[I]     = 'R') and
       (Raw[I + 1] = 'e') and
       (Raw[I + 2] = 'S') and
       (Raw[I + 3] = 'h') and
       (Raw[I + 4] = 'a') and
       (Raw[I + 5] = 'd') and
       (Raw[I + 6] = 'e') then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

function InstallerDir: String;
begin
  Result := ExtractFilePath(ExpandConstant('{srcexe}'));
end;

{ Look for a local VRto3D zip next to the installer .exe (either case variant). }
function FindLocalVRto3DZip: String;
var
  Candidate: String;
begin
  Result := '';
  Candidate := PathCombine(InstallerDir, 'vrto3d.zip');
  if FileExists(Candidate) then
  begin
    Result := Candidate;
    Exit;
  end;
  Candidate := PathCombine(InstallerDir, 'VRto3D.zip');
  if FileExists(Candidate) then
    Result := Candidate;
end;

{ In-place text patch on a small ASCII file. }
procedure PatchTextFile(const Path, Find, Replace: String);
var
  Raw: AnsiString;
  S: String;
begin
  if not FileExists(Path) then
  begin
    LogLine('PatchTextFile: missing file ' + Path);
    Exit;
  end;
  if not LoadStringFromFile(Path, Raw) then
  begin
    LogLine('PatchTextFile: load failed for ' + Path);
    Exit;
  end;
  S := String(Raw);
  StringChangeEx(S, Find, Replace, True);
  if SaveStringToFile(Path, AnsiString(S), False) then
    LogLine('Patched ' + Path)
  else
    LogLine('PatchTextFile: save failed for ' + Path);
end;

{ ============================================================ }
{ Download / extract                                            }
{ ============================================================ }

function ResolveZip(const Url, FileName: String; out ZipPath: String): Boolean;
var
  TmpZip, LocalZip: String;
begin
  Result := False;
  TmpZip := ExpandConstant('{tmp}\') + FileName;

  try
    DownloadTemporaryFile(Url, FileName, '', nil);
    if FileExists(TmpZip) then
    begin
      ZipPath := TmpZip;
      LogLine('Downloaded ' + FileName);
      Result := True;
      Exit;
    end;
  except
    LogLine('Download failed for ' + FileName + ': ' + GetExceptionMessage);
  end;

  if LowerCase(FileName) = 'vrto3d.zip' then
    LocalZip := FindLocalVRto3DZip
  else
  begin
    LocalZip := PathCombine(InstallerDir, FileName);
    if not FileExists(LocalZip) then
      LocalZip := '';
  end;

  if LocalZip <> '' then
  begin
    ZipPath := LocalZip;
    LogLine('Using local zip: ' + LocalZip);
    Result := True;
    Exit;
  end;

  LogLine('No source available for ' + FileName + ' (online + offline both failed).');
end;

{ ============================================================ }
{ Tasks                                                         }
{ ============================================================ }

procedure TaskInstallVRto3D;
var
  ZipPath, ExtractDir, SrcDir, DestDir, ChannelUrl, Channel, LocalZip: String;
begin
  if SteamVRPath = '' then
  begin
    LogLine('TaskInstallVRto3D: SteamVR path not set, skipping.');
    Exit;
  end;

  if WizardIsTaskSelected('install\local') then
  begin
    Channel := 'local';
    LocalZip := FindLocalVRto3DZip;
    if LocalZip = '' then
    begin
      LogLine('TaskInstallVRto3D: local zip not found next to installer.');
      MsgBox(
        'No vrto3d.zip / VRto3D.zip found next to this installer.' #13#10 #13#10 +
        'Place the zip next to VRto3D-Installer.exe and re-run, or pick a different channel.',
        mbError, MB_OK);
      Exit;
    end;
    ZipPath := LocalZip;
    LogLine('TaskInstallVRto3D: channel=local using ' + ZipPath);
  end
  else
  begin
    if WizardIsTaskSelected('install\prerelease') then
    begin
      ChannelUrl := '{#VRto3DPreReleaseUrl}';
      Channel := 'pre-release';
    end
    else
    begin
      ChannelUrl := '{#VRto3DReleaseUrl}';
      Channel := 'release';
    end;
    LogLine('TaskInstallVRto3D: channel=' + Channel + ' url=' + ChannelUrl);

    if not ResolveZip(ChannelUrl, 'vrto3d.zip', ZipPath) then
    begin
      MsgBox(
        'Could not download vrto3d.zip and no local copy was found next to the installer.' #13#10 #13#10 +
        'Reconnect to the internet, or download vrto3d.zip from:' #13#10 +
        '  https://github.com/oneup03/VRto3D/releases/latest' #13#10 +
        'and place it next to this installer, then re-run.',
        mbError, MB_OK);
      Exit;
    end;
  end;

  ExtractDir := ExpandConstant('{tmp}\vrto3d_extract');
  ForceDirectories(ExtractDir);
  ExtractArchive(ZipPath, ExtractDir, '', True, nil);

  SrcDir := PathCombine(ExtractDir, 'drivers\vrto3d');
  if not DirExists(SrcDir) then
    SrcDir := PathCombine(ExtractDir, 'vrto3d');
  if not DirExists(SrcDir) then
  begin
    LogLine('TaskInstallVRto3D: vrto3d folder not found in zip extracted at ' + ExtractDir);
    MsgBox('The vrto3d.zip archive does not contain a vrto3d folder. Aborting install.', mbError, MB_OK);
    Exit;
  end;

  DestDir := PathCombine(SteamVRPath, 'drivers\vrto3d');
  if DirExists(DestDir) then
    DeleteFolderRecursive(DestDir);
  ForceDirectories(PathCombine(SteamVRPath, 'drivers'));
  CopyFolderRecursive(SrcDir, DestDir);
  LogLine('Installed VRto3D driver to ' + DestDir);
end;

procedure DeleteReShadePresetGlob(const Bin64: String);
var
  Rec: TFindRec;
  Found: Boolean;
begin
  Found := FindFirst(PathCombine(Bin64, 'ReShadePreset*.ini'), Rec);
  if not Found then Exit;
  try
    repeat
      DeleteFileLogged(PathCombine(Bin64, Rec.Name));
    until not FindNext(Rec);
  finally
    FindClose(Rec);
  end;
end;

procedure TaskCleanReshade;
var
  Bin64, P: String;
  I: Integer;
  Dlls: array[0..5] of String;
  Files: array[0..2] of String;
  Folders: array[0..2] of String;
begin
  if SteamVRPath = '' then
  begin
    LogLine('TaskCleanReshade: SteamVR path not set, skipping.');
    Exit;
  end;
  Bin64 := PathCombine(SteamVRPath, 'bin\win64');
  if not DirExists(Bin64) then
  begin
    LogLine('TaskCleanReshade: ' + Bin64 + ' does not exist, skipping.');
    Exit;
  end;

  Dlls[0] := 'dxgi.dll';
  Dlls[1] := 'd3d9.dll';
  Dlls[2] := 'd3d10.dll';
  Dlls[3] := 'd3d11.dll';
  Dlls[4] := 'd3d12.dll';
  Dlls[5] := 'opengl32.dll';
  for I := 0 to 5 do
  begin
    P := PathCombine(Bin64, Dlls[I]);
    if FileExists(P) and IsReShadeDll(P) then
      DeleteFileLogged(P);
  end;

  Files[0] := 'ReShade.ini';
  Files[1] := 'ReShade.log';
  Files[2] := '3DToElse.fx';
  for I := 0 to 2 do
    DeleteFileLogged(PathCombine(Bin64, Files[I]));

  DeleteReShadePresetGlob(Bin64);

  Folders[0] := 'reshade-shaders';
  Folders[1] := 'reshade-presets';
  Folders[2] := 'reshade-cache';
  for I := 0 to 2 do
    DeleteFolderRecursive(PathCombine(Bin64, Folders[I]));
end;

procedure TaskCleanDrivers;
var
  DriversRoot, VrSettings, ItemPath, NameLower: String;
  Rec: TFindRec;
  Allow: array[0..11] of String;
  Keep, Found: Boolean;
  I: Integer;
begin
  if SteamVRPath = '' then
  begin
    LogLine('TaskCleanDrivers: SteamVR path not set, skipping.');
    Exit;
  end;
  DriversRoot := PathCombine(SteamVRPath, 'drivers');
  if not DirExists(DriversRoot) then
  begin
    LogLine('TaskCleanDrivers: ' + DriversRoot + ' does not exist, skipping.');
    Exit;
  end;

  Allow[0]  := 'gamepad';
  Allow[1]  := 'htc';
  Allow[2]  := 'indexcontroller';
  Allow[3]  := 'indexhmd';
  Allow[4]  := 'lighthouse';
  Allow[5]  := 'null';
  Allow[6]  := 'oculus';
  Allow[7]  := 'oculus_legacy';
  Allow[8]  := 'prism';
  Allow[9]  := 'tundra_labs';
  Allow[10] := 'vrlink';
  Allow[11] := 'vrto3d';

  Found := FindFirst(PathCombine(DriversRoot, '*'), Rec);
  if Found then
  try
    repeat
      if (Rec.Name = '.') or (Rec.Name = '..') then Continue;
      if (Rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0 then Continue;
      NameLower := LowerCase(Rec.Name);
      Keep := False;
      for I := 0 to 11 do
        if NameLower = Allow[I] then
        begin
          Keep := True;
          Break;
        end;
      if not Keep then
      begin
        ItemPath := PathCombine(DriversRoot, Rec.Name);
        DeleteFolderRecursive(ItemPath);
      end;
    until not FindNext(Rec);
  finally
    FindClose(Rec);
  end;

  if SteamPath <> '' then
  begin
    VrSettings := PathCombine(SteamPath, 'config\steamvr.vrsettings');
    DeleteFileLogged(VrSettings);
  end;
end;

procedure TaskInstallWibbleWobble;
var
  ZipPath, ExtractDir, SrcDir, DestDir, RegBat, LegacyDriver: String;
  ResultCode: Integer;
begin
  if SteamVRPath <> '' then
  begin
    LegacyDriver := PathCombine(SteamVRPath, 'drivers\WibbleWobbleVR');
    if DirExists(LegacyDriver) then
    begin
      LogLine('Removing legacy WibbleWobbleVR SteamVR driver: ' + LegacyDriver);
      DeleteFolderRecursive(LegacyDriver);
    end;
  end;

  if not ResolveZip('{#WWZipUrl}', 'WibbleWobbleBeta7.2.zip', ZipPath) then
  begin
    MsgBox(
      'Could not download WibbleWobbleBeta7.2.zip and no local copy was found next to the installer.' #13#10 #13#10 +
      'Skipping WibbleWobble install. The legacy WibbleWobbleVR driver (if present) was still removed.',
      mbInformation, MB_OK);
    Exit;
  end;

  ExtractDir := ExpandConstant('{tmp}\ww_extract');
  ForceDirectories(ExtractDir);
  ExtractArchive(ZipPath, ExtractDir, '', True, nil);

  SrcDir := PathCombine(ExtractDir, 'WibbleWobble');
  if not DirExists(SrcDir) then
  begin
    LogLine('TaskInstallWibbleWobble: WibbleWobble folder not found inside zip at ' + ExtractDir);
    MsgBox('The WibbleWobble zip did not contain a WibbleWobble folder.', mbError, MB_OK);
    Exit;
  end;

  if WWPage = nil then
    DestDir := 'C:\Apps\WibbleWobble'
  else
    DestDir := RemoveBackslashUnlessRoot(Trim(WWPage.Values[0]));

  if DirExists(DestDir) then
  begin
    if MsgBox(
        'A WibbleWobble folder already exists at:' #13#10 + DestDir + #13#10 #13#10 +
        'Overwrite it?',
        mbConfirmation, MB_YESNO) <> IDYES then
    begin
      LogLine('User declined WibbleWobble overwrite at ' + DestDir);
      Exit;
    end;
    DeleteFolderRecursive(DestDir);
  end;

  ForceDirectories(DestDir);
  CopyFolderRecursive(SrcDir, DestDir);

  RegBat := PathCombine(DestDir, 'WibbleWobbleClient\Register.bat');
  if not FileExists(RegBat) then
  begin
    LogLine('TaskInstallWibbleWobble: Register.bat not found at ' + RegBat);
    MsgBox(
      'Register.bat not found at:' #13#10 + RegBat + #13#10 #13#10 +
      'WibbleWobble files were copied but registration was skipped.',
      mbError, MB_OK);
    Exit;
  end;

  { Strip the interactive prompts so Register.bat and its child PowerShell run unattended. }
  PatchTextFile(RegBat, #13#10 + 'pause', '');
  PatchTextFile(RegBat, '"%install_path%\"' + #13#10, '"%install_path%\" /f' + #13#10);
  PatchTextFile(PathCombine(DestDir, 'WibbleWobbleClient\SetRealtimePrivilege.ps1'),
                'Read-Host "Press Enter to exit"', '');

  LogLine('Running Register.bat (installer is already elevated): ' + RegBat);
  if not Exec(ExpandConstant('{cmd}'),
              '/C ""' + RegBat + '""',
              ExtractFilePath(RegBat),
              SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode) then
  begin
    LogLine('Exec failed for Register.bat');
    MsgBox(
      'Failed to launch Register.bat. You can run it manually as administrator from:' #13#10 +
      RegBat,
      mbError, MB_OK);
  end
  else
    LogLine('Register.bat exited with code ' + IntToStr(ResultCode));
end;

procedure TaskAddLeiaSRPath;
var
  LeiaPath, CurrentPath: String;
  ResultCode: Integer;
begin
  LeiaPath := 'C:\Program Files\LeiaSR\Platform\bin';
  CurrentPath := GetEnv('PATH');
  if Pos(LowerCase(LeiaPath), LowerCase(CurrentPath)) > 0 then
  begin
    LogLine('TaskAddLeiaSRPath: ' + LeiaPath + ' already on PATH, skipping.');
    Exit;
  end;
  LogLine('TaskAddLeiaSRPath: prepending ' + LeiaPath + ' to user PATH (reboot required)');
  if not Exec(ExpandConstant('{cmd}'),
              '/C setx PATH "' + LeiaPath + ';%PATH%"',
              '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    LogLine('setx exec failed')
  else
    LogLine('setx exited with code ' + IntToStr(ResultCode));
end;

procedure TaskLaunchSteamVR;
var
  ResultCode: Integer;
begin
  LogLine('Launching SteamVR via steam://run/{#SteamVRAppId}');
  ShellExec('open', 'steam://run/{#SteamVRAppId}', '', '', SW_SHOWNORMAL, ewNoWait, ResultCode);
end;

{ ============================================================ }
{ Wizard hooks                                                  }
{ ============================================================ }

function InitializeSetup: Boolean;
var
  Resp: Integer;
  ResultCode: Integer;
  LogDir: String;
begin
  Result := True;
  SteamPath := DetectSteamPath;

  if SteamPath <> '' then
  begin
    LogDir := PathCombine(SteamPath, 'logs');
    ForceDirectories(LogDir);
    LogPath := PathCombine(LogDir, 'VRto3D-Installer.log');
  end
  else
    LogPath := ExpandConstant('{tmp}\VRto3D-Installer.log');

  LogLine('--- VRto3D Installer started ---');
  LogLine('Steam path: ' + SteamPath);
  if SteamPath = '' then
  begin
    Resp := MsgBox(
      'Steam was not found on this machine.' #13#10 #13#10 +
      'Install Steam from https://store.steampowered.com/about/ , then re-run this installer.' #13#10 #13#10 +
      'Continue anyway and pick a SteamVR folder manually?',
      mbConfirmation, MB_YESNO);
    if Resp <> IDYES then
    begin
      Result := False;
      Exit;
    end;
  end;

  SteamVRPath := DetectSteamVRPath;
  if SteamVRPath = '' then
  begin
    Resp := MsgBox(
      'SteamVR was not detected in any Steam library folder.' #13#10 #13#10 +
      'Yes  - open Steam''s install page for SteamVR (re-run this installer afterwards).' #13#10 +
      'No   - continue and pick the SteamVR folder manually.' #13#10 +
      'Cancel - abort the installer.',
      mbConfirmation, MB_YESNOCANCEL);
    if Resp = IDYES then
    begin
      ShellExec('open', 'steam://install/{#SteamVRAppId}', '', '', SW_SHOWNORMAL, ewNoWait, ResultCode);
      Result := False;
      Exit;
    end
    else if Resp = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
  end;

  FetchLatestRelease;

  if FindLocalVRto3DZip <> '' then
    LocalZipNote := #13#10 + 'Local zip detected: ' + FindLocalVRto3DZip
  else
    LocalZipNote := '';
end;

procedure InitializeWizard;
var
  WelcomeText: String;
begin
  if ReleaseTag <> '' then
    WelcomeText := 'Latest release: ' + ReleaseTag
  else
    WelcomeText := 'Latest release: unknown';
  if PreReleaseTitle <> '' then
    WelcomeText := WelcomeText + #13#10 + 'Latest pre-release: ' + PreReleaseTitle
  else if PreReleaseTag <> '' then
    WelcomeText := WelcomeText + #13#10 + 'Latest pre-release: ' + PreReleaseTag
  else
    WelcomeText := WelcomeText + #13#10 + 'Latest pre-release: unknown';
  WelcomeText := WelcomeText + LocalZipNote;

  WizardForm.WelcomeLabel2.Caption :=
    WizardForm.WelcomeLabel2.Caption + #13#10 + #13#10 + WelcomeText;

  SteamPage := CreateInputDirPage(wpWelcome,
    'SteamVR Location',
    'Confirm where SteamVR is installed.',
    'VRto3D will be installed into the drivers subfolder of this path.',
    False, '');
  SteamPage.Add('SteamVR root folder:');
  if SteamVRPath <> '' then
    SteamPage.Values[0] := SteamVRPath
  else if SteamPath <> '' then
    SteamPage.Values[0] := PathCombine(SteamPath, 'steamapps\common\SteamVR')
  else
    SteamPage.Values[0] := 'C:\Program Files (x86)\Steam\steamapps\common\SteamVR';

  WWPage := CreateInputDirPage(wpSelectTasks,
    'WibbleWobble Location',
    'Choose where WibbleWobble will be installed.',
    'WibbleWobble files will be extracted directly into this folder.',
    False, '');
  WWPage.Add('WibbleWobble install folder:');
  WWPage.Values[0] := 'C:\Apps\WibbleWobble';
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if (WWPage <> nil) and (PageID = WWPage.ID) then
    Result := not WizardIsTaskSelected('wibblewobble');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (SteamPage <> nil) and (CurPageID = SteamPage.ID) then
  begin
    SteamVRPath := RemoveBackslashUnlessRoot(Trim(SteamPage.Values[0]));
    if SteamVRPath = '' then
    begin
      MsgBox('Please specify the SteamVR folder.', mbError, MB_OK);
      Result := False;
      Exit;
    end;
    if not DirExists(SteamVRPath) then
    begin
      if MsgBox('The folder' #13#10 + SteamVRPath + #13#10 + 'does not exist. Continue anyway?',
                mbConfirmation, MB_YESNO) <> IDYES then
      begin
        Result := False;
        Exit;
      end;
    end;
    LogLine('SteamVR root selected: ' + SteamVRPath);
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo,
  MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  S: String;
begin
  S := 'SteamVR root:' + NewLine + Space + SteamVRPath + NewLine + NewLine;
  if WizardIsTaskSelected('install') then
  begin
    S := S + 'Install / update VRto3D driver:' + NewLine +
         Space + SteamVRPath + '\drivers\vrto3d' + NewLine;
    if WizardIsTaskSelected('install\local') then
      S := S + Space + 'Channel: local zip next to installer' + NewLine
    else if WizardIsTaskSelected('install\prerelease') then
      S := S + Space + 'Channel: pre-release (' + PreReleaseTag + ')' + NewLine
    else
      S := S + Space + 'Channel: official release (' + ReleaseTag + ')' + NewLine;
    S := S + NewLine;
  end;
  if WizardIsTaskSelected('wibblewobble') and (WWPage <> nil) then
    S := S + 'Install WibbleWobble:' + NewLine +
         Space + WWPage.Values[0] + NewLine +
         Space + '(legacy ' + SteamVRPath + '\drivers\WibbleWobbleVR will be removed if present)' + NewLine + NewLine;
  if WizardIsTaskSelected('cleanreshade') then
    S := S + 'Remove ReShade from:' + NewLine +
         Space + SteamVRPath + '\bin\win64' + NewLine +
         Space + '(only files identified as ReShade will be deleted)' + NewLine + NewLine;
  if WizardIsTaskSelected('cleandrivers') then
  begin
    S := S + 'Remove third-party SteamVR drivers under:' + NewLine +
         Space + SteamVRPath + '\drivers' + NewLine +
         Space + '(default Valve drivers and vrto3d are kept)' + NewLine;
    if SteamPath <> '' then
      S := S + Space + 'Also delete: ' + SteamPath + '\config\steamvr.vrsettings' + NewLine;
    S := S + NewLine;
  end;
  if WizardIsTaskSelected('addleiasrpath') then
    S := S + 'Fix LeiaSR library loading (requires restart):' + NewLine +
         Space + 'Add C:\Program Files\LeiaSR\Platform\bin to user PATH' + NewLine + NewLine;
  if WizardIsTaskSelected('launchsteamvr') then
    S := S + 'Launch SteamVR after install completes' + NewLine;
  Result := S;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep <> ssInstall then Exit;

  WizardForm.StatusLabel.Caption := 'Running selected tasks...';

  if WizardIsTaskSelected('install') then
  begin
    WizardForm.StatusLabel.Caption := 'Installing VRto3D driver...';
    try
      TaskInstallVRto3D;
    except
      LogLine('install task error: ' + GetExceptionMessage);
    end;
  end;

  if WizardIsTaskSelected('wibblewobble') then
  begin
    WizardForm.StatusLabel.Caption := 'Installing WibbleWobble...';
    try
      TaskInstallWibbleWobble;
    except
      LogLine('wibblewobble task error: ' + GetExceptionMessage);
    end;
  end;

  if WizardIsTaskSelected('cleanreshade') then
  begin
    WizardForm.StatusLabel.Caption := 'Cleaning up legacy ReShade...';
    try
      TaskCleanReshade;
    except
      LogLine('cleanreshade task error: ' + GetExceptionMessage);
    end;
  end;

  if WizardIsTaskSelected('cleandrivers') then
  begin
    WizardForm.StatusLabel.Caption := 'Removing third-party SteamVR drivers...';
    try
      TaskCleanDrivers;
    except
      LogLine('cleandrivers task error: ' + GetExceptionMessage);
    end;
  end;

  if WizardIsTaskSelected('addleiasrpath') then
  begin
    WizardForm.StatusLabel.Caption := 'Adding LeiaSR Platform\bin to PATH...';
    try
      TaskAddLeiaSRPath;
    except
      LogLine('addleiasrpath task error: ' + GetExceptionMessage);
    end;
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then
  begin
    if WizardIsTaskSelected('launchsteamvr') then
      TaskLaunchSteamVR;
  end;
end;

procedure DeinitializeSetup;
begin
  LogLine('--- VRto3D Installer finished ---');
end;
