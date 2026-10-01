#pragma once
#include <JuceHeader.h>
#include "DawFeatureTypes.h"
#include <vector>
#include <memory>

struct RhythmRenderCache;

/**
 * AudioClip - 1つの音声ブロック（Web版のAudioClipに相当）
 * トラック上の特定の時間位置に配置される音声データの参照
 */
struct AudioClip
{
    juce::String id;
    std::shared_ptr<juce::AudioBuffer<float>> buffer; // 音声データ本体（共有ポインタでコピーコスト削減）
    double startTime = 0.0;  // トラック上での開始時間（秒）
    double offset = 0.0;     // buffer内の再生開始位置（秒）
    double duration = 0.0;   // クリップの再生長（秒）
    juce::String name;       // クリップ名

    // 非破壊編集。元のbufferは変更せず、再生範囲と音量カーブだけを保存する。
    double fadeInSeconds = 0.0;
    double fadeOutSeconds = 0.0;
    std::vector<RhythmMarker> rhythmMarkers;
    // Transient, immutable pitch-preserving render. It is deliberately not
    // serialised; projects rebuild it after load and edits invalidate it by
    // signature without touching the original audio buffer.
    std::shared_ptr<const RhythmRenderCache> rhythmRenderCache;

    int sampleRate = 44100;  // このクリップのサンプルレート

    int getNumChannels() const noexcept
    {
        return buffer != nullptr ? buffer->getNumChannels() : 0;
    }

    double getSourceDuration() const noexcept
    {
        return buffer != nullptr && sampleRate > 0
                   ? static_cast<double>(buffer->getNumSamples()) / static_cast<double>(sampleRate)
                   : 0.0;
    }
};

/**
 * TrackData - 1つのトラックのデータモデル（Web版のTrackに相当）
 */
struct TrackData
{
    static constexpr int timelineVst3AraHostFormatVersion = 6;
    static constexpr int currentVst3AraHostFormatVersion =
        timelineVst3AraHostFormatVersion;
    static constexpr int hostFormatVersionForPluginLoad(
        bool hasAraExtension,
        bool hasSavedAraArchive,
        int savedHostFormatVersion) noexcept
    {
        return !hasAraExtension ? 1
            : !hasSavedAraArchive ? currentVst3AraHostFormatVersion
            : savedHostFormatVersion > 1 ? savedHostFormatVersion : 1;
    }

    juce::String id;
    juce::String name;
    TrackRole role = TrackRole::unknown;
    std::vector<AudioClip> clips;  // クリップ配列
    float volume = 1.0f;
    float pan = 0.0f; // -1.0=L, 0.0=C, 1.0=R
    double pitchSemitones = 0.0;
    double playbackSpeed = 1.0;
    bool isMuted = false;
    bool isSolo = false;
    bool isArmed = false;   // 録音待機状態
    juce::Colour color;     // トラックカラー

    PitchCorrectionSettings pitchCorrection;
    SimpleMixSettings simpleMix;
    NoiseReductionSettings noiseReduction;
    std::vector<SpecialFxRegion> specialFxRegions;
    std::vector<AudioAutomationRegion> automationRegions;

    // トラックに挿入したVST3エフェクト。実体はAudioEngineが保持し、
    // ここには画面表示とプロジェクト保存に必要な情報だけを持つ。
    juce::String vst3Name;
    juce::String vst3DescriptionXml;
    // Plug-in payloads can be hundreds of megabytes (PitchNet, for example,
    // may persist its analysed audio). Keep them as immutable shared binary
    // blocks so project snapshots and asynchronous plug-in loading never make
    // deep copies of the payload.
    std::shared_ptr<const juce::MemoryBlock> vst3State;
    std::shared_ptr<const juce::MemoryBlock> vst3AraArchive;
    juce::String vst3AraArchiveId;
    // 1: legacy full-buffer sources, 2: cropped source IDs,
    // 3: stable full-buffer IDs with trimmed samples masked to silence,
    // 4: restored regions normalised and both PitchNet payloads recaptured.
    // 5: validated PitchNet PNAR data is pruned to current source ranges.
    // 6: all clips on a track share one timeline source and modification.
    int vst3AraHostFormatVersion = 1;
    bool vst3AraPlaybackEnabled = false;
    bool vst3Bypassed = false;
    int vst3LatencySamples = 0; // 実行中の専用PitchNet/ARAスロットから取得
    double vst3TailSeconds = 0.0;

    // 通常のVST3は複数スロットとして保持する。PitchNet/ARAは上の専用枠に残す。
    bool effectChainBypassed = false;
    std::vector<EffectSlotData> effectSlots;

    /** トラック全体の長さ（秒）を計算 */
    double getSourceDuration() const
    {
        double maxEnd = 0.0;
        for (const auto& clip : clips)
            maxEnd = std::max(maxEnd, clip.startTime + clip.duration);
        if (maxEnd <= 0.0)
            return maxEnd;

        double tail = vst3Bypassed ? 0.0 : std::max(0.0, vst3TailSeconds);
        if (!effectChainBypassed)
            for (const auto& effect : effectSlots)
                if (!effect.bypassed)
                    tail += std::max(0.0, effect.tailSeconds);
        if (simpleMix.enabled && simpleMix.ambience > 0.001f)
            tail += 0.35;
        return maxEnd + std::min(60.0, tail);
    }

    double getDuration() const
    {
        return getSourceDuration() / juce::jlimit(0.75, 1.5, playbackSpeed);
    }
};

/** 再生状態 */
enum class PlaybackState
{
    Stopped,
    Playing,
    Recording,
    CountIn
};

/** トラックカラーパレット（Web版と同じ10色） */
inline juce::Colour getTrackColor(int index)
{
    static const juce::Colour palette[] = {
        juce::Colour(0xff10b981), // エメラルドグリーン
        juce::Colour(0xffef4444), // レッド
        juce::Colour(0xff3b82f6), // ブルー
        juce::Colour(0xfff59e0b), // アンバー
        juce::Colour(0xff8b5cf6), // パープル
        juce::Colour(0xffec4899), // ピンク
        juce::Colour(0xff06b6d4), // シアン
        juce::Colour(0xfff97316), // オレンジ
        juce::Colour(0xff14b8a6), // ティール
        juce::Colour(0xff6366f1), // インディゴ
    };
    return palette[index % 10];
}
