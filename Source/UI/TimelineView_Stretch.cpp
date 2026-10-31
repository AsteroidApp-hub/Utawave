// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave
//
// クリップのタイムストレッチ (下側の角のドラッグ) とキー変更の合成レンダー。
// 伸縮後のファイルは元ファイルの「クリップ区間 + 前後余白」だけを PitchCore でキー変更と同時に
// 1 回レンダーしたもの。再ストレッチ / 100% 戻しは常に元ファイルから作り直す。

#include "TimelineView.h"
#include "../Audio/PitchEngine.h"
#include "../Edit/ClipStretchMath.h"

namespace
{
    // 変換をモーダル進捗ウィンドウで実行する (キャンセル可)。work は進捗コールバックを受け取る
    class StretchJob : public juce::ThreadWithProgressWindow
    {
    public:
        StretchJob(const juce::String& title,
                   std::function<bool(const std::function<bool(double)>&)> w)
            : juce::ThreadWithProgressWindow(title, true, true), work(std::move(w)) {}

        void run() override
        {
            ok = work([this](double p) { setProgress(p); return ! threadShouldExit(); });
        }
        bool ok { false };

    private:
        std::function<bool(const std::function<bool(double)>&)> work;
    };

    bool runStretchJob(const juce::String& title,
                       std::function<bool(const std::function<bool(double)>&)> work)
    {
        StretchJob job(title, std::move(work));
        const bool finished = job.runThread();
        return finished && job.ok;
    }

    juce::String semitoneSuffix(double semis)
    {
        const int s = juce::roundToInt(semis);
        return (s > 0 ? juce::String("+") : juce::String()) + juce::String(s);
    }

    void showStretchError(const juce::String& msg)
    {
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle(tr(u8"タイムストレッチ"))
            .withMessage(msg)
            .withButton("OK"), nullptr);
    }
}

juce::File TimelineView::findPitchOriginal(const juce::File& f, double& semitonesOut)
{
    semitonesOut = 0.0;
    const auto stem = f.getFileNameWithoutExtension();
    const int at = stem.lastIndexOf("_pitch");
    if (at < 0) return f;

    const auto suffix = stem.substring(at + 6);
    if (suffix.isEmpty() || ! suffix.trimCharactersAtStart("+-").containsOnly("0123456789"))
        return f;

    const auto base = stem.substring(0, at);
    static const char* exts[] = { ".wav", ".aif", ".aiff", ".mp3", ".m4a", ".flac", ".ogg", ".opus" };
    for (auto* e : exts)
    {
        auto cand = f.getParentDirectory().getChildFile(base + e);
        if (cand.existsAsFile())
        {
            semitonesOut = suffix.getDoubleValue();
            return cand;
        }
    }
    return f;
}

void TimelineView::setClipFileUndoable(AudioClip* clip, const juce::File& f)
{
    if (clip == nullptr || clip->getFile() == f) return;
    EditActions::ClipState before; before.capture(clip);
    clip->setFile(f);
    clip->invalidateWaveformCache();
    clip->refreshThumbnail();
    EditActions::ClipState after; after.capture(clip);
    if (undoManager)
    {
        undoManager->beginNewTransaction("Change key");
        undoManager->perform(new EditActions::ClipsPropertyAction(
            { before }, { after }, editChangeCb, clipAliveValidator()));
    }
    else if (editChangeCb) editChangeCb();
    if (onWaveformRefreshNeeded) onWaveformRefreshNeeded();
    repaint();
}

bool TimelineView::applyClipStretch(Track* track, AudioClip* clip, double ratio, bool anchorEnd,
                                    double semitones)
{
    if (track == nullptr || clip == nullptr) return false;
    namespace CSM = ClipStretchMath;

    // 元ファイルと「元ファイル上の区間」を解決 (伸縮済みは記録から、未伸縮はキー変更の命名から)
    const auto& cur = clip->getStretch();
    juce::File source;
    double curSemis = 0.0;
    CSM::SourceSpan span;
    if (cur.isActive())
    {
        source   = cur.source;
        curSemis = cur.semitones;
        span     = CSM::spanOf(clip->getFileOffset(), clip->getDuration(), true, cur.ratio, cur.srcStart);
    }
    else
    {
        source = findPitchOriginal(clip->getFile(), curSemis);
        span   = { clip->getFileOffset(), clip->getDuration() };
    }
    if (std::isnan(semitones)) semitones = curSemis;
    semitones = (double) juce::roundToInt(semitones);
    ratio = CSM::clampRatio(std::round(ratio * 10000.0) / 10000.0);

    if (! source.existsAsFile())
    {
        showStretchError(tr(u8"元の音声ファイルが見つかりませんでした。"));
        return false;
    }

    const double oldStart = clip->getStartPosition();
    const double oldEnd   = clip->getEndPosition();
    const double oldDur   = clip->getDuration();
    auto& fmt = track->getFormatManager();

    juce::File newFile;
    ClipStretch newStretch;
    CSM::Placement pl;

    if (CSM::isUnity(ratio))
    {
        // 伸縮なし: 元ファイル (またはそのキー変更版 = 既存の _pitch±N 命名) を直接参照する
        pl = CSM::placeUnstretched(span, oldStart, oldEnd, anchorEnd);
        if (semitones == 0.0)
            newFile = source;
        else
        {
            newFile = source.getParentDirectory().getChildFile(
                source.getFileNameWithoutExtension() + "_pitch" + semitoneSuffix(semitones) + ".wav");
            if (! newFile.existsAsFile())
            {
                const bool ok = runStretchJob(tr(u8"キーを変換中..."),
                    [&](const std::function<bool(double)>& prog)
                    {
                        return PitchEngine::processFile(source, newFile, fmt, semitones, 32,
                                                        [&prog](double p) { prog(p); });
                    });
                if (! ok) { newFile.deleteFile(); showStretchError(tr(u8"ピッチシフト処理に失敗しました。")); return false; }
            }
        }
    }
    else
    {
        double sr = 0.0, srcLen = 0.0;
        if (std::unique_ptr<juce::AudioFormatReader> r { fmt.createReaderFor(source) }; r && r->sampleRate > 0.0)
        {
            sr     = r->sampleRate;
            srcLen = (double) r->lengthInSamples / sr;
        }
        if (sr <= 0.0)
        {
            showStretchError(tr(u8"元の音声ファイルが見つかりませんでした。"));
            return false;
        }

        auto region = CSM::renderRegion(span, srcLen);
        const auto startSample = (juce::int64) std::llround(region.start * sr);
        const auto numSamples  = (juce::int64) std::llround(region.duration * sr);
        region.start    = (double) startSample / sr;   // サンプル境界に揃えて配置と一致させる
        region.duration = (double) numSamples / sr;

        newFile = source.getParentDirectory().getChildFile(
            source.getFileNameWithoutExtension()
            + "_st" + juce::String(juce::roundToInt(ratio * 10000.0))
            + (semitones != 0.0 ? "_k" + semitoneSuffix(semitones) : juce::String())
            + "_" + juce::String(startSample) + "_" + juce::String(numSamples) + ".wav");

        if (! newFile.existsAsFile())
        {
            const bool ok = runStretchJob(tr(u8"タイムストレッチ中..."),
                [&](const std::function<bool(double)>& prog)
                {
                    return PitchEngine::renderRegion(source, newFile, fmt, startSample, numSamples,
                                                     semitones, ratio, prog);
                });
            if (! ok)
            {
                newFile.deleteFile();
                return false;   // キャンセル or 失敗 (キャンセル時はダイアログを出さない)
            }
        }

        pl = CSM::place(span, ratio, region, oldStart, oldEnd, anchorEnd);
        newStretch.ratio     = ratio;
        newStretch.source    = source;
        newStretch.srcStart  = region.start;
        newStretch.semitones = semitones;
    }

    EditActions::ClipState before; before.capture(clip);

    if (clip->getFile() != newFile)
    {
        clip->setFile(newFile);
        clip->invalidateWaveformCache();
        clip->refreshThumbnail();
    }
    clip->setStretch(newStretch);
    clip->setStartPosition(pl.startPos);
    clip->setDuration(pl.duration);
    clip->setFileOffset(pl.fileOffset);
    if (oldDur > 0.0)
    {
        const double k = pl.duration / oldDur;   // ゲインエンベロープも同じ比率で伸縮
        for (auto& gp : clip->getGainPointsRW()) gp.time *= k;
    }

    EditActions::ClipState after; after.capture(clip);
    if (undoManager)
    {
        undoManager->beginNewTransaction("Time stretch");
        undoManager->perform(new EditActions::ClipsPropertyAction(
            { before }, { after }, editChangeCb, clipAliveValidator()));
    }
    else if (editChangeCb) editChangeCb();
    if (onWaveformRefreshNeeded) onWaveformRefreshNeeded();
    repaint();
    return true;
}

// ── 下側の角のドラッグ ──
void TimelineView::beginClipStretch(const ClipRef& ref, bool anchorEnd)
{
    auto* c = ref.clip;
    const auto& st = c->getStretch();
    const auto span = ClipStretchMath::spanOf(c->getFileOffset(), c->getDuration(),
                                              st.isActive(), st.ratio, st.srcStart);
    stretchActive       = true;
    stretchRef          = ref;
    stretchAnchorEnd    = anchorEnd;
    stretchOrigStart    = c->getStartPosition();
    stretchOrigEnd      = c->getEndPosition();
    stretchSpanDur      = span.duration;
    stretchPreviewRatio = st.isActive() ? st.ratio : 1.0;
    repaint();
}

void TimelineView::updateClipStretch(const juce::MouseEvent& e)
{
    if (stretchSpanDur <= 0.0) return;
    const double raw = juce::jmax(0.0, xToPosition(e.x));
    const double t   = e.mods.isCommandDown() ? raw : snapTime(raw);
    const double newDur = stretchAnchorEnd ? (stretchOrigEnd - t) : (t - stretchOrigStart);
    stretchPreviewRatio = ClipStretchMath::ratioForDuration({ 0.0, stretchSpanDur }, newDur);
    repaint();
}

void TimelineView::finishClipStretch()
{
    stretchActive = false;
    const auto ref   = stretchRef;
    const double ratio = stretchPreviewRatio;
    const bool anchorEnd = stretchAnchorEnd;
    stretchRef.clear();
    repaint();

    if (! ref.valid() || ! clipStillExists(ref.clip) || trackManager.indexOf(ref.track) < 0) return;
    const auto& st = ref.clip->getStretch();
    const double curRatio = st.isActive() ? st.ratio : 1.0;
    if (std::abs(ratio - curRatio) < 1.0e-4) return;   // 倍率が変わっていない = クリック

    // 変換はモーダル進捗ウィンドウを伴うため、マウスイベントの外 (次のメッセージ) で行う
    juce::MessageManager::callAsync(
        [safe = juce::Component::SafePointer<TimelineView>(this), ref, ratio, anchorEnd]
        {
            auto* tv = safe.getComponent();
            if (tv == nullptr || ! tv->clipStillExists(ref.clip)
                || tv->trackManager.indexOf(ref.track) < 0) return;
            tv->applyClipStretch(ref.track, ref.clip, ratio, anchorEnd);
        });
}
