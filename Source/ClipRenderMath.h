#pragma once

#include <cstdint>

namespace ClipRenderMath
{
/** Left/right gains for a mono signal using a -3 dB centre pan law. */
struct StereoGains
{
    float left = 0.70710678f;
    float right = 0.70710678f;
};

/** A clamped timeline range. `valid` is false when no positive range remains. */
struct NormalisedRange
{
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    bool valid = false;

    double lengthSeconds() const noexcept
    {
        return valid ? endSeconds - startSeconds : 0.0;
    }
};

/**
 * Describes one contiguous render segment before a loop boundary.
 *
 * Callers can render `samplesToRender` from `startSample`, then continue from
 * `nextSample`. If `wrapped` is true, DSP with seek-sensitive state should be
 * reset or crossfaded before rendering the next segment.
 */
struct LoopSegment
{
    std::int64_t startSample = 0;
    int samplesToRender = 0;
    std::int64_t nextSample = 0;
    bool wrapped = false;
    bool loopIsValid = false;
};

/**
 * Returns a deterministic equal-power fade gain in the range [0, 1].
 * Invalid values are treated as silence. Overlapping fades are shortened
 * proportionally so their combined duration never exceeds the clip.
 */
float getFadeGain(double positionSeconds,
                  double clipDurationSeconds,
                  double fadeInSeconds,
                  double fadeOutSeconds) noexcept;

/** Returns constant-power pan gains for `pan` clamped to [-1, 1]. */
StereoGains getConstantPowerPanGains(float pan) noexcept;

/**
 * Orders A/B, clamps both points to [0, timelineDurationSeconds], and enforces
 * an optional minimum length when possible. Non-finite input is invalid.
 */
NormalisedRange normaliseRange(double pointASeconds,
                               double pointBSeconds,
                               double timelineDurationSeconds,
                               double minimumLengthSeconds = 0.0) noexcept;

/** Returns `position` wrapped into [loopStart, loopEnd), when the loop is valid. */
std::int64_t wrapLoopPosition(std::int64_t position,
                              std::int64_t loopStart,
                              std::int64_t loopEnd) noexcept;

/**
 * Splits a requested render block at the next loop end without allocating.
 * Invalid loops behave like ordinary linear transport.
 */
LoopSegment decideLoopSegment(std::int64_t currentSample,
                              int requestedSamples,
                              std::int64_t loopStart,
                              std::int64_t loopEnd) noexcept;
}
