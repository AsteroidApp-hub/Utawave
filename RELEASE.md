# リリース手順 / Release Guide

配布物（macOS / Windows のビルド済みアプリ）は **公式サイト (utawave.com)** で公開します。
署名 (macOS: Developer ID + 公証 = **設定済み・有効** / Windows: SignPath = **未設定 = 現状未署名**) を
付けるため、**配布ビルドは GitHub Actions (手動発火) で行います**。zip は **GitHub Releases（アプリ
repo `AsteroidApp-hub/Utawave`）へ
アップロード**し、**公式サイト (utawave.com) の `download.html` から、その Release アセットへ直リンク**する。
サイトリポジトリ (Utawave-Site) には**バイナリを置かない**（zip は GitHub が配信。容量上限や repo 履歴の
肥大を気にせず済む）。

> repo が public である限り誰でも匿名でダウンロードできる（**private に戻すとリンクが切れる**点に注意）。
> ランディングは自ドメイン (`download.html`)・ファイル実体だけ GitHub という形なので、被リンクは自ドメインに
> 溜まり SEO 上の不利も無い。

> **署名のセットアップ**は `Docs/MACOS_SIGNING_SETUP.md` (Apple) と `Docs/WINDOWS_SIGNING_SETUP.md`
> (SignPath) を参照。**macOS は署名 + 公証の Secrets を登録済みで、CI が自動で署名 + 公証 + ステープル
> します**（成果物はそのままダブルクリックで起動可能）。**Windows は SignPath を未設定のため現状は
> 未署名 exe** を zip 化します（ASIO 対応は有効）。SignPath を設定すれば次回実行から署名されます。
> なお Secrets 未登録の環境 (fork 等) では macOS も自動で ad-hoc (未署名) にフォールバックします。
>
> **ASIO 対応の Windows 版も CI で作れます**: 再配布制限のある ASIO SDK は public repo に置けないため、
> **非公開 repo `AsteroidApp-hub/utawave-asiosdk`** に置き、CI が read-only deploy key (secret
> `ASIOSDK_DEPLOY_KEY`) で取得してビルドします。deploy key 未登録の環境 (fork 等) では取得を skip し、
> WASAPI/DirectSound のみでビルドします (= fork 安全)。ローカル手動ビルドの手順は末尾「補足」にも残します。

zip のファイル名にはバージョン + アーキテクチャを含めます（例: `Utawave-0.1.0-macOS-arm64.zip` /
`Utawave-0.1.0-Windows-x64.zip`）。

## 0. リリースビルドに焼き込むフラグ（重要）

公式配布ビルドは、**コンパイル時マクロ**でサーバー連携機能（クラッシュ送信・更新通知・広告）を
有効化する。**configure 時に以下の `-D` を渡したビルドだけ**がこれらの機能を持つ。
**CI / ローカルどちらでも同じ**で、渡さない＝公開ソース既定＝各機能オフになる。

| フラグ | 用途 | 値 / 状態 |
|---|---|---|
| `-DUTAWAVE_CRASH_REPORT_URL=` | クラッシュレポート送信先 | **稼働中** → `https://crash.utawave.com/report` |
| `-DUTAWAVE_VERSION_URL=` | アップデート通知の version JSON | `https://utawave.com/version.json`（utawave.com 稼働後に有効） |
| `-DUTAWAVE_DOWNLOAD_PAGE_URL=` | 更新通知のダウンロードページ | `https://utawave.com/download.html`（同上） |
| `-DUTAWAVE_ADS_ENABLED=ON -DUTAWAVE_AD_FEED_URL=` | 起動画面の広告枠 | 現状 OFF（使うときだけ設定） |

> **CACHE 変数なので注意**: 一度フラグ無しで configure 済みのビルドディレクトリは値が空でキャッシュ
> されており、**ただ再ビルドしても焼き込まれない**。**`-D` 付きの configure をもう一度実行**して
> キャッシュを上書きすること（ディレクトリの作り直しは不要・一度入れれば以降の `--build` に残る）。

> **CI（GitHub Actions）はリポジトリ変数から読む。** `release-build.yml` の cmake configure は
> `-DUTAWAVE_CRASH_REPORT_URL="${{ vars.UTAWAVE_CRASH_REPORT_URL }}"` を渡す。**この repo の
> Settings → Secrets and variables → Actions → Variables に `UTAWAVE_CRASH_REPORT_URL` =
> `https://crash.utawave.com/report` を登録**しておくこと（未登録だと空＝送信オフのまま出荷される）。
> `UTAWAVE_VERSION_URL` / `UTAWAVE_DOWNLOAD_PAGE_URL` / 広告系も有効化時は同様に変数化する。
>
> **fork 安全性**: リポジトリ変数は **fork に引き継がれない**ので、第三者が public ソースを
> そのままビルドしても／fork で CI を回しても、クラッシュ URL は空＝**送信機能ごとオフ**になり、
> 誤って公式エンドポイントへ届くことはない。ローカルの公式ビルドだけ、上の `-D` で URL を明示する。

**焼き込めたかの確認**:

1. configure ログに `-- Utawave: crash report URL = ...`（および設定した他フラグ）が出れば成功。
2. アプリ側: ダミーの crash ログ（`Docs/CRASH_REPORT_SETUP.md` の動作確認）を置いて起動し、
   同意ダイアログに**「送信する」が出ればオン**（出ない＝ローカル表示のみ＝オフ）。

クラッシュレポートの受信基盤（Cloudflare Worker + D1）の詳細・調査クエリは、Worker プロジェクト
`~/Dropbox/アプリ開発/utawave-crash-worker/SETUP.md` を参照。

## 1. バージョン更新

`CMakeLists.txt` の `VERSION`（必要なら About 等）を更新してコミット・push する。

## 2. ワークフローを手動発火してビルド + 署名

1. GitHub の **Actions タブ → "Release Build (macOS + Windows)" → Run workflow**。
2. **version** に今回のバージョン（例 `0.1.0`）を入力して実行。
3. macOS は自動で署名 + 公証 + ステープルされる（追加操作なし）。
   **Windows は現状 SignPath 未設定のため未署名のまま完了する**（SignPath を設定済みの場合のみ、
   SignPath.io で署名要求を**承認**する）。
4. 完了後、ワークフローの成果物 (Artifacts) から以下の 2 つの zip をダウンロード:
   - `Utawave-<version>-macOS-arm64`（**署名 + 公証済み** / Secrets 未登録の fork 等では ad-hoc）
   - `Utawave-<version>-Windows-x64`（**現状は未署名** exe を zip 化 / SignPath 設定後は署名済み。ASIO 対応）

> 両 zip とも利用者向けドキュメント（LICENSE.txt / THIRD_PARTY_LICENSES.txt / MANUAL.html / README.txt）を同梱済み。
> **同梱物は利用者がそのまま開けるよう .txt / .html のみ**（`.md` は配布しない。`README.txt` は `Docs/README.txt`、`LICENSE.txt` は `LICENSE` をリネームしたもの）。
> ローカルでの再パッケージは不要。

> **Windows の `Utawave.pdb` を必ず保管すること**: Release ビルドでは exe の隣に PDB が生成される。
> 配布 zip には入れず、**リリースごとに exe + PDB のペアを手元に保管**する。クラッシュレポートの
> スタックトレース（`Utawave.exe + 0x<RVA>` 形式）を関数・行番号へ解決するのに必要
> （手順は `Docs/CRASH_REPORT_SETUP.md`）。RVA はビルドごとに変わるため、該当バージョンの PDB が無いと解決できない。

## 3. ユニットテスト（任意・推奨）

ワークフローとは別に、ローカルで回す場合:

```sh
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac --target UtawaveTests --config Release
# 生成された UtawaveTests を実行（全合格で終了コード 0）
```

## 4. GitHub Release を作成（配布物の本体 + タグ）

GitHub Releases が**配布物の実体の置き場**で、公式サイトの `download.html` はここへ直リンクする。
タグを打つことで AGPL の「対応ソースコード」も同じタグに紐づく。

```sh
gh release create v0.1.0 Utawave-0.1.0-macOS-arm64.zip Utawave-0.1.0-Windows-x64.zip \
  --title "Utawave v0.1.0" --notes "更新内容の要約"
```

タグ `v0.1.0` が未作成なら `gh` が作成して push する。Web から行う場合は
**「Releases」→「Draft a new release」**でタグを新規作成し、zip をドラッグ&ドロップして
**Publish release**。

## 5. 公式サイトのリンクを更新（download.html を Release へ向ける）

サイトに**バイナリは置かない**。手順 4 の Release アセット URL を `download.html` に差し込むだけ。
CI やトークンは不要で、push すると Cloudflare Pages が自動デプロイする。サイト repo は
`~/Dropbox/アプリ開発/Utawave-Site`。

1. **`download.html` のダウンロードリンクを Release アセット URL に差し替える**（表示バージョンも更新）。
   URL の形式（タグ `v<version>` とファイル名は手順 2/4 のもの）:

   ```
   https://github.com/AsteroidApp-hub/Utawave/releases/download/v0.1.0/Utawave-0.1.0-macOS-arm64.zip
   https://github.com/AsteroidApp-hub/Utawave/releases/download/v0.1.0/Utawave-0.1.0-Windows-x64.zip
   ```

2. **メタ情報を更新**:
   - `version.json`（**リポジトリ直下**）の `"version"` を今回の版に更新
     → 旧バージョンのアプリの起動画面に「アップデートがあります」が出るようになる（`UpdateChecker`）
   - `changelog.html` に新しいリリースブロックを追加

3. **コミット & push**:

   ```sh
   cd ~/Dropbox/アプリ開発/Utawave-Site
   git add version.json download.html changelog.html
   git commit -m "release: Utawave v0.1.0"
   git push
   ```

4. **反映確認**: https://utawave.com/download.html から macOS / Windows 両方ダウンロードできること、
   旧バージョンのアプリの起動画面に「アップデートがあります」が表示されること

> **毎回 download.html を編集したくない場合**: CI のアセット名から**バージョンを外し**（例
> `Utawave-macOS-arm64.zip`）、`https://github.com/AsteroidApp-hub/Utawave/releases/latest/download/Utawave-macOS-arm64.zip`
> を使うと**常に最新 Release へリダイレクト**されるので、`download.html` のリンクは固定にできる
> （リリース時に触るのは `version.json` / `changelog.html` だけになる。要 workflow のアセット名変更）。

> 本リリースは AGPL-3.0-or-later で配布されます。対応するソースコードは同じタグのリポジトリです
> （同梱ライブラリのライセンスは THIRD_PARTY_LICENSES.txt を参照）。
> macOS 版は署名 + 公証済みなので、ダウンロード後そのままダブルクリックで起動できます。
> Windows 版は現状未署名のため、初回のみ SmartScreen で「詳細情報 → 実行」が必要です。

---

## 補足: ローカルで ASIO 対応 Windows 版を作る（フォールバック・通常は不要）

**通常リリースは CI が ASIO 対応 Windows 版まで作る**（上記「2.」。ASIO SDK は非公開 repo
`AsteroidApp-hub/utawave-asiosdk` から deploy key で取得し、CMake が `JUCE_ASIO=1` を自動有効化）。
以下は CI が使えない時や手元検証用の**フォールバック**。先に ASIO SDK を
`Source/ThirdParty/asiosdk/` に配置すると CMake が自動検出して `JUCE_ASIO=1` を有効化する
（この成果物は**未署名**になる）。

**公式リリースとして配布するなら、「0. リリースビルドに焼き込むフラグ」も configure に付ける**
（付けないとクラッシュ送信などがオフのまま）。クラッシュ URL は今すぐ有効:

```powershell
cmake -S . -B build-win -DCMAKE_BUILD_TYPE=Release `
  -DUTAWAVE_CRASH_REPORT_URL="https://crash.utawave.com/report"
cmake --build build-win --config Release

$exe = Get-ChildItem -Path build-win -Recurse -Filter Utawave.exe | Select-Object -First 1
New-Item -ItemType Directory -Force -Path Utawave-Windows | Out-Null
Copy-Item $exe.FullName Utawave-Windows/
# 同梱物は利用者がそのまま開ける .txt / .html のみ (LICENSE は LICENSE.txt にリネーム)
Copy-Item THIRD_PARTY_LICENSES.txt,Docs/MANUAL.html,Docs/README.txt Utawave-Windows/
Copy-Item LICENSE Utawave-Windows/LICENSE.txt
Compress-Archive -Path Utawave-Windows -DestinationPath Utawave-0.1.0-win64-asio.zip -Force

# クラッシュレポート解決用に exe + PDB のペアを保管する (zip には入れない)
$pdb = Get-ChildItem -Path build-win -Recurse -Filter Utawave.pdb | Select-Object -First 1
New-Item -ItemType Directory -Force -Path Symbols-0.1.0 | Out-Null
Copy-Item $exe.FullName,$pdb.FullName Symbols-0.1.0/
```

> CMake configure 時に `ASIO SDK found ... — ASIO サポート有効` と表示されれば ASIO 付きです。
