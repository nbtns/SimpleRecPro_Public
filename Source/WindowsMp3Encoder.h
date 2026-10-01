#pragma once

#include <JuceHeader.h>

#include <functional>

/**
 * Encodes an in-memory float buffer with the FFmpeg executable bundled beside
 * the application. The target is replaced only after the temporary MP3 has
 * been finalised successfully.
 */
class WindowsMp3Encoder
{
public:
    struct Result
    {
        bool succeeded = false;
        bool cancelled = false;
        juce::String errorMessage;
    };

    static Result encode(const juce::AudioBuffer<float>& audio,
                         int sampleRate,
                         int bitRateKbps,
                         const juce::File& targetFile,
                         const std::function<bool()>& shouldCancel = {});
};
