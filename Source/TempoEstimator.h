#pragma once

#include <JuceHeader.h>

#include <vector>

/** One tempo hypothesis. Confidence is normalised to [0, 1]. */
struct TempoCandidate
{
    double bpm = 0.0;
    double confidence = 0.0;
};

/**
 * Deterministic, dependency-free tempo estimation for imported accompaniment.
 *
 * The analyser builds a 200 Hz onset envelope and correlates it over the
 * beginner-facing range 60-200 BPM. It does not modify the source buffer.
 */
class TempoEstimator
{
public:
    /**
     * Returns up to three candidates in confidence/ranking order.
     *
     * Silence, non-finite data, invalid sample rates, very short input, and a
     * non-positive `maximumCandidates` return an empty vector. This is an
     * offline analysis function and may allocate its result/work buffers.
     */
    static std::vector<TempoCandidate> estimate(
        const juce::AudioBuffer<float>& audio,
        double sampleRate,
        int maximumCandidates = 3);

    static constexpr double minimumBpm = 60.0;
    static constexpr double maximumBpm = 200.0;
};
