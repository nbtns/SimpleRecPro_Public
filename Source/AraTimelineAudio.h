#pragma once

#include "TrackData.h"

#include <cstdint>
#include <vector>

namespace AraTimelineAudio
{
bool isUsableClip(const AudioClip& clip) noexcept;

/**
 * Returns true when clip edits leave the audible timeline unchanged.
 * A split into adjacent source-contiguous clips is treated as the same audio.
 */
bool hasSameRenderedAudio(const std::vector<AudioClip>& previous,
                          const std::vector<AudioClip>& next);

/**
 * Presents the clips from one track as one continuous ARA audio source.
 *
 * Gaps are returned as silence. Clips with a different sample rate are
 * resampled onto the timeline with linear interpolation.
 */
class Reader final
{
public:
    Reader() = default;
    explicit Reader(const TrackData& track);

    void setTrack(const TrackData& track);

    bool read(float* const* destination,
              int64_t startSample,
              int64_t requestedSamples) const noexcept;
    bool read(double* const* destination,
              int64_t startSample,
              int64_t requestedSamples) const noexcept;

    int getSampleRate() const noexcept { return sampleRate; }
    int getChannelCount() const noexcept { return channelCount; }
    int64_t getSampleCount() const noexcept { return sampleCount; }
    double getDuration() const noexcept;
    const std::vector<AudioClip>& getClips() const noexcept { return clips; }

private:
    template <typename Sample>
    bool readSamples(Sample* const* destination,
                     int64_t startSample,
                     int64_t requestedSamples) const noexcept;

    std::vector<AudioClip> clips;
    int sampleRate = 44100;
    int channelCount = 1;
    int64_t sampleCount = 0;
};
}
