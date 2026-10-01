#pragma once

#include <JuceHeader.h>

#include "DawFeatureTypes.h"

#include <atomic>
#include <vector>

/**
 * Real-time-safe master peak protection and approximate loudness matching.
 *
 * `prepare()` allocates all look-ahead storage. `process()` is noexcept and
 * allocation-free. Loudness is an intentionally lightweight LUFS estimate,
 * suitable for an on-screen guide rather than compliance measurement.
 */
class MasterBusProcessor
{
public:
    MasterBusProcessor() = default;

    /** Allocates a 5 ms look-ahead buffer. Returns false for invalid input. */
    bool prepare(double sampleRate, int maximumBlockSize, int channelCount);

    /** Clears delay, loudness, target-gain, and limiter history. */
    void reset() noexcept;

    /** Publishes bounded settings to the audio thread. */
    void setSettings(const MasteringSettings& newSettings) noexcept;
    MasteringSettings getSettings() const noexcept;

    /**
     * Sets loudness values captured from dry and processed auditions. The
     * processed path is then adjusted by at most +/-12 dB for fair comparison.
     */
    void setComparisonLoudness(float dryLufs, float processedLufs) noexcept;
    void clearComparisonLoudness() noexcept;

    /** Processes a valid section of the supplied master buffer in place. */
    void process(juce::AudioBuffer<float>& buffer,
                 int startSample = 0,
                 int numSamples = -1) noexcept;

    /** Current 400 ms RMS-based loudness guide for the audible output. */
    float getApproximateLoudnessLufs() const noexcept;

    /** Current 400 ms RMS-based loudness guide before mastering. */
    float getApproximateInputLoudnessLufs() const noexcept;

    /**
     * Current loudness after target matching but before the comparison gain.
     * This comparison-independent estimate avoids feedback when level-matching
     * dry and processed auditions.
     */
    float getApproximateProcessedLoudnessLufs() const noexcept;
    float getLastOutputPeak() const noexcept;
    float getLimiterGainReductionDb() const noexcept;

    /** Latency introduced by the active look-ahead limiter. */
    int getLatencySamples() const noexcept
    {
        return enabledValue.load(std::memory_order_relaxed)
            && limiterEnabledValue.load(std::memory_order_relaxed)
            ? lookAheadSamples : 0;
    }

    /** Offline, allocation-free loudness estimate for a complete buffer. */
    static float estimateApproximateLoudnessLufs(
        const juce::AudioBuffer<float>& buffer,
        int startSample = 0,
        int numSamples = -1) noexcept;

    /** Gain needed to match `candidateLufs` to `referenceLufs`, clamped. */
    static float calculateLevelMatchGainDb(float referenceLufs,
                                           float candidateLufs,
                                           float maximumAdjustmentDb = 12.0f) noexcept;

private:
    static float sanitiseDb(float value, float minimum, float maximum,
                            float fallback) noexcept;

    std::atomic<bool> enabledValue { false };
    std::atomic<bool> limiterEnabledValue { true };
    std::atomic<float> ceilingDbValue { -1.0f };
    std::atomic<float> targetLufsValue { -14.0f };
    std::atomic<bool> auditionProcessedValue { true };
    std::atomic<float> comparisonGainDbValue { 0.0f };

    std::atomic<float> publishedInputLoudnessLufs { -100.0f };
    std::atomic<float> publishedProcessedLoudnessLufs { -100.0f };
    std::atomic<float> publishedOutputLoudnessLufs { -100.0f };
    std::atomic<float> publishedOutputPeak { 0.0f };
    std::atomic<float> publishedGainReductionDb { 0.0f };

    double preparedSampleRate = 0.0;
    int preparedChannels = 0;
    int lookAheadSamples = 0;
    int delayWritePosition = 0;
    int delayedSamplesAvailable = 0;
    juce::AudioBuffer<float> processedDelay;
    juce::AudioBuffer<float> dryDelay;
    std::vector<float> peakHistory;

    double rollingInputMeanSquare = 0.0;
    double rollingProcessedMeanSquare = 0.0;
    double rollingOutputMeanSquare = 0.0;
    float loudnessGain = 1.0f;
    float comparisonGain = 1.0f;
    float limiterGain = 1.0f;
    bool limiterWasEnabled = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MasterBusProcessor)
};
