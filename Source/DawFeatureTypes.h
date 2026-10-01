#pragma once

#include <JuceHeader.h>

#include <cmath>
#include <memory>
#include <vector>

enum class MusicalScale
{
    chromatic = 0,
    major,
    naturalMinor,
    harmonicMinor,
    pentatonicMajor,
    pentatonicMinor
};

enum class MixPreset
{
    flat = 0,
    natural,
    clear,
    wide,
    radio
};

enum class TrackRole
{
    unknown = 0,
    mainVocal,
    chorus,
    harmony,
    accompaniment,
    other
};

/** 内蔵の範囲エフェクト。VST3がなくても同じプロジェクト音を再現できる。 */
enum class SpecialFxType
{
    none = 0,
    muffled,
    radio,
    noise,
    distortion,
    bitcrush
};

inline TrackRole sanitiseTrackRole(int value) noexcept
{
    return static_cast<TrackRole>(juce::jlimit(
        static_cast<int>(TrackRole::unknown),
        static_cast<int>(TrackRole::other),
        value));
}

enum class ExportFormat
{
    wav = 0,
    mp3
};

enum class ExportChannels
{
    mono = 1,
    stereo = 2
};

enum class ExportTarget
{
    fullMix = 0,
    abRange,
    selectedTrack
};

struct RhythmMarker
{
    // Values are relative to the visible beginning of the clip.  The start
    // pair is the non-destructive warp anchor.  The optional end pair turns
    // the anchor into an editable note range; old projects leave both ends at
    // zero and keep the original point-marker behaviour.
    double sourceTime = 0.0;
    double targetTime = 0.0;
    double sourceEndTime = 0.0;
    double targetEndTime = 0.0;

    bool hasDuration() const noexcept
    {
        return sourceEndTime > sourceTime + 1.0e-6
            && targetEndTime > targetTime + 1.0e-6;
    }
};

struct PitchCorrectionSettings
{
    bool enabled = false;
    bool auditionCorrected = true;
    int key = 0; // C=0 ... B=11
    MusicalScale scale = MusicalScale::chromatic;
    float strength = 0.60f;
};

struct SimpleMixSettings
{
    bool enabled = false;
    MixPreset preset = MixPreset::flat;
    float brightness = 0.0f;
    float ambience = 0.0f;
    float stability = 0.0f;
};

struct NoiseReductionSettings
{
    bool enabled = false;
    float amount = 0.0f;
    float deEssAmount = 0.0f;
};

struct SpecialFxRegion
{
    juce::String id;
    SpecialFxType type = SpecialFxType::none;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    float amount = 0.65f;
    float wet = 1.0f;
    double fadeSeconds = 0.03;
    bool enabled = true;
};

/** Timeline seconds (after playback speed). Overlapping regions run in array order. */
struct AudioAutomationRegion
{
    juce::String id;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    float gainDb = 0.0f;       // -24 .. +12 dB
    float brightness = 0.0f;   // -1 .. +1
    float ambience = 0.0f;     // 0 .. 1
    double fadeSeconds = 0.03; // 0 .. min(10, duration / 2)
    bool enabled = true;
};

inline bool operator==(const AudioAutomationRegion& a,
                       const AudioAutomationRegion& b) noexcept
{
    return a.id == b.id && a.startSeconds == b.startSeconds
        && a.endSeconds == b.endSeconds && a.gainDb == b.gainDb
        && a.brightness == b.brightness && a.ambience == b.ambience
        && a.fadeSeconds == b.fadeSeconds && a.enabled == b.enabled;
}

struct MasteringSettings
{
    bool enabled = false;
    bool limiterEnabled = true;
    float ceilingDb = -1.0f;
    float targetLufs = -14.0f;
    bool auditionProcessed = true;
};

struct TempoPoint
{
    double timeSeconds = 0.0;
    double bpm = 120.0;
    int numerator = 4;
    int denominator = 4;
};

struct EffectSlotData
{
    juce::String id;
    juce::String name;
    juce::String descriptionXml;
    std::shared_ptr<const juce::MemoryBlock> state;
    bool bypassed = false;
    int latencySamples = 0;
    double tailSeconds = 0.0;
};

struct ExportSettings
{
    ExportFormat format = ExportFormat::wav;
    ExportChannels channels = ExportChannels::stereo;
    int wavBits = 16;
    int mp3Kbps = 192;
    int sampleRate = 44100;
    ExportTarget target = ExportTarget::fullMix;
    int trackIndex = -1;
    double rangeStartSeconds = 0.0;
    double rangeEndSeconds = 0.0;
};

struct ProjectDawSettings
{
    int musicalKey = 0;
    MusicalScale musicalScale = MusicalScale::chromatic;
    float pitchCorrectionStrength = 0.60f;
    bool hasLoopStart = false;
    bool hasLoopEnd = false;
    double loopStartSeconds = 0.0;
    double loopEndSeconds = 0.0;
    bool loopEnabled = false;
    std::vector<TempoPoint> tempoMap { TempoPoint{} };
    ExportSettings exportDefaults;
    MasteringSettings mastering;
};

inline bool approximatelyEqual(double left, double right,
                               double tolerance = 1.0e-9) noexcept
{
    return std::abs(left - right) <= tolerance;
}

inline bool operator==(const RhythmMarker& left, const RhythmMarker& right) noexcept
{
    return approximatelyEqual(left.sourceTime, right.sourceTime)
        && approximatelyEqual(left.targetTime, right.targetTime)
        && approximatelyEqual(left.sourceEndTime, right.sourceEndTime)
        && approximatelyEqual(left.targetEndTime, right.targetEndTime);
}

inline bool operator==(const PitchCorrectionSettings& left,
                       const PitchCorrectionSettings& right) noexcept
{
    return left.enabled == right.enabled
        && left.auditionCorrected == right.auditionCorrected
        && left.key == right.key
        && left.scale == right.scale
        && approximatelyEqual(left.strength, right.strength, 1.0e-6);
}

inline bool operator==(const SimpleMixSettings& left,
                       const SimpleMixSettings& right) noexcept
{
    return left.enabled == right.enabled
        && left.preset == right.preset
        && approximatelyEqual(left.brightness, right.brightness, 1.0e-6)
        && approximatelyEqual(left.ambience, right.ambience, 1.0e-6)
        && approximatelyEqual(left.stability, right.stability, 1.0e-6);
}

inline bool operator==(const NoiseReductionSettings& left,
                       const NoiseReductionSettings& right) noexcept
{
    return left.enabled == right.enabled
        && approximatelyEqual(left.amount, right.amount, 1.0e-6)
        && approximatelyEqual(left.deEssAmount, right.deEssAmount, 1.0e-6);
}

inline bool operator==(const SpecialFxRegion& left,
                       const SpecialFxRegion& right) noexcept
{
    return left.id == right.id
        && left.type == right.type
        && approximatelyEqual(left.startSeconds, right.startSeconds)
        && approximatelyEqual(left.endSeconds, right.endSeconds)
        && approximatelyEqual(left.amount, right.amount, 1.0e-6)
        && approximatelyEqual(left.wet, right.wet, 1.0e-6)
        && approximatelyEqual(left.fadeSeconds, right.fadeSeconds)
        && left.enabled == right.enabled;
}

inline bool operator==(const MasteringSettings& left,
                       const MasteringSettings& right) noexcept
{
    return left.enabled == right.enabled
        && left.limiterEnabled == right.limiterEnabled
        && approximatelyEqual(left.ceilingDb, right.ceilingDb, 1.0e-6)
        && approximatelyEqual(left.targetLufs, right.targetLufs, 1.0e-6)
        && left.auditionProcessed == right.auditionProcessed;
}

inline bool operator==(const TempoPoint& left, const TempoPoint& right) noexcept
{
    return approximatelyEqual(left.timeSeconds, right.timeSeconds)
        && approximatelyEqual(left.bpm, right.bpm, 1.0e-6)
        && left.numerator == right.numerator
        && left.denominator == right.denominator;
}

inline bool operator==(const ExportSettings& left,
                       const ExportSettings& right) noexcept
{
    return left.format == right.format
        && left.channels == right.channels
        && left.wavBits == right.wavBits
        && left.mp3Kbps == right.mp3Kbps
        && left.sampleRate == right.sampleRate
        && left.target == right.target
        && left.trackIndex == right.trackIndex
        && approximatelyEqual(left.rangeStartSeconds, right.rangeStartSeconds)
        && approximatelyEqual(left.rangeEndSeconds, right.rangeEndSeconds);
}

inline bool operator==(const EffectSlotData& left,
                       const EffectSlotData& right) noexcept
{
    const bool stateEqual = left.state == right.state
        || (left.state != nullptr && right.state != nullptr
            && *left.state == *right.state);
    return left.id == right.id
        && left.name == right.name
        && left.descriptionXml == right.descriptionXml
        && stateEqual
        && left.bypassed == right.bypassed
        && left.latencySamples == right.latencySamples
        && approximatelyEqual(left.tailSeconds, right.tailSeconds);
}

inline bool operator==(const ProjectDawSettings& left,
                       const ProjectDawSettings& right) noexcept
{
    return left.musicalKey == right.musicalKey
        && left.musicalScale == right.musicalScale
        && approximatelyEqual(left.pitchCorrectionStrength,
                              right.pitchCorrectionStrength, 1.0e-6)
        && left.hasLoopStart == right.hasLoopStart
        && left.hasLoopEnd == right.hasLoopEnd
        && approximatelyEqual(left.loopStartSeconds, right.loopStartSeconds)
        && approximatelyEqual(left.loopEndSeconds, right.loopEndSeconds)
        && left.loopEnabled == right.loopEnabled
        && left.tempoMap == right.tempoMap
        && left.exportDefaults == right.exportDefaults
        && left.mastering == right.mastering;
}
