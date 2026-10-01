#include "RhythmRender.h"

#include "PlaybackTransform.h"
#include "RhythmWarp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace
{
constexpr std::uint64_t fnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t fnvPrime = 1099511628211ULL;

void hashValue(std::uint64_t& hash, std::uint64_t value) noexcept
{
    for (int byte = 0; byte < 8; ++byte)
    {
        hash ^= (value >> (byte * 8)) & 0xffULL;
        hash *= fnvPrime;
    }
}

void hashDouble(std::uint64_t& hash, double value) noexcept
{
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    hashValue(hash, bits);
}

struct Anchor
{
    double source = 0.0;
    double target = 0.0;
};

std::vector<Anchor> makeAnchors(const AudioClip& clip)
{
    const double duration = std::max(0.0, clip.duration);
    const auto markers = RhythmWarp::sanitise(clip.rhythmMarkers, duration);
    std::vector<Anchor> anchors;
    anchors.reserve(markers.size() * 2 + 2);
    anchors.push_back({ 0.0, 0.0 });

    const auto append = [&anchors, duration](double source, double target)
    {
        source = juce::jlimit(0.0, duration, source);
        target = juce::jlimit(0.0, duration, target);
        const auto& previous = anchors.back();
        if (source < previous.source - 1.0e-9
            || target < previous.target - 1.0e-9)
            return;
        if (std::abs(source - previous.source) <= 1.0e-9
            && std::abs(target - previous.target) <= 1.0e-9)
            return;
        if (source <= previous.source + 1.0e-9
            || target <= previous.target + 1.0e-9)
            return;
        anchors.push_back({ source, target });
    };

    for (const auto& marker : markers)
    {
        append(marker.sourceTime, marker.targetTime);
        if (marker.hasDuration())
            append(marker.sourceEndTime, marker.targetEndTime);
    }
    append(duration, duration);
    return anchors;
}

std::shared_ptr<juce::AudioBuffer<float>> stretchToLength(
    const juce::AudioBuffer<float>& input,
    double sampleRate,
    int targetSamples,
    const std::function<bool()>& shouldCancel)
{
    if (targetSamples <= 0 || input.getNumSamples() <= 0)
        return {};

    auto current = std::make_shared<juce::AudioBuffer<float>>(input);
    // Keep every Signalsmith step in the well-behaved 0.5x..2x range. This
    // also handles legacy point anchors whose adjacent interval is extreme.
    for (int pass = 0; pass < 12; ++pass)
    {
        if (shouldCancel && shouldCancel())
            return {};
        const int currentSamples = current->getNumSamples();
        if (currentSamples <= 0)
            return {};
        if (currentSamples == targetSamples)
            return current;

        const double desiredSpeed = static_cast<double>(currentSamples)
                                  / static_cast<double>(targetSamples);
        const double stepSpeed = desiredSpeed > 2.0 ? 2.0
                               : desiredSpeed < 0.5 ? 0.5
                               : desiredSpeed;
        current = PlaybackTransform::processOffline(*current,
                                                     sampleRate,
                                                     stepSpeed,
                                                     0.0,
                                                     shouldCancel);
        if (current == nullptr)
            return {};
        if (stepSpeed == desiredSpeed)
            break;
    }

    if (current == nullptr || current->getNumSamples() != targetSamples)
        return {};
    return current;
}

void applyBoundaryFades(juce::AudioBuffer<float>& buffer,
                        const std::vector<int>& boundaries,
                        int sampleRate)
{
    const int defaultFade = std::max(1, juce::roundToInt(sampleRate * 0.002));
    int previousBoundary = 0;
    for (size_t boundaryIndex = 0; boundaryIndex < boundaries.size();
         ++boundaryIndex)
    {
        const int boundary = boundaries[boundaryIndex];
        const int nextBoundary = boundaryIndex + 1 < boundaries.size()
            ? boundaries[boundaryIndex + 1] : buffer.getNumSamples();
        const int fade = std::min({ defaultFade,
                                    std::max(0, boundary - previousBoundary) / 2,
                                    std::max(0, nextBoundary - boundary) / 2 });
        if (fade <= 0 || boundary <= 0 || boundary >= buffer.getNumSamples())
        {
            previousBoundary = boundary;
            continue;
        }

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            auto* samples = buffer.getWritePointer(channel);
            for (int offset = 0; offset < fade; ++offset)
            {
                const float fadeOut = static_cast<float>(fade - offset - 1)
                                    / static_cast<float>(fade);
                const float fadeIn = static_cast<float>(offset + 1)
                                   / static_cast<float>(fade);
                samples[boundary - fade + offset] *= fadeOut;
                samples[boundary + offset] *= fadeIn;
            }
        }
        previousBoundary = boundary;
    }
}
}

std::uint64_t RhythmRender::signatureFor(const AudioClip& clip) noexcept
{
    std::uint64_t hash = fnvOffset;
    hashValue(hash, static_cast<std::uint64_t>(
                        reinterpret_cast<std::uintptr_t>(clip.buffer.get())));
    hashValue(hash, static_cast<std::uint64_t>(clip.sampleRate));
    hashDouble(hash, clip.offset);
    hashDouble(hash, clip.duration);
    if (clip.buffer != nullptr)
    {
        hashValue(hash, static_cast<std::uint64_t>(clip.buffer->getNumChannels()));
        hashValue(hash, static_cast<std::uint64_t>(clip.buffer->getNumSamples()));
    }
    hashValue(hash, static_cast<std::uint64_t>(clip.rhythmMarkers.size()));
    for (const auto& marker : clip.rhythmMarkers)
    {
        hashDouble(hash, marker.sourceTime);
        hashDouble(hash, marker.targetTime);
        hashDouble(hash, marker.sourceEndTime);
        hashDouble(hash, marker.targetEndTime);
    }
    return hash;
}

bool RhythmRenderCache::matches(const AudioClip& clip) const noexcept
{
    return signature == RhythmRender::signatureFor(clip)
        && sampleRate == clip.sampleRate
        && std::abs(duration - clip.duration) <= 1.0e-9
        && buffer.getNumSamples()
            == std::max(0, juce::roundToInt(clip.duration * clip.sampleRate));
}

std::shared_ptr<const RhythmRenderCache> RhythmRender::render(
    const AudioClip& clip,
    const std::function<bool()>& shouldCancel)
{
    if (clip.buffer == nullptr || clip.sampleRate <= 0
        || clip.duration <= 0.0 || clip.rhythmMarkers.empty())
        return {};
    if (shouldCancel && shouldCancel())
        return {};

    const int channels = juce::jlimit(1, 2, clip.buffer->getNumChannels());
    const int totalSamples = std::max(
        1, juce::roundToInt(clip.duration * clip.sampleRate));
    auto result = std::make_shared<RhythmRenderCache>();
    result->signature = signatureFor(clip);
    result->sampleRate = clip.sampleRate;
    result->duration = clip.duration;
    result->buffer.setSize(channels, totalSamples);
    result->buffer.clear();

    const auto anchors = makeAnchors(clip);
    if (anchors.size() < 2)
        return {};

    std::vector<int> warpedBoundaries;
    for (size_t index = 0; index + 1 < anchors.size(); ++index)
    {
        if (shouldCancel && shouldCancel())
            return {};
        const auto& left = anchors[index];
        const auto& right = anchors[index + 1];
        const int targetStart = juce::jlimit(
            0, totalSamples,
            juce::roundToInt(left.target * clip.sampleRate));
        const int targetEnd = juce::jlimit(
            targetStart, totalSamples,
            juce::roundToInt(right.target * clip.sampleRate));
        const int targetCount = targetEnd - targetStart;

        const int sourceStart = juce::jlimit(
            0, clip.buffer->getNumSamples(),
            juce::roundToInt((clip.offset + left.source) * clip.sampleRate));
        const int sourceEnd = juce::jlimit(
            sourceStart, clip.buffer->getNumSamples(),
            juce::roundToInt((clip.offset + right.source) * clip.sampleRate));
        const int sourceCount = sourceEnd - sourceStart;
        if (targetCount <= 0 || sourceCount <= 0)
            continue;

        juce::AudioBuffer<float> sourceSegment(channels, sourceCount);
        for (int channel = 0; channel < channels; ++channel)
            sourceSegment.copyFrom(channel, 0, *clip.buffer,
                                   std::min(channel,
                                            clip.buffer->getNumChannels() - 1),
                                   sourceStart, sourceCount);

        std::shared_ptr<juce::AudioBuffer<float>> rendered;
        if (sourceCount == targetCount)
            rendered = std::make_shared<juce::AudioBuffer<float>>(sourceSegment);
        else
            rendered = stretchToLength(sourceSegment,
                                       clip.sampleRate,
                                       targetCount,
                                       shouldCancel);
        if (rendered == nullptr || rendered->getNumSamples() != targetCount)
            return {};

        for (int channel = 0; channel < channels; ++channel)
            result->buffer.copyFrom(channel, targetStart, *rendered,
                                    std::min(channel,
                                             rendered->getNumChannels() - 1),
                                    0, targetCount);

        const double sourceLength = right.source - left.source;
        const double targetLength = right.target - left.target;
        if (targetEnd < totalSamples
            && std::abs(sourceLength - targetLength) > 1.0e-6)
            warpedBoundaries.push_back(targetEnd);
    }

    std::sort(warpedBoundaries.begin(), warpedBoundaries.end());
    warpedBoundaries.erase(
        std::unique(warpedBoundaries.begin(), warpedBoundaries.end()),
        warpedBoundaries.end());
    applyBoundaryFades(result->buffer, warpedBoundaries, clip.sampleRate);
    return result;
}
