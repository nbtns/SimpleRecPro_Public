#include "AraTimelineAudio.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace
{
int64_t getVisibleSourceSampleCount(const AudioClip& clip) noexcept
{
    if (clip.buffer == nullptr || clip.sampleRate <= 0)
        return 0;

    const auto fullSampleCount =
        static_cast<int64_t>(clip.buffer->getNumSamples());
    const auto startSample = std::clamp<int64_t>(
        static_cast<int64_t>(std::llround(
            std::max(0.0, clip.offset) * clip.sampleRate)),
        0,
        fullSampleCount);
    const auto requestedSamples = static_cast<int64_t>(std::llround(
        std::max(0.0, clip.duration) * clip.sampleRate));
    return std::clamp<int64_t>(requestedSamples,
                               0,
                               fullSampleCount - startSample);
}

double getTrackDuration(const TrackData& track) noexcept
{
    double duration = 0.0;
    for (const auto& clip : track.clips)
        if (AraTimelineAudio::isUsableClip(clip))
            duration = std::max(
                duration,
                std::max(0.0, clip.startTime) + clip.duration);
    return duration;
}

struct TimelineSegment
{
    const juce::AudioBuffer<float>* buffer = nullptr;
    double sampleRate = 0.0;
    double startTime = 0.0;
    double offset = 0.0;
    double duration = 0.0;
};

std::vector<TimelineSegment> makeCanonicalSegments(
    const std::vector<AudioClip>& clips)
{
    std::vector<TimelineSegment> segments;
    for (const auto& clip : clips)
    {
        if (!AraTimelineAudio::isUsableClip(clip))
            continue;
        segments.push_back({
            clip.buffer.get(),
            static_cast<double>(clip.sampleRate),
            clip.startTime,
            clip.offset,
            clip.duration
        });
    }
    std::sort(segments.begin(), segments.end(),
        [](const TimelineSegment& left, const TimelineSegment& right)
        {
            if (left.startTime != right.startTime)
                return left.startTime < right.startTime;
            if (left.buffer != right.buffer)
                return std::less<const juce::AudioBuffer<float>*>()(
                    left.buffer, right.buffer);
            return left.offset < right.offset;
        });

    std::vector<TimelineSegment> canonical;
    for (const auto& segment : segments)
    {
        if (!canonical.empty())
        {
            auto& previous = canonical.back();
            const bool contiguous =
                previous.buffer == segment.buffer
                && previous.sampleRate == segment.sampleRate
                && std::abs(previous.startTime + previous.duration
                            - segment.startTime) <= 1.0e-9
                && std::abs(previous.offset + previous.duration
                            - segment.offset) <= 1.0e-9;
            if (contiguous)
            {
                previous.duration += segment.duration;
                continue;
            }
        }
        canonical.push_back(segment);
    }
    return canonical;
}
}

namespace AraTimelineAudio
{
bool isUsableClip(const AudioClip& clip) noexcept
{
    return clip.id.isNotEmpty()
        && clip.buffer != nullptr
        && clip.buffer->getNumSamples() > 0
        && clip.buffer->getNumChannels() > 0
        && clip.sampleRate > 0
        && clip.duration > 0.0
        && getVisibleSourceSampleCount(clip) > 0;
}

bool hasSameRenderedAudio(const std::vector<AudioClip>& previous,
                          const std::vector<AudioClip>& next)
{
    const auto previousSegments = makeCanonicalSegments(previous);
    const auto nextSegments = makeCanonicalSegments(next);
    if (previousSegments.size() != nextSegments.size())
        return false;
    for (size_t index = 0; index < previousSegments.size(); ++index)
    {
        const auto& left = previousSegments[index];
        const auto& right = nextSegments[index];
        if (left.buffer != right.buffer
            || left.sampleRate != right.sampleRate
            || std::abs(left.startTime - right.startTime) > 1.0e-9
            || std::abs(left.offset - right.offset) > 1.0e-9
            || std::abs(left.duration - right.duration) > 1.0e-9)
            return false;
    }
    return true;
}

Reader::Reader(const TrackData& track)
{
    setTrack(track);
}

void Reader::setTrack(const TrackData& track)
{
    clips.clear();
    sampleRate = 1;
    channelCount = 1;
    for (const auto& clip : track.clips)
    {
        if (!isUsableClip(clip))
            continue;
        clips.push_back(clip);
        sampleRate = std::max(sampleRate, clip.sampleRate);
        channelCount = std::max(channelCount,
                                clip.buffer->getNumChannels());
    }
    if (clips.empty())
        sampleRate = 44100;

    const auto exactSampleCount =
        std::ceil(getTrackDuration(track) * sampleRate);
    if (exactSampleCount <= 0.0)
        sampleCount = 0;
    else if (exactSampleCount
             >= static_cast<double>(std::numeric_limits<int64_t>::max()))
        sampleCount = std::numeric_limits<int64_t>::max();
    else
        sampleCount = static_cast<int64_t>(exactSampleCount);
}

double Reader::getDuration() const noexcept
{
    return sampleRate > 0
        ? static_cast<double>(sampleCount) / sampleRate
        : 0.0;
}

bool Reader::read(float* const* destination,
                  int64_t startSample,
                  int64_t requestedSamples) const noexcept
{
    return readSamples(destination, startSample, requestedSamples);
}

bool Reader::read(double* const* destination,
                  int64_t startSample,
                  int64_t requestedSamples) const noexcept
{
    return readSamples(destination, startSample, requestedSamples);
}

template <typename Sample>
bool Reader::readSamples(Sample* const* destination,
                         int64_t startSample,
                         int64_t requestedSamples) const noexcept
{
    if (destination == nullptr || requestedSamples < 0
        || requestedSamples > std::numeric_limits<int>::max()
        || startSample > std::numeric_limits<int64_t>::max() - requestedSamples)
        return false;

    const auto length = static_cast<int>(requestedSamples);
    for (int channel = 0; channel < channelCount; ++channel)
    {
        if (destination[channel] == nullptr)
            return false;
        std::fill_n(destination[channel], length, Sample{});
    }

    const auto requestEnd = startSample + requestedSamples;
    for (const auto& clip : clips)
    {
        const auto clipStart = static_cast<int64_t>(std::llround(
            std::max(0.0, clip.startTime) * sampleRate));
        const auto clipEnd = static_cast<int64_t>(std::llround(
            (std::max(0.0, clip.startTime) + clip.duration) * sampleRate));
        const auto overlapStart = std::max(startSample, clipStart);
        const auto overlapEnd = std::min(requestEnd, clipEnd);
        if (overlapEnd <= overlapStart)
            continue;

        const auto sourceRate = static_cast<double>(clip.sampleRate);
        const auto sourceSamples = clip.buffer->getNumSamples();
        const auto sourceChannels = clip.buffer->getNumChannels();
        const auto destinationOffset =
            static_cast<int>(overlapStart - startSample);
        const auto samplesToMix =
            static_cast<int>(overlapEnd - overlapStart);
        const double firstSourcePosition =
            std::max(0.0, clip.offset) * sourceRate
            + static_cast<double>(overlapStart - clipStart)
                * sourceRate / sampleRate;
        const double sourceStep = sourceRate / sampleRate;

        for (int index = 0; index < samplesToMix; ++index)
        {
            const auto sourcePosition =
                firstSourcePosition + static_cast<double>(index) * sourceStep;
            if (sourcePosition < 0.0 || sourcePosition >= sourceSamples)
                continue;

            const auto index0 = static_cast<int>(std::floor(sourcePosition));
            const auto index1 = std::min(index0 + 1, sourceSamples - 1);
            const auto fraction = sourcePosition - index0;
            for (int channel = 0; channel < channelCount; ++channel)
            {
                const auto sourceChannel = sourceChannels == 1
                    ? 0
                    : std::min(channel, sourceChannels - 1);
                const auto* input =
                    clip.buffer->getReadPointer(sourceChannel);
                const auto value = input[index0]
                    + (input[index1] - input[index0]) * fraction;
                destination[channel][destinationOffset + index]
                    += static_cast<Sample>(value);
            }
        }
    }
    return true;
}
}
