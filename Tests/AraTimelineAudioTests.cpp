#include "../Source/AraTimelineAudio.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace
{
bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAILED: " << message << std::endl;
    return condition;
}

bool isNear(double actual, double expected)
{
    return std::abs(actual - expected) < 1.0e-6;
}

std::shared_ptr<juce::AudioBuffer<float>> makeMonoBuffer(float base)
{
    auto buffer = std::make_shared<juce::AudioBuffer<float>>(1, 10);
    for (int sample = 0; sample < buffer->getNumSamples(); ++sample)
        buffer->setSample(0, sample, base + 0.01f * sample);
    return buffer;
}

std::shared_ptr<juce::AudioBuffer<float>> makeStereoBuffer()
{
    auto buffer = std::make_shared<juce::AudioBuffer<float>>(2, 10);
    for (int sample = 0; sample < buffer->getNumSamples(); ++sample)
    {
        buffer->setSample(0, sample, 0.40f + 0.01f * sample);
        buffer->setSample(1, sample, -0.40f - 0.01f * sample);
    }
    return buffer;
}

bool testSeparatedClipsBecomeOneSilentGapTimeline()
{
    AudioClip first;
    first.id = "clip-a";
    first.buffer = makeMonoBuffer(0.10f);
    first.sampleRate = 10;
    first.startTime = 1.0;
    first.offset = 0.2;
    first.duration = 0.4;

    AudioClip second;
    second.id = "clip-b";
    second.buffer = makeStereoBuffer();
    second.sampleRate = 10;
    second.startTime = 3.0;
    second.offset = 0.1;
    second.duration = 0.3;

    TrackData track;
    track.id = "track-a";
    track.clips = { first, second };

    TrackData foreignTrack;
    foreignTrack.id = "track-b";
    AudioClip foreign;
    foreign.id = "foreign";
    foreign.buffer = makeMonoBuffer(0.90f);
    foreign.sampleRate = 10;
    foreign.startTime = 2.0;
    foreign.duration = 0.5;
    foreignTrack.clips = { foreign };

    AraTimelineAudio::Reader reader(track);
    AraTimelineAudio::Reader foreignReader(foreignTrack);
    float left[40] {};
    float right[40] {};
    float foreignOutput[40] {};
    float* channels[] { left, right };
    float* foreignChannels[] { foreignOutput };

    bool ok = expect(reader.getSampleRate() == 10,
                     "timeline should retain the track clip sample rate")
        && expect(reader.getChannelCount() == 2,
                  "timeline should expose enough channels for every clip")
        && expect(reader.getSampleCount() == 33,
                  "timeline should end at the end of the last clip")
        && expect(reader.read(channels, 0, 40),
                  "timeline float read should succeed")
        && expect(foreignReader.read(foreignChannels, 0, 40),
                  "foreign track read should succeed");

    for (int sample = 0; sample < 10; ++sample)
        ok = expect(isNear(left[sample], 0.0)
                        && isNear(right[sample], 0.0),
                    "audio before the first clip should be silent") && ok;

    for (int sample = 0; sample < 4; ++sample)
    {
        const auto expected = 0.12 + 0.01 * sample;
        ok = expect(isNear(left[10 + sample], expected)
                        && isNear(right[10 + sample], expected),
                    "the first clip should use its source offset") && ok;
    }

    for (int sample = 14; sample < 30; ++sample)
        ok = expect(isNear(left[sample], 0.0)
                        && isNear(right[sample], 0.0),
                    "the gap between clips should be silent") && ok;

    for (int sample = 0; sample < 3; ++sample)
    {
        ok = expect(isNear(left[30 + sample], 0.41 + 0.01 * sample)
                        && isNear(right[30 + sample],
                                  -0.41 - 0.01 * sample),
                    "the second clip should appear at its timeline position") && ok;
    }

    for (int sample = 33; sample < 40; ++sample)
        ok = expect(isNear(left[sample], 0.0)
                        && isNear(right[sample], 0.0),
                    "audio after the track end should be silent") && ok;

    ok = expect(isNear(left[20], 0.0)
                    && isNear(foreignOutput[20], 0.90),
                "another track's audio must not enter this timeline") && ok;

    double partialLeft[7] {};
    double partialRight[7] {};
    double* partialChannels[] { partialLeft, partialRight };
    ok = expect(reader.read(partialChannels, 28, 7),
                "timeline double read with a non-zero request offset should succeed") && ok;
    ok = expect(isNear(partialLeft[0], 0.0)
                    && isNear(partialLeft[1], 0.0)
                    && isNear(partialLeft[2], 0.41)
                    && isNear(partialLeft[4], 0.43)
                    && isNear(partialLeft[5], 0.0)
                    && isNear(partialLeft[6], 0.0),
                "partial reads should align the second clip and trailing silence") && ok;
    return ok;
}

bool testEmptyTrackTransitions()
{
    TrackData track;
    track.id = "track";
    AraTimelineAudio::Reader reader(track);

    float output[4] { 1.0f, 1.0f, 1.0f, 1.0f };
    float* channels[] { output };
    bool ok = expect(reader.getSampleCount() == 0,
                     "an empty track should have no timeline samples")
        && expect(reader.read(channels, 0, 4),
                  "an empty timeline should still return a valid silent read")
        && expect(isNear(output[0], 0.0) && isNear(output[3], 0.0),
                  "an empty timeline read should be silent");

    AudioClip clip;
    clip.id = "first";
    clip.buffer = makeMonoBuffer(0.20f);
    clip.sampleRate = 10;
    clip.startTime = 0.5;
    clip.duration = 0.2;
    track.clips = { clip };
    reader.setTrack(track);
    ok = expect(reader.getSampleCount() == 7,
                "adding the first clip should create a timeline length") && ok;

    std::fill_n(output, 4, 0.0f);
    ok = expect(reader.read(channels, 4, 4)
                    && isNear(output[0], 0.0)
                    && isNear(output[1], 0.20)
                    && isNear(output[2], 0.21)
                    && isNear(output[3], 0.0),
                "the first added clip should become readable in place") && ok;

    track.clips.clear();
    reader.setTrack(track);
    std::fill_n(output, 4, 1.0f);
    ok = expect(reader.getSampleCount() == 0,
                "deleting the last clip should return to an empty timeline") && ok;
    ok = expect(reader.read(channels, 0, 4)
                    && isNear(output[0], 0.0)
                    && isNear(output[3], 0.0),
                "the timeline should remain safe and silent after the last deletion")
        && ok;
    return ok;
}

bool testSplitPreservesRenderedTimeline()
{
    AudioClip original;
    original.id = "original";
    original.buffer = makeMonoBuffer(0.10f);
    original.sampleRate = 10;
    original.startTime = 1.0;
    original.offset = 0.0;
    original.duration = 0.8;

    AudioClip first = original;
    first.duration = 0.5;
    AudioClip second = original;
    second.id = "split";
    second.startTime = 1.5;
    second.offset = 0.5;
    second.duration = 0.3;

    auto moved = second;
    moved.startTime = 2.0;
    return expect(
               AraTimelineAudio::hasSameRenderedAudio(
                   { original }, { first, second }),
               "splitting a clip must not request analysis for unchanged audio")
        && expect(
               !AraTimelineAudio::hasSameRenderedAudio(
                   { original }, { first, moved }),
               "moving a split section must be treated as changed timeline audio")
        && expect(
               !AraTimelineAudio::hasSameRenderedAudio(
                   { original }, { first }),
               "deleting a split section must be treated as changed timeline audio");
}
}

bool runAraTimelineAudioTests()
{
    return testSeparatedClipsBecomeOneSilentGapTimeline()
        && testEmptyTrackTransitions()
        && testSplitPreservesRenderedTimeline();
}
