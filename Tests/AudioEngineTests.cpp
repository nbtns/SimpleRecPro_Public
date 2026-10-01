#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "../Source/ProjectSerializer.h"
#include "../Source/RhythmWarp.h"

#include <cmath>
#include <cstring>
#include <iostream>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

bool runLatencyCalibrationTests();
bool runAraTimelineAudioTests();
bool runPitchNetArchiveFilterTests();
bool runDawFeatureTests();
bool runRegionAutomationTests();
bool runAssistantControllerTests();
int runAssistantIntegrationHost(const juce::File& directory);
bool runUiLayoutTests(const juce::File& snapshotDirectory);
int inspectPitchNetProjectArchive(const juce::File& projectFile);

namespace
{
bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAILED: " << message << std::endl;
    return condition;
}

std::shared_ptr<const juce::MemoryBlock> makePluginData(const juce::String& text)
{
    return std::make_shared<const juce::MemoryBlock>(text.toRawUTF8(),
                                                     text.getNumBytesAsUTF8());
}

bool pluginDataEquals(const std::shared_ptr<const juce::MemoryBlock>& data,
                      const juce::String& expected)
{
    return data != nullptr
        && data->getSize() == static_cast<size_t>(expected.getNumBytesAsUTF8())
        && std::memcmp(data->getData(),
                       expected.toRawUTF8(),
                       data->getSize()) == 0;
}

bool writeProjectFixture(const juce::File& file,
                         const char* signature,
                         const juce::String& json,
                         const std::vector<float>& audioSamples = {})
{
    const juce::MemoryBlock jsonBytes(json.toRawUTF8(),
                                      json.getNumBytesAsUTF8());
    const int paddingLength = (4 - static_cast<int>(jsonBytes.getSize() % 4)) % 4;
    juce::FileOutputStream out(file);
    if (!out.openedOk())
        return false;

    out.write(signature, 8);
    out.writeInt(static_cast<int>(jsonBytes.getSize()));
    out.write(jsonBytes.getData(), jsonBytes.getSize());
    for (int index = 0; index < paddingLength; ++index)
        out.writeByte(0);
    for (const float sample : audioSamples)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &sample, sizeof(bits));
        out.writeInt(juce::ByteOrder::swapIfBigEndian(bits));
    }
    out.flush();
    return out.getStatus().wasOk();
}

void processStereoBlock(AudioEngine& engine,
                        juce::AudioBuffer<float>& input,
                        juce::AudioBuffer<float>& output)
{
    const float* channels[] = { input.getReadPointer(0), input.getReadPointer(1) };
    juce::AudioSourceChannelInfo info(&output, 0, output.getNumSamples());
    engine.processBlock(info, channels, 2);
}

bool testRoundTripLatencyAndStereo()
{
    constexpr double sampleRate = 48000.0;
    constexpr int inputLatency = 480;
    constexpr int outputLatency = 240;
    constexpr int manualOffset = 48;
    constexpr int recordedSamples = 1000;

    AudioEngine engine;
    engine.prepare(512, sampleRate);
    engine.setDeviceLatencySamples(inputLatency, outputLatency);
    engine.setManualRecordingOffsetSamples(manualOffset);
    const int trackIndex = engine.addTrack("Stereo");
    engine.setCurrentTime(0.1);
    engine.record(trackIndex);

    juce::AudioBuffer<float> input(2, recordedSamples);
    juce::AudioBuffer<float> output(2, recordedSamples);
    input.clear();
    for (int i = 0; i < recordedSamples; ++i)
    {
        input.setSample(0, i, 0.25f);
        input.setSample(1, i, -0.5f);
    }

    processStereoBlock(engine, input, output);
    engine.stop();

    const auto tracks = engine.getTracksSnapshot();
    if (!expect(tracks.size() == 1 && tracks[0].clips.size() == 1,
                "stereo recording should create one clip"))
        return false;

    const auto& clip = tracks[0].clips[0];
    const double expectedStart = (0.1 * sampleRate
                                  - inputLatency - outputLatency - manualOffset) / sampleRate;
    return expect(engine.getEffectiveRecordingLatencySamples() == 768,
                  "automatic and manual latency should be combined")
           && expect(std::abs(clip.startTime - expectedStart) < 1.0e-9,
                     "recorded clip should move earlier by round-trip latency")
           && expect(clip.buffer != nullptr && clip.buffer->getNumChannels() == 2,
                     "recording should preserve stereo channels")
           && expect(clip.buffer->getNumSamples() == recordedSamples,
                     "latency correction must not shorten a non-negative take")
           && expect(std::abs(clip.buffer->getSample(0, 10) - 0.25f) < 1.0e-6f,
                     "left channel should be preserved")
           && expect(std::abs(clip.buffer->getSample(1, 10) + 0.5f) < 1.0e-6f,
                     "right channel should be preserved");
}

bool testMeasuredLatencyOverridesDeviceEstimate()
{
    AudioEngine engine;
    engine.prepare(512, 48000.0);
    engine.setDeviceLatencySamples(480, 240);
    engine.setManualRecordingOffsetSamples(48);

    engine.setCalibratedRecordingLatencySamples(900);
    const bool measuredValueUsed = expect(engine.hasCalibratedRecordingLatency(),
                                          "measured latency should be marked as available")
        && expect(engine.getAutomaticRecordingLatencySamples() == 900,
                  "measured round-trip latency should replace the device estimate")
        && expect(engine.getEffectiveRecordingLatencySamples() == 948,
                  "manual fine tuning should remain additive after calibration");

    engine.clearCalibratedRecordingLatency();
    return measuredValueUsed
        && expect(!engine.hasCalibratedRecordingLatency(),
                  "clearing calibration should restore the estimated mode")
        && expect(engine.getEffectiveRecordingLatencySamples() == 768,
                  "device-reported input and output latency should remain the fallback");
}

bool testInputMeterUpdatesDuringCalibrationBypass()
{
    AudioEngine engine;
    engine.prepare(4, 48000.0);

    const float inputSamples[] { 0.1f, -0.5f, 0.2f, -0.3f };
    const float* inputChannels[] { inputSamples };
    engine.updateInputMeter(inputChannels, 1, 4);

    return expect(std::abs(engine.getCurrentInputLevel() - 0.5f) < 1.0e-6f,
                  "the input meter should update when calibration bypasses normal processing");
}

bool testLatencyTrimAtTimelineStart()
{
    constexpr double sampleRate = 48000.0;
    constexpr int latencySamples = 768;
    constexpr int recordedSamples = 1000;

    AudioEngine engine;
    engine.prepare(512, sampleRate);
    engine.setDeviceLatencySamples(latencySamples, 0);
    const int trackIndex = engine.addTrack("Start");
    engine.setCurrentTime(0.0);
    engine.record(trackIndex);

    juce::AudioBuffer<float> input(2, recordedSamples);
    juce::AudioBuffer<float> output(2, recordedSamples);
    input.clear();
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < recordedSamples; ++i)
            input.setSample(ch, i, 0.4f);

    processStereoBlock(engine, input, output);
    engine.stop();

    const auto tracks = engine.getTracksSnapshot();
    if (!expect(tracks.size() == 1 && tracks[0].clips.size() == 1,
                "timeline-start recording should keep its audible remainder"))
        return false;

    const auto& clip = tracks[0].clips[0];
    return expect(std::abs(clip.startTime) < 1.0e-12,
                  "corrected clip must not start before zero")
           && expect(std::abs(clip.offset - latencySamples / sampleRate) < 1.0e-9,
                     "negative corrected time should become a source offset")
           && expect(std::abs(clip.duration
                              - (recordedSamples - latencySamples) / sampleRate) < 1.0e-9,
                     "trimmed duration should match the remaining samples");
}

bool testStereoAfterInitialMissingInput()
{
    AudioEngine engine;
    engine.prepare(4, 48000.0);
    const int trackIndex = engine.addTrack("Late stereo");
    engine.record(trackIndex);

    juce::AudioBuffer<float> output(2, 4);
    juce::AudioSourceChannelInfo outputInfo(&output, 0, 4);
    engine.processBlock(outputInfo, nullptr, 0);

    juce::AudioBuffer<float> input(2, 4);
    for (int i = 0; i < 4; ++i)
    {
        input.setSample(0, i, 0.25f);
        input.setSample(1, i, -0.5f);
    }
    processStereoBlock(engine, input, output);
    engine.stop();

    const auto tracks = engine.getTracksSnapshot();
    if (!expect(tracks.size() == 1 && tracks[0].clips.size() == 1,
                "late stereo input should still create one clip"))
        return false;

    const auto& buffer = *tracks[0].clips[0].buffer;
    return expect(buffer.getNumChannels() == 2 && buffer.getNumSamples() == 8,
                  "late stereo input must keep aligned left and right channels")
           && expect(std::abs(buffer.getSample(1, 0)) < 1.0e-9f,
                     "missing right-channel history should be backfilled with silence")
           && expect(std::abs(buffer.getSample(1, 6) + 0.5f) < 1.0e-6f,
                     "right-channel samples should be preserved after input appears");
}

bool testSampleRateConversion()
{
    AudioEngine engine;
    engine.prepare(512, 48000.0);

    TrackData track;
    track.id = "rate-track";
    track.name = "44.1 kHz";

    AudioClip clip;
    clip.id = "rate-clip";
    clip.sampleRate = 44100;
    clip.duration = 1.0;
    clip.buffer = std::make_shared<juce::AudioBuffer<float>>(2, 44100);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 44100; ++i)
            clip.buffer->setSample(ch, i, static_cast<float>(i) / 44100.0f);
    track.clips.push_back(std::move(clip));

    std::vector<TrackData> tracks;
    tracks.push_back(std::move(track));
    if (!expect(AudioEngine::prepareTracksForSampleRate(tracks, 48000.0),
                "worker-side project preparation should convert mismatched audio"))
        return false;
    const auto preparedBuffer = tracks[0].clips[0].buffer;
    engine.replaceTracks(std::move(tracks));

    const auto converted = engine.getTracksSnapshot();
    const auto& convertedClip = converted[0].clips[0];
    return expect(convertedClip.sampleRate == 48000,
                  "clip sample rate should follow the active device")
           && expect(convertedClip.buffer->getNumSamples() == 48000,
                      "one second at 44.1 kHz should become 48000 samples")
           && expect(convertedClip.buffer == preparedBuffer,
                     "applying prepared tracks should not resample or copy them again")
           && expect(std::abs(convertedClip.duration - 1.0) < 1.0e-12,
                     "sample-rate conversion must preserve timeline duration");
}

bool testDeviceRateSwitchDoesNotResampleAgain()
{
    AudioEngine engine;
    engine.prepare(512, 48000.0);

    TrackData track;
    AudioClip clip;
    clip.sampleRate = 44100;
    clip.duration = 1.0;
    clip.buffer = std::make_shared<juce::AudioBuffer<float>>(1, 44100);
    clip.buffer->clear();
    track.clips.push_back(std::move(clip));
    engine.replaceTracks({ std::move(track) });

    const auto convertedOnce = engine.getTracksSnapshot();
    if (!expect(convertedOnce[0].clips[0].sampleRate == 48000
                    && convertedOnce[0].clips[0].buffer->getNumSamples() == 48000,
                "initial project load should convert to the active rate"))
        return false;

    engine.prepare(512, 44100.0);
    engine.prepare(512, 48000.0);
    const auto afterSwitches = engine.getTracksSnapshot();
    return expect(afterSwitches[0].clips[0].sampleRate == 48000
                      && afterSwitches[0].clips[0].buffer->getNumSamples() == 48000,
                  "device-rate switches must not repeatedly rewrite clip samples");
}

bool testCachedDurationAndContentRevision()
{
    AudioEngine engine;
    engine.prepare(512, 48000.0);

    TrackData track;
    AudioClip clip;
    clip.id = "duration-clip";
    clip.startTime = 2.0;
    clip.duration = 1.0;
    clip.sampleRate = 48000;
    clip.buffer = std::make_shared<juce::AudioBuffer<float>>(1, 48000);
    track.clips.push_back(std::move(clip));
    engine.replaceTracks({ std::move(track) });

    const auto revisionAfterLoad = engine.getContentRevision();
    engine.markDirty();
    const bool initialStateOk = expect(std::abs(engine.getDuration() - 3.0) < 1.0e-12,
                                       "cached duration should match loaded clips")
                                && expect(engine.getContentRevision() > revisionAfterLoad,
                                          "content revision must advance while already dirty");

    engine.moveClip("duration-clip", 0, 0.5);
    const bool movedStateOk = expect(std::abs(engine.getDuration() - 1.5) < 1.0e-12,
                                     "cached duration should follow clip moves");
    engine.removeClip("duration-clip");
    return initialStateOk && movedStateOk
           && expect(std::abs(engine.getDuration()) < 1.0e-12,
                     "cached duration should shrink after clip removal");
}

bool testProjectRoundTrip()
{
    AudioEngine source;
    source.prepare(512, 48000.0);
    source.getMetronome().setBpm(137);
    source.getMetronome().setTimeSignature(7, 8);
    source.getMetronome().setEnabled(true);
    source.getMetronome().setVolume(0.33f);
    source.setCountInEnabled(true);
    source.setMasterVolume(0.65f);
    source.setZoomLevel(80.0);

    TrackData track;
    track.id = "saved-track";
    track.name = "Saved Stereo";
    track.role = TrackRole::mainVocal;
    track.color = juce::Colours::cyan;
    track.volume = 0.72f;
    track.pan = -0.35f;
    track.pitchSemitones = -4.0;
    track.playbackSpeed = 1.25;
    track.isMuted = true;
    track.isArmed = true;
    track.pitchCorrection.enabled = true;
    track.pitchCorrection.auditionCorrected = false;
    track.pitchCorrection.key = 9;
    track.pitchCorrection.scale = MusicalScale::harmonicMinor;
    track.pitchCorrection.strength = 0.82f;
    track.simpleMix.enabled = true;
    track.simpleMix.preset = MixPreset::radio;
    track.simpleMix.brightness = 0.41f;
    track.simpleMix.ambience = 0.27f;
    track.simpleMix.stability = 0.73f;
    track.noiseReduction.enabled = true;
    track.noiseReduction.amount = 0.46f;
    track.noiseReduction.deEssAmount = 0.19f;
    SpecialFxRegion specialFx;
    specialFx.id = "saved-special-fx";
    specialFx.type = SpecialFxType::radio;
    specialFx.startSeconds = 0.001;
    specialFx.endSeconds = 0.008;
    specialFx.amount = 0.74f;
    specialFx.wet = 0.63f;
    specialFx.fadeSeconds = 0.015;
    specialFx.enabled = true;
    track.specialFxRegions.push_back(specialFx);
    // Metadata-only fixture: verifies project persistence without loading a
    // machine-specific third-party plugin during deterministic tests.
    track.vst3Name = "Saved Effect";
    track.vst3State = makePluginData("saved-state");
    track.vst3AraArchive = makePluginData("ara-archive");
    track.vst3AraArchiveId = "com.example.saved-ara-archive.1";
    track.vst3AraHostFormatVersion = TrackData::currentVst3AraHostFormatVersion;
    track.vst3AraPlaybackEnabled = true;
    track.vst3Bypassed = true;
    track.vst3TailSeconds = 1.25;

    EffectSlotData effectSlot;
    effectSlot.id = "saved-effect-slot";
    effectSlot.name = "Saved Slot Effect";
    effectSlot.descriptionXml = "<PLUGIN name=\"Saved Slot Effect\"/>";
    effectSlot.state = makePluginData("saved-slot-state");
    effectSlot.bypassed = true;
    effectSlot.latencySamples = 384;
    effectSlot.tailSeconds = 0.4;
    track.effectSlots.push_back(effectSlot);
    EffectSlotData secondEffectSlot;
    secondEffectSlot.id = "saved-effect-slot-2";
    secondEffectSlot.name = "Second Saved Effect";
    secondEffectSlot.descriptionXml = "<PLUGIN name=\"Second Saved Effect\"/>";
    secondEffectSlot.state = makePluginData("second-slot-state");
    secondEffectSlot.bypassed = false;
    secondEffectSlot.latencySamples = 128;
    secondEffectSlot.tailSeconds = 0.2;
    track.effectSlots.push_back(secondEffectSlot);
    track.effectChainBypassed = true;

    AudioClip clip;
    clip.id = "saved-clip";
    clip.name = "Stereo clip";
    clip.sampleRate = 48000;
    clip.duration = 0.01;
    clip.fadeInSeconds = 0.002;
    clip.fadeOutSeconds = 0.003;
    clip.rhythmMarkers = {
        { 0.001, 0.0015, 0.0030, 0.0038 },
        { 0.006, 0.0070, 0.0080, 0.0090 }
    };
    clip.buffer = std::make_shared<juce::AudioBuffer<float>>(2, 480);
    for (int i = 0; i < 480; ++i)
    {
        clip.buffer->setSample(0, i, 0.2f);
        clip.buffer->setSample(1, i, -0.3f);
    }
    track.clips.push_back(std::move(clip));
    source.replaceTracks({ std::move(track) });

    ProjectDawSettings projectSettings;
    projectSettings.musicalKey = 9;
    projectSettings.musicalScale = MusicalScale::harmonicMinor;
    projectSettings.pitchCorrectionStrength = 0.77f;
    projectSettings.hasLoopStart = true;
    projectSettings.hasLoopEnd = true;
    projectSettings.loopStartSeconds = 0.002;
    projectSettings.loopEndSeconds = 0.009;
    projectSettings.loopEnabled = true;
    projectSettings.tempoMap = {
        { 0.0, 137.0, 7, 8 },
        { 0.005, 90.0, 3, 4 }
    };
    projectSettings.exportDefaults.format = ExportFormat::mp3;
    projectSettings.exportDefaults.channels = ExportChannels::mono;
    projectSettings.exportDefaults.wavBits = 24;
    projectSettings.exportDefaults.mp3Kbps = 192;
    projectSettings.exportDefaults.sampleRate = 48000;
    projectSettings.exportDefaults.target = ExportTarget::abRange;
    projectSettings.exportDefaults.trackIndex = 0;
    projectSettings.exportDefaults.rangeStartSeconds = 0.002;
    projectSettings.exportDefaults.rangeEndSeconds = 0.008;
    projectSettings.mastering.enabled = true;
    projectSettings.mastering.limiterEnabled = false;
    projectSettings.mastering.ceilingDb = -1.5f;
    projectSettings.mastering.targetLufs = -12.5f;
    projectSettings.mastering.auditionProcessed = false;
    source.setProjectDawSettings(projectSettings, false);

    const auto sourceTracks = source.getTracksSnapshot();
    const auto snapshot = ProjectSerializer::captureProjectSnapshot(source);
    if (!expect(snapshot.tracks.size() == 1
                    && snapshot.tracks[0].vst3State == sourceTracks[0].vst3State
                    && snapshot.tracks[0].vst3AraArchive
                           == sourceTracks[0].vst3AraArchive
                    && snapshot.tracks[0].effectSlots.size() == 2
                    && snapshot.tracks[0].effectSlots[0].state
                           == sourceTracks[0].effectSlots[0].state
                    && snapshot.tracks[0].effectSlots[1].state
                           == sourceTracks[0].effectSlots[1].state,
                "project snapshots should share immutable plug-in payloads"))
        return false;

    juce::TemporaryFile projectFile(".srec");
    if (!expect(ProjectSerializer::saveProject(projectFile.getFile(), snapshot),
                "project should save through an atomic temporary file"))
        return false;

    {
        juce::FileInputStream input(projectFile.getFile());
        char signature[8] {};
        if (!expect(input.openedOk() && input.read(signature, sizeof(signature)) == 8
                        && std::memcmp(signature, "SREC_V4\0", 8) == 0,
                    "new projects should use the V4 binary-payload format"))
            return false;
        const int jsonLength = input.readInt();
        juce::MemoryBlock jsonBytes(static_cast<size_t>(std::max(0, jsonLength)), false);
        if (!expect(jsonLength > 0
                        && input.read(jsonBytes.getData(), jsonBytes.getSize()) == jsonLength,
                    "V4 metadata should be readable"))
            return false;
        const juce::String json = juce::String::fromUTF8(
            static_cast<const char*>(jsonBytes.getData()), jsonLength);
        if (!expect(json.contains("\"vst3State\"")
                        && !json.contains("c2F2ZWQtc3RhdGU="),
                    "V4 JSON should reference binary plug-in state instead of Base64"))
            return false;
    }

    AudioEngine restored;
    restored.prepare(512, 44100.0);
    if (!expect(ProjectSerializer::loadProject(projectFile.getFile(), restored),
                "saved project should load"))
        return false;

    const auto tracks = restored.getTracksSnapshot();
    if (!expect(tracks.size() == 1 && tracks[0].clips.size() == 1,
                "round-trip should preserve track and clip counts"))
        return false;

    const auto& restoredTrack = tracks[0];
    const auto& restoredClip = restoredTrack.clips[0];
    const auto restoredProjectSettings = restored.getProjectDawSettings();
    return expect(restored.getMetronome().getBpm() == 137,
                  "round-trip should preserve BPM")
           && expect(restored.getMetronome().getNumerator() == 7
                         && restored.getMetronome().getDenominator() == 8,
                     "round-trip should preserve 7/8")
           && expect(restored.getMetronome().getEnabled()
                         && restored.isCountInEnabled(),
                     "round-trip should preserve click and count-in")
           && expect(std::abs(restored.getMasterVolume() - 0.65f) < 1.0e-6f,
                     "round-trip should preserve master volume")
           && expect(restoredTrack.isMuted && restoredTrack.isArmed,
                      "round-trip should preserve track switches")
           && expect(restoredTrack.role == TrackRole::mainVocal,
                     "round-trip should preserve the track role")
           && expect(std::abs(restoredTrack.pan + 0.35f) < 1.0e-6f,
                     "round-trip should preserve track pan")
           && expect(std::abs(restoredTrack.pitchSemitones + 4.0) < 1.0e-9
                          && std::abs(restoredTrack.playbackSpeed - 1.25) < 1.0e-9,
                      "round-trip should preserve per-track key and speed")
           && expect(restoredTrack.pitchCorrection == sourceTracks[0].pitchCorrection,
                     "round-trip should preserve pitch-correction settings")
           && expect(restoredTrack.simpleMix == sourceTracks[0].simpleMix,
                     "round-trip should preserve simple-mix settings")
           && expect(restoredTrack.noiseReduction == sourceTracks[0].noiseReduction,
                     "round-trip should preserve noise-reduction settings")
           && expect(restoredTrack.specialFxRegions
                         == sourceTracks[0].specialFxRegions,
                     "round-trip should preserve special-FX regions")
           && expect(restoredTrack.effectSlots == sourceTracks[0].effectSlots,
                     "round-trip should preserve ordered effect slots and state")
           && expect(restoredTrack.effectChainBypassed
                         == sourceTracks[0].effectChainBypassed,
                     "round-trip should preserve effect-chain bypass")
           && expect(restoredTrack.vst3Name == "Saved Effect"
                           && pluginDataEquals(restoredTrack.vst3State, "saved-state")
                           && pluginDataEquals(restoredTrack.vst3AraArchive, "ara-archive")
                          && restoredTrack.vst3AraArchiveId
                                 == "com.example.saved-ara-archive.1"
                          && restoredTrack.vst3AraHostFormatVersion
                                 == TrackData::currentVst3AraHostFormatVersion
                          && restoredTrack.vst3AraPlaybackEnabled
                          && restoredTrack.vst3Bypassed
                          && std::abs(restoredTrack.vst3TailSeconds - 1.25) < 1.0e-9,
                      "round-trip should preserve VST3 and ARA metadata and state")
           && expect(restoredClip.buffer->getNumChannels() == 2,
                     "round-trip should preserve stereo")
           && expect(restoredClip.sampleRate == 44100
                          && restoredClip.buffer->getNumSamples() == 441,
                      "load should convert saved audio to the active sample rate")
           && expect(std::abs(restoredClip.duration - 0.01) < 1.0e-9,
                     "load conversion should preserve clip duration")
           && expect(std::abs(restoredClip.fadeInSeconds - 0.002) < 1.0e-9
                          && std::abs(restoredClip.fadeOutSeconds - 0.003) < 1.0e-9,
                     "round-trip should preserve clip fades")
           && expect(restoredClip.rhythmMarkers
                          == sourceTracks[0].clips[0].rhythmMarkers,
                     "round-trip should preserve clip rhythm markers")
           && expect(restoredProjectSettings.musicalKey
                          == projectSettings.musicalKey
                          && restoredProjectSettings.musicalScale
                                 == projectSettings.musicalScale
                          && std::abs(restoredProjectSettings.pitchCorrectionStrength
                                      - projectSettings.pitchCorrectionStrength)
                                 < 1.0e-6f,
                     "round-trip should preserve project key and scale")
           && expect(restoredProjectSettings.hasLoopStart
                          == projectSettings.hasLoopStart
                          && restoredProjectSettings.hasLoopEnd
                                 == projectSettings.hasLoopEnd
                          && restoredProjectSettings.loopEnabled
                                 == projectSettings.loopEnabled
                          && std::abs(restoredProjectSettings.loopStartSeconds
                                      - projectSettings.loopStartSeconds) < 1.0e-9
                          && std::abs(restoredProjectSettings.loopEndSeconds
                                      - projectSettings.loopEndSeconds) < 1.0e-9,
                     "round-trip should preserve A/B loop points")
           && expect(restoredProjectSettings.tempoMap == projectSettings.tempoMap,
                     "round-trip should preserve the tempo map")
           && expect(restoredProjectSettings.exportDefaults
                          == projectSettings.exportDefaults,
                     "round-trip should preserve all export defaults")
           && expect(restoredProjectSettings.mastering == projectSettings.mastering,
                     "round-trip should preserve mastering settings");
}

bool testLegacyProjectDefaultsAraMetadata()
{
    juce::MemoryBlock legacyState("legacy-state", 12);
    juce::String legacyJson = R"json({
        "version": 3,
        "project": {},
        "tracks": [{
            "id": "legacy-track",
            "name": "Legacy",
            "role": 1,
            "vst3Name": "Legacy Effect",
            "vst3StateBase64": "__LEGACY_STATE__",
            "clips": [{
                "id": "legacy-clip",
                "name": "Legacy audio",
                "startTime": 0.0,
                "offset": 0.0,
                "duration": 0.00008333333333333333,
                "audio": {
                    "sampleRate": 48000,
                    "numberOfChannels": 1,
                    "length": 4,
                    "byteOffset": 0,
                    "byteLength": 16
                }
            }]
        }]
    })json";
    legacyJson = legacyJson.replace("__LEGACY_STATE__",
                                    legacyState.toBase64Encoding());
    const juce::MemoryBlock jsonBytes(legacyJson.toRawUTF8(),
                                      legacyJson.getNumBytesAsUTF8());
    const int paddingLength = (4 - static_cast<int>(jsonBytes.getSize() % 4)) % 4;

    juce::TemporaryFile projectFile(".srec");
    {
        juce::FileOutputStream out(projectFile.getFile());
        if (!expect(out.openedOk(), "legacy project fixture should open for writing"))
            return false;

        out.write("SREC_V3\0", 8);
        out.writeInt(static_cast<int>(jsonBytes.getSize()));
        out.write(jsonBytes.getData(), jsonBytes.getSize());
        for (int i = 0; i < paddingLength; ++i)
            out.writeByte(0);
        const float legacySamples[] { 0.25f, -0.5f, 0.75f, -1.0f };
        for (const float sample : legacySamples)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &sample, sizeof(bits));
            out.writeInt(juce::ByteOrder::swapIfBigEndian(bits));
        }
        out.flush();
        if (!expect(out.getStatus().wasOk(), "legacy project fixture should be written"))
            return false;
    }

    AudioEngine restored;
    restored.prepare(512, 48000.0);
    if (!expect(ProjectSerializer::loadProject(projectFile.getFile(), restored),
                "project saved before ARA metadata should still load"))
        return false;

    const auto tracks = restored.getTracksSnapshot();
    return expect(tracks.size() == 1 && tracks[0].clips.size() == 1,
                  "legacy project should preserve its track and audio")
           && expect(tracks[0].vst3Name.isEmpty()
                         && tracks[0].vst3State == nullptr
                         && tracks[0].effectSlots.size() == 1
                         && tracks[0].effectSlots[0].name == "Legacy Effect"
                         && pluginDataEquals(tracks[0].effectSlots[0].state,
                                             "legacy-state"),
                     "V3 single plug-in state should migrate into effect slot zero")
           && expect(tracks[0].vst3AraArchive == nullptr
                          && tracks[0].vst3AraArchiveId.isEmpty()
                          && tracks[0].vst3AraHostFormatVersion == 1
                          && !tracks[0].vst3AraPlaybackEnabled,
                      "missing ARA metadata should use backward-compatible defaults")
           && expect(tracks[0].role == TrackRole::unknown,
                     "projects before schema 7 should default to an unknown track role")
           && expect(std::abs(tracks[0].clips[0].buffer->getSample(0, 1) + 0.5f)
                          < 1.0e-6f,
                      "V3 raw audio should remain backward compatible");
}

bool testProjectSchemaAndRhythmMarkerValidation()
{
    juce::TemporaryFile futureProject(".srec");
    const auto futureVersion = ProjectSerializer::currentSchemaVersion + 1;
    const auto futureJson = juce::String(R"json({
        "version": __FUTURE_SCHEMA__,
        "schemaVersion": __FUTURE_SCHEMA__,
        "project": {},
        "tracks": []
    })json").replace("__FUTURE_SCHEMA__", juce::String(futureVersion));
    if (!expect(writeProjectFixture(futureProject.getFile(),
                                    "SREC_V4\0", futureJson),
                "future-schema fixture should be written"))
        return false;

    ProjectSerializer::LoadedProjectData futureLoaded;
    juce::String futureError;
    const bool futureRejected = expect(
        !ProjectSerializer::loadProject(futureProject.getFile(),
                                        futureLoaded, &futureError),
        "a project from a future schema must be rejected")
        && expect(futureError.contains("schema " + juce::String(futureVersion))
                      && futureError.contains("SimpleRec Pro"),
                  "future-schema rejection should explain how to recover");

    juce::TemporaryFile roleProject(".srec");
    const juce::String roleJson = R"json({
        "version": 7,
        "schemaVersion": 7,
        "project": {},
        "tracks": [
            { "id": "low-role", "role": -10, "clips": [] },
            { "id": "harmony-role", "role": 3, "clips": [] },
            { "id": "high-role", "role": 99, "clips": [] }
        ]
    })json";
    if (!expect(writeProjectFixture(roleProject.getFile(),
                                    "SREC_V4\0", roleJson),
                "track-role validation fixture should be written"))
        return false;

    ProjectSerializer::LoadedProjectData rolesLoaded;
    if (!expect(ProjectSerializer::loadProject(roleProject.getFile(), rolesLoaded),
                "schema 7 track roles should load"))
        return false;
    const bool rolesSanitised = expect(rolesLoaded.tracks.size() == 3,
        "track-role fixture should preserve all tracks")
        && expect(rolesLoaded.tracks[0].role == TrackRole::unknown,
                  "track roles below the supported range should become unknown")
        && expect(rolesLoaded.tracks[1].role == TrackRole::harmony,
                  "valid track roles should be preserved")
        && expect(rolesLoaded.tracks[2].role == TrackRole::other,
                  "track roles above the supported range should become other");

    const std::vector<float> audioSamples(48000, 0.1f);
    juce::TemporaryFile malformedProject(".srec");
    const juce::String malformedJson = R"json({
        "version": 6,
        "schemaVersion": 6,
        "project": {},
        "tracks": [{
            "id": "rhythm-track",
            "name": "Rhythm",
            "clips": [{
                "id": "rhythm-clip",
                "name": "Rhythm clip",
                "startTime": 0.0,
                "offset": 0.0,
                "duration": 6.0,
                "rhythmMarkers": [
                    { "sourceTime": 3.0, "targetTime": 4.0,
                      "sourceEndTime": 4.0, "targetEndTime": 5.5 },
                    { "sourceTime": 1.0, "targetTime": 2.0,
                      "sourceEndTime": 2.0, "targetEndTime": 4.5 },
                    { "sourceTime": 1.0002, "targetTime": 2.0002,
                      "sourceEndTime": 1.5, "targetEndTime": 2.5 },
                    { "sourceTime": 0.5, "targetTime": 3.0,
                      "sourceEndTime": 0.8, "targetEndTime": 3.5 },
                    { "sourceTime": 7.0, "targetTime": 7.0,
                      "sourceEndTime": 9.0, "targetEndTime": 9.0 }
                ],
                "audio": {
                    "sampleRate": 8000,
                    "numberOfChannels": 1,
                    "length": 48000,
                    "byteOffset": 0,
                    "byteLength": 192000
                }
            }]
        }]
    })json";
    if (!expect(writeProjectFixture(malformedProject.getFile(),
                                    "SREC_V4\0", malformedJson,
                                    audioSamples),
                "malformed-marker fixture should be written"))
        return false;

    ProjectSerializer::LoadedProjectData sanitisedLoaded;
    if (!expect(ProjectSerializer::loadProject(malformedProject.getFile(),
                                               sanitisedLoaded),
                "valid project data with malformed markers should load safely"))
        return false;
    if (!expect(sanitisedLoaded.tracks.size() == 1
                    && sanitisedLoaded.tracks[0].clips.size() == 1,
                "sanitised marker fixture should retain its audio clip"))
        return false;
    const auto& markers = sanitisedLoaded.tracks[0].clips[0].rhythmMarkers;
    const bool markersSanitised = expect(markers.size() == 2,
        "duplicate, out-of-order and out-of-range markers should be removed")
        && expect(std::abs(markers[0].sourceTime - 1.0) < 1.0e-9
                      && std::abs(markers[0].targetTime - 2.0) < 1.0e-9
                      && std::abs(markers[0].targetEndTime - 3.999) < 1.0e-9
                      && std::abs(markers[1].sourceTime - 3.0) < 1.0e-9,
                  "overlapping marker ends should be bounded by the next note");

    juce::TemporaryFile legacyPointProject(".srec");
    const juce::String legacyPointJson = R"json({
        "version": 4,
        "project": {},
        "tracks": [{
            "id": "legacy-rhythm-track",
            "name": "Legacy rhythm",
            "clips": [{
                "id": "legacy-rhythm-clip",
                "name": "Legacy point marker",
                "startTime": 0.0,
                "offset": 0.0,
                "duration": 6.0,
                "rhythmMarkers": [
                    { "sourceTime": 2.0, "targetTime": 1.0 }
                ],
                "audio": {
                    "sampleRate": 8000,
                    "numberOfChannels": 1,
                    "length": 48000,
                    "byteOffset": 0,
                    "byteLength": 192000
                }
            }]
        }]
    })json";
    if (!expect(writeProjectFixture(legacyPointProject.getFile(),
                                    "SREC_V4\0", legacyPointJson,
                                    audioSamples),
                "legacy point-marker fixture should be written"))
        return false;

    ProjectSerializer::LoadedProjectData legacyLoaded;
    if (!expect(ProjectSerializer::loadProject(legacyPointProject.getFile(),
                                               legacyLoaded),
                "a project without schemaVersion should remain compatible"))
        return false;
    const auto& legacyMarkers = legacyLoaded.tracks[0].clips[0].rhythmMarkers;
    const bool legacyMeaningPreserved = expect(
        legacyMarkers.size() == 1 && !legacyMarkers[0].hasDuration(),
        "old rhythm anchors must remain point markers rather than note ranges")
        && expect(std::abs(RhythmWarp::mapTargetToSource(
                              legacyMarkers, 6.0, 1.0) - 2.0) < 1.0e-9,
                  "old point-marker timing should retain its original mapping");

    return futureRejected && rolesSanitised
        && markersSanitised && legacyMeaningPreserved;
}

bool testNewAraPluginUsesCurrentTimelineFormat()
{
    return expect(
               TrackData::hostFormatVersionForPluginLoad(
                   true, false, 1)
                   == TrackData::currentVst3AraHostFormatVersion,
               "a new ARA plug-in must use the current timeline host format")
        && expect(
               TrackData::hostFormatVersionForPluginLoad(
                   true, true, 5) == 5,
               "a saved ARA archive must retain its legacy host format")
        && expect(
               TrackData::hostFormatVersionForPluginLoad(
                   false, false, 1) == 1,
               "a conventional VST3 must keep the neutral host format");
}

int runProjectFormatBenchmark(const juce::File& sourceFile,
                              const juce::File& v4File)
{
    ProjectSerializer::LoadedProjectData sourceProject;
    const double initialLoadStart = juce::Time::getMillisecondCounterHiRes();
    if (!ProjectSerializer::loadProject(sourceFile, sourceProject))
    {
        std::cerr << "Could not load benchmark source." << std::endl;
        return 2;
    }
    const double initialLoadMs = juce::Time::getMillisecondCounterHiRes()
                               - initialLoadStart;

    const double saveStart = juce::Time::getMillisecondCounterHiRes();
    if (!ProjectSerializer::saveProject(v4File, sourceProject))
    {
        std::cerr << "Could not save V4 benchmark project." << std::endl;
        return 3;
    }
    const double saveMs = juce::Time::getMillisecondCounterHiRes() - saveStart;

    auto measureLoad = [](const juce::File& file)
    {
        double total = 0.0;
        for (int iteration = 0; iteration < 3; ++iteration)
        {
            ProjectSerializer::LoadedProjectData loaded;
            const double start = juce::Time::getMillisecondCounterHiRes();
            if (!ProjectSerializer::loadProject(file, loaded))
                return -1.0;
            total += juce::Time::getMillisecondCounterHiRes() - start;
        }
        return total / 3.0;
    };

    const double v3WarmLoadMs = measureLoad(sourceFile);
    const double v4WarmLoadMs = measureLoad(v4File);
    std::cout << "source_bytes=" << sourceFile.getSize() << std::endl
              << "v4_bytes=" << v4File.getSize() << std::endl
              << "v3_initial_load_ms=" << initialLoadMs << std::endl
              << "v3_warm_load_ms=" << v3WarmLoadMs << std::endl
              << "v4_save_ms=" << saveMs << std::endl
              << "v4_warm_load_ms=" << v4WarmLoadMs << std::endl;
    return v3WarmLoadMs >= 0.0 && v4WarmLoadMs >= 0.0 ? 0 : 4;
}

bool testProjectReplacementClearsUndoHistory()
{
    AudioEngine engine;
    engine.prepare(512, 48000.0);
    const int index = engine.addTrack("Project A");
    engine.setTrackName(index, "Project A edited");

    TrackData replacement;
    replacement.id = "project-b";
    replacement.name = "Project B";
    engine.replaceTracks({ replacement });
    engine.undo();

    const auto tracks = engine.getTracksSnapshot();
    return expect(tracks.size() == 1 && tracks[0].name == "Project B",
                  "undo after project replacement must not restore the old project");
}

bool testPitchCorrectionRuntimeStateConsistency()
{
    AudioEngine engine;
    engine.prepare(512, 48000.0);

    TrackData plainTrack;
    plainTrack.id = "pitch-plain";
    plainTrack.name = "No PitchNet";
    engine.replaceTracks({ plainTrack });
    engine.setTrackPitchCorrectionAudition(0, true);
    bool ok = expect(!engine.getTracksSnapshot()[0].pitchCorrection.enabled,
                     "comparison must not enable correction without a running PitchNet")
        && expect(!engine.isPitchCorrectionActive(0),
                  "PitchNet metadata alone must not count as audible correction");

    TrackData staleTrack;
    staleTrack.id = "pitch-stale";
    staleTrack.name = "Stale PitchNet state";
    staleTrack.vst3Name = "PitchNet";
    staleTrack.pitchCorrection.enabled = true;
    staleTrack.pitchCorrection.auditionCorrected = true;
    staleTrack.vst3AraPlaybackEnabled = true;
    staleTrack.vst3Bypassed = false;
    engine.restoreTracksState({ staleTrack });

    ok = expect(!engine.isPitchCorrectionActive(0),
                "a failed or missing PitchNet runtime must never show correction ON") && ok;
    engine.capturePluginStates();
    auto snapshot = engine.getTracksSnapshot();
    ok = expect(!snapshot[0].pitchCorrection.enabled
                    && !snapshot[0].pitchCorrection.auditionCorrected
                    && !snapshot[0].vst3AraPlaybackEnabled
                    && snapshot[0].vst3Bypassed,
                "saving must normalise stale PitchNet metadata to a safe OFF state") && ok;

    engine.restoreTracksState({ staleTrack });
    engine.removeVst3PluginFromTrack(0);
    snapshot = engine.getTracksSnapshot();
    return expect(snapshot[0].vst3Name.isEmpty()
                      && snapshot[0].vst3DescriptionXml.isEmpty()
                      && snapshot[0].vst3State == nullptr
                      && snapshot[0].vst3AraArchive == nullptr
                      && !snapshot[0].pitchCorrection.enabled
                      && !snapshot[0].pitchCorrection.auditionCorrected
                      && !snapshot[0].vst3AraPlaybackEnabled,
                  "removing PitchNet must clear both its payload and correction display state")
        && ok;
}

bool testNonDestructiveEditUndoRedo()
{
    constexpr double sampleRate = 48000.0;
    AudioEngine engine;
    engine.prepare(512, sampleRate);

    TrackData fixtureTrack;
    fixtureTrack.id = "edit-track";
    fixtureTrack.name = "Non-destructive edit";
    fixtureTrack.pan = -0.25f;

    AudioClip fixtureClip;
    fixtureClip.id = "edit-clip";
    fixtureClip.name = "Edit fixture";
    fixtureClip.startTime = 1.0;
    fixtureClip.offset = 0.1;
    fixtureClip.duration = 0.8;
    fixtureClip.fadeInSeconds = 0.05;
    fixtureClip.fadeOutSeconds = 0.06;
    fixtureClip.rhythmMarkers = { { 0.2, 0.22 }, { 0.5, 0.48 } };
    fixtureClip.sampleRate = static_cast<int>(sampleRate);
    fixtureClip.buffer = std::make_shared<juce::AudioBuffer<float>>(1, 48000);
    for (int sample = 0; sample < fixtureClip.buffer->getNumSamples(); ++sample)
        fixtureClip.buffer->setSample(0, sample,
            static_cast<float>(sample) / 48000.0f - 0.5f);

    const auto originalBuffer = fixtureClip.buffer;
    const float originalStartSample = originalBuffer->getSample(0, 0);
    const float originalMiddleSample = originalBuffer->getSample(0, 12345);
    const float originalEndSample = originalBuffer->getSample(0, 47999);
    fixtureTrack.clips.push_back(fixtureClip);
    engine.replaceTracks({ fixtureTrack });

    const auto clipSnapshot = [&engine]()
    {
        return engine.getTracksSnapshot()[0].clips[0];
    };
    const auto sourceAudioIsUnchanged = [&]()
    {
        const auto clip = clipSnapshot();
        return clip.buffer == originalBuffer
            && clip.buffer != nullptr
            && clip.buffer->getNumSamples() == 48000
            && std::abs(clip.buffer->getSample(0, 0) - originalStartSample) < 1.0e-7f
            && std::abs(clip.buffer->getSample(0, 12345) - originalMiddleSample)
                   < 1.0e-7f
            && std::abs(clip.buffer->getSample(0, 47999) - originalEndSample)
                   < 1.0e-7f;
    };

    bool ok = true;
    engine.trimClipLeft("edit-clip", 1.2);
    auto clip = clipSnapshot();
    ok = expect(std::abs(clip.startTime - 1.2) < 1.0e-9
                    && std::abs(clip.offset - 0.3) < 1.0e-9
                    && std::abs(clip.duration - 0.6) < 1.0e-9,
                "left trim should only change the visible source range") && ok;
    ok = expect(sourceAudioIsUnchanged(),
                "left trim must not rewrite the source audio") && ok;
    engine.undo();
    clip = clipSnapshot();
    ok = expect(std::abs(clip.startTime - 1.0) < 1.0e-9
                    && std::abs(clip.offset - 0.1) < 1.0e-9
                    && std::abs(clip.duration - 0.8) < 1.0e-9
                    && clip.rhythmMarkers == fixtureClip.rhythmMarkers,
                "undo should restore the clip before a left trim") && ok;
    engine.redo();
    clip = clipSnapshot();
    ok = expect(std::abs(clip.startTime - 1.2) < 1.0e-9
                    && std::abs(clip.offset - 0.3) < 1.0e-9
                    && std::abs(clip.duration - 0.6) < 1.0e-9,
                "redo should restore the left trim") && ok;

    engine.trimClipRight("edit-clip", 1.5);
    clip = clipSnapshot();
    ok = expect(std::abs(clip.duration - 0.3) < 1.0e-9,
                "right trim should change only the visible duration") && ok;
    ok = expect(sourceAudioIsUnchanged(),
                "right trim must not rewrite the source audio") && ok;
    engine.undo();
    ok = expect(std::abs(clipSnapshot().duration - 0.6) < 1.0e-9,
                "undo should restore the clip before a right trim") && ok;
    engine.redo();
    ok = expect(std::abs(clipSnapshot().duration - 0.3) < 1.0e-9,
                "redo should restore the right trim") && ok;

    engine.setClipFades("edit-clip", 1.0, 2.0);
    clip = clipSnapshot();
    ok = expect(std::abs(clip.fadeInSeconds - 0.15) < 1.0e-9
                    && std::abs(clip.fadeOutSeconds - 0.15) < 1.0e-9,
                "overlapping fades should be scaled to the visible duration") && ok;
    ok = expect(sourceAudioIsUnchanged(),
                "fade edits must not rewrite the source audio") && ok;
    engine.undo();
    clip = clipSnapshot();
    ok = expect(std::abs(clip.fadeInSeconds - 0.05) < 1.0e-9
                    && std::abs(clip.fadeOutSeconds - 0.06) < 1.0e-9,
                "undo should restore the previous fades") && ok;
    engine.redo();
    clip = clipSnapshot();
    ok = expect(std::abs(clip.fadeInSeconds - 0.15) < 1.0e-9
                    && std::abs(clip.fadeOutSeconds - 0.15) < 1.0e-9,
                "redo should restore the edited fades") && ok;

    engine.setTrackPan(0, 0.75f);
    ok = expect(std::abs(engine.getTracksSnapshot()[0].pan - 0.75f) < 1.0e-6f,
                "pan edit should update the track") && ok;
    ok = expect(sourceAudioIsUnchanged(),
                "pan edits must not rewrite the source audio") && ok;
    engine.undo();
    ok = expect(std::abs(engine.getTracksSnapshot()[0].pan + 0.25f) < 1.0e-6f,
                "undo should restore the previous pan") && ok;
    engine.redo();
    ok = expect(std::abs(engine.getTracksSnapshot()[0].pan - 0.75f) < 1.0e-6f,
                "redo should restore the edited pan") && ok;

    // Reinstall the original fixture so each extreme boundary starts from the
    // same visible source range and cannot be influenced by prior edits.
    engine.replaceTracks({ fixtureTrack });
    engine.trimClipLeft("edit-clip", -100.0);
    clip = clipSnapshot();
    ok = expect(std::abs(clip.startTime - 0.9) < 1.0e-9
                    && std::abs(clip.offset) < 1.0e-9
                    && std::abs(clip.duration - 0.9) < 1.0e-9,
                "left trim should stop at the beginning of source audio") && ok;
    ok = expect(sourceAudioIsUnchanged(),
                "expanding a left trim must keep source audio intact") && ok;
    engine.undo();

    engine.trimClipLeft("edit-clip", 100.0);
    clip = clipSnapshot();
    ok = expect(std::abs(clip.startTime - 1.79) < 1.0e-9
                    && std::abs(clip.offset - 0.89) < 1.0e-9
                    && std::abs(clip.duration - 0.01) < 1.0e-9,
                "left trim should keep the minimum visible duration") && ok;
    ok = expect(clip.offset >= 0.0
                    && clip.offset + clip.duration <= clip.getSourceDuration() + 1.0e-9,
                "left trim should remain inside source boundaries") && ok;
    engine.undo();

    engine.trimClipRight("edit-clip", -100.0);
    clip = clipSnapshot();
    ok = expect(std::abs(clip.duration - 0.01) < 1.0e-9,
                "right trim should keep the minimum visible duration") && ok;
    ok = expect(sourceAudioIsUnchanged(),
                "minimum right trim must keep source audio intact") && ok;
    engine.undo();

    engine.trimClipRight("edit-clip", 100.0);
    clip = clipSnapshot();
    ok = expect(std::abs(clip.duration - 0.9) < 1.0e-9
                    && std::abs(clip.offset + clip.duration
                                - clip.getSourceDuration()) < 1.0e-9,
                "right trim should stop at the end of source audio") && ok;
    return expect(sourceAudioIsUnchanged(),
                  "all trim boundary edits must preserve source audio") && ok;
}

class ScopedExportFile
{
public:
    explicit ScopedExportFile(const juce::String& extension)
        : file(juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getNonexistentChildFile(
                       "simple-rec-pro-export-" + juce::Uuid().toString(),
                       extension,
                       false))
    {
    }

    ~ScopedExportFile()
    {
        file.deleteFile();
    }

    juce::File file;
};

bool waitForExport(AudioEngine& engine,
                   AudioEngine::ExportStatus& finalStatus,
                   int timeoutMilliseconds = 30000)
{
    const double startedAt = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - startedAt
           < timeoutMilliseconds)
    {
        finalStatus = engine.getExportStatus();
        if (finalStatus.completed)
            return true;
        juce::Thread::sleep(10);
    }

    engine.cancelExport();
    finalStatus = engine.getExportStatus();
    return false;
}

bool inspectExportedAudio(const juce::File& file,
                          int expectedChannels,
                          int64_t expectedSamples,
                          int expectedBits,
                          bool allowCodecPadding,
                          int expectedSampleRate = 48000)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(
        formats.createReaderFor(file));
    if (!expect(reader != nullptr,
                "exported audio should be readable by JUCE"))
        return false;

    bool ok = true;
    ok = expect(static_cast<int>(reader->numChannels) == expectedChannels,
                "exported audio should use the requested channel count") && ok;
    ok = expect(std::abs(reader->sampleRate - expectedSampleRate) < 0.5,
                "exported audio should use the requested sample rate") && ok;
    if (expectedBits > 0)
        ok = expect(reader->bitsPerSample == expectedBits,
                    "WAV export should use the requested bit depth") && ok;

    // MP3は1152サンプル単位のフレームなので、生デコーダーが見せる
    // エンコーダー遅延／末尾パディングは最大3フレームだけ許す。
    // 実際の音声区間は同条件WAVとの相関で別途検証する。
    const int64_t lengthTolerance = allowCodecPadding ? 3456 : 1;
    if (std::abs(reader->lengthInSamples - expectedSamples)
            > lengthTolerance)
    {
        std::cerr << "Exported length details: actual="
                  << reader->lengthInSamples
                  << " expected=" << expectedSamples
                  << " tolerance=" << lengthTolerance << std::endl;
    }
    ok = expect(std::abs(reader->lengthInSamples - expectedSamples)
                    <= lengthTolerance,
                "exported audio should have the requested timeline length") && ok;
    const auto readableSamples64 = std::min<int64_t>(reader->lengthInSamples,
                                                      96000);
    if (!expect(readableSamples64 > 0,
                "exported audio should contain samples"))
        return false;
    const int readableSamples = static_cast<int>(readableSamples64);
    juce::AudioBuffer<float> decoded(expectedChannels, readableSamples);
    decoded.clear();
    if (!expect(reader->read(&decoded, 0, readableSamples, 0, true, true),
                "JUCE should decode the exported samples"))
        return false;

    ok = expect(decoded.getMagnitude(0, readableSamples) < 1.0f,
                "decoded export should retain peak headroom below 0 dBFS")
        && ok;

    const int margin = std::min(2048, readableSamples / 10);
    const int analysisSamples = readableSamples - 2 * margin;
    const float leftRms = decoded.getRMSLevel(0, margin, analysisSamples);
    if (expectedChannels == 2)
    {
        const float rightRms = decoded.getRMSLevel(1, margin, analysisSamples);
        ok = expect(leftRms > 0.03f && rightRms > leftRms * 1.6f,
                    "stereo export should preserve distinct left and right levels") && ok;

        double differenceSquare = 0.0;
        for (int sample = margin; sample < readableSamples - margin; ++sample)
        {
            const double difference = decoded.getSample(0, sample)
                                    - decoded.getSample(1, sample);
            differenceSquare += difference * difference;
        }
        const double differenceRms = std::sqrt(
            differenceSquare / std::max(1, analysisSamples));
        ok = expect(differenceRms > 0.03,
                    "stereo export should not duplicate one channel into both sides") && ok;
    }
    else
    {
        ok = expect(leftRms > 0.03f,
                    "mono export should contain the downmixed fixture") && ok;
    }
    return ok;
}

bool readAudioForComparison(const juce::File& file,
                            juce::AudioBuffer<float>& audio,
                            double& sampleRate)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(
        formats.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0
        || reader->lengthInSamples > std::numeric_limits<int>::max())
        return false;

    sampleRate = reader->sampleRate;
    audio.setSize(static_cast<int>(reader->numChannels),
                  static_cast<int>(reader->lengthInSamples));
    audio.clear();
    return reader->read(&audio, 0, audio.getNumSamples(), 0, true, true);
}

double spectralAmplitude(const juce::AudioBuffer<float>& audio,
                         double sampleRate,
                         double frequency,
                         double startSeconds,
                         double durationSeconds)
{
    if (audio.getNumChannels() <= 0 || sampleRate <= 0.0)
        return 0.0;
    const int start = juce::jlimit(
        0, audio.getNumSamples(), juce::roundToInt(startSeconds * sampleRate));
    const int count = juce::jlimit(
        0, audio.getNumSamples() - start,
        juce::roundToInt(durationSeconds * sampleRate));
    if (count <= 0)
        return 0.0;

    double real = 0.0;
    double imaginary = 0.0;
    for (int sample = 0; sample < count; ++sample)
    {
        const double phase = juce::MathConstants<double>::twoPi
                           * frequency * sample / sampleRate;
        const double value = audio.getSample(0, start + sample);
        real += value * std::cos(phase);
        imaginary -= value * std::sin(phase);
    }
    return 2.0 * std::sqrt(real * real + imaginary * imaginary) / count;
}

bool compareMp3ContentToWav(const juce::File& mp3File,
                            const juce::File& wavFile)
{
    juce::AudioBuffer<float> mp3;
    juce::AudioBuffer<float> wav;
    double mp3Rate = 0.0;
    double wavRate = 0.0;
    if (!expect(readAudioForComparison(mp3File, mp3, mp3Rate)
                    && readAudioForComparison(wavFile, wav, wavRate),
                "MP3 and WAV references should both decode"))
        return false;
    if (!expect(mp3.getNumChannels() == wav.getNumChannels()
                    && std::abs(mp3Rate - wavRate) < 0.5,
                "MP3 and WAV references should use identical layouts"))
        return false;

    const int maximumShift = std::min(4096,
        std::max(0, mp3.getNumSamples() - 1));
    double bestCorrelation = -1.0;
    int bestShift = 0;
    for (int shift = 0; shift <= maximumShift; ++shift)
    {
        const int comparable = std::min(wav.getNumSamples(),
                                        mp3.getNumSamples() - shift);
        if (comparable < 1024)
            continue;
        double dot = 0.0;
        double wavEnergy = 0.0;
        double mp3Energy = 0.0;
        for (int sample = 0; sample < comparable; sample += 32)
        {
            const double left = wav.getSample(0, sample);
            const double right = mp3.getSample(0, sample + shift);
            dot += left * right;
            wavEnergy += left * left;
            mp3Energy += right * right;
        }
        const double denominator = std::sqrt(wavEnergy * mp3Energy);
        const double correlation = denominator > 1.0e-12
            ? dot / denominator : 0.0;
        if (correlation > bestCorrelation)
        {
            bestCorrelation = correlation;
            bestShift = shift;
        }
    }

    const int audibleSamples = mp3.getNumSamples() - bestShift;
    return expect(bestCorrelation > 0.88,
                  "MP3 audible content should match the same-settings WAV")
        && expect(std::abs(audibleSamples - wav.getNumSamples()) <= 3456,
                  "MP3 audible timeline should match WAV within codec padding");
}

std::unique_ptr<juce::AudioBuffer<float>> readExportedAudio(
    const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(
        formats.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0
        || reader->lengthInSamples > 1000000)
        return {};

    auto result = std::make_unique<juce::AudioBuffer<float>>(
        static_cast<int>(reader->numChannels),
        static_cast<int>(reader->lengthInSamples));
    result->clear();
    if (!reader->read(result.get(),
                      0,
                      result->getNumSamples(),
                      0,
                      true,
                      true))
        return {};
    return result;
}

bool testMasteringExportKeepsTimelineEdges()
{
    constexpr int sampleRate = 48000;
    constexpr int fixtureSamples = 4096;

    AudioEngine engine;
    engine.prepare(512, sampleRate);
    engine.setMasterVolume(1.0f);

    ProjectDawSettings projectSettings;
    projectSettings.mastering.enabled = true;
    projectSettings.mastering.limiterEnabled = true;
    projectSettings.mastering.ceilingDb = -1.0f;
    projectSettings.mastering.targetLufs = -14.0f;
    projectSettings.mastering.auditionProcessed = false;
    engine.setProjectDawSettings(projectSettings, false);

    TrackData track;
    track.id = "mastering-edge-track";
    track.name = "Mastering edge fixture";
    track.volume = 1.0f;

    AudioClip clip;
    clip.id = "mastering-edge-clip";
    clip.sampleRate = sampleRate;
    clip.duration = static_cast<double>(fixtureSamples) / sampleRate;
    clip.buffer = std::make_shared<juce::AudioBuffer<float>>(2, fixtureSamples);
    clip.buffer->clear();
    clip.buffer->setSample(0, 0, 0.60f);
    clip.buffer->setSample(1, 0, 0.35f);
    clip.buffer->setSample(0, fixtureSamples - 1, -0.55f);
    clip.buffer->setSample(1, fixtureSamples - 1, -0.30f);
    track.clips.push_back(std::move(clip));
    engine.replaceTracks({ std::move(track) });

    ScopedExportFile output(".wav");
    ExportSettings settings;
    settings.format = ExportFormat::wav;
    settings.channels = ExportChannels::stereo;
    settings.wavBits = 24;
    settings.sampleRate = sampleRate;
    settings.target = ExportTarget::fullMix;
    if (!expect(engine.startExport(output.file, settings),
                "mastered edge-fixture export should start"))
        return false;

    AudioEngine::ExportStatus status;
    if (!expect(waitForExport(engine, status),
                "mastered edge-fixture export should complete"))
        return false;
    if (!expect(status.succeeded,
                "mastered edge-fixture export should succeed"))
        return false;

    auto decoded = readExportedAudio(output.file);
    if (!expect(decoded != nullptr
                    && decoded->getNumChannels() == 2
                    && decoded->getNumSamples() == fixtureSamples,
                "mastered edge-fixture WAV should retain its exact shape"))
        return false;

    const float firstPeak = std::max(
        std::abs(decoded->getSample(0, 0)),
        std::abs(decoded->getSample(1, 0)));
    const float lastPeak = std::max(
        std::abs(decoded->getSample(0, fixtureSamples - 1)),
        std::abs(decoded->getSample(1, fixtureSamples - 1)));
    return expect(firstPeak > 0.10f,
                  "master look-ahead compensation must retain the first impulse")
        && expect(lastPeak > 0.10f,
                  "master look-ahead flush must retain the final impulse");
}

bool testExportFinalPeakProtection()
{
    constexpr int sampleRate = 48000;
    constexpr int fixtureSamples = 12000;

    AudioEngine engine;
    engine.prepare(512, sampleRate);
    engine.setMasterVolume(1.0f);
    ProjectDawSettings projectSettings;
    projectSettings.mastering.enabled = false;
    engine.setProjectDawSettings(projectSettings, false);

    auto sharedAudio = std::make_shared<juce::AudioBuffer<float>>(
        2, fixtureSamples);
    for (int channel = 0; channel < sharedAudio->getNumChannels(); ++channel)
        for (int sample = 0; sample < fixtureSamples; ++sample)
            sharedAudio->setSample(channel, sample, 0.90f);

    std::vector<TrackData> tracks;
    for (int index = 0; index < 2; ++index)
    {
        TrackData track;
        track.id = "peak-track-" + juce::String(index);
        track.name = "Peak fixture";
        track.volume = 1.0f;

        AudioClip clip;
        clip.id = "peak-clip-" + juce::String(index);
        clip.sampleRate = sampleRate;
        clip.duration = 0.25;
        clip.buffer = sharedAudio;
        track.clips.push_back(std::move(clip));
        tracks.push_back(std::move(track));
    }
    engine.replaceTracks(std::move(tracks));

    const double unprotectedPeak = 2.0 * 0.90 * std::sqrt(0.5);
    bool ok = expect(unprotectedPeak > 1.0,
                     "peak-protection fixture should exceed 0 dBFS before export");

    ScopedExportFile output(".wav");
    ExportSettings settings;
    settings.format = ExportFormat::wav;
    settings.channels = ExportChannels::stereo;
    settings.wavBits = 24;
    settings.sampleRate = sampleRate;
    settings.target = ExportTarget::fullMix;
    if (!expect(engine.startExport(output.file, settings),
                "over-level WAV export should start"))
        return false;

    AudioEngine::ExportStatus status;
    if (!expect(waitForExport(engine, status),
                "over-level WAV export should complete"))
        return false;
    if (!expect(status.succeeded,
                "over-level WAV export should succeed"))
        return false;

    auto decoded = readExportedAudio(output.file);
    if (!expect(decoded != nullptr && decoded->getNumChannels() == 2,
                "protected WAV should be readable as stereo"))
        return false;
    const float finalPeak = decoded->getMagnitude(
        0, decoded->getNumSamples());
    ok = expect(finalPeak < 1.0f,
                "final WAV peak protection must stay below 0 dBFS") && ok;
    ok = expect(finalPeak > 0.98f,
                "final WAV peak protection should preserve usable level") && ok;
    return ok;
}

bool testAsyncExportCancellation()
{
    constexpr int sampleRate = 48000;
    constexpr int durationSeconds = 30;
    constexpr int fixtureSamples = sampleRate * durationSeconds;

    AudioEngine engine;
    engine.prepare(512, sampleRate);
    TrackData track;
    track.id = "cancel-track";
    track.name = "Cancellation fixture";
    track.playbackSpeed = 0.82;
    track.pitchSemitones = 5.0;
    AudioClip clip;
    clip.id = "cancel-clip";
    clip.sampleRate = sampleRate;
    clip.duration = durationSeconds;
    clip.buffer = std::make_shared<juce::AudioBuffer<float>>(2,
                                                             fixtureSamples);
    clip.buffer->clear();
    for (int sample = 0; sample < fixtureSamples; sample += 97)
    {
        clip.buffer->setSample(0, sample, 0.2f);
        clip.buffer->setSample(1, sample, -0.2f);
    }
    track.clips.push_back(std::move(clip));
    engine.replaceTracks({ std::move(track) });

    ScopedExportFile output(".wav");
    ExportSettings settings;
    settings.format = ExportFormat::wav;
    settings.channels = ExportChannels::stereo;
    settings.wavBits = 24;
    settings.sampleRate = sampleRate;
    settings.target = ExportTarget::fullMix;
    if (!expect(engine.startExport(output.file, settings),
                "cancellable export should start asynchronously"))
        return false;

    const double waitStarted = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - waitStarted < 1000.0)
    {
        const auto status = engine.getExportStatus();
        if (!status.active || status.progress >= 0.05f)
            break;
        juce::Thread::sleep(1);
    }
    engine.cancelExport();

    AudioEngine::ExportStatus finalStatus;
    if (!expect(waitForExport(engine, finalStatus, 10000),
                "cancelled export should stop promptly"))
        return false;
    return expect(finalStatus.cancelled && !finalStatus.succeeded,
                  "cancelled export should never report success")
        && expect(!output.file.existsAsFile(),
                  "cancelled export should not replace the requested file");
}

bool testExportPipeline()
{
    constexpr int sampleRate = 48000;
    constexpr int fixtureSamples = sampleRate * 2;

    AudioEngine engine;
    engine.prepare(512, sampleRate);
    engine.setMasterVolume(1.0f);

    ProjectDawSettings projectSettings;
    projectSettings.mastering.enabled = false;
    engine.setProjectDawSettings(projectSettings, false);

    TrackData track;
    track.id = "export-track";
    track.name = "Export fixture";
    track.volume = 1.0f;
    track.pan = 0.0f;

    AudioClip clip;
    clip.id = "export-clip";
    clip.name = "Stereo fixture";
    clip.sampleRate = sampleRate;
    clip.duration = 2.0;
    clip.buffer = std::make_shared<juce::AudioBuffer<float>>(2, fixtureSamples);
    for (int sample = 0; sample < fixtureSamples; ++sample)
    {
        const double time = static_cast<double>(sample) / sampleRate;
        const double leftFrequency = time < 1.0 ? 440.0 : 660.0;
        const double rightFrequency = time < 1.0 ? 880.0 : 1320.0;
        clip.buffer->setSample(0, sample,
            static_cast<float>(0.20 * std::sin(
                juce::MathConstants<double>::twoPi * leftFrequency * time)));
        clip.buffer->setSample(1, sample,
            static_cast<float>(0.42 * std::sin(
                juce::MathConstants<double>::twoPi * rightFrequency * time)));
    }
    track.clips.push_back(std::move(clip));

    TrackData secondTrack;
    secondTrack.id = "export-track-secondary";
    secondTrack.name = "Full mix identity";
    secondTrack.volume = 1.0f;
    AudioClip secondClip;
    secondClip.id = "export-clip-secondary";
    secondClip.name = "1100 Hz identity";
    secondClip.sampleRate = sampleRate;
    secondClip.duration = 2.0;
    secondClip.buffer = std::make_shared<juce::AudioBuffer<float>>(
        2, fixtureSamples);
    for (int sample = 0; sample < fixtureSamples; ++sample)
    {
        const double time = static_cast<double>(sample) / sampleRate;
        const float value = static_cast<float>(0.12 * std::sin(
            juce::MathConstants<double>::twoPi * 1100.0 * time));
        secondClip.buffer->setSample(0, sample, value);
        secondClip.buffer->setSample(1, sample, value);
    }
    secondTrack.clips.push_back(std::move(secondClip));
    engine.replaceTracks({ std::move(track), std::move(secondTrack) });

    struct WavCase
    {
        ExportTarget target;
        ExportChannels channels;
        int bits;
        double rangeStart;
        double rangeEnd;
        int64_t expectedSamples;
    };
    const WavCase wavCases[] {
        { ExportTarget::fullMix, ExportChannels::stereo, 16,
          0.0, 0.0, fixtureSamples },
        { ExportTarget::abRange, ExportChannels::stereo, 24,
          0.25, 0.75, fixtureSamples / 4 },
        { ExportTarget::fullMix, ExportChannels::mono, 24,
          0.0, 0.0, fixtureSamples },
        { ExportTarget::abRange, ExportChannels::mono, 16,
          0.125, 0.375, fixtureSamples / 8 }
    };

    bool ok = true;
    for (const auto& wavCase : wavCases)
    {
        ScopedExportFile output(".wav");
        ExportSettings settings;
        settings.format = ExportFormat::wav;
        settings.channels = wavCase.channels;
        settings.wavBits = wavCase.bits;
        settings.sampleRate = sampleRate;
        settings.target = wavCase.target;
        settings.rangeStartSeconds = wavCase.rangeStart;
        settings.rangeEndSeconds = wavCase.rangeEnd;

        if (!expect(engine.startExport(output.file, settings),
                    "WAV export should start"))
        {
            ok = false;
            continue;
        }

        AudioEngine::ExportStatus status;
        if (!expect(waitForExport(engine, status),
                    "WAV export should complete before the timeout"))
        {
            ok = false;
            continue;
        }
        if (!status.succeeded)
            std::cerr << "WAV export error: "
                      << status.message.toStdString() << std::endl;
        ok = expect(status.succeeded,
                    "WAV export should finish successfully") && ok;
        if (status.succeeded)
            ok = inspectExportedAudio(output.file,
                                      static_cast<int>(wavCase.channels),
                                      wavCase.expectedSamples,
                                      wavCase.bits,
                                      false) && ok;
    }

    ok = testMasteringExportKeepsTimelineEdges() && ok;
    ok = testExportFinalPeakProtection() && ok;
    ok = testAsyncExportCancellation() && ok;

    // 保存データに有効なVST3があるのに実体を読み込めていない場合、
    // 無加工音を成功扱いで書き出してはならない。
    const auto validTracks = engine.getTracksSnapshot();
    auto missingPluginTracks = validTracks;
    missingPluginTracks[0].vst3Name = "Missing Test VST3";
    missingPluginTracks[0].vst3Bypassed = false;
    engine.replaceTracks(std::move(missingPluginTracks));
    {
        ScopedExportFile missingPluginOutput(".wav");
        ExportSettings missingPluginSettings;
        missingPluginSettings.format = ExportFormat::wav;
        missingPluginSettings.channels = ExportChannels::stereo;
        missingPluginSettings.wavBits = 24;
        missingPluginSettings.sampleRate = sampleRate;
        missingPluginSettings.target = ExportTarget::fullMix;
        if (!expect(engine.startExport(missingPluginOutput.file,
                                       missingPluginSettings),
                    "missing-plugin export validation should start"))
        {
            return false;
        }

        AudioEngine::ExportStatus missingPluginStatus;
        if (!expect(waitForExport(engine, missingPluginStatus),
                    "missing-plugin validation should finish"))
        {
            return false;
        }
        ok = expect(!missingPluginStatus.succeeded
                        && missingPluginStatus.message.containsIgnoreCase(
                            juce::String::fromUTF8(u8"読み込まれていません")),
                    "an unloaded active VST3 must stop export with a clear reason")
            && ok;
    }
    engine.replaceTracks(validTracks);

    struct Mp3Case
    {
        ExportTarget target;
        ExportChannels channels;
        int outputSampleRate;
        double rangeStart;
        double rangeEnd;
        int64_t expectedSamples;
        double expectedFrequency;
        double rejectedFrequency;
    };
    const Mp3Case mp3Cases[] {
        { ExportTarget::fullMix, ExportChannels::stereo, 48000,
          0.0, 0.0, 96000, 1100.0, 0.0 },
        { ExportTarget::abRange, ExportChannels::mono, 44100,
          1.0, 1.5, 22050, 1320.0, 440.0 },
        { ExportTarget::selectedTrack, ExportChannels::stereo, 44100,
          0.0, 0.0, 88200, 440.0, 1100.0 }
    };

    for (const auto& mp3Case : mp3Cases)
    {
        ScopedExportFile mp3Output(".mp3");
        ScopedExportFile wavReference(".wav");
        ExportSettings mp3Settings;
        mp3Settings.format = ExportFormat::mp3;
        mp3Settings.channels = mp3Case.channels;
        mp3Settings.mp3Kbps = 192;
        mp3Settings.sampleRate = mp3Case.outputSampleRate;
        mp3Settings.target = mp3Case.target;
        mp3Settings.trackIndex = 0;
        mp3Settings.rangeStartSeconds = mp3Case.rangeStart;
        mp3Settings.rangeEndSeconds = mp3Case.rangeEnd;

        auto wavSettings = mp3Settings;
        wavSettings.format = ExportFormat::wav;
        wavSettings.wavBits = 24;
        if (!expect(engine.startExport(wavReference.file, wavSettings),
                    "same-settings WAV reference should start"))
        {
            ok = false;
            continue;
        }
        AudioEngine::ExportStatus wavStatus;
        if (!expect(waitForExport(engine, wavStatus) && wavStatus.succeeded,
                    "same-settings WAV reference should finish"))
        {
            ok = false;
            continue;
        }

        if (!expect(engine.startExport(mp3Output.file, mp3Settings),
                    "192 kbps MP3 export should start"))
        {
            ok = false;
            continue;
        }

        AudioEngine::ExportStatus mp3Status;
        if (!expect(waitForExport(engine, mp3Status),
                    "MP3 export should complete before the timeout"))
        {
            ok = false;
            continue;
        }
        if (!mp3Status.succeeded)
            std::cerr << "MP3 export error: "
                      << mp3Status.message.toStdString() << std::endl;
        ok = expect(mp3Status.succeeded,
                    "192 kbps MP3 export should finish successfully") && ok;
        if (mp3Status.succeeded)
        {
            ok = inspectExportedAudio(mp3Output.file,
                                      static_cast<int>(mp3Case.channels),
                                      mp3Case.expectedSamples,
                                      0,
                                      true,
                                      mp3Case.outputSampleRate) && ok;
            ok = compareMp3ContentToWav(mp3Output.file,
                                        wavReference.file) && ok;

            juce::AudioBuffer<float> referenceAudio;
            double referenceRate = 0.0;
            if (expect(readAudioForComparison(wavReference.file,
                                              referenceAudio,
                                              referenceRate),
                       "WAV reference should decode for target validation"))
            {
                const double expectedAmplitude = spectralAmplitude(
                    referenceAudio, referenceRate,
                    mp3Case.expectedFrequency, 0.10, 0.30);
                ok = expect(expectedAmplitude > 0.04,
                            "export target should contain its identity tone") && ok;
                if (mp3Case.rejectedFrequency > 0.0)
                {
                    const double rejectedAmplitude = spectralAmplitude(
                        referenceAudio, referenceRate,
                        mp3Case.rejectedFrequency, 0.10, 0.30);
                    ok = expect(expectedAmplitude
                                    > std::max(0.01,
                                               rejectedAmplitude * 3.0),
                                "A/B and selected-track exports should contain the requested content")
                        && ok;
                }
            }
            else
            {
                ok = false;
            }
            const double durationSeconds = static_cast<double>(
                mp3Case.expectedSamples) / mp3Case.outputSampleRate;
            const double estimatedKbps = static_cast<double>(
                mp3Output.file.getSize()) * 8.0 / 1000.0
                / durationSeconds;
            ok = expect(estimatedKbps >= 150.0 && estimatedKbps <= 300.0,
                        "MP3 export should be close to the requested 192 kbps")
                && ok;
        }
    }
    return ok;
}

bool testRecordingAcrossChunkBoundary()
{
    constexpr int sampleCount = 70000;
    AudioEngine engine;
    engine.prepare(512, 48000.0);
    const int trackIndex = engine.addTrack("Chunked recording");
    engine.record(trackIndex);

    juce::AudioBuffer<float> input(2, sampleCount);
    juce::AudioBuffer<float> output(2, sampleCount);
    for (int i = 0; i < sampleCount; ++i)
    {
        input.setSample(0, i, i < 65536 ? 0.2f : 0.4f);
        input.setSample(1, i, i < 65536 ? -0.3f : -0.6f);
    }
    processStereoBlock(engine, input, output);
    engine.stop();

    const auto tracks = engine.getTracksSnapshot();
    if (!expect(tracks.size() == 1 && tracks[0].clips.size() == 1,
                "chunked recording should create one clip"))
        return false;

    const auto& buffer = *tracks[0].clips[0].buffer;
    return expect(buffer.getNumSamples() == sampleCount,
                  "chunked recording must preserve its full length")
           && expect(std::abs(buffer.getSample(0, 65535) - 0.2f) < 1.0e-6f
                         && std::abs(buffer.getSample(0, 65536) - 0.4f) < 1.0e-6f,
                     "left audio must remain continuous across a chunk boundary")
           && expect(std::abs(buffer.getSample(1, 65535) + 0.3f) < 1.0e-6f
                         && std::abs(buffer.getSample(1, 65536) + 0.6f) < 1.0e-6f,
                     "right audio must remain continuous across a chunk boundary");
}

bool testLongRecordingPreviewKeepsWholeTake()
{
    AudioEngine engine;
    engine.prepare(1, 48000.0);
    const int trackIndex = engine.addTrack("Long preview");
    engine.record(trackIndex);

    juce::AudioBuffer<float> input(2, 1);
    juce::AudioBuffer<float> output(2, 1);
    for (int block = 0; block < 5000; ++block)
    {
        const float value = block == 0 ? 0.9f : 0.1f;
        input.setSample(0, 0, value);
        input.setSample(1, 0, value);
        processStereoBlock(engine, input, output);
    }

    const auto preview = engine.getRecordingBufferSnapshot();
    const bool result = expect(engine.getRecordingSampleCount() == 5000,
                               "preview reads must not lose recorded samples")
                        && expect(!preview.empty() && preview.size() <= 4096,
                                  "recording preview should remain bounded")
                        && expect(preview.front() > 0.8f,
                                  "compressed preview should retain the start of a long take");
    engine.stop();
    return result;
}

bool testTempoPointInsertionPreservesEarlierPoint()
{
    AudioEngine engine;
    auto settings = engine.getProjectDawSettings();
    settings.tempoMap = {
        { 0.0, 120.0, 4, 4 },
        { 8.0, 90.0, 3, 4 }
    };
    engine.setProjectDawSettings(settings);
    engine.setCurrentTime(4.0);
    engine.setTempoPoint(engine.getCurrentTime(), 100.0, 4, 4);

    const auto points = engine.getProjectDawSettings().tempoMap;
    return expect(points.size() == 3
                      && std::abs(points[0].timeSeconds) < 1.0e-9
                      && std::abs(points[0].bpm - 120.0) < 1.0e-9,
                  "adding a tempo point at the playhead should preserve the active earlier point")
        && expect(std::abs(points[1].timeSeconds - 4.0) < 1.0e-9
                      && std::abs(points[1].bpm - 100.0) < 1.0e-9
                      && std::abs(points[2].timeSeconds - 8.0) < 1.0e-9,
                  "the new tempo point should be inserted at the playhead");
}

bool testRhythmQuantizeUsesCumulativeTempoMap()
{
    constexpr int sampleRate = 1000;
    AudioEngine engine;
    engine.prepare(128, sampleRate);

    TrackData track;
    track.id = "tempo-rhythm-track";
    track.name = "Tempo rhythm";
    AudioClip clip;
    clip.id = "tempo-rhythm-clip";
    clip.name = "Tempo rhythm clip";
    clip.startTime = 1.25;
    clip.duration = 1.0;
    clip.sampleRate = sampleRate;
    clip.buffer = std::make_shared<juce::AudioBuffer<float>>(1, sampleRate);
    clip.buffer->clear();
    for (int sample = 400; sample < 410; ++sample)
        clip.buffer->setSample(0, sample, 1.0f);
    track.clips.push_back(clip);
    engine.restoreTracksState({ track });

    auto settings = engine.getProjectDawSettings();
    settings.tempoMap = {
        { 0.0, 120.0, 4, 4 },
        { 1.25, 60.0, 4, 4 }
    };
    engine.setProjectDawSettings(settings);

    const int markerCount = engine.quantizeClipRhythm(clip.id, 1.0f);
    const auto tracks = engine.getTracksSnapshot();
    if (!expect(markerCount == 1 && tracks.size() == 1
                    && tracks[0].clips.size() == 1
                    && tracks[0].clips[0].rhythmMarkers.size() == 1,
                "tempo-map rhythm quantize should detect the deterministic onset"))
        return false;

    const auto& marker = tracks[0].clips[0].rhythmMarkers.front();
    return expect(std::abs(marker.sourceTime - 0.40) < 1.0e-6
                      && std::abs(marker.targetTime - 0.50) < 1.0e-6,
                  "rhythm quantize should target the cumulative beat after a tempo change");
}

bool testTempoMapCountInUsesCurrentSegment()
{
    constexpr int blockSize = 256;
    constexpr double sampleRate = 8000.0;
    constexpr int expectedCountInSamples = 24000; // 3/4 at 60 BPM

    AudioEngine engine;
    engine.prepare(blockSize, sampleRate);
    auto settings = engine.getProjectDawSettings();
    settings.tempoMap = {
        { 0.0, 120.0, 4, 4 },
        { 2.0, 60.0, 3, 4 }
    };
    engine.setProjectDawSettings(settings);
    engine.setCurrentTime(3.0);
    engine.getMetronome().setEnabled(false);
    engine.setCountInEnabled(true);
    const int trackIndex = engine.addTrack("Tempo Count-In");
    engine.record(trackIndex);

    juce::AudioBuffer<float> input(2, blockSize);
    juce::AudioBuffer<float> output(2, blockSize);
    input.clear();
    input.addSample(0, 0, 0.35f);
    input.addSample(1, 0, 0.35f);

    const int completeBlocks = expectedCountInSamples / blockSize;
    const int remainder = expectedCountInSamples % blockSize;
    bool heardForcedClick = false;
    for (int block = 0; block < completeBlocks; ++block)
    {
        processStereoBlock(engine, input, output);
        heardForcedClick = heardForcedClick
            || output.getMagnitude(0, 0, blockSize) > 0.001f;
    }

    const bool stayedInCountIn = expect(
        engine.getPlaybackState() == PlaybackState::CountIn,
        "count-in should use the tempo and meter at the recording position");
    processStereoBlock(engine, input, output);
    const bool transitioned = expect(
        engine.getPlaybackState() == PlaybackState::Recording
            && engine.getRecordingSampleCount() == blockSize - remainder,
        "tempo-map count-in should transition at the exact measure boundary")
        && expect(heardForcedClick,
                  "tempo-map count-in should force an audible click");
    engine.stop();
    return stayedInCountIn && transitioned;
}

bool testPlaybackMetronomeUsesTempoMap()
{
    constexpr int sampleRate = 1000;
    AudioEngine engine;
    engine.prepare(700, sampleRate);

    TrackData track;
    track.id = "tempo-click-track";
    track.name = "Tempo click";
    AudioClip clip;
    clip.id = "tempo-click-clip";
    clip.name = "Tempo click clip";
    clip.duration = 4.0;
    clip.sampleRate = sampleRate;
    clip.buffer = std::make_shared<juce::AudioBuffer<float>>(1,
                                                             sampleRate * 4);
    clip.buffer->clear();
    track.clips.push_back(clip);
    engine.restoreTracksState({ track });

    auto settings = engine.getProjectDawSettings();
    settings.tempoMap = {
        { 0.0, 120.0, 4, 4 },
        { 2.25, 60.0, 4, 4 }
    };
    engine.setProjectDawSettings(settings);
    engine.getMetronome().setEnabled(true);
    engine.setCurrentTime(2.40);
    engine.play();

    juce::AudioBuffer<float> input(2, 700);
    juce::AudioBuffer<float> output(2, 700);
    input.clear();
    processStereoBlock(engine, input, output);
    engine.stop();

    return expect(output.getMagnitude(0, 350, 8) > 0.1f,
                  "playback metronome should click on the cumulative beat")
        && expect(output.getMagnitude(0, 598, 8) < 1.0e-6f,
                  "playback metronome should not restart phase at a tempo point");
}

bool testCountInBoundary(int numerator, int denominator, int expectedCountInSamples)
{
    constexpr int blockSize = 512;
    constexpr double sampleRate = 48000.0;

    AudioEngine engine;
    engine.prepare(blockSize, sampleRate);
    engine.setTempoPoint(0.0, 120.0, numerator, denominator);
    engine.getMetronome().setEnabled(false);
    engine.setCountInEnabled(true);
    const int trackIndex = engine.addTrack("Count-In");
    engine.record(trackIndex);

    juce::AudioBuffer<float> input(2, blockSize);
    juce::AudioBuffer<float> output(2, blockSize);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < blockSize; ++i)
            input.setSample(ch, i, 0.35f);

    const int completeBlocks = expectedCountInSamples / blockSize;
    const int remainder = expectedCountInSamples % blockSize;
    bool heardForcedClick = false;
    for (int block = 0; block < completeBlocks; ++block)
    {
        processStereoBlock(engine, input, output);
        heardForcedClick = heardForcedClick || output.getMagnitude(0, 0, blockSize) > 0.001f;
    }

    if (!expect(engine.getPlaybackState() == PlaybackState::CountIn,
                "count-in must last for the full measure"))
        return false;

    processStereoBlock(engine, input, output);
    const int expectedRecordedTail = blockSize - remainder;
    const bool boundaryOk = expect(engine.getPlaybackState() == PlaybackState::Recording,
                                   "count-in should transition inside the boundary block")
                            && expect(engine.getRecordingSampleCount() == expectedRecordedTail,
                                      "only post-count-in samples should be recorded")
                            && expect(heardForcedClick,
                                      "count-in click should sound even when normal click is off");
    engine.stop();
    return boundaryOk;
}

bool testPlaybackTransformOffline()
{
    constexpr double sampleRate = 48000.0;
    juce::AudioBuffer<float> offlineInput(2, static_cast<int>(sampleRate * 2.0));
    for (int channel = 0; channel < 2; ++channel)
    {
        for (int sample = 0; sample < offlineInput.getNumSamples(); ++sample)
        {
            offlineInput.setSample(channel,
                                   sample,
                                   std::sin(juce::MathConstants<double>::twoPi
                                            * 440.0 * sample / sampleRate)
                                       * 0.25f);
        }
    }

    const auto estimateFrequency = [sampleRate](const juce::AudioBuffer<float>& buffer)
    {
        const int start = buffer.getNumSamples() / 4;
        const int end = buffer.getNumSamples() * 3 / 4;
        const float* samples = buffer.getReadPointer(0);
        int positiveCrossings = 0;
        for (int sample = start + 1; sample < end; ++sample)
        {
            if (samples[sample - 1] <= 0.0f && samples[sample] > 0.0f)
                ++positiveCrossings;
        }
        return positiveCrossings * sampleRate / static_cast<double>(end - start);
    };

    auto offlineOutput = PlaybackTransform::processOffline(offlineInput,
                                                            sampleRate,
                                                            0.75,
                                                            0.0);
    const bool offlineDuration = expect(
        offlineOutput != nullptr && offlineOutput->getNumSamples() == 128000,
        "export transform should apply the selected speed to duration")
        && expect(std::abs(estimateFrequency(*offlineOutput) - 440.0) < 10.0,
                  "speed-only export should preserve the original key");

    auto octaveUp = PlaybackTransform::processOffline(offlineInput,
                                                       sampleRate,
                                                       1.0,
                                                       12.0);
    const bool offlinePitch = expect(
        octaveUp != nullptr && octaveUp->getNumSamples() == offlineInput.getNumSamples(),
        "key-only export should preserve the original duration")
        && expect(std::abs(estimateFrequency(*octaveUp) - 880.0) < 20.0,
                  "a twelve-semitone export should raise the tone by one octave");

    int cancellationChecks = 0;
    auto cancelledOutput = PlaybackTransform::processOffline(
        offlineInput,
        sampleRate,
        0.75,
        0.0,
        [&cancellationChecks]
        {
            ++cancellationChecks;
            return cancellationChecks >= 5;
        });
    const bool offlineCancellation = expect(
        cancelledOutput == nullptr,
        "export transform should return no audio when cancellation is requested")
        && expect(cancellationChecks >= 5,
                  "export transform should poll cancellation during a long conversion");

    return offlineDuration && offlinePitch && offlineCancellation;
}

double estimateFrequency(const juce::AudioBuffer<float>& buffer,
                         double sampleRate,
                         int startSample,
                         int endSample)
{
    const int start = juce::jlimit(0, buffer.getNumSamples(), startSample);
    const int end = juce::jlimit(start, buffer.getNumSamples(), endSample);
    if (end - start < 2)
        return 0.0;

    const auto* samples = buffer.getReadPointer(0);
    int positiveCrossings = 0;
    for (int sample = start + 1; sample < end; ++sample)
        if (samples[sample - 1] <= 0.0f && samples[sample] > 0.0f)
            ++positiveCrossings;
    return positiveCrossings * sampleRate / static_cast<double>(end - start);
}

double getEnergyCentroid(const juce::AudioBuffer<float>& buffer)
{
    const auto* samples = buffer.getReadPointer(0);
    double weightedPosition = 0.0;
    double totalEnergy = 0.0;
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        const double energy = static_cast<double>(samples[sample])
                            * samples[sample];
        weightedPosition += sample * energy;
        totalEnergy += energy;
    }
    return totalEnergy > 1.0e-12 ? weightedPosition / totalEnergy : -1.0;
}

std::shared_ptr<juce::AudioBuffer<float>> renderRealtimeTransform(
    const juce::AudioBuffer<float>& input,
    double sampleRate,
    double playbackSpeed,
    double pitchSemitones,
    int outputSamples,
    int blockSize,
    int64_t sourceStart = 0)
{
    PlaybackTransform transform;
    transform.prepare(2, sampleRate);
    auto result = std::make_shared<juce::AudioBuffer<float>>(2, outputSamples);
    result->clear();

    int64_t sourcePosition = sourceStart;
    for (int outputPosition = 0; outputPosition < outputSamples;)
    {
        const int outputCount = std::min(blockSize,
                                         outputSamples - outputPosition);
        const int inputCount = transform.getInputSamplesForOutput(
            outputCount, playbackSpeed);
        juce::AudioBuffer<float> source(2, std::max(1, inputCount));
        source.clear();
        if (sourcePosition < input.getNumSamples())
        {
            const int available = static_cast<int>(std::max<int64_t>(0,
                std::min<int64_t>(inputCount,
                    input.getNumSamples() - sourcePosition)));
            if (available > 0)
                for (int channel = 0; channel < 2; ++channel)
                    source.copyFrom(channel, 0, input,
                                    std::min(channel,
                                             input.getNumChannels() - 1),
                                    static_cast<int>(sourcePosition),
                                    available);
        }

        juce::AudioBuffer<float> outputView(
            result->getArrayOfWritePointers(),
            result->getNumChannels(),
            outputPosition,
            outputCount);
        transform.process(source,
                          inputCount,
                          outputView,
                          outputCount,
                          playbackSpeed,
                          pitchSemitones);
        sourcePosition += inputCount;
        outputPosition += outputCount;
    }
    return result;
}

TrackData makeTransformTestTrack(const juce::AudioBuffer<float>& audio,
                                 double sampleRate,
                                 double playbackSpeed,
                                 double pitchSemitones)
{
    TrackData track;
    track.id = juce::Uuid().toString();
    track.name = "Transform alignment";
    track.playbackSpeed = playbackSpeed;
    track.pitchSemitones = pitchSemitones;
    AudioClip clip;
    clip.id = juce::Uuid().toString();
    clip.sampleRate = static_cast<int>(sampleRate);
    clip.duration = audio.getNumSamples() / sampleRate;
    clip.buffer = std::make_shared<juce::AudioBuffer<float>>(audio);
    track.clips.push_back(std::move(clip));
    return track;
}

juce::AudioBuffer<float> renderEngine(AudioEngine& engine,
                                      int totalSamples,
                                      int blockSize)
{
    juce::AudioBuffer<float> rendered(2, totalSamples);
    rendered.clear();
    juce::AudioBuffer<float> input(2, blockSize);
    input.clear();
    for (int position = 0; position < totalSamples; position += blockSize)
    {
        const int count = std::min(blockSize, totalSamples - position);
        juce::AudioBuffer<float> outputView(rendered.getArrayOfWritePointers(),
                                            2,
                                            position,
                                            count);
        juce::AudioBuffer<float> inputView(input.getArrayOfWritePointers(),
                                           2,
                                           0,
                                           count);
        processStereoBlock(engine, inputView, outputView);
    }
    return rendered;
}

bool testPlaybackTransformRealtimeAlignment()
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;
    constexpr int inputSamples = 144000;
    juce::AudioBuffer<float> source(2, inputSamples);
    source.clear();

    // A finite burst gives a deterministic centre that can be compared with
    // both an untransformed track and the offline renderer.
    const int burstStart = 24000;
    const int burstLength = 4800;
    for (int sample = 0; sample < inputSamples; ++sample)
    {
        float value = 0.0f;
        if (sample >= burstStart && sample < burstStart + burstLength)
        {
            const double phase = static_cast<double>(sample - burstStart)
                               / burstLength;
            const double envelope = std::sin(
                juce::MathConstants<double>::pi * phase);
            value = static_cast<float>(0.35 * envelope
                * std::sin(juce::MathConstants<double>::twoPi
                           * 440.0 * sample / sampleRate));
        }
        source.setSample(0, sample, value);
        source.setSample(1, sample, value);
    }

    auto realtime = renderRealtimeTransform(source,
                                             sampleRate,
                                             1.0,
                                             7.0,
                                             inputSamples,
                                             blockSize);
    auto offline = PlaybackTransform::processOffline(source,
                                                      sampleRate,
                                                      1.0,
                                                      7.0);
    const double expectedCentroid = burstStart + burstLength * 0.5;
    const double realtimeCentroid = getEnergyCentroid(*realtime);
    const double offlineCentroid = offline != nullptr
        ? getEnergyCentroid(*offline) : -1.0;
    bool ok = expect(std::abs(realtimeCentroid - expectedCentroid) < 320.0,
                     "realtime transform should compensate its start latency")
           && expect(offline != nullptr
                         && std::abs(offlineCentroid - realtimeCentroid) < 320.0,
                     "realtime and offline transforms should keep the same timing");

    AudioEngine plainEngine;
    plainEngine.prepare(blockSize, sampleRate);
    plainEngine.replaceTracks({ makeTransformTestTrack(source,
                                                        sampleRate,
                                                        1.0,
                                                        0.0) });
    plainEngine.play();
    auto plain = renderEngine(plainEngine, inputSamples, blockSize);
    plainEngine.stop();

    AudioEngine pitchedEngine;
    pitchedEngine.prepare(blockSize, sampleRate);
    pitchedEngine.replaceTracks({ makeTransformTestTrack(source,
                                                          sampleRate,
                                                          1.0,
                                                          7.0) });
    pitchedEngine.play();
    auto pitched = renderEngine(pitchedEngine, inputSamples, blockSize);
    pitchedEngine.stop();
    ok = expect(std::abs(getEnergyCentroid(plain)
                         - getEnergyCentroid(pitched)) < 320.0,
                "transformed and untransformed tracks should stay aligned") && ok;

    // Distinct source regions make it possible to prove that a live seek and
    // a loop wrap re-prime from the requested position instead of leaking the
    // transform's previous history.
    juce::AudioBuffer<float> regionSource(2, inputSamples);
    for (int sample = 0; sample < inputSamples; ++sample)
    {
        const double frequency = sample < static_cast<int>(sampleRate)
            ? 220.0 : 660.0;
        const float value = static_cast<float>(0.3
            * std::sin(juce::MathConstants<double>::twoPi
                       * frequency * sample / sampleRate));
        regionSource.setSample(0, sample, value);
        regionSource.setSample(1, sample, value);
    }

    AudioEngine seekEngine;
    seekEngine.prepare(blockSize, sampleRate);
    seekEngine.replaceTracks({ makeTransformTestTrack(regionSource,
                                                       sampleRate,
                                                       1.25,
                                                       0.0) });
    seekEngine.setCurrentTime(1.0);
    seekEngine.play();
    auto seekOutput = renderEngine(seekEngine, 8192, blockSize);
    const double seekFrequency = estimateFrequency(seekOutput,
                                                   sampleRate,
                                                   3000,
                                                   7500);
    ok = expect(seekOutput.getMagnitude(0, 0, blockSize) > 0.01f,
                "seeked transformed playback should not start with latency silence") && ok;
    ok = expect(std::abs(seekFrequency - 660.0) < 20.0,
                "seeked transformed playback should read the requested source region") && ok;
    seekEngine.stop();

    ProjectDawSettings loopSettings;
    loopSettings.hasLoopStart = true;
    loopSettings.hasLoopEnd = true;
    loopSettings.loopStartSeconds = 0.4;
    loopSettings.loopEndSeconds = 1.2;
    loopSettings.loopEnabled = true;
    seekEngine.setProjectDawSettings(loopSettings, false);
    const int samplesBeforeWrap = 100;
    seekEngine.setCurrentTime(loopSettings.loopEndSeconds
                              - samplesBeforeWrap / sampleRate);
    seekEngine.play();
    auto loopedTransform = renderEngine(seekEngine, 8192, blockSize);
    const double loopFrequency = estimateFrequency(loopedTransform,
                                                   sampleRate,
                                                   3000,
                                                   7500);
    const double expectedLoopTime = loopSettings.loopStartSeconds
        + (8192 - samplesBeforeWrap) / sampleRate;
    ok = expect(std::abs(loopFrequency - 220.0) < 20.0,
                "looped transformed playback should re-prime at point A") && ok;
    ok = expect(std::abs(seekEngine.getCurrentTime() - expectedLoopTime)
                    < 1.0 / sampleRate,
                "loop crossfade must not change transport length or position") && ok;
    seekEngine.stop();

    AudioEngine fadeEngine;
    fadeEngine.prepare(blockSize, sampleRate);
    juce::AudioBuffer<float> stepSource(2, inputSamples);
    for (int sample = 0; sample < inputSamples; ++sample)
    {
        const float value = sample < static_cast<int>(0.8 * sampleRate)
            ? -0.6f : 0.6f;
        stepSource.setSample(0, sample, value);
        stepSource.setSample(1, sample, value);
    }
    fadeEngine.replaceTracks({ makeTransformTestTrack(stepSource,
                                                       sampleRate,
                                                       1.0,
                                                       0.0) });
    ProjectDawSettings fadeSettings;
    fadeSettings.hasLoopStart = true;
    fadeSettings.hasLoopEnd = true;
    fadeSettings.loopStartSeconds = 0.4;
    fadeSettings.loopEndSeconds = 1.2;
    fadeSettings.loopEnabled = true;
    fadeEngine.setProjectDawSettings(fadeSettings, false);
    const int samplesBeforeCallbackWrap = blockSize;
    fadeEngine.setCurrentTime(fadeSettings.loopEndSeconds
                              - samplesBeforeCallbackWrap / sampleRate);
    fadeEngine.play();
    auto fadedBoundary = renderEngine(fadeEngine, blockSize * 2, blockSize);
    float maximumBoundaryJump = 0.0f;
    for (int sample = samplesBeforeCallbackWrap - 20;
         sample < samplesBeforeCallbackWrap + 20;
         ++sample)
        maximumBoundaryJump = std::max(maximumBoundaryJump,
            std::abs(fadedBoundary.getSample(0, sample)
                     - fadedBoundary.getSample(0, sample - 1)));
    ok = expect(maximumBoundaryJump < 0.03f,
                "loop boundary fade should suppress a hard polarity click") && ok;
    ok = expect(fadedBoundary.getMagnitude(0,
                                            samplesBeforeCallbackWrap + 300,
                                            100) > 0.2f,
                "loop fade-in should continue correctly in the next callback") && ok;
    fadeEngine.stop();

    return ok;
}

bool testIndependentPitchAndPlaybackSpeed()
{
    constexpr int blockSize = 512;
    constexpr double sampleRate = 48000.0;

    AudioEngine engine;
    engine.prepare(blockSize, sampleRate);

    TrackData track;
    track.id = "transform-track";
    track.name = "Transform";
    AudioClip clip;
    clip.id = "transform-clip";
    clip.sampleRate = static_cast<int>(sampleRate);
    clip.duration = 1.0;
    clip.buffer = std::make_shared<juce::AudioBuffer<float>>(2,
                                                             static_cast<int>(sampleRate));
    for (int channel = 0; channel < 2; ++channel)
    {
        for (int sample = 0; sample < clip.buffer->getNumSamples(); ++sample)
        {
            clip.buffer->setSample(channel,
                                   sample,
                                   std::sin(juce::MathConstants<double>::twoPi
                                            * 440.0 * sample / sampleRate)
                                       * 0.25f);
        }
    }
    track.clips.push_back(std::move(clip));
    engine.replaceTracks({ std::move(track) });
    engine.addTrack("Unchanged");

    engine.setTrackPitchSemitones(0, 20.0);
    engine.setTrackPlaybackSpeed(0, 2.0);
    const auto clampedTracks = engine.getTracksSnapshot();
    const bool clamped = expect(clampedTracks.size() == 2
                                    && clampedTracks[0].pitchSemitones == 12.0,
                                "track key should clamp to the supported octave")
                         && expect(clampedTracks[0].playbackSpeed == 1.5,
                                   "track speed should clamp to the supported range")
                         && expect(clampedTracks[1].pitchSemitones == 0.0
                                       && clampedTracks[1].playbackSpeed == 1.0,
                                   "changing one track should not affect another track")
                         && expect(std::abs(engine.getDuration() - (1.0 / 1.5))
                                       < 1.0e-9,
                                   "track speed should update the visible project duration");

    engine.setTrackPitchSemitones(0, 3.0);
    engine.setTrackPlaybackSpeed(0, 1.5);
    engine.play();

    juce::AudioBuffer<float> input(2, blockSize);
    juce::AudioBuffer<float> output(2, blockSize);
    input.clear();
    processStereoBlock(engine, input, output);

    const double expectedTime = blockSize / sampleRate;
    const bool globalTimelineUnchanged = expect(
        std::abs(engine.getCurrentTime() - expectedTime) < 1.0e-9,
        "per-track speed should not change the global timeline speed");
    engine.stop();

    return clamped && globalTimelineUnchanged
        && testPlaybackTransformOffline()
        && testPlaybackTransformRealtimeAlignment();
}
}

int main(int argc, char* argv[])
{
#if JUCE_WINDOWS
    // 自動テストが失敗してもWindowsのモーダルエラー画面を残さず、
    // 終了コードとクラッシュダンプで確実に検出できるようにする。
    ::SetErrorMode(SEM_FAILCRITICALERRORS
                   | SEM_NOGPFAULTERRORBOX
                   | SEM_NOOPENFILEERRORBOX);
#endif
    juce::ScopedJuceInitialiser_GUI initialiseJuce;

    if (argc == 3 && juce::String(argv[1]) == "--assistant-host")
        return runAssistantIntegrationHost(juce::File(argv[2]));
    if (argc == 2 && juce::String(argv[1]) == "--test-assistant")
        return runAssistantControllerTests() && runRegionAutomationTests() ? 0 : 1;

    if ((argc == 2 || argc == 3) && juce::String(argv[1]) == "--test-ui-layout")
        return runUiLayoutTests(argc == 3 ? juce::File(argv[2]) : juce::File()) ? 0 : 1;

    if (argc == 4 && juce::String(argv[1]) == "--benchmark-project")
        return runProjectFormatBenchmark(juce::File(argv[2]), juce::File(argv[3]));
    if (argc == 3 && juce::String(argv[1]) == "--inspect-pitchnet-project")
        return inspectPitchNetProjectArchive(juce::File(argv[2]));
    if (argc == 2 && juce::String(argv[1]) == "--test-project-round-trip")
        return testProjectRoundTrip() ? 0 : 1;
    if (argc == 2 && juce::String(argv[1]) == "--test-project-schema")
        return testProjectSchemaAndRhythmMarkerValidation() ? 0 : 1;
    if (argc == 2 && juce::String(argv[1]) == "--test-nondestructive-edit")
        return testNonDestructiveEditUndoRedo() ? 0 : 1;
    if (argc == 2 && juce::String(argv[1]) == "--test-export-pipeline")
        return testExportPipeline() ? 0 : 1;
    if (argc == 2 && juce::String(argv[1]) == "--test-ara-timeline")
        return runAraTimelineAudioTests() ? 0 : 1;
    if (argc == 2 && juce::String(argv[1]) == "--test-playback-transform")
        return testPlaybackTransformOffline()
            && testPlaybackTransformRealtimeAlignment() ? 0 : 1;
    if (argc == 2 && juce::String(argv[1]) == "--test-pitchnet-state")
        return testPitchCorrectionRuntimeStateConsistency() ? 0 : 1;
    if (argc == 2 && juce::String(argv[1]) == "--test-daw-features")
        return runDawFeatureTests() ? 0 : 1;

    bool ok = true;
    ok = testRoundTripLatencyAndStereo() && ok;
    ok = testMeasuredLatencyOverridesDeviceEstimate() && ok;
    ok = testInputMeterUpdatesDuringCalibrationBypass() && ok;
    ok = testLatencyTrimAtTimelineStart() && ok;
    ok = testStereoAfterInitialMissingInput() && ok;
    ok = testSampleRateConversion() && ok;
    ok = testDeviceRateSwitchDoesNotResampleAgain() && ok;
    ok = testCachedDurationAndContentRevision() && ok;
    ok = testNewAraPluginUsesCurrentTimelineFormat() && ok;
    ok = testProjectRoundTrip() && ok;
    ok = testLegacyProjectDefaultsAraMetadata() && ok;
    ok = testProjectSchemaAndRhythmMarkerValidation() && ok;
    ok = testProjectReplacementClearsUndoHistory() && ok;
    ok = testPitchCorrectionRuntimeStateConsistency() && ok;
    ok = testNonDestructiveEditUndoRedo() && ok;
    ok = testExportPipeline() && ok;
    ok = testRecordingAcrossChunkBoundary() && ok;
    ok = testLongRecordingPreviewKeepsWholeTake() && ok;
    ok = testTempoPointInsertionPreservesEarlierPoint() && ok;
    ok = testRhythmQuantizeUsesCumulativeTempoMap() && ok;
    ok = testTempoMapCountInUsesCurrentSegment() && ok;
    ok = testPlaybackMetronomeUsesTempoMap() && ok;
    ok = testCountInBoundary(6, 8, 72000) && ok;
    ok = testCountInBoundary(7, 8, 84000) && ok;
    ok = testIndependentPitchAndPlaybackSpeed() && ok;
    ok = runAraTimelineAudioTests() && ok;
    ok = runLatencyCalibrationTests() && ok;
    ok = runPitchNetArchiveFilterTests() && ok;
    ok = runDawFeatureTests() && ok;
    ok = runRegionAutomationTests() && ok;
    ok = runAssistantControllerTests() && ok;
    ok = runUiLayoutTests({}) && ok;

    if (ok)
        std::cout << "All deterministic audio-engine tests passed." << std::endl;
    return ok ? 0 : 1;
}
