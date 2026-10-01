#pragma once

#include <vector>

namespace LatencyCalibration
{
/** Parameters for the deterministic acoustic probe played during calibration. */
struct ProbeOptions
{
    double durationSeconds = 0.18;
    double startFrequencyHz = 650.0;
    double endFrequencyHz = 7500.0;
    double fadeSeconds = 0.008;
    float amplitude = 0.28f;
};

/**
 * Creates a logarithmic sweep with a short fade at both ends.
 *
 * The returned mono signal is deterministic: the same sample rate and options
 * always produce exactly the same samples. An empty vector means that the
 * sample rate or options were invalid.
 */
std::vector<float> createProbeSignal(double sampleRate,
                                     const ProbeOptions& options = {});

enum class MeasurementStatus
{
    success,
    invalidArguments,
    signalTooShort,
    captureTooShort,
    inputTooQuiet,
    inputClipped,
    unreliableMatch
};

struct AnalysisOptions
{
    /** Sample position in the captured stream at which probe playback began. */
    int emissionStartSample = 0;

    /** Inclusive round-trip latency search range, relative to emissionStartSample. */
    int minimumLatencySamples = 0;
    int maximumLatencySamples = -1; // -1 searches to the end of the capture.

    /** The matched input window must exceed this AC RMS level. */
    float minimumInputRms = 0.0025f;

    /** A measurement is rejected when too many samples reach this magnitude. */
    float clippingThreshold = 0.995f;
    float maximumClippedFraction = 0.002f;

    /** Quality gates for the normalised cross-correlation peak. */
    float minimumCorrelation = 0.32f;
    float minimumPeakProminence = 0.06f;

    /**
     * Radius around the best peak ignored while finding the competing peak.
     * Zero selects an automatic value of approximately 3 ms.
     */
    int competingPeakExclusionSamples = 0;
};

struct Measurement
{
    MeasurementStatus status = MeasurementStatus::invalidArguments;
    int latencySamples = 0;
    double latencyMilliseconds = 0.0;

    /** Signed coefficient; a negative value means that polarity was inverted. */
    float normalizedCorrelation = 0.0f;
    float peakProminence = 0.0f;
    float confidence = 0.0f;
    bool polarityInverted = false;

    /** Level statistics for the window that matched the probe. */
    float inputRms = 0.0f;
    float inputPeak = 0.0f;
    float clippedFraction = 0.0f;

    bool isValid() const noexcept { return status == MeasurementStatus::success; }
};

/**
 * Finds where referenceSignal begins in capturedSignal and returns the delay
 * relative to AnalysisOptions::emissionStartSample.
 *
 * This is intentionally an offline operation. It allocates memory and performs
 * an FFT, so it must never be called from the real-time audio callback.
 */
Measurement analyse(const std::vector<float>& referenceSignal,
                    const std::vector<float>& capturedSignal,
                    double sampleRate,
                    const AnalysisOptions& options = {});

enum class AggregateStatus
{
    success,
    invalidArguments,
    notEnoughValidMeasurements,
    inconsistentMeasurements
};

struct AggregateOptions
{
    int minimumValidMeasurements = 3;
    float minimumMeasurementConfidence = 0.0f;

    /** Robust outlier threshold: max(minimum tolerance, MAD * multiplier). */
    double minimumOutlierToleranceMilliseconds = 0.5;
    double medianAbsoluteDeviationMultiplier = 3.5;

    /** The total range of accepted measurements may not exceed this value. */
    double maximumSpreadMilliseconds = 2.0;
};

struct AggregateResult
{
    AggregateStatus status = AggregateStatus::invalidArguments;
    int latencySamples = 0;
    double latencyMilliseconds = 0.0;
    float confidence = 0.0f;

    int totalMeasurementCount = 0;
    int validMeasurementCount = 0;
    int acceptedMeasurementCount = 0;
    int rejectedMeasurementCount = 0;

    double medianAbsoluteDeviationSamples = 0.0;
    int spreadSamples = 0;

    bool isValid() const noexcept { return status == AggregateStatus::success; }
};

/** Combines repeated successful measurements using a median/MAD outlier filter. */
AggregateResult aggregate(const std::vector<Measurement>& measurements,
                          double sampleRate,
                          const AggregateOptions& options = {});
}
