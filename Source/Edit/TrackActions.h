// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "../Tracks/TrackManager.h"

// トラック追加 / 複製の Undo アクション。
// PluginActions.h と同じくヘッダオンリー (UtawaveTests がリンク追加なしで検証できる)。
//
// 設計:
//  - 「追加済み」のトラックに対して作る (呼び出し側が先に追加し、最初の perform() は no-op)。
//  - undo は Track を破棄せずアクションが unique_ptr で所有して延命する。redo で同一
//    インスタンス (プラグインチェーン / クリップ / レーン含む) がそのまま復帰するため、
//    履歴中の他アクションが持つ Track* / Lane* もダングリングしない。
//  - undo 時の位置解決は indexOf (ポインタ一致) で行う。トラックが既に消えている
//    (Undo 非対応のトラック削除など) 場合は false を返して安全に何もしない。
//  - willRemove はリストから外す直前に呼ばれる。呼び出し側はここでプラグインエディタを
//    閉じ、audioEngine.clearPlayback() (UAF バリア) を張る。
namespace EditActions
{

class TrackAddAction : public juce::UndoableAction
{
public:
    TrackAddAction(TrackManager& tmIn, Track* addedTrack,
                   std::function<void(Track*)> willRemoveCb,
                   std::function<void()> onChangeCb)
        : tm(tmIn), track(addedTrack),
          willRemove(std::move(willRemoveCb)),
          onChange(std::move(onChangeCb)) {}

    bool perform() override
    {
        if (firstPerform)
        {
            firstPerform = false;          // 追加自体は呼び出し側が実施済み
            return track != nullptr;
        }
        if (!stored) return false;         // 整合しない状態 (undo を経ていない)
        tm.insertTrack(insertIndex, std::move(stored));
        if (onChange) onChange();
        return true;
    }

    bool undo() override
    {
        const int idx = tm.indexOf(track);
        if (idx < 0) return false;         // 既に存在しない → 安全に no-op
        insertIndex = idx;                 // redo で同じ位置へ戻す
        if (willRemove) willRemove(track);
        stored = tm.extractTrack(idx);
        if (onChange) onChange();
        return stored != nullptr;
    }

private:
    TrackManager& tm;
    Track* track { nullptr };              // 同一性の解決用 (所有は stored / TrackManager)
    std::unique_ptr<Track> stored;         // undo 中の延命所有
    int  insertIndex { 0 };
    bool firstPerform { true };
    std::function<void(Track*)> willRemove;
    std::function<void()>       onChange;

    JUCE_DECLARE_NON_COPYABLE(TrackAddAction)
};

// トラック削除の Undo アクション (TrackAddAction の逆)。
//
// 設計:
//  - 呼び出し側はまだ削除していない状態で作る (TrackAddAction は「追加済み」で作り最初の
//    perform を no-op にするのに対し、こちらは perform() が実際の削除を行う)。
//  - 複数トラックを 1 アクションで扱う (まとめ削除を 1 つの Undo トランザクションにする)。
//  - 削除した Track は破棄せずアクションが unique_ptr で延命所有する。undo で同一インスタンス
//    (プラグイン / クリップ / レーン含む) がそのまま元の位置へ復帰するため、履歴中の他アクションが
//    持つ Track* / Lane* もダングリングしない。
//  - perform (削除) は index 降順、undo (復元) は元 index 昇順で処理し、index ずれを防いで
//    元の並びを正確に再構成する。位置解決は indexOf (ポインタ一致)。
//  - willRemove は各トラックをリストから外す直前に呼ばれる。呼び出し側はここでプラグイン
//    エディタ / ピアノロールを閉じ、audioEngine.clearPlayback() (UAF バリア) を張る。
class TrackDeleteAction : public juce::UndoableAction
{
public:
    TrackDeleteAction(TrackManager& tmIn, std::vector<Track*> tracksToDelete,
                      std::function<void(Track*)> willRemoveCb,
                      std::function<void()> onChangeCb)
        : tm(tmIn), willRemove(std::move(willRemoveCb)), onChange(std::move(onChangeCb))
    {
        for (auto* t : tracksToDelete)
            if (t != nullptr) entries.push_back({ t, nullptr, -1 });
    }

    bool perform() override   // 削除 (redo も同じ)
    {
        // 現在 index を解決し、降順に外す (外すたびに後続 index がずれるのを防ぐ)。
        std::vector<Entry*> order;
        for (auto& e : entries)
            if (tm.indexOf(e.track) >= 0) order.push_back(&e);
        std::sort(order.begin(), order.end(),
                  [this](Entry* a, Entry* b){ return tm.indexOf(a->track) > tm.indexOf(b->track); });

        bool any = false;
        for (auto* e : order)
        {
            const int idx = tm.indexOf(e->track);
            if (idx < 0) continue;
            if (willRemove) willRemove(e->track);
            e->index  = idx;                       // undo で戻す位置
            e->stored = tm.extractTrack(idx);      // 破棄せず延命所有
            any = any || (e->stored != nullptr);
        }
        if (any && onChange) onChange();
        return any;
    }

    bool undo() override   // 復元
    {
        // 元 index 昇順に挿入して並びを再構成する。
        std::vector<Entry*> order;
        for (auto& e : entries)
            if (e.stored != nullptr) order.push_back(&e);
        std::sort(order.begin(), order.end(),
                  [](Entry* a, Entry* b){ return a->index < b->index; });

        bool any = false;
        for (auto* e : order)
        {
            tm.insertTrack(e->index, std::move(e->stored));   // 所有権を TrackManager へ返す
            any = true;
        }
        if (any && onChange) onChange();
        return any;
    }

private:
    struct Entry
    {
        Track* track { nullptr };          // 同一性の解決用 (所有は stored / TrackManager)
        std::unique_ptr<Track> stored;     // 削除中の延命所有
        int    index { -1 };               // 削除時の位置 (undo の復元先)
    };
    TrackManager& tm;
    std::vector<Entry> entries;
    std::function<void(Track*)> willRemove;
    std::function<void()>       onChange;

    JUCE_DECLARE_NON_COPYABLE(TrackDeleteAction)
};

// トラック並べ替えの Undo アクション。
//
// 設計:
//  - 並べ替え自体は呼び出し側 (TrackHeaderPanel::performReorder) が実施済みなので、
//    最初の perform() は no-op。before/after の「トラック順 (Track* 列)」を保持し、
//    undo は before 順、redo は after 順へ `reorderTo` で並べ直す。
//  - Track の生成/破棄はしない (並べ替えはトラック集合を変えない) ので延命所有は不要。
//    保持する Track* は順序解決にのみ使い、参照外しはしない。
//  - reorderTo は「現在のトラック集合の並べ替えか」を検証し、Undo 非対応のトラック削除等で
//    トラックが消えていれば false を返す → アクションも安全に false で no-op。
class TrackReorderAction : public juce::UndoableAction
{
public:
    TrackReorderAction(TrackManager& tmIn,
                       std::vector<Track*> beforeOrder,
                       std::vector<Track*> afterOrder,
                       std::function<void()> onChangeCb)
        : tm(tmIn), before(std::move(beforeOrder)), after(std::move(afterOrder)),
          onChange(std::move(onChangeCb)) {}

    bool perform() override   // redo (初回は呼び出し側が並べ替え済みなので no-op)
    {
        if (firstPerform) { firstPerform = false; return true; }
        const bool ok = tm.reorderTo(after);
        if (ok && onChange) onChange();
        return ok;
    }

    bool undo() override
    {
        const bool ok = tm.reorderTo(before);
        if (ok && onChange) onChange();
        return ok;
    }

private:
    TrackManager& tm;
    std::vector<Track*> before, after;   // 順序解決用 (参照外ししない)
    bool firstPerform { true };
    std::function<void()> onChange;

    JUCE_DECLARE_NON_COPYABLE(TrackReorderAction)
};

// フォルダ所属変更 (フォルダへ移動 / フォルダから出す / フォルダ削除時の子解放) の Undo。
//
// 設計:
//  - perform() が実際に after 状態 (親ポインタ + トラック順) を適用する (呼び出し側は
//    未適用のまま undoManager.perform() に渡す)。undo() は before 状態へ戻す。
//  - Track の生成/破棄はしないので延命所有は不要。child/parent の生存は indexOf で確認し、
//    消えていれば安全にスキップ (親が消えていればトップレベルへ)。
//  - 順序は before/after の Track* 列を reorderTo で復元する (「フォルダへ移動」は所属と
//    同時に子をフォルダ直後へ動かすため、位置も往復させる)。集合が合わなければ
//    reorderTo が false を返し、normalizeFolderContiguity が最低限の整合を回復する。
class FolderAssignAction : public juce::UndoableAction
{
public:
    struct ParentChange
    {
        Track* child        { nullptr };
        Track* beforeParent { nullptr };
        Track* afterParent  { nullptr };
    };

    FolderAssignAction(TrackManager& tmIn, std::vector<ParentChange> changesIn,
                       std::vector<Track*> beforeOrder, std::vector<Track*> afterOrder,
                       std::function<void()> onChangeCb)
        : tm(tmIn), changes(std::move(changesIn)),
          before(std::move(beforeOrder)), after(std::move(afterOrder)),
          onChange(std::move(onChangeCb)) {}

    bool perform() override { return apply(true); }
    bool undo()    override { return apply(false); }

private:
    bool apply(bool useAfter)
    {
        bool any = false;
        for (auto& c : changes)
        {
            if (c.child == nullptr || tm.indexOf(c.child) < 0) continue;   // 子が消えた
            Track* p = useAfter ? c.afterParent : c.beforeParent;
            if (p != nullptr && (tm.indexOf(p) < 0 || !p->isFolderTrack()))
                p = nullptr;                                               // 親が消えた
            c.child->setFolderParent(p);
            any = true;
        }
        const auto& ord = useAfter ? after : before;
        if (!ord.empty())
            tm.reorderTo(ord);          // 集合が合わなければ false (並びは現状のまま)
        tm.normalizeFolderContiguity(); // 子=フォルダ直後の不変条件を必ず回復
        if (any && onChange) onChange();
        return any;
    }

    TrackManager& tm;
    std::vector<ParentChange> changes;
    std::vector<Track*> before, after;   // 順序解決用 (参照外ししない)
    std::function<void()> onChange;

    JUCE_DECLARE_NON_COPYABLE(FolderAssignAction)
};

} // namespace EditActions
