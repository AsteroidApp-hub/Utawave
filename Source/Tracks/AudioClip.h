// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#pragma once
#include <JuceHeader.h>

// クリップゲインのブレークポイント（時間, dB値）
struct GainPoint
{
    double time { 0.0 };  // クリップ開始からの秒数
    float  dB   { 0.0f }; // dB値
};

enum class FadeCurve { Linear, Logarithmic, EqualPower, SCurve };

// ファイル内容 (ファイル名 + サイズ + 更新日時) から算出するサムネイルキャッシュ用ハッシュ。
// 同一パスでも内容が差し替わるとハッシュが変わるので、AudioThumbnailCache がそのファイル
// だけ自動的にミス → 再デコードする (ファイル単位の stale 判定)。逆に「同一内容の確定済み
// ファイル」は常に同じハッシュになるため、それを参照する複数クリップ (録音の lane0 +
// テイク退避など) は同一キャッシュエントリを共有し、必ず同じ波形になる。
// キーは絶対パスでなく「ファイル名」を使う: 音声は全て <projectFolder>/Audio/ 直下で
// ファイル名が一意なので衝突せず、プロジェクトフォルダの移動/リネームでもディスク
// キャッシュ (thumbnails.bin) がヒットし続ける (絶対パスだと全ミス = 毎回フルデコード)。
inline juce::int64 contentHashForFile(const juce::File& f)
{
    return (f.getFileName() + ":" + juce::String(f.getSize()) + ":"
            + juce::String(f.getLastModificationTime().toMilliseconds())).hashCode64();
}

// サムネイルキャッシュ用の InputSource。標準の FileInputSource はパスのみのハッシュなので、
// 同一パスに別内容が書かれても古い波形がヒットしてしまう。内容ハッシュでそれを防ぐ。
struct ContentHashedFileSource : juce::FileInputSource
{
    explicit ContentHashedFileSource(const juce::File& f)
        : juce::FileInputSource(f), theFile(f) {}
    juce::int64 hashCode() const override { return contentHashForFile(theFile); }
    juce::File theFile;
};

class AudioClip
{
public:
    AudioClip(const juce::File& file, double startPos, double durationSecs,
              juce::AudioFormatManager& fmtMgr, juce::AudioThumbnailCache& cache);

    const juce::File&    getFile()          const { return file; }
    double               getStartPosition() const { return startPosition; }
    double               getDuration()      const { return duration; }
    double               getEndPosition()   const { return startPosition + duration; }
    float                getGain()          const { return gain; }
    const juce::String&  getName()          const { return name; }
    juce::Colour         getColour()        const { return colour; }
    juce::AudioThumbnail& getThumbnail()          { return thumbnail; }

    // 2 クリップが「同一の連続した音声」か (= Alt+Click 分割など)。同一ファイルかつ
    // タイムライン↔ファイル の対応 (fileOffset - startPosition) が一致する場合のみ true。
    // この場合だけクロスフェードを抑止する (重ねるとコーム/二重再生になるため #I2)。
    // テイク (同一ファイルでも別リージョン = 別の対応) は通常のクロスフェードを許可する。
    static bool isSameContinuousAudio(const AudioClip& a, const AudioClip& b)
    {
        if (a.getFile() != b.getFile()) return false;
        const double anchorA = a.getFileOffset() - a.getStartPosition();
        const double anchorB = b.getFileOffset() - b.getStartPosition();
        return std::abs(anchorA - anchorB) < 0.010;  // 10ms 以内なら同一連続音声
    }

    // サンプルレベル描画用: ファイルを開いたままにしてサンプルを直接読み出す
    juce::AudioFormatReader* getOrCreateReader()
    {
        if (!cachedReader && formatManager != nullptr)
        {
            cachedReader.reset(formatManager->createReaderFor(file));
            if (cachedReader) cachedSampleRate = cachedReader->sampleRate;
        }
        return cachedReader.get();
    }
    double getCachedSampleRate() const { return cachedSampleRate; }
    void   closeReader() { cachedReader.reset(); }
    // ファイル末尾が伸びた後にサムネイルを再読込（録音中に作られたクリップ用）。
    // ハッシュは内容ベース (contentHashForFile)。録音中に書かれた古い (短い) ファイルとは
    // サイズ/更新日時が違うので確実にキャッシュをバイパスして再デコードする一方、確定後の
    // 同一ファイルを参照する別クリップ (lane0 + テイク退避) は同じハッシュでキャッシュを
    // 共有するため、必ず同一の波形になる。
    // (旧実装は毎回ユニーク値で、クリップごとに別デコード → キャッシュ逼迫時に片方だけ
    //  未完成のまま表示され「テイクだけ波形が違う」不具合があった。ユニーク値は使い捨て
    //  エントリでキャッシュも汚染していた)
    void refreshThumbnail()
    {
        if (formatManager == nullptr) return;
        if (auto* reader = formatManager->createReaderFor(file))
        {
            const juce::int64 hash = contentHashForFile(file);
            thumbnail.setReader(reader, hash);
        }
        // 波形が変わったのでキャッシュも破棄
        waveformCache.width = -1;
        // サンプルレベル描画用の cachedReader も破棄する。録音確定やテイクバックアップで
        // 同一パスのファイル内容・長さが変わるため、古いリーダーを使い続けると拡大表示が
        // 古い末尾長のままになる (次回 getOrCreateReader で開き直させる)。
        cachedReader.reset();
        cachedSampleRate = 0.0;
    }

    void setStartPosition(double pos)   { startPosition = pos; }
    void setDuration(double d)
    {
        duration = juce::jmax(0.0, d);
        // 既存フェードを新しい長さに再クランプする。これをしないとリサイズや
        // クロスフェード解除で fadeIn+fadeOut > duration となり、再生時に
        // クリップ全体が誤ったフェードになる (描画と実音の乖離も防ぐ)。
        fadeInSecs  = juce::jmin(fadeInSecs,  duration * 0.5);
        fadeOutSecs = juce::jmin(fadeOutSecs, duration * 0.5);
    }
    void setGain(float g)               { gain = g; }
    void setName(const juce::String& n) { name = n; }
    void setColour(juce::Colour c)      { colour = c; customColour = true; }
    // ファイルパスを変更（プロジェクト Audio フォルダへの移行用）。サムネイル再読込も行う
    void setFile(const juce::File& f)
    {
        file = f;
        thumbnail.setSource(new ContentHashedFileSource(f));
        cachedReader.reset();
        cachedSampleRate = 0.0;
    }
    bool hasCustomColour() const        { return customColour; }
    void resetColour()                   { customColour = false; }

    // フェード
    double getFadeInSecs()  const { return fadeInSecs; }
    double getFadeOutSecs() const { return fadeOutSecs; }
    void   setFadeInSecs(double s)  { fadeInSecs  = juce::jmax(0.0, juce::jmin(s, duration * 0.5)); }
    void   setFadeOutSecs(double s) { fadeOutSecs = juce::jmax(0.0, juce::jmin(s, duration * 0.5)); }

    // ファイル内オフセット（分割クリップで使用）
    double getFileOffset() const        { return fileOffset; }
    void   setFileOffset(double offset) { fileOffset = juce::jmax(0.0, offset); }

    // フェードカーブ
    FadeCurve getFadeInCurve()  const { return fadeInCurve; }
    FadeCurve getFadeOutCurve() const { return fadeOutCurve; }
    void setFadeInCurve(FadeCurve c)  { fadeInCurve  = c; }
    void setFadeOutCurve(FadeCurve c) { fadeOutCurve = c; }

    // 0.0〜1.0 の進行度から実際のゲインを返す（フェードイン用：0で無音、1で最大）
    static float applyFadeCurve(float t, FadeCurve curve)
    {
        t = juce::jlimit(0.0f, 1.0f, t);
        switch (curve)
        {
            case FadeCurve::Linear:      return t;
            case FadeCurve::Logarithmic: return std::sqrt(t);              // +3dB at midpoint
            case FadeCurve::EqualPower:  return std::sin(t * juce::MathConstants<float>::halfPi);  // -3dB at midpoint
            case FadeCurve::SCurve:      return 0.5f - 0.5f * std::cos(t * juce::MathConstants<float>::pi);
        }
        return t;
    }

    // クリップゲインエンベロープ (ブレークポイント方式)
    const std::vector<GainPoint>& getGainPoints() const { return gainPoints; }
    std::vector<GainPoint>&       getGainPointsRW()     { return gainPoints; }
    void clearGainPoints() { gainPoints.clear(); }
    bool hasGainEnvelope() const { return !gainPoints.empty(); }

    // 時刻 t (クリップ内秒数) におけるエンベロープのdBを返す（線形補間）
    // ポイントがない場合は 0dB（= 倍率なし）
    float getEnvelopeDBAt(double timeInClip) const
    {
        if (gainPoints.empty()) return 0.0f;
        if (gainPoints.size() == 1) return gainPoints.front().dB;
        if (timeInClip <= gainPoints.front().time) return gainPoints.front().dB;
        if (timeInClip >= gainPoints.back().time)  return gainPoints.back().dB;
        for (size_t i = 0; i + 1 < gainPoints.size(); ++i)
        {
            const auto& a = gainPoints[i];
            const auto& b = gainPoints[i + 1];
            if (timeInClip >= a.time && timeInClip <= b.time)
            {
                double t = (timeInClip - a.time) / juce::jmax(1e-6, b.time - a.time);
                return a.dB + (float)(t * (b.dB - a.dB));
            }
        }
        return gainPoints.back().dB;
    }

private:
    juce::AudioFormatManager* formatManager { nullptr };
    juce::File           file;
    double               startPosition { 0.0 };
    double               duration      { 0.0 };
    float                gain          { 1.0f };
    juce::String         name;
    juce::Colour         colour        { juce::Colour(0xff3a6ea5) };
    bool                 customColour  { false };
    double fadeInSecs  { 0.010 };  // プチノイズ防止用デフォルト 10ms
    double fadeOutSecs { 0.010 };
    double fileOffset  { 0.0 };  // ファイル内の開始オフセット（秒）
    FadeCurve fadeInCurve  { FadeCurve::Linear };
    FadeCurve fadeOutCurve { FadeCurve::Linear };

    std::vector<GainPoint> gainPoints;  // クリップゲインエンベロープ

    juce::AudioThumbnail thumbnail;
    std::unique_ptr<juce::AudioFormatReader> cachedReader;
    double                                   cachedSampleRate { 0.0 };

public:
    // 波形描画キャッシュ。同じ条件なら drawChannels を再実行せず Image を blit するだけ。
    struct WaveformCache
    {
        juce::Image image;
        int    width      { 0 };
        int    height     { 0 };
        double fileOffset { -1.0 };
        double duration   { -1.0 };
        float  gain       { -1.0f };
        double vZoom      { -1.0 };
        juce::uint32 colourARGB { 0 };
        // サムネイルの非同期デコード進捗。これをキーに含めないと、デコード途中で
        // 部分波形の Image を作ってキャッシュした後、完了しても再生成されず部分波形が固定される。
        juce::int64 samplesFinished { -1 };
        // 物理ピクセル倍率 (Retina = 2.0)。倍率が変わったら作り直す (ディスプレイ間移動)
        float pixelScale { 0.0f };

        // ── 巨大クリップ用 (キャッシュ上限超え scaledW>8192) の「可視範囲+マージン帯」キャッシュ ──
        // 以前は useCache=false の clip を毎ペイント fillPath で描き直していたが、JUCE の CALayer は
        // drawsAsynchronously のため CA::Transaction::commit がその描画完了をメインスレッドで待ち、
        // 重い波形 fillPath でメッセージスレッドが数百 ms 固まる (再生バー/UI のカクツキ)。
        // 可視範囲に左右マージンを付けた帯を 1 枚キャッシュし、blit の平行移動で描く。スクロールが
        // マージン内に収まる限りは再生成せず blit のみ (横スクロールのもたつき解消)。帯は scrollX を
        // 含まない「絶対ピクセル座標」(= time_sec * pxPerSec) で保持するので、再生バー移動でもキー不変。
        juce::Image  bigImage;
        double       bigBandL { 0.0 };         // 帯の左端 (絶対 px = time_sec * pxPerSec)
        double       bigBandR { -1.0 };        // 帯の右端 (絶対 px)。R<L で無効
        // 帯生成時のクリップ開始位置 (秒)。クリップの Move (fileOffset 不変) では帯の内容は
        // 変わらないため再生成せず、描画側が (現在の開始位置 − これ) だけ帯を平行移動して
        // blit する (これが無いと移動後も帯が旧位置に描かれて波形が枠から剥がれる)
        double       bigStartPos { 0.0 };
        int          bigHeight { 0 };
        double       bigPixelsPerBeat { -1.0 };
        double       bigBpm { -1.0 };
        double       bigFileOffset { -1.0 };
        float        bigGain { -1.0f };
        double       bigVZoom { -1.0 };
        juce::uint32 bigColourARGB { 0 };
        juce::int64  bigSamplesFinished { -1 };
        float        bigPixelScale { 0.0f };
    };
    WaveformCache& getWaveformCache() { return waveformCache; }
    // width=-1 で useCache 経路を、bigBandR=-1 + bigImage 解放で巨大クリップ経路を無効化する。
    void invalidateWaveformCache()
    {
        waveformCache.width = -1;
        waveformCache.bigBandR = -1.0;
        waveformCache.bigImage = juce::Image();
    }

private:
    WaveformCache waveformCache;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioClip)
};
