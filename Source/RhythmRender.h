#pragma once

#include "TrackData.h"

#include <cstdint>
#include <functional>
#include <memory>

/**
 * Immutable audio produced from a clip's rhythm anchors.
 *
 * The buffer covers the visible clip duration at the clip's own sample rate.
 * Each piecewise rhythm interval is time-stretched with zero pitch shift, so
 * the audio callback only needs ordinary cached sample reads.
 */
struct RhythmRenderCache
{
    std::uint64_t signature = 0;
    int sampleRate = 0;
    double duration = 0.0;
    juce::AudioBuffer<float> buffer;

    bool matches(const AudioClip& clip) const noexcept;
};

namespace RhythmRender
{
std::uint64_t signatureFor(const AudioClip& clip) noexcept;

std::shared_ptr<const RhythmRenderCache> render(
    const AudioClip& clip,
    const std::function<bool()>& shouldCancel = {});
}
