#include "TempoEstimator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr double envelopeFramesPerSecond = 200.0;
constexpr double minimumAnalysisSeconds = 2.0;

struct RankedCandidate
{
    TempoCandidate candidate;
    double rank = 0.0;
};

double tempoPreference(double bpm) noexcept
{
    // A weak prior breaks exact half/double-time ties while keeping genuine
    // slow and fast candidates available to the user.
    const auto octavesFrom120 = std::log2(std::max(1.0, bpm) / 120.0);
    return 0.82 + 0.18 * std::exp(-0.5 * octavesFrom120 * octavesFrom120
                                  / (0.80 * 0.80));
}

bool isFiniteBuffer(const juce::AudioBuffer<float>& audio) noexcept
{
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
    {
        const auto* samples = audio.getReadPointer(channel);
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            if (!std::isfinite(samples[sample]))
                return false;
    }
    return true;
}
}

std::vector<TempoCandidate> TempoEstimator::estimate(
    const juce::AudioBuffer<float>& audio,
    double sampleRate,
    int maximumCandidates)
{
    std::vector<TempoCandidate> result;
    if (!std::isfinite(sampleRate) || sampleRate < 8000.0
        || sampleRate > 384000.0 || maximumCandidates <= 0
        || audio.getNumChannels() <= 0 || audio.getNumSamples() <= 0
        || static_cast<double>(audio.getNumSamples()) / sampleRate
            < minimumAnalysisSeconds
        || !isFiniteBuffer(audio))
        return result;

    const auto requestedCount = std::min(maximumCandidates, 3);
    const auto hopSamples = std::max(1,
        static_cast<int>(std::lround(sampleRate / envelopeFramesPerSecond)));
    const auto actualEnvelopeRate = sampleRate / hopSamples;
    const auto frameCount = (audio.getNumSamples() + hopSamples - 1)
                          / hopSamples;
    const auto maximumLag = static_cast<int>(std::ceil(
        60.0 * actualEnvelopeRate / minimumBpm));
    if (frameCount <= maximumLag + 2)
        return result;

    std::vector<double> frameEnergy(static_cast<size_t>(frameCount), 0.0);
    double maximumRms = 0.0;
    for (int frame = 0; frame < frameCount; ++frame)
    {
        const auto start = frame * hopSamples;
        const auto length = std::min(hopSamples,
                                     audio.getNumSamples() - start);
        long double sumSquares = 0.0;
        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        {
            const auto* samples = audio.getReadPointer(channel, start);
            for (int sample = 0; sample < length; ++sample)
            {
                const auto value = static_cast<long double>(samples[sample]);
                sumSquares += value * value;
            }
        }
        const auto divisor = static_cast<long double>(
            std::max(1, length * audio.getNumChannels()));
        const auto rms = std::sqrt(static_cast<double>(sumSquares / divisor));
        frameEnergy[static_cast<size_t>(frame)] = rms;
        maximumRms = std::max(maximumRms, rms);
    }
    if (!std::isfinite(maximumRms) || maximumRms < 1.0e-5)
        return result;

    std::vector<double> onset(static_cast<size_t>(frameCount), 0.0);
    double previous = frameEnergy.front();
    double slowAverage = previous;
    long double onsetSquareSum = 0.0;
    for (int frame = 1; frame < frameCount; ++frame)
    {
        const auto current = frameEnergy[static_cast<size_t>(frame)];
        slowAverage += 0.025 * (current - slowAverage);
        const auto positiveFlux = std::max(0.0, current - previous * 0.82);
        const auto adaptiveFloor = std::max(0.0, slowAverage * 0.12);
        const auto strength = std::max(0.0, positiveFlux - adaptiveFloor);
        onset[static_cast<size_t>(frame)] = strength;
        onsetSquareSum += static_cast<long double>(strength * strength);
        previous = current;
    }
    if (onsetSquareSum <= 1.0e-12L)
        return result;

    const auto minimumLag = std::max(1, static_cast<int>(std::floor(
        60.0 * actualEnvelopeRate / maximumBpm)));
    const auto safeMaximumLag = std::min(maximumLag, frameCount - 2);
    if (safeMaximumLag <= minimumLag)
        return result;

    std::vector<double> correlations(static_cast<size_t>(safeMaximumLag + 1),
                                     0.0);
    for (int lag = minimumLag; lag <= safeMaximumLag; ++lag)
    {
        long double cross = 0.0;
        long double leftEnergy = 0.0;
        long double rightEnergy = 0.0;
        for (int frame = lag; frame < frameCount; ++frame)
        {
            const auto left = onset[static_cast<size_t>(frame)];
            const auto right = onset[static_cast<size_t>(frame - lag)];
            cross += static_cast<long double>(left * right);
            leftEnergy += static_cast<long double>(left * left);
            rightEnergy += static_cast<long double>(right * right);
        }
        const auto normaliser = std::sqrt(leftEnergy * rightEnergy);
        if (normaliser > 1.0e-15L)
        {
            correlations[static_cast<size_t>(lag)] = std::clamp(
                static_cast<double>(cross / normaliser), 0.0, 1.0);
        }
    }

    std::vector<RankedCandidate> ranked;
    ranked.reserve(static_cast<size_t>(safeMaximumLag - minimumLag + 1));
    for (int lag = minimumLag; lag <= safeMaximumLag; ++lag)
    {
        const auto score = correlations[static_cast<size_t>(lag)];
        const auto previousScore = lag > minimumLag
            ? correlations[static_cast<size_t>(lag - 1)] : -1.0;
        const auto nextScore = lag < safeMaximumLag
            ? correlations[static_cast<size_t>(lag + 1)] : -1.0;
        if (score < 0.05 || score < previousScore || score < nextScore)
            continue;

        double refinedLag = static_cast<double>(lag);
        if (lag > minimumLag && lag < safeMaximumLag)
        {
            const auto denominator = previousScore - 2.0 * score + nextScore;
            if (std::abs(denominator) > 1.0e-12)
            {
                const auto offset = std::clamp(
                    0.5 * (previousScore - nextScore) / denominator,
                    -0.5,
                    0.5);
                refinedLag += offset;
            }
        }

        const auto bpm = 60.0 * actualEnvelopeRate / refinedLag;
        if (!std::isfinite(bpm) || bpm < minimumBpm || bpm > maximumBpm)
            continue;
        ranked.push_back({ { bpm, score },
                           score * tempoPreference(bpm) });
    }

    std::sort(ranked.begin(), ranked.end(),
        [](const RankedCandidate& left, const RankedCandidate& right)
        {
            if (std::abs(left.candidate.confidence
                         - right.candidate.confidence) > 1.0e-12)
                return left.candidate.confidence > right.candidate.confidence;
            if (std::abs(left.rank - right.rank) > 1.0e-12)
                return left.rank > right.rank;
            return left.candidate.bpm < right.candidate.bpm;
        });

    result.reserve(static_cast<size_t>(requestedCount));
    for (const auto& item : ranked)
    {
        const auto tooClose = std::any_of(result.begin(), result.end(),
            [&item](const TempoCandidate& accepted)
            {
                return std::abs(accepted.bpm - item.candidate.bpm)
                    < std::max(1.0, accepted.bpm * 0.015);
            });
        if (tooClose)
            continue;
        result.push_back(item.candidate);
        if (static_cast<int>(result.size()) >= requestedCount)
            break;
    }
    return result;
}
