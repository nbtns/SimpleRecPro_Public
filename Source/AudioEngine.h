#pragma once

#include <JuceHeader.h>
#include "TrackData.h"
#include "MetronomeEngine.h"
#include "MasterBusProcessor.h"
#include "PlaybackTransform.h"
#include "SpecialFxProcessor.h"
#include "TempoMap.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class TrackStateAction;

enum class CursorMode
{
    Arrow,
    Hand,
    Split
};

/**
 * 録音・再生・ファイル入出力をまとめて扱う音声エンジン。
 *
 * UIや保存処理は getTracksSnapshot()/replaceTracks() を使い、
 * 音声処理中のトラックデータと競合しないようにする。
 */
class AudioEngine : public juce::AsyncUpdater
{
public:
    AudioEngine();
    ~AudioEngine() override;

    // Audio callback
    void prepare(int samplesPerBlock, double sampleRate);
    void processBlock(const juce::AudioSourceChannelInfo& bufferToFill,
                      const float* const* inputChannelData,
                      int numInputChannels);
    /** Updates the shared input meter when another real-time path consumes the input. */
    void updateInputMeter(const float* const* inputChannelData,
                          int numInputChannels,
                          int numSamples) noexcept;
    void release();

    // Transport
    void play();
    void record(int trackIndex);
    void stop();

    // Tracks and files
    int addTrack(const juce::String& name = "");
    void removeTrack(int index);
    bool loadAudioFile(const juce::File& file);
    int loadAudioFiles(const juce::Array<juce::File>& files);
    void loadAudioFilesAsync(const juce::Array<juce::File>& files);
    void loadAudioFileWithDialog();

    void toggleMute(int trackIndex);
    void toggleSolo(int trackIndex);
    void toggleArm(int trackIndex);
    void setTrackName(int trackIndex, const juce::String& name);
    void setTrackRole(int trackIndex, TrackRole role);
    void setTrackVolume(int trackIndex, float volume);
    void setTrackPan(int trackIndex, float pan);
    void setTrackPitchSemitones(int trackIndex, double semitones);
    void setTrackPlaybackSpeed(int trackIndex, double speed);
    void resetTrackTransform(int trackIndex);
    void setTrackPitchCorrectionSettings(
        int trackIndex, const PitchCorrectionSettings& settings);
    void setTrackPitchCorrectionAudition(int trackIndex, bool corrected);
    void setTrackSimpleMixSettings(int trackIndex,
                                   const SimpleMixSettings& settings);
    void setTrackNoiseReductionSettings(
        int trackIndex, const NoiseReductionSettings& settings);
    bool addTrackSpecialFxRegion(int trackIndex,
                                 SpecialFxType type,
                                 double startSeconds,
                                 double endSeconds,
                                 float amount,
                                 float wet,
                                 double fadeSeconds);
    void clearTrackSpecialFxRegions(int trackIndex);
    /** Rejects non-finite/out-of-range values and more than 256 regions. Undoable. */
    bool setTrackAutomationRegions(int trackIndex,
                                   const std::vector<AudioAutomationRegion>& regions);
    int duplicateTrackAsHarmony(int trackIndex,
                                int semitones,
                                float volume,
                                float pan);

    // Per-track VST3 effects
    void loadVst3PluginForTrack(int trackIndex, const juce::File& pluginFile);
    void removeVst3PluginFromTrack(int trackIndex);
    void setTrackVst3Bypassed(int trackIndex, bool shouldBeBypassed);
    void showVst3PluginEditor(int trackIndex);
    void addEffectPluginForTrack(int trackIndex,
                                 const juce::File& pluginFile);
    void removeEffectSlot(int trackIndex, int slotIndex);
    void moveEffectSlot(int trackIndex, int fromIndex, int toIndex);
    void setEffectSlotBypassed(int trackIndex,
                               int slotIndex,
                               bool shouldBeBypassed);
    void setEffectChainBypassed(int trackIndex, bool shouldBeBypassed);
    void showEffectSlotEditor(int trackIndex, int slotIndex);
    juce::File getDefaultVst3PluginDirectory() const;
    bool capturePluginStates();
    juce::File findInstalledPitchNet() const;
    bool isPitchNetInstalled() const { return findInstalledPitchNet().exists(); }
    bool isPitchCorrectionActive(int trackIndex) const;
    void applyNaturalPitchCorrection(int trackIndex);
    void showPitchCorrectionDetails(int trackIndex);

    std::vector<TrackData> getTracksSnapshot() const;
    void replaceTracks(std::vector<TrackData> newTracks);

    // Clips
    void setSelectedClipId(const juce::String& id)
    {
        selectedClipId = id;
        if (onStateChanged) onStateChanged();
    }
    juce::String getSelectedClipId() const { return selectedClipId; }
    void moveClip(const juce::String& clipId, int destTrackIndex, double newStartTime);
    void splitClip(const juce::String& clipId, double splitTime);
    void removeClip(const juce::String& clipId);
    void trimClipLeft(const juce::String& clipId, double newStartTime);
    void trimClipRight(const juce::String& clipId, double newEndTime);
    void setClipFades(const juce::String& clipId,
                      double fadeInSeconds,
                      double fadeOutSeconds);
    void setClipRhythmMarkers(const juce::String& clipId,
                              std::vector<RhythmMarker> markers);
    int quantizeClipRhythm(const juce::String& clipId, float strength);

    // Undo / Redo
    void beginEdit();
    void endEdit();
    void undo();
    void redo();
    void restoreTracksState(const std::vector<TrackData>& state);

    // State
    PlaybackState getPlaybackState() const { return playbackState.load(); }
    double getCurrentTime() const { return currentPlaybackPosition.load(); }
    void setCurrentTime(double time);
    double getDuration() const;
    double getSampleRate() const { return currentSampleRate.load(); }
    int getRecordingTrackIndex() const { return recordingTrackIndex.load(); }

    void setCountInEnabled(bool enabled)
    {
        countInEnabled.store(enabled);
        if (onStateChanged) onStateChanged();
    }
    bool isCountInEnabled() const { return countInEnabled.load(); }

    float getCurrentInputLevel() const { return currentInputLevel.load(); }
    double getRecordingStartTime() const { return recordingStartTime; }

    // Fixed-size preview; the full recording buffer stays private to recording.
    std::vector<float> getRecordingBufferSnapshot() const;
    int64_t getRecordingSampleCount() const { return recordingSampleCount.load(); }

    void setMasterVolume(float vol) { masterVolume.store(juce::jlimit(0.0f, 1.0f, vol)); }
    float getMasterVolume() const { return masterVolume.load(); }

    void setZoomLevel(double zoom)
    {
        zoomLevel.store(juce::jlimit(10.0, 500.0, zoom));
        markDirty();
        if (onStateChanged) onStateChanged();
    }
    double getZoomLevel() const { return zoomLevel.load(); }

    void setCursorMode(CursorMode mode)
    {
        cursorMode = mode;
        if (onStateChanged) onStateChanged();
    }
    CursorMode getCursorMode() const { return cursorMode; }

    MetronomeEngine& getMetronome() { return metronome; }

    // Project-wide DAW settings (key/scale, A-B loop, tempo map, export,
    // mastering). Additive defaults keep legacy projects loadable.
    ProjectDawSettings getProjectDawSettings() const;
    void setProjectDawSettings(const ProjectDawSettings& settings,
                               bool markProjectDirty = true);
    void setLoopStartAtCurrentTime();
    void setLoopEndAtCurrentTime();
    void clearLoopRange();
    void setLoopEnabled(bool enabled);
    void setTempoPoint(double timeSeconds,
                       double bpm,
                       int numerator,
                       int denominator);
    void removeTempoPointAt(double timeSeconds);
    TempoPoint getTempoPointAt(double timeSeconds) const;
    float getMasteringLoudnessLufs() const
    {
        return masterBusProcessor.getApproximateLoudnessLufs();
    }
    float getMasteringOutputPeak() const
    {
        return masterBusProcessor.getLastOutputPeak();
    }
    float getMasteringGainReductionDb() const
    {
        return masterBusProcessor.getLimiterGainReductionDb();
    }

    // Latency compensation. Positive manual values move a recording further forward.
    void setDeviceLatencySamples(int inputLatency, int outputLatency);
    int getInputLatencySamples() const { return inputLatencySamples.load(); }
    int getOutputLatencySamples() const { return outputLatencySamples.load(); }
    void setCalibratedRecordingLatencySamples(int samples);
    void clearCalibratedRecordingLatency();
    bool hasCalibratedRecordingLatency() const { return calibratedRecordingLatencySamples.load() >= 0; }
    int getCalibratedRecordingLatencySamples() const { return calibratedRecordingLatencySamples.load(); }
    int getAutomaticRecordingLatencySamples() const;
    void setManualRecordingOffsetSamples(int samples);
    int getManualRecordingOffsetSamples() const { return manualRecordingOffsetSamples.load(); }
    int getEffectiveRecordingLatencySamples() const;

    // Normalises already-loaded clips to the active device sample rate.
    bool resampleAllClipsToCurrentSampleRate();
    // Prepares loaded tracks on a worker before applyLoadedProject(). The
    // shared buffers make the resulting transfer to the message thread cheap.
    static bool prepareTracksForSampleRate(std::vector<TrackData>& tracksToPrepare,
                                           double targetSampleRate);

    // Export
    void exportTrackToWav(int trackIndex);
    void exportMixdownToWav();

    struct ExportStatus
    {
        bool active = false;
        bool completed = false;
        bool cancelled = false;
        bool succeeded = false;
        float progress = 0.0f;
        juce::String message;
        juce::File outputFile;
    };

    bool startExport(const juce::File& outputFile,
                     const ExportSettings& settings);
    void cancelExport();
    ExportStatus getExportStatus() const;

    // Project dirty state
    bool isDirty() const { return dirty.load(); }
    std::uint64_t getContentRevision() const { return contentRevision.load(); }
    void markDirty();
    void markClean();

    // UI callbacks (message thread)
    std::function<void()> onStateChanged;
    std::function<void(bool)> onDirtyChanged;
    // bool is true for an error/warning and false for a success/info message.
    std::function<void(const juce::String&, bool)> onStatusMessage;
    // Called on the message thread after VST3 inspection finishes. `reason`
    // is empty on success and user-facing on failure.
    std::function<void(const juce::File&, bool, const juce::String&)>
        onPluginScanFinished;

    void handleAsyncUpdate() override;

private:
    struct ImportBatchResult;
    struct TrackPluginSlot;
    struct TrackPlaybackSlot;
    struct ChunkedRecordingBuffer
    {
        static constexpr size_t chunkSize = 65536;

        void preallocate(size_t samples);
        void clear() noexcept { sampleCount = 0; }
        bool empty() const noexcept { return sampleCount == 0; }
        size_t size() const noexcept { return sampleCount; }
        void append(const float* source, size_t numSamples);
        void appendValue(float value, size_t numSamples);
        void resize(size_t newSize, float value);
        void copyTo(juce::AudioBuffer<float>& destination,
                    int destinationChannel,
                    int numSamples) const;

    private:
        void ensureChunk(size_t chunkIndex);

        std::vector<std::unique_ptr<float[]>> chunks;
        size_t sampleCount = 0;
    };

    void processPlayback(juce::AudioBuffer<float>& outputBuffer, int numSamples);
    void processRecording(const float* const* inputChannelData,
                          int numInputChannels,
                          int numSamples,
                          int inputStartOffset = 0);
    void startPlayback();
    void playFromAraEditor(const juce::String& trackId);
    void handleAraDocumentChanged(const juce::String& trackId);
    void loadTrackPlugin(const juce::String& trackId,
                         const juce::PluginDescription& description,
                         std::shared_ptr<const juce::MemoryBlock> savedState,
                         std::shared_ptr<const juce::MemoryBlock> savedAraArchive,
                         const juce::String& savedAraArchiveId,
                         bool araPlaybackEnabled,
                         bool shouldBeBypassed,
                         bool markProjectDirty,
                         bool openEditorAfterLoad,
                         bool applyNaturalAfterLoad = false);
    void scanVst3PluginForTrack(int trackIndex,
                                const juce::File& pluginFile,
                                bool addAsEffectSlot,
                                bool applyNaturalAfterLoad = false);
    void markPitchCorrectionUnavailable(const juce::String& trackId);
    void loadEffectPlugin(const juce::String& trackId,
                          const juce::String& effectSlotId,
                          const juce::PluginDescription& description,
                          std::shared_ptr<const juce::MemoryBlock> savedState,
                          bool shouldBeBypassed,
                          bool createMetadataIfMissing,
                          bool markProjectDirty,
                          bool openEditorAfterLoad);
    void restoreTrackPluginsFromMetadata(const std::vector<TrackData>& tracksToRestore);
    void reorderTrackPluginSlotsLocked(
        const juce::String& trackId,
        const std::vector<juce::String>& effectOrder);
    void requestPluginLatencyRefresh() noexcept;
    void refreshTrackPluginLatencies();
    void syncAraProcessingState(const juce::String& trackId);
    void queueAraTrackSync();
    void syncAraTracksToModel();
    void setPluginEditorTransportActive(const juce::String& trackId, bool active);
    void pumpOpenPluginEditorTransport(int numSamples) noexcept;
    bool processTrackPlugin(const juce::String& trackId,
                            juce::AudioBuffer<float>& buffer,
                            int numSamples,
                            int64_t timelineStartSample) noexcept;
    bool measureTrackPluginLatencyForOffline(
        const TrackData& track,
        double sampleRate,
        int& latencySamples,
        juce::String& errorMessage);
    bool processTrackPluginOffline(
        const TrackData& track,
        juce::AudioBuffer<float>& buffer,
        double sampleRate,
        const std::function<bool()>& shouldCancel,
        juce::String& errorMessage);
    void hideVst3PluginEditor(const juce::String& expectedTrackId,
                              const juce::String& expectedEffectSlotId = {});
    void closeVst3PluginEditor(bool syncProcessingState = true);

    void mixClipIntoBuffer(const AudioClip& clip,
                           juce::AudioBuffer<float>& destination,
                           int64_t timelineStartSample,
                           double timelineSampleRate,
                           float gain,
                           bool ensureRhythmRender = false,
                           const std::function<bool()>& shouldCancel = {}) const;
    static double mapRhythmTimeToSource(const AudioClip& clip,
                                        double targetTime) noexcept;
    void applyTrackOutputProcessing(const TrackData& track,
                                    TrackPlaybackSlot* playbackSlot,
                                    juce::AudioBuffer<float>& buffer,
                                    int numSamples,
                                    int64_t timelineStartSample) noexcept;
    static bool resampleClip(AudioClip& clip, double targetSampleRate);
    static double getTracksDuration(const std::vector<TrackData>& tracksToMeasure);
    void rebuildTrackPlaybackSlotsLocked();
    void resetTrackPlaybackTransformsLocked();
    void queueMissingRhythmRenders();

    void postStatusMessage(const juce::String& message, bool isError);
    void applyCompletedImports();
    void queueMissingInputWarning();
    void appendRecordingPreviewPoint(float peak) noexcept;
    void setDirtyState(bool shouldBeDirty);

    juce::UndoManager undoManager;
    std::vector<TrackData> lastSavedState;
    bool editInProgress = false;

    mutable juce::CriticalSection tracksLock;
    std::vector<TrackData> tracks;
    juce::AudioFormatManager formatManager;
    juce::AudioPluginFormatManager pluginFormatManager;
    mutable juce::SpinLock pluginLock;
    std::vector<std::unique_ptr<TrackPluginSlot>> trackPlugins;
    std::vector<std::unique_ptr<TrackPlaybackSlot>> trackPlaybackSlots;
    std::atomic<std::uint64_t> pluginGeneration { 1 };
    std::atomic<std::uint64_t> pluginScanGeneration { 1 };
    std::atomic<bool> pluginScanShutdownRequested { false };
    std::atomic<bool> araTrackSyncPending { false };
    std::atomic<bool> pluginLatencyRefreshPending { false };
    juce::AudioBuffer<float> trackProcessingBuffer;
    juce::AudioBuffer<float> pluginEditorTransportBuffer;
    juce::AudioBuffer<float> trackTransformInputBuffer;
    std::atomic<bool> playbackTransformResetRequested { false };
    std::unique_ptr<juce::DocumentWindow> pluginEditorWindow;
    juce::String pluginEditorTrackId;
    juce::String pluginEditorEffectSlotId;
    mutable juce::CriticalSection completedImportsLock;
    std::vector<std::unique_ptr<ImportBatchResult>> completedImports;
    std::atomic<std::uint64_t> projectGeneration { 1 };
    std::atomic<bool> importShutdownRequested { false };
    std::atomic<std::uint64_t> rhythmRenderGeneration { 1 };
    std::atomic<bool> rhythmRenderShutdownRequested { false };
    // Declared after the state used by import jobs so it is destroyed first.
    juce::ThreadPool importThreadPool;
    juce::ThreadPool exportThreadPool {
        juce::ThreadPool::Options{}
            .withNumberOfThreads(1)
            .withThreadName("Audio Export")
            .withDesiredThreadPriority(juce::Thread::Priority::background)
    };
    MetronomeEngine metronome;
    MasterBusProcessor masterBusProcessor;

    mutable juce::CriticalSection projectSettingsLock;
    ProjectDawSettings projectDawSettings;
    std::shared_ptr<const TempoMap> tempoMapSnapshot;
    std::atomic<bool> loopEnabled { false };
    std::atomic<bool> hasLoopStart { false };
    std::atomic<bool> hasLoopEnd { false };
    std::atomic<double> loopStartSeconds { 0.0 };
    std::atomic<double> loopEndSeconds { 0.0 };
    std::atomic<int> loopFadeInSamplesRemaining { 0 };

    mutable juce::CriticalSection exportStatusLock;
    ExportStatus exportStatus;
    std::atomic<bool> exportCancelRequested { false };
    std::atomic<std::uint64_t> exportGeneration { 1 };

    std::atomic<PlaybackState> playbackState { PlaybackState::Stopped };
    std::atomic<double> currentSampleRate { 44100.0 };
    int currentBlockSize = 512;
    std::atomic<int64_t> playbackPositionInSamples { 0 };
    std::atomic<double> currentPlaybackPosition { 0.0 };
    std::atomic<double> cachedDuration { 0.0 };

    // Recording data
    std::atomic<int> recordingTrackIndex { -1 };
    juce::String recordingTrackId;
    mutable juce::CriticalSection recordingLock;
    ChunkedRecordingBuffer recordingBuffer;       // left/mono recording data
    ChunkedRecordingBuffer recordingBufferRight;  // right channel when available
    static constexpr size_t recordingPreviewCapacity = 4096;
    mutable juce::SpinLock recordingPreviewLock;
    std::array<float, recordingPreviewCapacity> recordingPreview {};
    size_t recordingPreviewSize = 0;
    uint64_t recordingPreviewBlocksPerPoint = 1;
    uint64_t recordingPreviewBlocksAccumulated = 0;
    float recordingPreviewAccumulator = 0.0f;
    std::atomic<int64_t> recordingSampleCount { 0 };
    int recordingNumChannels = 0;
    std::atomic<bool> recordingHadValidInput { false };
    std::atomic<float> recordingPeak { 0.0f };
    double recordingStartTime = 0.0;
    double recordingSampleRate = 44100.0;
    int recordingLatencySamples = 0;

    std::atomic<bool> countInEnabled { false };
    std::atomic<int64_t> countInSamplesRemaining { 0 };
    std::atomic<int64_t> countInSamplesTotal { 0 };
    std::atomic<float> currentInputLevel { 0.0f };
    std::atomic<int> lastKnownInputChannels { 0 };
    std::atomic<bool> missingInputWarningSent { false };
    std::atomic<bool> pendingMissingInputWarning { false };

    std::atomic<int> inputLatencySamples { 0 };
    std::atomic<int> outputLatencySamples { 0 };
    // -1 means that no measured round-trip value is available for this device setup.
    std::atomic<int> calibratedRecordingLatencySamples { -1 };
    std::atomic<int> manualRecordingOffsetSamples { 0 };

    std::atomic<float> masterVolume { 0.8f };
    int nextTrackColorIndex = 0;

    std::atomic<double> zoomLevel { 50.0 };
    CursorMode cursorMode = CursorMode::Arrow;
    juce::String selectedClipId;

    std::atomic<bool> dirty { false };
    std::atomic<std::uint64_t> contentRevision { 1 };
    std::atomic<bool> dirtyNotificationPending { false };
    std::atomic<bool> pendingDirtyValue { false };

    JUCE_DECLARE_WEAK_REFERENCEABLE(AudioEngine)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)

    juce::ThreadPool rhythmRenderThreadPool {
        juce::ThreadPool::Options{}
            .withNumberOfThreads(1)
            .withThreadName("Rhythm Render")
            .withDesiredThreadPriority(juce::Thread::Priority::background)
    };

    // Keep this as the final data member. It is therefore destroyed before all
    // cancellation state read by an in-flight vendor scanner. JUCE's bounded
    // pool shutdown also avoids waiting forever for a broken VST3 scanner.
    juce::ThreadPool pluginScanThreadPool;
};
