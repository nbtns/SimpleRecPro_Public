#include "../Source/AutoMixAnalyzer.h"
#include "../Source/ClipRenderMath.h"
#include "../Source/MasterBusProcessor.h"
#include "../Source/MetronomeEngine.h"
#include "../Source/RhythmRender.h"
#include "../Source/RhythmWarp.h"
#include "../Source/ReferenceMixAnalyzer.h"
#include "../Source/SimpleMixProcessor.h"
#include "../Source/SpecialFxProcessor.h"
#include "../Source/TempoEstimator.h"
#include "../Source/TempoMap.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace
{
bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAILED: " << message << std::endl;
    return condition;
}

bool near(double left, double right, double tolerance = 1.0e-6)
{
    return std::abs(left - right) <= tolerance;
}

bool testClipFadeAndPanMath()
{
    const auto fadeStart = ClipRenderMath::getFadeGain(0.0, 4.0, 1.0, 1.0);
    const auto fadeMiddle = ClipRenderMath::getFadeGain(0.5, 4.0, 1.0, 1.0);
    const auto fullLevel = ClipRenderMath::getFadeGain(1.0, 4.0, 1.0, 1.0);
    const auto fadeEnd = ClipRenderMath::getFadeGain(4.0, 4.0, 1.0, 1.0);
    const auto overlappingMiddle = ClipRenderMath::getFadeGain(2.0,
                                                               4.0,
                                                               4.0,
                                                               4.0);
    const auto invalid = ClipRenderMath::getFadeGain(
        std::numeric_limits<double>::quiet_NaN(), 4.0, 1.0, 1.0);

    const auto hardLeft = ClipRenderMath::getConstantPowerPanGains(-1.0f);
    const auto centre = ClipRenderMath::getConstantPowerPanGains(0.0f);
    const auto hardRight = ClipRenderMath::getConstantPowerPanGains(1.0f);

    return expect(near(fadeStart, 0.0) && near(fadeEnd, 0.0),
                  "fade endpoints should be silent")
        && expect(near(fadeMiddle, std::sqrt(0.5), 1.0e-5),
                  "equal-power fade midpoint should be deterministic")
        && expect(near(fullLevel, 1.0) && near(overlappingMiddle, 1.0),
                  "fade handles should reach full level after overlap normalisation")
        && expect(near(invalid, 0.0),
                  "non-finite fade input should be silent")
        && expect(near(hardLeft.left, 1.0) && near(hardLeft.right, 0.0),
                  "hard-left pan should remove the right channel")
        && expect(near(hardRight.left, 0.0) && near(hardRight.right, 1.0),
                  "hard-right pan should remove the left channel")
        && expect(near(centre.left, centre.right)
                      && near(centre.left * centre.left
                                  + centre.right * centre.right,
                              1.0,
                              1.0e-5),
                  "centre pan should use a constant-power law");
}

bool testRangeAndLoopMath()
{
    const auto reversed = ClipRenderMath::normaliseRange(8.0, 2.0, 5.0);
    const auto minimum = ClipRenderMath::normaliseRange(4.9, 4.9, 5.0, 2.0);
    const auto invalid = ClipRenderMath::normaliseRange(
        0.0,
        std::numeric_limits<double>::infinity(),
        5.0);

    const auto boundary = ClipRenderMath::decideLoopSegment(190, 32, 100, 200);
    const auto afterEnd = ClipRenderMath::decideLoopSegment(250, 20, 100, 200);
    const auto linear = ClipRenderMath::decideLoopSegment(50, 32, 200, 100);

    return expect(reversed.valid && near(reversed.startSeconds, 2.0)
                      && near(reversed.endSeconds, 5.0),
                  "A/B points should be ordered and clamped")
        && expect(minimum.valid && near(minimum.startSeconds, 3.0)
                      && near(minimum.endSeconds, 5.0),
                  "minimum A/B length should move safely inside the timeline")
        && expect(!invalid.valid,
                  "non-finite A/B points should be rejected")
        && expect(ClipRenderMath::wrapLoopPosition(200, 100, 200) == 100
                      && ClipRenderMath::wrapLoopPosition(250, 100, 200) == 150
                      && ClipRenderMath::wrapLoopPosition(50, 100, 200) == 50,
                  "loop wrapping should preserve phase inside the range")
        && expect(boundary.loopIsValid && boundary.samplesToRender == 10
                      && boundary.nextSample == 100 && boundary.wrapped,
                  "a render block should split exactly at the loop end")
        && expect(afterEnd.startSample == 150 && afterEnd.nextSample == 170
                      && afterEnd.wrapped,
                  "transport beyond B should wrap before rendering")
        && expect(!linear.loopIsValid && linear.samplesToRender == 32
                      && linear.nextSample == 82 && !linear.wrapped,
                  "invalid loops should fall back to linear transport");
}

bool testTempoMapNormalisationAndLookup()
{
    TempoMap defaults;
    const auto defaultPoint = defaults.getPointAt(100.0);

    TempoMap unordered({
        { 10.0, 60.0, 3, 4 },
        { 5.0, 150.0, 5, 16 },
        { 5.0, 90.0, 7, 8 },
        { std::numeric_limits<double>::quiet_NaN(), 200.0, 9, 8 }
    });
    const auto& points = unordered.getPoints();

    TempoMap sanitised({ { -2.0, 999.0, 0, 3 } });
    const auto sanitisedPoint = sanitised.getPointAt(0.0);

    return expect(near(defaultPoint.bpm, 120.0)
                      && defaultPoint.numerator == 4
                      && defaultPoint.denominator == 4,
                  "an empty tempo map should default to 120 BPM in 4/4")
        && expect(points.size() == 3
                      && near(points[0].timeSeconds, 0.0)
                      && near(points[1].timeSeconds, 5.0)
                      && near(points[2].timeSeconds, 10.0),
                  "tempo points should be sorted with a zero-time default")
        && expect(near(unordered.getBpmAt(5.0), 90.0)
                      && unordered.getTimeSignatureAt(5.0)
                             == std::pair<int, int> { 7, 8 },
                  "the final duplicate tempo point should win deterministically")
        && expect(near(unordered.getBpmAt(9.999), 90.0)
                      && near(unordered.getBpmAt(10.0), 60.0),
                  "tempo lookup should switch exactly at a point")
        && expect(near(sanitisedPoint.timeSeconds, 0.0)
                      && near(sanitisedPoint.bpm, 300.0)
                      && sanitisedPoint.numerator == 1
                      && sanitisedPoint.denominator == 4,
                  "invalid tempo values should be clamped to safe limits");
}

bool testTempoMapConversionsAndBeatPositions()
{
    TempoMap map({
        { 0.0, 120.0, 4, 4 },
        { 2.0, 60.0, 4, 4 },
        { 4.0, 60.0, 6, 8 }
    });

    const double beatsAtTwo = map.secondsToBeats(2.0);
    const double beatsAtFour = map.secondsToBeats(4.0);
    const double beatsAtFive = map.secondsToBeats(5.0);
    const double secondsAtEightBeats = map.beatsToSeconds(8.0);

    TempoMap constant;
    const auto positions = constant.getBeatPositions(0.25, 1.25);
    const auto reversed = constant.getBeatPositions(2.0, 1.0);

    TempoMap displacedChange({
        { 0.0, 120.0, 4, 4 },
        { 2.25, 60.0, 4, 4 },
        { 4.25, 60.0, 3, 4 }
    });
    const auto afterTempoChange = displacedChange.getBeatPosition(2.75);
    const auto afterMeterChange = displacedChange.getBeatPosition(4.75);

    bool roundTrips = true;
    for (const double seconds : { 0.0, 0.25, 1.5, 2.0, 3.25, 4.0, 5.0 })
        roundTrips = roundTrips
            && near(map.beatsToSeconds(map.secondsToBeats(seconds)),
                    seconds,
                    1.0e-8);

    return expect(near(beatsAtTwo, 4.0)
                      && near(beatsAtFour, 6.0)
                      && near(beatsAtFive, 8.0),
                  "seconds-to-beats should integrate tempo and meter segments")
        && expect(near(secondsAtEightBeats, 5.0) && roundTrips,
                  "beat conversion should be invertible across map changes")
        && expect(near(constant.snapToNextBeat(0.1), 0.5)
                      && near(constant.snapToNextBeat(0.5), 0.5),
                  "snap should choose the first beat at or after the cursor")
        && expect(near(displacedChange.snapToNearestBeat(2.60), 2.75)
                      && afterTempoChange.beatIndex == 5
                      && afterTempoChange.beatInBar == 1,
                  "nearest snap and bar phase should use cumulative beats after a tempo change")
        && expect(afterMeterChange.beatIndex == 7
                      && afterMeterChange.beatInBar == 0
                      && afterMeterChange.barIndex == 2,
                  "a meter change should start its first bar on the cumulative beat grid")
        && expect(positions.size() == 2
                      && near(positions[0], 0.5)
                      && near(positions[1], 1.0),
                  "beat enumeration should honour both range boundaries")
        && expect(reversed.empty(),
                  "reversed beat ranges should be rejected");
}

bool testMetronomeUsesCumulativeTempoMap()
{
    constexpr double sampleRate = 1000.0;
    constexpr int64_t blockStart = 2400;
    TempoMap map({
        { 0.0, 120.0, 4, 4 },
        { 2.25, 60.0, 4, 4 }
    });

    MetronomeEngine metronome;
    metronome.prepare(sampleRate);
    metronome.setEnabled(true);
    metronome.setVolume(1.0f);

    juce::AudioBuffer<float> output(1, 700);
    output.clear();
    metronome.processBlock(output, output.getNumSamples(), blockStart, map);

    // The cumulative fifth beat is at 2.75 s. A local-origin calculation
    // would incorrectly click at 3.0 s after this off-grid tempo point.
    return expect(output.getMagnitude(0, 350, 8) > 0.1f,
                  "metronome should click on the cumulative beat after a tempo change")
        && expect(output.getMagnitude(0, 598, 8) < 1.0e-6f,
                  "metronome should not restart beat phase at the new BPM");
}

bool testTempoEstimatorDeterminismAndGuards()
{
    constexpr double sampleRate = 8000.0;
    constexpr int durationSeconds = 10;
    constexpr int clickInterval = static_cast<int>(sampleRate * 0.5); // 120 BPM

    juce::AudioBuffer<float> clickTrack(1,
                                        static_cast<int>(sampleRate)
                                            * durationSeconds);
    clickTrack.clear();
    for (int beat = 0, start = clickInterval;
         start < clickTrack.getNumSamples();
         ++beat, start += clickInterval)
    {
        const float accent = beat % 4 == 0 ? 1.0f : 0.7f;
        for (int sample = 0; sample < 80
             && start + sample < clickTrack.getNumSamples(); ++sample)
        {
            clickTrack.setSample(0,
                                 start + sample,
                                 accent * static_cast<float>(
                                     std::exp(-sample / 16.0)));
        }
    }

    const auto first = TempoEstimator::estimate(clickTrack, sampleRate, 5);
    const auto second = TempoEstimator::estimate(clickTrack, sampleRate, 5);

    const auto has120BpmCandidate = [](const std::vector<TempoCandidate>& candidates)
    {
        for (const auto& candidate : candidates)
            if (std::abs(candidate.bpm - 120.0) <= 2.0)
                return true;
        return false;
    };

    bool orderedAndBounded = first.size() <= 5;
    for (std::size_t index = 0; index < first.size(); ++index)
    {
        orderedAndBounded = orderedAndBounded
            && first[index].bpm >= 60.0 && first[index].bpm <= 200.0
            && first[index].confidence >= 0.0
            && first[index].confidence <= 1.0;
        if (index > 0)
            orderedAndBounded = orderedAndBounded
                && first[index - 1].confidence >= first[index].confidence;
    }

    bool deterministic = first.size() == second.size();
    for (std::size_t index = 0;
         deterministic && index < first.size(); ++index)
    {
        deterministic = near(first[index].bpm, second[index].bpm, 1.0e-12)
            && near(first[index].confidence,
                    second[index].confidence,
                    1.0e-12);
    }

    juce::AudioBuffer<float> silence(2, static_cast<int>(sampleRate * 4.0));
    silence.clear();
    juce::AudioBuffer<float> tooShort(1, 64);
    tooShort.clear();

    return expect(!first.empty() && has120BpmCandidate(first),
                  "tempo estimation should find a 120 BPM click track")
        && expect(orderedAndBounded,
                  "tempo candidates should be bounded and confidence-sorted")
        && expect(deterministic,
                  "tempo estimation should return identical candidates for identical audio")
        && expect(TempoEstimator::estimate(clickTrack, sampleRate, 1).size() <= 1,
                  "tempo estimation should honour the candidate limit")
        && expect(TempoEstimator::estimate(silence, sampleRate).empty(),
                  "silence should not produce a tempo candidate")
        && expect(TempoEstimator::estimate(tooShort, sampleRate).empty(),
                  "audio that is too short should not produce a tempo candidate")
        && expect(TempoEstimator::estimate(clickTrack, 0.0).empty(),
                  "an invalid sample rate should be rejected");
}

bool testEditableRhythmNotesAndWarpMapping()
{
    std::vector<RhythmMarker> markers {
        { 3.0, 4.0, 4.0, 5.5 },
        { 1.0, 2.0, 2.0, 4.0 },
        { 1.0002, 2.0002, 1.5, 2.5 },
        { 7.0, 7.0, 9.0, 9.0 }
    };
    const auto sanitised = RhythmWarp::sanitise(std::move(markers), 6.0);
    if (!expect(sanitised.size() == 2,
                "rhythm notes should be ordered, bounded and deduplicated"))
        return false;

    const auto& first = sanitised[0];
    const auto& second = sanitised[1];
    const bool rangesAreEditable = first.hasDuration() && second.hasDuration()
        && near(first.sourceTime, 1.0) && near(first.targetTime, 2.0)
        && near(first.sourceEndTime, 2.0) && near(first.targetEndTime, 3.999)
        && near(second.targetEndTime, 5.5);

    const double beforeNote = RhythmWarp::mapTargetToSource(
        sanitised, 6.0, 1.0);
    const double insideNote = RhythmWarp::mapTargetToSource(
        sanitised, 6.0, 3.0);
    const double atMovedEnd = RhythmWarp::mapTargetToSource(
        sanitised, 6.0, first.targetEndTime);
    const double finalSample = RhythmWarp::mapTargetToSource(
        sanitised, 6.0, 6.0);

    const auto legacyPoint = RhythmWarp::sanitise(
        { RhythmMarker { 2.0, 1.0 } }, 4.0);
    const bool legacyStillWorks = legacyPoint.size() == 1
        && !legacyPoint.front().hasDuration()
        && near(RhythmWarp::mapTargetToSource(legacyPoint, 4.0, 1.0), 2.0);
    const bool noteCanMoveToClipStart = near(
        RhythmWarp::mapTargetToSource(
            { RhythmMarker { 0.5, 0.0, 1.0, 0.5 } }, 4.0, 0.0),
        0.5);

    return expect(rangesAreEditable,
                  "detected notes should retain editable start and end anchors")
        && expect(near(beforeNote, 0.5)
                      && near(insideNote, 1.5, 0.001)
                      && near(atMovedEnd, 2.0)
                      && near(finalSample, 6.0),
                  "note movement and length edits should map deterministically")
        && expect(legacyStillWorks,
                  "old point-only rhythm markers should remain compatible")
        && expect(noteCanMoveToClipStart,
                  "an editable note should be movable to the clip start");
}

double estimatePositiveCrossingFrequency(const juce::AudioBuffer<float>& audio,
                                         double sampleRate,
                                         double startSeconds,
                                         double durationSeconds)
{
    if (audio.getNumChannels() <= 0 || sampleRate <= 0.0)
        return 0.0;

    const int start = juce::jlimit(
        1, audio.getNumSamples(), juce::roundToInt(startSeconds * sampleRate));
    const int end = juce::jlimit(
        start, audio.getNumSamples(),
        juce::roundToInt((startSeconds + durationSeconds) * sampleRate));
    int crossings = 0;
    for (int sample = start; sample < end; ++sample)
        if (audio.getSample(0, sample - 1) <= 0.0f
            && audio.getSample(0, sample) > 0.0f)
            ++crossings;

    return durationSeconds > 0.0 ? crossings / durationSeconds : 0.0;
}

bool testRhythmRenderPreservesPitch()
{
    constexpr double sampleRate = 48000.0;
    constexpr double frequency = 440.0;
    constexpr double duration = 1.0;

    AudioClip clip;
    clip.id = "rhythm-pitch-fixture";
    clip.sampleRate = static_cast<int>(sampleRate);
    clip.duration = duration;
    clip.buffer = std::make_shared<juce::AudioBuffer<float>>(
        1, static_cast<int>(sampleRate * duration));
    for (int sample = 0; sample < clip.buffer->getNumSamples(); ++sample)
        clip.buffer->setSample(
            0, sample,
            0.5f * static_cast<float>(std::sin(
                juce::MathConstants<double>::twoPi * frequency
                * sample / sampleRate)));

    // Stretch 200 ms of source audio to 400 ms. Resampling would halve the
    // pitch here; the rendered cache must keep the musical note at A4.
    clip.rhythmMarkers.push_back({ 0.2, 0.2, 0.4, 0.6 });
    const auto cache = RhythmRender::render(clip);
    if (!expect(cache != nullptr && cache->matches(clip),
                "rhythm render should build a matching immutable cache"))
        return false;

    const double stretchedFrequency = estimatePositiveCrossingFrequency(
        cache->buffer, sampleRate, 0.27, 0.26);
    const auto originalSignature = cache->signature;
    clip.rhythmMarkers.front().targetEndTime = 0.55;

    return expect(std::abs(stretchedFrequency - frequency) < 12.0,
                  "rhythm stretching should preserve the source pitch")
        && expect(originalSignature != RhythmRender::signatureFor(clip)
                      && !cache->matches(clip),
                  "editing a rhythm note should invalidate the old cache");
}

bool testSimpleMixIsSafeAndBlockSizeIndependent()
{
    constexpr double sampleRate = 48000.0;
    constexpr int sampleCount = 24000;
    juce::AudioBuffer<float> source(2, sampleCount);
    for (int sample = 0; sample < sampleCount; ++sample)
    {
        const double time = sample / sampleRate;
        const float envelope = sample < 16000 ? 0.3f : 0.002f;
        const float value = envelope * static_cast<float>(
            std::sin(2.0 * juce::MathConstants<double>::pi * 6400.0 * time));
        source.setSample(0, sample, value);
        source.setSample(1, sample, value * 0.8f);
    }

    auto whole = source;
    auto chunked = source;
    SimpleMixProcessor wholeProcessor;
    SimpleMixProcessor chunkedProcessor;
    if (!expect(wholeProcessor.prepare(sampleRate, 8192, 2)
                    && chunkedProcessor.prepare(sampleRate, 8192, 2),
                "simple mix should prepare for stereo processing"))
        return false;

    auto settings = SimpleMixProcessor::makePresetSettings(MixPreset::clear);
    settings.ambience = 0.25f;
    NoiseReductionSettings noise;
    noise.enabled = true;
    noise.amount = 0.75f;
    noise.deEssAmount = 0.8f;
    wholeProcessor.setSettings(settings);
    chunkedProcessor.setSettings(settings);
    wholeProcessor.setNoiseReductionSettings(noise);
    chunkedProcessor.setNoiseReductionSettings(noise);
    wholeProcessor.process(whole);
    for (int position = 0; position < sampleCount; position += 257)
        chunkedProcessor.process(chunked, position,
                                 std::min(257, sampleCount - position));

    float maximumDifference = 0.0f;
    float processedPeak = 0.0f;
    bool allFinite = true;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < sampleCount; ++sample)
        {
            const auto value = whole.getSample(channel, sample);
            maximumDifference = std::max(
                maximumDifference,
                std::abs(value - chunked.getSample(channel, sample)));
            processedPeak = std::max(processedPeak, std::abs(value));
            allFinite = allFinite && std::isfinite(value);
        }

    SimpleMixProcessor bypass;
    if (!expect(bypass.prepare(sampleRate, 512, 2),
                "simple mix bypass should prepare"))
        return false;
    auto unchanged = source;
    bypass.process(unchanged);
    bool bypassIsExact = true;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < sampleCount; ++sample)
            bypassIsExact = bypassIsExact
                && unchanged.getSample(channel, sample)
                    == source.getSample(channel, sample);

    juce::AudioBuffer<float> quietNoise(2, sampleCount);
    for (int sample = 0; sample < sampleCount; ++sample)
    {
        const float value = 0.002f * static_cast<float>(std::sin(
            2.0 * juce::MathConstants<double>::pi * 6000.0
            * sample / sampleRate));
        quietNoise.setSample(0, sample, value);
        quietNoise.setSample(1, sample, value);
    }
    const float quietInputRms = quietNoise.getRMSLevel(0,
                                                       sampleCount - 4096,
                                                       4096);
    SimpleMixProcessor noiseOnly;
    noiseOnly.prepare(sampleRate, 512, 2);
    NoiseReductionSettings gateOnly;
    gateOnly.enabled = true;
    gateOnly.amount = 1.0f;
    gateOnly.deEssAmount = 0.0f;
    noiseOnly.setNoiseReductionSettings(gateOnly);
    noiseOnly.process(quietNoise);
    const float quietOutputRms = quietNoise.getRMSLevel(0,
                                                        sampleCount - 4096,
                                                        4096);

    return expect(maximumDifference < 1.0e-6f,
                  "simple mix should sound identical across realtime and offline block sizes")
        && expect(allFinite && processedPeak <= 1.0f,
                  "mix, gate and de-ess processing should remain finite and bounded")
        && expect(quietOutputRms < quietInputRms * 0.5f,
                  "noise reduction should attenuate a quiet high-frequency tail")
        && expect(bypassIsExact,
                  "disabled simple mix and noise reduction should be bit-exact");
}

void fillConstant(juce::AudioBuffer<float>& buffer, float value)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            buffer.setSample(channel, sample, value);
}

bool testMasterBusReportsInputAndAudibleOutputLoudness()
{
    constexpr double sampleRate = 48000.0;
    MasterBusProcessor processor;
    if (!expect(processor.prepare(sampleRate, 512, 2),
                "master bus should accept a normal stereo configuration"))
        return false;

    MasteringSettings settings;
    settings.enabled = true;
    settings.limiterEnabled = false;
    settings.targetLufs = -14.0f;
    settings.auditionProcessed = true;
    processor.setSettings(settings);

    juce::AudioBuffer<float> audio(2, static_cast<int>(sampleRate * 2.0));
    fillConstant(audio, 0.1f);
    processor.process(audio);

    const auto inputLufs = processor.getApproximateInputLoudnessLufs();
    const auto processedLufs =
        processor.getApproximateProcessedLoudnessLufs();
    const auto outputLufs = processor.getApproximateLoudnessLufs();
    const auto tailLufs = MasterBusProcessor::estimateApproximateLoudnessLufs(
        audio,
        audio.getNumSamples() - static_cast<int>(sampleRate * 0.25),
        static_cast<int>(sampleRate * 0.25));

    processor.reset();
    settings.limiterEnabled = true;
    settings.ceilingDb = -12.0f;
    settings.targetLufs = -8.0f;
    processor.setSettings(settings);
    juce::AudioBuffer<float> limitedAudio(
        2,
        static_cast<int>(sampleRate * 2.0));
    fillConstant(limitedAudio, 1.0f);
    processor.process(limitedAudio);
    const auto preLimiterLufs =
        processor.getApproximateProcessedLoudnessLufs();
    const auto limitedOutputLufs = processor.getApproximateLoudnessLufs();

    return expect(inputLufs > -21.1f && inputLufs < -20.3f,
                  "input loudness should describe the signal before mastering")
        && expect(outputLufs > inputLufs + 4.0f
                      && outputLufs > -16.0f && outputLufs < -12.5f,
                  "output loudness should describe the mastered signal")
        && expect(std::abs(processedLufs - outputLufs) < 0.1f,
                  "processed and output guides should agree before level matching")
        && expect(std::abs(outputLufs - tailLufs) < 1.0f,
                  "the published output guide should follow the audible tail")
        && expect(limitedOutputLufs < preLimiterLufs - 3.0f
                      && limitedOutputLufs > -13.5f
                      && limitedOutputLufs < -11.5f
                      && processor.getLastOutputPeak() <= 0.252f,
                  "output loudness should include the audible limiter result");
}

bool testMasterBusComparisonGainIsSmoothedAndResettable()
{
    constexpr double sampleRate = 48000.0;
    MasterBusProcessor processor;
    if (!expect(processor.prepare(sampleRate, 512, 2),
                "master bus comparison test should prepare"))
        return false;

    MasteringSettings settings;
    settings.enabled = true;
    settings.limiterEnabled = false;
    settings.targetLufs = -20.691f;
    settings.auditionProcessed = true;
    processor.setSettings(settings);

    juce::AudioBuffer<float> warmup(2, static_cast<int>(sampleRate * 2.0));
    fillConstant(warmup, 0.1f);
    processor.process(warmup);
    const auto levelBeforeMatch = warmup.getSample(0,
                                                   warmup.getNumSamples() - 1);

    processor.setComparisonLoudness(-20.0f, -8.0f);
    juce::AudioBuffer<float> transition(2, 512);
    fillConstant(transition, 0.1f);
    processor.process(transition);
    const auto firstMatchedSample = transition.getSample(0, 0);
    const auto lastTransitionSample = transition.getSample(
        0,
        transition.getNumSamples() - 1);

    juce::AudioBuffer<float> settled(2, static_cast<int>(sampleRate * 2.0));
    fillConstant(settled, 0.1f);
    processor.process(settled);
    const auto settledMatchedSample = settled.getSample(
        0,
        settled.getNumSamples() - 1);
    const auto processedLufsDuringMatch =
        processor.getApproximateProcessedLoudnessLufs();
    const auto outputLufsDuringMatch = processor.getApproximateLoudnessLufs();

    processor.clearComparisonLoudness();
    juce::AudioBuffer<float> recovery(2, static_cast<int>(sampleRate));
    fillConstant(recovery, 0.1f);
    processor.process(recovery);
    const auto firstRecoverySample = recovery.getSample(0, 0);
    const auto recoveredSample = recovery.getSample(0,
                                                     recovery.getNumSamples() - 1);

    processor.setComparisonLoudness(-20.0f, -8.0f);
    fillConstant(settled, 0.1f);
    processor.process(settled);
    processor.reset();
    const bool resetClearsMeters =
        processor.getApproximateInputLoudnessLufs() == -100.0f
        && processor.getApproximateProcessedLoudnessLufs() == -100.0f
        && processor.getApproximateLoudnessLufs() == -100.0f
        && processor.getLastOutputPeak() == 0.0f
        && processor.getLimiterGainReductionDb() == 0.0f;

    juce::AudioBuffer<float> afterReset(2, static_cast<int>(sampleRate));
    fillConstant(afterReset, 0.1f);
    processor.process(afterReset);
    const auto resetRecoveredSample = afterReset.getSample(
        0,
        afterReset.getNumSamples() - 1);

    return expect(std::abs(firstMatchedSample - levelBeforeMatch) < 0.005f,
                  "level matching should not jump on the first sample")
        && expect(lastTransitionSample < firstMatchedSample * 0.95f
                      && lastTransitionSample > 0.07f,
                  "level matching should ramp toward the requested gain")
        && expect(settledMatchedSample > 0.02f
                      && settledMatchedSample < 0.04f,
                  "level matching should settle at the bounded adjustment")
        && expect(processedLufsDuringMatch > outputLufsDuringMatch + 10.0f,
                  "processed loudness should remain independent of comparison gain")
        && expect(firstRecoverySample < 0.04f
                      && recoveredSample > 0.09f,
                  "clearing level matching should ramp smoothly back to unity")
        && expect(resetClearsMeters && resetRecoveredSample > 0.09f,
                  "reset should clear meters and all comparison state");
}

bool testMasterBusInvalidAndDisabledInputsAreSafe()
{
    MasterBusProcessor processor;
    const bool invalidPrepare = !processor.prepare(
        std::numeric_limits<double>::quiet_NaN(), 512, 2);
    const bool invalidPrepareClearsMeters =
        processor.getApproximateInputLoudnessLufs() == -100.0f
        && processor.getApproximateProcessedLoudnessLufs() == -100.0f
        && processor.getApproximateLoudnessLufs() == -100.0f;

    if (!expect(processor.prepare(48000.0, 512, 2),
                "master bus should recover after an invalid prepare"))
        return false;

    MasteringSettings settings;
    settings.enabled = true;
    settings.limiterEnabled = false;
    settings.ceilingDb = std::numeric_limits<float>::quiet_NaN();
    settings.targetLufs = std::numeric_limits<float>::infinity();
    processor.setSettings(settings);
    processor.setComparisonLoudness(
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity());

    juce::AudioBuffer<float> invalidAudio(2, 4);
    invalidAudio.setSample(0, 0, std::numeric_limits<float>::quiet_NaN());
    invalidAudio.setSample(0, 1, std::numeric_limits<float>::infinity());
    invalidAudio.setSample(0, 2, -std::numeric_limits<float>::infinity());
    invalidAudio.setSample(0, 3, 0.2f);
    invalidAudio.copyFrom(1, 0, invalidAudio, 0, 0, 4);
    processor.process(invalidAudio);

    bool finiteOutput = std::isfinite(processor.getApproximateInputLoudnessLufs())
        && std::isfinite(processor.getApproximateProcessedLoudnessLufs())
        && std::isfinite(processor.getApproximateLoudnessLufs())
        && std::isfinite(processor.getLastOutputPeak());
    for (int channel = 0; channel < invalidAudio.getNumChannels(); ++channel)
        for (int sample = 0; sample < invalidAudio.getNumSamples(); ++sample)
            finiteOutput = finiteOutput
                && std::isfinite(invalidAudio.getSample(channel, sample));

    processor.reset();
    settings.enabled = false;
    processor.setSettings(settings);
    juce::AudioBuffer<float> bypassed(2, 48000);
    fillConstant(bypassed, 0.25f);
    processor.process(bypassed);

    bool bypassUnchanged = true;
    for (int channel = 0; channel < bypassed.getNumChannels(); ++channel)
        for (int sample = 0; sample < bypassed.getNumSamples(); ++sample)
            bypassUnchanged = bypassUnchanged
                && bypassed.getSample(channel, sample) == 0.25f;

    return expect(invalidPrepare && invalidPrepareClearsMeters,
                  "invalid prepare values should fail with cleared meters")
        && expect(finiteOutput
                      && MasterBusProcessor::calculateLevelMatchGainDb(
                             std::numeric_limits<float>::quiet_NaN(),
                             -14.0f) == 0.0f,
                  "non-finite audio and comparison values should remain safe")
        && expect(bypassUnchanged,
                  "disabled mastering must leave audio bit-for-bit unchanged")
        && expect(std::abs(processor.getApproximateInputLoudnessLufs()
                           - processor.getApproximateLoudnessLufs()) < 1.0e-6f,
                  "disabled mastering should report identical input and output loudness")
        && expect(std::abs(processor.getApproximateProcessedLoudnessLufs()
                           - processor.getApproximateLoudnessLufs()) < 1.0e-6f,
                  "disabled mastering should keep the processed estimate neutral");
}

TrackData makeAutoMixToneTrack(const juce::String& id,
                               TrackRole role,
                               double frequency,
                               float amplitude,
                               double durationSeconds = 1.0)
{
    constexpr int sampleRate = 48000;
    const auto sampleCount = std::max(
        1, static_cast<int>(std::llround(durationSeconds * sampleRate)));
    TrackData track;
    track.id = id;
    track.role = role;
    AudioClip clip;
    clip.id = id + "-clip";
    clip.sampleRate = sampleRate;
    clip.duration = durationSeconds;
    clip.buffer = std::make_shared<juce::AudioBuffer<float>>(1, sampleCount);
    for (int sample = 0; sample < sampleCount; ++sample)
        clip.buffer->setSample(0, sample,
            amplitude * static_cast<float>(std::sin(
                2.0 * juce::MathConstants<double>::pi * frequency
                * sample / sampleRate)));
    track.clips.push_back(std::move(clip));
    return track;
}

bool testAutoMixAnalysisIsDeterministicAndFrequencyAware()
{
    const auto lowTrack = makeAutoMixToneTrack(
        "low", TrackRole::other, 120.0, 0.2f);
    const auto highTrack = makeAutoMixToneTrack(
        "high", TrackRole::other, 7200.0, 0.2f);
    const auto firstLow = AutoMixAnalyzer::analyseTrack(lowTrack);
    const auto secondLow = AutoMixAnalyzer::analyseTrack(lowTrack);
    const auto high = AutoMixAnalyzer::analyseTrack(highTrack);

    return expect(firstLow.valid && high.valid,
                  "automatic mix should analyse normal audio")
        && expect(firstLow.rmsDb > -18.0f && firstLow.rmsDb < -16.0f
                      && firstLow.peakDb > -14.5f
                      && firstLow.peakDb < -13.5f,
                  "automatic mix should report deterministic RMS and peak levels")
        && expect(near(firstLow.rmsDb, secondLow.rmsDb)
                      && near(firstLow.lowEnergy, secondLow.lowEnergy)
                      && near(firstLow.noiseFloorDb,
                              secondLow.noiseFloorDb),
                  "automatic mix analysis should be deterministic")
        && expect(firstLow.lowEnergy > firstLow.highEnergy
                      && high.highEnergy > firstLow.highEnergy
                      && high.brightness > firstLow.brightness
                      && high.sibilance > firstLow.sibilance,
                  "automatic mix should distinguish low and bright/sibilant audio");
}

bool testAutoMixSuggestionsRespectRolesMoodAndMasking()
{
    AutoMixAnalysis vocal;
    vocal.valid = true;
    vocal.rmsDb = -24.0f;
    vocal.peakDb = -8.0f;
    vocal.crestFactorDb = 16.0f;
    vocal.lowEnergy = 0.12f;
    vocal.midEnergy = 0.68f;
    vocal.highEnergy = 0.20f;
    vocal.brightness = 0.42f;
    vocal.sibilance = 0.34f;
    vocal.noiseFloorDb = -34.0f;

    AutoMixAnalysis backing = vocal;
    backing.rmsDb = -17.0f;
    backing.lowEnergy = 0.20f;
    backing.midEnergy = 0.64f;
    backing.highEnergy = 0.16f;
    const auto masking = AutoMixAnalyzer::calculateMasking(vocal, backing);
    const auto wideMain = AutoMixAnalyzer::suggestTrack(
        "main", TrackRole::mainVocal, vocal, 0.0f, 0,
        MixPreset::wide);
    const auto maskedMain = AutoMixAnalyzer::suggestTrack(
        "main", TrackRole::mainVocal, vocal, masking, 0,
        MixPreset::wide);
    const auto chorusLeft = AutoMixAnalyzer::suggestTrack(
        "chorus-a", TrackRole::chorus, vocal, 0.0f, 0);
    const auto chorusRight = AutoMixAnalyzer::suggestTrack(
        "chorus-b", TrackRole::chorus, vocal, 0.0f, 1);

    return expect(masking > 0.35f,
                  "overlapping loud accompaniment should report masking")
        && expect(maskedMain.volume >= 0.0f && maskedMain.volume <= 1.0f
                      && maskedMain.simpleMix.brightness
                          > wideMain.simpleMix.brightness
                      && maskedMain.simpleMix.stability
                          > wideMain.simpleMix.stability,
                  "masking should safely lift vocal clarity and stability")
        && expect(maskedMain.simpleMix.preset == MixPreset::wide
                      && maskedMain.simpleMix.ambience
                          > 0.20f,
                  "automatic mix should preserve the selected mood")
        && expect(maskedMain.noiseReduction.enabled
                      && maskedMain.noiseReduction.amount > 0.0f
                      && maskedMain.noiseReduction.deEssAmount > 0.0f,
                  "noisy sibilant vocals should receive noise and de-ess suggestions")
        && expect(chorusLeft.pan < 0.0f && chorusRight.pan > 0.0f,
                  "successive chorus tracks should be spread left and right");
}

bool testAutoMixWholeProjectAndInvalidAudioAreSafe()
{
    auto vocal = makeAutoMixToneTrack(
        "vocal", TrackRole::mainVocal, 900.0, 0.08f);
    vocal.simpleMix.preset = MixPreset::radio;
    auto accompaniment = makeAutoMixToneTrack(
        "backing", TrackRole::accompaniment, 900.0, 0.3f);
    TrackData invalid;
    invalid.id = "invalid";
    invalid.role = TrackRole::harmony;
    AudioClip invalidClip;
    invalidClip.duration = 4.0 / 48000.0;
    invalidClip.sampleRate = 48000;
    invalidClip.buffer = std::make_shared<juce::AudioBuffer<float>>(1, 4);
    invalidClip.buffer->clear();
    invalidClip.buffer->setSample(
        0, 0, std::numeric_limits<float>::quiet_NaN());
    invalidClip.buffer->setSample(
        0, 1, std::numeric_limits<float>::infinity());
    invalid.clips.push_back(std::move(invalidClip));

    const auto result = AutoMixAnalyzer::analyseAndSuggest(
        { vocal, accompaniment, invalid });
    const auto emptyResult = AutoMixAnalyzer::analyseAndSuggest({});

    return expect(result.success && result.tracks.size() == 3,
                  "automatic mix should return one result for every track")
        && expect(result.tracks[0].trackId == "vocal"
                      && result.tracks[0].analysisValid
                      && result.tracks[0].simpleMix.preset == MixPreset::radio,
                  "project suggestions should retain track IDs and chosen mood")
        && expect(result.tracks[0].maskingScore > 0.0f
                      && result.tracks[1].maskingScore > 0.0f
                      && result.tracks[0].volume >= 0.0f
                      && result.tracks[0].volume <= 1.0f
                      && result.tracks[1].volume >= 0.0f
                      && result.tracks[1].volume <= 1.0f,
                  "project analysis should share vocal/accompaniment masking")
        && expect(!result.tracks[2].analysisValid
                      && result.tracks[2].volume == 1.0f
                      && !result.tracks[2].simpleMix.enabled
                      && result.tracks[2].warning.isNotEmpty(),
                  "NaN, infinity and silent short audio should remain unchanged")
        && expect(!emptyResult.success && emptyResult.tracks.empty()
                      && emptyResult.message.isNotEmpty(),
                  "an empty project should fail safely with a useful message");
}

bool testAutoMixMaskingRequiresTimelineOverlap()
{
    auto vocal = makeAutoMixToneTrack(
        "vocal", TrackRole::mainVocal, 1000.0, 0.12f);
    auto accompaniment = makeAutoMixToneTrack(
        "backing", TrackRole::accompaniment, 1000.0, 0.3f);
    accompaniment.clips.front().startTime = 2.0;

    const auto separated = AutoMixAnalyzer::analyseAndSuggest(
        { vocal, accompaniment });
    accompaniment.clips.front().startTime = 0.5;
    const auto overlapping = AutoMixAnalyzer::analyseAndSuggest(
        { vocal, accompaniment });

    return expect(separated.success && overlapping.success,
                  "timeline masking fixtures should analyse normally")
        && expect(separated.tracks[0].maskingScore < 1.0e-6f
                      && separated.tracks[1].maskingScore < 1.0e-6f,
                  "spectrally similar tracks should not mask when they never overlap")
        && expect(overlapping.tracks[0].maskingScore > 0.0f
                      && overlapping.tracks[1].maskingScore > 0.0f,
                  "spectrally similar tracks should mask while sounding together");
}

bool testSpecialFxIsRegionBoundedAndBlockSizeIndependent()
{
    constexpr int sampleRate = 48000;
    constexpr int sampleCount = 4096;
    juce::AudioBuffer<float> original(2, sampleCount);
    for (int channel = 0; channel < original.getNumChannels(); ++channel)
        for (int sample = 0; sample < sampleCount; ++sample)
            original.setSample(channel, sample,
                0.22f * static_cast<float>(std::sin(
                    2.0 * juce::MathConstants<double>::pi
                    * (channel == 0 ? 730.0 : 1170.0)
                    * sample / sampleRate)));

    auto whole = original;
    auto chunked = original;
    SpecialFxProcessor wholeProcessor;
    SpecialFxProcessor chunkedProcessor;
    if (!expect(wholeProcessor.prepare(sampleRate, sampleCount, 2)
                    && chunkedProcessor.prepare(sampleRate, 257, 2),
                "special FX processors should prepare for normal audio"))
        return false;

    SpecialFxProcessor::Settings settings;
    settings.enabled = true;
    settings.type = SpecialFxType::noise;
    settings.amount = 0.78f;
    settings.wet = 0.86f;
    settings.limitToRegion = true;
    settings.regionStartSample = 600;
    settings.regionEndSampleExclusive = 3300;
    settings.fadeSamples = 64;
    settings.noiseSeed = 0x1234abcdu;
    wholeProcessor.setSettings(settings);
    chunkedProcessor.setSettings(settings);
    wholeProcessor.process(whole, 0, sampleCount, 0);
    for (int start = 0; start < sampleCount; start += 257)
    {
        const auto count = std::min(257, sampleCount - start);
        chunkedProcessor.process(chunked, start, count, start);
    }

    bool outsideUnchanged = true;
    bool insideChanged = false;
    bool identical = true;
    bool finite = true;
    for (int channel = 0; channel < whole.getNumChannels(); ++channel)
        for (int sample = 0; sample < sampleCount; ++sample)
        {
            const auto rendered = whole.getSample(channel, sample);
            if (sample < 600 || sample >= 3300)
                outsideUnchanged = outsideUnchanged
                    && rendered == original.getSample(channel, sample);
            else
                insideChanged = insideChanged
                    || rendered != original.getSample(channel, sample);
            identical = identical
                && rendered == chunked.getSample(channel, sample);
            finite = finite && std::isfinite(rendered);
        }

    return expect(outsideUnchanged,
                  "special FX should not alter audio outside its A-B range")
        && expect(insideChanged,
                  "special FX should audibly alter audio inside its A-B range")
        && expect(identical,
                  "special FX should be deterministic across block sizes")
        && expect(finite,
                  "special FX should always produce finite audio");
}

bool testReferenceMixAnalysisIsSafeAndProducesSuggestions()
{
    constexpr int sampleRate = 48000;
    constexpr int sampleCount = sampleRate;
    juce::AudioBuffer<float> reference(2, sampleCount);
    for (int sample = 0; sample < sampleCount; ++sample)
    {
        const auto phase = 2.0 * juce::MathConstants<double>::pi
                         * 1150.0 * sample / sampleRate;
        const auto value = 0.30f * static_cast<float>(std::sin(phase));
        reference.setSample(0, sample, value);
        reference.setSample(1, sample, value * 0.82f);
    }

    const auto features = ReferenceMixAnalyzer::analyseReference(
        reference, sampleRate);
    const auto target = makeAutoMixToneTrack(
        "reference-target", TrackRole::mainVocal, 640.0, 0.12f);
    const auto result = ReferenceMixAnalyzer::analyseAndSuggest(
        reference, sampleRate, { target });
    juce::AudioBuffer<float> silence(1, 1024);
    silence.clear();
    const auto silentResult = ReferenceMixAnalyzer::analyseAndSuggest(
        silence, sampleRate, { target });

    return expect(features.valid && features.analysedSeconds > 0.9,
                  "reference analysis should measure normal audio")
        && expect(result.success && result.reference.valid
                      && result.target.valid && result.tracks.size() == 1,
                  "reference matching should return one suggestion per target track")
        && expect(result.tracks[0].trackId == target.id
                      && result.tracks[0].analysisValid
                      && result.tracks[0].simpleMix.enabled
                      && result.tracks[0].volume >= 0.0f
                      && result.tracks[0].volume <= 1.0f
                      && result.tracks[0].pan >= -1.0f
                      && result.tracks[0].pan <= 1.0f,
                  "reference matching should produce bounded non-destructive settings")
        && expect(result.limitation.isNotEmpty(),
                  "reference matching should explain that it is an approximation")
        && expect(!silentResult.success && silentResult.message.isNotEmpty(),
                  "silent reference audio should fail safely with a useful message");
}
}

bool runDawFeatureTests()
{
    bool ok = true;
    ok = testClipFadeAndPanMath() && ok;
    ok = testRangeAndLoopMath() && ok;
    ok = testTempoMapNormalisationAndLookup() && ok;
    ok = testTempoMapConversionsAndBeatPositions() && ok;
    ok = testMetronomeUsesCumulativeTempoMap() && ok;
    ok = testTempoEstimatorDeterminismAndGuards() && ok;
    ok = testEditableRhythmNotesAndWarpMapping() && ok;
    ok = testRhythmRenderPreservesPitch() && ok;
    ok = testSimpleMixIsSafeAndBlockSizeIndependent() && ok;
    ok = testMasterBusReportsInputAndAudibleOutputLoudness() && ok;
    ok = testMasterBusComparisonGainIsSmoothedAndResettable() && ok;
    ok = testMasterBusInvalidAndDisabledInputsAreSafe() && ok;
    ok = testAutoMixAnalysisIsDeterministicAndFrequencyAware() && ok;
    ok = testAutoMixSuggestionsRespectRolesMoodAndMasking() && ok;
    ok = testAutoMixWholeProjectAndInvalidAudioAreSafe() && ok;
    ok = testAutoMixMaskingRequiresTimelineOverlap() && ok;
    ok = testSpecialFxIsRegionBoundedAndBlockSizeIndependent() && ok;
    ok = testReferenceMixAnalysisIsSafeAndProducesSuggestions() && ok;
    return ok;
}

#if defined(SIMPLE_REC_PRO_DAW_FEATURE_TEST_STANDALONE)
int main()
{
    return runDawFeatureTests() ? 0 : 1;
}
#endif
