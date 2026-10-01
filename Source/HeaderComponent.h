#pragma once
#include <JuceHeader.h>

class AudioEngine; // 前方宣言

class HeaderComponent : public juce::Component
{
public:
    HeaderComponent();
    ~HeaderComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    
    /** AudioEngineを設定 */
    void setAudioEngine(AudioEngine* engine);
    
    void triggerSave() { saveButton.triggerClick(); }
    void triggerOpen() { openButton.triggerClick(); }
    void triggerUndo() { undoButton.triggerClick(); }
    void triggerRedo() { redoButton.triggerClick(); }
    void triggerSaveAs() { saveAsButton.triggerClick(); }

    /** 現在のプロジェクト名と未保存状態を表示 */
    void setProjectDisplayName(const juce::String& name, bool isDirty);

    /** 最近使ったプロジェクト一覧を更新 */
    void setRecentProjects(const juce::StringArray& paths);

    /** AudioEngineの現在値を画面へ反映 */
    void syncFromEngine();
    
    std::function<void()> onSettingsClicked;
    std::function<void()> onOpenClicked;
    std::function<void()> onSaveClicked;
    std::function<void()> onSaveAsClicked;
    std::function<void()> onExportClicked;
    std::function<void(const juce::File&)> onRecentProjectSelected;

private:
    AudioEngine* audioEngine = nullptr;
    
    juce::Label titleLabel;
    juce::Label projectLabel;
    
    juce::TextButton openButton { "Open" };
    juce::TextButton saveButton { "Save" };
    juce::TextButton saveAsButton { "SaveAs" };
    juce::TextButton recentButton { "Recent" };
    juce::TextButton undoButton { "Undo" };
    juce::TextButton redoButton { "Redo" };
    juce::StringArray recentProjectPaths;
    
    // UI Tools removed from here
    
    juce::TextButton exportButton;
    
    juce::TextButton settingsButton { "Set" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeaderComponent)
};
