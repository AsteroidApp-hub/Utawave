// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// TimelineView の描画関連実装。
// drawClip / drawClipSamples / drawCrossfadeOverlay / drawMidiClip / drawTrackRows
// および ドラッグモード判定 (getDragMode) を切り出している。
// TimelineView.cpp が肥大化したため分割。クラス本体は TimelineView.h で宣言。

#include "TimelineView.h"
#include "../AppColours.h"
#include "../Tracks/MidiClip.h"
#include "TextImageCache.h"
#include <map>

// 共有 Font プール (FontOptions 構築コストを避ける)。
// static なので TU 単位で独立 — TimelineView.cpp と TimelineView_Drawing.cpp で
// 別実体が作られるが、両 TU から見える名前は同じなので透過的に使える。
static const juce::Font& sharedBarFont()      { static const juce::Font f { juce::FontOptions(10.5f) }; return f; }
static const juce::Font& sharedTimeFont()     { static const juce::Font f { juce::FontOptions(9.5f) };  return f; }
static const juce::Font& sharedHeaderFont()   { static const juce::Font f { juce::FontOptions(10.0f, juce::Font::bold) }; return f; }
static const juce::Font& sharedClipNameFont() { static const juce::Font f { juce::FontOptions(10.0f) }; return f; }

// クロスフェード視覚要素 (X カーブ / 端ハンドル) のアルファ減衰係数。重なりが画面上で
// 細い (= 縮小) ほど小さくして白い縦線が目立たないようにする。drawCrossfadeOverlay と
// drawTrackRows の Lane0 オーバーレイで共用 (係数のドリフト防止)。
static float crossfadeWidthVis(float overlapPx) noexcept
{
    return juce::jlimit(0.28f, 1.0f, (overlapPx + 1.0f) / 13.0f);
}

TimelineView::DragMode TimelineView::getDragMode(const ClipRef& ref, int mouseX, int mouseY) const
{
    if (!ref.valid()) return DragMode::None;

    const double bps = bpm / 60.0;
    int cx = (int)(ref.clip->getStartPosition() * bps * pixelsPerBeat - scrollX);
    int cw = juce::jmax(4, (int)(ref.clip->getDuration() * bps * pixelsPerBeat));

    // クリップ上端の Y を計算してチップ領域を検出
    auto area     = getContentArea();
    int trackTop  = area.getY() + trackManager.getTrackY(ref.trackIdx) - scrollY;
    int lTop      = trackTop + (ref.laneIdx == 0 ? 0
                    : ref.track->getMainHeight() + (ref.laneIdx - 1) * ref.track->getLaneHeight());
    int clipTopY  = lTop + 1;

    // クリップ上部 12px がフェードチップヒットエリア
    if (mouseY >= clipTopY && mouseY < clipTopY + 12)
    {
        int fiPx    = (int)(ref.clip->getFadeInSecs()  * bps * pixelsPerBeat);
        int foPx    = (int)(ref.clip->getFadeOutSecs() * bps * pixelsPerBeat);
        int fiChipX = cx + fiPx;
        int foChipX = cx + cw - foPx;

        if (std::abs(mouseX - fiChipX) <= kHandleHitPx) return DragMode::FadeIn;
        if (std::abs(mouseX - foChipX) <= kHandleHitPx) return DragMode::FadeOut;
    }

    // クロスフェード境界ハンドル（XFADE ON 時、全高さで有効）
    if (appSettings.autoCrossfade)
    {
        auto* lane = ref.track->getLane(ref.laneIdx);
        if (lane)
        {
            // 左隣クリップ（異ファイル）が接触 → このクリップの FadeIn 境界
            for (auto& cPtr : lane->clips)
            {
                auto* nb = cPtr.get();
                if (nb == ref.clip || AudioClip::isSameContinuousAudio(*nb, *ref.clip)) continue;
                if (std::abs(nb->getEndPosition() - ref.clip->getStartPosition()) > 0.002) continue;

                int boundary = (int)((ref.clip->getStartPosition() + ref.clip->getFadeInSecs())
                                     * bps * pixelsPerBeat - scrollX);
                if (std::abs(mouseX - boundary) <= kHandleHitPx) return DragMode::FadeIn;
            }
            // 右隣クリップ（異ファイル）が接触 → このクリップの FadeOut 境界
            for (auto& cPtr : lane->clips)
            {
                auto* nb = cPtr.get();
                if (nb == ref.clip || AudioClip::isSameContinuousAudio(*nb, *ref.clip)) continue;
                if (std::abs(nb->getStartPosition() - ref.clip->getEndPosition()) > 0.002) continue;

                int boundary = (int)((ref.clip->getEndPosition() - ref.clip->getFadeOutSecs())
                                     * bps * pixelsPerBeat - scrollX);
                if (std::abs(mouseX - boundary) <= kHandleHitPx) return DragMode::FadeOut;
            }
        }
    }

    // 左右端 8px がリサイズエリア（クリップ幅が 20px 以上のとき有効）
    if (cw >= 20)
    {
        if (mouseX <= cx + 8)      return DragMode::ResizeLeft;
        if (mouseX >= cx + cw - 8) return DragMode::ResizeRight;
    }

    // 左下ゲインウィジェット（常時）
    int clipBottomY = lTop + (ref.laneIdx == 0
                              ? juce::jmin(ref.track->getMainHeight(), ref.track->getTotalHeight())
                              : ref.track->getLaneHeight()) - 2;
    if (cw >= 70 && (clipBottomY - clipTopY) >= 30)
    {
        juce::Rectangle<int> gWidget { cx + 4, clipBottomY - 13, 56, 12 };
        if (gWidget.contains(mouseX, mouseY))
            return DragMode::Gain;
    }

    // GAIN ON 時: ブレークポイント or ゲインラインのヒット検出
    if (appSettings.showClipGain)
    {
        int clipMidY = (clipTopY + clipBottomY) / 2;
        int halfH    = (clipBottomY - clipTopY) / 2;

        // 既存ブレークポイント上か（dB→Y は非対称マッピング）
        auto dbToNorm = [](float dB) -> float {
            if (dB >= 0.0f) return juce::jlimit(0.0f, 1.0f, dB / 12.0f);
            return juce::jlimit(-1.0f, 0.0f, dB / 60.0f);
        };
        const auto& pts = ref.clip->getGainPoints();
        for (size_t i = 0; i < pts.size(); ++i)
        {
            int px = cx + (int)(pts[i].time * bps * pixelsPerBeat);
            int py = clipMidY - (int)(dbToNorm(pts[i].dB) * halfH);
            if (std::abs(mouseX - px) <= kHandleHitPx && std::abs(mouseY - py) <= kHandleHitPx)
                return DragMode::GainPoint;
        }

        // ライン上（ポイント追加可能エリア）
        if (mouseY >= clipTopY + 14 && mouseY <= clipBottomY - 14
            && mouseX > cx + 8 && mouseX < cx + cw - 8)
            return DragMode::GainPoint;  // クリックで新規ポイント追加
    }

    // ── ツールモードに応じた挙動 ──
    switch (appSettings.toolMode)
    {
        case ToolMode::Click:
            return DragMode::Move;
        case ToolMode::Selection:
            return DragMode::Selection;
        case ToolMode::Both:
        {
            int smartMidY = (clipTopY + clipBottomY) / 2;
            if ((clipBottomY - clipTopY) >= 30 && mouseY < smartMidY)
                return DragMode::Selection;
            return DragMode::Move;
        }
    }
    return DragMode::Move;
}
void TimelineView::drawClip(juce::Graphics& g, AudioClip& clip,
                              juce::Rectangle<int> laneBounds,
                              juce::Colour trackColour,
                              bool isSelected,
                              Lane* ownerLane)
{
    // 個別色が明示設定されていればそれを使う、それ以外はトラック色
    if (clip.hasCustomColour())
        trackColour = clip.getColour();

    const double beatsPerSec = bpm / 60.0;
    int clipX = (int)(clip.getStartPosition() * beatsPerSec * pixelsPerBeat - scrollX);
    int clipW = juce::jmax(4, (int)(clip.getDuration() * beatsPerSec * pixelsPerBeat));

    // クリップの矩形（レーン高さを使用）
    juce::Rectangle<int> clipRect {
        clipX,
        laneBounds.getY() + 1,
        clipW,
        laneBounds.getHeight() - 2
    };

    // 画面外 / 部分 repaint のクリップ範囲外は描画スキップ
    if (!clipRect.intersects(getLocalBounds())) return;
    if (!clipRect.intersects(g.getClipBounds())) return;

    // ── ここからレーン境界にクリップ ──
    g.saveState();
    g.reduceClipRegion(laneBounds);  // 隣レーンへの滲み出しを防ぐ

    // 移動中の選択クリップは半透明（ゴースト表示）
    bool isMovingGhost = (dragMode == DragMode::Move) && isSelected;
    float alphaMul = isMovingGhost ? 0.45f : 1.0f;

    // クリップ背景
    g.setColour(trackColour.withAlpha(0.55f * alphaMul));
    g.fillRoundedRectangle(clipRect.toFloat(), 2.0f);

    // 波形サムネイル（fileOffset 対応、ゲイン × 波形ズームで振幅をスケール）
    // キャッシュ: 同じ条件なら drawChannels を呼ばず Image を blit するだけ
    // 既定 (waveformZoom=1.0・gain=1.0) では係数 1.0 = ピーク 0dB がレーン全高まで届く。
    // 超過 (ゲイン等で >0dB) は drawClipWaveform / drawClipSamples の jlimit(topY,botY) で
    // レーン端に頭打ち = クリップしたように平らな天井で描かれる
    if (clip.getThumbnail().getTotalLength() > 0.0)
    {
        const juce::Colour wfColour = trackColour.brighter(0.9f).withAlpha(0.9f * alphaMul);
        const double fo = clip.getFileOffset();
        const float verticalZoom = juce::jlimit(0.05f, 4.0f,
                                                1.0f * clip.getGain() * (float)waveformZoom);
        auto wfRect = clipRect.reduced(0, 4);

        // 全ズームでサムネイル塗りに統一 (サンプル単位描画は廃止)。
        // 深い拡大では 64 サンプル粒度のサムネイルなのでブロック状になる。
        {
            const int needW = wfRect.getWidth();
            const int needH = wfRect.getHeight();

            // Retina (高 DPI) では物理ピクセル密度で描いて初めてシャープになる。
            // 倍率はメモリ保護のため 2.0 で頭打ち (一般的な Retina をカバー)。
            const float pixelScale = juce::jlimit(1.0f, 2.0f,
                                                  g.getInternalContext().getPhysicalPixelScaleFactor());
            const int scaledW = juce::jmax(1, juce::roundToInt(needW * pixelScale));
            const int scaledH = juce::jmax(1, juce::roundToInt(needH * pixelScale));

            // 巨大すぎる場合 (長尺 × 高 DPI) はキャッシュせず g 直描画 (これも物理解像度で鮮明)
            const bool useCache = (needW > 0 && needH > 0 && scaledW <= 8192 && scaledH <= 512);

            if (useCache)
            {
                auto& cache = clip.getWaveformCache();
                const juce::uint32 colourArgb = wfColour.getARGB();
                const juce::int64 samplesDone = clip.getThumbnail().getNumSamplesFinished();
                // 連続ズーム中 (zoomActive) は再生成を抑止し、既存キャッシュを新しい幅へ拡縮 blit
                // するだけにする (全クリップを毎イベント再描画するもたつきを防ぐ)。ズームが止まると
                // タイマーが zoomActive を下げて綺麗に再描画する。有効キャッシュが無ければ通常生成。
                const bool skipRegen = zoomActive && cache.image.isValid();
                if (!skipRegen
                    && (cache.width      != needW
                    || cache.height  != needH
                    || cache.fileOffset != fo
                    || cache.duration   != clip.getDuration()
                    || cache.gain       != clip.getGain()
                    || cache.vZoom      != waveformZoom
                    || cache.colourARGB != colourArgb
                    || cache.samplesFinished != samplesDone
                    || cache.pixelScale != pixelScale
                    || !cache.image.isValid()))
                {
                    cache.width      = needW;
                    cache.height     = needH;
                    cache.fileOffset = fo;
                    cache.duration   = clip.getDuration();
                    cache.gain       = clip.getGain();
                    cache.vZoom      = waveformZoom;
                    cache.colourARGB = colourArgb;
                    cache.samplesFinished = samplesDone;
                    cache.pixelScale = pixelScale;
                    // 物理ピクセルサイズで Image を確保 → Retina でも 1:1 でシャープ
                    cache.image      = juce::Image(juce::Image::ARGB, scaledW, scaledH, true);

                    juce::Graphics ig(cache.image);
                    drawClipWaveform(ig, clip, { 0, 0, scaledW, scaledH },
                                     fo, clip.getDuration(), verticalZoom, wfColour);
                }
                // 物理解像度の Image を貼る。横は「丸めた矩形 (wfRect.getX()/needW)」ではなく
                // クリップ開始/尺から計算した端数 (fractional) の絶対位置・絶対幅で貼ることで、
                // 波形が絶対タイムライングリッドに揃う。これによりクリップを分割/トリムして矩形
                // (丸め) が変わっても波形が ±1px ずれて見えない (カット端が動かない)。
                // キャッシュ内容はクリップ相対 [0,scaledW]→[fileOffset,+duration] のままなので
                // スクロール非依存・再生成不要。
                const double pxPerSec = pixelsPerBeat * (bpm / 60.0);
                const double exactL   = clip.getStartPosition() * pxPerSec - scrollX;
                const double exactW   = juce::jmax(1.0, clip.getDuration() * pxPerSec);
                g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
                // 端数貼り付けで波形が矩形を最大 ~0.5px はみ出し得るので、クリップ矩形に
                // クリップして隣のクリップへにじまないようにする (本体のグリッド整合は不変)。
                juce::Graphics::ScopedSaveState wfClip(g);
                g.reduceClipRegion(wfRect);
                // スケールは実際のキャッシュ画像サイズ基準にする。ズーム中に再生成を抑止すると
                // cache.image は旧サイズ (scaledW と不一致) のことがあるため、画像幅で割って正しく
                // 新しい幅 exactW へ引き伸ばす (位置は exactL で絶対グリッドに揃う)。
                const int imgW = juce::jmax(1, cache.image.getWidth());
                const int imgH = juce::jmax(1, cache.image.getHeight());
                g.drawImageTransformed(cache.image,
                    juce::AffineTransform::scale((float) (exactW / (double) imgW),
                                                 (float) needH / (float) imgH)
                        .translated((float) exactL, (float) wfRect.getY()));
            }
            else
            {
                // 巨大クリップ (キャッシュ上限超え): 毎ペイント fillPath で描き直すと、JUCE の CALayer は
                // drawsAsynchronously のため CA::Transaction::commit がメインスレッドでその完了を待ち、
                // 重い波形描画で数百 ms 固まる (再生バーのカクツキ)。可視範囲スライスを per-clip Image に
                // キャッシュして blit にし、スクロール/ズーム/内容が変わった時だけ再生成する。可視範囲は
                // コンポーネント全幅でクランプ (プレイヘッド帯 g.getClipBounds() では無い) ので、再生バーが
                // 動いても毎フレーム同じキー = キャッシュヒット = blit のみ → CA コミットの待ちが消える。
                auto& cache = clip.getWaveformCache();
                const auto comp = getLocalBounds();
                const int vx0 = juce::jmax(wfRect.getX(),     comp.getX());
                const int vx1 = juce::jmin(wfRect.getRight(), comp.getRight());
                if (vx1 > vx0)
                {
                    // 帯は scrollX を含まない絶対ピクセル座標 (= time_sec * pxPerSec) で扱う。
                    // 可視絶対範囲 [visAbsL, visAbsR] に左右マージン (1 ビューポート幅) を足し、
                    // クリップの絶対ピクセル範囲でクランプして帯を決める。スクロールが帯内に
                    // 収まる限り再生成せず、blit の平行移動だけで描く (横スクロールのもたつき解消)。
                    const double clipAbsL = wfRect.getX()     + scrollX;
                    const double clipAbsR = wfRect.getRight() + scrollX;
                    const double visAbsL  = vx0 + scrollX;
                    const double visAbsR  = vx1 + scrollX;
                    const double margin   = (double) (vx1 - vx0); // 片側 1 ビューポート幅
                    const juce::uint32 colourArgb = wfColour.getARGB();
                    const juce::int64  samplesDone = clip.getThumbnail().getNumSamplesFinished();

                    // 内容 (ズーム以外) が不変か。pixelsPerBeat / bpm は別扱いにする:
                    // 連続ズーム中 (zoomActive) は ppb が変わっても再生成せず、下の時刻写像で
                    // stale 帯を拡縮 blit し、止まった後にタイマーがシャープに再描画する。
                    const bool contentSame =
                           cache.bigImage.isValid()
                        && cache.bigBandR > cache.bigBandL
                        && cache.bigHeight == needH
                        && cache.bigFileOffset == fo
                        && cache.bigGain == clip.getGain()
                        && cache.bigVZoom == waveformZoom
                        && cache.bigColourARGB == colourArgb
                        && cache.bigSamplesFinished == samplesDone
                        && cache.bigPixelScale == pixelScale;
                    const bool zoomSame = cache.bigPixelsPerBeat == pixelsPerBeat
                                       && cache.bigBpm == bpm;
                    // 可視範囲が帯に収まっているか (ppb 不変前提の絶対座標比較)
                    const bool covered = contentSame && zoomSame
                        && visAbsL >= cache.bigBandL - 0.5 && visAbsR <= cache.bigBandR + 0.5;
                    // ズーム中 / 高速スクロール中は内容一致なら再生成を抑止し、stale 帯を時刻写像で
                    // 拡縮 blit する (重い fillPath を走らせず滑らかに流す。止まるとタイマーで鮮明化)。
                    const bool reuseDeferred = contentSame && (zoomActive || deferBigClipRegen);

                    if (!covered && !reuseDeferred)
                    {
                        const double bandL = juce::jmax(clipAbsL, visAbsL - margin);
                        const double bandR = juce::jmin(clipAbsR, visAbsR + margin);
                        const double bandW = juce::jmax(1.0, bandR - bandL);
                        const int sW = juce::jmax(1, juce::roundToInt(bandW * pixelScale));
                        const int sH = juce::jmax(1, juce::roundToInt(needH * pixelScale));
                        cache.bigBandL = bandL; cache.bigBandR = bandR; cache.bigHeight = needH;
                        cache.bigPixelsPerBeat = pixelsPerBeat; cache.bigBpm = bpm;
                        cache.bigFileOffset = fo; cache.bigGain = clip.getGain(); cache.bigVZoom = waveformZoom;
                        cache.bigColourARGB = colourArgb; cache.bigSamplesFinished = samplesDone;
                        cache.bigPixelScale = pixelScale;
                        cache.bigImage = juce::Image(juce::Image::ARGB, sW, sH, true);
                        juce::Graphics ig(cache.bigImage);
                        // 帯 [bandL, bandR] (絶対 px) に対応するファイル時刻範囲を Image 全体に描く
                        const double pxPerSec     = pixelsPerBeat * (bpm / 60.0);
                        const double fileStartVis = fo + (bandL / juce::jmax(1e-9, pxPerSec)
                                                          - clip.getStartPosition());
                        const double durVis       = (pxPerSec > 0.0) ? (bandW / pxPerSec) : clip.getDuration();
                        drawClipWaveform(ig, clip, { 0, 0, sW, sH },
                                         fileStartVis, durVis, verticalZoom, wfColour);
                    }
                    // 帯が表す「タイムライン秒」範囲 (描画時 ppb 基準) を現在の ppb でコンポーネント
                    // 座標へ写す。ppb 不変なら bandL - scrollX に一致し、ズーム中は stale 帯が拡縮される。
                    const double cachedPxSec = juce::jmax(1e-9, cache.bigPixelsPerBeat * (cache.bigBpm / 60.0));
                    const double secL    = cache.bigBandL / cachedPxSec;
                    const double secR    = cache.bigBandR / cachedPxSec;
                    const double curPxSec = juce::jmax(1e-9, pixelsPerBeat * beatsPerSec);
                    const double compL   = secL * curPxSec - scrollX;
                    const double compW   = juce::jmax(1.0, (secR - secL) * curPxSec);
                    const int    imgW    = juce::jmax(1, cache.bigImage.getWidth());
                    const int    imgH    = juce::jmax(1, cache.bigImage.getHeight());
                    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
                    g.drawImageTransformed(cache.bigImage,
                        juce::AffineTransform::scale((float) (compW / imgW),
                                                     (float) needH / (float) imgH)
                            .translated((float) compL, (float) wfRect.getY()));
                }
            }
        }
    }

    // ── フェードイン / フェードアウトのカーブ描画 (X 字交差) ──
    // xStart / fadeW は「ピクセル」(float)。整数に丸めると、ズームで fadeW が
    // 0↔1px を跨ぐたびに全高の縦線がパッと点滅する。float のまま扱い、さらに
    // 幅が狭いほど不透明度を滑らかに落とすことで「線が見えたり消えたり」を防ぐ。
    // (掴む位置は常時表示の角の三角ハンドルが示すので、消えても操作性は不変)
    auto drawFadeCurve = [&](float xStart, float fadeW, bool isFadeIn, FadeCurve curve)
    {
        if (fadeW <= 0.5f) return;
        const float top    = (float)clipRect.getY();
        const float bottom = (float)clipRect.getBottom();
        const float height = bottom - top;

        // 幅 3px 以下は非表示、14px で全表示まで滑らかにランプ (点滅・縦線化の抑制)
        const float vis = juce::jlimit(0.0f, 1.0f, (fadeW - 3.0f) / 11.0f);
        if (vis <= 0.001f) return;

        // 薄い暗化オーバーレイ (波形が透けるよう alpha 控えめ)
        juce::Path tri;
        if (isFadeIn)
        {
            tri.addTriangle(xStart,          top,
                            xStart + fadeW,  top,
                            xStart,          bottom);
        }
        else
        {
            tri.addTriangle(xStart,          top,
                            xStart + fadeW,  top,
                            xStart + fadeW,  bottom);
        }
        g.setColour(juce::Colours::black.withAlpha(0.30f * vis));
        g.fillPath(tri);

        // フェードカーブ本体 (ストローク)
        juce::Path p;
        const int segments = juce::jmax(8, (int)(fadeW / 3.0f));
        for (int i = 0; i <= segments; ++i)
        {
            const float t   = (float)i / (float)segments;
            const float gain = AudioClip::applyFadeCurve(isFadeIn ? t : (1.0f - t), curve);
            const float x   = xStart + t * fadeW;
            const float y   = bottom - gain * height;
            if (i == 0) p.startNewSubPath(x, y);
            else        p.lineTo(x, y);
        }
        g.setColour(juce::Colours::white.withAlpha(0.85f * vis));
        g.strokePath(p, juce::PathStrokeType(1.4f));
    };

    // ドラッグ中は対象 (selected) クリップ以外のフェードは描画を抑制 (微調整の視認性向上)
    const bool dragActive = (dragMode == DragMode::Move
                             || dragMode == DragMode::ResizeLeft
                             || dragMode == DragMode::ResizeRight);
    const bool suppressFades = dragActive && !isSelected;

    // この clip が他クリップと overlap してクロスフェードになっている場合、
    // 該当側のフェードは drawCrossfadeOverlay が描くので per-clip では描かない
    // (二重線の防止)
    // ただし片方/両方が「意図的でない小さなデフォルトフェード」の場合は
    // クロスフェード扱いせず per-clip で個別描画する
    constexpr double kXfadeMinSecs = kCrossfadeFadeMinSecs;
    bool overlapFadeIn  = false;
    bool overlapFadeOut = false;
    // 所有レーンは呼び出し側 (drawTrackRows) が既知のため、全プロジェクト走査せず
    // ownerLane->clips を直接見る。
    if (ownerLane != nullptr)
    {
        const double cS = clip.getStartPosition();
        const double cE = clip.getEndPosition();
        for (auto& cPtr : ownerLane->clips)
        {
            auto* o = cPtr.get();
            if (o == &clip) continue;
            // overlay (drawCrossfadeOverlay) が同一連続音声でも X を描くようになったため、per-clip 側も
            // 通常通り抑制する (overlay と二重に三角を描かない)。Alt+Click 分割は overlap=0 で
            // 下の ov<=0.001 が弾くので、それらの個別フェード描画には影響しない。
            const double oS = o->getStartPosition();
            const double oE = o->getEndPosition();
            const double ov = juce::jmin(cE, oE) - juce::jmax(cS, oS);
            if (ov <= 0.001) continue;
            if (oS < cS)
            {
                // 左隣 overlap: c.fadeIn と o.fadeOut のどちらかが閾値超なら抑制
                if (clip.getFadeInSecs() >= kXfadeMinSecs
                    || o->getFadeOutSecs() >= kXfadeMinSecs)
                    overlapFadeIn = true;
            }
            else
            {
                if (clip.getFadeOutSecs() >= kXfadeMinSecs
                    || o->getFadeInSecs() >= kXfadeMinSecs)
                    overlapFadeOut = true;
            }
        }
    }

    if (!suppressFades && !overlapFadeIn && clip.getFadeInSecs() > 0.0)
    {
        const double bps = bpm / 60.0;
        float fadeW = (float)(clip.getFadeInSecs() * bps * pixelsPerBeat);
        fadeW = juce::jmin(fadeW, (float)clipRect.getWidth());
        drawFadeCurve((float)clipRect.getX(), fadeW, /*isFadeIn=*/true, clip.getFadeInCurve());
    }
    if (!suppressFades && !overlapFadeOut && clip.getFadeOutSecs() > 0.0)
    {
        const double bps = bpm / 60.0;
        float fadeW = (float)(clip.getFadeOutSecs() * bps * pixelsPerBeat);
        fadeW = juce::jmin(fadeW, (float)clipRect.getWidth());
        drawFadeCurve((float)clipRect.getRight() - fadeW, fadeW, /*isFadeIn=*/false, clip.getFadeOutCurve());
    }
    // ── フェードハンドル（クリップ上部角の三角ハンドル） ──
    // ドラッグ中は非選択クリップのハンドルも非表示 (微調整視認性のため)
    // ホバー時のみ表示 (多数クリップでの常時表示が視認性ノイズになるため非ホバーは描かない):
    //   ・通常             → 非表示
    //   ・クリップにホバー  → 両方の三角がはっきり
    //   ・三角の当たり判定上 → その三角だけ最大強調 (大きく+縁取り、掴めることを明示)
    if (!suppressFades)
    {
        const double bps2    = bpm / 60.0;
        const float  top     = (float)clipRect.getY() + 1.0f;
        const bool   clipHot = (&clip == hoveredClip);

        auto drawHandle = [&](float cx, bool isFadeIn, bool handleHot)
        {
            // 非ホバー時は描かない (handleHot は clipHot を含意するので clipHot 判定で足りる)
            if (!clipHot && !handleHot) return;
            const float S     = handleHot ? 11.0f : 9.0f;   // 当たり判定上は少し大きく
            const float fillA = handleHot ? 1.0f : 0.9f;
            juce::Path tri;
            if (isFadeIn)
            {
                // 左上角: 直角を境界上端に置き、クリップ内側 (右下) を指す
                tri.addTriangle(cx,     top,
                                cx + S, top,
                                cx,     top + S);
            }
            else
            {
                // 右上角: 直角を境界上端に置き、クリップ内側 (左下) を指す
                tri.addTriangle(cx,     top,
                                cx - S, top,
                                cx,     top + S);
            }
            g.setColour(juce::Colours::white.withAlpha(fillA));
            g.fillPath(tri);
            if (handleHot || clipHot)
            {
                g.setColour(juce::Colours::black.withAlpha(handleHot ? 0.5f : 0.35f));
                g.strokePath(tri, juce::PathStrokeType(1.0f));
            }
        };

        const bool fadeInHot  = clipHot && hoveredHandle == DragMode::FadeIn;
        const bool fadeOutHot = clipHot && hoveredHandle == DragMode::FadeOut;

        // FadeIn ハンドル（フェードイン境界位置）
        // 位置は drawFadeCurve と同じ float 演算で求め、曲線とハンドルを 1px もずらさない
        {
            float fiPx = (float)(clip.getFadeInSecs() * bps2 * pixelsPerBeat);
            float fiCX = juce::jlimit((float)clipRect.getX(), (float)clipRect.getRight(),
                                      (float)clipRect.getX() + fiPx);
            drawHandle(fiCX, /*isFadeIn=*/true, fadeInHot);
        }

        // FadeOut ハンドル（フェードアウト開始位置）
        {
            float foPx = (float)(clip.getFadeOutSecs() * bps2 * pixelsPerBeat);
            float foCX = juce::jlimit((float)clipRect.getX(), (float)clipRect.getRight(),
                                      (float)clipRect.getRight() - foPx);
            drawHandle(foCX, /*isFadeIn=*/false, fadeOutHot);
        }
    }

    // ── クリップゲインライン（GAIN ON のときのみ表示） ──
    if (appSettings.showClipGain)
    {
        const double bps2 = bpm / 60.0;
        const float baseDB = juce::Decibels::gainToDecibels(clip.getGain(), -60.0f);
        const auto& pts    = clip.getGainPoints();
        const int   midY   = clipRect.getCentreY();
        const int   halfH  = clipRect.getHeight() / 2 - 2;

        // 0dB を中央、+12dB を上、-60dB（無音）を下にマップ（非対称）
        auto dbToY = [&](float dB) {
            float norm;
            if (dB >= 0.0f) norm = juce::jlimit(0.0f, 1.0f, dB / 12.0f);
            else            norm = juce::jlimit(-1.0f, 0.0f, dB / 60.0f);
            return midY - (int)(norm * halfH);
        };
        auto timeToX = [&](double t) {
            return clipRect.getX() + (int)(t * bps2 * pixelsPerBeat);
        };

        g.setColour(juce::Colour(0xffffaa44).withAlpha(0.85f));
        if (pts.empty())
        {
            // ポイントなし: 静的ゲインの水平線
            int y = dbToY(baseDB);
            g.drawHorizontalLine(y, (float)(clipRect.getX() + 4), (float)(clipRect.getRight() - 4));
        }
        else
        {
            // エンベロープ折れ線
            int prevX = clipRect.getX();
            int prevY = dbToY(pts.front().dB);
            for (size_t i = 0; i < pts.size(); ++i)
            {
                int x = juce::jlimit(clipRect.getX(), clipRect.getRight(), timeToX(pts[i].time));
                int y = dbToY(pts[i].dB);
                if (i == 0)
                {
                    g.drawLine((float)clipRect.getX(), (float)y, (float)x, (float)y, 1.5f);
                }
                else
                {
                    g.drawLine((float)prevX, (float)prevY, (float)x, (float)y, 1.5f);
                }
                prevX = x; prevY = y;
            }
            g.drawLine((float)prevX, (float)prevY,
                       (float)clipRect.getRight(), (float)prevY, 1.5f);
            // ポイントマーカー
            for (auto& p : pts)
            {
                int x = juce::jlimit(clipRect.getX(), clipRect.getRight(), timeToX(p.time));
                int y = dbToY(p.dB);
                g.fillEllipse((float)(x - 4), (float)(y - 4), 8.0f, 8.0f);
            }
        }
    }

    // ── 左下: ゲインの dB 表示（小さなウィジェット、常時表示） ──
    // 多数のクリップに常時出るため、波形と同系の控えめな色にして煩わしさを抑える
    // (旧: 明るいオレンジで浮いていた)。クリップゲインの操作対象でもあるので表示自体は残す。
    {
        float gDB = juce::Decibels::gainToDecibels(clip.getGain(), -60.0f);
        juce::Rectangle<int> gWidget {
            clipRect.getX() + 4, clipRect.getBottom() - 14, 56, 12
        };
        if (clipRect.getWidth() >= 70 && clipRect.getHeight() >= 30)
        {
            // 背景ボックスは薄め (旧 0.55 → 0.38) にして視認ノイズを減らす
            g.setColour(juce::Colours::black.withAlpha(0.38f));
            g.fillRoundedRectangle(gWidget.toFloat(), 2.0f);
            // 文字色はそのクリップの波形と同系色 (trackColour ベース)。既定 (0 dB) は更に控えめ、
            // 実際にゲイン調整された時だけ少しはっきりさせて変更に気付けるようにする。
            const bool isDefault = (gDB > -0.05f && gDB < 0.05f);
            g.setColour(trackColour.brighter(0.9f).withAlpha(isDefault ? 0.55f : 0.95f));
            g.setFont(sharedTimeFont());
            juce::String label = juce::String::formatted("%+.1f dB", gDB);
            g.drawText(label, gWidget, juce::Justification::centred);
        }
    }

    // ── リサイズグリップ（左右端の縦線 2 本） ──
    if (clipRect.getWidth() >= 20)
    {
        const int gripH = juce::jmin(20, clipRect.getHeight() / 2);
        const int gripY = clipRect.getCentreY() - gripH / 2;
        g.setColour(juce::Colours::white.withAlpha(0.45f));
        // 左端グリップ
        g.drawLine((float)(clipRect.getX() + 3), (float)gripY,
                   (float)(clipRect.getX() + 3), (float)(gripY + gripH), 1.5f);
        g.drawLine((float)(clipRect.getX() + 5), (float)gripY,
                   (float)(clipRect.getX() + 5), (float)(gripY + gripH), 1.5f);
        // 右端グリップ
        g.drawLine((float)(clipRect.getRight() - 5), (float)gripY,
                   (float)(clipRect.getRight() - 5), (float)(gripY + gripH), 1.5f);
        g.drawLine((float)(clipRect.getRight() - 3), (float)gripY,
                   (float)(clipRect.getRight() - 3), (float)(gripY + gripH), 1.5f);
    }

    // クリップ名（テキストキャッシュで blit）— 名前領域がクリップ範囲と交差する時だけ
    {
        const auto nameRect = clipRect.withTrimmedLeft(8).withTrimmedTop(1).withHeight(juce::jmin(14, clipRect.getHeight()));
        if (nameRect.intersects(g.getClipBounds()))
        {
            TextImageCache::getInstance().drawText(g, clip.getName(), nameRect,
                sharedClipNameFont(), juce::Colours::white.withAlpha(0.9f),
                juce::Justification::topLeft);
        }
    }

    // クリップ枠線（選択中はオレンジ）
    g.setColour(isSelected ? juce::Colour(0xffff8800).withAlpha(0.9f)
                           : trackColour.brighter(0.4f).withAlpha(0.8f));
    g.drawRoundedRectangle(clipRect.toFloat(), 2.0f, isSelected ? 2.0f : 1.0f);

    // スマートツールゾーン区切り（薄い水平線、選択中のクリップでより目立たせる）
    if (clipRect.getHeight() >= 30)
    {
        int midY = clipRect.getCentreY();
        g.setColour(juce::Colours::white.withAlpha(isSelected ? 0.18f : 0.08f));
        for (float x = (float)clipRect.getX() + 4; x < (float)clipRect.getRight() - 4; x += 6.0f)
            g.drawLine(x, (float)midY, x + 3.0f, (float)midY, 0.6f);
    }

    g.restoreState();
    // ── クリップ終わり ──
}

void TimelineView::drawClipWaveform(juce::Graphics& g, AudioClip& clip, juce::Rectangle<int> area,
                                    double fileStart, double durationSecs, float verticalZoom,
                                    juce::Colour colour)
{
    auto& thumb = clip.getThumbnail();
    const int numCh = juce::jmax(1, thumb.getNumChannels());
    const int w = area.getWidth();
    const int h = area.getHeight();
    if (w <= 0 || h <= 0 || durationSecs <= 0.0) return;

    // 計算するのは可視範囲の列だけ (巨大クリップを g 直描画するときも画面幅で頭打ちになる。
    // キャッシュ Image へ描くときは ig のクリップ = Image 全体なので全列が対象)
    const auto clipB = g.getClipBounds();
    const int cx0 = juce::jmax(area.getX(),     clipB.getX());
    const int cx1 = juce::jmin(area.getRight(), clipB.getRight());
    if (cx0 >= cx1) return;
    const int cols = cx1 - cx0;

    g.setColour(colour);
    auto& top = waveTopScratch;
    auto& bot = waveBotScratch;
    top.resize((size_t) cols);
    bot.resize((size_t) cols);

    for (int ch = 0; ch < numCh; ++ch)
    {
        // drawChannels と同じ等分割ジオメトリ。正規化値 [-1,1] → ピクセルは
        // drawChannels の vscale (= vz * stripH / 256 を int8 値[-128..127]に掛ける) と等価:
        //   offset = v * vz * stripH / 2
        const float topY  = (float)(area.getY() + juce::roundToInt((double)(ch       * h) / numCh));
        const float botY  = (float)(area.getY() + juce::roundToInt((double)((ch + 1) * h) / numCh));
        const float midY  = (topY + botY) * 0.5f;
        const float scale = verticalZoom * (botY - topY) * 0.5f;

        for (int i = 0; i < cols; ++i)
        {
            const int    x  = cx0 + i;
            const double t0 = fileStart + ((double)(x - area.getX())       / w) * durationSecs;
            const double t1 = fileStart + ((double)(x - area.getX() + 1.0) / w) * durationSecs;
            float mn = 0.0f, mx = 0.0f;
            thumb.getApproximateMinMax(t0, t1, ch, mn, mx);
            top[(size_t) i] = juce::jlimit(topY, botY, midY - mx * scale);
            bot[(size_t) i] = juce::jlimit(topY, botY, midY - mn * scale);
        }

        // 塗りつぶしシルエット: 上辺 (max) を左→右、下辺 (min) を右→左で閉じる。
        // フロート Path なのでフチがアンチエイリアスされ、櫛状のギザギザが消える
        juce::Path silhouette;
        silhouette.startNewSubPath((float) cx0, top[0]);
        for (int i = 1; i < cols; ++i)
            silhouette.lineTo((float)(cx0 + i), top[(size_t) i]);
        for (int i = cols - 1; i >= 0; --i)
            silhouette.lineTo((float)(cx0 + i), bot[(size_t) i]);
        silhouette.closeSubPath();
        g.fillPath(silhouette);
    }
}

void TimelineView::drawClipSamples(juce::Graphics& g, AudioClip& clip,
                                    juce::Rectangle<int> wfRect,
                                    juce::Colour wfColour,
                                    float verticalZoom)
{
    auto* reader = clip.getOrCreateReader();
    if (reader == nullptr || reader->sampleRate <= 0.0) return;

    const double sr        = reader->sampleRate;
    const double bps_      = bpm / 60.0;
    const double pxPerSec  = pixelsPerBeat * bps_;
    if (pxPerSec <= 0.0) return;

    // 可視範囲（クリップ範囲 ∩ クリップ Bounds の交差）に絞ってサンプル読み出し
    const auto clipBounds = g.getClipBounds();
    const int x0 = juce::jmax(wfRect.getX(),     clipBounds.getX());
    const int x1 = juce::jmin(wfRect.getRight(), clipBounds.getRight());
    if (x0 >= x1) return;

    // 画面 x → プロジェクト時刻 → ファイル内サンプル
    const double clipStartProj = clip.getStartPosition();
    auto xToFileSample = [&](double px) -> double {
        const double timeProj = (px + scrollX) / pxPerSec;
        const double timeClip = timeProj - clipStartProj;
        const double timeFile = clip.getFileOffset() + timeClip;
        return timeFile * sr;
    };

    const juce::int64 firstSample = juce::jmax((juce::int64) 0,
                                               (juce::int64) std::floor(xToFileSample((double)x0)));
    const juce::int64 lastSample  = juce::jmin(reader->lengthInSamples,
                                               (juce::int64) std::ceil(xToFileSample((double)x1)) + 1);
    int numSamples = (int)(lastSample - firstSample);
    // 安全上限（1 ペイントで読みすぎないように）
    numSamples = juce::jmin(numSamples, 1 << 16);
    if (numSamples <= 0) return;

    const int numCh = (int)juce::jmin((unsigned int)reader->numChannels, 2u);
    juce::AudioBuffer<float> buf(numCh, numSamples);
    buf.clear();
    reader->read(&buf, 0, numSamples, firstSample, true, true);

    const float  gainMul = clip.getGain();
    const int    h = wfRect.getHeight();
    const int    W = x1 - x0;
    const double samplesPerPixel = sr / pxPerSec;

    // ズームインで描画要素が段階的に増える (どの段階も「塗りつぶし」は維持するので、
    // ズームアウト側のサムネイル塗り (>=4 sample/px) と地続きで継ぎ目が出ない):
    //   常に     : 列ごと min/max の塗りつぶしシルエット
    //   < 2      : サンプルを繋ぐ線を上に重ねる
    //   < 1      : 線をカトマル-ロム曲線に (アナログ再構成風の丸み)
    //   < 0.4    : サンプル点も打つ
    const bool  doLine   = (samplesPerPixel < 2.0);
    const bool  useSpline = (samplesPerPixel < 1.0);
    const bool  doDots   = (samplesPerPixel < 0.4);
    const float strokeW  = (samplesPerPixel < 0.5) ? 1.4f : 1.0f;

    // サンプル番号 → 画面 x は線形 (s が 1 増えるごとに pxPerSec/sr px 進む)
    const double xFirst = (clipStartProj + ((double)firstSample / sr - clip.getFileOffset()))
                              * pxPerSec - scrollX;
    const double dxds   = pxPerSec / sr;

    auto& pmax = waveTopScratch;        // 列ごと max 値
    auto& pmin = waveBotScratch;        // 列ごと min 値
    auto& pts  = samplePtsScratch;      // サンプル点列 (線/曲線用)

    for (int ch = 0; ch < numCh; ++ch)
    {
        // drawClipWaveform (サムネイル塗り) と同一の strip ジオメトリ / 振幅スケール。
        // これを一致させないと 4 sample/px の境界で波形の高さが段差になる
        const float topY  = (float)(wfRect.getY() + juce::roundToInt((double)(ch       * h) / numCh));
        const float botY  = (float)(wfRect.getY() + juce::roundToInt((double)((ch + 1) * h) / numCh));
        const float midY  = (topY + botY) * 0.5f;
        const float scale = verticalZoom * (botY - topY) * 0.5f;
        const float* src  = buf.getReadPointer(ch);

        // ── 列ごと min/max を集計して塗りつぶしシルエットを描く ──
        pmax.assign((size_t) W, -2.0f);
        pmin.assign((size_t) W,  2.0f);
        for (int s = 0; s < numSamples; ++s)
        {
            const int idx = juce::roundToInt(xFirst + (double) s * dxds) - x0;
            if (idx < 0 || idx >= W) continue;
            const float v = juce::jlimit(-1.0f, 1.0f, src[s] * gainMul);
            if (v > pmax[(size_t) idx]) pmax[(size_t) idx] = v;
            if (v < pmin[(size_t) idx]) pmin[(size_t) idx] = v;
        }
        juce::Path body;
        float carryMax = 0.0f, carryMin = 0.0f;
        for (int i = 0; i < W; ++i)               // 上辺 (max) 左→右
        {
            float mx = pmax[(size_t) i];
            if (mx < -1.5f) mx = carryMax; else carryMax = mx;   // サンプルが無い列は隣を継ぐ
            const float y  = juce::jlimit(topY, botY, midY - mx * scale);
            const float fx = (float)(x0 + i);
            if (i == 0) body.startNewSubPath(fx, y);
            else        body.lineTo(fx, y);
        }
        for (int i = W - 1; i >= 0; --i)          // 下辺 (min) 右→左
        {
            float mn = pmin[(size_t) i];
            if (mn > 1.5f) mn = carryMin; else carryMin = mn;
            body.lineTo((float)(x0 + i), juce::jlimit(topY, botY, midY - mn * scale));
        }
        body.closeSubPath();
        g.setColour(wfColour);
        g.fillPath(body);

        // ── 拡大時: サンプルを繋ぐ線 (+ 点) を塗りの上に重ねて精密なトレースにする ──
        if (doLine)
        {
            pts.clear();
            pts.reserve((size_t) numSamples);
            for (int s = 0; s < numSamples; ++s)
            {
                const float x = (float)(xFirst + (double) s * dxds);
                const float v = juce::jlimit(-1.0f, 1.0f, src[s] * gainMul);
                pts.push_back({ x, midY - v * scale });
            }
            juce::Path p;
            const int n = (int) pts.size();
            if (n > 0)
            {
                p.startNewSubPath(pts[0]);
                if (useSpline && n >= 3)
                {
                    // カトマル-ロム: 各区間 i→i+1 の制御点を前後の点の接線から求める
                    for (int i = 0; i < n - 1; ++i)
                    {
                        const auto p0 = pts[(size_t) juce::jmax(0,     i - 1)];
                        const auto p1 = pts[(size_t) i];
                        const auto p2 = pts[(size_t) (i + 1)];
                        const auto p3 = pts[(size_t) juce::jmin(n - 1, i + 2)];
                        const juce::Point<float> c1 { p1.x + (p2.x - p0.x) / 6.0f,
                                                      p1.y + (p2.y - p0.y) / 6.0f };
                        const juce::Point<float> c2 { p2.x - (p3.x - p1.x) / 6.0f,
                                                      p2.y - (p3.y - p1.y) / 6.0f };
                        p.cubicTo(c1, c2, p2);
                    }
                }
                else
                {
                    for (int i = 1; i < n; ++i) p.lineTo(pts[(size_t) i]);
                }
            }
            g.setColour(wfColour);
            g.strokePath(p, juce::PathStrokeType(strokeW));

            if (doDots)
            {
                const float r = 2.0f;
                for (const auto& pt : pts)
                    g.fillEllipse(pt.x - r, pt.y - r, r * 2.0f, r * 2.0f);
            }
        }
    }
}

void TimelineView::drawCrossfadeOverlay(juce::Graphics& g, Lane* lane,
                                         juce::Rectangle<int> laneBounds)
{
    if (lane == nullptr || lane->clips.size() < 2) return;

    const double bps = bpm / 60.0;
    const int top    = laneBounds.getY() + 4;
    const int bottom = laneBounds.getBottom() - 4;
    const int height = bottom - top;
    if (height <= 0) return;

    // 開始位置順にソートし、隣接ペアだけを X 表示の対象にする (#L2)。全ペア O(n^2) で
    // 描くと 3 枚以上重ねたとき非隣接ペアにも X が出て、再生 (隣接境界のみ) と食い違う。
    std::vector<AudioClip*> sorted;
    for (auto& c : lane->clips) sorted.push_back(c.get());
    std::sort(sorted.begin(), sorted.end(),
              [](AudioClip* a, AudioClip* b){ return a->getStartPosition() < b->getStartPosition(); });

    auto secToX = [&](double t){ return (float)(t * bps * pixelsPerBeat - scrollX); };

    for (size_t i = 0; i + 1 < sorted.size(); ++i)
    {
        AudioClip* clipA = sorted[i];
        AudioClip* clipB = sorted[i + 1];

        const double aEnd    = clipA->getEndPosition();
        const double bStart  = clipB->getStartPosition();
        // 重なり領域は「両クリップが存在する範囲」= [bStart, min(aEnd, bEnd)]。clipB が clipA に
        // 内包される (bEnd < aEnd) ケースで aEnd まで X を伸ばすと、clipB が無い所まで X が
        // 描かれてしまう。Lane0 オーバーレイ (path #2) と同じく終端を min(aEnd,bEnd) に揃える。
        const double ovEnd   = juce::jmin(aEnd, clipB->getEndPosition());
        const double overlap = ovEnd - bStart;
        if (overlap < kOverlapEpsilonSecs) continue;
        // 同一連続音声でも、重なり (overlap>1ms) と ≥30ms フェードがあればクロスフェードとして描く。
        // テイクを同位置に差し込んだ際の透過クロスフェード (線形カーブ) もここで X 表示する。
        // Alt+Click 分割は overlap=0 のため直前の overlap<0.001 で既に除外済み。

        const double fOutA = clipA->getFadeOutSecs();
        const double fInB  = clipB->getFadeInSecs();
        // 小さなデフォルトフェードは X 字として描かない
        // (意図的にユーザーが作成したクロスフェードのみ X 表示)
        if (fOutA < kCrossfadeFadeMinSecs && fInB < kCrossfadeFadeMinSecs) continue;

        // 重なり領域の画面 x 範囲 (両端の縦線ハンドル/選択領域に使う)
        const int xL = (int)(bStart * bps * pixelsPerBeat - scrollX);
        const int xR = (int)(ovEnd  * bps * pixelsPerBeat - scrollX);
        if (xR <= xL) continue;

        // 行内にクリップ (縦方向に隣の行へ漏れないように)
        g.saveState();
        g.reduceClipRegion(laneBounds);

        // クロスフェードは「重なり領域 [bStart, aEnd]」全体に corner-to-corner で X を描く。
        // 左右のフェード長 (fOutA / fInB) が setFade*Secs の個別クランプ (duration*0.5) 等で
        // 多少ずれても、描画は常に重なり領域いっぱいの対称な X になり、綺麗な X 字を保てる
        // (初心者にとって分かりやすい)。ここに来るのは入口ゲート (>=30ms) を通った「意図的な
        // クロスフェード」だけ。縮小 (ズームアウト) で重なりが細くなると X が縦線化して白く
        // 目立つため、crossfadeWidthVis で幅が狭いほどアルファを大きく落として控えめにする。
        const float xOvL  = secToX(bStart);
        const float xOvR  = secToX(ovEnd);
        const float ovVis = crossfadeWidthVis(xOvR - xOvL);
        const int   ovSegs = juce::jmax(8, (int)((xOvR - xOvL) / 3.0f));

        if (fOutA > 0.001 && ovVis > 0.001f)
        {
            juce::Path pa;
            for (int s = 0; s <= ovSegs; ++s)
            {
                const float t = (float)s / (float)ovSegs;   // 0(重なり先頭) → 1(末尾)
                const float gain = juce::jlimit(0.0f, 1.0f,
                    AudioClip::applyFadeCurve(1.0f - t, clipA->getFadeOutCurve()));
                const float x = xOvL + t * (xOvR - xOvL);
                const float y = (float)bottom - gain * (float)height;
                if (s == 0) pa.startNewSubPath(x, y);
                else        pa.lineTo(x, y);
            }
            g.setColour(juce::Colours::white.withAlpha(0.42f * ovVis));
            g.strokePath(pa, juce::PathStrokeType(1.4f));
        }

        if (fInB > 0.001 && ovVis > 0.001f)
        {
            juce::Path pb;
            for (int s = 0; s <= ovSegs; ++s)
            {
                const float t = (float)s / (float)ovSegs;   // 0(重なり先頭) → 1(末尾)
                const float gain = juce::jlimit(0.0f, 1.0f,
                    AudioClip::applyFadeCurve(t, clipB->getFadeInCurve()));
                const float x = xOvL + t * (xOvR - xOvL);
                const float y = (float)bottom - gain * (float)height;
                if (s == 0) pb.startNewSubPath(x, y);
                else        pb.lineTo(x, y);
            }
            g.setColour(juce::Colours::white.withAlpha(0.42f * ovVis));
            g.strokePath(pb, juce::PathStrokeType(1.4f));
        }

        g.restoreState();

        // ── 両端の細い縦線ハンドル (重なり領域の両端。選択時は強調) ──
        const bool selected = (selectedCrossfade.valid()
                               && selectedCrossfade.clipA == clipA
                               && selectedCrossfade.clipB == clipB);
        juce::Colour handleColour = selected
                                      ? juce::Colour(0xffff8800).withAlpha(0.95f)
                                      : juce::Colours::white.withAlpha(0.22f * ovVis);
        const float lineW = selected ? 2.0f : 1.0f;
        g.setColour(handleColour);
        g.drawLine((float)xL, (float)(laneBounds.getY() + 2),
                   (float)xL, (float)(laneBounds.getBottom() - 2), lineW);
        g.drawLine((float)xR, (float)(laneBounds.getY() + 2),
                   (float)xR, (float)(laneBounds.getBottom() - 2), lineW);
    }
}

void TimelineView::drawMidiClip(juce::Graphics& g, MidiClip& clip,
                                juce::Rectangle<int> laneBounds,
                                juce::Colour trackColour)
{
    const double bps = bpm / 60.0;
    const int cx = (int)(clip.getStartPosition() * bps * pixelsPerBeat - scrollX);
    const int cw = juce::jmax(4, (int)(clip.getDuration() * bps * pixelsPerBeat));
    if (cx + cw < laneBounds.getX() || cx > laneBounds.getRight()) return;

    juce::Rectangle<int> r { cx, laneBounds.getY() + 1, cw, laneBounds.getHeight() - 2 };

    // 背景: トラック色を薄く重ねる
    g.setColour(trackColour.withAlpha(0.25f));
    g.fillRoundedRectangle(r.toFloat(), 2.0f);
    const bool isSelected = (&clip == selectedMidiClip);
    if (isSelected)
    {
        // 選択中: 明るい縁取り + 内側ハイライト
        g.setColour(juce::Colours::white.withAlpha(0.12f));
        g.fillRoundedRectangle(r.toFloat(), 2.0f);
        g.setColour(juce::Colours::white.withAlpha(0.95f));
        g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 2.0f, 2.0f);
    }
    else
    {
        g.setColour(trackColour.withAlpha(0.7f));
        g.drawRoundedRectangle(r.toFloat(), 2.0f, 1.0f);
    }

    // ヘッダ: クリップ名 + "MIDI" ラベル（テキストキャッシュで blit）
    const int headerH = juce::jmin(14, r.getHeight() / 4);
    if (headerH >= 10)
    {
        g.setColour(trackColour.withAlpha(0.5f));
        g.fillRect(r.getX(), r.getY(), r.getWidth(), headerH);
        const juce::Rectangle<int> nameRect { r.getX() + 4, r.getY(), r.getWidth() - 8, headerH };
        if (nameRect.intersects(g.getClipBounds()))
        {
            const juce::String label = clip.getName().isEmpty() ? juce::String("MIDI") : clip.getName();
            TextImageCache::getInstance().drawText(g, label, nameRect,
                sharedClipNameFont(), juce::Colours::white.withAlpha(0.85f),
                juce::Justification::centredLeft);
        }
    }

    // ノート描画（ピアノロール風）
    const auto noteArea = r.withTrimmedTop(headerH + 1).reduced(2, 2);
    if (noteArea.getHeight() < 8 || noteArea.getWidth() < 8) return;

    int minN, maxN;
    clip.getNoteRange(minN, maxN);
    if (maxN <= minN) maxN = minN + 1;
    const int noteRange = maxN - minN + 1;

    const auto& seq = clip.getSequence();
    const double clipDur = juce::jmax(0.001, clip.getDuration());

    // 描画可視 X 範囲（部分 repaint / クリップ範囲外の note を排除）
    const auto gClip = g.getClipBounds();
    const float visL = (float)gClip.getX();
    const float visR = (float)gClip.getRight();

    g.setColour(juce::Colours::white.withAlpha(0.85f));
    for (int i = 0; i < seq.getNumEvents(); ++i)
    {
        auto* ev = seq.getEventPointer(i);
        const auto& m = ev->message;
        if (!m.isNoteOn()) continue;

        // クリップ先頭からの時刻
        const double t0 = m.getTimeStamp();
        double t1 = t0 + 0.1;
        auto* off = ev->noteOffObject;
        if (off) t1 = off->message.getTimeStamp();

        const float x0 = (float)noteArea.getX() + (float)(t0 / clipDur) * noteArea.getWidth();
        const float x1 = (float)noteArea.getX() + (float)(juce::jmin(t1, clipDur) / clipDur) * noteArea.getWidth();
        // シーケンスは時刻順なので x0 が可視右端を越えたら以降全部スキップ
        if (x0 > visR) break;
        if (x1 < visL) continue;
        const float w  = juce::jmax(1.0f, x1 - x0);

        const int notePos = juce::jlimit(0, noteRange, maxN - m.getNoteNumber());
        const float y = (float)noteArea.getY() + (float)notePos / (float)noteRange * (float)noteArea.getHeight();
        const float h = juce::jmax(1.0f, (float)noteArea.getHeight() / (float)noteRange - 0.5f);

        g.fillRect(juce::Rectangle<float>(x0, y, w, h));
    }
}

//==============================================================================
void TimelineView::drawTrackRows(juce::Graphics& g, juce::Rectangle<int> area)
{
    const int trackCount = trackManager.getTrackCount();

    if (trackCount == 0)
    {
        g.setColour(AppColours::textDim);
        g.setFont(juce::FontOptions(13.0f));
        g.drawText("Add a track to get started.", area, juce::Justification::centred);
        return;
    }

    // クリップ境界（部分 repaint 時のカリング用）
    const auto clipBounds = g.getClipBounds();
    const int clipL = clipBounds.getX();
    const int clipR = clipBounds.getRight();
    const int clipT = clipBounds.getY();
    const int clipB = clipBounds.getBottom();

    // ── 背景グリッド線の X 座標を一度だけ算出 ──
    // GRID が Off のときは空 (線を一切描かない = 背景すっきり)。GRID オン時のみ、小節線
    // (アンカー) と GRID 設定値の間隔の細線の X を集める。小節/拍は拍子・BPM 変更を反映。
    // ここでは描画せず、各トラックの背景塗りの後 (クリップの下) に描く。先に 1 回描いて
    // しまうと、後段の不透明なトラック背景 fillRect に覆われて一部トラックで消えるため。
    std::vector<float>& barLineXs  = gridBarXScratch;   // 小節線 (明るめ)。毎 paint 確保を避けメンバ再利用
    std::vector<float>& gridLineXs = gridLineXScratch;  // GRID 単位の細線 (暗め)
    barLineXs.clear();
    gridLineXs.clear();
    if (pixelsPerBeat > 0.5 && appSettings.snapMode != SnapMode::Off)
    {
        const double pxPerSec = pixelsPerBeat * (bpm / 60.0);
        const double spb = 60.0 / juce::jmax(1.0, bpm);
        // グリッド単位を「拍数」で表す (音価なので BPM に依らず一定。Bar=4 / Quarter=1 / Eighth=0.5 …)。
        const double gridUnitBeats = snapModeUnitSecs(appSettings.snapMode, bpm) / juce::jmax(1e-9, spb);
        // サブグリッドを集めるか。Bar スナップは小節線そのものがグリッドなので細線は出さない
        // (Bar の gridUnitBeats は 4 固定で、6/4・8/4 等では小節中央に無意味な線が出てしまうため)。
        // 画面間隔が狭すぎ (密集して塗りに見える) る場合も小節線のみにする。
        const bool collectSubGrid = gridUnitBeats > 1e-6
                                 && appSettings.snapMode != SnapMode::Bar
                                 && (gridUnitBeats * pixelsPerBeat) >= 6.0;

        int   bar = 1;
        double t = 0.0;
        const int W = area.getRight();
        std::vector<double> beatBounds;  // 小節内の各拍境界の時刻 (size n+1)。ヒープ確保はループ外で 1 回
        while (true)
        {
            int n, d; ruler.getMeterAtBar1(bar, n, d);
            n = juce::jmax(1, n);
            double barStart = t;
            beatBounds.clear();
            beatBounds.push_back(t);
            for (int beat = 0; beat < n; ++beat)
            {
                double bp = ruler.bpmAt(t);
                t += 60.0 / juce::jmax(1.0, bp);
                beatBounds.push_back(t);
            }
            double x1 = area.getX() + barStart * pxPerSec - scrollX;
            double x2 = area.getX() + t * pxPerSec - scrollX;
            if (x2 < area.getX()) { ++bar; if (bar > 100000) break; continue; }
            if (x1 > W) break;
            // クリップ範囲外の bar はスキップ
            if (x2 < clipL || x1 > clipR) { ++bar; if (bar > 100000) break; continue; }

            barLineXs.push_back((float)x1);

            // GRID 設定値の間隔の細線 (小節頭 p=0 は小節線が担うので gridUnitBeats から)
            if (collectSubGrid)
            {
                for (double p = gridUnitBeats; p < (double)n - 1e-6; p += gridUnitBeats)
                {
                    const int j = (int)p;                 // 拍インデックス
                    if (j >= (int)beatBounds.size() - 1) break;
                    const double frac = p - (double)j;    // 拍内の位置 (拍内は等速補間)
                    const double tt = beatBounds[j] + frac * (beatBounds[j + 1] - beatBounds[j]);
                    const float bx = (float)(area.getX() + tt * pxPerSec - scrollX);
                    if (bx < clipL || bx > clipR) continue;
                    gridLineXs.push_back(bx);
                }
            }

            ++bar;
            if (bar > 100000) break;
        }
    }

    // ── 背景グリッド線 (全トラック共通・1 パスで全高描画) ──
    // 以前は per-track に描いていたが、描画コストがトラック数倍になり高速スクロールで
    // カクついた。下のトラック背景 fillRect (fillAll と同色で冗長) を除去したので、ここで
    // fillAll の上に 1 回だけ全高描画する (クリップは後段で上書き = クリップのある所は従来どおり隠れる)。
    // 縦範囲は可視コンテンツ上端〜最終トラック下端 (トラックの無い余白には引かない)。
    if (!barLineXs.empty() || !gridLineXs.empty())
    {
        const int lastIdx = trackCount - 1;
        const int tracksBottom = area.getY()
                               + trackManager.getTrackY(lastIdx)
                               + trackManager.getTrack(lastIdx)->getTotalHeight() - scrollY;
        const float gy0 = (float) area.getY();
        const float gy1 = (float) juce::jmin(area.getBottom(), tracksBottom);
        if (gy1 > gy0)
        {
            g.setColour(AppColours::rulerLine.withAlpha(0.08f));
            for (const float gx : gridLineXs) g.drawLine(gx, gy0, gx, gy1, 1.0f);
            g.setColour(AppColours::rulerLineBright.withAlpha(0.15f));
            for (const float gx : barLineXs)  g.drawLine(gx, gy0, gx, gy1, 1.0f);
        }
    }

    // ── 各トラック ──
    for (int ti = 0; ti < trackCount; ++ti)
    {
        auto* track    = trackManager.getTrack(ti);
        int   trackTop = area.getY() + trackManager.getTrackY(ti) - scrollY;
        int   trackH   = track->getTotalHeight();
        int   trackBot = trackTop + trackH;

        if (trackBot < area.getY() || trackTop > area.getBottom()) continue;
        // クリップ範囲外のトラックはスキップ（部分 repaint 時の高速化）
        if (trackBot < clipT || trackTop > clipB) continue;

        juce::Rectangle<int> trackBounds { area.getX(), trackTop, area.getWidth(), trackH };

        // トラック背景は paint() の fillAll(trackBg) で既に塗られているため per-track の不透明塗りは
        // 不要 (冗長・per-track 塗りはグリッドを覆い隠す原因だった)。録音アーム中だけ赤みを重ねる。
        if (track->isRecArmed())
        {
            g.setColour(AppColours::recRed.withAlpha(0.06f));
            g.fillRect(trackBounds);
        }

        // ── レーンごとに描画（非等分: Lane0=defaultHeight, Lane1以降=laneHeight）──
        const int laneCount    = track->getLaneCount();
        const bool collapsed   = track->isLanesCollapsed();
        // 折りたたみ時は Lane 0 のみ、全高を使って描画
        const int visibleLanes = collapsed ? 1 : laneCount;

        const int trackMainH = track->getMainHeight();
        const int trackLaneH = track->getLaneHeight();
        for (int li = 0; li < visibleLanes; ++li)
        {
            int thisLaneTop = trackTop + (li == 0 ? 0
                              : trackMainH + (li - 1) * trackLaneH);
            int thisLaneH   = (collapsed)       ? trackH
                            : (li == 0)         ? juce::jmin(trackMainH, trackH)
                                                : trackLaneH;

            juce::Rectangle<int> laneBounds {
                area.getX(), thisLaneTop,
                area.getWidth(), thisLaneH
            };

            // レーン行の交互背景
            if (li % 2 == 1)
            {
                g.setColour(juce::Colours::white.withAlpha(0.025f));
                g.fillRect(laneBounds);
            }

            const bool isLiveLane = track->hasLiveRecording()
                                    && (li == track->getLiveRecordingLaneIndex());

            // クリップ描画
            {
                auto* lane = track->getLane(li);
                if (lane && !lane->clips.empty())
                {
                    g.saveState();
                    g.reduceClipRegion(laneBounds);
                    const double bps = bpm / 60.0;
                    const bool hasOverlap = (li == 0 && lane->clips.size() > 1);

                    for (auto& clip : lane->clips)
                    {
                        if (hasOverlap)
                        {
                            // Lane 0 で重なりがある場合: 描画前にこのクリップの
                            // エリアをトラック背景色で消去 → 古いクリップを隠し
                            // 後から描く（＝新しい）クリップだけが見える
                            int cx = (int)(clip->getStartPosition() * bps * pixelsPerBeat - scrollX);
                            int cw = juce::jmax(4, (int)(clip->getDuration() * bps * pixelsPerBeat));
                            g.setColour(AppColours::trackBg);
                            g.fillRect(cx, laneBounds.getY(), cw, laneBounds.getHeight());
                        }
                        bool sel = isClipInSelection(clip.get());
                        drawClip(g, *clip, laneBounds, track->getColour(), sel, lane);
                    }
                    // 全クリップ描画後に重なり部分の X 字クロスフェードを上書き描画
                    // (ドラッグ中も表示してリアルタイムに overlap 位置を確認できる)
                    drawCrossfadeOverlay(g, lane, laneBounds);
                    g.restoreState();
                }
            }

            // ── MIDI クリップ描画（Lane 0 のみ）──
            if (li == 0 && track->isMidiTrack() && track->getMidiClipCount() > 0)
            {
                g.saveState();
                g.reduceClipRegion(laneBounds);
                const double bps = bpm / 60.0;
                for (int ci = 0; ci < track->getMidiClipCount(); ++ci)
                {
                    auto* clip = track->getMidiClip(ci);
                    if (!clip) continue;
                    drawMidiClip(g, *clip, laneBounds, track->getColour());
                }
                g.restoreState();
            }

            if (isLiveLane)
            {
                // ── ライブ録音中の波形をオーバーレイ ──
                // バッファ先頭の lead (カウントイン/プリロールの遡及録音分) は表に出さず、
                // R 押下位置 (recordingStartPos) から lead 以降の内容だけを描く
                auto& lb = track->getLiveBuffer();

                const double lead = track->getLiveBufferLeadSecs();
                double startSecs = track->getRecordingStartPos();
                const double bufSecs = lb.getPeakCount() > 0
                                       ? lb.getDurationSeconds(sampleRate)
                                       : 0.0;
                const double durSecs = juce::jmax(0.0, bufSecs - lead);
                int liveX = (int)positionToX(startSecs);
                int liveW = durSecs > 0.0
                            ? juce::jmax(2, (int)(durSecs * (bpm / 60.0) * pixelsPerBeat))
                            : 2;

                juce::Rectangle<int> liveRect {
                    liveX, thisLaneTop + 1,
                    liveW, thisLaneH - 2
                };

                g.saveState();
                g.reduceClipRegion(laneBounds);

                // 録音中の半透明背景
                g.setColour(track->getColour().withAlpha(0.25f));
                g.fillRoundedRectangle(liveRect.toFloat(), 2.0f);

                // リアルタイム波形 (lead 以降のみ)
                if (lb.getPeakCount() > 0 && durSecs > 0.0)
                    lb.draw(g, liveRect.reduced(2, 4),
                            track->getColour().brighter(0.9f),
                            lead, durSecs, sampleRate);

                // 録音中を示す赤枠
                g.setColour(AppColours::recRed.withAlpha(0.9f));
                g.drawRoundedRectangle(liveRect.toFloat(), 2.0f, 1.5f);

                g.restoreState();
            }

            // ── レーンセパレーター（クリップの上に描画） ──
            if (li > 0)
            {
                g.setColour(AppColours::background);
                g.drawLine((float)area.getX(), (float)thisLaneTop,
                           (float)area.getRight(), (float)thisLaneTop, 2.0f);
                g.setColour(AppColours::separatorLight.withAlpha(0.6f));
                g.drawLine((float)area.getX(), (float)thisLaneTop,
                           (float)area.getRight(), (float)thisLaneTop, 1.0f);
                // 左端のインデント指示線（TrackHeaderViewと視覚的に対応）
                g.setColour(track->getColour().withAlpha(0.4f));
                g.fillRect(area.getX(), thisLaneTop + 1, 4, thisLaneH - 2);
            }
        }

        // ── クロスフェード視覚オーバーレイ（Lane 0） ──
        // autoCrossfade は「ドラッグ時に自動でクロスフェードを作るか」の設定であって、
        // 既存クロスフェードの表示可否ではない。X キー / テイク差し替えで作った
        // クロスフェード (≥30ms フェード) は設定 OFF でも必ず X を描く。各ペアの判定は
        // cursor 側の xfadeDrawn (TimelineView_Mouse) と同条件にする (#L3)。
        if (track->getLaneCount() > 0)
        {
            auto* lane0 = track->getLane(0);
            if (lane0 && lane0->clips.size() >= 2)
            {
                // clipStart 順にソートしたコピーで隣接ペアを検出
                std::vector<AudioClip*> sorted;
                for (auto& c : lane0->clips) sorted.push_back(c.get());
                std::sort(sorted.begin(), sorted.end(),
                          [](AudioClip* a, AudioClip* b){ return a->getStartPosition() < b->getStartPosition(); });

                const double bps     = bpm / 60.0;
                const double xfadeSecs = appSettings.crossfadeDuration;
                const int laneTop    = trackTop;
                const int laneH      = collapsed ? trackH : juce::jmin(track->getMainHeight(), trackH);

                g.saveState();
                g.reduceClipRegion(area.getX(), laneTop, area.getWidth(), laneH);

                for (size_t ci = 0; ci + 1 < sorted.size(); ++ci)
                {
                    auto* a = sorted[ci];
                    auto* b = sorted[ci + 1];

                    // 同一連続音声も、重なり (下の overlap>1ms) があればクロスフェードとして描く。
                    // Alt+Click 分割は overlap=0 のため下の overlap<0.001 でスキップされる。

                    // 実際に重なっている場合のみ（overlap > 1ms）
                    double overlapSecs = a->getEndPosition() - b->getStartPosition();
                    if (overlapSecs < kOverlapEpsilonSecs) continue;
                    // autoCrossfade OFF の「単なる重なり」(両フェードが小) には X を描かない。
                    // 実フェードのあるクロスフェードのみ表示 (cursor / mouseDown と同条件)。
                    if (!isCrossfadeInteractive(a, b)) continue;

                    // 重なり領域 [bxL, axR] が X の範囲
                    // ただし両クリップの実際の波形境界内にクランプする
                    int axL  = (int)(a->getStartPosition() * bps * pixelsPerBeat - scrollX);
                    int axR  = (int)(a->getEndPosition()   * bps * pixelsPerBeat - scrollX);
                    int bxL  = (int)(b->getStartPosition() * bps * pixelsPerBeat - scrollX);
                    int bxR  = (int)(b->getEndPosition()   * bps * pixelsPerBeat - scrollX);
                    int xLeft  = juce::jmax(bxL, axL);  // clip A が存在する範囲の左端
                    int xRight = juce::jmin(axR, bxR);  // clip B が存在する範囲の右端
                    if (xRight <= xLeft) continue;

                    // 重なり領域の暗化（対向グラデ 2 枚の合成 ≒ ほぼ均一な暗化を単色塗りで近似）。
                    // 目立ちすぎないよう控えめのアルファにする (波形が透ける程度)。
                    {
                        g.setColour(juce::Colours::black.withAlpha(0.16f));
                        g.fillRect(xLeft, laneTop, xRight - xLeft, laneH);
                    }

                    // クロスフェード選択状態の判定
                    bool xfadeSelected = (selectedCrossfade.valid()
                                          && selectedCrossfade.clipA == a
                                          && selectedCrossfade.clipB == b);

                    // 非選択時の X 字は drawCrossfadeOverlay のフェードカーブ (フェードと同じ
                    // 濃さ・太さ) に任せ、ここでは太い corner-to-corner X を重ねない (二重線で
                    // 目立ちすぎるため)。選択時のみ強調用にオレンジの X + 枠を描く。
                    if (xfadeSelected)
                    {
                        g.setColour(juce::Colour(0xffff8800).withAlpha(0.9f));
                        g.drawLine((float)xLeft,  (float)(laneTop + 2), (float)xRight, (float)(laneTop + laneH - 2), 2.0f);
                        g.drawLine((float)xRight, (float)(laneTop + 2), (float)xLeft,  (float)(laneTop + laneH - 2), 2.0f);
                        g.setColour(juce::Colour(0xffff8800).withAlpha(0.6f));
                        g.drawRect(juce::Rectangle<float>((float)xLeft, (float)(laneTop + 1),
                                                         (float)(xRight - xLeft), (float)(laneH - 2)), 1.5f);
                    }

                    // 両端のリサイズハンドル（縦バー）。非選択時は控えめの白（目立ちすぎ防止）。
                    // 縮小で重なりが細いほどさらに薄くし、白い縦線が目立たないようにする。
                    const float ovVis = crossfadeWidthVis((float)(xRight - xLeft));
                    g.setColour(xfadeSelected ? juce::Colour(0xffff8800).withAlpha(0.85f)
                                              : juce::Colours::white.withAlpha(0.16f * ovVis));
                    g.fillRect(xLeft - 2,  laneTop + 2, 3, laneH - 4);
                    g.fillRect(xRight - 1, laneTop + 2, 3, laneH - 4);
                }

                g.restoreState();
            }
        }

        // トラック下端ライン
        g.setColour(AppColours::separator);
        g.drawLine((float)area.getX(), (float)trackBot - 1.0f,
                   (float)area.getRight(), (float)trackBot - 1.0f, 1.0f);
    }

    // ── 選択範囲（波形上にも表示） ──
    if (loopEndTV > loopStartTV + 0.001)
    {
        const double bps = bpm / 60.0;
        int x1 = (int)(loopStartTV * bps * pixelsPerBeat - scrollX);
        int x2 = (int)(loopEndTV   * bps * pixelsPerBeat - scrollX);
        if (x2 > area.getX() && x1 < area.getRight())
        {
            x1 = juce::jmax(x1, area.getX());
            x2 = juce::jmin(x2, area.getRight());

            // フォーカスレーンが指定されている場合はそのレーンだけ強調
            int yStart = area.getY();
            int yHeight = area.getHeight();
            bool focused = (selectionFocusTrackIdx >= 0 && selectionFocusLaneIdx >= 0
                            && selectionFocusTrackIdx < trackManager.getTrackCount());
            if (focused)
            {
                auto* tr = trackManager.getTrack(selectionFocusTrackIdx);
                int trackTop = area.getY() + trackManager.getTrackY(selectionFocusTrackIdx) - scrollY;
                if (selectionFocusLaneIdx == 0)
                {
                    yStart = trackTop;
                    yHeight = tr->isLanesCollapsed() ? tr->getTotalHeight() : tr->getMainHeight();
                }
                else
                {
                    yStart = trackTop + tr->getMainHeight()
                             + (selectionFocusLaneIdx - 1) * tr->getLaneHeight();
                    yHeight = tr->getLaneHeight();
                }
            }
            float alphaFill = loopActiveTV ? 0.20f : 0.13f;
            g.setColour(juce::Colour(0xff5a8aaa).withAlpha(focused ? alphaFill * 1.4f : alphaFill));
            g.fillRect(x1, yStart, x2 - x1, yHeight);
            g.setColour(juce::Colour(0xff5a8aaa).withAlpha(0.7f));
            g.drawLine((float)x1, (float)yStart, (float)x1, (float)(yStart + yHeight), 1.0f);
            g.drawLine((float)x2, (float)yStart, (float)x2, (float)(yStart + yHeight), 1.0f);
        }
    }

    // ── プレイヘッドライン（全トラックを貫通） ──
    float phx = (float)positionToX(playheadSecs);
    if (phx >= 0 && phx <= area.getRight())
    {
        g.setColour(AppColours::playhead);
        g.drawLine(phx, (float)area.getY(), phx, (float)area.getBottom(), 2.0f);
    }
}
