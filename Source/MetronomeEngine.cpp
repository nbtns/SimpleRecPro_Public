#include "MetronomeEngine.h"
#include <cmath>

MetronomeEngine::MetronomeEngine()
{
}

MetronomeEngine::~MetronomeEngine()
{
}

void MetronomeEngine::prepare(double sampleRate)
{
    currentSampleRate = sampleRate;
    // クリック音の長さ: 約20ms
    clickDurationSamples = static_cast<int>(sampleRate * 0.02);
}

void MetronomeEngine::setTimeSignature(int numerator, int denominator)
{
    timeSignatureNumerator.store(juce::jlimit(1, 12, numerator));
    timeSignatureDenominator.store(juce::jlimit(4, 8, denominator));
}

float MetronomeEngine::generateClickSample(int sampleIndex, bool isAccent) const
{
    if (sampleIndex >= clickDurationSamples) return 0.0f;
    
    // 周波数: 強拍1000Hz, 弱拍800Hz（Web版と同じ）
    float freq = isAccent ? 1000.0f : 800.0f;
    
    // サイン波で生成
    float phase = static_cast<float>(sampleIndex) / static_cast<float>(currentSampleRate);
    float sample = std::sin(2.0f * juce::MathConstants<float>::pi * freq * phase);
    
    // エンベロープ: 急速な減衰（指数関数的フェードアウト）
    float envelope = std::exp(-8.0f * static_cast<float>(sampleIndex) / static_cast<float>(clickDurationSamples));
    
    // 強拍は音量1.0、弱拍は0.6
    float accentGain = isAccent ? 1.0f : 0.6f;
    
    return sample * envelope * accentGain * volume.load();
}

void MetronomeEngine::processBlock(juce::AudioBuffer<float>& buffer, int numSamples,
                                    int64_t playbackPositionInSamples,
                                    bool forceEnabled)
{
    if (!forceEnabled && !isEnabled.load()) return;

    const int currentBpm = bpm.load();
    const int numerator = timeSignatureNumerator.load();
    const int denominator = timeSignatureDenominator.load();
    if (currentBpm <= 0) return;
    
    // 1拍あたりのサンプル数を計算
    double samplesPerBeat = (currentSampleRate * 60.0) / static_cast<double>(currentBpm);
    
    // 8分音符の場合は拍の長さを半分に
    if (denominator == 8)
        samplesPerBeat *= 0.5;
    
    for (int i = 0; i < numSamples; ++i)
    {
        int64_t currentSample = playbackPositionInSamples + i;
        
        // 現在のサンプル位置がどの拍に属するかを計算
        double beatPosition = static_cast<double>(currentSample) / samplesPerBeat;
        int beatIndex = static_cast<int>(std::floor(beatPosition));
        int sampleWithinBeat = static_cast<int>(currentSample - static_cast<int64_t>(beatIndex * samplesPerBeat));
        
        // クリック音の範囲内か
        if (sampleWithinBeat < clickDurationSamples)
        {
            // 強拍かどうかを判定（小節の先頭）
            int beatInMeasure = beatIndex % numerator;
            bool isAccent = (beatInMeasure == 0);
            
            float clickSample = generateClickSample(sampleWithinBeat, isAccent);
            
            // 全チャンネルに加算
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                buffer.getWritePointer(ch)[i] += clickSample;
            }
        }
    }
}

void MetronomeEngine::processBlock(juce::AudioBuffer<float>& buffer,
                                    int numSamples,
                                    int64_t playbackPositionInSamples,
                                    const TempoMap& tempoMap,
                                    bool forceEnabled)
{
    if ((!forceEnabled && !isEnabled.load())
        || numSamples <= 0
        || currentSampleRate <= 0.0)
        return;

    const int samplesToProcess = std::min(numSamples, buffer.getNumSamples());
    if (samplesToProcess <= 0)
        return;

    const auto blockStartSample = std::max<int64_t>(0,
                                                     playbackPositionInSamples);
    const auto blockEndSample = blockStartSample + samplesToProcess - 1;
    const double searchStartSeconds = static_cast<double>(std::max<int64_t>(
        0, blockStartSample - clickDurationSamples)) / currentSampleRate;
    const double blockEndSeconds = static_cast<double>(blockEndSample)
                                 / currentSampleRate;

    const double firstBeat = std::ceil(
        tempoMap.secondsToBeats(searchStartSeconds) - 1.0e-9);
    const double lastBeat = std::floor(
        tempoMap.secondsToBeats(blockEndSeconds) + 1.0e-9);
    constexpr int maximumClicksPerBlock = 1024;
    int clicksProcessed = 0;
    for (double beat = firstBeat;
         beat <= lastBeat && clicksProcessed < maximumClicksPerBlock;
         beat += 1.0, ++clicksProcessed)
    {
        const double beatTime = tempoMap.beatsToSeconds(beat);
        const auto beatSample = static_cast<int64_t>(
            std::llround(beatTime * currentSampleRate));
        const int firstOutputSample = static_cast<int>(std::max<int64_t>(
            0, beatSample - blockStartSample));
        const int firstClickSample = static_cast<int>(std::max<int64_t>(
            0, blockStartSample - beatSample));
        const int clickSamples = std::min(
            samplesToProcess - firstOutputSample,
            clickDurationSamples - firstClickSample);
        if (clickSamples <= 0)
            continue;

        const bool isAccent = tempoMap.getBeatPosition(beatTime).beatInBar == 0;
        for (int sample = 0; sample < clickSamples; ++sample)
        {
            const float clickSample = generateClickSample(
                firstClickSample + sample, isAccent);
            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                buffer.getWritePointer(channel)[firstOutputSample + sample]
                    += clickSample;
        }
    }
}
