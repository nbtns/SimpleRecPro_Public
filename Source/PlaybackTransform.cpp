#include "PlaybackTransform.h"

#include <signalsmith-stretch.h>

#include <algorithm>
#include <array>
#include <cmath>

struct PlaybackTransform::Impl
{
    signalsmith::stretch::SignalsmithStretch<float> stretch { 0 };
    int channels = 0;
    double sampleRate = 0.0;
    juce::AudioBuffer<float> preRollOutput;
    bool primingRequired = true;
    bool requestPending = false;
    int pendingSeekInputSamples = 0;
    int pendingPreRollInputSamples = 0;
    int pendingMainInputSamples = 0;
    int pendingOutputSamples = 0;
    int foldbackOutputPosition = 0;
    double pendingPlaybackSpeed = 1.0;
};

PlaybackTransform::PlaybackTransform()
    : impl(std::make_unique<Impl>())
{
}

PlaybackTransform::~PlaybackTransform() = default;

void PlaybackTransform::prepare(int channels, double sampleRate)
{
    impl->channels = juce::jlimit(1, 2, channels);
    impl->sampleRate = std::max(8000.0, sampleRate);
    impl->stretch.presetCheaper(impl->channels,
                                static_cast<float>(impl->sampleRate));
    impl->preRollOutput.setSize(impl->channels,
                                std::max(1, impl->stretch.outputLatency()),
                                false,
                                false,
                                true);
    reset();
}

void PlaybackTransform::reset()
{
    inputSampleRemainder = 0.0;
    if (impl->channels > 0)
        impl->stretch.reset();
    impl->preRollOutput.clear();
    impl->primingRequired = true;
    impl->requestPending = false;
    impl->pendingSeekInputSamples = 0;
    impl->pendingPreRollInputSamples = 0;
    impl->pendingMainInputSamples = 0;
    impl->pendingOutputSamples = 0;
    impl->foldbackOutputPosition = 0;
    impl->pendingPlaybackSpeed = 1.0;
}

int PlaybackTransform::getInputSamplesForOutput(int outputSamples,
                                                double playbackSpeed) noexcept
{
    const double speed = juce::jlimit(0.75, 1.5, playbackSpeed);
    const int safeOutputSamples = std::max(0, outputSamples);

    // A request is always consumed by the following process() call. Returning
    // the same value makes an accidental duplicate query harmless instead of
    // advancing the fractional input cursor twice.
    if (impl->requestPending)
    {
        jassert(impl->pendingOutputSamples == safeOutputSamples);
        jassert(std::abs(impl->pendingPlaybackSpeed - speed) < 1.0e-9);
        return impl->pendingSeekInputSamples
             + impl->pendingPreRollInputSamples
             + impl->pendingMainInputSamples;
    }

    const auto takeInputForOutput = [this, speed](int requestedOutput)
    {
        const double exactSamples = requestedOutput * speed
                                    + inputSampleRemainder;
        const int result = std::max(0,
            static_cast<int>(std::floor(exactSamples)));
        inputSampleRemainder = exactSamples - result;
        return result;
    };

    impl->pendingSeekInputSamples = impl->primingRequired
        ? getInputLatency() : 0;
    impl->pendingPreRollInputSamples = impl->primingRequired
        ? takeInputForOutput(getOutputLatency()) : 0;
    impl->pendingMainInputSamples = takeInputForOutput(safeOutputSamples);
    impl->pendingOutputSamples = safeOutputSamples;
    impl->pendingPlaybackSpeed = speed;
    impl->requestPending = true;

    return impl->pendingSeekInputSamples
         + impl->pendingPreRollInputSamples
         + impl->pendingMainInputSamples;
}

int PlaybackTransform::getMaximumInputSamplesForOutput(
    int outputSamples,
    double playbackSpeed) const noexcept
{
    const double speed = juce::jlimit(0.75, 1.5, playbackSpeed);
    const int processOutput = std::max(0, outputSamples) + getOutputLatency();
    return getInputLatency()
         + static_cast<int>(std::ceil(processOutput * speed))
         + 2;
}

void PlaybackTransform::process(const juce::AudioBuffer<float>& input,
                                int inputSamples,
                                juce::AudioBuffer<float>& output,
                                int outputSamples,
                                double playbackSpeed,
                                double pitchSemitones) noexcept
{
    const int channels = std::min({ impl->channels,
                                    input.getNumChannels(),
                                    output.getNumChannels() });
    if (channels <= 0 || outputSamples <= 0)
    {
        output.clear();
        impl->requestPending = false;
        return;
    }

    const double speed = juce::jlimit(0.75, 1.5, playbackSpeed);
    const int requiredInput = impl->pendingSeekInputSamples
                            + impl->pendingPreRollInputSamples
                            + impl->pendingMainInputSamples;
    if (!impl->requestPending
        || impl->pendingOutputSamples != outputSamples
        || std::abs(impl->pendingPlaybackSpeed - speed) >= 1.0e-9
        || inputSamples < requiredInput
        || input.getNumSamples() < requiredInput)
    {
        jassertfalse;
        output.clear();
        reset();
        return;
    }

    impl->stretch.setTransposeSemitones(
        static_cast<float>(juce::jlimit(-12.0, 12.0, pitchSemitones)));

    int inputOffset = 0;
    if (impl->primingRequired)
    {
        std::array<const float*, 2> seekInputPointers {
            input.getReadPointer(0),
            input.getReadPointer(std::min(1, channels - 1))
        };
        impl->stretch.seek(seekInputPointers.data(),
                           impl->pendingSeekInputSamples,
                           speed);
        inputOffset += impl->pendingSeekInputSamples;

        const int preRollOutputSamples = getOutputLatency();
        if (preRollOutputSamples > 0)
        {
            std::array<const float*, 2> preRollInputPointers {
                input.getReadPointer(0, inputOffset),
                input.getReadPointer(std::min(1, channels - 1), inputOffset)
            };
            std::array<float*, 2> preRollOutputPointers {
                impl->preRollOutput.getWritePointer(0),
                impl->preRollOutput.getWritePointer(std::min(1, channels - 1))
            };
            impl->stretch.process(preRollInputPointers.data(),
                                  impl->pendingPreRollInputSamples,
                                  preRollOutputPointers.data(),
                                  preRollOutputSamples);
        }
        inputOffset += impl->pendingPreRollInputSamples;
        impl->primingRequired = false;
        impl->foldbackOutputPosition = 0;
    }

    std::array<const float*, 2> inputPointers {
        input.getReadPointer(0, inputOffset),
        input.getReadPointer(std::min(1, channels - 1), inputOffset)
    };
    std::array<float*, 2> outputPointers {
        output.getWritePointer(0),
        output.getWritePointer(std::min(1, channels - 1))
    };

    impl->stretch.process(inputPointers.data(),
                          impl->pendingMainInputSamples,
                          outputPointers.data(),
                          outputSamples);

    // Signalsmith's exact-length recipe folds the discarded pre-roll back
    // into the beginning of the audible output. Keep the cursor across audio
    // callbacks because outputLatency is normally several blocks long.
    const int preRollOutputSamples = getOutputLatency();
    const int foldbackSamples = std::min(
        outputSamples,
        std::max(0, preRollOutputSamples - impl->foldbackOutputPosition));
    for (int channel = 0; channel < channels; ++channel)
    {
        auto* destination = output.getWritePointer(channel);
        const auto* preRoll = impl->preRollOutput.getReadPointer(channel);
        for (int sample = 0; sample < foldbackSamples; ++sample)
        {
            const int preRollIndex = preRollOutputSamples - 1
                                   - impl->foldbackOutputPosition - sample;
            destination[sample] -= preRoll[preRollIndex];
        }
    }
    impl->foldbackOutputPosition += foldbackSamples;
    impl->requestPending = false;

    for (int channel = channels; channel < output.getNumChannels(); ++channel)
        output.copyFrom(channel, 0, output, channels - 1, 0, outputSamples);
}

int PlaybackTransform::getInputLatency() const noexcept
{
    return impl->channels > 0 ? impl->stretch.inputLatency() : 0;
}

int PlaybackTransform::getOutputLatency() const noexcept
{
    return impl->channels > 0 ? impl->stretch.outputLatency() : 0;
}

std::shared_ptr<juce::AudioBuffer<float>> PlaybackTransform::processOffline(
    const juce::AudioBuffer<float>& input,
    double sampleRate,
    double playbackSpeed,
    double pitchSemitones,
    const std::function<bool()>& shouldCancel)
{
    const auto cancellationRequested = [&shouldCancel]
    {
        return shouldCancel && shouldCancel();
    };

    if (cancellationRequested())
        return nullptr;

    const int channels = juce::jlimit(1, 2, input.getNumChannels());
    const int inputSamples = input.getNumSamples();
    // Offline rhythm-note renders may use the wider, UI-bounded 0.5x..2.0x
    // range. Real-time whole-track controls remain limited to 0.75x..1.5x.
    const double speed = juce::jlimit(0.5, 2.0, playbackSpeed);
    if (inputSamples <= 0)
        return std::make_shared<juce::AudioBuffer<float>>(channels, 0);

    signalsmith::stretch::SignalsmithStretch<float> stretch { 0 };
    stretch.presetDefault(channels, static_cast<float>(std::max(8000.0, sampleRate)));
    stretch.setTransposeSemitones(
        static_cast<float>(juce::jlimit(-12.0, 12.0, pitchSemitones)));

    const int outputSamples = std::max(1,
        static_cast<int>(std::llround(inputSamples / speed)));
    const int seekInputSamples = stretch.inputLatency();
    const int preRollOutputSamples = stretch.outputLatency();
    double inputRemainder = 0.0;
    const auto takeInputForOutput = [&inputRemainder, speed](int requestedOutput)
    {
        const double exact = requestedOutput * speed + inputRemainder;
        const int result = std::max(0, static_cast<int>(std::floor(exact)));
        inputRemainder = exact - result;
        return result;
    };
    const int preRollInputSamples = takeInputForOutput(preRollOutputSamples);
    const int maximumProcessInput = static_cast<int>(std::ceil(
        (preRollOutputSamples + outputSamples) * speed)) + 2;

    juce::AudioBuffer<float> paddedInput(
        channels, seekInputSamples + maximumProcessInput);
    paddedInput.clear();
    for (int channel = 0; channel < channels; ++channel)
    {
        paddedInput.copyFrom(channel,
                             0,
                             input,
                             std::min(channel, input.getNumChannels() - 1),
                             0,
                             inputSamples);
    }

    std::array<const float*, 2> seekPointers {
        paddedInput.getReadPointer(0),
        paddedInput.getReadPointer(std::min(1, channels - 1))
    };
    stretch.seek(seekPointers.data(), seekInputSamples, speed);

    juce::AudioBuffer<float> preRollOutput(
        channels, std::max(1, preRollOutputSamples));
    preRollOutput.clear();
    if (preRollOutputSamples > 0)
    {
        std::array<const float*, 2> inputPointers {
            paddedInput.getReadPointer(0, seekInputSamples),
            paddedInput.getReadPointer(std::min(1, channels - 1),
                                       seekInputSamples)
        };
        std::array<float*, 2> outputPointers {
            preRollOutput.getWritePointer(0),
            preRollOutput.getWritePointer(std::min(1, channels - 1))
        };
        stretch.process(inputPointers.data(),
                        preRollInputSamples,
                        outputPointers.data(),
                        preRollOutputSamples);
    }

    auto result = std::make_shared<juce::AudioBuffer<float>>(channels,
                                                              outputSamples);
    result->clear();
    constexpr int outputChunkSamples = 8192;
    int inputOffset = seekInputSamples + preRollInputSamples;
    for (int outputOffset = 0; outputOffset < outputSamples;)
    {
        if (cancellationRequested())
            return nullptr;

        const int outputCount = std::min(outputChunkSamples,
                                         outputSamples - outputOffset);
        const int inputCount = takeInputForOutput(outputCount);
        std::array<const float*, 2> inputPointers {
            paddedInput.getReadPointer(0, inputOffset),
            paddedInput.getReadPointer(std::min(1, channels - 1), inputOffset)
        };
        std::array<float*, 2> outputPointers {
            result->getWritePointer(0, outputOffset),
            result->getWritePointer(std::min(1, channels - 1), outputOffset)
        };
        stretch.process(inputPointers.data(),
                        inputCount,
                        outputPointers.data(),
                        outputCount);

        const int foldbackCount = std::min(preRollOutputSamples - outputOffset,
                                           outputCount);
        if (foldbackCount > 0)
        {
            for (int channel = 0; channel < channels; ++channel)
            {
                auto* destination = result->getWritePointer(channel, outputOffset);
                const auto* preRoll = preRollOutput.getReadPointer(channel);
                for (int sample = 0; sample < foldbackCount; ++sample)
                {
                    destination[sample] -= preRoll[preRollOutputSamples - 1
                                                   - outputOffset - sample];
                }
            }
        }

        inputOffset += inputCount;
        outputOffset += outputCount;
    }

    if (cancellationRequested())
        return nullptr;

    return result;
}
