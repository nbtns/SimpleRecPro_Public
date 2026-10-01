#include "TempoMap.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr double defaultBpm = 120.0;
constexpr int defaultNumerator = 4;
constexpr int defaultDenominator = 4;
constexpr double minimumBpm = 20.0;
constexpr double maximumBpm = 300.0;
constexpr double timeTolerance = 1.0e-9;
constexpr std::size_t maximumReturnedBeatPositions = 1'000'000;

bool isSupportedDenominator(int denominator) noexcept
{
    switch (denominator)
    {
        case 1:
        case 2:
        case 4:
        case 8:
        case 16:
        case 32:
            return true;
        default:
            return false;
    }
}

TempoPoint defaultPoint() noexcept
{
    return { 0.0, defaultBpm, defaultNumerator, defaultDenominator };
}
}

TempoMap::TempoMap()
    : points { defaultPoint() }
{
}

TempoMap::TempoMap(std::vector<TempoPoint> pointsToUse)
    : points(normalise(std::move(pointsToUse)))
{
}

void TempoMap::setPoints(std::vector<TempoPoint> pointsToUse)
{
    points = normalise(std::move(pointsToUse));
}

TempoPoint TempoMap::getPointAt(double timeSeconds) const noexcept
{
    return points[findPointIndexAt(timeSeconds)];
}

double TempoMap::getBpmAt(double timeSeconds) const noexcept
{
    return getPointAt(timeSeconds).bpm;
}

std::pair<int, int> TempoMap::getTimeSignatureAt(double timeSeconds) const noexcept
{
    const auto point = getPointAt(timeSeconds);
    return { point.numerator, point.denominator };
}

double TempoMap::secondsToBeats(double timeSeconds) const noexcept
{
    if (!std::isfinite(timeSeconds) || timeSeconds <= 0.0)
        return 0.0;

    double result = 0.0;
    for (std::size_t index = 0; index < points.size(); ++index)
    {
        const double segmentStart = points[index].timeSeconds;
        if (timeSeconds <= segmentStart)
            break;

        const double segmentEnd = index + 1 < points.size()
            ? std::min(timeSeconds, points[index + 1].timeSeconds)
            : timeSeconds;
        if (segmentEnd > segmentStart)
            result += (segmentEnd - segmentStart) * beatsPerSecond(points[index]);

        if (segmentEnd >= timeSeconds)
            break;
    }
    return result;
}

double TempoMap::beatsToSeconds(double beats) const noexcept
{
    if (!std::isfinite(beats) || beats <= 0.0)
        return 0.0;

    double consumedBeats = 0.0;
    for (std::size_t index = 0; index < points.size(); ++index)
    {
        const double rate = beatsPerSecond(points[index]);
        if (index + 1 >= points.size())
            return points[index].timeSeconds + (beats - consumedBeats) / rate;

        const double segmentDuration = points[index + 1].timeSeconds
                                     - points[index].timeSeconds;
        const double segmentBeats = segmentDuration * rate;
        if (beats <= consumedBeats + segmentBeats + timeTolerance)
        {
            const double beatsIntoSegment = std::max(0.0, beats - consumedBeats);
            return points[index].timeSeconds + beatsIntoSegment / rate;
        }
        consumedBeats += segmentBeats;
    }

    return 0.0;
}

TempoMap::BeatPosition TempoMap::getBeatPosition(double timeSeconds) const noexcept
{
    const double safeTime = std::isfinite(timeSeconds)
        ? std::max(0.0, timeSeconds)
        : 0.0;
    const auto beatIndex = static_cast<std::int64_t>(
        std::floor(secondsToBeats(safeTime) + timeTolerance));
    const std::size_t activePoint = findPointIndexAt(safeTime);

    std::int64_t meterAnchorBeat = 0;
    std::int64_t barsBeforeAnchor = 0;
    int activeNumerator = std::max(1, points.front().numerator);
    int activeDenominator = points.front().denominator;

    for (std::size_t index = 1; index <= activePoint; ++index)
    {
        const auto& point = points[index];
        if (point.numerator == activeNumerator
            && point.denominator == activeDenominator)
            continue;

        // A meter change starts a new bar on the first cumulative beat at or
        // after the change point. BPM-only points deliberately keep the
        // existing bar phase.
        const auto changeBeat = static_cast<std::int64_t>(std::ceil(
            secondsToBeats(point.timeSeconds) - timeTolerance));
        const auto beatsInPreviousMeter = std::max<std::int64_t>(
            0, changeBeat - meterAnchorBeat);
        barsBeforeAnchor += (beatsInPreviousMeter + activeNumerator - 1)
                          / activeNumerator;
        meterAnchorBeat = changeBeat;
        activeNumerator = std::max(1, point.numerator);
        activeDenominator = point.denominator;
    }

    const auto beatsSinceAnchor = std::max<std::int64_t>(
        0, beatIndex - meterAnchorBeat);
    return {
        beatIndex,
        static_cast<int>(beatsSinceAnchor % activeNumerator),
        barsBeforeAnchor + beatsSinceAnchor / activeNumerator
    };
}

double TempoMap::snapToNearestBeat(double timeSeconds) const noexcept
{
    if (!std::isfinite(timeSeconds) || timeSeconds <= 0.0)
        return 0.0;

    const double beat = secondsToBeats(timeSeconds);
    return beatsToSeconds(std::max(0.0, std::floor(beat + 0.5)));
}

double TempoMap::snapToNextBeat(double timeSeconds) const noexcept
{
    if (!std::isfinite(timeSeconds) || timeSeconds <= 0.0)
        return 0.0;

    const double beat = secondsToBeats(timeSeconds);
    const double nextWholeBeat = std::ceil(beat - timeTolerance);
    return beatsToSeconds(std::max(0.0, nextWholeBeat));
}

std::vector<double> TempoMap::getBeatPositions(double startSeconds,
                                               double endSeconds) const
{
    if (!std::isfinite(startSeconds) || !std::isfinite(endSeconds)
        || startSeconds < 0.0 || endSeconds < startSeconds)
        return {};

    const double firstBeat = std::ceil(secondsToBeats(startSeconds)
                                       - timeTolerance);
    const double lastBeat = std::floor(secondsToBeats(endSeconds)
                                       + timeTolerance);
    if (lastBeat < firstBeat)
        return {};

    const double countAsDouble = lastBeat - firstBeat + 1.0;
    const auto count = static_cast<std::size_t>(std::min(
        countAsDouble,
        static_cast<double>(maximumReturnedBeatPositions)));

    std::vector<double> result;
    result.reserve(count);
    for (std::size_t offset = 0; offset < count; ++offset)
    {
        const double position = beatsToSeconds(firstBeat
                                               + static_cast<double>(offset));
        if (position + timeTolerance >= startSeconds
            && position <= endSeconds + timeTolerance)
            result.push_back(position);
    }
    return result;
}

std::vector<TempoPoint> TempoMap::normalise(std::vector<TempoPoint> input)
{
    std::vector<TempoPoint> valid;
    valid.reserve(input.size() + 1);

    for (auto point : input)
    {
        if (!std::isfinite(point.timeSeconds))
            continue;

        point.timeSeconds = std::max(0.0, point.timeSeconds);
        point.bpm = std::isfinite(point.bpm)
            ? juce::jlimit(minimumBpm, maximumBpm, point.bpm)
            : defaultBpm;
        point.numerator = juce::jlimit(1, 32, point.numerator);
        if (!isSupportedDenominator(point.denominator))
            point.denominator = defaultDenominator;
        valid.push_back(point);
    }

    std::stable_sort(valid.begin(), valid.end(),
        [](const TempoPoint& left, const TempoPoint& right)
        {
            return left.timeSeconds < right.timeSeconds;
        });

    std::vector<TempoPoint> merged;
    merged.reserve(valid.size() + 1);
    for (const auto& point : valid)
    {
        if (!merged.empty()
            && std::abs(merged.back().timeSeconds - point.timeSeconds)
                   <= timeTolerance)
        {
            // Stable sorting preserves input order, so the last user-provided
            // point deterministically wins at a duplicate timestamp.
            merged.back() = point;
        }
        else
        {
            merged.push_back(point);
        }
    }

    if (merged.empty() || merged.front().timeSeconds > timeTolerance)
        merged.insert(merged.begin(), defaultPoint());
    else
        merged.front().timeSeconds = 0.0;

    return merged;
}

double TempoMap::beatsPerSecond(const TempoPoint& point) noexcept
{
    return (point.bpm / 60.0)
         * (static_cast<double>(point.denominator) / 4.0);
}

std::size_t TempoMap::findPointIndexAt(double timeSeconds) const noexcept
{
    const double safeTime = std::isfinite(timeSeconds)
        ? std::max(0.0, timeSeconds)
        : 0.0;

    const auto iterator = std::upper_bound(
        points.begin(), points.end(), safeTime,
        [](double value, const TempoPoint& point)
        {
            return value < point.timeSeconds;
        });
    if (iterator == points.begin())
        return 0;
    return static_cast<std::size_t>(std::distance(points.begin(), iterator) - 1);
}
