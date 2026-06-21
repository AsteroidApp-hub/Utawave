# Windows コード署名 (SmartScreen 対策) — Certum Open Source 手順

Windows 版 `Utawave.exe` にコードサイニング署名を付け、SmartScreen の「WindowsによってPCが保護されました」
警告を低減するための手順。**Certum Open Source Code Signing 証明書 (OSS 開発者向け・安価・年 ~$108)** を使う。

> このファイルは配布側 (開発者) のセットアップ手順。アプリ本体のコードは一切変更しない
> (署名はビルド後のバイナリに対する外部処理)。

> **経緯**: 当初は SignPath Foundation (無料 OSS) を使う方針だったが OSS 審査に落選したため、
> 有料だが確実で日本からでも取得できる Certum へ切替えた。証明書は途中で切り替えると SmartScreen の
> 評価がリセットされるため、一度 Certum で署名を始めたら長く使い続ける ([[utawave-windows-signing-plan]])。

---

## なぜ署名するか / 何が解決して何が解決しないか

- **SmartScreen 警告**は「署名が無い」または「署名はあるが評価 (ダウンロード実績) が溜まっていない」
  EXE に出る。
- Certum Open Source が発行するのは **OV 相当**の証明書。署名しても**最初は警告が出続け**、
  ダウンロード実績が増えるにつれ消えていく (= 評価の蓄積が必要)。
- 「署名すれば即警告ゼロ」になるのは **EV 証明書だけ**で、これは個人 OSS 開発者には高額・入手困難。
- それでも**発行元が署名者名として表示される / 改ざん検知が効く / 評価が貯まれば警告が消える**
  という明確なメリットがある。

---

## SignPath との違い (設計上の前提)

- **出所検証 (Origin Verification) の縛りが無い**。SignPath Foundation は「指定リポジトリから
  GitHub Actions でビルドした artifact のみ署名可」という制約があったが、**Certum は任意のバイナリを
  署名できる**。よって **CI でビルドした未署名 exe を手元で署名する運用が可能**。
- **CI 完全自動署名は難しい**。Certum のクラウド署名 (SimplySign) はスマホアプリの **TOTP (ワンタイム
  パスワード) 認証**を伴うため、SignPath のような完全ヘッドレス自動署名が素直にできない。
  → **既定はローカル署名**にする (下記「手順」)。CI 自動署名は付録 (任意・上級) を参照。

> **macOS と統合済み**: リリースワークフロー `.github/workflows/release-build.yml` は Mac
> (Developer ID + 公証) と Windows (未署名 exe + PDB のビルド) を 1 つの手動発火にまとめてある。
> Windows ジョブは**未署名 exe と PDB を artifact 化するところまで**を担当し、署名はこの手順で
> ローカルに行う。Mac 側の手順は `Docs/MACOS_SIGNING_SETUP.md`。
> **ASIO 対応も CI で行う**: 再配布制限のある SDK は非公開 repo `AsteroidApp-hub/utawave-asiosdk` に置き、
> `windows` ジョブが read-only deploy key (secret `ASIOSDK_DEPLOY_KEY`) で取得してビルドする
> (詳細は `RELEASE.md`)。

---

## セットアップ手順

### 1. Certum Open Source 証明書を取得する

1. <https://shop.certum.eu/> (または日本代理店) で **Open Source Code Signing** を購入する。
   - **クラウド署名 (SimplySign) 版を選ぶ**: 物理 USB トークン/スマートカードの郵送を待たずに済み、
     ローカル PC からクラウド経由で署名できる。日本からでも取得可。
   - 物理カード版 (カード + リーダーのセット) もあるが、郵送と専用リーダーが要るのでクラウド版を推奨。
2. **本人確認 (個人開発者としての ID 確認)** を行う。書類審査に数日〜かかる前提で早めに着手する。
3. 発行されると **SimplySign クラウドのアカウント**と証明書が使えるようになる。スマホに
   **SimplySign モバイルアプリ**を入れ、QR でアクティベートする (TOTP 認証用)。

> **保管 (重要)**: アクティベーション時の QR / シークレットは、CI 自動署名 (付録) に使う可能性があるので
> 安全に保管する。証明書の thumbprint (拇印) も控えておく (signtool で指定する)。

### 2. ローカル署名ツールを用意する

Windows 機 (リリースを作る PC) に以下を入れる:

- **SimplySign Desktop** (Certum 配布) — クラウドの証明書を Windows の証明書ストアに「仮想スマートカード」
  として読み込ませるアプリ。署名時に SimplySign アプリの TOTP でログインする。
- **signtool.exe** — Windows SDK 同梱 (Visual Studio または Windows SDK を入れると入る)。
  - 代替として **jsign** (Java 製・クロスプラットフォームの Authenticode 署名ツール) でも署名できる。
    Certum を直接サポートする。Mac/Linux から署名したい場合はこちら。

### 3. リリースをビルドする (CI)

1. **Actions タブ → "Release Build (macOS + Windows)" → Run workflow** で version (例 `0.2.0`) を
   入力して実行する。
2. 完了後、Windows 関連の成果物をダウンロードする:
   - **`utawave-unsigned`** … 署名対象の `Utawave.exe` + `lame.dll` (MP3 エンコーダの動的リンク DLL)
   - **`Utawave-<version>-Windows-Setup`** … **未署名インストーラ** `Utawave-<version>-Setup.exe` (配布の主役)
   - **`Utawave-<version>-Windows-x64`** … 同梱ドキュメント + `lame.dll` 入りの zip (中の exe は未署名・ポータブル用途)
   - **`Utawave-<version>-Windows-x64-pdb`** … クラッシュ解決用 PDB (`Utawave.pdb` + `lame.pdb`・配布しない・手元に永久保管)

> **`lame.dll` について**: MP3 エンコーダ LAME を LGPL 準拠で動的リンクしているため、`Utawave.exe` は
> 同じフォルダの `lame.dll` を必要とする (無いと起動時に DLL not found で落ちる)。インストーラ / zip とも
> CI が自動同梱する。`lame.dll` の署名は必須ではないが、AV 誤検知を減らしたければ exe と同じ手順で署名してよい (任意)。

> **何を署名するか (優先順位)**: SmartScreen の評価は**起動される exe 単位**。利用者がまず実行するのは
> **インストーラ (`Utawave-<version>-Setup.exe`)** なので、**インストーラの署名を最優先**する。インストーラ内の
> `Utawave.exe` / `lame.dll` の署名は任意 (AV 誤検知低減目的)。zip 配布も残す場合は zip 内 `Utawave.exe` も署名する。

### 4. exe をローカルで署名する

SimplySign Desktop を起動し、スマホの SimplySign アプリの TOTP でログイン (= 証明書がストアに入る) してから、
ダウンロードした **`Utawave.exe` を署名**する。

**signtool の例** (証明書 thumbprint で指定。SHA-256 + RFC3161 タイムスタンプ):

```pwsh
signtool sign `
  /sha1 <証明書のthumbprint> `
  /fd SHA256 `
  /tr http://time.certum.pl `
  /td SHA256 `
  /d "Utawave" `
  Utawave.exe

# 検証
signtool verify /pa /v Utawave.exe
```

**jsign の例** (Mac/Linux/Win 共通。CRYPTOCERTUM ストアを使う):

```bash
jsign --storetype CRYPTOCERTUM \
      --tsaurl http://time.certum.pl \
      Utawave.exe
```

> **タイムスタンプは必須**。`/tr`(signtool) / `--tsaurl`(jsign) を必ず付ける。これが無いと証明書失効後に
> 署名が無効になる。

### 5. インストーラを署名する (配布の主役)

利用者がまず実行するのは **インストーラ (`Utawave-<version>-Setup.exe`)** なので、ここを署名するのが
最重要。CI が作る Setup.exe は未署名なので、**署名済みファイルでインストーラを作り直し → インストーラ自身を署名**する:

```pwsh
# 1. 署名済み exe / (任意で) lame.dll を 1 フォルダにまとめる
New-Item -ItemType Directory -Force -Path signed | Out-Null
Copy-Item .\Utawave.exe signed\Utawave.exe -Force
Copy-Item .\lame.dll    signed\lame.dll    -Force   # 署名していなければ未署名のままでも可

# 2. 署名済みファイルでインストーラを再コンパイル (リポジトリ直下から)
& "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" `
  /DAppVersion=0.3.0 /DSrcDir=signed /Oinstaller_out installer\Utawave.iss

# 3. 生成された Setup.exe を署名する (exe と同じ手順)
signtool sign /sha1 <thumbprint> /fd SHA256 /tr http://time.certum.pl /td SHA256 `
  /d "Utawave Setup" installer_out\Utawave-0.3.0-Setup.exe
signtool verify /pa /v installer_out\Utawave-0.3.0-Setup.exe
```

> `installer\Utawave.iss` の `SrcDir` は exe/dll の場所。CI 既定は `..\artifact` (未署名)、ローカル署名時は
> 上記のように `signed` を渡す。Inno Setup が手元に無ければ https://jrsoftware.org/isdl.php から導入する
> (CI と同じ Inno Setup 6)。

### 5.1 (任意) zip 版も署名して作り直す

ポータブル zip も併せて配布する場合は、CI が作った zip の中の exe を、署名済み exe で差し替える:

```pwsh
# 1. CI の Utawave-<ver>-Windows-x64.zip を展開
Expand-Archive Utawave-0.2.0-Windows-x64.zip -DestinationPath stage

# 2. 中の Utawave.exe を、署名済み exe で上書き
Copy-Item .\Utawave.exe .\stage\Utawave-Windows\Utawave.exe -Force
#    (任意) lame.dll も署名したならここで上書きする。未署名のままでも起動・配布は可
# Copy-Item .\lame.dll .\stage\Utawave-Windows\lame.dll -Force

# 3. zip を作り直す
Compress-Archive -Path stage\Utawave-Windows -DestinationPath Utawave-0.2.0-Windows-x64.zip -Force
```

> zip 自体には署名しない (Windows は zip の署名を見ない)。利用者が展開した `Utawave.exe` が署名済みであればよい。
> **`lame.dll` は zip に既に含まれている** (CI が同梱) ので、exe を差し替えるだけでよい。`lame.dll` を
> 削除しないこと (起動に必須)。

### 6. 配布する (RELEASE.md との接続)

差し替えた zip をそのまま GitHub Release / 公式サイト `dl/` に配置する (`RELEASE.md` の手順 5・6)。
PDB は配布せず手元に永久保管する (その版の RVA → 関数/行 解決に必要。署名は RVA を変えないので
未署名 exe と PDB のペアで解決できる。詳細は `Docs/CRASH_REPORT_SETUP.md`)。

---

## SmartScreen 評価について (重要)

- 署名直後の新バージョンは **しばらく警告が出続ける**。これは仕様 (OV 証明書は評価の蓄積が必要)。
- **同じ証明書で署名し続けると証明書全体に評価が蓄積**し、新バージョンでも警告が出にくくなる
  → バージョンごとに証明書を変えない・Certum で一貫して署名し続けることが重要。
- ダウンロード数が少ない初期は警告が出やすい。`Docs/MANUAL.html` / `download.html` に
  「警告が出たら『詳細情報』→『実行』」の案内を載せておくと親切。

---

## チェックリスト

- [ ] Certum Open Source 証明書 (SimplySign クラウド) を購入し本人確認が完了した
- [ ] SimplySign モバイルアプリをアクティベートした (TOTP)
- [ ] リリース PC に SimplySign Desktop + signtool (または jsign) を用意した
- [ ] CI を手動発火し `utawave-unsigned` (Utawave.exe + lame.dll) と Setup / zip / PDB を取得した
- [ ] signtool/jsign で exe を署名し `signtool verify /pa` が通った (タイムスタンプ付き)
- [ ] 署名済みファイルでインストーラを再コンパイル (`ISCC /DSrcDir=signed`) し、**Setup.exe を署名**した
- [ ] (zip も配布するなら) 署名済み exe で zip の中身を差し替えた
- [ ] 署名済み Setup.exe / exe の発行元が署名者名で表示されることを Windows で確認
      (右クリック → プロパティ → デジタル署名)

---

## 付録: CI 自動署名 (任意・上級)

毎回ローカルで署名するのが手間な場合、CI で自動署名することも一応可能だが、**SimplySign の TOTP 認証を
プログラムで突破する必要があり、ヘッドレス運用は壊れやすい**ため、まずはローカル署名で運用を固めてから
検討する。

- 仕組み: アクティベーション QR に含まれる `otpauth://` シークレット (Base32) を CI の Secret に入れ、
  `oathtool` 等で **TOTP を都度生成 → SimplySign セッションを確立 → jsign / signtool で署名**する。
- 実装する場合の配線案 (`.github/workflows/release-build.yml` の windows ジョブ):
  1. Secrets を登録: `CERTUM_OTP_SEED` (Base32 シークレット) ほか SimplySign のログイン情報。
  2. `Package zip` の直前に署名ステップを追加し、`signed/Utawave.exe` を生成する
     (既存の zip 化は `Test-Path signed/Utawave.exe` で署名済みを自動的に拾う設計のまま使える)。
  3. ステップは `env.CERTUM_OTP_SEED != ''` でガードし、未登録時は従来どおり未署名 zip にする。
- 参考:
  - jsign で Certum/SimplySign を自動化する手順 (TOTP 生成含む):
    <https://www.devas.life/how-to-automate-signing-your-windows-app-with-certum/>
  - jsign 公式 (Certum サポート): <https://ebourg.github.io/jsign/>

---

## 参考リンク

- Certum Open Source (クラウド署名): <https://certum.store/open-source-code-signing-on-simplysign.html>
- Certum: signtool / jarsigner での署名手順 (PDF):
  <https://www.files.certum.eu/documents/manual_en/Code-Signing-signing-the-code-using-tools-like-Singtool-and-Jarsigner_v2.3.pdf>
- jsign (クロスプラットフォーム Authenticode 署名): <https://ebourg.github.io/jsign/>
- Microsoft: コード署名の選択肢: <https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options>
- Microsoft: SmartScreen の評価: <https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation>
