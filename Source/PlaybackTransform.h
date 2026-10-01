#pragma once

#include <JuceHeader.h>

#include <functional>
#include <memory>

/**
 * Pitch and speed processor used independently by each track.
 *
 * Input and output lengths control playback speed, while transpose is applied
 * separately. This keeps the original key when only the speed is changed.
 */
class PlaybackTransform
{
public:
    PlaybackTransform();
    ~PlaybackTransform();

    void prepare(int channels, double sampleRate);
    void reset();

    int getInputSamplesForOutput(int outputSamples, double playbackSpeed) noexcept;
    int getMaximumInputSamplesForOutput(int outputSamples,
                                        double playbackSpeed) const noexcept;
    void process(const juce::AudioBuffer<float>& input,
                 int inputSamples,
                 juce::AudioBuffer<float>& output,
                 int outputSamples,
                 double playbackSpeed,
                 double pitchSemitones) noexcept;

    int getInputLatency() const noexcept;
    int getOutputLatency() const noexcept;

    static std::shared_ptr<juce::AudioBuffer<float>> processOffline(
        const juce::AudioBuffer<float>& input,
        double sampleRate,
        double playbackSpeed,
        double pitchSemitones,
        const std::function<bool()>& shouldCancel = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    double inputSampleRemainder = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlaybackTransform)
};
