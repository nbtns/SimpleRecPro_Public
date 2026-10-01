#pragma once

#include "TrackData.h"

#include <vector>

struct AutoMixAnalysis
{
    bool valid = false;
    double analysedSeconds = 0.0;
    float rmsDb = -100.0f;
    float peakDb = -100.0f;
    float crestFactorDb = 0.0f;
    float lowEnergy = 0.0f;
    float midEnergy = 0.0f;
    float highEnergy = 0.0f;
    float brightness = 0.0f;
    float sibilance = 0.0f;
    float noiseFloorDb = -100.0f;
};

struct AutoMixTrackSuggestion
{
    juce::String trackId;
    TrackRole role = TrackRole::unknown;
    AutoMixAnalysis analysis;
    SimpleMixSettings simpleMix;
    NoiseReductionSettings noiseReduction;
    float volume = 1.0f;
    float pan = 0.0f;
    float maskingScore = 0.0f;
    bool analysisValid = false;
    juce::String warning;
};

struct AutoMixResult
{
    bool success = false;
    juce::String message;
    std::vector<AutoMixTrackSuggestion> tracks;
};

/**
 * Offline, deterministic analysis used by the role-aware automatic mix.
 *
 * No method mutates TrackData or its audio buffers. Callers can therefore
 * analyse a project snapshot on a worker thread and decide separately whether
 * to apply the returned suggestions.
 */
class AutoMixAnalyzer
{
public:
    static AutoMixAnalysis analyseTrack(const TrackData& track) noexcept;

    static float calculateMasking(const AutoMixAnalysis& vocal,
                                  const AutoMixAnalysis& accompaniment) noexcept;

    static AutoMixTrackSuggestion suggestTrack(
        const juce::String& trackId,
        TrackRole role,
        const AutoMixAnalysis& analysis,
        float maskingScore = 0.0f,
        int roleOrdinal = 0,
        MixPreset preferredPreset = MixPreset::flat) noexcept;

    static AutoMixResult analyseAndSuggest(
        const std::vector<TrackData>& tracks) noexcept;
};
