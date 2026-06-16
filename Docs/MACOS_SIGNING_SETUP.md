# macOS コード署名 + 公証 (Notarization) — CI セットアップ手順

macOS 版 `Utawave.app` に **Developer ID 署名 + Apple 公証 (notarization)** を付け、
Gatekeeper の「開発元を確認できないため開けません」を解消するための手順。
ビルド〜署名〜公証は **GitHub Actions (`.github/workflows/release-build.yml`)** の `macos` ジョブで行う。

> **現状: 設定済み・有効。** 必要なシークレット (証明書 / 公証情報) はこの repo に登録済みで、CI が
> 自動で署名 + 公証 + ステープルする (成果物はそのままダブルクリックで起動可能)。
> シークレット未登録の環境 (fork 等) では `HAS_SIGNING` ガードにより**自動で ad-hoc ビルド (未署名)**
> のまま zip 化される。以下はその初期セットアップ手順 (再設定 / 証明書更新時の参照用)。

---

## 前提：必要なもの

- **Apple Developer Program 登録** (年 $99)。これで以下が使える:
  - **Developer ID Application 証明書** (App Store 外で配布するアプリ用)
  - **公証 (notarytool)**
- App Store 配布ではないので **Developer ID Application** 証明書を使う (Mac App Distribution ではない)。

---

## 重要な entitlements (録音アプリ + プラグインホスト特有)

公証には **hardened runtime** が必須で、その状態では既定で色々ブロックされる。Utawave に必要なのは
`.github/macos/Utawave.entitlements` の 3 つ:

| entitlement | 理由 |
|---|---|
| `com.apple.security.device.audio-input` | **録音のマイクアクセス**。Info.plist の `NSMicrophoneUsageDescription` (JUCE が `MICROPHONE_PERMISSION_ENABLED` で付与) に**加えて**この entitlement が無いと hardened runtime 下で録音がブロックされる |
| `com.apple.security.cs.disable-library-validation` | **他社署名の VST3 / AU プラグインをロード可能にする**。無いと notarized 版で他社プラグインが一切ロードできない (プラグインホストに必須) |
| `com.apple.security.cs.allow-unsigned-executable-memory` | 一部プラグイン / JIT が未署名の実行可能メモリを使うため (DAW では一般的) |

> codesign は `--options runtime --entitlements .github/macos/Utawave.entitlements` で行う
> (ワークフローに記述済み)。entitlements を変更したらこのファイルを編集する。

---

## セットアップ手順

### 1. Developer ID Application 証明書を作って .p12 でエクスポート

1. (まだなら) Xcode または developer.apple.com で **Developer ID Application** 証明書を作成し、
   ローカルの **キーチェーンアクセス**に取り込む。
2. キーチェーンアクセスで証明書 (+ 秘密鍵) を選択 → 右クリック → **「書き出す」→ `.p12`**。
   書き出し時にパスワードを設定する (これが `MACOS_CERT_PASSWORD` になる)。
3. `.p12` を base64 にする:
   ```sh
   base64 -i Certificates.p12 | pbcopy   # クリップボードへ (これが MACOS_CERT_P12)
   ```

### 2. 公証用の App 用パスワードと Team ID を用意

- **App 用パスワード**: <https://appleid.apple.com> → サインインとセキュリティ → 「Appパスワード」で生成
  (これが `AC_APP_PASSWORD`)。通常の Apple ID パスワードではない。
- **Team ID**: developer.apple.com の Membership ページの 10 文字の ID (これが `AC_TEAM_ID`)。
- **署名 ID 文字列** (`MACOS_SIGN_IDENTITY`): 通常 `Developer ID Application: Your Name (TEAMID)`。
  ローカルで `security find-identity -v -p codesigning` で正確な文字列を確認できる。

### 3. GitHub リポジトリに Secrets を登録

`AsteroidApp-hub/Utawave` の **Settings → Secrets and variables → Actions → Secrets** に:

| 名前 | 値 |
|------|----|
| `MACOS_CERT_P12` | 手順 1 の base64 文字列 |
| `MACOS_CERT_PASSWORD` | .p12 書き出し時のパスワード |
| `MACOS_SIGN_IDENTITY` | `Developer ID Application: ... (TEAMID)` |
| `AC_APPLE_ID` | Apple ID のメールアドレス |
| `AC_APP_PASSWORD` | 手順 2 の App 用パスワード |
| `AC_TEAM_ID` | 10 文字の Team ID |

> `MACOS_CERT_P12` を登録した瞬間から、ワークフローの `HAS_SIGNING` が true になり署名 + 公証が有効化される。

### 4. 実行して確認

1. **Actions タブ → "Release Build (macOS + Windows)" → Run workflow** で version を入力して実行。
2. 完了後、`Utawave-<version>-macOS` 成果物 (zip) をダウンロード。
3. 署名 + 公証 + ステープルを確認:
   ```sh
   spctl -a -vvv -t install /path/to/Utawave.app      # → "accepted ... source=Notarized Developer ID"
   codesign --verify --strict --verbose=2 /path/to/Utawave.app
   xcrun stapler validate /path/to/Utawave.app
   ```

---

## RELEASE.md との接続

署名 + 公証は設定済み。**リリースは `RELEASE.md` のとおり CI (GitHub Actions `release-build.yml`) で
ビルド + 署名 + 公証し、`Utawave-<version>-macOS` 成果物 zip を GitHub Release に上げる**
(ローカル手動ビルドは検証 / フォールバック用)。公証済みなので「初回は右クリック→開く」の注記も不要。

---

## よくある詰まりどころ

- **公証は通るが起動後に録音できない** → `com.apple.security.device.audio-input` entitlement 漏れ。
- **公証は通るが他社プラグインが読めない** → `com.apple.security.cs.disable-library-validation` 漏れ。
- **`notarytool` が Invalid credentials** → 通常の Apple ID パスワードを使っている。**App 用パスワード**が必要。
- **codesign が "errSecInternalComponent"** → キーチェーンの `set-key-partition-list` 漏れ
  (ワークフローでは実行済み)。
- **証明書の有効期限切れ** → Developer ID 証明書は数年で失効する。失効すると新バージョンが署名できない
  (公証済みの既存配布物はそのまま有効)。

---

## 参考リンク

- Developer ID 配布: <https://developer.apple.com/developer-id/>
- notarytool: <https://developer.apple.com/documentation/security/customizing-the-notarization-workflow>
- hardened runtime entitlements: <https://developer.apple.com/documentation/security/hardened-runtime>
