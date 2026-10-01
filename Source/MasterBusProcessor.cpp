#include "MasterBusProcessor.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <limits>

namespace
{
constexpr float silenceLufs = -100.0f;

float amplitudeFromDb(float decibels) noexcept
{
    return std::pow(10.0f, decibels * 0.05f);
}

float dbFromAmplitude(float amplitude) noexcept
{
    return 20.0f * std::log10(std::max(amplitude, 1.0e-9f));
}

float lufsFromMeanSquare(double meanSquare) noexcept
{
    if (!std::isfinite(meanSquare) || meanSquare <= 1.0e-12)
        return silenceLufs;
    return static_cast<float>(-0.691 + 10.0 * std::log10(meanSquare));
}

float finiteOrZero(float sample) noexcept
{
    return std::isfinite(sample) ? sample : 0.0f;
}
}

bool MasterBusProcessor::prepare(double sampleRate,
                                 int maximumBlockSize,
                                 int channelCount)
{
    if (!std::isfinite(sampleRate) || sampleRate < 8000.0
        || sampleRate > 384000.0 || maximumBlockSize <= 0
        || channelCount <= 0)
    {
        preparedSampleRate = 0.0;
        preparedChannels = 0;
        lookAheadSamples = 0;
        processedDelay.setSize(0, 0);
        dryDelay.setSize(0, 0);
        peakHistory.clear();
        reset();
        return false;
    }

    preparedSampleRate = sampleRate;
    preparedChannels = std::clamp(channelCount, 1, 2);
    lookAheadSamples = std::max(1,
        static_cast<int>(std::lround(preparedSampleRate * 0.005)));
    const auto capacity = lookAheadSamples + 1;
    processedDelay.setSize(preparedChannels, capacity, false, false, true);
    dryDelay.setSize(preparedChannels, capacity, false, false, true);
    peakHistory.assign(static_cast<size_t>(capacity), 0.0f);
    reset();
    return true;
}

void MasterBusProcessor::reset() noexcept
{
    processedDelay.clear();
    dryDelay.clear();
    std::fill(peakHistory.begin(), peakHistory.end(), 0.0f);
    delayWritePosition = 0;
    delayedSamplesAvailable = 0;
    rollingInputMeanSquare = 0.0;
    rollingProcessedMeanSquare = 0.0;
    rollingOutputMeanSquare = 0.0;
    loudnessGain = 1.0f;
    comparisonGain = 1.0f;
    limiterGain = 1.0f;
    limiterWasEnabled = limiterEnabledValue.load(std::memory_order_relaxed);
    comparisonGainDbValue.store(0.0f, std::memory_order_relaxed);
    publishedInputLoudnessLufs.store(silenceLufs,
                                      std::memory_order_relaxed);
    publishedProcessedLoudnessLufs.store(silenceLufs,
                                          std::memory_order_relaxed);
    publishedOutputLoudnessLufs.store(silenceLufs,
                                       std::memory_order_relaxed);
    publishedOutputPeak.store(0.0f, std::memory_order_relaxed);
    publishedGainReductionDb.store(0.0f, std::memory_order_relaxed);
}

void MasterBusProcessor::setSettings(
    const MasteringSettings& newSettings) noexcept
{
    enabledValue.store(newSettings.enabled, std::memory_order_relaxed);
    limiterEnabledValue.store(newSettings.limiterEnabled,
                              std::memory_order_relaxed);
    ceilingDbValue.store(sanitiseDb(newSettings.ceilingDb,
                                    -12.0f,
                                    -0.1f,
                                    -1.0f),
                         std::memory_order_relaxed);
    targetLufsValue.store(sanitiseDb(newSettings.targetLufs,
                                     -30.0f,
                                     -8.0f,
                                     -14.0f),
                          std::memory_order_relaxed);
    auditionProcessedValue.store(newSettings.auditionProcessed,
                                 std::memory_order_relaxed);
}

MasteringSettings MasterBusProcessor::getSettings() const noexcept
{
    MasteringSettings result;
    result.enabled = enabledValue.load(std::memory_order_relaxed);
    result.limiterEnabled = limiterEnabledValue.load(std::memory_order_relaxed);
    result.ceilingDb = ceilingDbValue.load(std::memory_order_relaxed);
    result.targetLufs = targetLufsValue.load(std::memory_order_relaxed);
    result.auditionProcessed = auditionProcessedValue.load(
        std::memory_order_relaxed);
    return result;
}

void MasterBusProcessor::setComparisonLoudness(float dryLufs,
                                                float processedLufs) noexcept
{
    comparisonGainDbValue.store(calculateLevelMatchGainDb(dryLufs,
                                                           processedLufs),
                                std::memory_order_relaxed);
}

void MasterBusProcessor::clearComparisonLoudness() noexcept
{
    comparisonGainDbValue.store(0.0f, std::memory_order_relaxed);
}

void MasterBusProcessor::process(juce::AudioBuffer<float>& buffer,
                                 int startSample,
                                 int numSamples) noexcept
{
    if (preparedChannels <= 0 || preparedSampleRate <= 0.0
        || buffer.getNumChannels() <= 0 || buffer.getNumSamples() <= 0)
        return;

    const auto safeStart = std::clamp(startSample, 0, buffer.getNumSamples());
    const auto available = buffer.getNumSamples() - safeStart;
    const auto safeCount = numSamples < 0 ? available
        : std::clamp(numSamples, 0, available);
    const auto channels = std::min({ preparedChannels,
                                     buffer.getNumChannels(),
                                     2 });
    if (safeCount <= 0 || channels <= 0)
        return;

    const auto processorEnabled = enabledValue.load(std::memory_order_relaxed);
    const auto loudnessCoefficient = std::exp(
        -1.0 / (0.400 * preparedSampleRate));
    std::array<float*, 2> audio {
        buffer.getWritePointer(0, safeStart),
        channels > 1 ? buffer.getWritePointer(1, safeStart) : nullptr
    };

    if (!processorEnabled)
    {
        // OFF中に古いルックアヘッド音声を残すと、再びONにした瞬間に
        // 前回の音が数msだけ混ざる。メーターは動かしつつ処理履歴だけを
        // 安全な初期状態へ戻しておく。
        processedDelay.clear();
        dryDelay.clear();
        std::fill(peakHistory.begin(), peakHistory.end(), 0.0f);
        delayWritePosition = 0;
        delayedSamplesAvailable = 0;
        loudnessGain = 1.0f;
        comparisonGain = 1.0f;
        limiterGain = 1.0f;
        limiterWasEnabled = limiterEnabledValue.load(
            std::memory_order_relaxed);

        float blockPeak = 0.0f;
        for (int sampleIndex = 0; sampleIndex < safeCount; ++sampleIndex)
        {
            double sampleMeanSquare = 0.0;
            for (int channel = 0; channel < channels; ++channel)
            {
                const auto sample = finiteOrZero(
                    audio[static_cast<size_t>(channel)][sampleIndex]);
                sampleMeanSquare += static_cast<double>(sample) * sample;
                blockPeak = std::max(blockPeak, std::abs(sample));
            }
            sampleMeanSquare /= static_cast<double>(channels);
            rollingInputMeanSquare =
                loudnessCoefficient * rollingInputMeanSquare
                + (1.0 - loudnessCoefficient) * sampleMeanSquare;
            rollingProcessedMeanSquare =
                loudnessCoefficient * rollingProcessedMeanSquare
                + (1.0 - loudnessCoefficient) * sampleMeanSquare;
            rollingOutputMeanSquare =
                loudnessCoefficient * rollingOutputMeanSquare
                + (1.0 - loudnessCoefficient) * sampleMeanSquare;
        }

        publishedInputLoudnessLufs.store(
            lufsFromMeanSquare(rollingInputMeanSquare),
            std::memory_order_relaxed);
        publishedProcessedLoudnessLufs.store(
            lufsFromMeanSquare(rollingProcessedMeanSquare),
            std::memory_order_relaxed);
        publishedOutputLoudnessLufs.store(
            lufsFromMeanSquare(rollingOutputMeanSquare),
            std::memory_order_relaxed);
        publishedOutputPeak.store(blockPeak, std::memory_order_relaxed);
        publishedGainReductionDb.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const auto limiterEnabled = limiterEnabledValue.load(
        std::memory_order_relaxed);
    if (limiterEnabled != limiterWasEnabled)
    {
        std::fill(peakHistory.begin(), peakHistory.end(), 0.0f);
        delayWritePosition = 0;
        delayedSamplesAvailable = 0;
        limiterGain = 1.0f;
        limiterWasEnabled = limiterEnabled;
    }
    const auto auditionProcessed = auditionProcessedValue.load(
        std::memory_order_relaxed);
    const auto ceiling = amplitudeFromDb(
        ceilingDbValue.load(std::memory_order_relaxed));
    const auto targetLufs = targetLufsValue.load(std::memory_order_relaxed);
    const auto comparisonTargetGain = amplitudeFromDb(sanitiseDb(
        comparisonGainDbValue.load(std::memory_order_relaxed),
        -12.0f,
        12.0f,
        0.0f));
    const auto loudnessGainCoefficient = static_cast<float>(std::exp(
        -1.0 / (0.750 * preparedSampleRate)));
    const auto comparisonGainCoefficient = static_cast<float>(std::exp(
        -1.0 / (0.050 * preparedSampleRate)));
    const auto limiterReleaseCoefficient = static_cast<float>(std::exp(
        -1.0 / (0.080 * preparedSampleRate)));

    std::array<float*, 2> processedDelayData {
        processedDelay.getWritePointer(0),
        preparedChannels > 1 ? processedDelay.getWritePointer(1) : nullptr
    };
    std::array<float*, 2> dryDelayData {
        dryDelay.getWritePointer(0),
        preparedChannels > 1 ? dryDelay.getWritePointer(1) : nullptr
    };
    const auto delayCapacity = processedDelay.getNumSamples();
    float blockPeak = 0.0f;

    for (int sampleIndex = 0; sampleIndex < safeCount; ++sampleIndex)
    {
        std::array<float, 2> dry {};
        double sampleMeanSquare = 0.0;
        for (int channel = 0; channel < channels; ++channel)
        {
            dry[static_cast<size_t>(channel)] = finiteOrZero(
                audio[static_cast<size_t>(channel)][sampleIndex]);
            const auto value = static_cast<double>(dry[static_cast<size_t>(channel)]);
            sampleMeanSquare += value * value;
        }
        sampleMeanSquare /= static_cast<double>(channels);
        rollingInputMeanSquare =
            loudnessCoefficient * rollingInputMeanSquare
            + (1.0 - loudnessCoefficient) * sampleMeanSquare;
        const auto currentLufs = lufsFromMeanSquare(rollingInputMeanSquare);

        float targetGain = 1.0f;
        if (currentLufs > silenceLufs + 1.0f)
        {
            const auto requiredDb = std::clamp(targetLufs - currentLufs,
                                               -12.0f,
                                               6.0f);
            targetGain = amplitudeFromDb(requiredDb);
        }
        loudnessGain = targetGain
                     + loudnessGainCoefficient * (loudnessGain - targetGain);
        comparisonGain = comparisonTargetGain
            + comparisonGainCoefficient
                * (comparisonGain - comparisonTargetGain);
        if (!std::isfinite(comparisonGain))
            comparisonGain = 1.0f;

        double processedMeanSquare = 0.0;
        for (int channel = 0; channel < channels; ++channel)
        {
            const auto processedBeforeComparison = finiteOrZero(
                dry[static_cast<size_t>(channel)] * loudnessGain);
            processedMeanSquare +=
                static_cast<double>(processedBeforeComparison)
                * processedBeforeComparison;
        }
        processedMeanSquare /= static_cast<double>(channels);
        rollingProcessedMeanSquare =
            loudnessCoefficient * rollingProcessedMeanSquare
            + (1.0 - loudnessCoefficient) * processedMeanSquare;

        const auto processedInputGain = loudnessGain * comparisonGain;
        float processedPeak = 0.0f;
        for (int channel = 0; channel < channels; ++channel)
        {
            const auto processed = dry[static_cast<size_t>(channel)]
                                 * processedInputGain;
            processedPeak = std::max(processedPeak, std::abs(processed));
            if (limiterEnabled)
            {
                processedDelayData[static_cast<size_t>(channel)]
                                  [delayWritePosition] = processed;
                dryDelayData[static_cast<size_t>(channel)]
                           [delayWritePosition] = dry[static_cast<size_t>(channel)];
            }
        }

        if (!limiterEnabled)
        {
            double outputMeanSquare = 0.0;
            for (int channel = 0; channel < channels; ++channel)
            {
                const auto output = auditionProcessed
                    ? dry[static_cast<size_t>(channel)] * processedInputGain
                    : dry[static_cast<size_t>(channel)];
                const auto safeOutput = finiteOrZero(output);
                audio[static_cast<size_t>(channel)][sampleIndex] = safeOutput;
                blockPeak = std::max(blockPeak, std::abs(safeOutput));
                outputMeanSquare += static_cast<double>(safeOutput)
                                  * safeOutput;
            }
            outputMeanSquare /= static_cast<double>(channels);
            rollingOutputMeanSquare =
                loudnessCoefficient * rollingOutputMeanSquare
                + (1.0 - loudnessCoefficient) * outputMeanSquare;
            continue;
        }

        peakHistory[static_cast<size_t>(delayWritePosition)] = processedPeak;
        float lookAheadPeak = 0.0f;
        for (const auto peak : peakHistory)
            lookAheadPeak = std::max(lookAheadPeak, peak);
        const auto requestedLimiterGain = lookAheadPeak > ceiling
            ? ceiling / std::max(lookAheadPeak, 1.0e-9f) : 1.0f;
        if (requestedLimiterGain < limiterGain)
            limiterGain = requestedLimiterGain;
        else
            limiterGain = requestedLimiterGain
                        + limiterReleaseCoefficient
                            * (limiterGain - requestedLimiterGain);

        auto readPosition = delayWritePosition + 1;
        if (readPosition >= delayCapacity)
            readPosition = 0;
        const bool delayReady = delayedSamplesAvailable >= lookAheadSamples;
        double outputMeanSquare = 0.0;
        for (int channel = 0; channel < channels; ++channel)
        {
            const auto processed = delayReady
                ? processedDelayData[static_cast<size_t>(channel)][readPosition]
                    * limiterGain
                : 0.0f;
            const auto dryOutput = delayReady
                ? dryDelayData[static_cast<size_t>(channel)][readPosition]
                : 0.0f;
            const auto output = auditionProcessed
                ? std::clamp(processed, -ceiling, ceiling)
                : dryOutput;
            const auto safeOutput = finiteOrZero(output);
            audio[static_cast<size_t>(channel)][sampleIndex] = safeOutput;
            blockPeak = std::max(blockPeak, std::abs(safeOutput));
            outputMeanSquare += static_cast<double>(safeOutput) * safeOutput;
        }
        outputMeanSquare /= static_cast<double>(channels);
        rollingOutputMeanSquare =
            loudnessCoefficient * rollingOutputMeanSquare
            + (1.0 - loudnessCoefficient) * outputMeanSquare;

        if (++delayWritePosition >= delayCapacity)
            delayWritePosition = 0;
        delayedSamplesAvailable = std::min(lookAheadSamples,
                                           delayedSamplesAvailable + 1);
    }

    publishedInputLoudnessLufs.store(
        lufsFromMeanSquare(rollingInputMeanSquare),
        std::memory_order_relaxed);
    publishedProcessedLoudnessLufs.store(
        lufsFromMeanSquare(rollingProcessedMeanSquare),
        std::memory_order_relaxed);
    publishedOutputLoudnessLufs.store(
        lufsFromMeanSquare(rollingOutputMeanSquare),
        std::memory_order_relaxed);
    publishedOutputPeak.store(blockPeak, std::memory_order_relaxed);
    publishedGainReductionDb.store(
        std::min(0.0f, dbFromAmplitude(limiterGain)),
        std::memory_order_relaxed);
}

float MasterBusProcessor::getApproximateLoudnessLufs() const noexcept
{
    return publishedOutputLoudnessLufs.load(std::memory_order_relaxed);
}

float MasterBusProcessor::getApproximateInputLoudnessLufs() const noexcept
{
    return publishedInputLoudnessLufs.load(std::memory_order_relaxed);
}

float MasterBusProcessor::getApproximateProcessedLoudnessLufs() const noexcept
{
    return publishedProcessedLoudnessLufs.load(std::memory_order_relaxed);
}

float MasterBusProcessor::getLastOutputPeak() const noexcept
{
    return publishedOutputPeak.load(std::memory_order_relaxed);
}

float MasterBusProcessor::getLimiterGainReductionDb() const noexcept
{
    return publishedGainReductionDb.load(std::memory_order_relaxed);
}

float MasterBusProcessor::estimateApproximateLoudnessLufs(
    const juce::AudioBuffer<float>& buffer,
    int startSample,
    int numSamples) noexcept
{
    if (buffer.getNumChannels() <= 0 || buffer.getNumSamples() <= 0)
        return silenceLufs;

    const auto safeStart = std::clamp(startSample, 0, buffer.getNumSamples());
    const auto available = buffer.getNumSamples() - safeStart;
    const auto safeCount = numSamples < 0 ? available
        : std::clamp(numSamples, 0, available);
    if (safeCount <= 0)
        return silenceLufs;

    long double sumSquares = 0.0;
    std::uint64_t validValues = 0;
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        const auto* samples = buffer.getReadPointer(channel, safeStart);
        for (int sample = 0; sample < safeCount; ++sample)
        {
            if (!std::isfinite(samples[sample]))
                continue;
            const auto value = static_cast<long double>(samples[sample]);
            sumSquares += value * value;
            ++validValues;
        }
    }
    if (validValues == 0)
        return silenceLufs;
    return lufsFromMeanSquare(static_cast<double>(
        sumSquares / static_cast<long double>(validValues)));
}

float MasterBusProcessor::calculateLevelMatchGainDb(
    float referenceLufs,
    float candidateLufs,
    float maximumAdjustmentDb) noexcept
{
    if (!std::isfinite(referenceLufs) || !std::isfinite(candidateLufs)
        || !std::isfinite(maximumAdjustmentDb)
        || referenceLufs <= silenceLufs || candidateLufs <= silenceLufs)
        return 0.0f;
    const auto limit = std::clamp(std::abs(maximumAdjustmentDb), 0.0f, 24.0f);
    return std::clamp(referenceLufs - candidateLufs, -limit, limit);
}

float MasterBusProcessor::sanitiseDb(float value,
                                     float minimum,
                                     float maximum,
                                     float fallback) noexcept
{
    return std::isfinite(value) ? std::clamp(value, minimum, maximum)
                                : fallback;
}
