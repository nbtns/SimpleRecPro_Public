#include "SpecialFxProcessor.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double twoPi = 6.28318530717958647692;

float amplitudeFromDb(float decibels) noexcept
{
    return std::pow(10.0f, decibels * 0.05f);
}

float smoothTowards(float current, float target, float coefficient) noexcept
{
    return target + coefficient * (current - target);
}
}

bool SpecialFxProcessor::prepare(double sampleRate,
                                 int maximumBlockSize,
                                 int channelCount)
{
    if (!std::isfinite(sampleRate) || sampleRate < 8000.0
        || sampleRate > 384000.0 || maximumBlockSize <= 0
        || channelCount <= 0)
    {
        preparedSampleRate = 0.0;
        preparedChannels = 0;
        reset();
        return false;
    }

    preparedSampleRate = sampleRate;
    preparedChannels = std::clamp(channelCount, 1, 2);
    reset();
    return true;
}

void SpecialFxProcessor::reset() noexcept
{
    currentAmount = amountValue.load(std::memory_order_relaxed);
    currentWet = 0.0f;
    muffledLowState.fill(0.0f);
    radioHighPassLowState.fill(0.0f);
    radioLowPassState.fill(0.0f);
    heldBitcrushSample.fill(0.0f);
    heldBitcrushSampleValid.fill(false);
    expectedNextTimelineSample = std::numeric_limits<std::int64_t>::min();
}

void SpecialFxProcessor::setSettings(const Settings& newSettings) noexcept
{
    const auto type = std::clamp(static_cast<int>(newSettings.type),
                                 static_cast<int>(SpecialFxType::none),
                                 static_cast<int>(SpecialFxType::bitcrush));
    const auto start = std::max<std::int64_t>(0,
                                              newSettings.regionStartSample);
    const auto end = std::max(start, newSettings.regionEndSampleExclusive);
    const auto maximumFade = preparedSampleRate > 0.0
        ? static_cast<int>(std::min(10.0 * preparedSampleRate,
                                    static_cast<double>(std::numeric_limits<int>::max())))
        : 3840000;

    enabledValue.store(newSettings.enabled, std::memory_order_relaxed);
    typeValue.store(type, std::memory_order_relaxed);
    amountValue.store(sanitiseUnit(newSettings.amount, 0.5f),
                      std::memory_order_relaxed);
    wetValue.store(sanitiseUnit(newSettings.wet, 1.0f),
                   std::memory_order_relaxed);
    limitToRegionValue.store(newSettings.limitToRegion,
                             std::memory_order_relaxed);
    regionStartValue.store(start, std::memory_order_relaxed);
    regionEndValue.store(end, std::memory_order_relaxed);
    fadeSamplesValue.store(std::clamp(newSettings.fadeSamples, 0, maximumFade),
                           std::memory_order_relaxed);
    noiseSeedValue.store(newSettings.noiseSeed, std::memory_order_relaxed);
}

SpecialFxProcessor::Settings SpecialFxProcessor::getSettings() const noexcept
{
    Settings result;
    result.enabled = enabledValue.load(std::memory_order_relaxed);
    result.type = static_cast<SpecialFxType>(
        typeValue.load(std::memory_order_relaxed));
    result.amount = amountValue.load(std::memory_order_relaxed);
    result.wet = wetValue.load(std::memory_order_relaxed);
    result.limitToRegion = limitToRegionValue.load(std::memory_order_relaxed);
    result.regionStartSample = regionStartValue.load(std::memory_order_relaxed);
    result.regionEndSampleExclusive = regionEndValue.load(
        std::memory_order_relaxed);
    result.fadeSamples = fadeSamplesValue.load(std::memory_order_relaxed);
    result.noiseSeed = noiseSeedValue.load(std::memory_order_relaxed);
    return result;
}

void SpecialFxProcessor::process(juce::AudioBuffer<float>& buffer,
                                 int startSample,
                                 int numSamples,
                                 std::int64_t timelineStartSample) noexcept
{
    if (preparedChannels <= 0 || preparedSampleRate <= 0.0
        || buffer.getNumChannels() <= 0 || buffer.getNumSamples() <= 0)
        return;

    const auto safeStart = std::clamp(startSample, 0, buffer.getNumSamples());
    const auto available = buffer.getNumSamples() - safeStart;
    const auto safeCount = numSamples < 0
        ? available : std::clamp(numSamples, 0, available);
    const auto channels = std::min({ preparedChannels,
                                     buffer.getNumChannels(),
                                     2 });
    if (safeCount <= 0 || channels <= 0)
        return;

    const auto enabled = enabledValue.load(std::memory_order_relaxed);
    const auto type = static_cast<SpecialFxType>(
        typeValue.load(std::memory_order_relaxed));
    const auto targetAmount = amountValue.load(std::memory_order_relaxed);
    const auto targetWet = enabled && type != SpecialFxType::none
        ? wetValue.load(std::memory_order_relaxed) : 0.0f;
    const auto limitToRegion = limitToRegionValue.load(
        std::memory_order_relaxed);
    const auto regionStart = regionStartValue.load(std::memory_order_relaxed);
    const auto regionEnd = regionEndValue.load(std::memory_order_relaxed);
    const auto fadeSamples = fadeSamplesValue.load(std::memory_order_relaxed);
    const auto noiseSeed = noiseSeedValue.load(std::memory_order_relaxed);

    const auto muffledCutoff = 8000.0 * std::pow(0.075,
                                                 targetAmount);
    const auto muffledCoefficient = onePoleAmount(muffledCutoff,
                                                   preparedSampleRate);
    const auto radioHighPassCoefficient = onePoleAmount(
        180.0 + 300.0 * targetAmount,
        preparedSampleRate);
    const auto radioLowPassCoefficient = onePoleAmount(
        5200.0 - 3000.0 * targetAmount,
        preparedSampleRate);
    const auto distortionDrive = 1.0f + 15.0f * targetAmount;
    const auto distortionNormalisation = std::max(
        std::tanh(distortionDrive), 1.0e-6f);
    const auto noiseBaseGain = amplitudeFromDb(
        -50.0f + 30.0f * targetAmount);
    const auto bitDepth = std::clamp(
        static_cast<int>(std::lround(16.0f - 12.0f * targetAmount)), 4, 16);
    const auto quantisationLevels = static_cast<float>(1 << (bitDepth - 1));
    const auto holdFactor = std::clamp(
        static_cast<int>(std::lround(1.0f + 23.0f * targetAmount)), 1, 24);

    if (timelineStartSample != expectedNextTimelineSample)
        heldBitcrushSampleValid.fill(false);

    std::array<float*, 2> audio {
        buffer.getWritePointer(0, safeStart),
        channels > 1 ? buffer.getWritePointer(1, safeStart) : nullptr
    };

    const auto smoothingCoefficient = static_cast<float>(std::exp(
        -1.0 / (0.010 * preparedSampleRate)));
    for (int sampleIndex = 0; sampleIndex < safeCount; ++sampleIndex)
    {
        currentAmount = smoothTowards(currentAmount,
                                      targetAmount,
                                      smoothingCoefficient);
        const auto timelineSample = timelineStartSample + sampleIndex;
        const auto boundaryWet = limitToRegion
            ? regionEnvelope(timelineSample,
                             regionStart,
                             regionEnd,
                             fadeSamples)
            : 1.0f;
        currentWet = smoothTowards(currentWet,
                                   targetWet,
                                   smoothingCoefficient);
        const auto effectiveWet = currentWet * boundaryWet;

        const auto amount = std::clamp(currentAmount, 0.0f, 1.0f);
        for (int channel = 0; channel < channels; ++channel)
        {
            const auto index = static_cast<size_t>(channel);
            const auto dry = finiteOrZero(audio[index][sampleIndex]);
            auto effected = dry;

            switch (type)
            {
                case SpecialFxType::muffled:
                {
                    auto& low = muffledLowState[index];
                    low += muffledCoefficient * (dry - low);
                    effected = dry + amount * (low - dry);
                    break;
                }
                case SpecialFxType::radio:
                {
                    auto& low = radioHighPassLowState[index];
                    low += radioHighPassCoefficient * (dry - low);
                    const auto highPassed = dry - low;
                    auto& band = radioLowPassState[index];
                    band += radioLowPassCoefficient * (highPassed - band);
                    const auto saturated = std::tanh(
                        band * (1.0f + 3.0f * amount));
                    effected = dry + amount * (saturated - dry);
                    break;
                }
                case SpecialFxType::noise:
                    effected = dry + noiseBaseGain * amount
                        * deterministicNoise(
                        timelineSample, channel, noiseSeed);
                    break;
                case SpecialFxType::distortion:
                {
                    const auto saturated = std::tanh(dry * distortionDrive)
                                         / distortionNormalisation;
                    effected = dry + amount * (saturated - dry);
                    break;
                }
                case SpecialFxType::bitcrush:
                {
                    if (!heldBitcrushSampleValid[index]
                        || timelineSample % holdFactor == 0)
                    {
                        heldBitcrushSample[index] = dry;
                        heldBitcrushSampleValid[index] = true;
                    }
                    const auto quantised = std::round(
                        heldBitcrushSample[index] * quantisationLevels)
                        / quantisationLevels;
                    effected = dry + amount * (quantised - dry);
                    break;
                }
                case SpecialFxType::none:
                default:
                    break;
            }

            audio[index][sampleIndex] = finiteOrZero(
                dry + effectiveWet * (effected - dry));
        }
    }

    expectedNextTimelineSample = timelineStartSample + safeCount;
}

float SpecialFxProcessor::sanitiseUnit(float value, float fallback) noexcept
{
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : fallback;
}

float SpecialFxProcessor::finiteOrZero(float value) noexcept
{
    return std::isfinite(value) ? value : 0.0f;
}

float SpecialFxProcessor::onePoleAmount(double cutoff,
                                        double sampleRate) noexcept
{
    const auto boundedCutoff = std::clamp(cutoff, 10.0, sampleRate * 0.45);
    return static_cast<float>(1.0 - std::exp(
        -twoPi * boundedCutoff / sampleRate));
}

float SpecialFxProcessor::deterministicNoise(std::int64_t timelineSample,
                                             int channel,
                                             std::uint32_t seed) noexcept
{
    const auto sampleBits = static_cast<std::uint64_t>(timelineSample);
    auto value = seed
               ^ static_cast<std::uint32_t>(sampleBits)
               ^ static_cast<std::uint32_t>(sampleBits >> 32)
               ^ (0x9e3779b9u * static_cast<std::uint32_t>(channel + 1));
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return static_cast<float>(value & 0x00ffffffu) * (2.0f / 16777215.0f)
         - 1.0f;
}

float SpecialFxProcessor::regionEnvelope(std::int64_t timelineSample,
                                         std::int64_t regionStart,
                                         std::int64_t regionEnd,
                                         int fadeSamples) noexcept
{
    if (timelineSample < regionStart || timelineSample >= regionEnd
        || regionEnd <= regionStart)
        return 0.0f;
    if (fadeSamples <= 0)
        return 1.0f;

    const auto regionLength = regionEnd - regionStart;
    const auto boundedFade = std::min<std::int64_t>(
        fadeSamples, std::max<std::int64_t>(1, regionLength / 2));
    const auto fadeIn = static_cast<float>(timelineSample - regionStart)
                      / static_cast<float>(boundedFade);
    const auto fadeOut = static_cast<float>(regionEnd - 1 - timelineSample)
                       / static_cast<float>(boundedFade);
    return std::clamp(std::min(fadeIn, fadeOut), 0.0f, 1.0f);
}
