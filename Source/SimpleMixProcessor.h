#pragma once

#include <JuceHeader.h>

#include "DawFeatureTypes.h"

#include <array>
#include <atomic>

/**
 * Lightweight beginner-facing vocal processing for real-time and offline use.
 *
 * `prepare()` performs all allocation. `process()` is noexcept, takes no locks,
 * and performs no allocation, so one instance may live in each track's audio
 * path. Parameter setters publish bounded atomic values to the audio thread.
 */
class SimpleMixProcessor
{
public:
    SimpleMixProcessor() = default;

    /** Allocates internal ambience storage. Returns false for invalid input. */
    bool prepare(double sampleRate, int maximumBlockSize, int channelCount);

    /** Clears filter, envelope, compressor, and ambience history. */
    void reset() noexcept;

    /**
     * Publishes custom settings. Brightness is clamped to [-1, 1]; ambience
     * and stability are clamped to [0, 1].
     */
    void setSettings(const SimpleMixSettings& newSettings) noexcept;

    /** Publishes non-destructive gate/de-ess settings, clamped to [0, 1]. */
    void setNoiseReductionSettings(
        const NoiseReductionSettings& newSettings) noexcept;

    /** Returns the currently published settings. */
    SimpleMixSettings getSettings() const noexcept;
    NoiseReductionSettings getNoiseReductionSettings() const noexcept;

    /** Returns safe starting values for the named beginner preset. */
    static SimpleMixSettings makePresetSettings(MixPreset preset) noexcept;

    /** Convenience for publishing `makePresetSettings(preset)`. */
    void setPreset(MixPreset preset) noexcept;

    /**
     * Processes a section of `buffer` in place. Invalid or unprepared ranges
     * are ignored. At most the channel count supplied to `prepare()` is used.
     */
    void process(juce::AudioBuffer<float>& buffer,
                 int startSample = 0,
                 int numSamples = -1) noexcept;

private:
    static float sanitiseBipolar(float value) noexcept;
    static float sanitiseUnit(float value) noexcept;

    std::atomic<bool> mixEnabled { false };
    std::atomic<int> presetValue { static_cast<int>(MixPreset::flat) };
    std::atomic<float> brightnessValue { 0.0f };
    std::atomic<float> ambienceValue { 0.0f };
    std::atomic<float> stabilityValue { 0.0f };

    std::atomic<bool> noiseEnabled { false };
    std::atomic<float> noiseAmountValue { 0.0f };
    std::atomic<float> deEssAmountValue { 0.0f };

    double preparedSampleRate = 0.0;
    int preparedChannels = 0;
    int ambienceWritePosition = 0;
    std::array<int, 2> ambienceDelaySamples {};
    juce::AudioBuffer<float> ambienceDelay;

    std::array<float, 2> deEssLowState {};
    std::array<float, 2> brightnessLowState {};
    std::array<float, 2> radioHighPassLowState {};
    std::array<float, 2> radioLowPassState {};
    float gateEnvelope = 0.0f;
    float gateGain = 1.0f;
    float deEssEnvelope = 0.0f;
    float compressorEnvelope = 0.0f;
    float compressorGain = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SimpleMixProcessor)
};
