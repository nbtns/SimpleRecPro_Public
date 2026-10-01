#pragma once

#include "AutoMixAnalyzer.h"

#include <vector>

/**
 * Perceptual measurements extracted from a rendered reference or target mix.
 *
 * Band values, brightness, noiseAmount, and stereoWidth are normalised to
 * 0..1. They describe audible tendencies only; they do not identify the
 * original plug-in or processing chain.
 */
struct ReferenceMixFeatures
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
    float noiseFloorDb = -100.0f;
    float noiseAmount = 0.0f;
    float stereoWidth = 0.0f;
    float zeroCrossingRate = 0.0f;
    float clippingRatio = 0.0f;
    float coarseQuantisation = 0.0f;
};

struct ReferenceSpecialFxSuggestion
{
    SpecialFxType type = SpecialFxType::none;
    float amount = 0.0f;
    float confidence = 0.0f;
    juce::String reason;
};

struct ReferenceMixTrackSuggestion
{
    juce::String trackId;
    TrackRole role = TrackRole::unknown;
    AutoMixAnalysis sourceAnalysis;
    SimpleMixSettings simpleMix;
    NoiseReductionSettings noiseReduction;
    float volume = 1.0f;
    float pan = 0.0f;
    float matchConfidence = 0.0f;
    bool analysisValid = false;
    juce::String warning;
};

struct ReferenceMixResult
{
    bool success = false;
    juce::String message;
    ReferenceMixFeatures reference;
    ReferenceMixFeatures target;
    std::vector<ReferenceMixTrackSuggestion> tracks;
    std::vector<ReferenceSpecialFxSuggestion> specialFx;
    float overallConfidence = 0.0f;

    // Deliberately user-facing: a rendered song can reveal a sound tendency,
    // but not the exact source tracks, automation, or proprietary plug-in.
    juce::String limitation;
};

/**
 * Offline and deterministic reference-song matching.
 *
 * The analyser never mutates the supplied buffer or TrackData. It reuses the
 * role-aware AutoMixAnalyzer for safe track balance, then nudges those settings
 * toward the reference's broad tone and dynamics. All public methods swallow
 * exceptions; invalid input is reported through `valid`, `success`, and the
 * result message.
 */
class ReferenceMixAnalyzer
{
public:
    static ReferenceMixFeatures analyseReference(
        const juce::AudioBuffer<float>& buffer,
        double sampleRate) noexcept;

    static ReferenceMixResult analyseAndSuggest(
        const juce::AudioBuffer<float>& referenceBuffer,
        double referenceSampleRate,
        const std::vector<TrackData>& targetTracks) noexcept;
};
