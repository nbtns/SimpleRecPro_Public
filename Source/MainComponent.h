#pragma once
#include <JuceHeader.h>
#include "HeaderComponent.h"
#include "TrackAreaComponent.h"
#include "TransportBarComponent.h"
#include "AudioEngine.h"
#include "ProjectSerializer.h"
#include "ExportDialogComponent.h"
#include "StudioPanelComponent.h"
#include "AutoMixAnalyzer.h"
#include "TempoEstimator.h"
#include "Vst3BrowserComponent.h"
#include "Vst3PluginCatalog.h"
#include "AssistantController.h"
#include "AssistantBridge.h"
#include "ComparisonBarComponent.h"

class MainComponent : public juce::AudioAppComponent,
                      public juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    //==============================================================================
    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;

    //==============================================================================
    void paint (juce::Graphics& g) override;
    void resized() override;
    
    /** タイマーコールバック: UI更新（再生位置の表示等） */
    void timerCallback() override;
    
    bool keyPressed(const juce::KeyPress& key) override;

    /** AudioEngineへの参照を取得 */
    AudioEngine& getAudioEngine() { return audioEngine; }

    /** 未保存確認を行ってアプリを終了する */
    void requestQuit();

private:
    struct LatencyCalibrationSession;

    AudioEngine audioEngine;
    std::unique_ptr<AssistantController> assistantController;
    std::unique_ptr<AssistantBridge> assistantBridge;
    ComparisonBarComponent comparisonBar;
    
    HeaderComponent header;
    TrackAreaComponent trackArea;
    TransportBarComponent transportBar;

    juce::AudioBuffer<float> tempInputBuffer;
    int currentNumInputChannels = 0;

    std::unique_ptr<LatencyCalibrationSession> latencyCalibration;
    std::atomic<bool> latencyDeviceRefreshRequested { false };
    juce::ThreadPool latencyAnalysisThread {
        juce::ThreadPool::Options{}
            .withNumberOfThreads(1)
            .withThreadName("Latency Calibration")
            .withDesiredThreadPriority(juce::Thread::Priority::background)
    };

    std::unique_ptr<juce::PropertiesFile> appSettings;
    std::unique_ptr<juce::FileChooser> projectFileChooser;
    juce::ScopedMessageBox activeMessageBox;
    juce::File currentProjectFile;
    juce::File recoveryFile;
    juce::File vst3CatalogStateFile;
    juce::StringArray recentProjectPaths;
    juce::String projectDisplayName { juce::CharPointer_UTF8(u8"新規プロジェクト") };
    juce::uint64 lastRecoverySaveMs = 0;
    double manualRecordingOffsetMs = 0.0;
    bool recoveryPromptShown = false;
    bool recoveryDecisionDeferred = false;
    juce::CriticalSection recoveryFileLock;
    std::atomic<juce::uint64> recoveryGeneration { 1 };
    std::atomic<bool> recoverySaveInProgress { false };
    std::atomic<std::uint64_t> lastRecoveryContentRevision { 0 };
    std::atomic<bool> projectIoInProgress { false };
    juce::ThreadPool projectIoThread {
        juce::ThreadPool::Options{}
            .withNumberOfThreads(1)
            .withThreadName("Project I/O")
            .withDesiredThreadPriority(juce::Thread::Priority::background)
    };
    juce::ThreadPool recoverySaveThread {
        juce::ThreadPool::Options{}
            .withNumberOfThreads(1)
            .withThreadName("Project Recovery")
            .withDesiredThreadPriority(juce::Thread::Priority::background)
    };
    juce::ThreadPool studioWorkThread {
        juce::ThreadPool::Options{}
            .withNumberOfThreads(1)
            .withThreadName("Studio Analysis")
            .withDesiredThreadPriority(juce::Thread::Priority::background)
    };
    Vst3PluginCatalog vst3Catalog;
    juce::File vst3ScanMarkerFile;
    juce::File pendingVst3ScanFile;
    std::unique_ptr<juce::FileChooser> exportFileChooser;
    std::unique_ptr<juce::FileChooser> referenceMixFileChooser;
    bool exportDestinationPending = false;
    bool bpmAnalysisInProgress = false;
    bool autoMixAnalysisInProgress = false;
    bool referenceMixAnalysisInProgress = false;
    juce::Component::SafePointer<ExportDialogComponent> exportDialog;
    juce::Component::SafePointer<StudioPanelComponent> studioPanel;
    std::unique_ptr<StudioPanelComponent> studioDock;
    juce::Component::SafePointer<Vst3BrowserComponent> vst3Browser;
    juce::Component::SafePointer<juce::DialogWindow> exportWindow;
    juce::Component::SafePointer<juce::DialogWindow> vst3Window;
    juce::Component::SafePointer<juce::DialogWindow> bpmCandidateWindow;
    juce::Component::SafePointer<juce::DialogWindow> settingsWindow;

    void initialiseAppSettings();
    void configureProjectActions();
    void requestOpenProject();
    void requestOpenRecentProject(const juce::File& file);
    void saveProject(bool forceSaveAs, std::function<void(bool)> completion = {});
    void saveProjectToFile(const juce::File& file,
                           std::function<void(bool)> completion = {});
    void loadProjectFile(const juce::File& file, bool fromRecovery = false);
    void confirmBeforeDestructiveAction(const juce::String& actionText, std::function<void()> continuation);
    void rememberRecentProject(const juce::File& file);
    void refreshProjectDisplay();
    void maybeSaveRecovery();
    void offerRecoveryIfAvailable();
    void clearRecoveryFile(bool force = false);
    void showStatus(const juce::String& message, bool isError = false);
    void setManualRecordingOffsetMs(double milliseconds);
    void startLatencyCalibration();
    void cancelLatencyCalibration();
    bool processLatencyCalibration(const juce::AudioSourceChannelInfo& bufferToFill,
                                   const float* const* inputChannelData,
                                   int numInputChannels) noexcept;
    void serviceLatencyCalibration();
    void beginLatencyCalibrationAnalysis();
    void applyLatencyCalibrationResult();
    void refreshLatencyCalibrationForCurrentDevice();
    juce::String createLatencyCalibrationFingerprint() const;
    juce::String getLatencyCalibrationStatusText() const;
    bool isLatencyCalibrationRunning() const;
    void showExportDialog();
    void showStudioPanel(int selectedTrackIndex);
    void showVst3Browser(int selectedTrackIndex);
    void refreshStudioPanel(int selectedTrackIndex);
    void refreshVst3Browser(int selectedTrackIndex);
    void rebuildVst3Catalog();
    void estimateTrackBpm(int trackIndex);
    void runAutoMix();
    void chooseAndRunReferenceMix(int selectedTrackIndex);
    juce::var handleAssistantRequest(const juce::var& request);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
