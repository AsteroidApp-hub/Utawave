# Windows コード署名 (SmartScreen 対策) — SignPath Foundation 手順

Windows 版 `Utawave.exe` にコードサイニング署名を付け、SmartScreen の「WindowsによってPCが保護されました」
警告を低減するための手順。**OSS 向けに無料**で証明書と署名基盤を提供する **SignPath Foundation** を使う。

> このファイルは配布側 (開発者) のセットアップ手順。アプリ本体のコードは一切変更しない
> (署名はビルド後のバイナリに対する外部処理)。

---

## なぜ署名するか / 何が解決して何が解決しないか

- **SmartScreen 警告**は「署名が無い」または「署名はあるが評価 (ダウンロード実績) が溜まっていない」
  EXE に出る。
- SignPath Foundation が発行するのは **OV 相当**の証明書。署名しても**最初は警告が出続け**、
  ダウンロード実績が増えるにつれ消えていく (= 評価の蓄積が必要)。
- 「署名すれば即警告ゼロ」になるのは **EV 証明書だけ**で、これは無料では入手できない
  (個人での購入はハードウェアトークン必須・高額)。
- それでも**発行元が「Utawave」として表示される / 改ざん検知が効く / 評価が貯まれば警告が消える**
  という明確なメリットがあるので、無料でできる範囲としては SignPath Foundation が最適。

---

## 重要な前提：署名対象は GitHub Actions でビルドする必要がある

SignPath Foundation は **出所検証 (Origin Verification)** を技術的に強制する。これは
**「署名するバイナリが、指定リポジトリのソースから自動ビルドされたものである」ことを GitHub が保証する**
仕組みで、具体的には:

- 署名対象は **GitHub Actions のワークフロー成果物 (artifact)** であること
- 出所メタデータは**ビルドスクリプトではなく GitHub 自身が付与**するため偽装できない
- ローカル PC で手動ビルドした EXE を手で SignPath にアップロードする経路は **Foundation では使えない**

→ このため**署名対象の Windows ビルドは GitHub Actions で行う**。`workflow_dispatch` (手動発火) で
動くワークフローでも GitHub が出所メタデータを付与するので問題ない (push トリガーである必要はない)。

> **macOS と統合済み**: 署名は Mac (Developer ID + 公証) と Windows (SignPath) を 1 つの手動発火
> ワークフロー `.github/workflows/release-build.yml` にまとめてある (`windows` ジョブが SignPath 担当)。
> Mac 側の手順は `Docs/MACOS_SIGNING_SETUP.md` を参照。以下は Windows (SignPath) 固有の設定。
> **ASIO 対応も CI で行う**: 再配布制限のある SDK は非公開 repo `AsteroidApp-hub/utawave-asiosdk` に置き、
> `windows` ジョブが read-only deploy key (secret `ASIOSDK_DEPLOY_KEY`) で取得してビルドする
> (詳細は `RELEASE.md`)。よって ASIO 入り Windows 版もローカル手動ではなく CI で作れる
> (現状 SignPath 未設定なので署名は付かないが、ASIO は有効)。

---

## セットアップ手順

### 1. SignPath Foundation に申請する

**事前準備 (申請前にやること)**
- GitHub アカウントで **2FA (多要素認証) を有効化**しておく (Foundation の必須条件)。
- リポジトリが **public** で、ルートに `LICENSE` (AGPLv3 全文) があることを確認 (済)。

申請は <https://signpath.org/apply.html> のフォームから行う。**公開される文 (Tagline / Description /
Reputation) は英語で書く** (審査担当が読む & Foundation サイトに掲載されるため)。Utawave の回答案:

| フォーム項目 | 回答案 |
|------|--------|
| Project name | `Utawave` |
| Repository URL | `https://github.com/AsteroidApp-hub/Utawave` |
| Project website | `https://github.com/AsteroidApp-hub/Utawave` (公式サイト未公開のため。任意・空欄可・公開後に差し替え) |
| Privacy Policy URL | **空欄**。CI でビルドする署名版はクラッシュ送信URL等を埋め込まずユーザーデータを送信しないため不要 |
| Wikipedia URL | 空欄 (記事なし) |
| License | `AGPL-3.0-or-later` (OSI 承認・商用デュアルライセンス無し) |
| Tagline | `Free, open-source recording DAW for vocal cover artists.` |
| Description | `Utawave is a free and open-source recording-focused digital audio workstation for vocal cover artists, available on Windows and macOS under the AGPL-3.0-or-later license.` (※ 依存ライブラリ/バージョン固有機能は書かない指示があるので JUCE 等は書かない) |
| Reputation | 下記ブロック参照 |
| Maintainer Type | `Individual` |
| Build System | `GitHub Actions` |
| First / Last Name | 申請者本人の氏名 (SignPath アカウント名になる) |
| Email | 通知が届くメール |
| Company Name | 空欄 (個人) |
| Primary Discovery Channel | `Google search` (ネット検索で知ったため。exact source は空欄で可) |

**Reputation 欄 (英語・実績リンクを示す):**

```
Utawave is a newly released, actively maintained open-source project.
- Source, issues and release history (public): https://github.com/AsteroidApp-hub/Utawave
- Releases (Windows & macOS builds): https://github.com/AsteroidApp-hub/Utawave/releases
- Development updates and announcements: https://x.com/SAsteroid2021
It is a free, recording-focused DAW for the Japanese "vocal cover" (歌い手) community.
```

> **資格の根拠** (確認欄用): ① OSI 承認ライセンス (AGPLv3) で商用デュアルライセンスでない /
> ② マルウェア・PUP を含まない / ③ アクティブにメンテされている (GitHub + X で継続開発・告知) /
> ④ 既にリリース済み (GitHub Releases v0.1.0 で Windows / macOS 配布)。

承認されると SignPath.io 上に Foundation の組織 (Organization) とプロジェクトが用意され、招待が届く。
**SignPath 側でも 2FA を有効化**する。

### 2. SignPath 側でプロジェクト / 署名ポリシーを設定

SignPath.io の管理画面で以下を作成 (承認後に表示される値を控える):

- **Organization ID** — 組織の GUID
- **Project Slug** — 例 `utawave`
- **Signing Policy Slug** — リリース署名用。例 `release-signing`
- **Trusted Build System** に **GitHub** を登録し、上記リポジトリを紐付ける (出所検証の有効化)
- 署名は**リリースごとに人間の承認 (Approver) が必須**。Author / Reviewer / Approver のロールを割り当てる
  (個人運用なら自分が兼任で可)

API 連携用に **SignPath の API トークン**を 1 つ発行する (次の手順で GitHub Secret に入れる)。

### 3. GitHub リポジトリに Secrets / Variables を登録

`AsteroidApp-hub/Utawave` の **Settings → Secrets and variables → Actions** で:

| 種別 | 名前 | 値 |
|------|------|----|
| Secret | `SIGNPATH_API_TOKEN` | SignPath で発行した API トークン |
| Variable | `SIGNPATH_ORGANIZATION_ID` | 組織の GUID |
| Variable | `SIGNPATH_PROJECT_SLUG` | 例 `utawave` |
| Variable | `SIGNPATH_POLICY_SLUG` | 例 `release-signing` |

### 4. ワークフローについて

署名ワークフローは既に **`.github/workflows/release-build.yml`** にコミット済み (Mac と統合)。
`windows` ジョブが「ビルド → 未署名 exe を artifact アップロード → SignPath へ署名要求 →
署名済み exe を zip 化」を行う。手順 3 の Secrets / Variables を登録すれば動く。

> **`SIGNPATH_API_TOKEN` 未登録のうちは署名ステップを自動スキップ**し、未署名 exe をそのまま
> zip 化する (`HAS_SIGNING` ガード)。SignPath 設定前でもワークフローは緑になり、パイプライン全体を
> テストできる。トークンを登録すると次回実行から署名 + 出所検証が有効になる。

ワークフローを編集する際の要点:
- 実際の action 名 / 入力名は SignPath のドキュメント
  (<https://github.com/SignPath/github-action-submit-signing-request> /
  <https://docs.signpath.io/trusted-build-systems/github>) の最新版に合わせること
  (バージョンや入力キーが更新される場合がある)。
- **署名は承認待ちで止まる**: ワークフローが署名要求を送信したあと、SignPath.io 上で
  Approver が承認するまで `wait-for-completion: true` が待機する。承認するとワークフローが続行する。
- **SignPath の署名ポリシーの出所検証設定で `workflow_dispatch` トリガー / 対象ブランチを許可**して
  おくこと (手動発火を弾く設定だと署名要求が拒否される)。

### 5. リリースで使う (RELEASE.md との接続)

1. **Actions タブ → "Release Build (macOS + Windows)" → Run workflow** で version (例 `0.1.0`) を
   入力して実行。
2. SignPath.io で署名要求を**承認**する。
3. 完了後、ワークフローの **`Utawave-<version>-win64` 成果物 (署名済み exe を zip 化済み) をダウンロード**。
4. その zip をそのまま GitHub Release / 公式サイト `dl/` に配置する (`RELEASE.md` の手順 5・6)。

> exe の zip 化までワークフロー側で済むので、ローカルでの再パッケージは不要。

### 6. exe を zip に入れたまま署名できるか

SmartScreen の評価対象は **exe 単体**なので、上記のとおり **exe を署名 → zip に格納**する流れにする。
zip 自体には署名しない (Windows は zip の署名を見ない)。利用者が展開した `Utawave.exe` が署名済みであればよい。

---

## SmartScreen 評価について (重要)

- 署名直後の新バージョンは **しばらく警告が出続ける**。これは仕様 (OV 証明書は評価の蓄積が必要)。
- 同じ証明書で署名し続けると**証明書全体に評価が蓄積**し、新バージョンでも警告が出にくくなる
  → バージョンごとに証明書を変えない・SignPath で一貫して署名し続けることが重要。
- ダウンロード数が少ない初期は警告が出やすい。`Docs/MANUAL.html` / `download.html` に
  「警告が出たら『詳細情報』→『実行』」の案内を載せておくと親切 (macOS の右クリック→開く 案内と同様)。

---

## チェックリスト

- [ ] SignPath Foundation に OSS 申請し承認された
- [ ] チーム全員が SignPath / GitHub で MFA 有効
- [ ] SignPath で Project / Signing Policy / Trusted Build System (GitHub) を設定
- [ ] GitHub に `SIGNPATH_API_TOKEN` (Secret) と組織/プロジェクト/ポリシーの Variables を登録
- [ ] SignPath の署名ポリシーの出所検証で `workflow_dispatch` / 対象ブランチを許可した
- [ ] `.github/workflows/release-build.yml` を手動発火で一度通した (承認 → 署名済み zip 取得)
- [ ] 署名済み exe の発行元が「Utawave」と表示されることを Windows で確認
      (exe を右クリック → プロパティ → デジタル署名)

---

## 参考リンク

- SignPath Foundation: <https://signpath.org/>
- 申請条件 (OSS): <https://signpath.org/terms.html>
- GitHub 連携 (Trusted Build System): <https://docs.signpath.io/trusted-build-systems/github>
- 署名要求 Action: <https://github.com/SignPath/github-action-submit-signing-request>
- 出所検証 (Origin Verification): <https://docs.signpath.io/origin-verification>
- Microsoft: コード署名の選択肢: <https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options>
- Microsoft: SmartScreen の評価: <https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation>
</content>
</invoke>
