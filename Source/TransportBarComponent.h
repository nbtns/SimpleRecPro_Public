#pragma once
#include <JuceHeader.h>
#include "AudioEngine.h"

/**
 * カスタムツールボタン（矢印・ハンド・分割のアイコン描画用）
 */
class ToolButton : public juce::Button
{
public:
    enum class Type { Arrow, Hand, Split };
    
    ToolButton(const juce::String& name, Type t);
    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

private:
    Type type;
};

/**
 * TransportBarComponent - 下部の操作パネル
 * 録音・再生・停止ボタン、時間表示などを配置
 * AudioEngineへのポインタを受け取り、ボタン操作で直接制御する
 */
class TransportBarComponent : public juce::Component,
                              private juce::Timer
{
public:
    TransportBarComponent();
    ~TransportBarComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** 編集操作はタイムライン上部、録音操作は下部へ配置する。 */
    juce::Component& getEditingToolbar() { return editingToolbar; }
    int getPreferredHeight(int width) const;
    int getEditingToolbarHeight(int width) const;
    
    /** AudioEngineを設定（MainComponentから呼ばれる） */
    void setAudioEngine(AudioEngine* engine);

    /** プロジェクト読込後などに、エンジン側の設定を操作バーへ反映する。 */
    void syncFromEngine();

    /** 操作結果や入力エラーを、編集ツールバーの通知欄へ一定時間表示する。 */
    void showStatusMessage(const juce::String& message, bool isError = false);
    
    /** 時間表示を更新（MainComponentのtimerCallbackから呼ばれる） */
    void updateTimeDisplay(double currentTime, double duration);
    
    /** 再生状態の変更に応じてボタン表示を更新 */
    void updatePlaybackState(PlaybackState state);

    /** 詳細なWAV／MP3書き出し画面をMainComponentへ依頼する。 */
    std::function<void()> onExportClicked;

private:
    class EditingToolbar : public juce::Component
    {
    public:
        std::function<void()> onResized;
        void resized() override { if (onResized) onResized(); }
        void paint(juce::Graphics&) override;
    };

    void timerCallback() override;
    void layoutEditingToolbar();

    AudioEngine* audioEngine = nullptr;
    EditingToolbar editingToolbar;
    
    juce::Label bpmTitleLabel { "BPMTitle", "BPM" };
    juce::Label bpmValueLabel { "BPMValue", "120" };
    juce::TextButton tapButton { "TAP" };
    juce::TextButton tempoDetailsButton { juce::CharPointer_UTF8(u8"▼") };
    std::vector<juce::uint32> tapTimes;
    juce::TextButton timeSigButton;
    juce::TextButton clickButton { "Click" };
    juce::TextButton countInButton { "Count-In" };
    juce::Label clickVolLabel { "ClickVolumeLabel",
                                juce::CharPointer_UTF8(u8"音量") };
    juce::Slider clickVolSlider;
    juce::Label masterVolLabel { "MasterVolume", juce::CharPointer_UTF8(u8"全体音量") };
    juce::Label masterVolValue { "MasterVolumeValue", "80 %" };
    juce::Slider masterVolSlider;

    juce::TextButton recordButton { "R" };
    juce::TextButton playButton { "P" };
    juce::TextButton stopButton { "S" };

    juce::Label timeLabel { "Time", "00:00 / 00:00" };
    juce::Label playbackLabel { "Playback", juce::CharPointer_UTF8(u8"停止中") };
    juce::Label durationLabel { "Duration", " / 00:00" };
    juce::Label statusLabel { "Status", {} };

    ToolButton arrowButton { "Arrow", ToolButton::Type::Arrow };
    ToolButton handButton { "Hand", ToolButton::Type::Hand };
    ToolButton splitButton { "Split", ToolButton::Type::Split };
    juce::TextButton loopStartButton { "A" };
    juce::TextButton loopEndButton { "B" };
    juce::TextButton loopToggleButton { "LOOP" };
    juce::TextButton loopClearButton { juce::CharPointer_UTF8(u8"解除") };
    
    juce::Slider zoomSlider;
    juce::Label zoomLabel { "Zoom", "100%" };
    juce::TextButton exportButton { "Export" };

    void updateZoomLabelText();
    void updateLoopControls();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportBarComponent)
};
