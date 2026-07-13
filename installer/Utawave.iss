; ============================================================================
;  Utawave Windows インストーラ (Inno Setup 6)
; ----------------------------------------------------------------------------
;  - ユーザー単位インストール (管理者権限・UAC 不要 / %LocalAppData%\Programs\Utawave)
;  - Utawave.exe (LAME は静的リンク済みなので単一 exe) とドキュメントを配置する
;  - デスクトップ / スタートメニューにショートカット
;  - .uta (プロジェクトファイル) を Utawave に関連付け (任意・既定 ON)
;  - アンインストーラ
;
;  コンパイル例 (CI / ローカル共通。リポジトリ直下から実行):
;    ISCC /DAppVersion=0.3.0 installer\Utawave.iss
;  exe/dll の場所を変える場合 (例: 署名済みフォルダ):
;    ISCC /DAppVersion=0.3.0 /DSrcDir=signed installer\Utawave.iss
;  出力先を変える:
;    ISCC /DAppVersion=0.3.0 /Oinstaller_out installer\Utawave.iss
; ============================================================================

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
; Utawave.exe の置き場所 (.iss からの相対 or 絶対)。既定は CI が staged する repo\artifact。
#ifndef SrcDir
  #define SrcDir "..\artifact"
#endif
; 同梱ドキュメント置き場
#ifndef DocDir
  #define DocDir "..\Docs"
#endif
; リポジトリ直下 (LICENSE / THIRD_PARTY_LICENSES.txt 用)
#ifndef RepoRoot
  #define RepoRoot ".."
#endif

#define AppName "Utawave"
#define AppPublisher "Utawave"
#define AppExe "Utawave.exe"
#define AppUrl "https://utawave.com/"

[Setup]
; AppId はアップグレード検出に使う固定 GUID (変更しないこと)
AppId={{8F2A7E14-3C9B-4D6E-A1F0-2B5C7D9E0A11}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; 管理者権限を要求しない (UAC を出さずに各ユーザーへインストール)
PrivilegesRequired=lowest
OutputBaseFilename=Utawave-{#AppVersion}-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
ChangesAssociations=yes

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "utaassoc"; Description: ".uta ファイルを Utawave で開く (関連付け)"; GroupDescription: "ファイルの関連付け:"

[Files]
Source: "{#SrcDir}\{#AppExe}";              DestDir: "{app}"; Flags: ignoreversion
; ドキュメント類は Docs\ サブフォルダへまとめる (多言語 help でファイル数が多く、直下が
; 散らかるため)。アプリ内「使い方ドキュメント」は exe 隣の Docs\help*.html を開く
; (旧配置 = exe 隣 も後方互換で探すが、新規配置は Docs\ に統一)
Source: "{#DocDir}\help*.html";             DestDir: "{app}\Docs"; Flags: ignoreversion
Source: "{#DocDir}\MANUAL.html";            DestDir: "{app}\Docs"; Flags: ignoreversion
Source: "{#DocDir}\README.txt";             DestDir: "{app}\Docs"; Flags: ignoreversion
Source: "{#RepoRoot}\THIRD_PARTY_LICENSES.txt"; DestDir: "{app}\Docs"; Flags: ignoreversion
Source: "{#RepoRoot}\LICENSE";              DestDir: "{app}\Docs"; DestName: "LICENSE.txt"; Flags: ignoreversion

[InstallDelete]
; 旧配置 ({app} 直下・Docs\ 移行前のバージョン) のドキュメントをアップグレード時に掃除する。
; 残すとアプリの後方互換探索が新しい Docs\ より先に古い直下を拾うことはない (Docs\ 優先) が、
; 古い版のドキュメントがゴミとして残り続けるため削除する
Type: files; Name: "{app}\help*.html"
Type: files; Name: "{app}\MANUAL.html"
Type: files; Name: "{app}\README.txt"
Type: files; Name: "{app}\THIRD_PARTY_LICENSES.txt"
Type: files; Name: "{app}\LICENSE.txt"

[Icons]
Name: "{group}\{#AppName}";                         Filename: "{app}\{#AppExe}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}";   Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";                   Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; .uta 関連付けはユーザー単位 (HKCU\Software\Classes)。アイコン/起動コマンドは exe を指す。
Root: HKCU; Subkey: "Software\Classes\.uta";                              ValueType: string; ValueName: ""; ValueData: "Utawave.Project"; Flags: uninsdeletevalue; Tasks: utaassoc
Root: HKCU; Subkey: "Software\Classes\Utawave.Project";                   ValueType: string; ValueName: ""; ValueData: "Utawave プロジェクト"; Flags: uninsdeletekey; Tasks: utaassoc
Root: HKCU; Subkey: "Software\Classes\Utawave.Project\DefaultIcon";       ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: utaassoc
Root: HKCU; Subkey: "Software\Classes\Utawave.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: utaassoc

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
