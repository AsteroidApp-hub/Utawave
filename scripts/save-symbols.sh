#!/usr/bin/env bash
# リリース後に Windows の PDB (クラッシュレポートの RVA→関数/行 解決用シンボル) を
# CI の artifact から取得し、リポジトリ直下の Symbols/ (gitignore 済・Dropbox バックアップ) へ
# 永久保管する。CI artifact は 90 日で消えるため、リリースのたびに 1 回実行すること。
#
# 使い方:
#   ./scripts/save-symbols.sh            # CMakeLists の VERSION を使う
#   ./scripts/save-symbols.sh 0.3.0      # バージョン明示
#
# 前提: gh CLI が AsteroidApp-hub にログイン済み。release-build.yml が version=<VER> で
#       成功し、artifact `Utawave-<VER>-Windows-x64-pdb` が存在すること。
set -euo pipefail

REPO="AsteroidApp-hub/Utawave"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

VER="${1:-}"
if [ -z "$VER" ]; then
  VER=$(grep -m1 -E 'project\(Utawave VERSION' "$ROOT/CMakeLists.txt" | sed -E 's/.*VERSION ([0-9.]+).*/\1/')
fi
[ -n "$VER" ] || { echo "エラー: バージョンを判定できません。引数で指定してください (例: 0.3.0)" >&2; exit 1; }

ART="Utawave-${VER}-Windows-x64-pdb"
DST="$ROOT/Symbols/Utawave-${VER}-Windows-x64"

echo "バージョン $VER の Windows シンボルを取得します (artifact: $ART)"

# その artifact を持つ最新 (未期限切れ) の run を探す
RID=$(gh api "repos/$REPO/actions/artifacts?per_page=100" \
  --jq "[.artifacts[] | select(.name==\"$ART\" and .expired==false)] | sort_by(.created_at) | last | .workflow_run.id // empty")

if [ -z "$RID" ]; then
  echo "エラー: artifact '$ART' が見つかりません (未ビルド、または 90 日で期限切れ)。" >&2
  echo "  → Actions タブで release-build.yml を version=$VER で実行してから再実行してください。" >&2
  exit 1
fi

echo "run $RID から DL 中..."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
gh run download "$RID" -R "$REPO" -n "$ART" -D "$tmp"

mkdir -p "$DST"
cp "$tmp"/Utawave.pdb "$tmp"/Utawave.exe "$DST"/

echo "保管しました: $DST"
ls -lh "$DST"
echo "※ この PDB はそのバージョンの配布 exe とだけ一致します。リリースごとに保管してください。"
