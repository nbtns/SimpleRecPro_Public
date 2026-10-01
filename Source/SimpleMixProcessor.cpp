#include "SimpleMixProcessor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr double twoPi = 6.28318530717958647692;

float amplitudeFromDb(float decibels) noexcept
{
    return std::pow(10.0f, decibels * 0.05f);
}

float onePoleAmount(double cutoff, double sampleRate) noexcept
{
    const auto boundedCutoff = std::clamp(cutoff, 10.0, sampleRate * 0.45);
    return static_cast<float>(1.0 - std::exp(-twoPi * boundedCutoff / sampleRate));
}

float envelopeCoefficient(double milliseconds, double sampleRate) noexcept
{
    return static_cast<float>(std::exp(
        -1.0 / (std::max(0.1, milliseconds) * 0.001 * sampleRate)));
}

float smoothTowards(float current, float target, float coefficient) noexcept
{
    return target + coefficient * (current - target);
}

float finiteOrZero(float sample) noexcept
{
    return std::isfinite(sample) ? sample : 0.0f;
}

float presetOutputGain(MixPreset preset) noexcept
{
    switch (preset)
    {
        case MixPreset::natural: return amplitudeFromDb(-0.5f);
        case MixPreset::clear:   return amplitudeFromDb(-1.0f);
        case MixPreset::wide:    return amplitudeFromDb(-1.0f);
        case MixPreset::radio:   return amplitudeFromDb(-2.5f);
        case MixPreset::flat:
        default:                 return 1.0f;
    }
}
}

bool SimpleMixProcessor::prepare(double sampleRate,
                                 int maximumBlockSize,
                                 int channelCount)
{
    if (!std::isfinite(sampleRate) || sampleRate < 8000.0
        || sampleRate > 384000.0 || maximumBlockSize <= 0
        || channelCount <= 0)
    {
        preparedSampleRate = 0.0;
        preparedChannels = 0;
        ambienceDelay.setSize(0, 0);
        return false;
    }

    preparedSampleRate = sampleRate;
    preparedChannels = std::clamp(channelCount, 1, 2);
    const auto delayCapacity = std::max(
        2, static_cast<int>(std::ceil(preparedSampleRate * 0.060)) + 1);
    ambienceDelay.setSize(preparedChannels,
                          delayCapacity,
                          false,
                          false,
                          true);
    ambienceDelaySamples[0] = std::clamp(
        static_cast<int>(std::lround(preparedSampleRate * 0.037)),
        1,
        delayCapacity - 1);
    ambienceDelaySamples[1] = std::clamp(
        static_cast<int>(std::lround(preparedSampleRate * 0.043)),
        1,
        delayCapacity - 1);
    reset();
    return true;
}

void SimpleMixProcessor::reset() noexcept
{
    ambienceDelay.clear();
    ambienceWritePosition = 0;
    deEssLowState.fill(0.0f);
    brightnessLowState.fill(0.0f);
    radioHighPassLowState.fill(0.0f);
    radioLowPassState.fill(0.0f);
    gateEnvelope = 0.0f;
    gateGain = 1.0f;
    deEssEnvelope = 0.0f;
    compressorEnvelope = 0.0f;
    compressorGain = 1.0f;
}

void SimpleMixProcessor::setSettings(
    const SimpleMixSettings& newSettings) noexcept
{
    mixEnabled.store(newSettings.enabled, std::memory_order_relaxed);
    const auto preset = std::clamp(static_cast<int>(newSettings.preset),
                                   static_cast<int>(MixPreset::flat),
                                   static_cast<int>(MixPreset::radio));
    presetValue.store(preset, std::memory_order_relaxed);
    brightnessValue.store(sanitiseBipolar(newSettings.brightness),
                          std::memory_order_relaxed);
    ambienceValue.store(sanitiseUnit(newSettings.ambience),
                        std::memory_order_relaxed);
    stabilityValue.store(sanitiseUnit(newSettings.stability),
                         std::memory_order_relaxed);
}

void SimpleMixProcessor::setNoiseReductionSettings(
    const NoiseReductionSettings& newSettings) noexcept
{
    noiseEnabled.store(newSettings.enabled, std::memory_order_relaxed);
    noiseAmountValue.store(sanitiseUnit(newSettings.amount),
                           std::memory_order_relaxed);
    deEssAmountValue.store(sanitiseUnit(newSettings.deEssAmount),
                           std::memory_order_relaxed);
}

SimpleMixSettings SimpleMixProcessor::getSettings() const noexcept
{
    SimpleMixSettings result;
    result.enabled = mixEnabled.load(std::memory_order_relaxed);
    result.preset = static_cast<MixPreset>(presetValue.load(
        std::memory_order_relaxed));
    result.brightness = brightnessValue.load(std::memory_order_relaxed);
    result.ambience = ambienceValue.load(std::memory_order_relaxed);
    result.stability = stabilityValue.load(std::memory_order_relaxed);
    return result;
}

NoiseReductionSettings
SimpleMixProcessor::getNoiseReductionSettings() const noexcept
{
    NoiseReductionSettings result;
    result.enabled = noiseEnabled.load(std::memory_order_relaxed);
    result.amount = noiseAmountValue.load(std::memory_order_relaxed);
    result.deEssAmount = deEssAmountValue.load(std::memory_order_relaxed);
    return result;
}

SimpleMixSettings SimpleMixProcessor::makePresetSettings(
    MixPreset preset) noexcept
{
    SimpleMixSettings settings;
    settings.preset = preset;

    switch (preset)
    {
        case MixPreset::natural:
            settings.enabled = true;
            settings.brightness = 0.28f;
            settings.ambience = 0.14f;
            settings.stability = 0.32f;
            break;
        case MixPreset::clear:
            settings.enabled = true;
            settings.brightness = 0.72f;
            settings.ambience = 0.07f;
            settings.stability = 0.52f;
            break;
        case MixPreset::wide:
            settings.enabled = true;
            settings.brightness = 0.30f;
            settings.ambience = 0.48f;
            settings.stability = 0.28f;
            break;
        case MixPreset::radio:
            settings.enabled = true;
            settings.brightness = 0.18f;
            settings.ambience = 0.04f;
            settings.stability = 0.68f;
            break;
        case MixPreset::flat:
        default:
            settings.enabled = false;
            settings.brightness = 0.0f;
            settings.ambience = 0.0f;
            settings.stability = 0.0f;
            break;
    }
    return settings;
}

void SimpleMixProcessor::setPreset(MixPreset preset) noexcept
{
    setSettings(makePresetSettings(preset));
}

void SimpleMixProcessor::process(juce::AudioBuffer<float>& buffer,
                                 int startSample,
                                 int numSamples) noexcept
{
    if (preparedChannels <= 0 || preparedSampleRate <= 0.0
        || ambienceDelay.getNumSamples() <= 1
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

    const bool useMix = mixEnabled.load(std::memory_order_relaxed);
    const bool useNoise = noiseEnabled.load(std::memory_order_relaxed);
    if (!useMix && !useNoise)
        return;

    const auto brightness = useMix
        ? brightnessValue.load(std::memory_order_relaxed) : 0.0f;
    const auto ambience = useMix
        ? ambienceValue.load(std::memory_order_relaxed) : 0.0f;
    const auto stability = useMix
        ? stabilityValue.load(std::memory_order_relaxed) : 0.0f;
    const auto preset = useMix
        ? static_cast<MixPreset>(presetValue.load(std::memory_order_relaxed))
        : MixPreset::flat;
    const auto radioAmount = preset == MixPreset::radio ? 1.0f : 0.0f;
    const auto noiseAmount = useNoise
        ? noiseAmountValue.load(std::memory_order_relaxed) : 0.0f;
    const auto deEssAmount = useNoise
        ? deEssAmountValue.load(std::memory_order_relaxed) : 0.0f;

    const auto gateAttack = envelopeCoefficient(4.0, preparedSampleRate);
    const auto gateRelease = envelopeCoefficient(90.0, preparedSampleRate);
    const auto gateThreshold = amplitudeFromDb(-65.0f + 18.0f * noiseAmount);
    const auto deEssFilter = onePoleAmount(5600.0, preparedSampleRate);
    const auto deEssAttack = envelopeCoefficient(1.5, preparedSampleRate);
    const auto deEssRelease = envelopeCoefficient(55.0, preparedSampleRate);
    const auto brightnessFilter = onePoleAmount(2600.0, preparedSampleRate);
    const auto brightnessBoost = amplitudeFromDb(4.5f * brightness) - 1.0f;
    const auto radioHighPass = onePoleAmount(260.0, preparedSampleRate);
    const auto radioLowPass = onePoleAmount(3900.0, preparedSampleRate);
    const auto compressorAttack = envelopeCoefficient(8.0, preparedSampleRate);
    const auto compressorRelease = envelopeCoefficient(110.0, preparedSampleRate);
    const auto compressorGainAttack = envelopeCoefficient(4.0, preparedSampleRate);
    const auto compressorGainRelease = envelopeCoefficient(90.0, preparedSampleRate);
    const auto compressorThresholdDb = -8.0f - 16.0f * stability;
    const auto compressorRatio = 1.0f + 5.0f * stability;
    const auto compressorMakeup = amplitudeFromDb(2.2f * stability);
    const auto outputGain = presetOutputGain(preset);
    const auto ambienceWet = 0.30f * ambience;
    const auto ambienceFeedback = 0.18f + 0.30f * ambience;

    std::array<float*, 2> audio {
        buffer.getWritePointer(0, safeStart),
        channels > 1 ? buffer.getWritePointer(1, safeStart) : nullptr
    };
    std::array<float*, 2> delay {
        ambienceDelay.getWritePointer(0),
        preparedChannels > 1 ? ambienceDelay.getWritePointer(1) : nullptr
    };
    const auto delayCapacity = ambienceDelay.getNumSamples();

    for (int sampleIndex = 0; sampleIndex < safeCount; ++sampleIndex)
    {
        std::array<float, 2> samples {};
        std::array<float, 2> highBand {};
        float inputPeak = 0.0f;
        for (int channel = 0; channel < channels; ++channel)
        {
            samples[static_cast<size_t>(channel)] = finiteOrZero(
                audio[static_cast<size_t>(channel)][sampleIndex]);
            inputPeak = std::max(inputPeak,
                                 std::abs(samples[static_cast<size_t>(channel)]));
        }

        const auto gateEnvelopeCoeff = inputPeak > gateEnvelope
            ? gateAttack : gateRelease;
        gateEnvelope = smoothTowards(gateEnvelope,
                                     inputPeak,
                                     gateEnvelopeCoeff);
        const auto gateTarget = gateEnvelope >= gateThreshold ? 1.0f : 0.0f;
        const auto gateGainCoeff = gateTarget > gateGain ? gateAttack : gateRelease;
        gateGain = smoothTowards(gateGain, gateTarget, gateGainCoeff);
        const auto effectiveGate = 1.0f - noiseAmount * (1.0f - gateGain);

        float highPeak = 0.0f;
        for (int channel = 0; channel < channels; ++channel)
        {
            auto& value = samples[static_cast<size_t>(channel)];
            value *= effectiveGate;
            auto& low = deEssLowState[static_cast<size_t>(channel)];
            low += deEssFilter * (value - low);
            highBand[static_cast<size_t>(channel)] = value - low;
            highPeak = std::max(highPeak,
                                std::abs(highBand[static_cast<size_t>(channel)]));
        }

        const auto deEssCoeff = highPeak > deEssEnvelope
            ? deEssAttack : deEssRelease;
        deEssEnvelope = smoothTowards(deEssEnvelope, highPeak, deEssCoeff);
        constexpr float deEssThreshold = 0.055f;
        const auto deEssOver = std::max(0.0f,
            (deEssEnvelope - deEssThreshold) / deEssThreshold);
        const auto deEssFloor = 1.0f - 0.78f * deEssAmount;
        const auto deEssGain = std::max(
            deEssFloor,
            1.0f / (1.0f + 5.0f * deEssAmount * deEssOver));

        float processedPeak = 0.0f;
        for (int channel = 0; channel < channels; ++channel)
        {
            auto& value = samples[static_cast<size_t>(channel)];
            value = deEssLowState[static_cast<size_t>(channel)]
                  + highBand[static_cast<size_t>(channel)] * deEssGain;

            auto& brightnessLow = brightnessLowState[static_cast<size_t>(channel)];
            brightnessLow += brightnessFilter * (value - brightnessLow);
            value += (value - brightnessLow) * brightnessBoost;

            auto& radioLow = radioHighPassLowState[static_cast<size_t>(channel)];
            radioLow += radioHighPass * (value - radioLow);
            const auto highPassed = value - radioLow;
            auto& radioBand = radioLowPassState[static_cast<size_t>(channel)];
            radioBand += radioLowPass * (highPassed - radioBand);
            const auto saturatedRadio = std::tanh(radioBand * 1.8f)
                                      / std::tanh(1.8f);
            value += radioAmount * (saturatedRadio - value);
            processedPeak = std::max(processedPeak, std::abs(value));
        }

        const auto compressorEnvelopeCoeff = processedPeak > compressorEnvelope
            ? compressorAttack : compressorRelease;
        compressorEnvelope = smoothTowards(compressorEnvelope,
                                            processedPeak,
                                            compressorEnvelopeCoeff);
        const auto envelopeDb = 20.0f * std::log10(
            std::max(compressorEnvelope, 1.0e-7f));
        const auto overDb = std::max(0.0f, envelopeDb - compressorThresholdDb);
        const auto reductionDb = -overDb * (1.0f - 1.0f / compressorRatio);
        const auto targetCompressorGain = amplitudeFromDb(reductionDb);
        const auto compressorGainCoeff = targetCompressorGain < compressorGain
            ? compressorGainAttack : compressorGainRelease;
        compressorGain = smoothTowards(compressorGain,
                                        targetCompressorGain,
                                        compressorGainCoeff);

        for (int channel = 0; channel < channels; ++channel)
        {
            auto value = samples[static_cast<size_t>(channel)]
                       * compressorGain * compressorMakeup;
            auto readPosition = ambienceWritePosition
                              - ambienceDelaySamples[static_cast<size_t>(channel)];
            if (readPosition < 0)
                readPosition += delayCapacity;
            const auto delayed = delay[static_cast<size_t>(channel)][readPosition];
            delay[static_cast<size_t>(channel)][ambienceWritePosition] =
                finiteOrZero(value + delayed * ambienceFeedback);
            value = (value + delayed * ambienceWet) * outputGain;
            audio[static_cast<size_t>(channel)][sampleIndex] = finiteOrZero(value);
        }

        if (++ambienceWritePosition >= delayCapacity)
            ambienceWritePosition = 0;
    }
}

float SimpleMixProcessor::sanitiseBipolar(float value) noexcept
{
    return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
}

float SimpleMixProcessor::sanitiseUnit(float value) noexcept
{
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}
