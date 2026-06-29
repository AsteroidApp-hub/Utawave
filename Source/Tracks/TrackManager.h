// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "Track.h"

class TrackManager
{
public:
    TrackManager(juce::AudioFormatManager& fmt);

    Track* addTrack(const juce::String& name = {}, bool stereo = false);
    Track* addClickTrack();
    bool   hasClickTrack() const;
    Track* getClickTrack() const;   // CLICK トラック (無ければ nullptr)
    // MIDI トラックが 1 つでも存在するか (MIDI 書き出しメニューの活性判定などに使う)
    bool   hasMidiTrack() const;
    void   removeTrack(int index);
    // Undo 用: トラックを破棄せずリストから取り外す / 指定位置へ戻す。
    // インスタンスをアクション側が所有して延命するため、undo→redo で同一インスタンス
    // (プラグイン/クリップ含む) が復帰する。色サイクル (nextColourIndex) は消費しない
    std::unique_ptr<Track> extractTrack(int index);
    void   insertTrack(int index, std::unique_ptr<Track> track);
    // t の現在のインデックス (見つからなければ -1)。Undo 時の生存確認 + 位置解決に使う
    int    indexOf(const Track* t) const;
    // トラックを from → to に移動。to は移動後の最終インデックス
    void   moveTrack(int from, int to);
    // Undo 用: 現在のトラックを desired (Track* 列) の順に並べ替える。desired が現在の
    // トラック集合の並べ替えでなければ (数違い / 消えたトラックを含む) false を返し何もしない。
    bool   reorderTo(const std::vector<Track*>& desired);
    // sourceIdx のトラックを複製し、すぐ後ろに挿入する。
    // クリップ・ゲイン/パン/リバーブ送り・色・MIDI 設定までコピーする。
    // includeTakeLanes=false なら Lane 0 のみコピーし、テイクレーン (Lane 1 以降) は複製しない。
    // プラグインチェーンは AudioPluginInstance の生成が非同期なので別途呼び出し側でクローンする。
    Track* duplicateTrack(int sourceIdx, bool includeTakeLanes = true);
    int    getTrackCount() const { return (int)tracks.size(); }
    Track* getTrack(int i)      { return tracks[(size_t)i].get(); }
    const Track* getTrack(int i) const { return tracks[(size_t)i].get(); }

    // y-offset of each track in the timeline (accounting for lane heights)
    int getTrackY(int index) const;
    int getTotalHeight() const;
    int trackAtY(int y) const;

    juce::AudioThumbnailCache& getThumbnailCache() { return thumbnailCache; }
    juce::AudioFormatManager&  getFormatManager()  { return formatManager; }

    // 全トラック・全レーンの AudioClip を走査する共通ヘルパ。
    // fn は bool を返し、false でイテレーションを打ち切る (find 用途にも使える)。
    template <typename Fn>
    void forEachAudioClip(Fn&& fn)
    {
        for (auto& t : tracks)
        {
            if (!t) continue;
            for (int li = 0; li < t->getLaneCount(); ++li)
            {
                auto* lane = t->getLane(li);
                if (!lane) continue;
                for (auto& c : lane->clips)
                    if (c && !fn(*c)) return;
            }
        }
    }

    // 波形サムネイルのディスクキャッシュ。プロジェクトの Cache/ に保存しておくと、
    // 次回読み込み時に WAV のデコードを丸ごとスキップでき、波形表示が爆速になる。
    // stale 防止はファイル単位: 各サムネイルのハッシュを内容 (パス + サイズ + 更新日時)
    // から算出する ContentHashedFileSource により、差し替わったファイルや新規追加された
    // ファイルだけが自動的にキャッシュミス → 再デコードされる。フォルダ全体を破棄しないので、
    // 録音で 1 ファイル増えても既存波形のキャッシュは全てヒットする。
    void loadThumbnailCache(const juce::File& cacheFile, const juce::File& /*audioFolder*/)
    {
        thumbnailCache.clear();
        if (!cacheFile.existsAsFile()) return;
        juce::FileInputStream in(cacheFile);
        if (in.openedOk()) thumbnailCache.readFromStream(in);
    }
    // force=false: 波形が変わっていなければ (Audio フォルダの署名が既存の .sig と一致) 書き出しを
    //   スキップする。大規模プロジェクトで Cmd+S のたびに数 MB を書き出して固まるのを防ぐ。
    // force=true: 署名が同じでも必ず書き出す。「サムネイルのデコードが完了して初めて完全な
    //   キャッシュになる」一方、署名 (= 音声ファイルの内容) は変わらないため、デコード未完了の
    //   まま保存された空/部分キャッシュが署名スキップで永久に残り、毎回再デコードになる不具合が
    //   あった。波形ロード完了時はデコード結果が新しくなっているので force=true で確実に上書きする。
    void saveThumbnailCache(const juce::File& cacheFile, const juce::File& audioFolder,
                            bool force = false)
    {
        const auto sig = audioFolderSignature(audioFolder);
        auto sigFile = cacheFile.withFileExtension("sig");
        if (!force && cacheFile.existsAsFile() && sigFile.loadFileAsString().trim() == sig)
            return;

        cacheFile.getParentDirectory().createDirectory();
        juce::TemporaryFile tmp(cacheFile);   // アトミック書き込み
        {
            juce::FileOutputStream out(tmp.getFile());
            if (out.openedOk()) { out.setPosition(0); out.truncate();
                                  thumbnailCache.writeToStream(out); }
            else return;
        }
        tmp.overwriteTargetFileWithTemporary();
        // 署名を更新 (次回読み込み時の stale 判定用)
        sigFile.replaceWithText(sig);
    }

    // Audio フォルダ内の音声ファイルの (名前:サイズ:更新日時) を連結してハッシュ化した署名。
    static juce::String audioFolderSignature(const juce::File& audioFolder)
    {
        if (!audioFolder.isDirectory()) return {};
        juce::StringArray entries;
        for (auto& f : audioFolder.findChildFiles(juce::File::findFiles, false))
        {
            const auto ext = f.getFileExtension().toLowerCase();
            if (ext == ".wav" || ext == ".mp3" || ext == ".aif" || ext == ".aiff")
                entries.add(f.getFileName() + ":" + juce::String(f.getSize()) + ":"
                            + juce::String(f.getLastModificationTime().toMilliseconds()));
        }
        entries.sort(true);
        return juce::String(entries.joinIntoString("|").hashCode64());
    }

    std::function<void()> onChanged;

private:
    juce::AudioFormatManager&  formatManager;
    // 保持上限はトラック/クリップ数をカバーする大きさに。64 だと 200+ トラックで
    // スラッシング (再デコード) が起きて遅くなる。
    juce::AudioThumbnailCache  thumbnailCache { 1024 };
    std::vector<std::unique_ptr<Track>> tracks;
    // 新規トラック追加時に巡回的に色を割り当てるためのカウンタ。
    // (旧実装の Track::colourIndex は process-wide static でプロジェクトを開き直しても
    //  続きから振られていたため、本クラスのメンバに移動)
    int nextColourIndex { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackManager)
};
