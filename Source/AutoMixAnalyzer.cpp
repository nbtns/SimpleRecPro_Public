#include "AutoMixAnalyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
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

float gainFromDb(float decibels) noexcept
{
    if (!std::isfinite(decibels))
        return 1.0f;
    return static_cast<float>(std::pow(10.0, decibels / 20.0));
}

double lowPassCoefficient(double cutoff, double sampleRate) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return 0.0;
    const auto safeCutoff = std::clamp(cutoff, 10.0, sampleRate * 0.45);
    return 1.0 - std::exp(-2.0 * juce::MathConstants<double>::pi
                          * safeCutoff / sampleRate);
}

struct AnalysisAccumulator
{
    double totalPower = 0.0;
    double lowPower = 0.0;
    double midPower = 0.0;
    double highPower = 0.0;
    double sibilancePower = 0.0;
    double analysedSeconds = 0.0;
    float peak = 0.0f;
    std::vector<float> frameRmsDb;
};

using TimelineInterval = std::array<double, 2>;

std::vector<TimelineInterval> mergedTimelineIntervals(
    const TrackData& track)
{
    std::vector<TimelineInterval> intervals;
    intervals.reserve(track.clips.size());
    for (const auto& clip : track.clips)
    {
        if (!std::isfinite(clip.startTime))
            continue;
        auto duration = clip.duration;
        if ((!std::isfinite(duration) || duration <= 0.0)
            && clip.buffer != nullptr && clip.sampleRate > 0)
            duration = std::max(
                0.0,
                static_cast<double>(clip.buffer->getNumSamples())
                    / clip.sampleRate
                - std::max(0.0, std::isfinite(clip.offset)
                                     ? clip.offset : 0.0));
        if (!std::isfinite(duration) || duration <= 0.0)
            continue;
        intervals.push_back({ clip.startTime, clip.startTime + duration });
    }
    std::sort(intervals.begin(), intervals.end(),
              [](const auto& left, const auto& right)
              {
                  return left[0] < right[0];
              });
    std::vector<TimelineInterval> merged;
    for (const auto& interval : intervals)
    {
        if (merged.empty() || interval[0] > merged.back()[1])
            merged.push_back(interval);
        else
            merged.back()[1] = std::max(merged.back()[1], interval[1]);
    }
    return merged;
}

float timelineOverlapRatio(const TrackData& first,
                           const TrackData& second) noexcept
{
    try
    {
        const auto firstIntervals = mergedTimelineIntervals(first);
        const auto secondIntervals = mergedTimelineIntervals(second);
        double firstDuration = 0.0;
        double secondDuration = 0.0;
        for (const auto& interval : firstIntervals)
            firstDuration += interval[1] - interval[0];
        for (const auto& interval : secondIntervals)
            secondDuration += interval[1] - interval[0];
        const auto referenceDuration = std::min(firstDuration, secondDuration);
        if (referenceDuration <= 0.0)
            return 0.0f;

        double overlap = 0.0;
        size_t firstIndex = 0;
        size_t secondIndex = 0;
        while (firstIndex < firstIntervals.size()
               && secondIndex < secondIntervals.size())
        {
            const auto& firstInterval = firstIntervals[firstIndex];
            const auto& secondInterval = secondIntervals[secondIndex];
            overlap += std::max(
                0.0,
                std::min(firstInterval[1], secondInterval[1])
                    - std::max(firstInterval[0], secondInterval[0]));
            if (firstInterval[1] < secondInterval[1])
                ++firstIndex;
            else
                ++secondIndex;
        }
        return clampUnit(static_cast<float>(overlap / referenceDuration));
    }
    catch (...)
    {
        return 0.0f;
    }
}

void analyseClip(const AudioClip& clip, AnalysisAccumulator& result)
{
    if (clip.buffer == nullptr || clip.sampleRate <= 0
        || clip.buffer->getNumChannels() <= 0
        || clip.buffer->getNumSamples() <= 0)
        return;

    const auto& buffer = *clip.buffer;
    const auto sampleRate = static_cast<double>(clip.sampleRate);
    const auto sourceStart = std::clamp(
        static_cast<int64_t>(std::llround(
            std::max(0.0, std::isfinite(clip.offset) ? clip.offset : 0.0)
            * sampleRate)),
        int64_t { 0 },
        static_cast<int64_t>(buffer.getNumSamples()));
    const auto available = static_cast<int64_t>(buffer.getNumSamples())
                         - sourceStart;
    const auto requested = clip.duration > 0.0 && std::isfinite(clip.duration)
        ? static_cast<int64_t>(std::llround(clip.duration * sampleRate))
        : available;
    const auto sampleCount = std::clamp(requested, int64_t { 0 }, available);
    if (sampleCount <= 0)
        return;

    const auto channels = buffer.getNumChannels();
    const auto lowCoefficient = lowPassCoefficient(250.0, sampleRate);
    const auto midCoefficient = lowPassCoefficient(4000.0, sampleRate);
    const auto sibilanceLowCoefficient = lowPassCoefficient(4500.0, sampleRate);
    const auto sibilanceHighCoefficient = lowPassCoefficient(11000.0, sampleRate);
    std::vector<double> lowState(static_cast<size_t>(channels), 0.0);
    std::vector<double> midState(static_cast<size_t>(channels), 0.0);
    std::vector<double> sibilanceLowState(static_cast<size_t>(channels), 0.0);
    std::vector<double> sibilanceHighState(static_cast<size_t>(channels), 0.0);

    const auto frameLength = std::max<int64_t>(1,
        static_cast<int64_t>(std::llround(sampleRate * 0.02)));
    double framePower = 0.0;
    int64_t frameSamples = 0;

    for (int64_t relativeSample = 0; relativeSample < sampleCount;
         ++relativeSample)
    {
        const auto sampleIndex = static_cast<int>(sourceStart + relativeSample);
        double instantPower = 0.0;
        double instantLowPower = 0.0;
        double instantMidPower = 0.0;
        double instantHighPower = 0.0;
        double instantSibilancePower = 0.0;

        for (int channel = 0; channel < channels; ++channel)
        {
            const auto raw = static_cast<double>(
                buffer.getSample(channel, sampleIndex));
            const auto value = std::isfinite(raw) ? raw : 0.0;
            result.peak = std::max(result.peak,
                                   static_cast<float>(std::abs(value)));

            auto& low = lowState[static_cast<size_t>(channel)];
            auto& belowFourK = midState[static_cast<size_t>(channel)];
            auto& belowSibilance = sibilanceLowState[
                static_cast<size_t>(channel)];
            auto& belowElevenK = sibilanceHighState[
                static_cast<size_t>(channel)];
            low += lowCoefficient * (value - low);
            belowFourK += midCoefficient * (value - belowFourK);
            belowSibilance += sibilanceLowCoefficient
                            * (value - belowSibilance);
            belowElevenK += sibilanceHighCoefficient
                          * (value - belowElevenK);

            const auto mid = belowFourK - low;
            const auto high = value - belowFourK;
            const auto sibilance = belowElevenK - belowSibilance;
            instantPower += value * value;
            instantLowPower += low * low;
            instantMidPower += mid * mid;
            instantHighPower += high * high;
            instantSibilancePower += sibilance * sibilance;
        }

        const auto inverseChannels = 1.0 / static_cast<double>(channels);
        instantPower *= inverseChannels;
        result.totalPower += instantPower;
        result.lowPower += instantLowPower * inverseChannels;
        result.midPower += instantMidPower * inverseChannels;
        result.highPower += instantHighPower * inverseChannels;
        result.sibilancePower += instantSibilancePower * inverseChannels;
        framePower += instantPower;

        if (++frameSamples >= frameLength || relativeSample + 1 == sampleCount)
        {
            result.frameRmsDb.push_back(safeDb(std::sqrt(
                framePower / static_cast<double>(frameSamples))));
            framePower = 0.0;
            frameSamples = 0;
        }
    }

    result.analysedSeconds += static_cast<double>(sampleCount) / sampleRate;
}

bool isVocalRole(TrackRole role) noexcept
{
    return role == TrackRole::mainVocal
        || role == TrackRole::chorus
        || role == TrackRole::harmony;
}

float targetRmsDbFor(TrackRole role) noexcept
{
    switch (role)
    {
        case TrackRole::mainVocal: return -18.0f;
        case TrackRole::chorus: return -25.0f;
        case TrackRole::harmony: return -23.0f;
        case TrackRole::accompaniment: return -20.0f;
        case TrackRole::other: return -22.0f;
        case TrackRole::unknown:
        default: return -22.0f;
    }
}

float rolePan(TrackRole role, int ordinal) noexcept
{
    const auto alternating = (std::max(0, ordinal) % 2 == 0) ? -1.0f : 1.0f;
    switch (role)
    {
        case TrackRole::chorus: return 0.34f * alternating;
        case TrackRole::harmony: return -0.22f * alternating;
        default: return 0.0f;
    }
}

SimpleMixSettings presetIntent(MixPreset preset) noexcept
{
    SimpleMixSettings settings;
    settings.enabled = preset != MixPreset::flat;
    settings.preset = preset;
    switch (preset)
    {
        case MixPreset::natural:
            settings.brightness = 0.28f;
            settings.ambience = 0.14f;
            settings.stability = 0.32f;
            break;
        case MixPreset::clear:
            settings.brightness = 0.72f;
            settings.ambience = 0.07f;
            settings.stability = 0.52f;
            break;
        case MixPreset::wide:
            settings.brightness = 0.30f;
            settings.ambience = 0.48f;
            settings.stability = 0.28f;
            break;
        case MixPreset::radio:
            settings.brightness = 0.18f;
            settings.ambience = 0.04f;
            settings.stability = 0.68f;
            break;
        case MixPreset::flat:
        default:
            break;
    }
    return settings;
}

SimpleMixSettings baseMixFor(TrackRole role,
                             MixPreset preferredPreset) noexcept
{
    SimpleMixSettings settings;
    settings.enabled = true;
    switch (role)
    {
        case TrackRole::mainVocal:
            settings.preset = MixPreset::clear;
            settings.brightness = 0.48f;
            settings.ambience = 0.10f;
            settings.stability = 0.56f;
            break;
        case TrackRole::chorus:
            settings.preset = MixPreset::wide;
            settings.brightness = 0.26f;
            settings.ambience = 0.46f;
            settings.stability = 0.40f;
            break;
        case TrackRole::harmony:
            settings.preset = MixPreset::natural;
            settings.brightness = 0.24f;
            settings.ambience = 0.30f;
            settings.stability = 0.46f;
            break;
        case TrackRole::accompaniment:
            settings.preset = MixPreset::natural;
            settings.brightness = 0.08f;
            settings.ambience = 0.06f;
            settings.stability = 0.18f;
            break;
        case TrackRole::other:
            settings.preset = MixPreset::natural;
            settings.brightness = 0.18f;
            settings.ambience = 0.12f;
            settings.stability = 0.24f;
            break;
        case TrackRole::unknown:
        default:
            settings.preset = MixPreset::natural;
            settings.brightness = 0.20f;
            settings.ambience = 0.10f;
            settings.stability = 0.25f;
            break;
    }
    if (preferredPreset != MixPreset::flat)
    {
        const auto intent = presetIntent(preferredPreset);
        settings.preset = preferredPreset;
        // The chosen mood remains audible, while the role still determines
        // where the track sits in the arrangement.
        settings.brightness = 0.55f * intent.brightness
                            + 0.45f * settings.brightness;
        settings.ambience = 0.55f * intent.ambience
                          + 0.45f * settings.ambience;
        settings.stability = 0.55f * intent.stability
                           + 0.45f * settings.stability;
    }
    return settings;
}
}

AutoMixAnalysis AutoMixAnalyzer::analyseTrack(const TrackData& track) noexcept
{
    AutoMixAnalysis analysis;
    try
    {
        AnalysisAccumulator accumulator;
        for (const auto& clip : track.clips)
            analyseClip(clip, accumulator);

        analysis.analysedSeconds = accumulator.analysedSeconds;
        if (accumulator.analysedSeconds <= 0.0
            || accumulator.totalPower <= silencePower)
            return analysis;

        double totalSamples = 0.0;
        for (const auto& clip : track.clips)
            if (clip.buffer != nullptr && clip.sampleRate > 0)
            {
                const auto availableSeconds = std::max(
                    0.0,
                    static_cast<double>(clip.buffer->getNumSamples())
                        / clip.sampleRate
                    - std::max(0.0, std::isfinite(clip.offset)
                                         ? clip.offset : 0.0));
                const auto duration = clip.duration > 0.0
                    && std::isfinite(clip.duration)
                    ? std::min(availableSeconds, clip.duration)
                    : availableSeconds;
                totalSamples += duration * clip.sampleRate;
            }
        if (totalSamples <= 0.0)
            return analysis;

        const auto rms = std::sqrt(accumulator.totalPower / totalSamples);
        analysis.rmsDb = safeDb(rms);
        analysis.peakDb = safeDb(accumulator.peak);
        analysis.crestFactorDb = std::clamp(
            analysis.peakDb - analysis.rmsDb, 0.0f, 40.0f);

        const auto bandPower = accumulator.lowPower + accumulator.midPower
                             + accumulator.highPower;
        if (bandPower > silencePower)
        {
            analysis.lowEnergy = clampUnit(static_cast<float>(
                accumulator.lowPower / bandPower));
            analysis.midEnergy = clampUnit(static_cast<float>(
                accumulator.midPower / bandPower));
            analysis.highEnergy = clampUnit(static_cast<float>(
                accumulator.highPower / bandPower));
            analysis.brightness = clampUnit(
                0.12f * analysis.lowEnergy
                + 0.55f * analysis.midEnergy
                + analysis.highEnergy);
        }
        analysis.sibilance = clampUnit(static_cast<float>(
            accumulator.sibilancePower
            / std::max(accumulator.midPower + accumulator.highPower,
                       silencePower)));

        auto frames = std::move(accumulator.frameRmsDb);
        frames.erase(std::remove_if(frames.begin(), frames.end(),
                                    [](float value)
                                    {
                                        return !std::isfinite(value)
                                            || value <= minimumDb;
                                    }),
                     frames.end());
        if (frames.empty())
            analysis.noiseFloorDb = minimumDb;
        else
        {
            std::sort(frames.begin(), frames.end());
            const auto percentileIndex = static_cast<size_t>(std::floor(
                0.15 * static_cast<double>(frames.size() - 1)));
            const auto candidate = frames[percentileIndex];
            // A steady note has no observable noise-only section. Treating
            // that note itself as the noise floor would incorrectly enable
            // a strong gate, so only publish a floor clearly below the
            // track-wide RMS level.
            analysis.noiseFloorDb = analysis.rmsDb - candidate >= 4.0f
                ? candidate : minimumDb;
        }
        analysis.valid = std::isfinite(analysis.rmsDb)
                      && std::isfinite(analysis.brightness)
                      && analysis.rmsDb > minimumDb;
    }
    catch (...)
    {
        return AutoMixAnalysis {};
    }
    return analysis;
}

float AutoMixAnalyzer::calculateMasking(
    const AutoMixAnalysis& vocal,
    const AutoMixAnalysis& accompaniment) noexcept
{
    if (!vocal.valid || !accompaniment.valid)
        return 0.0f;

    const auto spectralOverlap = clampUnit(
        0.10f * std::min(vocal.lowEnergy, accompaniment.lowEnergy)
        + 0.62f * std::min(vocal.midEnergy, accompaniment.midEnergy)
        + 0.28f * std::min(vocal.highEnergy, accompaniment.highEnergy));
    const auto relativeLevel = clampUnit(
        (accompaniment.rmsDb - vocal.rmsDb + 12.0f) / 24.0f);
    return clampUnit(spectralOverlap * (0.45f + 0.55f * relativeLevel)
                     * 2.2f);
}

AutoMixTrackSuggestion AutoMixAnalyzer::suggestTrack(
    const juce::String& trackId,
    TrackRole role,
    const AutoMixAnalysis& analysis,
    float maskingScore,
    int roleOrdinal,
    MixPreset preferredPreset) noexcept
{
    AutoMixTrackSuggestion result;
    result.trackId = trackId;
    result.role = role;
    result.analysis = analysis;
    result.analysisValid = analysis.valid;
    result.maskingScore = clampUnit(maskingScore);
    result.simpleMix = baseMixFor(role, preferredPreset);
    result.pan = rolePan(role, roleOrdinal);

    if (!analysis.valid)
    {
        result.simpleMix.enabled = false;
        result.simpleMix.preset = MixPreset::flat;
        result.simpleMix.brightness = 0.0f;
        result.simpleMix.ambience = 0.0f;
        result.simpleMix.stability = 0.0f;
        result.warning = "解析できる音声がないため、このトラックは変更しません。";
        return result;
    }

    auto levelChangeDb = targetRmsDbFor(role) - analysis.rmsDb;
    // TrackData volume is a 0..1 fader. A quiet source is kept at unity and
    // is made more present through the bounded compressor/clarity settings.
    levelChangeDb = std::clamp(levelChangeDb, -8.0f, 0.0f);
    if (role == TrackRole::accompaniment)
        levelChangeDb -= 1.5f * result.maskingScore;
    result.volume = std::clamp(gainFromDb(levelChangeDb), 0.40f, 1.0f);

    const auto darknessCorrection = std::clamp(
        (0.48f - analysis.brightness) * 0.75f, -0.18f, 0.28f);
    const auto sibilanceProtection = std::max(
        0.0f, (analysis.sibilance - 0.20f) * 0.45f);
    result.simpleMix.brightness = clampUnit(
        result.simpleMix.brightness + darknessCorrection
        - sibilanceProtection
        + (role == TrackRole::mainVocal ? 0.16f * result.maskingScore
                                         : 0.0f));
    result.simpleMix.stability = clampUnit(
        result.simpleMix.stability
        + std::clamp((analysis.crestFactorDb - 11.0f) / 24.0f,
                     -0.14f, 0.24f)
        + (role == TrackRole::mainVocal ? 0.12f * result.maskingScore
                                         : 0.0f));

    if (isVocalRole(role))
    {
        const auto noiseGapDb = analysis.rmsDb - analysis.noiseFloorDb;
        const auto noiseAmount = clampUnit((20.0f - noiseGapDb) / 16.0f);
        const auto deEssAmount = clampUnit(
            (analysis.sibilance - 0.12f) / 0.42f);
        result.noiseReduction.enabled = noiseAmount > 0.08f
                                     || deEssAmount > 0.08f;
        result.noiseReduction.amount = noiseAmount;
        result.noiseReduction.deEssAmount = deEssAmount;
    }

    return result;
}

AutoMixResult AutoMixAnalyzer::analyseAndSuggest(
    const std::vector<TrackData>& tracks) noexcept
{
    AutoMixResult result;
    try
    {
        result.tracks.reserve(tracks.size());
        std::vector<AutoMixAnalysis> analyses;
        analyses.reserve(tracks.size());
        for (const auto& track : tracks)
            analyses.push_back(analyseTrack(track));

        std::vector<size_t> accompanimentIndices;
        for (size_t index = 0; index < tracks.size(); ++index)
            if (tracks[index].role == TrackRole::accompaniment
                && analyses[index].valid)
                accompanimentIndices.push_back(index);

        std::array<int, 6> roleOrdinals {};
        int validCount = 0;
        for (size_t index = 0; index < tracks.size(); ++index)
        {
            float masking = 0.0f;
            if (isVocalRole(tracks[index].role))
                for (const auto accompanimentIndex : accompanimentIndices)
                    masking = std::max(
                        masking,
                        calculateMasking(analyses[index],
                                         analyses[accompanimentIndex])
                            * timelineOverlapRatio(
                                tracks[index],
                                tracks[accompanimentIndex]));

            const auto roleValue = std::clamp(
                static_cast<int>(tracks[index].role), 0, 5);
            auto suggestion = suggestTrack(tracks[index].id,
                                           tracks[index].role,
                                           analyses[index],
                                           masking,
                                           roleOrdinals[static_cast<size_t>(
                                               roleValue)]++,
                                           tracks[index].simpleMix.preset);
            if (tracks[index].role == TrackRole::accompaniment)
            {
                float maximumVocalMasking = 0.0f;
                for (size_t vocalIndex = 0; vocalIndex < tracks.size();
                     ++vocalIndex)
                    if (isVocalRole(tracks[vocalIndex].role))
                        maximumVocalMasking = std::max(
                            maximumVocalMasking,
                            calculateMasking(analyses[vocalIndex],
                                             analyses[index])
                                * timelineOverlapRatio(tracks[vocalIndex],
                                                       tracks[index]));
                suggestion.maskingScore = maximumVocalMasking;
                if (suggestion.analysisValid)
                    suggestion.volume = std::clamp(
                        suggestion.volume
                            * gainFromDb(-1.5f * maximumVocalMasking),
                        0.40f, 1.0f);
            }
            validCount += suggestion.analysisValid ? 1 : 0;
            result.tracks.push_back(std::move(suggestion));
        }

        result.success = validCount > 0;
        if (tracks.empty())
            result.message = "MIXするトラックがありません。";
        else if (validCount == 0)
            result.message = "解析できる音声がありませんでした。";
        else if (validCount == static_cast<int>(tracks.size()))
            result.message = "全トラックのお任せMIX設定を作成しました。";
        else
            result.message = juce::String(validCount)
                + "トラックを解析しました。音声のないトラックは変更しません。";
    }
    catch (...)
    {
        result.success = false;
        result.message = "音声解析中に問題が発生しました。";
    }
    return result;
}
