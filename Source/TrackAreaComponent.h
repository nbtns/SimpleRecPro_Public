#pragma once
#include <JuceHeader.h>
#include "AudioEngine.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

/**
 * TrackAreaComponent - 中央のトラック表示領域
 * AudioEngineからトラックデータを読み取り、動的にトラックと波形を描画
 */
class TrackAreaComponent : public juce::Component,
                           public juce::ScrollBar::Listener,
                           public juce::FileDragAndDropTarget
{
public:
    TrackAreaComponent();
    ~TrackAreaComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    
    // マウスイベント
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event,
                        const juce::MouseWheelDetails& wheel) override;
    
    // キーボードイベント
    bool keyPressed(const juce::KeyPress& key) override;
    
    /** AudioEngineを設定 */
    void setAudioEngine(AudioEngine* engine)
    {
        audioEngine = engine;
        cachedTracks.clear();
        selectedTrackIndex = -1;
        selectedTrackId.clear();
    }

    /** エンジンのトラック状態を画面表示用キャッシュへ同期する */
    void syncFromEngine();
    
    /** スクロールバーの状態を更新する */
    void updateScrollbar();
    
    /** 再生位置（プレイヘッド）が画面外に出た場合に自動スクロールする */
    void autoScrollToPlayhead();

    /** Repaints only the live input meters while the transport is stopped. */
    void repaintInputMeters();

    void scrollBarMoved(juce::ScrollBar* scrollBarThatHasMoved, double newRangeStart) override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    /** 選択トラック。外部からの設定では通知を送り返さない。 */
    int getSelectedTrackIndex() const { return selectedTrackIndex; }
    void setSelectedTrackIndex(int trackIndex);
    std::function<void(int)> onSelectedTrackChanged;
    /** 右側の補正・MIX・曲構成・仕上げ画面を開く。 */
    std::function<void(int)> onStudioClicked;
    std::function<void(int)> onVst3BrowserClicked;
    
private:
    struct WaveformCacheLevel
    {
        int64_t samplesPerPeak = 1;
        std::vector<float> peaks;
    };

    struct WaveformCache
    {
        const void* bufferIdentity = nullptr;
        std::weak_ptr<juce::AudioBuffer<float>> bufferOwner;
        int numSamples = 0;
        int sampleRate = 0;
        std::vector<WaveformCacheLevel> levels;
    };

    AudioEngine* audioEngine = nullptr;
    std::vector<TrackData> cachedTracks;

    std::unordered_map<std::string, std::shared_ptr<const WaveformCache>> waveformCaches;
    std::unordered_set<std::string> waveformJobsInFlight;
    std::unordered_set<std::string> currentWaveformKeys;
    std::atomic<std::uint64_t> waveformGeneration { 1 };
    std::atomic<bool> waveformShutdownRequested { false };
    juce::ThreadPool waveformThreadPool {
        juce::ThreadPool::Options{}
            .withNumberOfThreads(1)
            .withThreadName("Waveform Cache")
            .withDesiredThreadPriority(juce::Thread::Priority::background)
    };
    
    juce::TextButton addTrackBtn { "+ Add Track" };
    juce::TextButton loadFileBtn { "Load File" };
    juce::TextButton snapBtn { juce::CharPointer_UTF8(u8"スナップ ON") };
    juce::TextButton studioBtn { juce::CharPointer_UTF8(u8"制作パネル") };
    juce::Label trackNameEditor;
    
    juce::ScrollBar horizontalScrollBar { false }; // 横方向スクロールバー
    juce::ScrollBar verticalScrollBar { true };     // 縦方向スクロールバー

    std::shared_ptr<juce::FileChooser> fileChooser;
    std::shared_ptr<juce::FileChooser> vst3FileChooser;
    bool isFileDragOver = false;
    bool snapEnabled = true;
    bool isUpdatingScrollbars = false;
    int editingTrackIndex = -1;
    int selectedTrackIndex = -1;
    juce::String selectedTrackId;
    void selectTrack(int trackIndex);

    struct TrackHeaderLayout
    {
        juce::Rectangle<int> title, mute, solo, arm, effects, remove;
    };
    TrackHeaderLayout getTrackHeaderLayout(int trackIndex) const;
    void showTrackHeaderMenu(int trackIndex, juce::Rectangle<int> targetBounds);

    static constexpr int trackHeight = 148;
    static constexpr int trackGap = 8;
    static constexpr int sidebarWidth = 250;
    static constexpr int leftMargin = 18;
    static constexpr int actionToolbarHeight = 52;
    static constexpr int timeRulerHeight = 48;
    static constexpr int rulerHeight = actionToolbarHeight + timeRulerHeight;
    static constexpr int scrollbarThickness = 16;

    int getTrackStartY() const;
    int getTrackIndexAtY(int y) const;
    int getViewportBottom() const;
    double getVerticalContentHeight() const;
    double getCachedDuration() const;
    double snapTimeToBeat(double time) const;
    void updateSnapButtonText();
    void beginTrackNameEdit(int trackIndex);
    void finishTrackNameEdit();
    void loadAudioFiles(const juce::Array<juce::File>& files);
    void showVst3Menu(int trackIndex);
    void showTrackPitchMenu(int trackIndex, juce::Rectangle<int> targetBounds);
    void showTrackSpeedMenu(int trackIndex, juce::Rectangle<int> targetBounds);
    void chooseVst3File(int trackIndex);
    static bool isSupportedAudioFile(const juce::File& file);
    static std::string getWaveformCacheKey(const AudioClip& clip);
    static int getWaveformBaseSamplesPerPeak(int numSamples);
    void updateWaveformCaches();
    void scheduleWaveformCache(const AudioClip& clip);
    std::shared_ptr<const WaveformCache> findWaveformCache(const AudioClip& clip) const;

    /** 波形データを描画する */
    void drawWaveform(juce::Graphics& g, const AudioClip& clip, juce::Rectangle<int> area);
    
    /** 録音中のリアルタイム波形データを描画する */
    void drawRecordingWaveform(juce::Graphics& g, const std::vector<float>& buffer, int sampleRate, juce::Rectangle<int> area);

    // クリップドラッグ管理
    juce::String draggingClipId = "";
    enum class ClipDragMode
    {
        none,
        move,
        trimLeft,
        trimRight,
        fadeIn,
        fadeOut,
        rhythmMove,
        rhythmStart,
        rhythmEnd
    };
    ClipDragMode clipDragMode = ClipDragMode::none;
    bool isDraggingClip = false;
    double dragStartClipTime = 0.0;
    double dragStartClipEndTime = 0.0;
    double dragStartFadeIn = 0.0;
    double dragStartFadeOut = 0.0;
    double dragTrackSpeed = 1.0;
    int dragRhythmMarkerIndex = -1;
    std::vector<RhythmMarker> dragStartRhythmMarkers;
    int dragStartMouseX = 0;
    int dragStartTrackIndex = -1;
    
    enum class LoopMarkerDrag { none, start, end };
    LoopMarkerDrag loopMarkerDrag = LoopMarkerDrag::none;
    
    // パン（スクロール）管理
    double scrollX = 0.0;
    double scrollY = 0.0;
    int handDragStartX = 0;
    int handDragStartY = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackAreaComponent)
};
