#include "RhythmWarp.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double minimumGap = 0.001;

struct Anchor
{
    double source = 0.0;
    double target = 0.0;
};
}

std::vector<RhythmMarker> RhythmWarp::sanitise(
    std::vector<RhythmMarker> markers,
    double clipDuration)
{
    const double duration = std::max(0.0, clipDuration);
    for (auto& marker : markers)
    {
        marker.sourceTime = juce::jlimit(0.0, duration, marker.sourceTime);
        marker.targetTime = juce::jlimit(0.0, duration, marker.targetTime);
        marker.sourceEndTime = juce::jlimit(
            marker.sourceTime, duration, marker.sourceEndTime);
        marker.targetEndTime = juce::jlimit(
            marker.targetTime, duration, marker.targetEndTime);
        if (!marker.hasDuration())
        {
            marker.sourceEndTime = marker.sourceTime;
            marker.targetEndTime = marker.targetTime;
        }
    }

    std::stable_sort(markers.begin(), markers.end(),
        [](const RhythmMarker& left, const RhythmMarker& right)
        {
            if (!approximatelyEqual(left.targetTime, right.targetTime))
                return left.targetTime < right.targetTime;
            return left.sourceTime < right.sourceTime;
        });

    std::vector<RhythmMarker> result;
    result.reserve(markers.size());
    double previousSource = -minimumGap * 2.0;
    double previousTarget = -minimumGap * 2.0;
    for (const auto& marker : markers)
    {
        if (marker.sourceTime <= previousSource + minimumGap
            || marker.targetTime <= previousTarget + minimumGap
            || marker.targetTime >= duration - minimumGap)
            continue;
        result.push_back(marker);
        previousSource = marker.sourceTime;
        previousTarget = marker.targetTime;
    }

    for (size_t index = 0; index < result.size(); ++index)
    {
        auto& marker = result[index];
        if (!marker.hasDuration())
            continue;
        const double nextSource = index + 1 < result.size()
            ? result[index + 1].sourceTime - minimumGap : duration;
        const double nextTarget = index + 1 < result.size()
            ? result[index + 1].targetTime - minimumGap : duration;
        marker.sourceEndTime = juce::jlimit(
            marker.sourceTime, std::max(marker.sourceTime, nextSource),
            marker.sourceEndTime);
        marker.targetEndTime = juce::jlimit(
            marker.targetTime, std::max(marker.targetTime, nextTarget),
            marker.targetEndTime);
        const double sourceLength = marker.sourceEndTime - marker.sourceTime;
        const double availableTargetLength = std::max(
            0.0, nextTarget - marker.targetTime);
        if (sourceLength > minimumGap
            && availableTargetLength >= sourceLength * 0.5)
        {
            const double minimumTargetLength = sourceLength * 0.5;
            const double maximumTargetLength = std::min(
                sourceLength * 2.0, availableTargetLength);
            marker.targetEndTime = marker.targetTime + juce::jlimit(
                minimumTargetLength,
                std::max(minimumTargetLength, maximumTargetLength),
                marker.targetEndTime - marker.targetTime);
        }
        if (!marker.hasDuration())
        {
            marker.sourceEndTime = marker.sourceTime;
            marker.targetEndTime = marker.targetTime;
        }
    }
    return result;
}

double RhythmWarp::mapTargetToSource(const std::vector<RhythmMarker>& markers,
                                     double clipDuration,
                                     double targetTime) noexcept
{
    const double duration = std::max(0.0, clipDuration);
    const double target = juce::jlimit(0.0, duration, targetTime);
    Anchor previous;

    const auto interpolate = [target](const Anchor& left,
                                      const Anchor& right) noexcept
    {
        if (right.target <= left.target + 1.0e-12)
            return right.source;
        const double proportion = (target - left.target)
                                / (right.target - left.target);
        return left.source + proportion * (right.source - left.source);
    };

    for (const auto& marker : markers)
    {
        const Anchor start {
            juce::jlimit(0.0, duration, marker.sourceTime),
            juce::jlimit(0.0, duration, marker.targetTime)
        };
        if (start.target <= 1.0e-12 && previous.target <= 1.0e-12
            && start.source >= previous.source)
        {
            previous = start;
            if (target <= 1.0e-12)
                return previous.source;
        }
        if (start.target > previous.target + 1.0e-12
            && start.source >= previous.source)
        {
            if (target <= start.target)
                return juce::jlimit(0.0, duration,
                                    interpolate(previous, start));
            previous = start;
        }

        if (marker.hasDuration())
        {
            const Anchor end {
                juce::jlimit(previous.source, duration, marker.sourceEndTime),
                juce::jlimit(previous.target, duration, marker.targetEndTime)
            };
            if (end.target > previous.target + 1.0e-12)
            {
                if (target <= end.target)
                    return juce::jlimit(0.0, duration,
                                        interpolate(previous, end));
                previous = end;
            }
        }
    }

    const Anchor finalAnchor { duration, duration };
    if (target <= previous.target + 1.0e-12)
        return juce::jlimit(0.0, duration, previous.source);
    return juce::jlimit(0.0, duration,
                        interpolate(previous, finalAnchor));
}
