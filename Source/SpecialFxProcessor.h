#pragma once

#include <JuceHeader.h>

#include "DawFeatureTypes.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

/**
 * Allocation-free built-in creative effects for real-time and offline use.
 *
 * One instance represents one effect region. `prepare()` validates the audio
 * format and initialises all state; `process()` is noexcept, lock-free and
 * performs no allocation. Settings are published to the audio thread through
 * bounded atomics.
 */
class SpecialFxProcessor
{
public:
    struct Settings
    {
        bool enabled = false;
        SpecialFxType type = SpecialFxType::none;
        float amount = 0.5f;
        float wet = 1.0f;

        // When false the effect applies to every sample passed to process().
        bool limitToRegion = false;
        std::int64_t regionStartSample = 0;
        std::int64_t regionEndSampleExclusive =
            std::numeric_limits<std::int64_t>::max();
        int fadeSamples = 256;

        // Noise is derived from this seed and the absolute timeline sample,
        // so playback and offline export produce the same result.
        std::uint32_t noiseSeed = 0x53a9b4d1u;
    };

    SpecialFxProcessor() = default;

    /** Initialises processor state. Supports mono or stereo audio. */
    bool prepare(double sampleRate, int maximumBlockSize, int channelCount);

    /** Clears filter, smoothing and sample-hold history. */
    void reset() noexcept;

    /** Publishes sanitised settings without allocating or taking a lock. */
    void setSettings(const Settings& newSettings) noexcept;
    Settings getSettings() const noexcept;

    /**
     * Processes a section of `buffer` in place.
     *
     * `timelineStartSample` is the absolute timeline position corresponding
     * to the first sample in the processed range (not buffer sample zero).
     * This makes region fades and generated noise independent of block size.
     */
    void process(juce::AudioBuffer<float>& buffer,
                 int startSample,
                 int numSamples,
                 std::int64_t timelineStartSample) noexcept;

private:
    static float sanitiseUnit(float value, float fallback) noexcept;
    static float finiteOrZero(float value) noexcept;
    static float onePoleAmount(double cutoff, double sampleRate) noexcept;
    static float deterministicNoise(std::int64_t timelineSample,
                                    int channel,
                                    std::uint32_t seed) noexcept;
    static float regionEnvelope(std::int64_t timelineSample,
                                std::int64_t regionStart,
                                std::int64_t regionEnd,
                                int fadeSamples) noexcept;

    std::atomic<bool> enabledValue { false };
    std::atomic<int> typeValue { static_cast<int>(SpecialFxType::none) };
    std::atomic<float> amountValue { 0.5f };
    std::atomic<float> wetValue { 1.0f };
    std::atomic<bool> limitToRegionValue { false };
    std::atomic<std::int64_t> regionStartValue { 0 };
    std::atomic<std::int64_t> regionEndValue {
        std::numeric_limits<std::int64_t>::max()
    };
    std::atomic<int> fadeSamplesValue { 256 };
    std::atomic<std::uint32_t> noiseSeedValue { 0x53a9b4d1u };

    double preparedSampleRate = 0.0;
    int preparedChannels = 0;

    float currentAmount = 0.5f;
    float currentWet = 0.0f;
    std::array<float, 2> muffledLowState {};
    std::array<float, 2> radioHighPassLowState {};
    std::array<float, 2> radioLowPassState {};
    std::array<float, 2> heldBitcrushSample {};
    std::array<bool, 2> heldBitcrushSampleValid {};
    std::int64_t expectedNextTimelineSample =
        std::numeric_limits<std::int64_t>::min();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpecialFxProcessor)
};
