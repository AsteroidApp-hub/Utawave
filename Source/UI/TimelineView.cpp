// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "TimelineView.h"
#include "../Localisation.h"
#include "../AppColours.h"
#include "GridSnapMath.h"
#include "../Tracks/MidiClip.h"
#include "../Edit/SilenceDetector.h"
#include "../Audio/BpmDetector.h"
#include "../Audio/LufsMeter.h"
#include "TextImageCache.h"
#include "RulerRangeMath.h"
#include <set>
#include <map>
#include <utility>

// 共有 Font プール（FontOptions の毎回構築 + キャッシュ検索コストを回避）
static const juce::Font& sharedBarFont()      { static const juce::Font f { juce::FontOptions(10.5f) }; return f; }
static const juce::Font& sharedTimeFont()     { static const juce::Font f { juce::FontOptions(9.5f) };  return f; }
static const juce::Font& sharedHeaderFont()   { static const juce::Font f { juce::FontOptions(10.0f, juce::Font::bold) }; return f; }
static const juce::Font& sharedClipNameFont() { static const juce::Font f { juce::FontOptions(10.0f) }; return f; }

//==============================================================================
TimelineRuler::TimelineRuler()
{
    setMouseCursor(juce::MouseCursor::IBeamCursor);
}

double TimelineRuler::xToSeconds(int x) const
{
    double beatsPerSec = currentBpm / 60.0;
    double posX = x + scrollX;
    return juce::jmax(0.0, posX / (pixelsPerBeat * beatsPerSec));
}

void TimelineRuler::getMeterAtBar1(int bar1, int& outNum, int& outDen) const
{
    outNum = meterNum;
    outDen = meterDen;
    int barIdx = bar1 - 1;
    for (auto& mc : meterChanges)
        if (mc.barIndex <= barIdx) { outNum = mc.numerator; outDen = mc.denominator; }
        else break;
}

double TimelineRuler::bpmAt(double t) const
{
    double cur = currentBpm;
    for (auto& bc : bpmChanges)
        if (bc.timeSec <= t) cur = bc.bpm;
        else break;
    return cur;
}

double TimelineRuler::barStartSecs(int bar1) const
{
    if (bar1 <= 1) return 0.0;
    double t = 0.0;
    for (int b = 1; b < bar1; ++b)
    {
        int n, d; getMeterAtBar1(b, n, d);
        n = juce::jmax(1, n);
        for (int beat = 0; beat < n; ++beat)
        {
            double bpm = bpmAt(t);
            t += 60.0 / juce::jmax(1.0, bpm);
        }
    }
    return t;
}

int TimelineRuler::hitTestBpmMarker(int x, int y) const
{
    if (y < 16 || y >= 30) return -1;  // Tempo 行範囲外
    double pxPerSec = pixelsPerBeat * (currentBpm / 60.0);
    for (size_t i = 0; i < bpmChanges.size(); ++i)
    {
        float mx = (float)(bpmChanges[i].timeSec * pxPerSec - scrollX);
        if (x >= mx - 2 && x <= mx + 50) return (int)i;
    }
    return -1;
}

int TimelineRuler::hitTestMeterMarker(int x, int y) const
{
    if (y < 30 || y >= 44) return -1;  // Meter 行範囲外
    double pxPerSec = pixelsPerBeat * (currentBpm / 60.0);
    for (size_t i = 0; i < meterChanges.size(); ++i)
    {
        double secs = barStartSecs(meterChanges[i].barIndex + 1);
        float mx = (float)(secs * pxPerSec - scrollX);
        if (x >= mx - 2 && x <= mx + 50) return (int)i;
    }
    return -1;
}

int TimelineRuler::barAtTime(double secs) const
{
    double t = 0.0;
    int bar = 1;
    while (true)
    {
        int n, d; getMeterAtBar1(bar, n, d);
        n = juce::jmax(1, n);
        for (int beat = 0; beat < n; ++beat)
        {
            double bpm = bpmAt(t);
            double next = t + 60.0 / juce::jmax(1.0, bpm);
            if (next > secs) return bar;
            t = next;
        }
        ++bar;
        if (bar > 100000) return bar;
    }
}

int TimelineRuler::xToBar1(int x) const
{
    return barAtTime(xToSeconds(x));
}

int TimelineRuler::hitTestMarker(int x, int y) const
{
    if (y > 16) return -1;
    double bps_ = currentBpm / 60.0;
    for (size_t i = 0; i < markers.size(); ++i)
    {
        float mx = (float)(markers[i].time * bps_ * pixelsPerBeat - scrollX);
        if (x >= mx - 1 && x <= mx + 14) return (int)i;
    }
    return -1;
}

void TimelineRuler::showMarkerContextMenu(int idx)
{
    if (idx < 0 || idx >= (int)markers.size()) return;
    juce::PopupMenu m;
    juce::PopupMenu colourMenu;
    static const std::array<std::pair<const char*, juce::Colour>, 8> palette = {{
        {"Orange", juce::Colour(0xffffaa44)},
        {"Red",    juce::Colour(0xffff5555)},
        {"Green",  juce::Colour(0xff55cc55)},
        {"Blue",   juce::Colour(0xff5599ff)},
        {"Yellow", juce::Colour(0xffffdd44)},
        {"Purple", juce::Colour(0xffaa66cc)},
        {"Pink",   juce::Colour(0xffff66aa)},
        {"Cyan",   juce::Colour(0xff44cccc)}
    }};
    for (size_t i = 0; i < palette.size(); ++i)
    {
        juce::PopupMenu::Item item;
        item.itemID = 100 + (int)i;
        item.text = palette[i].first;
        item.colour = palette[i].second;
        colourMenu.addItem(item);
    }
    m.addSubMenu(tr(u8"色"), colourMenu);
    m.addItem(200, tr(u8"名前を変更"));
    m.addSeparator();
    m.addItem(40, tr(u8"マーカーに色を表示"), true, useMarkerColors);
    m.addSeparator();
    m.addItem(300, tr(u8"このマーカーを削除"));

    m.showMenuAsync(juce::PopupMenu::Options(),
        [this, idx](int result) {
            if (idx >= (int)markers.size()) return;
            if (result >= 100 && result <= 107)
            {
                beginMusicEdit();
                markers[idx].colour = palette[result - 100].second;
                commitMusicEdit();
                repaint();
            }
            if (result == 200 && onEditMarkerName) onEditMarkerName(idx);
            if (result == 40 && onToggleMarkerColors) onToggleMarkerColors(!useMarkerColors);
            if (result == 300)
            {
                beginMusicEdit();
                markers.erase(markers.begin() + idx);
                commitMusicEdit();
                repaint();
            }
        });
}

void TimelineRuler::mouseDown(const juce::MouseEvent& e)
{
    // ── 右クリック (どの行でも有効): マーカー固有メニュー or 共通メニュー ──
    if (e.mods.isRightButtonDown())
    {
        // マーカー上ならマーカー専用メニュー
        const int hitMarkerR = hitTestMarker(e.x, e.y);
        if (hitMarkerR >= 0)
        {
            showMarkerContextMenu(hitMarkerR);
            return;
        }
        // Tempo マーカー上ならテンポ削除メニュー
        const int hitBpmR = hitTestBpmMarker(e.x, e.y);
        if (hitBpmR >= 0)
        {
            juce::PopupMenu m;
            m.addItem(1, tr(u8"このテンポ変更を削除"));
            m.showMenuAsync(juce::PopupMenu::Options(),
                [this, hitBpmR](int result) {
                    if (result == 1 && hitBpmR < (int)bpmChanges.size())
                    {
                        beginMusicEdit();
                        bpmChanges.erase(bpmChanges.begin() + hitBpmR);
                        if (onBpmChangesUpdated) onBpmChangesUpdated(bpmChanges);
                        commitMusicEdit();
                        repaint();
                    }
                });
            return;
        }
        // Meter マーカー上なら拍子削除メニュー
        const int hitMeterR = hitTestMeterMarker(e.x, e.y);
        if (hitMeterR >= 0)
        {
            juce::PopupMenu m;
            m.addItem(1, tr(u8"この拍子変更を削除"));
            m.showMenuAsync(juce::PopupMenu::Options(),
                [this, hitMeterR](int result) {
                    if (result == 1 && hitMeterR < (int)meterChanges.size())
                    {
                        beginMusicEdit();
                        meterChanges.erase(meterChanges.begin() + hitMeterR);
                        if (onMeterChangesUpdated) onMeterChangesUpdated(meterChanges);
                        commitMusicEdit();
                        repaint();
                    }
                });
            return;
        }
        // それ以外（Tempo / Meter / Bars / Time / Marker 空きエリア）→ 共通メニュー
        const double t = xToSeconds(e.x);
        juce::PopupMenu m;
        m.addItem(1, tr(u8"ここにマーカーを追加"));
        m.addSeparator();
        m.addItem(22, tr(u8"ループ範囲をクリア"));
        m.addItem(30, tr(u8"全マーカーをクリア"));
        m.addSeparator();
        m.addItem(50, tr(u8"小節を表示"),   /*enabled*/ true, /*ticked*/ barsRowVisible);
        m.addItem(51, tr(u8"小節を非表示"), /*enabled*/ true, /*ticked*/ !barsRowVisible);
        m.addSeparator();
        m.addItem(40, tr(u8"時刻を表示"),   /*enabled*/ true, /*ticked*/ timeRowVisible);
        m.addItem(41, tr(u8"時刻を非表示"), /*enabled*/ true, /*ticked*/ !timeRowVisible);
        m.showMenuAsync(juce::PopupMenu::Options(),
            [this, t](int result) {
                if (result == 1 && onAddMarker)     onAddMarker(t);
                if (result == 22 && onClearLoop)    onClearLoop();
                if (result == 30 && onClearMarkers) onClearMarkers();
                if (result == 40 || result == 41)
                {
                    const bool show = (result == 40);
                    setTimeRowVisible(show);
                    if (onTimeRowVisibilityChanged) onTimeRowVisibilityChanged(show);
                }
                if (result == 50 || result == 51)
                {
                    const bool show = (result == 50);
                    setBarsRowVisible(show);
                    if (onBarsRowVisibilityChanged) onBarsRowVisibilityChanged(show);
                }
            });
        return;
    }

    // Tempo 行 (y=16〜29): 既存マーカークリック→ドラッグ、Cmd+クリック→ダイアログ、
    // 空きエリアの通常左クリック→シーク
    if (e.y >= 16 && e.y < 30)
    {
        int hit = hitTestBpmMarker(e.x, e.y);
        if (hit >= 0 && !e.mods.isCommandDown() && !e.mods.isRightButtonDown())
        {
            draggingBpmIdx = hit;
            beginMusicEdit();
            return;
        }
        if (e.mods.isCommandDown())
        {
            double t = xToSeconds(e.x);
            if (onSnapTimeForBpm) t = onSnapTimeForBpm(t);
            if (onEditBpm) onEditBpm(t);
            return;
        }
        beginSeekDrag(e);
        return;
    }
    // Meter 行 (y=30〜44): 既存マーカークリック→ドラッグ、Cmd+クリック→ダイアログ、
    // 空きエリアの通常左クリック→シーク
    if (e.y >= 30 && e.y < 44)
    {
        int hit = hitTestMeterMarker(e.x, e.y);
        if (hit >= 0 && !e.mods.isCommandDown() && !e.mods.isRightButtonDown())
        {
            draggingMeterIdx = hit;
            beginMusicEdit();
            return;
        }
        if (e.mods.isCommandDown())
        {
            if (onEditMeter) onEditMeter(xToBar1(e.x));
            return;
        }
        beginSeekDrag(e);
        return;
    }

    // 1. マーカー上のクリック判定（左クリックのみここに来る）
    int hitMarker = hitTestMarker(e.x, e.y);
    if (hitMarker >= 0)
    {
        draggingMarkerIdx = hitMarker;
        markerDragStartX  = e.x;
        markerDragMoved   = false;
        beginMusicEdit();
        return;
    }

    // ルーラー範囲の端/中央を掴んだら調整ドラッグへ（Cmd は無し = マーカー追加優先のため除外）
    if (! e.mods.isCommandDown())
    {
        const int lh = hitTestLoopHandle(e.x, e.y);   // 1=左端 / 2=右端 のみ
        if (lh != 0)
        {
            draggingLoopHandle = lh;
            seekArmed          = false;
            return;
        }
    }

    // Cmd+クリック: マーカー追加
    if (e.mods.isCommandDown())
    {
        if (onAddMarker) onAddMarker(xToSeconds(e.x));
        return;
    }

    // 左クリック: ドラッグ開始位置を記録 + シーク（snap 適用）
    beginSeekDrag(e);
}

int TimelineRuler::hitTestLoopHandle(int x, int y) const
{
    // ルーラー範囲は小節バー行(hBars)だけに描かれるので、その帯だけで掴める。
    // 描画 (paint) と同じ行高定数 + 同じ timeToX を使い、見た目と掴み位置を一致させる。
    const int yBars = hMarkerRow + hTempoRow + hMeterRow;   // Bars 行の上端
    const int hBars = barsRowVisible ? hBarsRow : 0;
    const double lx1 = RulerRangeMath::timeToX(loopStart, currentBpm, pixelsPerBeat, scrollX);
    const double lx2 = RulerRangeMath::timeToX(loopEnd,   currentBpm, pixelsPerBeat, scrollX);
    return RulerRangeMath::loopHandleAt(lx1, lx2, x, y, yBars, hBars, 6.0);
}

void TimelineRuler::beginSeekDrag(const juce::MouseEvent& e)
{
    double t0 = xToSeconds(e.x);
    if (onSnapTime) t0 = onSnapTime(t0);
    dragStartTime  = t0;
    dragStartX     = e.x;
    dragStartY     = e.y;
    isDraggingLoop = false;
    isDraggingZoom = false;
    seekArmed      = true;   // クリック確定 (mouseUp) でシーク。ズーム/ループに化けたら取り消す
    zoomStartPpb     = pixelsPerBeat;
    zoomCenterTime   = xToSeconds(e.x);
    zoomCenterXLocal = e.x;
    // ここでは即シークしない: 上下ドラッグでのズーム操作中に再生バーが飛ぶのを防ぐため、
    // ドラッグ方向が確定するまでシークを保留する (純粋なクリックは mouseUp で確定)。
}

void TimelineRuler::mouseDrag(const juce::MouseEvent& e)
{
    if (e.mods.isRightButtonDown()) return;

    // テンポマーカーのドラッグ
    if (draggingBpmIdx >= 0 && draggingBpmIdx < (int)bpmChanges.size())
    {
        double t = xToSeconds(e.x);
        if (onSnapTimeForBpm) t = onSnapTimeForBpm(t);
        if (t < 0.001) t = 0.001;  // 曲頭は予約
        bpmChanges[(size_t)draggingBpmIdx].timeSec = t;
        if (onBpmChangesUpdated) onBpmChangesUpdated(bpmChanges);
        repaint();
        return;
    }

    // 拍子マーカーのドラッグ
    if (draggingMeterIdx >= 0 && draggingMeterIdx < (int)meterChanges.size())
    {
        int newBar1 = xToBar1(e.x);
        int newBarIdx = juce::jmax(1, newBar1 - 1);  // 0番目（曲頭）は予約
        meterChanges[(size_t)draggingMeterIdx].barIndex = newBarIdx;
        if (onMeterChangesUpdated) onMeterChangesUpdated(meterChanges);
        repaint();
        return;
    }

    // マーカードラッグ中
    if (draggingMarkerIdx >= 0 && draggingMarkerIdx < (int)markers.size())
    {
        if (std::abs(e.x - markerDragStartX) > 3) markerDragMoved = true;
        if (markerDragMoved)
        {
            double t = xToSeconds(e.x);
            if (onSnapTime) t = onSnapTime(t);
            t = juce::jmax(0.0, t);
            // 位置だけ更新してから順序を立て直す
            Marker dragged = markers[draggingMarkerIdx];
            dragged.time = t;
            markers.erase(markers.begin() + draggingMarkerIdx);
            auto it = std::lower_bound(markers.begin(), markers.end(), dragged,
                [](const Marker& a, const Marker& b) { return a.time < b.time; });
            int newIdx = (int)(it - markers.begin());
            markers.insert(it, dragged);
            draggingMarkerIdx = newIdx;
            repaint();
        }
        return;
    }

    // ルーラー範囲の端ハンドルのドラッグ（左右端のリサイズのみ・全体移動は廃止）
    if (draggingLoopHandle != 0)
    {
        double t = xToSeconds(e.x);
        if (onSnapTime) t = onSnapTime(t);
        if (draggingLoopHandle == 1)        // 左端（右端を固定）
        {
            double s = juce::jlimit(0.0, loopEnd - 0.01, t);
            if (onSetLoopRange) onSetLoopRange(s, loopEnd);
        }
        else                                 // 右端（左端を固定）
        {
            double en = juce::jmax(loopStart + 0.01, t);
            if (onSetLoopRange) onSetLoopRange(loopStart, en);
        }
        return;
    }

    const int dx = e.x - dragStartX;
    const int dy = e.y - dragStartY;
    const int absDx = std::abs(dx);
    const int absDy = std::abs(dy);

    // 方向確定: 縦が支配的なら zoom、横ならループ
    if (!isDraggingLoop && !isDraggingZoom)
    {
        if (absDy > 6 && absDy > absDx * 1.5)
            isDraggingZoom = true;
        else if (absDx > 5)
            isDraggingLoop = true;
    }

    if (isDraggingZoom)
    {
        seekArmed = false;   // ズーム操作は再生位置を動かさない
        // 下方向 = 拡大、上方向 = 縮小
        // 1px ≈ 1.2% の指数変化で滑らかに
        const double factor = std::pow(1.012, (double) dy);
        const double newPpb = juce::jlimit(1.0, 200000.0, zoomStartPpb * factor);
        if (onZoomDragged) onZoomDragged(newPpb, zoomCenterTime, zoomCenterXLocal);
        return;
    }

    if (isDraggingLoop)
    {
        seekArmed = false;   // ループ範囲ドラッグも再生位置を動かさない
        double t1 = dragStartTime;
        double t2 = xToSeconds(e.x);
        if (onSnapTime) t2 = onSnapTime(t2);
        if (t1 > t2) std::swap(t1, t2);
        if (onSetLoopRange) onSetLoopRange(t1, t2);
    }
    // 方向未確定の微小移動ではシークしない (純粋なクリックは mouseUp で確定する)
}

void TimelineRuler::mouseUp(const juce::MouseEvent&)
{
    // ドラッグがズーム/ループに化けず純粋なクリックだった場合のみ、ここでシークを確定する
    // (マウスダウン即シークだと上下ドラッグのズーム中に再生バーが飛ぶため保留していた)
    if (seekArmed && onSeek) onSeek(dragStartTime);
    seekArmed         = false;

    draggingMarkerIdx = -1;
    draggingBpmIdx    = -1;
    draggingMeterIdx  = -1;
    draggingLoopHandle = 0;
    isDraggingLoop    = false;
    isDraggingZoom    = false;
    // ドラッグ中に beginMusicEdit() していれば、ここで 1 つの Undo として確定する
    // (位置が動いていなければ host 側が before==after で無視する)
    commitMusicEdit();
}

void TimelineRuler::mouseMove(const juce::MouseEvent& e)
{
    const bool cmdHeld = juce::ModifierKeys::currentModifiers.isCommandDown();
    const bool onMarker = hitTestMarker(e.x, e.y) >= 0
                       || hitTestBpmMarker(e.x, e.y) >= 0
                       || hitTestMeterMarker(e.x, e.y) >= 0;

    const int loopHandle = cmdHeld ? 0 : hitTestLoopHandle(e.x, e.y);

    if (onMarker)
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    else if (cmdHeld && e.y >= 0 && e.y < 44)
        // Cmd を押している間はマーカー / テンポ / 拍子の追加が可能 → ペンシル風カーソル
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
    else if (loopHandle == 1 || loopHandle == 2)
        // ルーラー範囲の左右端 → 横リサイズカーソル
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    else
        // マーカー行を除く全行 (Tempo / Meter / Bars / Time) でシーク可能
        setMouseCursor(juce::MouseCursor::IBeamCursor);
}

void TimelineRuler::modifierKeysChanged(const juce::ModifierKeys&)
{
    // Cmd の押下 / 離しに応じてカーソルを即時更新するため、
    // 現在のマウス位置で mouseMove 相当の判定をやり直す。
    auto pos = getMouseXYRelative();
    if (! getLocalBounds().contains(pos)) return;
    juce::MouseEvent fake(juce::Desktop::getInstance().getMainMouseSource(),
                          pos.toFloat(), juce::ModifierKeys::currentModifiers,
                          juce::MouseInputSource::defaultPressure,
                          juce::MouseInputSource::defaultOrientation,
                          juce::MouseInputSource::defaultRotation,
                          juce::MouseInputSource::defaultTiltX,
                          juce::MouseInputSource::defaultTiltY,
                          this, this, juce::Time::getCurrentTime(),
                          pos.toFloat(), juce::Time::getCurrentTime(), 1, false);
    mouseMove(fake);
}

void TimelineRuler::mouseDoubleClick(const juce::MouseEvent& e)
{
    // マーカー旗のヒットテスト（マーカー行 = top 16px）
    if (e.y > 16) return;
    double bpsLocal = currentBpm / 60.0;
    for (size_t i = 0; i < markers.size(); ++i)
    {
        float mx = (float)(markers[i].time * bpsLocal * pixelsPerBeat - scrollX);
        if (e.x >= mx && e.x <= mx + 14)
        {
            if (onEditMarkerName) onEditMarkerName((int)i);
            return;
        }
    }
}

void TimelineRuler::mouseEnter(const juce::MouseEvent&)
{
    setMouseCursor(juce::MouseCursor::IBeamCursor);
}

void TimelineRuler::mouseExit(const juce::MouseEvent&)
{
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void TimelineRuler::paint(juce::Graphics& g)
{
    g.fillAll(AppColours::rulerBg);

    // ── 行レイアウト (高さは TimelineRuler の constexpr 定数を共有) ──
    const int hMarker = hMarkerRow;
    const int hTempo  = hTempoRow;
    const int hMeter  = hMeterRow;
    const int hBars   = barsRowVisible ? hBarsRow : 0;
    const int hTime   = timeRowVisible ? hTimeRow : 0;
    const int yMarker = 0;
    const int yTempo  = yMarker + hMarker;
    const int yMeter  = yTempo  + hTempo;
    const int yBars   = yMeter  + hMeter;
    const int yTime   = yBars   + hBars;
    const int W = getWidth();
    const int H = getHeight();

    // 部分 repaint 時のクリップ範囲（画面外+クリップ外の bar はループから外す）
    const auto clipBounds = g.getClipBounds();
    const int clipL = clipBounds.getX();
    const int clipR = clipBounds.getRight();

    // 行間セパレーター（表示中の行のみ）
    g.setColour(AppColours::separator);
    g.drawLine(0.0f, (float)yTempo, (float)W, (float)yTempo, 1.0f);
    g.drawLine(0.0f, (float)yMeter, (float)W, (float)yMeter, 1.0f);
    if (barsRowVisible)
        g.drawLine(0.0f, (float)yBars, (float)W, (float)yBars, 1.0f);
    if (timeRowVisible)
        g.drawLine(0.0f, (float)yTime, (float)W, (float)yTime, 1.0f);
    g.drawLine(0.0f, (float)(H - 1), (float)W, (float)(H - 1), 1.0f);

    const double pxPerSec = pixelsPerBeat * (currentBpm / 60.0);

    if (pixelsPerBeat < 0.5) return;

    // 表示範囲: 左端 x=0 を超えるところから、右端 x=W を超えたら終了
    // 小節を 1 から順に時間積み上げ。途中BPM変更で各拍の幅は可変。
    g.setFont(sharedBarFont());
    int    bar = 1;
    double t = 0.0;
    std::vector<double> beatTimes;  // ループ外でヒープ確保を 1 回に抑える
    while (true)
    {
        int n, d; getMeterAtBar1(bar, n, d);
        n = juce::jmax(1, n);
        double barStart = t;
        // 拍を進めて bar end の time を求める
        beatTimes.clear();
        beatTimes.push_back(t);
        for (int beat = 0; beat < n; ++beat)
        {
            double bpm = bpmAt(t);
            t += 60.0 / juce::jmax(1.0, bpm);
            if (beat + 1 < n) beatTimes.push_back(t);
        }
        double barEnd = t;
        double x1 = barStart * pxPerSec - scrollX;
        double x2 = barEnd   * pxPerSec - scrollX;

        // 画面外（W）でループ終了。クリップ外 (clipR) は描画スキップ（テキスト処理を回避）
        if (x2 < 0) { ++bar; if (bar > 100000) break; continue; }
        if (x1 > W) break;
        if (x2 < clipL || x1 > clipR) { ++bar; if (bar > 100000) break; continue; }

        // 小節の幅（ピクセル）
        const double barWidthPx = x2 - x1;

        // 小節線（非表示なら省略）
        if (barsRowVisible)
        {
            // 小節線: ズームアウトで密集して潰れるのを防ぐため、線も間引く。
            // 番号より細かい間隔 (>=12px) で残し、グリッド感を保ちつつ重なりを避ける。
            int lineStride = 1;
            while ((double)lineStride * barWidthPx < 12.0 && lineStride < 1000000)
                lineStride *= 2;

            // バー番号: 小節が十分広ければ毎小節。狭くなったら 4 の倍数 (4,8,16,32...) に
            // 間引いて、ズームアウトしても現在地の目安が消えないようにする。
            int labelStride = 1;
            if (barWidthPx < 24.0)
            {
                labelStride = 4;
                // さらに狭ければ 4 の倍数を保ったまま間隔を広げ、番号同士の重なりを防ぐ
                while ((double)labelStride * barWidthPx < 36.0 && labelStride < 1000000)
                    labelStride *= 2;
            }

            // 先頭 (bar 1) の線と番号は曲頭アンカーとして常に描く
            if (bar == 1 || (bar % lineStride) == 0)
            {
                g.setColour(AppColours::rulerLineBright);
                g.drawLine((float)x1, (float)yBars, (float)x1, (float)(yBars + hBars), 1.0f);
            }
            if (bar == 1 || labelStride == 1 || (bar % labelStride) == 0)
            {
                TextImageCache::getInstance().drawText(g, juce::String(bar),
                    (int)x1 + 3, yBars + 2, 40, hBars - 4,
                    sharedBarFont(), AppColours::rulerText, juce::Justification::centredLeft);
            }
            // ビート線（拍 1 以降、各拍の実時間で）- 拍幅 8px 以上の時だけ
            if (barWidthPx >= (double)n * 8.0)
            {
                for (size_t bi = 1; bi < beatTimes.size(); ++bi)
                {
                    int bx = (int)(beatTimes[bi] * pxPerSec - scrollX);
                    g.setColour(AppColours::rulerLine);
                    g.drawLine((float)bx, (float)(yBars + hBars / 2),
                               (float)bx, (float)(yBars + hBars), 1.0f);
                }
            }
        }

        // 時刻行: 小節幅 36px 以上の時だけ（毎小節描画＝詰まりすぎ）
        if (timeRowVisible && barWidthPx >= 36.0)
        {
            double secs = barStart;
            int    mins = (int)secs / 60;
            int    s    = (int)secs % 60;
            TextImageCache::getInstance().drawText(g,
                juce::String::formatted("%d:%02d", mins, s),
                (int)x1 + 3, yTime + 2, 60, hTime - 4,
                sharedTimeFont(), AppColours::rulerText, juce::Justification::centredLeft);
        }

        ++bar;
        if (bar > 100000) break;
    }

    // ── テンポ行 ──（クリップ範囲が行の Y を含む時のみ描画）
    const bool drawTempoRow = (clipBounds.getY() <= yTempo + hTempo
                               && clipBounds.getBottom() > yTempo);
    if (drawTempoRow)
    {
        g.setColour(juce::Colour(0xff5a8aaa).withAlpha(0.15f));
        g.fillRect(0, yTempo, W, hTempo);
        // 固定ラベル "X BPM" はクリップ左端が 200px 未満のときだけ描画。
        // 値は「表示中の左端位置」の BPM を出す (スクロール追従)。初期値固定だと、テンポ変化を
        // 越えてスクロールした先で実際と違う BPM が出てしまうため。(数字 + BPM で自明なので "Tempo" は付けない)
        if (clipL < 200)
        {
            const double leftTime = juce::jmax(0.0, scrollX / pxPerSec);
            g.setColour(juce::Colour(0xffaaccdd));
            g.setFont(sharedHeaderFont());
            g.drawText(juce::String(bpmAt(leftTime), 2) + " BPM",
                       6, yTempo, 200, hTempo, juce::Justification::centredLeft);
        }
        for (auto& bc : bpmChanges)
        {
            float mx = (float)(bc.timeSec * pxPerSec - scrollX);
            if (mx < -40 || mx > W) continue;
            if (mx + 60 < clipL || mx > clipR) continue;
            g.setColour(juce::Colour(0xff5a8aaa));
            g.fillRect(mx, (float)yTempo, 2.0f, (float)hTempo);
            g.setColour(juce::Colour(0xffccddee));
            g.drawText(juce::String(bc.bpm, 1),
                       (int)mx + 4, yTempo, 60, hTempo,
                       juce::Justification::centredLeft);
        }
    }

    // ── 拍子行 ──（クリップ範囲が行を含む時のみ描画）
    const bool drawMeterRow = (clipBounds.getY() <= yMeter + hMeter
                               && clipBounds.getBottom() > yMeter);
    if (drawMeterRow)
    {
        g.setColour(juce::Colour(0xffaa6a2a).withAlpha(0.15f));
        g.fillRect(0, yMeter, W, hMeter);
        if (clipL < 200)
        {
            // 値は「表示中の左端位置」の拍子を出す (スクロール追従)。初期拍子固定だと、拍子変化を
            // 越えてスクロールした先で実際と違う拍子 (例: ずっと 4/4) が出てしまうため。
            const double leftTime = juce::jmax(0.0, scrollX / pxPerSec);
            int ln, ld; getMeterAtBar1(barAtTime(leftTime), ln, ld);
            g.setColour(juce::Colour(0xffddbb99));
            g.setFont(sharedHeaderFont());
            g.drawText("Meter  " + juce::String(ln) + "/" + juce::String(ld),
                       6, yMeter, 200, hMeter, juce::Justification::centredLeft);
        }
        for (auto& mc : meterChanges)
        {
            double secs = barStartSecs(mc.barIndex + 1);
            float mx = (float)(secs * pxPerSec - scrollX);
            if (mx < -40 || mx > W) continue;
            if (mx + 50 < clipL || mx > clipR) continue;
            g.setColour(juce::Colour(0xffaa6a2a));
            g.fillRect(mx, (float)yMeter, 2.0f, (float)hMeter);
            g.setColour(juce::Colour(0xffffcc88));
            g.drawText(juce::String(mc.numerator) + "/" + juce::String(mc.denominator),
                       (int)mx + 4, yMeter, 50, hMeter,
                       juce::Justification::centredLeft);
        }
    }

    // ── ルーラー範囲（ロケーター / ループ範囲を兼用） ──
    //    選択範囲(青)と区別できるよう薄い白系で、小節バー行(hBars)内だけを着色する。
    //    角は三角ロケーター。loopActive のときだけ少し濃くする。
    if (loopEnd > loopStart && hBars > 0)
    {
        float lx1 = (float) RulerRangeMath::timeToX(loopStart, currentBpm, pixelsPerBeat, scrollX);
        float lx2 = (float) RulerRangeMath::timeToX(loopEnd,   currentBpm, pixelsPerBeat, scrollX);
        if (lx2 > 0 && lx1 < W)
        {
            const float yb = (float)yBars;
            const float bh = (float)hBars;
            const juce::Colour loopCol = juce::Colours::white;
            const float alphaFill = loopActive ? 0.22f : 0.12f;
            const float alphaLine = loopActive ? 0.85f : 0.55f;
            g.setColour(loopCol.withAlpha(alphaFill));
            g.fillRect(lx1, yb, lx2 - lx1, bh);
            g.setColour(loopCol.withAlpha(alphaLine));
            g.drawLine(lx1, yb, lx1, yb + bh, 1.5f);
            g.drawLine(lx2, yb, lx2, yb + bh, 1.5f);
            // 角の三角ロケーター（左端=右下がり / 右端=左下がり）
            const float tw = 7.0f;
            const float th = juce::jmin(bh, 9.0f);
            if (lx2 - lx1 > tw * 2.0f)
            {
                g.setColour(loopCol.withAlpha(loopActive ? 0.95f : 0.7f));
                juce::Path triL, triR;
                triL.addTriangle(lx1, yb, lx1 + tw, yb, lx1, yb + th);
                triR.addTriangle(lx2, yb, lx2 - tw, yb, lx2, yb + th);
                g.fillPath(triL);
                g.fillPath(triR);
            }
        }
    }

    // ── マーカー行（旗 + 名前、色対応） ──
    g.setColour(juce::Colour(0xff2a2a2a));
    g.fillRect(0, yMarker, W, hMarker);
    {
        double bpsLocal = currentBpm / 60.0;
        for (auto& m : markers)
        {
            float mx = (float)(m.time * bpsLocal * pixelsPerBeat - scrollX);
            if (mx < -10 || mx > W + 10) continue;
            juce::Colour col = useMarkerColors ? m.colour : juce::Colour(0xffffaa44);
            g.setColour(col);
            juce::Path flag;
            flag.addTriangle(mx, (float)yMarker,
                             mx + 12.0f, (float)yMarker,
                             mx, (float)(yMarker + hMarker - 2));
            g.fillPath(flag);
            g.setColour(col);
            g.setFont(sharedBarFont());
            g.drawText(m.name.isEmpty() ? juce::String("Marker") : m.name,
                       (int)mx + 14, yMarker, 200, hMarker,
                       juce::Justification::centredLeft, true);
        }
    }

    // ── プレイヘッド（全行を貫通） ──
    float phx = (float)(playheadX - scrollX);
    if (phx >= 0.0f && phx <= W)
    {
        g.setColour(AppColours::playhead);
        g.fillRect(phx - 1.0f, 0.0f, 2.0f, (float)H);
    }
}

//==============================================================================
TimelineView::TimelineView(TrackManager& tm) : trackManager(tm)
{
    // paint() が fillAll で全面を塗るため不透明 (GPU コンポジットのブレンドを省く)。
    setOpaque(true);

    // ルーラーのシーククリックを TimelineView → MainComponent へ伝達
    ruler.onSeek = [this](double seconds)
    {
        setPlayheadPosition(seconds);
        if (onSeek) onSeek(seconds);
    };

    // ルーラーを上下ドラッグして横方向ズーム
    ruler.onZoomDragged = [this](double newPpb, double centerTime, int centerXLocal)
    {
        pixelsPerBeat = juce::jmin(newPpb, maxPixelsPerBeat());   // 拡大上限 (約 2 拍)
        const double bps = bpm / 60.0;
        // クリックした位置 (centerXLocal) が centerTime のまま動かないよう scrollX を再計算
        scrollX = juce::jmax(0.0, centerTime * bps * pixelsPerBeat - (double) centerXLocal);
        ruler.setPixelsPerBeat(pixelsPerBeat);
        ruler.setPlayheadX(playheadSecs * bps * pixelsPerBeat);
        ruler.setScrollX(scrollX);
        hScrollBar.setCurrentRange(scrollX, hScrollBar.getCurrentRangeSize());
        // Cmd+スクロールの横ズームと同じく、連続ズーム中は波形キャッシュの再生成を抑止して
        // 既存を拡縮 blit するだけにする (イベント毎の全可視クリップ再生成でカクつくため)。
        // ドラッグが止まって 70ms 後にタイマーが 1 度だけ綺麗に再描画する
        zoomActive = true;
        startTimer(70);
        resized();
        repaint();
    };

    addAndMakeVisible(ruler);

    auto styleBar = [](juce::ScrollBar& bar)
    {
        bar.setColour(juce::ScrollBar::backgroundColourId, AppColours::panelBg);
        bar.setColour(juce::ScrollBar::thumbColourId,      AppColours::separator);
    };
    styleBar(hScrollBar); styleBar(vScrollBar);
    hScrollBar.addListener(this);
    vScrollBar.addListener(this);
    addAndMakeVisible(hScrollBar);
    addAndMakeVisible(vScrollBar);

    // ── ズームボタン (Cmd+スクロールと同等、再生バー起点) ──
    auto styleZoomBtn = [](juce::TextButton& b)
    {
        b.setColour(juce::TextButton::buttonColourId,  AppColours::buttonBg);
        b.setColour(juce::TextButton::textColourOffId, AppColours::text);
        b.setWantsKeyboardFocus(false);
    };
    styleZoomBtn(hZoomOutBtn);
    styleZoomBtn(hZoomInBtn);
    styleZoomBtn(hZoomResetBtn);
    styleZoomBtn(vZoomOutBtn);
    styleZoomBtn(vZoomInBtn);
    styleZoomBtn(vZoomResetBtn);
    hZoomOutBtn.setTooltip(tr(u8"横方向を縮小 (再生バー中心)"));
    hZoomInBtn .setTooltip(tr(u8"横方向を拡大 (再生バー中心)"));
    hZoomResetBtn.setTooltip(tr(u8"横ズームをリセット (全体表示)"));
    vZoomOutBtn.setTooltip(tr(u8"波形振幅を縮小"));
    vZoomInBtn .setTooltip(tr(u8"波形振幅を拡大"));
    vZoomResetBtn.setTooltip(tr(u8"波形振幅をリセット"));
    hZoomOutBtn.onClick = [this] { applyHorizontalZoomStep(-1.0); };
    hZoomInBtn .onClick = [this] { applyHorizontalZoomStep(+1.0); };
    hZoomResetBtn.onClick = [this] { zoomToFitAll(); };
    vZoomOutBtn.onClick = [this] { applyVerticalZoomStep(-1.0); };
    vZoomInBtn .onClick = [this] { applyVerticalZoomStep(+1.0); };
    vZoomResetBtn.onClick = [this] { resetVerticalZoom(); };
    addAndMakeVisible(hZoomOutBtn);
    addAndMakeVisible(hZoomInBtn);
    addAndMakeVisible(hZoomResetBtn);
    addAndMakeVisible(vZoomOutBtn);
    addAndMakeVisible(vZoomInBtn);
    addAndMakeVisible(vZoomResetBtn);

    // ※ 過去は startTimerHz(60) で常時 repaint していたが、停止中も含めて CPU を浪費していたため廃止。
    //    プレイヘッドの移動は setPlayheadPosition() が直接、必要な縦帯だけ repaint する。
}

TimelineView::~TimelineView()
{
    stopTimer();
    hScrollBar.removeListener(this);
    vScrollBar.removeListener(this);
}

void TimelineView::timerCallback()
{
    // 横ズーム / 高速横スクロールが止まったら、抑止していた波形キャッシュ再生成を解禁して
    // 1 度だけ綺麗に再描画する (巨大クリップの帯が新しい位置/倍率で鮮明に描き直される)。
    if (zoomActive || deferBigClipRegen)
    {
        zoomActive = false;
        deferBigClipRegen = false;
        repaint();
    }
    stopTimer();
}

// 横スクロール中は巨大クリップ (Path-2) の重い帯再生成 (fillPath) を一切走らせず、既存帯を
// blit して滑らかに流す。スクロールが止まって 80ms 後にタイマーが 1 度だけ綺麗に再生成する。
// 速度に依らず必ず遅延するので、しきい値チューニング (デバイス差・速度ゆらぎ) に頼らず常に
// 滑らか。スクロール毎にタイマーを延長するので、止まるまで途中で再生成 = 一瞬のカクつきが
// 起きない (帯の外は一時的に stale/空白になるが、止まれば追従描画される。多くの DAW と同じ挙動)。
void TimelineView::noteHorizontalScroll()
{
    deferBigClipRegen = true;
    startTimer(80);
}

//==============================================================================
// ヒットテスト
//==============================================================================

juce::Rectangle<int> TimelineView::getContentArea() const
{
    return getLocalBounds()
               .withTrimmedTop(rulerHeight())
               .withTrimmedRight(scrollLaneSize)
               .withTrimmedBottom(scrollLaneSize);
}

double TimelineView::maxPixelsPerBeat() const
{
    // 最大ズーム = 約 4 拍がビュー幅に収まる所 (contentW / 4 拍)。これ以上の拡大は
    // 描画が重くなる割に用途が無いため制限する。レイアウト前 (幅 ~0) はハード上限を返し、
    // プロジェクトロード時の復元値を不当に潰さない。
    const double w = (double) getContentArea().getWidth();
    if (w < 50.0) return 200000.0;
    return juce::jlimit(50.0, 200000.0, w * 0.25);
}

ClipRef TimelineView::getClipAt(int mouseX, int mouseY) const
{
    auto area = getContentArea();
    if (!area.contains(mouseX, mouseY)) return {};

    const double bps = bpm / 60.0;
    const int count  = trackManager.getTrackCount();

    // 後ろのトラックから（上にあるトラックを優先）
    for (int ti = count - 1; ti >= 0; --ti)
    {
        auto* track   = trackManager.getTrack(ti);
        int   trackTop = area.getY() + trackManager.getTrackY(ti) - scrollY;
        int   trackH   = track->getTotalHeight();

        if (mouseY < trackTop || mouseY >= trackTop + trackH) continue;

        bool collapsed    = track->isLanesCollapsed();
        int  visibleLanes = collapsed ? 1 : track->getLaneCount();

        const int trackMainH = track->getMainHeight();
        const int trackLaneH = track->getLaneHeight();
        for (int li = 0; li < visibleLanes; ++li)
        {
            int lTop = trackTop + (li == 0 ? 0
                       : trackMainH + (li - 1) * trackLaneH);
            int lH   = (collapsed) ? trackH
                     : (li == 0)   ? juce::jmin(trackMainH, trackH)
                                   : trackLaneH;

            if (mouseY < lTop || mouseY >= lTop + lH) continue;

            auto* lane = track->getLane(li);
            if (!lane) continue;

            // 逆順で検索（後から追加されたクリップ = 上に表示 = 優先）
            for (int ci = (int)lane->clips.size() - 1; ci >= 0; --ci)
            {
                auto* clip = lane->clips[(size_t)ci].get();
                int   cx   = (int)(clip->getStartPosition() * bps * pixelsPerBeat - scrollX);
                int   cw   = juce::jmax(4, (int)(clip->getDuration() * bps * pixelsPerBeat));

                if (mouseX >= cx && mouseX < cx + cw &&
                    mouseY >= lTop + 1 && mouseY < lTop + lH - 1)
                {
                    ClipRef ref;
                    ref.track    = track;
                    ref.lane     = lane;
                    ref.clip     = clip;
                    ref.trackIdx = ti;
                    ref.laneIdx  = li;
                    return ref;
                }
            }
        }
    }
    return {};
}


//==============================================================================
// マウスイベント
//==============================================================================


//==============================================================================
// 編集操作（外部から呼ぶ）
//==============================================================================


double TimelineView::positionToX(double seconds) const
{
    return seconds * (bpm / 60.0) * pixelsPerBeat - scrollX;
}

double TimelineView::xToPosition(int x) const
{
    return (x + scrollX) / ((bpm / 60.0) * pixelsPerBeat);
}

bool TimelineView::isClipInSelection(const AudioClip* clip) const
{
    if (selectedClip.clip == clip) return true;
    for (auto& r : extraSelections) if (r.clip == clip) return true;
    return false;
}

void TimelineView::repaintTimeRange(double startSec, double endSec)
{
    // setPlayheadPosition と同じ座標系 (positionToX = コンテンツ x = コンポーネント x)。
    // 縦は全トラック行を覆う full height、横は対象範囲だけに絞る。
    const int x1 = (int) positionToX(startSec) - 2;
    const int x2 = (int) positionToX(endSec)   + 3;
    const int left = juce::jmin(x1, x2);
    const int w    = juce::jmax(1, juce::jmax(x1, x2) - left);
    repaint(left, 0, w, getHeight());
}

void TimelineView::repaintRecordingArea()
{
    // 録音中のライブ波形が描かれるトラック行 (録音開始 → 現在の波形末尾) だけを repaint。
    // 全面 repaint だと 20Hz で全トラックを再描画してちらつくため、領域を絞る。
    auto area = getContentArea();
    const double bps = bpm / 60.0;
    for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
    {
        auto* tr = trackManager.getTrack(ti);
        if (!tr || !tr->hasLiveRecording()) continue;
        const int trackTop = area.getY() + trackManager.getTrackY(ti) - scrollY;
        const int trackH   = tr->getTotalHeight();
        const double startSecs = tr->getRecordingStartPos();
        // 録音バーの右端は再生バーに追従する (drawTrackRows の isLiveLane ブロックと同じ)。
        // バッファ由来の長さも max で含め、負リード (内容を遅く描く) でも取りこぼさない
        const double durSecs   = juce::jmax(0.0, playheadSecs - startSecs,
            tr->getLiveBuffer().getDurationSeconds(sampleRate) - tr->getLiveBufferLeadSecs());
        int x1 = (int)(startSecs * bps * pixelsPerBeat - scrollX) - 2;
        int x2 = (int)((startSecs + durSecs) * bps * pixelsPerBeat - scrollX) + 6;
        x1 = juce::jmax(area.getX(), x1);
        x2 = juce::jmin(area.getRight(), x2);
        if (x2 > x1) repaint(x1, trackTop, x2 - x1, trackH);
    }

    // MIDI 録音インジケータ (アーム済み MIDI トラック行の録音開始 → 再生バー) も伸びる領域を
    // 部分 repaint する (MIDI のみの録音ではライブ波形レーンが無く上のループを通らないため)
    if (midiRecIndicator)
        for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
        {
            auto* tr = trackManager.getTrack(ti);
            if (!tr || !tr->isMidiTrack() || !tr->isRecArmed()) continue;
            const int trackTop = area.getY() + trackManager.getTrackY(ti) - scrollY;
            const int trackH   = tr->getTotalHeight();
            if (trackH <= 0) continue;
            int x1 = (int)(midiRecIndicatorStart * bps * pixelsPerBeat - scrollX) - 2;
            int x2 = (int)(playheadSecs * bps * pixelsPerBeat - scrollX) + 6;
            x1 = juce::jmax(area.getX(), x1);
            x2 = juce::jmin(area.getRight(), x2);
            if (x2 > x1) repaint(x1, trackTop, x2 - x1, trackH);
        }
}

void TimelineView::repaintLiveLeadingEdge()
{
    // 再生バー付近の先端帯 (marginSecs) だけを repaint する。ここに新着波形が載るので、
    // vblank ごとに描き直せば波形の先端が滑らかな再生バーに密着して追従する。draw は
    // clip 域 (この帯) だけをパス化するため、録音が長くなってもコストは帯幅で頭打ち。
    // marginSecs はレイテンシ補正 (手動 ±300ms + デバイス往復) と 20Hz 量子化を余裕を持って覆う。
    auto area = getContentArea();
    const double bps = bpm / 60.0;
    constexpr double marginSecs = 0.5;
    for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
    {
        auto* tr = trackManager.getTrack(ti);
        if (!tr || !tr->hasLiveRecording()) continue;
        const int trackTop = area.getY() + trackManager.getTrackY(ti) - scrollY;
        const int trackH   = tr->getTotalHeight();
        const double startSecs = tr->getRecordingStartPos();
        const double endSecs   = juce::jmax(startSecs, playheadSecs);
        const double leftSecs  = juce::jmax(startSecs, endSecs - marginSecs);
        int x1 = (int)(leftSecs * bps * pixelsPerBeat - scrollX) - 2;
        int x2 = (int)(endSecs  * bps * pixelsPerBeat - scrollX) + 6;
        x1 = juce::jmax(area.getX(), x1);
        x2 = juce::jmin(area.getRight(), x2);
        if (x2 > x1) repaint(x1, trackTop, x2 - x1, trackH);
    }
}

bool TimelineView::getRangeForRulerFromSelection(double& start, double& end) const
{
    // 範囲選択を最優先、無ければ選択クリップ群 (primary + 追加選択) の全体スパン
    std::vector<std::pair<double, double>> spans;
    if (selectedClip.valid())
        spans.emplace_back(selectedClip.clip->getStartPosition(),
                           selectedClip.clip->getEndPosition());
    for (auto& r : extraSelections)
        if (r.clip)
            spans.emplace_back(r.clip->getStartPosition(), r.clip->getEndPosition());
    return RulerRangeMath::rangeForRuler(hasSelectionRange(), loopStartTV, loopEndTV,
                                         spans, start, end);
}

void TimelineView::clearAllSelections()
{
    selectedClip.clear();
    extraSelections.clear();
    selectedCrossfade.clear();
    crossfadeNeighbor = nullptr;  // クロスフェードドラッグ相手の stale ポインタも防止
    selectedMidiClip  = nullptr;
    selectedMidiTrack = nullptr;
    hoveredClip       = nullptr;  // 破棄系編集後の stale ポインタを防止 (次の mouseMove で再設定)
    hoveredHandle     = DragMode::None;
    notifySelectionChanged();     // 選択クリア → ヘッダの採用ボタンを dim に
}

void TimelineView::addToSelection(const ClipRef& ref)
{
    if (!ref.valid()) return;
    if (selectedClip.clip == ref.clip) return;
    for (auto& r : extraSelections) if (r.clip == ref.clip) return;
    extraSelections.push_back(ref);
}

double TimelineView::snapTime(double secs) const
{
    // 変拍子/途中テンポ変更対応: 描画グリッド (drawTrackRows) と同じ歩進で
    // 「小節頭 + 小節内サブグリッド」へスナップする (GridSnapMath に一本化)。
    // 旧実装の固定間隔 round は 5/4 等の小節頭に吸着できなかった
    return GridSnapMath::snapToGrid(secs, appSettings.snapMode, bpm,
                                    appSettings.bpmChanges,
                                    appSettings.meterNumerator,
                                    appSettings.meterChanges);
}

void TimelineView::setPlayheadPosition(double seconds)
{
    if (std::abs(seconds - playheadSecs) < 1e-4) return;  // 微小変化はスキップ

    const double bps = bpm / 60.0;
    const int oldX = (int)(playheadSecs * bps * pixelsPerBeat - scrollX);
    const int newX = (int)(seconds      * bps * pixelsPerBeat - scrollX);

    playheadSecs = seconds;
    ruler.setPlayheadX(seconds * bps * pixelsPerBeat);

    // 旧位置と新位置を覆う細い縦帯だけを repaint（全面 repaint を回避）
    const int x1 = juce::jmin(oldX, newX) - 2;
    const int x2 = juce::jmax(oldX, newX) + 3;
    repaint(juce::Rectangle<int>(x1, 0, x2 - x1, getHeight()));
}

void TimelineView::refresh()
{
    const int totalH = juce::jmax(400, trackManager.getTotalHeight() + 200);
    vScrollBar.setRangeLimits(0.0, (double)totalH);
    vScrollBar.setCurrentRange(scrollY, getHeight() - rulerHeight() - scrollLaneSize);
    repaint();
}

void TimelineView::setScrollY(int y)
{
    scrollY = y;
    repaint();
}

void TimelineView::setBpm(double b)
{
    bpm = b;
    ruler.setBpm(b);
    // BPM 変更で playhead のピクセル位置が変わるため、旧位置の残像を消すには
    // 全面 repaint が必要 (setPlayheadPosition の差分 repaint だけだと旧位置の
    // ゴーストが残る)
    repaint();
}

//==============================================================================
// クリップを laneBounds に厳密にクリップして描画する

//==============================================================================
void TimelineView::paint(juce::Graphics& g)
{
    g.fillAll(AppColours::trackBg);

    auto content = getLocalBounds()
                       .withTrimmedTop(rulerHeight())
                       .withTrimmedRight(scrollLaneSize)
                       .withTrimmedBottom(scrollLaneSize);
    // ルーラーへのめり込みを防止: コンテンツ領域でクリップ
    g.saveState();
    g.reduceClipRegion(content);
    drawTrackRows(g, content);
    g.restoreState();

    // カットラインプレビュー（Cmd ホールド中）
    if (showCutLine && content.contains(cutLineX, content.getCentreY()))
    {
        g.setColour(juce::Colour(0xffffe066).withAlpha(0.9f));
        g.drawLine((float)cutLineX, (float)content.getY(),
                   (float)cutLineX, (float)content.getBottom(), 1.5f);
        g.fillEllipse((float)cutLineX - 3.0f, (float)content.getY() + 4.0f, 6.0f, 6.0f);
    }

    // Move ドラッグ中、別トラックの上にいたらゴースト表示
    if (dragMode == DragMode::Move && selectedClip.valid()
        && dragHoverTrackIdx >= 0
        && (dragHoverTrackIdx != selectedClip.trackIdx
            || dragHoverLaneIdx != selectedClip.laneIdx))
    {
        if (auto* targetTrack = trackManager.getTrack(dragHoverTrackIdx))
        {
            int trackTop = content.getY() + trackManager.getTrackY(dragHoverTrackIdx) - scrollY;
            bool collapsed = targetTrack->isLanesCollapsed();
            int trackH = targetTrack->getTotalHeight();
            int laneTop = trackTop + (dragHoverLaneIdx == 0 ? 0
                          : targetTrack->getMainHeight() + (dragHoverLaneIdx - 1) * targetTrack->getLaneHeight());
            int laneH   = collapsed ? trackH
                          : (dragHoverLaneIdx == 0
                              ? juce::jmin(targetTrack->getMainHeight(), trackH)
                              : targetTrack->getLaneHeight());

            const double bps = bpm / 60.0;
            int cx = (int)(selectedClip.clip->getStartPosition() * bps * pixelsPerBeat - scrollX);
            int cw = juce::jmax(4, (int)(selectedClip.clip->getDuration() * bps * pixelsPerBeat));
            juce::Rectangle<int> ghostRect { cx, laneTop + 1, cw, laneH - 2 };

            g.saveState();
            g.reduceClipRegion(content);

            // 背景
            g.setColour(selectedClip.track->getColour().withAlpha(0.4f));
            g.fillRoundedRectangle(ghostRect.toFloat(), 2.0f);

            // 波形
            if (selectedClip.clip->getThumbnail().getTotalLength() > 0.0)
            {
                double fo = selectedClip.clip->getFileOffset();
                float vz = juce::jlimit(0.05f, 4.0f,
                    1.0f * selectedClip.clip->getGain() * (float)waveformZoom);
                drawClipWaveform(g, *selectedClip.clip, ghostRect.reduced(0, 4),
                    fo, selectedClip.clip->getDuration(), vz,
                    selectedClip.track->getColour().brighter(0.9f).withAlpha(0.5f));
            }

            // オレンジの枠線
            g.setColour(juce::Colour(0xffff8800).withAlpha(0.85f));
            g.drawRoundedRectangle(ghostRect.toFloat(), 2.0f, 2.0f);

            g.restoreState();
        }
    }

    // ラバーバンド範囲選択の枠
    if (rubberBandActive)
    {
        int x1 = juce::jmin(rubberBandStart.x, rubberBandEnd.x);
        int x2 = juce::jmax(rubberBandStart.x, rubberBandEnd.x);
        int y1 = juce::jmin(rubberBandStart.y, rubberBandEnd.y);
        int y2 = juce::jmax(rubberBandStart.y, rubberBandEnd.y);
        juce::Rectangle<int> rb { x1, y1, x2 - x1, y2 - y1 };
        g.setColour(juce::Colour(0xff5a8aaa).withAlpha(0.15f));
        g.fillRect(rb);
        g.setColour(juce::Colour(0xff5a8aaa).withAlpha(0.8f));
        g.drawRect(rb.toFloat(), 1.0f);
    }
}


void TimelineView::resized()
{
    auto b = getLocalBounds();
    constexpr int zoomBtnSize = 18;   // +/- ボタンの正方形サイズ

    ruler.setBounds(b.removeFromTop(rulerHeight()).withTrimmedRight(zoomBtnSize));

    // 右側: vScrollBar (上、幅 zoomBtnSize) + +/-/リセット ボタン (下)
    auto rightCol = b.removeFromRight(zoomBtnSize);
    auto rightBottom = rightCol.removeFromBottom(zoomBtnSize * 3 + scrollBarSize)
                                .withTrimmedBottom(scrollBarSize);
    vZoomInBtn   .setBounds(rightBottom.removeFromTop(zoomBtnSize));
    vZoomOutBtn  .setBounds(rightBottom.removeFromTop(zoomBtnSize));
    vZoomResetBtn.setBounds(rightBottom.removeFromTop(zoomBtnSize));
    vScrollBar.setBounds(rightCol);

    // 下側: hScrollBar (左) + +/-/リセット ボタン (右、ボタン高さに合わせて行を高く)
    auto bottomRow = b.removeFromBottom(zoomBtnSize);
    auto rightZoomArea = bottomRow.removeFromRight(zoomBtnSize * 3);
    hZoomOutBtn  .setBounds(rightZoomArea.removeFromLeft(zoomBtnSize));
    hZoomInBtn   .setBounds(rightZoomArea.removeFromLeft(zoomBtnSize));
    hZoomResetBtn.setBounds(rightZoomArea);
    // hScrollBar は元の細い高さで中央に
    const int sbTop = bottomRow.getY() + (zoomBtnSize - scrollBarSize) / 2;
    hScrollBar.setBounds(bottomRow.getX(), sbTop, bottomRow.getWidth(), scrollBarSize);

    ruler.setPixelsPerBeat(pixelsPerBeat);
    ruler.setScrollX(scrollX);

    // ── スクロール範囲: 実コンテンツの末尾 + 30 秒余裕、最低 5 分まで保証 ──
    double contentEndSec = 0.0;
    for (int ti = 0; ti < trackManager.getTrackCount(); ++ti)
    {
        auto* tr = trackManager.getTrack(ti);
        if (!tr) continue;
        for (int li = 0; li < tr->getLaneCount(); ++li)
        {
            auto* ln = tr->getLane(li);
            if (!ln) continue;
            for (auto& c : ln->clips)
                if (c) contentEndSec = juce::jmax(contentEndSec, c->getEndPosition());
        }
        for (int ci = 0; ci < tr->getMidiClipCount(); ++ci)
            if (auto* mc = tr->getMidiClip(ci))
                contentEndSec = juce::jmax(contentEndSec, mc->getEndPosition());
    }
    const double minLenSec = 300.0;            // 最低でも 5 分は保証
    const double extraSec  = 30.0;             // 末尾に少しスクロール余白
    const double totalSec  = juce::jmax(minLenSec, contentEndSec + extraSec);
    const double totalPx   = totalSec * (bpm / 60.0) * pixelsPerBeat;
    hScrollBar.setRangeLimits(0.0, totalPx);
    hScrollBar.setCurrentRange(scrollX, b.getWidth());

    const int totalH = juce::jmax(400, trackManager.getTotalHeight() + 200);
    vScrollBar.setRangeLimits(0.0, (double)totalH);
    vScrollBar.setCurrentRange(scrollY, b.getHeight());
}

void TimelineView::scrollBarMoved(juce::ScrollBar* bar, double newRange)
{
    if (bar == &hScrollBar)
    {
        noteHorizontalScroll();  // スクロール中は巨大クリップの帯再生成を遅延 (止まってから再生成)
        scrollX = newRange;
        ruler.setScrollX(scrollX);
    }
    else
    {
        scrollY = (int)newRange;
        if (onVerticalScroll) onVerticalScroll(scrollY);
    }
    repaint();
}


bool TimelineView::isInterestedInFileDrag(const juce::StringArray& files)
{
    static const juce::StringArray exts {
        ".wav", ".aif", ".aiff", ".mp3", ".m4a", ".flac", ".ogg",
        ".mid", ".midi"
    };
    for (auto& f : files)
        for (auto& e : exts)
            if (f.endsWithIgnoreCase(e)) return true;
    return false;
}

void TimelineView::fileDragEnter(const juce::StringArray&, int, int)
{
    fileDragActive = true;
    repaint();
}

void TimelineView::fileDragExit(const juce::StringArray&)
{
    fileDragActive = false;
    fileDragHoverTrack = -1;
    repaint();
}

void TimelineView::filesDropped(const juce::StringArray& files, int x, int y)
{
    fileDragActive = false;
    fileDragHoverTrack = -1;

    auto area = getContentArea();
    int relY = y - area.getY() + scrollY;
    int targetTrackIdx = trackManager.trackAtY(relY);
    // フォルダトラックへは音声を置けない (クリップを持たないグループバス) → 新規トラック扱い
    if (targetTrackIdx >= 0)
        if (auto* tt = trackManager.getTrack(targetTrackIdx); tt && tt->isFolderTrack())
            targetTrackIdx = -1;

    // ドロップ位置の時間
    double dropTime = juce::jmax(0.0, xToPosition(x));
    dropTime = snapTime(dropTime);

    // MIDI とオーディオを仕分け。MIDI は専用ハンドラへ個別に、オーディオは 1 つの進捗
    // ウィンドウでまとめて変換 → 配置する (複数ドロップでもウィンドウは 1 つ)。
    juce::Array<juce::File> audioFiles;
    for (auto& f : files)
    {
        juce::File file(f);
        if (!file.existsAsFile()) continue;

        if (f.endsWithIgnoreCase(".mid") || f.endsWithIgnoreCase(".midi"))
        {
            if (onImportMidi) onImportMidi(file, dropTime);
            continue;
        }
        audioFiles.add(file);
    }

    if (!audioFiles.isEmpty() && onImportAudioFiles)
        onImportAudioFiles(audioFiles, dropTime, targetTrackIdx);   // 変換 + 配置 + refresh を委譲

    repaint();
}
