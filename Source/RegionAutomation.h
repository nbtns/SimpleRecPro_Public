#pragma once
#include "DawFeatureTypes.h"
#include <array>
#include <cstdint>
#include <limits>

namespace RegionAutomation
{
constexpr size_t maximumRegions = 256;
bool isValid(const AudioAutomationRegion&) noexcept;
juce::var toVar(const AudioAutomationRegion&);
bool fromVar(const juce::var&, AudioAutomationRegion&);
}

/** One region, bounded allocation in prepare only. No audio outside the region changes.
    Brightness is a tilt filter; ambience is a short stereo feedback echo.
    Seeking/looping clears history; sequential blocks are sample-identical to export. */
class RegionAutomationProcessor
{
public:
    bool prepare(double sampleRate, int channelCount, const AudioAutomationRegion&);
    void reset() noexcept;
    void process(juce::AudioBuffer<float>&, int startSample, int numSamples,
                 std::int64_t timelineStartSample) noexcept;
private:
    AudioAutomationRegion settings;
    double rate = 0.0;
    int channels = 0;
    std::int64_t start = 0, end = 0, fade = 0;
    std::int64_t expected = std::numeric_limits<std::int64_t>::min();
    float lowCoefficient = 0.0f;
    std::array<float, 2> lowState {};
    juce::AudioBuffer<float> delay;
    int writePosition = 0;
    std::array<int, 2> delayLength {};
};
