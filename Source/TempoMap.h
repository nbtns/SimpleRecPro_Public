#pragma once

#include "DawFeatureTypes.h"

#include <cstdint>
#include <utility>
#include <vector>

/**
 * Deterministic tempo/meter conversion for the project timeline.
 *
 * Beat counts use the active time-signature denominator, matching the current
 * metronome: at 120 BPM a quarter-note beat lasts 0.5 s, while an eighth-note
 * beat lasts 0.25 s.
 */
class TempoMap
{
public:
    struct BeatPosition
    {
        std::int64_t beatIndex = 0;
        int beatInBar = 0;
        std::int64_t barIndex = 0;
    };

    TempoMap();
    explicit TempoMap(std::vector<TempoPoint> pointsToUse);

    void setPoints(std::vector<TempoPoint> pointsToUse);
    const std::vector<TempoPoint>& getPoints() const noexcept { return points; }

    TempoPoint getPointAt(double timeSeconds) const noexcept;
    double getBpmAt(double timeSeconds) const noexcept;
    std::pair<int, int> getTimeSignatureAt(double timeSeconds) const noexcept;

    double secondsToBeats(double timeSeconds) const noexcept;
    double beatsToSeconds(double beats) const noexcept;

    /** Returns the cumulative beat/bar position at timeSeconds. */
    BeatPosition getBeatPosition(double timeSeconds) const noexcept;

    /** Returns the nearest beat boundary on the cumulative project grid. */
    double snapToNearestBeat(double timeSeconds) const noexcept;

    /** Returns the first beat boundary at or after timeSeconds. */
    double snapToNextBeat(double timeSeconds) const noexcept;

    /**
     * Returns beat boundaries in the closed [startSeconds, endSeconds] range.
     * Invalid/reversed ranges return an empty vector.
     */
    std::vector<double> getBeatPositions(double startSeconds,
                                         double endSeconds) const;

private:
    static std::vector<TempoPoint> normalise(std::vector<TempoPoint> input);
    static double beatsPerSecond(const TempoPoint& point) noexcept;
    std::size_t findPointIndexAt(double timeSeconds) const noexcept;

    std::vector<TempoPoint> points;
};
