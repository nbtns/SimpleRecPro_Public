#pragma once

#include <JuceHeader.h>

#include "DawFeatureTypes.h"

#include <functional>

/**
 * WAV / MP3 の書き出し条件と進捗を1画面で扱うダイアログ内容。
 *
 * 実際のレンダリングやファイル保存は呼び出し側が担当する。この部品は
 * AudioEngine に依存せず、確定した ExportSettings だけを通知する。
 */
class ExportDialogComponent : public juce::Component
{
public:
    ExportDialogComponent(const juce::StringArray& trackNames,
                          const ExportSettings& initialSettings);
    ~ExportDialogComponent() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void setTrackNames(const juce::StringArray& newTrackNames);
    void setSettings(const ExportSettings& newSettings);
    ExportSettings getSettings() const;

    /** 0.0〜1.0 の進捗と、その下に表示する短い説明を更新する。 */
    void setProgress(double progress, const juce::String& message = {});

    /** 実行中は条件変更・開始・閉じる操作を無効にし、キャンセルだけを有効にする。 */
    void setExportRunning(bool shouldBeRunning);
    bool isExportRunning() const noexcept { return exportRunning; }

    std::function<void(const ExportSettings&)> onStartExport;
    std::function<void()> onCancelExport;
    std::function<void()> onClose;

private:
    void configureComboBoxes();
    void updateControlsFromSettings();
    void updateDependentControls();
    void updateEnabledState();
    ExportSettings collectSettings() const;
    bool hasValidABRange() const noexcept;

    juce::Label titleLabel;
    juce::Label descriptionLabel;

    juce::Label formatLabel;
    juce::ComboBox formatBox;
    juce::Label channelsLabel;
    juce::ComboBox channelsBox;
    juce::Label qualityLabel;
    juce::ComboBox qualityBox;
    juce::Label sampleRateLabel;
    juce::ComboBox sampleRateBox;
    juce::Label targetLabel;
    juce::ComboBox targetBox;
    juce::Label trackLabel;
    juce::ComboBox trackBox;
    juce::Label rangeLabel;

    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };
    juce::Label progressLabel;

    juce::TextButton startButton;
    juce::TextButton cancelButton;
    juce::TextButton closeButton;

    juce::StringArray availableTrackNames;
    ExportSettings settings;
    bool updatingControls = false;
    bool exportRunning = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExportDialogComponent)
};

