#include "ClipRenderMath.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ClipRenderMath
{
namespace
{
constexpr double halfPi = 1.57079632679489661923;

std::int64_t saturatingAdd(std::int64_t value, int amount) noexcept
{
    if (amount <= 0)
        return value;

    const auto maximum = std::numeric_limits<std::int64_t>::max();
    if (value > maximum - static_cast<std::int64_t>(amount))
        return maximum;
    return value + static_cast<std::int64_t>(amount);
}

bool isValidLoop(std::int64_t start, std::int64_t end) noexcept
{
    return start >= 0 && end > start;
}
}

float getFadeGain(double positionSeconds,
                  double clipDurationSeconds,
                  double fadeInSeconds,
                  double fadeOutSeconds) noexcept
{
    if (!std::isfinite(positionSeconds)
        || !std::isfinite(clipDurationSeconds)
        || !std::isfinite(fadeInSeconds)
        || !std::isfinite(fadeOutSeconds)
        || clipDurationSeconds <= 0.0
        || positionSeconds < 0.0
        || positionSeconds > clipDurationSeconds)
        return 0.0f;

    auto fadeIn = std::clamp(fadeInSeconds, 0.0, clipDurationSeconds);
    auto fadeOut = std::clamp(fadeOutSeconds, 0.0, clipDurationSeconds);
    const auto combined = fadeIn + fadeOut;
    if (combined > clipDurationSeconds && combined > 0.0)
    {
        const auto scale = clipDurationSeconds / combined;
        fadeIn *= scale;
        fadeOut *= scale;
    }

    double gain = 1.0;
    if (fadeIn > 0.0 && positionSeconds < fadeIn)
    {
        const auto progress = std::clamp(positionSeconds / fadeIn, 0.0, 1.0);
        gain *= std::sin(progress * halfPi);
    }

    const auto timeToEnd = clipDurationSeconds - positionSeconds;
    if (fadeOut > 0.0 && timeToEnd < fadeOut)
    {
        const auto progress = std::clamp(timeToEnd / fadeOut, 0.0, 1.0);
        gain *= std::sin(progress * halfPi);
    }

    return static_cast<float>(std::clamp(gain, 0.0, 1.0));
}

StereoGains getConstantPowerPanGains(float pan) noexcept
{
    const auto safePan = std::isfinite(pan) ? std::clamp(pan, -1.0f, 1.0f)
                                            : 0.0f;
    const auto angle = (static_cast<double>(safePan) + 1.0) * halfPi * 0.5;
    return { static_cast<float>(std::cos(angle)),
             static_cast<float>(std::sin(angle)) };
}

NormalisedRange normaliseRange(double pointASeconds,
                               double pointBSeconds,
                               double timelineDurationSeconds,
                               double minimumLengthSeconds) noexcept
{
    NormalisedRange result;
    if (!std::isfinite(pointASeconds)
        || !std::isfinite(pointBSeconds)
        || !std::isfinite(timelineDurationSeconds)
        || !std::isfinite(minimumLengthSeconds)
        || timelineDurationSeconds <= 0.0)
        return result;

    auto start = std::clamp(std::min(pointASeconds, pointBSeconds),
                            0.0,
                            timelineDurationSeconds);
    auto end = std::clamp(std::max(pointASeconds, pointBSeconds),
                          0.0,
                          timelineDurationSeconds);
    const auto minimum = std::clamp(minimumLengthSeconds,
                                    0.0,
                                    timelineDurationSeconds);

    if (end - start < minimum)
    {
        end = std::min(timelineDurationSeconds, start + minimum);
        start = std::max(0.0, end - minimum);
    }

    result.startSeconds = start;
    result.endSeconds = end;
    result.valid = end > start;
    return result;
}

std::int64_t wrapLoopPosition(std::int64_t position,
                              std::int64_t loopStart,
                              std::int64_t loopEnd) noexcept
{
    position = std::max<std::int64_t>(0, position);
    if (!isValidLoop(loopStart, loopEnd) || position < loopStart)
        return position;

    const auto length = loopEnd - loopStart;
    return loopStart + (position - loopStart) % length;
}

LoopSegment decideLoopSegment(std::int64_t currentSample,
                              int requestedSamples,
                              std::int64_t loopStart,
                              std::int64_t loopEnd) noexcept
{
    LoopSegment result;
    result.loopIsValid = isValidLoop(loopStart, loopEnd);
    result.startSample = std::max<std::int64_t>(0, currentSample);
    result.nextSample = result.startSample;
    if (requestedSamples <= 0)
        return result;

    if (!result.loopIsValid)
    {
        result.samplesToRender = requestedSamples;
        result.nextSample = saturatingAdd(result.startSample, requestedSamples);
        return result;
    }

    if (result.startSample >= loopEnd)
    {
        result.startSample = wrapLoopPosition(result.startSample, loopStart, loopEnd);
        result.wrapped = true;
    }

    const auto samplesUntilEnd = loopEnd - result.startSample;
    if (samplesUntilEnd <= 0)
    {
        result.startSample = loopStart;
        result.nextSample = loopStart;
        result.wrapped = true;
        return result;
    }

    result.samplesToRender = static_cast<int>(std::min<std::int64_t>(
        requestedSamples,
        std::min<std::int64_t>(samplesUntilEnd,
                               std::numeric_limits<int>::max())));
    const auto linearNext = saturatingAdd(result.startSample,
                                          result.samplesToRender);
    if (linearNext >= loopEnd)
    {
        result.nextSample = loopStart;
        result.wrapped = true;
    }
    else
    {
        result.nextSample = linearNext;
    }
    return result;
}
}
