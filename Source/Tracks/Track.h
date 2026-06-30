// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include <atomic>
#include "AudioClip.h"
#include "MidiClip.h"
#include "../Recording/LiveRecordingBuffer.h"

struct Lane
{
    std::vector<std::unique_ptr<AudioClip>> clips;
    // soloed は audio thread から読まれる (再生レーン選択) ため atomic 化 (#R-7)。
    std::atomic<bool> muted  { false };
    std::atomic<bool> soloed { false };

    bool overlaps(double start, double end) const;
    AudioClip* addClip(const juce::File& file, double startPos, double dur,
                       juce::AudioFormatManager& fmt, juce::AudioThumbnailCache& cache);
};

class Track
{
public:
    static constexpr int minHeight     { 30 };   // 名前行 (M/S/R/I) だけが見える最小サイズ
    // 高さ上限 = INS 8 スロット枠の高さ (TrackHeaderView::insFrameH と一致させる。static_assert で担保)。
    // 8 スロット全部を出せる所までドラッグできれば十分で、それ以上は INS パネルより高くなり余白が出る。
    static constexpr int maxHeight     { 190 };
    static constexpr int defaultHeight { 100 };  // 基本: 上段 + Vol/Pan + メータ + In/TList 行
    static constexpr int laneHeight    { 48 };

    // INS スロット枠 (固定数、トラック右側に表示)。スロット高は UI 側 (TrackHeaderView::insSlotH)
    // が単一の真実源。旧 insertSlotHeight (model 側 18) は未使用で UI の 22 と食い違うため撤去。
    static constexpr int insertSlotCount  { 8 };
    static constexpr int insertAreaWidth  { 130 };  // 横に確保する幅

    Track(const juce::String& trackName, juce::AudioFormatManager& fmt,
          juce::AudioThumbnailCache& cache);
    // 色を呼び出し側 (TrackManager) で割り当てる場合のコンストラクタ。
    Track(const juce::String& trackName, juce::AudioFormatManager& fmt,
          juce::AudioThumbnailCache& cache, juce::Colour initialColour);
    ~Track();

    // TrackManager から色サイクルに使うためのパレットアクセサ。
    static juce::Colour paletteColour(int idx);
    static int paletteSize() { return 8; }

    // Identity
    const juce::String& getName()   const { return name; }
    void  setName(const juce::String& n)  { name = n; }
    juce::Colour getColour()         const { return colour; }
    void  setColour(juce::Colour c)        { colour = c; }

    // Mix
    float getVolume() const   { return volume; }
    void  setVolume(float db) { volume = db; }
    float getPan()    const   { return pan; }
    void  setPan(float p)     { pan = p; }
    // 簡易リバーブセンド量 (0..1)。0 で完全ドライ、1 で全幅ウェット。
    float getReverbSend() const { return reverbSend; }
    void  setReverbSend(float s) { reverbSend = juce::jlimit(0.0f, 1.0f, s); }

    // State
    bool isMuted()            const { return muted; }
    void setMuted(bool m)           { muted = m; }
    bool isSoloed()           const { return soloed; }
    void setSoloed(bool s)          { soloed = s; }
    bool isRecArmed()         const { return recArmed; }
    void setRecArmed(bool r)        { recArmed = r; }
    bool isInputMonitor()     const { return inputMonitor; }
    void setInputMonitor(bool v)    { inputMonitor = v; }
    int  getInputChannel()    const { return inputChannel; }
    void setInputChannel(int ch)    { inputChannel = ch; }
    bool isClickTrack()       const { return clickTrack; }
    void setClickTrack(bool b)      { clickTrack = b; }
    // MIDI トラック判定（true なら midiClips を持ち、AudioClip は持たない）
    bool isMidiTrack()        const { return midiTrack; }
    void setMidiTrack(bool b)       { midiTrack = b; }
    // 内蔵シンセ パラメータ
    int  getSynthWaveform()   const { return synthWaveform; }   // 0=Sine 1=Saw 2=Square
    void setSynthWaveform(int w)    { synthWaveform = juce::jlimit(0, 2, w); }
    // 内蔵シンセ有効/無効。OFF にすると MIDI は INS チェーン（VST 音源等）に直接送られる
    bool isSynthEnabled()     const { return synthEnabled; }
    void setSynthEnabled(bool v)    { synthEnabled = v; }

    // MIDI 移調: オクターブ単位 (±N) と 半音単位 (±N)
    int  getOctaveShift()        const { return octaveShift; }
    void setOctaveShift(int v)         { octaveShift = juce::jlimit(-4, 4, v); }
    int  getSemitoneTranspose()  const { return semitoneTranspose; }
    void setSemitoneTranspose(int v)   { semitoneTranspose = juce::jlimit(-12, 12, v); }
    // 合計移調量 (半音)
    int  getTotalTransposeSemitones() const { return octaveShift * 12 + semitoneTranspose; }
    bool isStereo()           const { return stereo; }
    void setStereo(bool s)          { stereo = s; }
    int  getClickSound()      const { return clickSound; }
    void setClickSound(int s)       { clickSound = s; }
    bool isClickAccent()      const { return clickAccent; }
    void setClickAccent(bool a)     { clickAccent = a; }
    // 0=Normal(x1), 1=Half(x1/2), 2=Double(x2)
    int  getClickRate()       const { return clickRate; }
    void setClickRate(int r)        { clickRate = r; }

    // Height (user-resizable)
    static constexpr int laneMinHeight { 30 };
    static constexpr int laneMaxHeight { 200 };

    int  getCustomHeight()    const { return customHeight; }
    void setCustomHeight(int h)     { customHeight = juce::jlimit(minHeight, maxHeight, h); }
    int  getMainHeight()      const { return juce::jlimit(minHeight, maxHeight, customHeight); }
    int  getLaneHeight()      const { return juce::jlimit(laneMinHeight, laneMaxHeight, customLaneHeight); }
    void setCustomLaneHeight(int h) { customLaneHeight = juce::jlimit(laneMinHeight, laneMaxHeight, h); }
    int  getTotalHeight()     const;

    // Lanes
    int         getLaneCount() const { return (int)lanes.size(); }
    Lane*       getLane(int i)       { return lanes[(size_t)i].get(); }
    const Lane* getLane(int i) const { return lanes[(size_t)i].get(); }
    AudioClip*  addClip(const juce::File& file, double startPos, double dur);
    // Lane 0 の指定位置へ直接クリップを追加 (D&D 取り込み用・既存クリップと重なってもよい)
    AudioClip*  addClipToLane0(const juce::File& file, double startPos, double dur);

    // ── MIDI クリップ ──
    int        getMidiClipCount()  const { return (int)midiClips.size(); }
    MidiClip*  getMidiClip(int i)        { return (i >= 0 && (size_t)i < midiClips.size()) ? midiClips[(size_t)i].get() : nullptr; }
    const MidiClip* getMidiClip(int i) const { return (i >= 0 && (size_t)i < midiClips.size()) ? midiClips[(size_t)i].get() : nullptr; }
    MidiClip*  addMidiClip(double startPos, double duration)
    {
        auto clip = std::make_unique<MidiClip>(startPos, duration);
        auto* ptr = clip.get();
        midiClips.push_back(std::move(clip));
        return ptr;
    }
    void removeMidiClip(int i)
    {
        if (i < 0 || (size_t)i >= midiClips.size()) return;
        midiClips.erase(midiClips.begin() + i);
    }
    // MIDI クリップを所有権ごと取り出す (Undo アクションで保持して後で戻すため)。
    // 見つからなければ nullptr。
    std::unique_ptr<MidiClip> extractMidiClip(MidiClip* clip)
    {
        for (auto it = midiClips.begin(); it != midiClips.end(); ++it)
            if (it->get() == clip)
            {
                auto p = std::move(*it);
                midiClips.erase(it);
                return p;
            }
        return {};
    }
    // 取り出した MIDI クリップを戻す (Undo で同一インスタンスを復活させる)
    void insertMidiClip(std::unique_ptr<MidiClip> clip)
    {
        if (clip != nullptr) midiClips.push_back(std::move(clip));
    }
    // 必要に応じてレーンを追加して指定インデックスのレーンを返す
    Lane*       ensureLane(int index)
    {
        while (index >= (int)lanes.size())
            lanes.push_back(std::make_unique<Lane>());
        return lanes[(size_t)index].get();
    }

    // レーン開閉
    bool isLanesCollapsed()   const { return lanesCollapsed; }
    void setLanesCollapsed(bool v)  { lanesCollapsed = v; }
    bool isInsertSlotsVisible() const { return insertSlotsVisible; }
    void setInsertSlotsVisible(bool v) { insertSlotsVisible = v; }

    // ライブ録音レーン管理 (常に Lane 0 へ書き込む方式)
    // fileOffset: 録音ファイル先頭の読み飛ばし秒 (録音レイテンシ補正でタイムライン 0 に
    // クランプされた分。lane0 クリップとテイク退避の両方に同値が入り dedup 3 つ組も揃う)
    // bufferLeadSecs: liveBuffer 先頭の「表示しない先行録音分」(カウントイン/プリロールの
    // 遡及録音区間)。オーバーレイは startPosSecs (R 押下位置) から、バッファの
    // bufferLeadSecs 以降だけを描く (録音はその手前から行われているが表には出さない)
    void       startLiveRecording(double startPosSecs, double bufferLeadSecs = 0.0);
    AudioClip* finishLiveRecording(const juce::File& file, double startPos, double dur,
                                   double fileOffset = 0.0);
    void       cancelLiveRecording();
    // 指定範囲を Take レーンへバックアップ (重ならない既存レーンを探し、無ければ新規作成)
    AudioClip* backupToTakeLane(const juce::File& file, double startPos, double dur,
                                 double fileOffset = 0.0);
    // Lane 0 内で newClip と重なるクリップをトリムし、境界に最小クロスフェードを作る
    void trimAndCrossfadeOnLane0(AudioClip* newClip, double startPos, double dur);
    bool       hasLiveRecording()          const { return liveRecordingLaneIdx >= 0; }
    int        getLiveRecordingLaneIndex() const { return liveRecordingLaneIdx; }
    LiveRecordingBuffer& getLiveBuffer()         { return liveBuffer; }
    double getRecordingStartPos()          const { return recordingStartPos; }
    // ループ録音のラップ後、ライブ波形オーバーレイの表示開始をループ頭へ進める (表示専用。
    // liveBuffer 自体は AudioEngine がラップ時に reset 済み)
    void   setRecordingStartPos(double s)        { recordingStartPos = s; }
    // liveBuffer 先頭の非表示先行録音分 (カウントイン/プリロール)。ラップ後は 0 に戻す
    double getLiveBufferLeadSecs()         const { return liveBufferLeadSecs; }
    void   setLiveBufferLeadSecs(double s)       { liveBufferLeadSecs = juce::jmax(0.0, s); }

    // 分割などの編集操作から参照するためにfriend宣言
    juce::AudioFormatManager&  getFormatManager()  { return formatManager; }
    juce::AudioThumbnailCache& getThumbnailCache() { return thumbnailCache; }

    // インサートエフェクトチェーン
    class PluginChain& getPluginChain() { return *pluginChain; }
    const class PluginChain& getPluginChain() const { return *pluginChain; }

private:
    static const juce::Colour trackColours[8];

    juce::String name;
    juce::Colour colour;
    // volume/pan/reverbSend は audio thread (renderClip / processBlock) から読まれ、UI thread から
    // 書かれるため atomic 化して data race (規格上 UB) を回避する。アクセサは暗黙 load/store になる。
    std::atomic<float> volume     { 0.0f };
    std::atomic<float> pan        { 0.0f };
    std::atomic<float> reverbSend { 0.0f };  // 0..1
    // muted/soloed は audio thread (processBlock) から読まれ、UI thread から書かれるため
    // atomic 化して data race (規格上 UB) を回避する (#R-7)。
    std::atomic<bool> muted  { false };
    std::atomic<bool> soloed { false };
    // recArmed: renderClip がパンチイン判定で読む。clickTrack: callback がクリック判定で読む。
    std::atomic<bool> recArmed { false };
    bool   inputMonitor       { false };
    std::atomic<bool> clickTrack { false };
    bool   midiTrack      { false };  // MIDI トラック判定
    bool   stereo         { false };  // false=mono, true=stereo
    int    clickSound     { 0 };
    bool   clickAccent    { true };
    int    clickRate      { 0 };  // 0=Normal, 1=Half, 2=Double
    int    inputChannel   { 0 };
    int    customHeight     { defaultHeight };
    int    customLaneHeight { laneHeight };
    // 内蔵シンセ パラメータ（MIDI トラック専用）。synthWaveform/synthEnabled は audio thread が
    // 毎ブロック反映するため atomic 化する。
    std::atomic<int>  synthWaveform { 1 };   // 0=Sine 1=Saw 2=Square (デフォルト: Saw)
    std::atomic<bool> synthEnabled  { true };// false で内蔵シンセを止め、INS チェーンへ MIDI を直接送る
    // MIDI 移調。getTotalTransposeSemitones() で audio thread が読む (実演奏中も即時反映) ため atomic。
    std::atomic<int>  octaveShift       { 0 };
    std::atomic<int>  semitoneTranspose { 0 };
    double recordingStartPos    { 0.0 };
    double liveBufferLeadSecs   { 0.0 };
    int    liveRecordingLaneIdx { -1 };
    bool   lanesCollapsed       { true };  // デフォルト非表示（TList OFF）
    bool   insertSlotsVisible   { false }; // デフォルト非表示

    std::vector<std::unique_ptr<Lane>> lanes;
    std::vector<std::unique_ptr<MidiClip>> midiClips;
    LiveRecordingBuffer liveBuffer;

    std::unique_ptr<class PluginChain> pluginChain;

    juce::AudioFormatManager&  formatManager;
    juce::AudioThumbnailCache& thumbnailCache;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Track)
};
