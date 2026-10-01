#include "../Source/RegionAutomation.h"
#include "../Source/AudioEngine.h"
#include "../Source/ProjectSerializer.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace
{
bool check(bool condition, const char* label)
{
    if (!condition) std::cerr << "FAILED automation: " << label << std::endl;
    return condition;
}

juce::AudioBuffer<float> makeSignal(int size)
{
    juce::AudioBuffer<float> signal(2, size);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < size; ++i)
            signal.setSample(c, i, 0.15f * static_cast<float>(
                std::sin(i * 0.14 + c * 0.2) + 0.25 * std::sin(i * 1.2)));
    return signal;
}

bool testRangeProcessing()
{
    constexpr int size = 16000;
    constexpr double rate = 8000;
    AudioAutomationRegion r;
    r.id = "region-test";
    r.startSeconds = 0.25;
    r.endSeconds = 1.5;
    r.gainDb = 6.0f;
    r.fadeSeconds = 0.03;
    auto original = makeSignal(size);
    auto output = makeSignal(size);
    RegionAutomationProcessor processor;
    if (!check(processor.prepare(rate, 2, r), "prepare")) return false;
    processor.process(output, 0, size, 0);
    const float multiplier = juce::Decibels::decibelsToGain(6.0f);
    bool ok = true;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < size; ++i)
        {
            if (i < 2000 || i >= 12000)
                ok = output.getSample(c, i) == original.getSample(c, i) && ok;
            if (i >= 2240 && i < 11760)
                ok = std::abs(output.getSample(c, i) - original.getSample(c, i) * multiplier)
                     < 1.0e-6f && ok;
        }
    if (!check(ok, "gain plateau and exact unchanged samples outside region")) return false;
    if (!check(output.getSample(0, 2000) == original.getSample(0, 2000)
                && output.getSample(0, 11999) == original.getSample(0, 11999),
               "both region boundaries start at unity")) return false;

    r.brightness = 0.7f;
    r.ambience = 0.6f;
    auto whole = makeSignal(size);
    auto blocks = makeSignal(size);
    RegionAutomationProcessor a, b;
    a.prepare(rate, 2, r);
    b.prepare(rate, 2, r);
    a.process(whole, 0, size, 0);
    for (int i = 0; i < size; i += 137)
        b.process(blocks, i, std::min(137, size - i), i);
    bool changed = false;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < size; ++i)
        {
            ok = whole.getSample(c, i) == blocks.getSample(c, i) && ok;
            if (i < 2000 || i >= 12000)
                ok = whole.getSample(c, i) == original.getSample(c, i) && ok;
            else if (std::abs(whole.getSample(c, i) - output.getSample(c, i)) > 0.001f)
                changed = true;
        }
    if (!check(ok && changed, "brightness/ambience change sound; block sizes match exactly")) return false;
    auto repeated = makeSignal(size);
    b.process(repeated, 0, size, 0);
    for (int i = 0; i < size; ++i)
        ok = whole.getSample(0, i) == repeated.getSample(0, i) && ok;
    if (!check(ok, "seek clears echo/filter history")) return false;
    r.enabled = false;
    a.prepare(rate, 2, r);
    auto bypass = makeSignal(size);
    a.process(bypass, 0, size, 0);
    for (int i = 0; i < size; ++i)
        ok = bypass.getSample(0, i) == original.getSample(0, i) && ok;
    return check(ok, "disabled region is exact bypass");
}

bool testValidationAndPersistence()
{
    AudioAutomationRegion r;
    r.id = "saved-region";
    r.startSeconds = 0.2;
    r.endSeconds = 0.8;
    r.gainDb = -3.0f;
    r.brightness = -0.25f;
    r.ambience = 0.5f;
    AudioAutomationRegion restored;
    auto invalid = r;
    invalid.gainDb = std::numeric_limits<float>::quiet_NaN();
    if (!check(!RegionAutomation::isValid(invalid), "NaN rejected")) return false;
    invalid = r;
    invalid.endSeconds = r.startSeconds;
    if (!check(!RegionAutomation::isValid(invalid), "empty range rejected")) return false;
    invalid = r;
    invalid.ambience = 1.1f;
    if (!check(!RegionAutomation::isValid(invalid), "out-of-range amount rejected")) return false;
    auto encoded = juce::JSON::parse(juce::JSON::toString(RegionAutomation::toVar(r)));
    if (!check(RegionAutomation::fromVar(encoded, restored) && restored == r,
               "JSON roundtrip")) return false;

    AudioEngine engine;
    const int index = engine.addTrack("Automation test");
    if (!check(engine.setTrackAutomationRegions(index, {r}), "engine setter")) return false;
    engine.undo();
    if (!check(engine.getTracksSnapshot()[static_cast<size_t>(index)].automationRegions.empty(),
               "undo restores previous regions")) return false;
    engine.redo();
    if (!check(engine.getTracksSnapshot()[static_cast<size_t>(index)].automationRegions
                    == std::vector<AudioAutomationRegion>{r}, "redo restores settings")) return false;
    if (!check(!engine.setTrackAutomationRegions(index, {r, r}), "duplicate ids rejected")) return false;
    if (!check(!engine.setTrackAutomationRegions(index, {invalid}), "invalid engine edit rejected")) return false;
    juce::TemporaryFile file(".srec");
    if (!check(ProjectSerializer::saveProject(file.getFile(), engine), "save project")) return false;
    ProjectSerializer::LoadedProjectData data;
    juce::String error;
    if (!check(ProjectSerializer::loadProject(file.getFile(), data, &error), "load project")) return false;
    return check(data.tracks.size() == 1
                 && data.tracks[0].automationRegions == std::vector<AudioAutomationRegion>{r},
                 "project file preserves every automation field");
}

bool testPlaybackIntegration()
{
    constexpr int size = 16000;
    AudioAutomationRegion r;
    r.id = "playback-region";
    r.startSeconds = 0.25;
    r.endSeconds = 1.5;
    r.gainDb = 3.0f;
    r.brightness = 0.4f;
    r.ambience = 0.3f;
    TrackData track;
    track.id = "playback-test";
    AudioClip clip;
    clip.id = "original-audio";
    clip.sampleRate = 8000;
    clip.duration = 2.0;
    clip.buffer = std::make_shared<juce::AudioBuffer<float>>(makeSignal(size));
    track.clips.push_back(clip);
    AudioEngine engine;
    engine.prepare(137, 8000);
    engine.replaceTracks({track});
    engine.setMasterVolume(1.0f);
    if (!check(engine.setTrackAutomationRegions(0, {r}), "playback setup")) return false;
    auto expected = makeSignal(size);
    RegionAutomationProcessor offline;
    offline.prepare(8000, 2, r);
    offline.process(expected, 0, size, 0);
    engine.play();
    juce::AudioBuffer<float> block(2, 137);
    const float panGain = std::sqrt(0.5f);
    bool ok = true;
    for (int i = 0; i < size; i += 137)
    {
        const int count = std::min(137, size - i);
        juce::AudioSourceChannelInfo info(&block, 0, count);
        engine.processBlock(info, nullptr, 0);
        for (int ch = 0; ch < 2; ++ch)
            for (int j = 0; j < count; ++j)
                ok = std::abs(block.getSample(ch, j)
                             - expected.getSample(ch, i + j) * panGain) < 1.0e-5f && ok;
    }
    engine.stop();
    engine.release();
    return check(ok, "real-time AudioEngine output equals offline region processing");
}

bool testDelayedTimelineAndSpeed()
{
    // Model two plugin paths: this track is delayed 83 samples, then gets
    // 41 samples of cross-track alignment. Export aligns before automation.
    constexpr int size = 10000, ownLatency = 83, extraLatency = 41;
    constexpr int totalLatency = ownLatency + extraLatency;
    AudioAutomationRegion r;
    r.id = "delayed-region";
    r.startSeconds = 0.25;
    r.endSeconds = 0.9;
    r.gainDb = 4.0f;
    r.brightness = 0.5f;
    r.ambience = 0.4f;
    auto source = makeSignal(size);
    juce::AudioBuffer<float> realtime(2, size), offline(2, size);
    realtime.clear();
    offline.clear();
    for (int ch = 0; ch < 2; ++ch)
    {
        realtime.copyFrom(ch, ownLatency, source, ch, 0, size - ownLatency);
        offline.copyFrom(ch, totalLatency, source, ch, 0, size - totalLatency);
    }
    RegionAutomationProcessor a, b;
    a.prepare(8000, 2, r);
    b.prepare(8000, 2, r);
    for (int offset = 0; offset < size; offset += 137)
        a.process(realtime, offset, std::min(137, size - offset), offset - ownLatency);
    b.process(offline, 0, size, -totalLatency);
    bool ok = true;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = totalLatency; i < size; ++i)
            ok = realtime.getSample(ch, i - extraLatency) == offline.getSample(ch, i) && ok;
    if (!check(ok, "plugin and cross-track delays preserve region timing in export")) return false;

    TrackData track;
    track.id = "speed-region-test";
    track.playbackSpeed = 1.25;
    AudioClip clip;
    clip.id = "speed-original";
    clip.sampleRate = 8000;
    clip.duration = 2.0;
    clip.buffer = std::make_shared<juce::AudioBuffer<float>>(makeSignal(16000));
    track.clips.push_back(clip);
    AudioEngine dry, wet;
    // Loading first exercises initial sync at the default device sample rate,
    // changing to 8 kHz exercises resync, and a second prepare clears history.
    dry.replaceTracks({track});
    wet.replaceTracks({track});
    r.brightness = 0.0f;
    r.ambience = 0.0f;
    wet.setTrackAutomationRegions(0, {r});
    for (auto* engine : { &dry, &wet })
    {
        engine->prepare(137, 8000);
        engine->prepare(137, 8000);
        engine->setMasterVolume(1.0f);
        engine->play();
    }
    juce::AudioBuffer<float> dryBlock(2, 137), wetBlock(2, 137);
    constexpr int samples = 12800;
    for (int i = 0; i < samples; i += 137)
    {
        const int count = std::min(137, samples - i);
        juce::AudioSourceChannelInfo dryInfo(&dryBlock, 0, count), wetInfo(&wetBlock, 0, count);
        dry.processBlock(dryInfo, nullptr, 0);
        wet.processBlock(wetInfo, nullptr, 0);
        for (int j = 0; j < count; ++j)
        {
            const int pos = i + j;
            float envelope = 0.0f;
            if (pos >= 2000 && pos < 7200)
                envelope = static_cast<float>(std::clamp(
                    std::min(pos - 2000, 7199 - pos) / 240.0, 0.0, 1.0));
            const float gain = juce::Decibels::decibelsToGain(r.gainDb * envelope);
            for (int ch = 0; ch < 2; ++ch)
                ok = std::abs(wetBlock.getSample(ch, j) - dryBlock.getSample(ch, j) * gain)
                    < 1.0e-5f && ok;
        }
    }
    dry.stop(); wet.stop();
    dry.release(); wet.release();
    return check(ok, "1.25x playback keeps region positions in project seconds after reprepare");
}
}

bool runRegionAutomationTests()
{
    return testRangeProcessing() && testValidationAndPersistence() && testPlaybackIntegration()
        && testDelayedTimelineAndSpeed();
}
