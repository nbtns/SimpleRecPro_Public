#include "LatencyCalibration.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numeric>

namespace LatencyCalibration
{
namespace
{
constexpr double twoPi = 6.283185307179586476925286766559;

template <typename Value>
Value clamp(Value value, Value low, Value high)
{
    return std::max(low, std::min(value, high));
}

bool isFinite(double value)
{
    return std::isfinite(value);
}

std::size_t nextPowerOfTwo(std::size_t value)
{
    std::size_t result = 1;
    while (result < value)
    {
        if (result > std::numeric_limits<std::size_t>::max() / 2)
            return 0;
        result <<= 1;
    }
    return result;
}

void fft(std::vector<std::complex<double>>& values, bool inverse)
{
    const std::size_t size = values.size();

    for (std::size_t i = 1, j = 0; i < size; ++i)
    {
        std::size_t bit = size >> 1;
        for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;
        j ^= bit;

        if (i < j)
            std::swap(values[i], values[j]);
    }

    for (std::size_t length = 2; length <= size; length <<= 1)
    {
        const double angle = (inverse ? twoPi : -twoPi) / static_cast<double>(length);
        const std::complex<double> step(std::cos(angle), std::sin(angle));

        for (std::size_t start = 0; start < size; start += length)
        {
            std::complex<double> phase(1.0, 0.0);
            const std::size_t halfLength = length / 2;
            for (std::size_t offset = 0; offset < halfLength; ++offset)
            {
                const auto even = values[start + offset];
                const auto odd = values[start + offset + halfLength] * phase;
                values[start + offset] = even + odd;
                values[start + offset + halfLength] = even - odd;
                phase *= step;
            }
        }

        if (length == size)
            break; // Avoid unsigned overflow when size is the highest power of two.
    }

    if (inverse)
    {
        const double scale = 1.0 / static_cast<double>(size);
        for (auto& value : values)
            value *= scale;
    }
}

std::vector<double> crossCorrelate(const std::vector<float>& captured,
                                   const std::vector<double>& zeroMeanReference)
{
    if (captured.empty() || zeroMeanReference.empty())
        return {};

    const std::size_t convolutionLength = captured.size() + zeroMeanReference.size() - 1;
    const std::size_t fftSize = nextPowerOfTwo(convolutionLength);
    if (fftSize == 0)
        return {};

    std::vector<std::complex<double>> captureSpectrum(fftSize);
    std::vector<std::complex<double>> referenceSpectrum(fftSize);

    for (std::size_t i = 0; i < captured.size(); ++i)
        captureSpectrum[i] = static_cast<double>(captured[i]);
    for (std::size_t i = 0; i < zeroMeanReference.size(); ++i)
        referenceSpectrum[i] = zeroMeanReference[zeroMeanReference.size() - 1 - i];

    fft(captureSpectrum, false);
    fft(referenceSpectrum, false);
    for (std::size_t i = 0; i < fftSize; ++i)
        captureSpectrum[i] *= referenceSpectrum[i];
    fft(captureSpectrum, true);

    std::vector<double> result(convolutionLength);
    for (std::size_t i = 0; i < convolutionLength; ++i)
        result[i] = captureSpectrum[i].real();
    return result;
}

double median(std::vector<double> values)
{
    if (values.empty())
        return 0.0;

    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    if ((values.size() & 1u) != 0u)
        return values[middle];
    return (values[middle - 1] + values[middle]) * 0.5;
}

float calculateMedianConfidence(const std::vector<const Measurement*>& measurements)
{
    std::vector<double> values;
    values.reserve(measurements.size());
    for (const auto* measurement : measurements)
        values.push_back(measurement->confidence);
    return static_cast<float>(median(std::move(values)));
}
}

std::vector<float> createProbeSignal(double sampleRate, const ProbeOptions& options)
{
    const double nyquist = sampleRate * 0.5;
    if (!isFinite(sampleRate) || sampleRate < 8000.0
        || !isFinite(options.durationSeconds) || options.durationSeconds <= 0.0
        || !isFinite(options.startFrequencyHz) || options.startFrequencyHz <= 0.0
        || !isFinite(options.endFrequencyHz)
        || options.endFrequencyHz <= options.startFrequencyHz
        || options.endFrequencyHz >= nyquist * 0.95
        || !isFinite(options.fadeSeconds) || options.fadeSeconds < 0.0
        || options.fadeSeconds * 2.0 >= options.durationSeconds
        || !std::isfinite(options.amplitude) || options.amplitude <= 0.0f
        || options.amplitude > 1.0f)
    {
        return {};
    }

    const int sampleCount = static_cast<int>(std::llround(options.durationSeconds * sampleRate));
    if (sampleCount < 128)
        return {};

    const int fadeSamples = std::max(1, static_cast<int>(std::llround(options.fadeSeconds * sampleRate)));
    const double sweepDuration = static_cast<double>(sampleCount) / sampleRate;
    const double logFrequencyRatio = std::log(options.endFrequencyHz / options.startFrequencyHz);
    const double sweepRate = logFrequencyRatio / sweepDuration;

    std::vector<float> signal(static_cast<std::size_t>(sampleCount));
    for (int sample = 0; sample < sampleCount; ++sample)
    {
        const double time = static_cast<double>(sample) / sampleRate;
        const double phase = twoPi * options.startFrequencyHz
                             * std::expm1(sweepRate * time) / sweepRate;

        double envelope = 1.0;
        if (sample < fadeSamples)
        {
            const double position = static_cast<double>(sample) / fadeSamples;
            envelope = 0.5 - 0.5 * std::cos(3.14159265358979323846 * position);
        }
        else if (sample >= sampleCount - fadeSamples)
        {
            const double position = static_cast<double>(sampleCount - 1 - sample) / fadeSamples;
            envelope = 0.5 - 0.5 * std::cos(3.14159265358979323846
                                            * std::max(0.0, position));
        }

        signal[static_cast<std::size_t>(sample)] = static_cast<float>(
            options.amplitude * envelope * std::sin(phase));
    }

    return signal;
}

Measurement analyse(const std::vector<float>& referenceSignal,
                    const std::vector<float>& capturedSignal,
                    double sampleRate,
                    const AnalysisOptions& options)
{
    Measurement result;

    if (!isFinite(sampleRate) || sampleRate <= 0.0
        || options.emissionStartSample < 0
        || options.minimumLatencySamples < 0
        || (options.maximumLatencySamples >= 0
            && options.maximumLatencySamples < options.minimumLatencySamples)
        || !std::isfinite(options.minimumInputRms) || options.minimumInputRms < 0.0f
        || !std::isfinite(options.clippingThreshold) || options.clippingThreshold <= 0.0f
        || !std::isfinite(options.maximumClippedFraction)
        || options.maximumClippedFraction < 0.0f || options.maximumClippedFraction > 1.0f
        || !std::isfinite(options.minimumCorrelation)
        || options.minimumCorrelation < 0.0f || options.minimumCorrelation > 1.0f
        || !std::isfinite(options.minimumPeakProminence)
        || options.minimumPeakProminence < 0.0f || options.minimumPeakProminence > 1.0f
        || options.competingPeakExclusionSamples < 0)
    {
        result.status = MeasurementStatus::invalidArguments;
        return result;
    }

    if (referenceSignal.size() < 128)
    {
        result.status = MeasurementStatus::signalTooShort;
        return result;
    }
    if (capturedSignal.size() < referenceSignal.size())
    {
        result.status = MeasurementStatus::captureTooShort;
        return result;
    }

    for (const float sample : referenceSignal)
    {
        if (!std::isfinite(sample))
        {
            result.status = MeasurementStatus::invalidArguments;
            return result;
        }
    }
    for (const float sample : capturedSignal)
    {
        if (!std::isfinite(sample))
        {
            result.status = MeasurementStatus::invalidArguments;
            return result;
        }
    }

    const int referenceLength = static_cast<int>(referenceSignal.size());
    const int lastPossibleStart = static_cast<int>(capturedSignal.size()) - referenceLength;
    const int firstCandidate = options.emissionStartSample + options.minimumLatencySamples;
    const int requestedLastCandidate = options.maximumLatencySamples < 0
        ? lastPossibleStart
        : options.emissionStartSample + options.maximumLatencySamples;
    const int lastCandidate = std::min(lastPossibleStart, requestedLastCandidate);

    if (firstCandidate < 0 || firstCandidate > lastCandidate)
    {
        result.status = MeasurementStatus::captureTooShort;
        return result;
    }

    const double referenceMean = std::accumulate(referenceSignal.begin(),
                                                 referenceSignal.end(), 0.0)
                                 / static_cast<double>(referenceLength);
    std::vector<double> zeroMeanReference(referenceSignal.size());
    double referenceEnergy = 0.0;
    for (std::size_t i = 0; i < referenceSignal.size(); ++i)
    {
        const double centred = static_cast<double>(referenceSignal[i]) - referenceMean;
        zeroMeanReference[i] = centred;
        referenceEnergy += centred * centred;
    }
    if (referenceEnergy <= std::numeric_limits<double>::epsilon())
    {
        result.status = MeasurementStatus::signalTooShort;
        return result;
    }

    const auto correlationNumerators = crossCorrelate(capturedSignal, zeroMeanReference);
    if (correlationNumerators.empty())
    {
        result.status = MeasurementStatus::invalidArguments;
        return result;
    }

    std::vector<double> prefixSum(capturedSignal.size() + 1, 0.0);
    std::vector<double> prefixSquareSum(capturedSignal.size() + 1, 0.0);
    for (std::size_t i = 0; i < capturedSignal.size(); ++i)
    {
        const double value = capturedSignal[i];
        prefixSum[i + 1] = prefixSum[i] + value;
        prefixSquareSum[i + 1] = prefixSquareSum[i] + value * value;
    }

    const int candidateCount = lastCandidate - firstCandidate + 1;
    std::vector<float> correlations(static_cast<std::size_t>(candidateCount), 0.0f);
    int bestCandidate = firstCandidate;
    double bestAbsoluteCorrelation = -1.0;
    double bestSignedCorrelation = 0.0;

    for (int candidate = firstCandidate; candidate <= lastCandidate; ++candidate)
    {
        const int end = candidate + referenceLength;
        const double windowSum = prefixSum[static_cast<std::size_t>(end)]
                                 - prefixSum[static_cast<std::size_t>(candidate)];
        const double windowSquareSum = prefixSquareSum[static_cast<std::size_t>(end)]
                                       - prefixSquareSum[static_cast<std::size_t>(candidate)];
        const double windowEnergy = std::max(0.0,
            windowSquareSum - windowSum * windowSum / static_cast<double>(referenceLength));

        double correlation = 0.0;
        if (windowEnergy > std::numeric_limits<double>::epsilon())
        {
            const std::size_t convolutionIndex = static_cast<std::size_t>(candidate
                                                                          + referenceLength - 1);
            correlation = correlationNumerators[convolutionIndex]
                          / std::sqrt(referenceEnergy * windowEnergy);
            correlation = clamp(correlation, -1.0, 1.0);
        }

        correlations[static_cast<std::size_t>(candidate - firstCandidate)]
            = static_cast<float>(correlation);

        const double absoluteCorrelation = std::abs(correlation);
        if (absoluteCorrelation > bestAbsoluteCorrelation)
        {
            bestAbsoluteCorrelation = absoluteCorrelation;
            bestSignedCorrelation = correlation;
            bestCandidate = candidate;
        }
    }

    const int exclusionSamples = options.competingPeakExclusionSamples > 0
        ? options.competingPeakExclusionSamples
        : std::max(1, static_cast<int>(std::llround(sampleRate * 0.003)));

    // A room reflection can be stronger than the direct arrival. If a distinct,
    // credible peak exists earlier than the strongest match, reject the result
    // as ambiguous instead of saving the later reflection as device latency.
    bool hasCredibleEarlierPeak = false;
    const double earlierPeakThreshold = std::max(0.16, bestAbsoluteCorrelation * 0.15);
    for (int candidate = firstCandidate + 1;
         candidate < bestCandidate - exclusionSamples;
         ++candidate)
    {
        const auto index = static_cast<std::size_t>(candidate - firstCandidate);
        const double value = std::abs(correlations[index]);
        if (value >= earlierPeakThreshold
            && value >= std::abs(correlations[index - 1])
            && value >= std::abs(correlations[index + 1]))
        {
            hasCredibleEarlierPeak = true;
            break;
        }
    }

    double secondBestAbsoluteCorrelation = 0.0;
    for (int candidate = firstCandidate; candidate <= lastCandidate; ++candidate)
    {
        if (std::abs(candidate - bestCandidate) <= exclusionSamples)
            continue;
        const double value = std::abs(correlations[static_cast<std::size_t>(candidate
                                                                            - firstCandidate)]);
        secondBestAbsoluteCorrelation = std::max(secondBestAbsoluteCorrelation, value);
    }

    result.latencySamples = bestCandidate - options.emissionStartSample;
    result.latencyMilliseconds = result.latencySamples * 1000.0 / sampleRate;
    result.normalizedCorrelation = static_cast<float>(bestSignedCorrelation);
    result.polarityInverted = bestSignedCorrelation < 0.0;
    result.peakProminence = static_cast<float>(
        std::max(0.0, bestAbsoluteCorrelation - secondBestAbsoluteCorrelation));

    double windowSquareSum = 0.0;
    double windowSum = 0.0;
    double inputPeak = 0.0;
    int clippedSamples = 0;
    for (int sample = bestCandidate; sample < bestCandidate + referenceLength; ++sample)
    {
        const double value = capturedSignal[static_cast<std::size_t>(sample)];
        windowSum += value;
        windowSquareSum += value * value;
        inputPeak = std::max(inputPeak, std::abs(value));
        if (std::abs(value) >= options.clippingThreshold)
            ++clippedSamples;
    }

    const double inputEnergy = std::max(0.0,
        windowSquareSum - windowSum * windowSum / static_cast<double>(referenceLength));
    result.inputRms = static_cast<float>(std::sqrt(inputEnergy
                                                   / static_cast<double>(referenceLength)));
    result.inputPeak = static_cast<float>(inputPeak);
    result.clippedFraction = static_cast<float>(clippedSamples)
                             / static_cast<float>(referenceLength);

    const double prominenceRatio = bestAbsoluteCorrelation > 0.0
        ? result.peakProminence / bestAbsoluteCorrelation
        : 0.0;
    result.confidence = static_cast<float>(clamp(0.72 * bestAbsoluteCorrelation
                                                 + 0.28 * prominenceRatio,
                                                 0.0, 1.0));

    if (result.inputRms < options.minimumInputRms)
        result.status = MeasurementStatus::inputTooQuiet;
    else if (result.clippedFraction > options.maximumClippedFraction)
        result.status = MeasurementStatus::inputClipped;
    else if (bestAbsoluteCorrelation < options.minimumCorrelation
             || result.peakProminence < options.minimumPeakProminence
             || hasCredibleEarlierPeak)
        result.status = MeasurementStatus::unreliableMatch;
    else
        result.status = MeasurementStatus::success;

    return result;
}

AggregateResult aggregate(const std::vector<Measurement>& measurements,
                          double sampleRate,
                          const AggregateOptions& options)
{
    AggregateResult result;
    result.totalMeasurementCount = static_cast<int>(measurements.size());

    if (!isFinite(sampleRate) || sampleRate <= 0.0
        || options.minimumValidMeasurements <= 0
        || !std::isfinite(options.minimumMeasurementConfidence)
        || options.minimumMeasurementConfidence < 0.0f
        || options.minimumMeasurementConfidence > 1.0f
        || !isFinite(options.minimumOutlierToleranceMilliseconds)
        || options.minimumOutlierToleranceMilliseconds < 0.0
        || !isFinite(options.medianAbsoluteDeviationMultiplier)
        || options.medianAbsoluteDeviationMultiplier <= 0.0
        || !isFinite(options.maximumSpreadMilliseconds)
        || options.maximumSpreadMilliseconds < 0.0)
    {
        result.status = AggregateStatus::invalidArguments;
        result.rejectedMeasurementCount = result.totalMeasurementCount;
        return result;
    }

    std::vector<const Measurement*> validMeasurements;
    std::vector<double> latencyValues;
    validMeasurements.reserve(measurements.size());
    latencyValues.reserve(measurements.size());
    for (const auto& measurement : measurements)
    {
        if (measurement.isValid()
            && measurement.latencySamples >= 0
            && std::isfinite(measurement.confidence)
            && measurement.confidence >= options.minimumMeasurementConfidence)
        {
            validMeasurements.push_back(&measurement);
            latencyValues.push_back(static_cast<double>(measurement.latencySamples));
        }
    }

    result.validMeasurementCount = static_cast<int>(validMeasurements.size());
    if (result.validMeasurementCount < options.minimumValidMeasurements)
    {
        result.status = AggregateStatus::notEnoughValidMeasurements;
        result.rejectedMeasurementCount = result.totalMeasurementCount;
        return result;
    }

    const double centre = median(latencyValues);
    std::vector<double> absoluteDeviations;
    absoluteDeviations.reserve(latencyValues.size());
    for (const double latency : latencyValues)
        absoluteDeviations.push_back(std::abs(latency - centre));
    result.medianAbsoluteDeviationSamples = median(std::move(absoluteDeviations));

    const double minimumToleranceSamples = options.minimumOutlierToleranceMilliseconds
                                           * sampleRate / 1000.0;
    const double outlierToleranceSamples = std::max(
        minimumToleranceSamples,
        result.medianAbsoluteDeviationSamples * options.medianAbsoluteDeviationMultiplier);

    std::vector<const Measurement*> acceptedMeasurements;
    std::vector<double> acceptedLatencies;
    for (const auto* measurement : validMeasurements)
    {
        if (std::abs(static_cast<double>(measurement->latencySamples) - centre)
            <= outlierToleranceSamples)
        {
            acceptedMeasurements.push_back(measurement);
            acceptedLatencies.push_back(static_cast<double>(measurement->latencySamples));
        }
    }

    result.acceptedMeasurementCount = static_cast<int>(acceptedMeasurements.size());
    result.rejectedMeasurementCount = result.totalMeasurementCount
                                      - result.acceptedMeasurementCount;
    if (result.acceptedMeasurementCount < options.minimumValidMeasurements)
    {
        result.status = AggregateStatus::inconsistentMeasurements;
        return result;
    }

    const auto acceptedMinMax = std::minmax_element(acceptedLatencies.begin(),
                                                    acceptedLatencies.end());
    result.spreadSamples = static_cast<int>(std::llround(*acceptedMinMax.second
                                                         - *acceptedMinMax.first));
    const double maximumSpreadSamples = options.maximumSpreadMilliseconds
                                        * sampleRate / 1000.0;
    if (result.spreadSamples > maximumSpreadSamples)
    {
        result.status = AggregateStatus::inconsistentMeasurements;
        return result;
    }

    const double acceptedMedian = median(acceptedLatencies);
    result.latencySamples = static_cast<int>(std::llround(acceptedMedian));
    result.latencyMilliseconds = result.latencySamples * 1000.0 / sampleRate;

    const double consistency = maximumSpreadSamples > 0.0
        ? clamp(1.0 - result.spreadSamples / maximumSpreadSamples, 0.0, 1.0)
        : (result.spreadSamples == 0 ? 1.0 : 0.0);
    const double acceptanceRatio = static_cast<double>(result.acceptedMeasurementCount)
                                   / static_cast<double>(result.validMeasurementCount);
    const double medianConfidence = calculateMedianConfidence(acceptedMeasurements);
    result.confidence = static_cast<float>(clamp(
        medianConfidence * (0.75 + 0.25 * consistency)
                         * (0.75 + 0.25 * acceptanceRatio),
        0.0, 1.0));
    result.status = AggregateStatus::success;
    return result;
}
}
