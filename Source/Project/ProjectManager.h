// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "../Tracks/TrackManager.h"
#include "../AppSettings.h"
#include "../UI/TimelineView.h"  // Marker

class PluginManager;
class PluginChain;

class ProjectManager
{
public:
    // 遅延プラグイン復元 1 件分。load() が VST3 の実生成 (createPluginInstance = プラグインごとに
    // 数百 ms〜数秒) を行わず、生成に必要な情報だけをここへ積む。呼び出し側 (MainComponent) が
    // UI 表示後に 1 件ずつ生成して chain へ挿す (プロジェクトを開く体感を最速にするため)。
    struct DeferredPlugin
    {
        Track*       track { nullptr };   // 挿入先トラック。nullptr = マスターチェーン
        juce::String id;                  // PluginDescription::createIdentifierString
        juce::String name;                // ID 不一致時の名前フォールバック照合用
        juce::String stateB64;            // setStateInformation 用 (Base64)
        int          slotIndex { 0 };
        bool         bypassed  { false };
    };

    struct State
    {
        TrackManager*               trackManager   { nullptr };
        AppSettings*                appSettings    { nullptr };
        std::vector<Marker>*        markers        { nullptr };
        // Transport
        double*                     bpm            { nullptr };
        double*                     loopStartSecs  { nullptr };
        double*                     loopEndSecs    { nullptr };
        bool*                       loopActive     { nullptr };
        double*                     playheadSecs   { nullptr };
        // プラグイン復元用（無くても XML 保存・読込は動くが、その場合プラグインは復元されない）
        PluginManager*              pluginManager  { nullptr };
        double                      pluginSampleRate { 48000.0 };
        int                         pluginBlockSize  { 512 };
        // マスターチェーン（オプション。指定があれば保存/復元される）
        PluginChain*                masterChain    { nullptr };
        // タイムライン横ズーム (px / beat)。プロジェクト保存時の値で次回開いた時に復元する
        double*                     pixelsPerBeat  { nullptr };
        // load 時に見つからなかった音声ファイルの相対パスを格納する出力。
        // 呼び出し側で警告ダイアログを出すために使用する (nullptr なら収集しない)。
        std::vector<juce::String>*  missingFiles   { nullptr };
        // 非 null なら load() は VST3 プラグインを同期生成せずここへ積む (遅延復元)。
        // 内蔵エフェクト (format="Utawave") は即時生成でも一瞬なので従来どおり load 内で復元する。
        // null なら従来どおり load 内で同期生成する (テスト/ヘッドレス経路は不変)。
        std::vector<DeferredPlugin>* deferredPlugins { nullptr };
    };

    static juce::String fileExtension() { return ".uta"; }

    // 旧名 (Trakova) 時代の拡張子。XML ルートタグ互換は load() に残すが、保存・「開く」は常に新拡張子
    static juce::String legacyFileExtension() { return ".trakova"; }
    // 「開く」ダイアログ用ワイルドカード (新拡張子のみ)
    static juce::String openWildcard() { return "*" + fileExtension(); }

    // .uta ファイルへ書き出し
    static bool save(const juce::File& projectFile, const State& s);
    // .uta ファイルから読み込み（呼び出し側で TrackManager 等の状態を復元）
    static bool load(const juce::File& projectFile, State& s);
};
