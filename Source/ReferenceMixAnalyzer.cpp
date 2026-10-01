#include "ReferenceMixAnalyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace
{
constexpr float minimumDb = -100.0f;
constexpr double silencePower = 1.0e-12;

float clampUnit(float value) noexcept
{
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

float safeDb(double amplitude) noexcept
{
    if (!std::isfinite(amplitude) || amplitude <= 1.0e-5)
        return minimumDb;
    return std::max(minimumDb,
                    static_cast<float>(20.0 * std::log10(amplitude)));
}

double lowPassCoefficient(double cutoff, double sampleRate) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return 0.0;
    const auto safeCutoff = std::clamp(cutoff, 10.0, sampleRate * 0.45);
    return 1.0 - std::exp(-2.0 * juce::MathConstants<double>::pi
                          * safeCutoff / sampleRate);
}

float preferredPanScale(float stereoWidth) noexcept
{
    return std::clamp(0.45f + stereoWidth * 1.35f, 0.45f, 1.35f);
}

ReferenceMixFeatures aggregateTarget(
    const std::vector<AutoMixAnalysis>& analyses) noexcept
{
    ReferenceMixFeatures result;
    double weightedPower = 0.0;
    double weightedLow = 0.0;
    double weightedMid = 0.0;
    double weightedHigh = 0.0;
    double weightedNoise = 0.0;
    double totalWeight = 0.0;
    float maximumPeak = minimumDb;

    for (const auto& analysis : analyses)
    {
        if (!analysis.valid || analysis.analysedSeconds <= 0.0)
            continue;

        const auto power = std::pow(10.0,
                                    static_cast<double>(analysis.rmsDb) / 10.0);
        const auto weight = std::max(0.001, analysis.analysedSeconds);
        weightedPower += power * weight;
        weightedLow += analysis.lowEnergy * power * weight;
        weightedMid += analysis.midEnergy * power * weight;
        weightedHigh += analysis.highEnergy * power * weight;
        const auto noiseGap = analysis.rmsDb - analysis.noiseFloorDb;
        weightedNoise += clampUnit((18.0f - noiseGap) / 14.0f) * weight;
        totalWeight += weight;
        maximumPeak = std::max(maximumPeak, analysis.peakDb);
        result.analysedSeconds += analysis.analysedSeconds;
    }

    if (totalWeight <= 0.0 || weightedPower <= silencePower)
        return result;

    const auto bandTotal = weightedLow + weightedMid + weightedHigh;
    result.rmsDb = safeDb(std::sqrt(weightedPower / totalWeight));
    result.peakDb = maximumPeak;
    result.crestFactorDb = std::clamp(result.peakDb - result.rmsDb,
                                      0.0f, 40.0f);
    if (bandTotal > silencePower)
    {
        result.lowEnergy = clampUnit(
            static_cast<float>(weightedLow / bandTotal));
        result.midEnergy = clampUnit(
            static_cast<float>(weightedMid / bandTotal));
        result.highEnergy = clampUnit(
            static_cast<float>(weightedHigh / bandTotal));
        result.brightness = clampUnit(0.12f * result.lowEnergy
                                    + 0.55f * result.midEnergy
                                    + result.highEnergy);
    }
    result.noiseAmount = clampUnit(
        static_cast<float>(weightedNoise / totalWeight));
    result.noiseFloorDb = result.rmsDb - (18.0f - 14.0f * result.noiseAmount);
    result.valid = true;
    return result;
}

void appendEffect(std::vector<ReferenceSpecialFxSuggestion>& destination,
                  SpecialFxType type,
                  float confidence,
                  float amount,
                  const juce::String& reason)
{
    confidence = clampUnit(confidence);
    if (confidence < 0.56f)
        return;

    ReferenceSpecialFxSuggestion suggestion;
    suggestion.type = type;
    suggestion.confidence = confidence;
    suggestion.amount = std::clamp(amount, 0.20f, 0.90f);
    suggestion.reason = reason;
    destination.push_back(std::move(suggestion));
}

std::vector<ReferenceSpecialFxSuggestion> detectSpecialFx(
    const ReferenceMixFeatures& features)
{
    std::vector<ReferenceSpecialFxSuggestion> result;

    const auto midDominance = clampUnit((features.midEnergy - 0.48f) / 0.35f);
    const auto missingBass = clampUnit((0.23f - features.lowEnergy) / 0.20f);
    const auto missingTreble = clampUnit((0.16f - features.highEnergy) / 0.14f);
    const auto radioConfidence = clampUnit(0.50f * midDominance
                                         + 0.25f * missingBass
                                         + 0.25f * missingTreble);

    const auto darkTone = clampUnit((0.38f - features.brightness) / 0.24f);
    const auto rolledOffTop = clampUnit((0.18f - features.highEnergy) / 0.17f);
    auto muffledConfidence = clampUnit(0.62f * darkTone
                                     + 0.38f * rolledOffTop);
    // A band-limited radio sound is more specific than a generic muffled one.
    if (radioConfidence >= 0.62f)
        muffledConfidence *= 0.55f;

    const auto noisyConfidence = clampUnit(
        0.68f * features.noiseAmount
        + 0.32f * clampUnit((features.zeroCrossingRate - 0.07f) / 0.18f));

    const auto lowCrest = clampUnit((9.0f - features.crestFactorDb) / 5.0f);
    const auto clipped = clampUnit(features.clippingRatio / 0.012f);
    const auto roughHighs = clampUnit((features.highEnergy - 0.15f) / 0.30f);
    const auto distortionConfidence = clampUnit(0.58f * clipped
                                              + 0.27f * lowCrest
                                              + 0.15f * roughHighs);

    const auto heldOrQuantised = features.coarseQuantisation;
    const auto bitcrushConfidence = clampUnit(
        0.78f * heldOrQuantised
        + 0.22f * clampUnit((features.zeroCrossingRate - 0.10f) / 0.25f));

    appendEffect(result, SpecialFxType::radio, radioConfidence,
                 0.38f + 0.45f * radioConfidence,
                 "中域が強く、低音と高音が狭いラジオ風の傾向があります。");
    appendEffect(result, SpecialFxType::muffled, muffledConfidence,
                 0.30f + 0.52f * muffledConfidence,
                 "高音が控えめで、こもった質感の傾向があります。");
    appendEffect(result, SpecialFxType::noise, noisyConfidence,
                 0.22f + 0.48f * noisyConfidence,
                 "小さい音の部分にも持続成分があり、ノイズ感があります。");
    appendEffect(result, SpecialFxType::distortion, distortionConfidence,
                 0.20f + 0.50f * distortionConfidence,
                 "ピークの潰れ方と高域から、歪んだ質感が考えられます。");
    appendEffect(result, SpecialFxType::bitcrush, bitcrushConfidence,
                 0.24f + 0.56f * bitcrushConfidence,
                 "粗い量子化または同じ値の保持が多く、デジタル劣化の傾向があります。");

    std::stable_sort(result.begin(), result.end(),
                     [](const auto& left, const auto& right)
                     {
                         return left.confidence > right.confidence;
                     });
    return result;
}
}

ReferenceMixFeatures ReferenceMixAnalyzer::analyseReference(
    const juce::AudioBuffer<float>& buffer,
    double sampleRate) noexcept
{
    ReferenceMixFeatures result;
    try
    {
        const auto channels = buffer.getNumChannels();
        const auto samples = buffer.getNumSamples();
        if (!std::isfinite(sampleRate) || sampleRate < 1000.0
            || channels <= 0 || samples <= 0)
            return result;

        const auto lowCoefficient = lowPassCoefficient(250.0, sampleRate);
        const auto midCoefficient = lowPassCoefficient(4000.0, sampleRate);
        std::vector<double> lowState(static_cast<size_t>(channels), 0.0);
        std::vector<double> midState(static_cast<size_t>(channels), 0.0);
        std::vector<float> previous(static_cast<size_t>(channels), 0.0f);

        const auto frameLength = std::max(
            1, static_cast<int>(std::llround(sampleRate * 0.02)));
        std::vector<float> frameRmsDb;
        frameRmsDb.reserve(static_cast<size_t>(samples / frameLength + 1));

        double totalPower = 0.0;
        double lowPower = 0.0;
        double midPower = 0.0;
        double highPower = 0.0;
        double middlePower = 0.0;
        double sidePower = 0.0;
        double framePower = 0.0;
        int frameSamples = 0;
        std::int64_t finiteSampleCount = 0;
        std::int64_t crossingCount = 0;
        std::int64_t clippingCount = 0;
        std::int64_t quantisationMatches = 0;
        std::int64_t quantisationChecks = 0;
        std::int64_t heldSampleCount = 0;
        std::int64_t heldSampleChecks = 0;
        float peak = 0.0f;

        // Quantisation checks are uniformly sub-sampled so very long files do
        // not make analysis latency or memory use surprising.
        const auto analysisStride = std::max(1, samples / 200000);

        for (int sample = 0; sample < samples; ++sample)
        {
            double instantPower = 0.0;
            double instantLowPower = 0.0;
            double instantMidPower = 0.0;
            double instantHighPower = 0.0;

            for (int channel = 0; channel < channels; ++channel)
            {
                const auto raw = buffer.getSample(channel, sample);
                const auto value = std::isfinite(raw) ? raw : 0.0f;
                peak = std::max(peak, std::abs(value));
                if (std::abs(value) >= 0.985f)
                    ++clippingCount;

                auto& low = lowState[static_cast<size_t>(channel)];
                auto& belowFourK = midState[static_cast<size_t>(channel)];
                low += lowCoefficient * (value - low);
                belowFourK += midCoefficient * (value - belowFourK);
                const auto mid = belowFourK - low;
                const auto high = value - belowFourK;
                instantPower += value * value;
                instantLowPower += low * low;
                instantMidPower += mid * mid;
                instantHighPower += high * high;

                const auto old = previous[static_cast<size_t>(channel)];
                if ((old < 0.0f && value >= 0.0f)
                    || (old >= 0.0f && value < 0.0f))
                    ++crossingCount;

                if (sample % analysisStride == 0 && std::abs(value) > 0.015f)
                {
                    // A true 8-bit style render stays close to this coarse
                    // grid. Lossy encoding may reduce confidence, which is why
                    // this remains a suggestion rather than a classification.
                    constexpr float quantisationStep = 1.0f / 127.0f;
                    const auto quantised = std::round(value * 127.0f) / 127.0f;
                    if (std::abs(value - quantised)
                        <= quantisationStep * 0.035f)
                        ++quantisationMatches;
                    ++quantisationChecks;

                    if (sample > 0)
                    {
                        if (std::abs(value - old) < 1.0e-7f)
                            ++heldSampleCount;
                        ++heldSampleChecks;
                    }
                }
                previous[static_cast<size_t>(channel)] = value;
                ++finiteSampleCount;
            }

            const auto inverseChannels = 1.0 / static_cast<double>(channels);
            instantPower *= inverseChannels;
            totalPower += instantPower;
            lowPower += instantLowPower * inverseChannels;
            midPower += instantMidPower * inverseChannels;
            highPower += instantHighPower * inverseChannels;
            framePower += instantPower;

            if (channels >= 2)
            {
                const auto leftRaw = buffer.getSample(0, sample);
                const auto rightRaw = buffer.getSample(1, sample);
                const auto left = std::isfinite(leftRaw) ? leftRaw : 0.0f;
                const auto right = std::isfinite(rightRaw) ? rightRaw : 0.0f;
                const auto middle = 0.5 * (left + right);
                const auto side = 0.5 * (left - right);
                middlePower += middle * middle;
                sidePower += side * side;
            }

            if (++frameSamples >= frameLength || sample + 1 == samples)
            {
                frameRmsDb.push_back(safeDb(std::sqrt(
                    framePower / static_cast<double>(frameSamples))));
                framePower = 0.0;
                frameSamples = 0;
            }
        }

        if (finiteSampleCount <= 0 || totalPower <= silencePower)
            return result;

        const auto rms = std::sqrt(totalPower / static_cast<double>(samples));
        result.analysedSeconds = static_cast<double>(samples) / sampleRate;
        result.rmsDb = safeDb(rms);
        result.peakDb = safeDb(peak);
        result.crestFactorDb = std::clamp(result.peakDb - result.rmsDb,
                                          0.0f, 40.0f);

        const auto bandPower = lowPower + midPower + highPower;
        if (bandPower > silencePower)
        {
            result.lowEnergy = clampUnit(
                static_cast<float>(lowPower / bandPower));
            result.midEnergy = clampUnit(
                static_cast<float>(midPower / bandPower));
            result.highEnergy = clampUnit(
                static_cast<float>(highPower / bandPower));
            result.brightness = clampUnit(0.12f * result.lowEnergy
                                        + 0.55f * result.midEnergy
                                        + result.highEnergy);
        }

        frameRmsDb.erase(
            std::remove_if(frameRmsDb.begin(), frameRmsDb.end(),
                           [](float value)
                           {
                               return !std::isfinite(value)
                                   || value <= minimumDb;
                           }),
            frameRmsDb.end());
        if (!frameRmsDb.empty())
        {
            std::sort(frameRmsDb.begin(), frameRmsDb.end());
            const auto index = static_cast<size_t>(std::floor(
                0.15 * static_cast<double>(frameRmsDb.size() - 1)));
            const auto candidate = frameRmsDb[index];
            result.noiseFloorDb = result.rmsDb - candidate >= 4.0f
                ? candidate : result.rmsDb - 4.0f;
            const auto noiseGap = result.rmsDb - result.noiseFloorDb;
            result.noiseAmount = clampUnit((18.0f - noiseGap) / 14.0f);
        }

        if (channels >= 2 && middlePower + sidePower > silencePower)
            result.stereoWidth = clampUnit(static_cast<float>(std::sqrt(
                sidePower / (middlePower + sidePower))));

        result.zeroCrossingRate = clampUnit(static_cast<float>(
            static_cast<double>(crossingCount)
            / static_cast<double>(finiteSampleCount)));
        result.clippingRatio = clampUnit(static_cast<float>(
            static_cast<double>(clippingCount)
            / static_cast<double>(finiteSampleCount)));

        const auto gridFit = quantisationChecks > 0
            ? static_cast<float>(quantisationMatches)
                / static_cast<float>(quantisationChecks)
            : 0.0f;
        const auto holdFit = heldSampleChecks > 0
            ? static_cast<float>(heldSampleCount)
                / static_cast<float>(heldSampleChecks)
            : 0.0f;
        result.coarseQuantisation = clampUnit(
            0.72f * clampUnit((gridFit - 0.05f) / 0.42f)
            + 0.28f * clampUnit((holdFit - 0.015f) / 0.24f));
        result.valid = std::isfinite(result.rmsDb)
                    && result.rmsDb > minimumDb;
    }
    catch (...)
    {
        return ReferenceMixFeatures {};
    }
    return result;
}

ReferenceMixResult ReferenceMixAnalyzer::analyseAndSuggest(
    const juce::AudioBuffer<float>& referenceBuffer,
    double referenceSampleRate,
    const std::vector<TrackData>& targetTracks) noexcept
{
    ReferenceMixResult result;
    result.limitation = "完成済み音源から元のプラグインや個別設定は特定できません。"
                        "音の明るさ、広がり、音量差、質感を似た方向へ調整します。";
    try
    {
        if (!std::isfinite(referenceSampleRate)
            || referenceSampleRate < 1000.0)
        {
            result.message = "参考曲のサンプルレートが正しくありません。";
            return result;
        }
        if (referenceBuffer.getNumChannels() <= 0
            || referenceBuffer.getNumSamples() <= 0)
        {
            result.message = "参考曲に解析できる音声がありません。";
            return result;
        }
        if (targetTracks.empty())
        {
            result.message = "MIXするトラックがありません。";
            return result;
        }

        result.reference = analyseReference(referenceBuffer,
                                             referenceSampleRate);
        if (!result.reference.valid)
        {
            result.message = "参考曲が無音、または解析できない音声です。";
            return result;
        }

        std::vector<AutoMixAnalysis> targetAnalyses;
        targetAnalyses.reserve(targetTracks.size());
        for (const auto& track : targetTracks)
            targetAnalyses.push_back(AutoMixAnalyzer::analyseTrack(track));
        result.target = aggregateTarget(targetAnalyses);
        if (!result.target.valid)
        {
            result.message = "対象トラックに解析できる音声がありません。";
            return result;
        }

        const auto baseMix = AutoMixAnalyzer::analyseAndSuggest(targetTracks);
        result.tracks.reserve(targetTracks.size());
        const auto brightnessDelta = std::clamp(
            result.reference.brightness - result.target.brightness,
            -0.45f, 0.45f);
        const auto dynamicsDelta = std::clamp(
            result.target.crestFactorDb - result.reference.crestFactorDb,
            -10.0f, 10.0f);
        const auto panScale = preferredPanScale(result.reference.stereoWidth);

        int validCount = 0;
        for (size_t index = 0; index < targetTracks.size(); ++index)
        {
            ReferenceMixTrackSuggestion suggestion;
            suggestion.trackId = targetTracks[index].id;
            suggestion.role = targetTracks[index].role;
            suggestion.sourceAnalysis = targetAnalyses[index];
            suggestion.analysisValid = targetAnalyses[index].valid;

            if (index < baseMix.tracks.size())
            {
                const auto& base = baseMix.tracks[index];
                suggestion.simpleMix = base.simpleMix;
                suggestion.noiseReduction = base.noiseReduction;
                suggestion.volume = base.volume;
                suggestion.pan = base.pan;
                suggestion.warning = base.warning;
            }

            if (!suggestion.analysisValid)
            {
                if (suggestion.warning.isEmpty())
                    suggestion.warning =
                        "解析できる音声がないため、このトラックは変更しません。";
                result.tracks.push_back(std::move(suggestion));
                continue;
            }

            ++validCount;
            const auto tonalWeight = suggestion.role == TrackRole::mainVocal
                ? 0.55f : suggestion.role == TrackRole::accompaniment
                ? 0.38f : 0.46f;
            suggestion.simpleMix.enabled = true;
            suggestion.simpleMix.brightness = std::clamp(
                suggestion.simpleMix.brightness
                    + tonalWeight * brightnessDelta,
                -1.0f, 1.0f);
            suggestion.simpleMix.stability = clampUnit(
                suggestion.simpleMix.stability
                    + 0.032f * dynamicsDelta);

            if (result.reference.stereoWidth > 0.50f
                && (suggestion.role == TrackRole::chorus
                    || suggestion.role == TrackRole::harmony
                    || suggestion.role == TrackRole::other))
            {
                suggestion.simpleMix.preset = MixPreset::wide;
                suggestion.simpleMix.ambience = clampUnit(
                    suggestion.simpleMix.ambience
                        + 0.20f * (result.reference.stereoWidth - 0.50f));
            }
            suggestion.pan = std::clamp(suggestion.pan * panScale,
                                        -0.85f, 0.85f);

            const auto tonalDistance = std::abs(brightnessDelta);
            const auto dynamicsDistance = std::abs(dynamicsDelta) / 10.0f;
            suggestion.matchConfidence = clampUnit(
                0.82f - 0.22f * tonalDistance
                      - 0.12f * dynamicsDistance);
            result.tracks.push_back(std::move(suggestion));
        }

        result.specialFx = detectSpecialFx(result.reference);
        const auto durationConfidence = clampUnit(static_cast<float>(
            result.reference.analysedSeconds / 8.0));
        const auto coverage = static_cast<float>(validCount)
                            / static_cast<float>(targetTracks.size());
        result.overallConfidence = clampUnit(
            (0.50f + 0.30f * durationConfidence) * coverage);
        result.success = validCount > 0;
        result.message = result.specialFx.empty()
            ? "参考曲の雰囲気に近づけるお任せMIX設定を作成しました。"
            : "参考曲の雰囲気に近づけるMIX設定と特殊効果の候補を作成しました。";
    }
    catch (...)
    {
        result.success = false;
        result.message = "参考曲の解析中に問題が発生しました。";
    }
    return result;
}
