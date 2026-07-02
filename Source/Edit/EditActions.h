// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>
#include "../Tracks/AudioClip.h"
#include "../Tracks/Track.h"

namespace EditActions
{

// 破棄系アクションが「取り除いた AudioClip」の所有権を渡す先 (遅延破棄シンク)。
// AudioEngine::deferClipDestruction に繋ぐ。再生中編集でも他トラックを止めずに済むよう、
// クリップを即破棄せず参照中スナップショットの寿命に合わせて回収させる。
using ClipSink = std::function<void(std::vector<std::unique_ptr<AudioClip>>&&)>;

// ClipState::restore 前の生存確認 (UAF 防止)。ClipState は AudioClip* を生ポインタで保持する
// ため、履歴中の後続アクションがインスタンスを再生成する系 (LaneSnapshotAction / ClipSplitAction /
// StripSilenceAction の生成クリップは undo/redo でインスタンスが変わる) と組むと、undo/redo で
// 破棄済みクリップを restore しかねない (v0.5.6 Windows 実クラッシュ: redo → setName の String UAF)。
// TimelineView::clipStillExists (全レーン走査) を繋ぐ。
using ClipValidator = std::function<bool(AudioClip*)>;

// ───────────────────────── 汎用スナップショット ─────────────────────────
// 任意の値型 T を before/after で差し替える Undo Action。
// 「状態をまるごと差し替える」系の編集 (マーカー / テンポ・拍子変更 / 曲全体の BPM /
// トラックのプロパティ等) に使う。apply は perform/undo の両方で呼ばれ、与えられた値を
// 実体へ書き戻す責務を持つ (UI 更新・dirty マークも apply 側で行う)。
// T は値 (vector やプレーンな構造体) を保持するため、Track* 等の生ポインタを T に入れない
// 限りダングリングしない。生ポインタを伴う場合は apply 内で存在チェックすること。
template <typename T>
class SnapshotAction : public juce::UndoableAction
{
public:
    SnapshotAction(T beforeVal, T afterVal, std::function<void(const T&)> applyFn)
        : oldV(std::move(beforeVal)), newV(std::move(afterVal)), apply(std::move(applyFn)) {}

    bool perform() override { if (apply) apply(newV); return true; }
    bool undo()    override { if (apply) apply(oldV); return true; }

private:
    T oldV, newV;
    std::function<void(const T&)> apply;
};

// クリップ生成用のパラメータ（Add アクションで使用）
struct ClipParams
{
    juce::File   file;
    double       startPos   { 0.0 };
    double       duration   { 0.0 };
    double       fileOffset { 0.0 };
    double       fadeIn     { 0.010 };
    double       fadeOut    { 0.010 };
    float        gain       { 1.0f };
    juce::String name;
    juce::Colour colour     { juce::Colour(0xff3a6ea5) };
};

// クリップ全プロパティのスナップショット
struct ClipState
{
    AudioClip* clip       { nullptr };
    double     startPos   { 0.0 };
    double     duration   { 0.0 };
    double     fileOffset { 0.0 };
    double     fadeIn     { 0.0 };
    double     fadeOut    { 0.0 };
    float      gain       { 1.0f };
    FadeCurve  fadeInCurve  { FadeCurve::Linear };
    FadeCurve  fadeOutCurve { FadeCurve::Linear };
    juce::String name;
    std::vector<GainPoint> gainPoints;

    void capture(AudioClip* c)
    {
        clip = c;
        if (!c) return;
        startPos     = c->getStartPosition();
        duration     = c->getDuration();
        fileOffset   = c->getFileOffset();
        fadeIn       = c->getFadeInSecs();
        fadeOut      = c->getFadeOutSecs();
        gain         = c->getGain();
        fadeInCurve  = c->getFadeInCurve();
        fadeOutCurve = c->getFadeOutCurve();
        name         = c->getName();
        gainPoints   = c->getGainPoints();
    }

    void restore() const
    {
        if (!clip) return;
        clip->setStartPosition(startPos);
        clip->setDuration(duration);
        clip->setFileOffset(fileOffset);
        clip->setFadeInSecs(fadeIn);
        clip->setFadeOutSecs(fadeOut);
        clip->setGain(gain);
        clip->setFadeInCurve(fadeInCurve);
        clip->setFadeOutCurve(fadeOutCurve);
        if (name.isNotEmpty()) clip->setName(name);
        clip->getGainPointsRW() = gainPoints;
    }

    bool differsFrom(const ClipState& other) const
    {
        if (startPos     != other.startPos
         || duration     != other.duration
         || fileOffset   != other.fileOffset
         || fadeIn       != other.fadeIn
         || fadeOut      != other.fadeOut
         || gain         != other.gain
         || fadeInCurve  != other.fadeInCurve
         || fadeOutCurve != other.fadeOutCurve
         || name         != other.name) return true;
        if (gainPoints.size() != other.gainPoints.size()) return true;
        for (size_t i = 0; i < gainPoints.size(); ++i)
            if (gainPoints[i].time != other.gainPoints[i].time
             || gainPoints[i].dB   != other.gainPoints[i].dB) return true;
        return false;
    }
};

// 複数クリップのプロパティ変更（移動・リサイズ・フェード・クロスフェード）
// clipAliveCb (生存チェック) は必須引数。本番経路は TimelineView::clipAliveValidator() を渡す。
// nullptr は「対象クリップの寿命を呼び出し側が保証する」テスト等の限定用途のみ (明示させるため
// デフォルト引数にしない。PluginActions の ChainResolver と同じ作法)
class ClipsPropertyAction : public juce::UndoableAction
{
public:
    ClipsPropertyAction(std::vector<ClipState> oldS,
                        std::vector<ClipState> newS,
                        std::function<void()> onChangeCb,
                        ClipValidator clipAliveCb)
        : oldStates(std::move(oldS)),
          newStates(std::move(newS)),
          onChange(std::move(onChangeCb)),
          clipAlive(std::move(clipAliveCb)) {}

    bool perform() override { return applyStates(newStates); }
    bool undo()    override { return applyStates(oldStates); }

private:
    bool applyStates(const std::vector<ClipState>& states)
    {
        for (auto& s : states)
            if (clipAlive == nullptr || clipAlive(s.clip))  // 破棄済みクリップは触らない (UAF 防止)
                s.restore();
        if (onChange) onChange();
        return true;
    }

    std::vector<ClipState> oldStates;
    std::vector<ClipState> newStates;
    std::function<void()>  onChange;
    ClipValidator          clipAlive;
};

// クリップ追加（コピー＆ペースト・複製で使用、undo で削除）
class ClipAddAction : public juce::UndoableAction
{
public:
    ClipAddAction(Lane* l, ClipParams p,
                  juce::AudioFormatManager& fmt, juce::AudioThumbnailCache& cache,
                  std::function<void()> onChangeCb)
        : lane(l), params(std::move(p)),
          formatMgr(fmt), thumbCache(cache),
          onChange(std::move(onChangeCb)) {}

    bool perform() override
    {
        if (!lane) return false;
        if (stored)
        {
            // Redo: 保存していたクリップを戻す
            addedPtr = stored.get();
            lane->clips.push_back(std::move(stored));
        }
        else
        {
            // 初回: 新規作成
            addedPtr = lane->addClip(params.file, params.startPos, params.duration,
                                      formatMgr, thumbCache);
            if (addedPtr)
            {
                addedPtr->setFileOffset(params.fileOffset);
                addedPtr->setFadeInSecs(params.fadeIn);
                addedPtr->setFadeOutSecs(params.fadeOut);
                addedPtr->setGain(params.gain);
                if (params.name.isNotEmpty()) addedPtr->setName(params.name);
                addedPtr->setColour(params.colour);
            }
        }
        if (onChange) onChange();
        return addedPtr != nullptr;
    }

    bool undo() override
    {
        if (!lane) return false;
        for (auto it = lane->clips.begin(); it != lane->clips.end(); ++it)
        {
            if (it->get() == addedPtr)
            {
                stored = std::move(*it);
                lane->clips.erase(it);
                addedPtr = nullptr;
                if (onChange) onChange();
                return true;
            }
        }
        return false;
    }

    AudioClip* getAddedClip() const { return addedPtr; }

private:
    Lane*                       lane;
    ClipParams                  params;
    juce::AudioFormatManager&   formatMgr;
    juce::AudioThumbnailCache&  thumbCache;
    std::function<void()>       onChange;

    AudioClip*                  addedPtr { nullptr };
    std::unique_ptr<AudioClip>  stored;
};

// クリップ削除（unique_ptr で保持して undo で復活）
class ClipDeleteAction : public juce::UndoableAction
{
public:
    ClipDeleteAction(Lane* l, AudioClip* clipToRemove,
                     std::function<void()> onChangeCb)
        : lane(l), clipPtr(clipToRemove), onChange(std::move(onChangeCb)) {}

    bool perform() override
    {
        if (!lane || !clipPtr) return false;
        for (auto it = lane->clips.begin(); it != lane->clips.end(); ++it)
        {
            if (it->get() == clipPtr)
            {
                stored = std::move(*it);
                lane->clips.erase(it);
                if (onChange) onChange();
                return true;
            }
        }
        return false;
    }

    bool undo() override
    {
        if (!lane || !stored) return false;
        clipPtr = stored.get();
        lane->clips.push_back(std::move(stored));
        if (onChange) onChange();
        return true;
    }

private:
    Lane*                       lane;
    AudioClip*                  clipPtr;
    std::unique_ptr<AudioClip>  stored;
    std::function<void()>       onChange;
};

// クリップ分割（Alt+Click = Mac は Option）
class ClipSplitAction : public juce::UndoableAction
{
public:
    ClipSplitAction(Lane* l, AudioClip* original, double splitPos,
                    juce::AudioFormatManager& fmt, juce::AudioThumbnailCache& cache,
                    std::function<void()> onChangeCb,
                    ClipSink deferClipsCb = {})
        : lane(l), originalPtr(original), splitSecs(splitPos),
          formatMgr(fmt), thumbCache(cache), onChange(std::move(onChangeCb)),
          deferClips(std::move(deferClipsCb))
    {
        if (original)
        {
            origState.capture(original);
            origFile          = original->getFile();
            origName          = original->getName();
            origColour        = original->getColour();
            origHasCustomCol  = original->hasCustomColour();
        }
    }

    bool perform() override
    {
        if (!lane || !originalPtr) return false;

        // 元クリップを lane から storedOriginal へ移動
        for (auto it = lane->clips.begin(); it != lane->clips.end(); ++it)
        {
            if (it->get() == originalPtr)
            {
                storedOriginal = std::move(*it);
                lane->clips.erase(it);
                break;
            }
        }
        if (!storedOriginal) return false;

        const double cs = origState.startPos;
        const double ce = origState.startPos + origState.duration;
        const double splitTimeInClip = splitSecs - cs;
        const bool   hasEnv = !origState.gainPoints.empty();
        const float  dBAtSplit = hasEnv
                                 ? storedOriginal->getEnvelopeDBAt(splitTimeInClip)
                                 : 0.0f;

        // 左クリップ
        leftPtr = lane->addClip(origFile, cs, splitSecs - cs, formatMgr, thumbCache);
        if (leftPtr)
        {
            leftPtr->setFileOffset(origState.fileOffset);
            leftPtr->setFadeInSecs(origState.fadeIn);
            leftPtr->setGain(origState.gain);
            leftPtr->setName(origName);
            // フェードカーブを継承 (継承しないと分割で Linear に戻ってしまう)
            leftPtr->setFadeInCurve(origState.fadeInCurve);
            leftPtr->setFadeOutCurve(origState.fadeOutCurve);
            // 元がカスタム色を持つ場合のみ引き継ぐ (トラック色追従を壊さないため)
            if (origHasCustomCol) leftPtr->setColour(origColour);

            // ゲインエンベロープを継承（境界に補間ポイントを追加）
            auto& leftPts = leftPtr->getGainPointsRW();
            for (auto& p : origState.gainPoints)
                if (p.time < splitTimeInClip) leftPts.push_back(p);
            if (hasEnv) leftPts.push_back({ splitTimeInClip, dBAtSplit });
        }
        // 右クリップ
        rightPtr = lane->addClip(origFile, splitSecs, ce - splitSecs, formatMgr, thumbCache);
        if (rightPtr)
        {
            rightPtr->setFileOffset(origState.fileOffset + (splitSecs - cs));
            rightPtr->setFadeOutSecs(origState.fadeOut);
            rightPtr->setGain(origState.gain);
            rightPtr->setName(origName);
            // フェードカーブを継承 (継承しないと分割で Linear に戻ってしまう)
            rightPtr->setFadeInCurve(origState.fadeInCurve);
            rightPtr->setFadeOutCurve(origState.fadeOutCurve);
            if (origHasCustomCol) rightPtr->setColour(origColour);

            // 右クリップ: 境界のポイント（時刻 0）と分割時刻以降のポイントを時刻シフトしてコピー
            auto& rightPts = rightPtr->getGainPointsRW();
            if (hasEnv) rightPts.push_back({ 0.0, dBAtSplit });
            for (auto& p : origState.gainPoints)
                if (p.time > splitTimeInClip)
                    rightPts.push_back({ p.time - splitTimeInClip, p.dB });
        }

        if (onChange) onChange();
        return true;
    }

    bool undo() override
    {
        if (!lane) return false;

        // UAF防止: 分割クリップ(left/right)を即破棄すると再生中スナップショットの sourceClip が
        // ダングリングするため、deferClips で遅延破棄へ所有権を渡す (audio を止めずに済む)。
        std::vector<std::unique_ptr<AudioClip>> removed;
        for (auto it = lane->clips.begin(); it != lane->clips.end(); )
        {
            if (it->get() == leftPtr || it->get() == rightPtr)
            {
                removed.push_back(std::move(*it));
                it = lane->clips.erase(it);
            }
            else
                ++it;
        }
        if (deferClips) deferClips(std::move(removed));
        leftPtr = rightPtr = nullptr;

        // 元クリップを復活
        if (storedOriginal)
        {
            originalPtr = storedOriginal.get();
            origState.clip = originalPtr;
            origState.restore();
            lane->clips.push_back(std::move(storedOriginal));
        }

        if (onChange) onChange();
        return true;
    }

private:
    Lane*                          lane;
    AudioClip*                     originalPtr;
    double                         splitSecs;
    juce::AudioFormatManager&      formatMgr;
    juce::AudioThumbnailCache&     thumbCache;
    std::function<void()>          onChange;

    ClipState                      origState;
    juce::File                     origFile;
    juce::String                   origName;
    juce::Colour                   origColour;
    bool                           origHasCustomCol { false };

    std::unique_ptr<AudioClip>     storedOriginal;
    AudioClip*                     leftPtr  { nullptr };
    AudioClip*                     rightPtr { nullptr };
    ClipSink                       deferClips;  // 取り除いたクリップを遅延破棄へ渡す (audio を止めない)
};

// 無音区間カット（Strip Silence）: 元クリップを「残すセグメント」群に分割
// keepSegments は元クリップ内秒数 [start, end]、startPosition は維持される
class StripSilenceAction : public juce::UndoableAction
{
public:
    struct Segment { double startInClip; double endInClip; };

    StripSilenceAction(Lane* l, AudioClip* original,
                       std::vector<Segment> segments, double fadeSecs,
                       juce::AudioFormatManager& fmt, juce::AudioThumbnailCache& cache,
                       std::function<void()> onChangeCb,
                       ClipSink deferClipsCb = {})
        : lane(l), originalPtr(original),
          keepSegments(std::move(segments)), fadeAtBoundarySecs(fadeSecs),
          formatMgr(fmt), thumbCache(cache),
          onChange(std::move(onChangeCb)),
          deferClips(std::move(deferClipsCb))
    {
        if (original)
        {
            origState.capture(original);
            origFile          = original->getFile();
            origName          = original->getName();
            origColour        = original->getColour();
            origHasCustomCol  = original->hasCustomColour();
        }
    }

    bool perform() override
    {
        if (!lane || !originalPtr) return false;

        // 元クリップを lane から storedOriginal へ移動
        for (auto it = lane->clips.begin(); it != lane->clips.end(); ++it)
        {
            if (it->get() == originalPtr)
            {
                storedOriginal = std::move(*it);
                lane->clips.erase(it);
                break;
            }
        }
        if (!storedOriginal) return false;

        createdClips.clear();
        const double cs        = origState.startPos;
        const double origFI    = origState.fadeIn;
        const double origFO    = origState.fadeOut;
        const double origDur   = origState.duration;
        const double boundary  = juce::jmax(0.0, fadeAtBoundarySecs);

        for (size_t i = 0; i < keepSegments.size(); ++i)
        {
            const auto& seg = keepSegments[i];
            const double segDur = seg.endInClip - seg.startInClip;
            if (segDur <= 0.001) continue;

            auto* nc = lane->addClip(origFile, cs + seg.startInClip, segDur,
                                      formatMgr, thumbCache);
            if (!nc) continue;
            nc->setFileOffset(origState.fileOffset + seg.startInClip);
            nc->setGain(origState.gain);
            nc->setName(origName);
            if (origHasCustomCol) nc->setColour(origColour);

            // フェード: 先頭/末尾セグメントは元のフェード値を継承、内側境界には短いフェードを当てる
            const bool isFirst = (i == 0);
            const bool isLast  = (i == keepSegments.size() - 1);
            const double fadeInSecs  = isFirst
                ? juce::jmin(origFI, segDur * 0.5)
                : juce::jmin(boundary, segDur * 0.5);
            const double fadeOutSecs = isLast
                ? juce::jmin(origFO, segDur * 0.5)
                : juce::jmin(boundary, segDur * 0.5);
            nc->setFadeInSecs(fadeInSecs);
            nc->setFadeOutSecs(fadeOutSecs);

            createdClips.push_back(nc);
        }

        // 元クリップにゲインエンベロープがあった場合は警告として捨てる（複雑化を避ける）
        // 必要なら後でセグメントごとに分割して引き継ぎ可能

        if (onChange) onChange();
        juce::ignoreUnused(origDur);
        return true;
    }

    bool undo() override
    {
        if (!lane) return false;

        // UAF防止: 生成クリップを即破棄すると再生中スナップショットの sourceClip がダングリング
        // するため、deferClips で遅延破棄へ所有権を渡す (audio を止めずに済む)。
        std::vector<std::unique_ptr<AudioClip>> removed;
        for (auto it = lane->clips.begin(); it != lane->clips.end(); )
        {
            const bool isCreated = std::find(createdClips.begin(), createdClips.end(),
                                              it->get()) != createdClips.end();
            if (isCreated) { removed.push_back(std::move(*it)); it = lane->clips.erase(it); }
            else           ++it;
        }
        if (deferClips) deferClips(std::move(removed));
        createdClips.clear();

        // 元クリップを復活
        if (storedOriginal)
        {
            originalPtr = storedOriginal.get();
            origState.clip = originalPtr;
            origState.restore();
            lane->clips.push_back(std::move(storedOriginal));
        }

        if (onChange) onChange();
        return true;
    }

private:
    Lane*                          lane;
    AudioClip*                     originalPtr;
    std::vector<Segment>           keepSegments;
    double                         fadeAtBoundarySecs;
    juce::AudioFormatManager&      formatMgr;
    juce::AudioThumbnailCache&     thumbCache;
    std::function<void()>          onChange;

    ClipState                      origState;
    juce::File                     origFile;
    juce::String                   origName;
    juce::Colour                   origColour;
    bool                           origHasCustomCol { false };

    std::unique_ptr<AudioClip>     storedOriginal;
    std::vector<AudioClip*>        createdClips;
    ClipSink                       deferClips;  // 取り除いたクリップを遅延破棄へ渡す (audio を止めない)
};

// レーン全体のスナップショット (テイク差し込み等、複雑な操作の Undo に使用)
class LaneSnapshotAction : public juce::UndoableAction
{
public:
    struct ClipSnap
    {
        juce::File   file;
        double       startPos { 0.0 };
        double       duration { 0.0 };
        double       fileOffset { 0.0 };
        double       fadeIn { 0.0 };
        double       fadeOut { 0.0 };
        float        gain { 1.0f };
        juce::String name;
        juce::Colour colour { juce::Colour(0xff3a6ea5) };
        bool         hasCustomColour { false };
        FadeCurve    fadeInCurve  { FadeCurve::Linear };
        FadeCurve    fadeOutCurve { FadeCurve::Linear };
        std::vector<GainPoint> gainPoints;

        static ClipSnap capture(AudioClip* c)
        {
            ClipSnap s;
            s.file            = c->getFile();
            s.startPos        = c->getStartPosition();
            s.duration        = c->getDuration();
            s.fileOffset      = c->getFileOffset();
            s.fadeIn          = c->getFadeInSecs();
            s.fadeOut         = c->getFadeOutSecs();
            s.gain            = c->getGain();
            s.name            = c->getName();
            s.colour          = c->getColour();
            s.hasCustomColour = c->hasCustomColour();
            s.fadeInCurve     = c->getFadeInCurve();
            s.fadeOutCurve    = c->getFadeOutCurve();
            s.gainPoints      = c->getGainPoints();  // クリップゲインエンベロープも保存 (テイク採用で消えるのを防ぐ)
            return s;
        }
    };

    LaneSnapshotAction(Lane* l,
                       std::vector<ClipSnap> beforeState,
                       std::vector<ClipSnap> afterState,
                       juce::AudioFormatManager& fmt, juce::AudioThumbnailCache& cache,
                       std::function<void()> onChangeCb,
                       ClipSink deferClipsCb = {})
        : lane(l),
          beforeSnap(std::move(beforeState)),
          afterSnap(std::move(afterState)),
          formatMgr(fmt), thumbCache(cache),
          onChange(std::move(onChangeCb)),
          deferClips(std::move(deferClipsCb)) {}

    bool perform() override { applySnap(afterSnap); return true; }
    bool undo()    override { applySnap(beforeSnap); return true; }

private:
    void applySnap(const std::vector<ClipSnap>& snap)
    {
        if (!lane) return;
        // UAF防止: clips を破棄すると再生中スナップショットの sourceClip がダングリングするため、
        // 即破棄せず deferClips で AudioEngine の遅延破棄へ所有権を渡す (参照中スナップショットが
        // 回収されるまで延命される)。deferClips が無い場合のみ即破棄 (フォールバック)。
        if (deferClips) deferClips(std::move(lane->clips));
        lane->clips.clear();
        for (auto& s : snap)
        {
            auto* c = lane->addClip(s.file, s.startPos, s.duration, formatMgr, thumbCache);
            if (!c) continue;
            c->setFileOffset(s.fileOffset);
            c->setGain(s.gain);
            if (s.name.isNotEmpty()) c->setName(s.name);
            if (s.hasCustomColour) c->setColour(s.colour);
            c->setFadeInCurve(s.fadeInCurve);
            c->setFadeOutCurve(s.fadeOutCurve);
            c->setFadeInSecs(s.fadeIn);
            c->setFadeOutSecs(s.fadeOut);
            c->getGainPointsRW() = s.gainPoints;  // ゲインエンベロープを復元
        }
        if (onChange) onChange();
    }

    Lane* lane;
    std::vector<ClipSnap> beforeSnap;
    std::vector<ClipSnap> afterSnap;
    juce::AudioFormatManager&  formatMgr;
    juce::AudioThumbnailCache& thumbCache;
    std::function<void()>      onChange;
    ClipSink                   deferClips;  // 取り除いたクリップを遅延破棄へ渡す (audio を止めない)
};

// ───────────────────────── MIDI クリップ ─────────────────────────
// MIDI クリップは AudioClip と違い、再生スナップショット (AudioEngine::preparePlayback) が
// MidiClip* ではなくイベント列を「値コピー」して保持するため、編集で MidiClip を破棄/作成しても
// audio thread の dangling は起きない (onChange = editChangeCb で再構築すれば良い)。よって
// 遅延破棄 (deferClips) は不要。開いているピアノロールだけ willRemove で閉じる。

// MIDI クリップの移動 / リサイズ (start / duration の変更。クリップ実体は作り直さないので
// 開いているピアノロールも保持される)
class MidiClipPropertyAction : public juce::UndoableAction
{
public:
    MidiClipPropertyAction(MidiClip* c,
                           double oldStart, double oldDur,
                           double newStart, double newDur,
                           std::function<void()> onChangeCb)
        : clip(c), oStart(oldStart), oDur(oldDur),
          nStart(newStart), nDur(newDur), onChange(std::move(onChangeCb)) {}

    bool perform() override { return apply(nStart, nDur); }
    bool undo()    override { return apply(oStart, oDur); }

private:
    bool apply(double s, double d)
    {
        if (clip == nullptr) return false;
        clip->setStartPosition(s);
        clip->setDuration(d);
        if (onChange) onChange();
        return true;
    }
    MidiClip* clip;
    double oStart, oDur, nStart, nDur;
    std::function<void()> onChange;
};

// MIDI クリップの差し替え (分割 / 削除 / 作成)。既存クリップ群 (toRemove) を取り除き、
// 新規クリップ群 (toAdd) を生成する。取り除いたクリップは同一インスタンスで undo 復活させ、
// 触らないクリップは識別子を保ったまま残す (= そのピアノロールは閉じない)。
class MidiClipReplaceAction : public juce::UndoableAction
{
public:
    struct NewMidiClip
    {
        double       startPos { 0.0 };
        double       duration { 0.0 };
        juce::String name;
        juce::Colour colour  { juce::Colour(0xff7a5aa5) };
        int          channel { 0 };
        juce::MidiMessageSequence sequence;
    };

    MidiClipReplaceAction(Track* tr,
                          std::vector<MidiClip*>   toRemove,
                          std::vector<NewMidiClip> toAdd,
                          std::function<void()>          onChangeCb,
                          std::function<void(MidiClip*)> willRemoveCb = {})
        : track(tr), removePtrs(std::move(toRemove)), addParams(std::move(toAdd)),
          onChange(std::move(onChangeCb)), willRemove(std::move(willRemoveCb)) {}

    bool perform() override
    {
        if (track == nullptr) return false;
        // 既存クリップを取り除く (ピアノロールを閉じてから所有権を退避)
        removedStored.clear();
        for (auto* c : removePtrs)
        {
            if (willRemove) willRemove(c);
            if (auto p = track->extractMidiClip(c))
                removedStored.push_back(std::move(p));
        }
        // 追加クリップ: 初回はパラメータから生成、redo 以降は同一インスタンスを戻す。
        // インスタンスを作り直さないのは、移動/リサイズの MidiClipPropertyAction が
        // この追加クリップの生ポインタを掴んでいる場合に、undo/redo を跨いでも
        // ダングリングしないようにするため (= use-after-free 回避。AudioClip の
        // ClipAddAction が stored unique_ptr で同一インスタンスを保つのと同じ方針)。
        addedPtrs.clear();
        if (! created)
        {
            for (const auto& np : addParams)
            {
                auto* c = track->addMidiClip(np.startPos, np.duration);
                if (c == nullptr) continue;
                c->setName(np.name);
                c->setColour(np.colour);
                c->setChannel(np.channel);
                c->getSequence() = np.sequence;
                addedPtrs.push_back(c);
            }
            created = true;
        }
        else
        {
            for (auto& p : addedStored)
            {
                addedPtrs.push_back(p.get());
                track->insertMidiClip(std::move(p));
            }
            addedStored.clear();
        }
        if (onChange) onChange();
        return true;
    }

    bool undo() override
    {
        if (track == nullptr) return false;
        // 追加したクリップを退避 (破棄せず保持 = redo で同一インスタンスを戻す)
        addedStored.clear();
        for (auto* c : addedPtrs)
        {
            if (willRemove) willRemove(c);
            if (auto p = track->extractMidiClip(c))
                addedStored.push_back(std::move(p));
        }
        addedPtrs.clear();
        // 退避した元クリップを同一インスタンスで復活し、次回 redo 用にポインタを再特定
        removePtrs.clear();
        for (auto& p : removedStored)
        {
            removePtrs.push_back(p.get());
            track->insertMidiClip(std::move(p));
        }
        removedStored.clear();
        if (onChange) onChange();
        return true;
    }

private:
    Track*                   track;
    std::vector<MidiClip*>   removePtrs;   // 取り除く既存クリップ (undo で再特定される)
    std::vector<NewMidiClip> addParams;    // 追加するクリップの内容 (初回生成のみ)
    bool                     created { false };  // 追加クリップを一度生成済みか
    std::function<void()>          onChange;
    std::function<void(MidiClip*)> willRemove;

    std::vector<std::unique_ptr<MidiClip>> removedStored;  // 取り除いた元クリップ (undo 復活用)
    std::vector<std::unique_ptr<MidiClip>> addedStored;    // undo 中に退避した追加クリップ (redo 復活用)
    std::vector<MidiClip*>                 addedPtrs;      // 追加したクリップ (現在 track にある間)
};

}  // namespace EditActions
