// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "PitchEngine.h"
#include "pitchcore/PitchShiftEngine.h"
#include <vector>
#include <cstring>

namespace PitchEngine
{
    bool shiftBuffer(const juce::AudioBuffer<float>& input,
                     double sampleRate,
                     double semitones,
                     juce::AudioBuffer<float>& output,
                     const std::function<bool(double)>& onProgress)
    {
        const int numCh = input.getNumChannels();
        const int inLen = input.getNumSamples();
        if (numCh <= 0 || inLen <= 0 || sampleRate <= 0.0) return false;

        std::vector<const float*> ptrs((size_t) numCh, nullptr);
        for (int ch = 0; ch < numCh; ++ch)
            ptrs[(size_t) ch] = input.getReadPointer(ch);

        std::vector<std::vector<float>> result;
        if (! pitchcore::shiftPitch(ptrs.data(), numCh, inLen, sampleRate,
                                    semitones, result, onProgress))
            return false;

        output.setSize(numCh, inLen);
        for (int ch = 0; ch < numCh; ++ch)
            std::memcpy(output.getWritePointer(ch), result[(size_t) ch].data(),
                        (size_t) inLen * sizeof(float));
        return true;
    }

    bool processFile(const juce::File& inputFile,
                     const juce::File& outputFile,
                     juce::AudioFormatManager& fmt,
                     double semitones,
                     int outputBits,
                     std::function<void(double)> onProgress)
    {
        std::unique_ptr<juce::AudioFormatReader> reader(fmt.createReaderFor(inputFile));
        if (! reader || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
            return false;

        const int numCh = (int) reader->numChannels;
        if (reader->lengthInSamples > (juce::int64) std::numeric_limits<int>::max())
            return false;
        const int totalSamples = (int) reader->lengthInSamples;
        const double sampleRate = reader->sampleRate;

        juce::AudioBuffer<float> inBuf(numCh, totalSamples);
        reader->read(&inBuf, 0, totalSamples, 0, true, true);

        juce::AudioBuffer<float> outBuf;
        if (! shiftBuffer(inBuf, sampleRate, semitones, outBuf,
                          [&onProgress](double p)
                          { if (onProgress) onProgress(p); return true; }))
            return false;

        auto stream = std::make_unique<juce::FileOutputStream>(outputFile);
        if (! stream->openedOk()) return false;
        stream->setPosition(0);
        stream->truncate();
        juce::WavAudioFormat wav;
        const bool useFloat = (outputBits >= 32);
        auto opts = juce::AudioFormatWriterOptions{}
                        .withSampleRate(sampleRate)
                        .withNumChannels((juce::uint32) numCh)
                        .withBitsPerSample(outputBits)
                        .withSampleFormat(useFloat
                            ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                            : juce::AudioFormatWriterOptions::SampleFormat::integral);
        std::unique_ptr<juce::OutputStream> outStream = std::move(stream);
        auto writer = wav.createWriterFor(outStream, opts);
        if (writer == nullptr) return false;
        writer->writeFromAudioSampleBuffer(outBuf, 0, outBuf.getNumSamples());
        writer->flush();
        return true;
    }

    bool shiftBufferAndTime(const juce::AudioBuffer<float>& input,
                            double sampleRate, double semitones, double timeRatio,
                            juce::AudioBuffer<float>& output,
                            const std::function<bool(double)>& onProgress)
    {
        const int numCh = input.getNumChannels();
        const int inLen = input.getNumSamples();
        if (numCh <= 0 || inLen <= 0 || sampleRate <= 0.0) return false;

        std::vector<const float*> ptrs((size_t) numCh, nullptr);
        for (int ch = 0; ch < numCh; ++ch)
            ptrs[(size_t) ch] = input.getReadPointer(ch);

        std::vector<std::vector<float>> result;
        if (! pitchcore::shiftPitchAndTime(ptrs.data(), numCh, inLen, sampleRate,
                                           semitones, timeRatio, result, onProgress))
            return false;

        const int outLen = (int) result[0].size();
        output.setSize(numCh, outLen);
        for (int ch = 0; ch < numCh; ++ch)
            std::memcpy(output.getWritePointer(ch), result[(size_t) ch].data(),
                        (size_t) outLen * sizeof(float));
        return true;
    }

    bool renderRegion(const juce::File& inputFile, const juce::File& outputFile,
                      juce::AudioFormatManager& fmt,
                      juce::int64 startSample, juce::int64 numSamples,
                      double semitones, double timeRatio,
                      const std::function<bool(double)>& onProgress)
    {
        std::unique_ptr<juce::AudioFormatReader> reader(fmt.createReaderFor(inputFile));
        if (! reader || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
            return false;

        startSample = juce::jlimit<juce::int64>(0, reader->lengthInSamples, startSample);
        numSamples  = juce::jmin(numSamples, reader->lengthInSamples - startSample);
        if (numSamples <= 0 || numSamples > (juce::int64) std::numeric_limits<int>::max() / 4)
            return false;

        const int numCh = (int) reader->numChannels;
        const double sampleRate = reader->sampleRate;
        juce::AudioBuffer<float> inBuf(numCh, (int) numSamples);
        reader->read(&inBuf, 0, (int) numSamples, startSample, true, true);
        reader.reset();

        juce::AudioBuffer<float> outBuf;
        if (! shiftBufferAndTime(inBuf, sampleRate, semitones, timeRatio, outBuf, onProgress))
            return false;

        outputFile.deleteFile();
        std::unique_ptr<juce::OutputStream> outStream = outputFile.createOutputStream();
        if (outStream == nullptr) return false;
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor(outStream, juce::AudioFormatWriterOptions{}
                          .withSampleRate(sampleRate)
                          .withNumChannels((juce::uint32) numCh)
                          .withBitsPerSample(32)
                          .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (writer == nullptr) { outputFile.deleteFile(); return false; }
        const bool ok = writer->writeFromAudioSampleBuffer(outBuf, 0, outBuf.getNumSamples());
        writer.reset();
        if (! ok) outputFile.deleteFile();
        return ok;
    }
}
