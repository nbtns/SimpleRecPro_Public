#include "../Source/LatencyCalibration.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace
{
bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAILED: " << message << std::endl;
    return condition;
}

std::vector<float> makeCapture(const std::vector<float>& probe,
                               int delaySamples,
                               int trailingSamples,
                               float gain,
                               float dcOffset,
                               float noiseAmplitude,
                               bool invertPolarity = false,
                               bool addLightReflections = false)
{
    std::vector<float> capture(static_cast<std::size_t>(delaySamples
                                                        + static_cast<int>(probe.size())
                                                        + trailingSamples),
                               dcOffset);

    std::uint32_t noiseState = 0x6d2b79f5u;
    for (auto& sample : capture)
    {
        noiseState ^= noiseState << 13;
        noiseState ^= noiseState >> 17;
        noiseState ^= noiseState << 5;
        const float unitNoise = static_cast<float>(noiseState & 0xffffu) / 32767.5f - 1.0f;
        sample += unitNoise * noiseAmplitude;
    }

    const float polarity = invertPolarity ? -1.0f : 1.0f;
    for (std::size_t i = 0; i < probe.size(); ++i)
        capture[static_cast<std::size_t>(delaySamples) + i] += probe[i] * gain * polarity;

    if (addLightReflections)
    {
        constexpr int firstReflectionDelay = 79;
        constexpr int secondReflectionDelay = 227;
        for (std::size_t i = 0; i < probe.size(); ++i)
        {
            const auto firstIndex = static_cast<std::size_t>(delaySamples
                                                             + firstReflectionDelay) + i;
            const auto secondIndex = static_cast<std::size_t>(delaySamples
                                                              + secondReflectionDelay) + i;
            if (firstIndex < capture.size())
                capture[firstIndex] += probe[i] * gain * polarity * 0.18f;
            if (secondIndex < capture.size())
                capture[secondIndex] -= probe[i] * gain * polarity * 0.07f;
        }
    }
    return capture;
}

bool testProbeIsDeterministicAndBounded()
{
    const auto first = LatencyCalibration::createProbeSignal(48000.0);
    const auto second = LatencyCalibration::createProbeSignal(48000.0);

    float peak = 0.0f;
    for (const float sample : first)
        peak = std::max(peak, std::abs(sample));

    return expect(first == second, "probe generation should be deterministic")
           && expect(first.size() == 8640, "default probe should last 180 ms at 48 kHz")
           && expect(peak <= 0.28001f, "probe should not exceed its configured amplitude")
           && expect(std::abs(first.front()) < 1.0e-7f
                         && std::abs(first.back()) < 1.0e-4f,
                     "probe should fade to silence at both ends")
           && expect(LatencyCalibration::createProbeSignal(0.0).empty(),
                     "invalid sample rates should not create a probe");
}

bool testKnownDelayWithNoiseAndDcOffset()
{
    constexpr double sampleRate = 48000.0;
    constexpr int delaySamples = 2371;
    const auto probe = LatencyCalibration::createProbeSignal(sampleRate);
    const auto capture = makeCapture(probe, delaySamples, 4000, 0.58f, 0.04f, 0.006f);

    LatencyCalibration::AnalysisOptions options;
    options.maximumLatencySamples = 6000;
    const auto measurement = LatencyCalibration::analyse(probe, capture, sampleRate, options);

    return expect(measurement.isValid(), "clean noisy capture should produce a valid measurement")
           && expect(measurement.latencySamples == delaySamples,
                     "normalised correlation should recover the exact sample delay")
           && expect(std::abs(measurement.latencyMilliseconds
                              - delaySamples * 1000.0 / sampleRate) < 1.0e-9,
                     "latency milliseconds should match the sample delay")
           && expect(measurement.normalizedCorrelation > 0.95f,
                     "known probe should have strong correlation")
           && expect(measurement.confidence > 0.85f,
                     "a clear match should have high confidence")
           && expect(!measurement.polarityInverted,
                     "non-inverted input should preserve polarity");
}

bool testEmissionOffsetAndInvertedPolarity()
{
    constexpr double sampleRate = 44100.0;
    constexpr int emissionStart = 700;
    constexpr int delaySamples = 1324;
    const auto probe = LatencyCalibration::createProbeSignal(sampleRate);
    auto capture = makeCapture(probe, emissionStart + delaySamples, 1800,
                               0.42f, -0.02f, 0.003f, true, true);

    LatencyCalibration::AnalysisOptions options;
    options.emissionStartSample = emissionStart;
    options.minimumLatencySamples = 500;
    options.maximumLatencySamples = 3000;
    const auto measurement = LatencyCalibration::analyse(probe, capture, sampleRate, options);

    return expect(measurement.isValid(),
                  "attenuation, polarity inversion, noise, and light reflections should remain measurable")
           && expect(measurement.latencySamples == delaySamples,
                     "latency should be relative to the known emission position")
           && expect(measurement.polarityInverted
                         && measurement.normalizedCorrelation < -0.95f,
                     "measurement should report inverted polarity");
}

bool testQuietButCorrelatedMicrophoneInputCanPassCalibrationThreshold()
{
    constexpr double sampleRate = 48000.0;
    constexpr int delaySamples = 1850;
    const auto probe = LatencyCalibration::createProbeSignal(sampleRate);
    const auto capture = makeCapture(probe, delaySamples, 2400,
                                     0.006f, 0.0f, 0.00005f);

    LatencyCalibration::AnalysisOptions options;
    options.maximumLatencySamples = 4000;
    options.minimumInputRms = 0.0007f;
    const auto measurement = LatencyCalibration::analyse(probe, capture,
                                                          sampleRate, options);

    return expect(measurement.isValid(),
                  "a quiet but strongly correlated microphone capture should remain measurable")
           && expect(measurement.latencySamples == delaySamples,
                     "quiet microphone capture should preserve the exact delay")
           && expect(measurement.inputRms >= options.minimumInputRms
                         && measurement.inputRms < 0.0025f,
                     "the calibration threshold should cover roughly -60 dB microphone input");
}

bool testRepeatedCalibrationSequence()
{
    constexpr double sampleRate = 48000.0;
    constexpr int knownLatency = 1777;
    constexpr int maximumLatency = 24000;
    const auto probe = LatencyCalibration::createProbeSignal(sampleRate);
    const std::vector<int> emissions { 31200, 69600, 108000, 146400 };
    const int totalSamples = emissions.back() + static_cast<int>(probe.size()) + 26400;
    std::vector<float> capture(static_cast<std::size_t>(totalSamples), 0.0f);

    std::uint32_t noiseState = 0x54f17ab3u;
    for (auto& sample : capture)
    {
        noiseState = noiseState * 1664525u + 1013904223u;
        sample = (static_cast<float>((noiseState >> 8) & 0xffffu) / 32767.5f - 1.0f)
                 * 0.001f;
    }

    for (std::size_t trial = 0; trial < emissions.size(); ++trial)
    {
        if (trial == 2) // One bad trial must not invalidate three stable measurements.
            continue;
        for (std::size_t sample = 0; sample < probe.size(); ++sample)
            capture[static_cast<std::size_t>(emissions[trial] + knownLatency) + sample]
                += probe[sample] * 0.45f;
    }

    std::vector<LatencyCalibration::Measurement> measurements;
    for (const int emission : emissions)
    {
        const int windowLength = maximumLatency + static_cast<int>(probe.size());
        const std::vector<float> window(capture.begin() + emission,
                                        capture.begin() + emission + windowLength);
        LatencyCalibration::AnalysisOptions options;
        options.maximumLatencySamples = maximumLatency;
        measurements.push_back(LatencyCalibration::analyse(probe, window,
                                                            sampleRate, options));
    }

    LatencyCalibration::AggregateOptions aggregateOptions;
    aggregateOptions.minimumValidMeasurements = 3;
    aggregateOptions.minimumMeasurementConfidence = 0.55f;
    const auto result = LatencyCalibration::aggregate(measurements, sampleRate,
                                                      aggregateOptions);
    return expect(result.isValid(),
                  "three stable repeated probes should survive one failed trial")
           && expect(result.latencySamples == knownLatency,
                     "repeated calibration windows should recover round-trip latency")
           && expect(result.acceptedMeasurementCount == 3,
                     "only the three audible probe trials should be accepted");
}

bool testStrongLateReflectionIsNeverAcceptedAsDirectArrival()
{
    constexpr double sampleRate = 48000.0;
    constexpr int directDelay = 1400;
    constexpr int reflectionDelay = 650;
    const auto probe = LatencyCalibration::createProbeSignal(sampleRate);
    auto capture = makeCapture(probe, directDelay, 5000, 0.24f, 0.0f, 0.001f);
    for (std::size_t sample = 0; sample < probe.size(); ++sample)
        capture[static_cast<std::size_t>(directDelay + reflectionDelay) + sample]
            += probe[sample] * 0.62f;

    LatencyCalibration::AnalysisOptions options;
    options.maximumLatencySamples = 4000;
    const auto measurement = LatencyCalibration::analyse(probe, capture,
                                                          sampleRate, options);
    if (measurement.isValid() && measurement.latencySamples != directDelay)
        std::cerr << "late-reflection result=" << measurement.latencySamples
                  << " correlation=" << measurement.normalizedCorrelation
                  << " prominence=" << measurement.peakProminence << std::endl;
    return expect(!measurement.isValid()
                      || measurement.latencySamples == directDelay,
                  "a stronger late reflection must never be saved as the direct-arrival latency");
}

bool testQuietAndClippedInputAreRejected()
{
    constexpr double sampleRate = 48000.0;
    const auto probe = LatencyCalibration::createProbeSignal(sampleRate);

    const auto quietCapture = makeCapture(probe, 900, 1200, 0.002f, 0.0f, 0.00001f);
    LatencyCalibration::AnalysisOptions quietOptions;
    quietOptions.maximumLatencySamples = 1800;
    const auto quiet = LatencyCalibration::analyse(probe, quietCapture,
                                                   sampleRate, quietOptions);

    auto clippedCapture = makeCapture(probe, 900, 1200, 5.0f, 0.0f, 0.0f);
    for (auto& sample : clippedCapture)
        sample = std::max(-1.0f, std::min(sample, 1.0f));
    LatencyCalibration::AnalysisOptions clippedOptions;
    clippedOptions.maximumLatencySamples = 1800;
    const auto clipped = LatencyCalibration::analyse(probe, clippedCapture,
                                                     sampleRate, clippedOptions);

    return expect(quiet.status == LatencyCalibration::MeasurementStatus::inputTooQuiet,
                  "a signal below the input threshold should be rejected")
           && expect(clipped.status == LatencyCalibration::MeasurementStatus::inputClipped,
                     "a clipped probe should be rejected")
           && expect(clipped.clippedFraction > clippedOptions.maximumClippedFraction,
                     "clipping ratio should explain the rejection");
}

bool testUnrelatedNoiseIsRejected()
{
    constexpr double sampleRate = 48000.0;
    const auto probe = LatencyCalibration::createProbeSignal(sampleRate);
    std::vector<float> capture(probe.size() + 6000);

    std::uint32_t state = 0x12345678u;
    for (auto& sample : capture)
    {
        state = state * 1664525u + 1013904223u;
        sample = (static_cast<float>((state >> 8) & 0xffffu) / 32767.5f - 1.0f) * 0.08f;
    }

    LatencyCalibration::AnalysisOptions options;
    options.maximumLatencySamples = 6000;
    const auto measurement = LatencyCalibration::analyse(probe, capture, sampleRate, options);
    return expect(measurement.status == LatencyCalibration::MeasurementStatus::unreliableMatch,
                  "unrelated input should not be accepted as the probe")
           && expect(std::abs(measurement.normalizedCorrelation) < options.minimumCorrelation,
                     "unrelated input should stay below the correlation threshold");
}

LatencyCalibration::Measurement successfulMeasurement(int latencySamples, float confidence)
{
    LatencyCalibration::Measurement measurement;
    measurement.status = LatencyCalibration::MeasurementStatus::success;
    measurement.latencySamples = latencySamples;
    measurement.confidence = confidence;
    return measurement;
}

bool testAggregationRejectsOutlier()
{
    constexpr double sampleRate = 48000.0;
    std::vector<LatencyCalibration::Measurement> measurements {
        successfulMeasurement(2400, 0.94f),
        successfulMeasurement(2402, 0.91f),
        successfulMeasurement(2399, 0.93f),
        successfulMeasurement(2401, 0.95f),
        successfulMeasurement(3900, 0.99f)
    };

    LatencyCalibration::Measurement failed;
    failed.status = LatencyCalibration::MeasurementStatus::inputTooQuiet;
    measurements.push_back(failed);

    const auto aggregate = LatencyCalibration::aggregate(measurements, sampleRate);
    return expect(aggregate.isValid(), "stable measurements should aggregate successfully")
           && expect(aggregate.latencySamples == 2401,
                     "aggregate should use the median of the stable measurements")
           && expect(aggregate.acceptedMeasurementCount == 4,
                     "one numerical outlier should be removed")
           && expect(aggregate.rejectedMeasurementCount == 2,
                     "failed and outlying measurements should be reported as rejected")
           && expect(aggregate.spreadSamples == 3,
                     "aggregate should report the accepted measurement spread");
}

bool testAggregationRequiresConsistency()
{
    constexpr double sampleRate = 48000.0;
    const std::vector<LatencyCalibration::Measurement> tooFew {
        successfulMeasurement(1200, 0.9f),
        successfulMeasurement(1201, 0.9f)
    };
    const std::vector<LatencyCalibration::Measurement> inconsistent {
        successfulMeasurement(100, 0.9f),
        successfulMeasurement(200, 0.9f),
        successfulMeasurement(300, 0.9f)
    };

    const auto tooFewResult = LatencyCalibration::aggregate(tooFew, sampleRate);
    const auto inconsistentResult = LatencyCalibration::aggregate(inconsistent, sampleRate);
    return expect(tooFewResult.status
                      == LatencyCalibration::AggregateStatus::notEnoughValidMeasurements,
                  "aggregation should require repeated measurements")
           && expect(inconsistentResult.status
                         == LatencyCalibration::AggregateStatus::inconsistentMeasurements,
                     "widely spread measurements should not produce an automatic offset");
}
}

bool runLatencyCalibrationTests()
{
    bool ok = true;
    ok = testProbeIsDeterministicAndBounded() && ok;
    ok = testKnownDelayWithNoiseAndDcOffset() && ok;
    ok = testEmissionOffsetAndInvertedPolarity() && ok;
    ok = testQuietButCorrelatedMicrophoneInputCanPassCalibrationThreshold() && ok;
    ok = testRepeatedCalibrationSequence() && ok;
    ok = testStrongLateReflectionIsNeverAcceptedAsDirectArrival() && ok;
    ok = testQuietAndClippedInputAreRejected() && ok;
    ok = testUnrelatedNoiseIsRejected() && ok;
    ok = testAggregationRejectsOutlier() && ok;
    ok = testAggregationRequiresConsistency() && ok;

    if (ok)
        std::cout << "All latency-calibration tests passed." << std::endl;
    return ok;
}
