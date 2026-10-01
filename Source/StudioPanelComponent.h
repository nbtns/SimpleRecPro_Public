#pragma once

#include <JuceHeader.h>

#include "DawFeatureTypes.h"

#include <functional>

/** StudioPanelComponent が表示する、エンジン非依存の画面状態。 */
struct StudioPanelState
{
    juce::StringArray trackNames;
    int selectedTrackIndex = -1;
    TrackRole selectedTrackRole = TrackRole::unknown;
    bool pitchNetInstalled = false;
    bool pitchCorrectionApplied = false;
    float trackVolume = 1.0f;
    float trackPan = 0.0f;
    double trackPitchSemitones = 0.0;
    double trackPlaybackSpeed = 1.0;

    PitchCorrectionSettings pitchCorrection;
    SimpleMixSettings simpleMix;
    NoiseReductionSettings noiseReduction;
    MasteringSettings mastering;
    bool hasSpecialFxRange = false;
    double specialFxRangeStartSeconds = 0.0;
    double specialFxRangeEndSeconds = 0.0;
    int specialFxRegionCount = 0;

    float rhythmStrength = 0.60f;
    TempoPoint tempoPoint;
    int harmonySemitones = 3;
};

/**
 * 初心者向けの「補正 → MIX → 曲構成 → 仕上げ」を1画面にまとめたパネル。
 * AudioEngine へ直接依存せず、すべての操作をコールバックで呼び出し側へ通知する。
 */
class StudioPanelComponent : public juce::Component
{
public:
    StudioPanelComponent();
    ~StudioPanelComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void setState(const StudioPanelState& newState);
    void setDockedMode(bool shouldDock);
    StudioPanelState getState() const;
    void setMasteringMeters(float loudnessLufs,
                            float outputPeak,
                            float gainReductionDb);
    void setAutoMixStatus(const juce::String& message, bool isBusy);
    void setReferenceMixStatus(const juce::String& message, bool isBusy);

    std::function<void(int)> onSelectedTrackChanged;
    std::function<void(int, float)> onTrackVolumeChanged;
    std::function<void(int, float)> onTrackPanChanged;
    std::function<void(int, double)> onTrackPitchSemitonesChanged;
    std::function<void(int, double)> onTrackPlaybackSpeedChanged;
    std::function<void(int)> onResetTrackTransform;
    std::function<void()> onBeginTrackEdit;
    std::function<void()> onEndTrackEdit;
    std::function<void(int, TrackRole)> onTrackRoleChanged;
    std::function<void(int, const PitchCorrectionSettings&)> onNaturalPitchCorrection;
    std::function<void(int)> onOpenPitchDetails;
    std::function<void(int, bool)> onPitchComparisonChanged;
    std::function<void(int, const SimpleMixSettings&)> onMixSettingsChanged;
    std::function<void(int, const NoiseReductionSettings&)> onNoiseReductionChanged;
    std::function<void(int)> onOpenVst3Browser;
    std::function<void()> onRunAutoMix;
    std::function<void(int, SpecialFxType, float, float, double)> onApplySpecialFx;
    std::function<void(int)> onClearSpecialFx;
    std::function<void(int)> onRunReferenceMix;
    std::function<void(int, float)> onRhythmCorrection;
    std::function<void(int)> onBpmEstimate;
    std::function<void(const TempoPoint&)> onAddTempoPoint;
    std::function<void(int, int)> onCreateHarmony;
    std::function<void(const MasteringSettings&)> onMasteringSettingsChanged;
    std::function<void()> onClose;

private:
    void configureControls();
    void configureCallbacks();
    void updateControlsFromState();
    void updateTrackDependentState();
    void updatePitchStatus();
    void showSelectedPage();
    void resizedBasicPage();

    PitchCorrectionSettings collectPitchSettings() const;
    SimpleMixSettings collectMixSettings() const;
    NoiseReductionSettings collectNoiseSettings() const;
    MasteringSettings collectMasteringSettings() const;
    TempoPoint collectTempoPoint() const;

    void resizedPitchPage();
    void resizedMixPage();
    void resizedSpecialFxPage();
    void resizedCompositionPage();
    void resizedMasteringPage();

    class PanelLookAndFeel;
    std::unique_ptr<PanelLookAndFeel> panelLookAndFeel;
    StudioPanelState state;
    bool updatingControls = false;
    bool dockedMode = false;

    juce::Label titleLabel;
    juce::Label trackLabel;
    juce::ComboBox trackBox;
    juce::TextButton closeButton;
    juce::Label categoryLabel;
    juce::ComboBox categoryBox;
    juce::Viewport pageViewport;
    juce::Component basicPage;
    juce::Component pitchPage;
    juce::Component mixPage;
    juce::Component specialFxPage;
    juce::Component compositionPage;
    juce::Component masteringPage;

    juce::Label basicHeading;
    juce::Label trackVolumeLabel;
    juce::Slider trackVolumeSlider;
    juce::Label trackPanLabel;
    juce::Slider trackPanSlider;
    juce::Label basicHelpLabel;
    juce::Label trackPitchLabel;
    juce::Slider trackPitchSlider;
    juce::Label trackSpeedLabel;
    juce::Slider trackSpeedSlider;
    juce::TextButton resetTrackTransformButton;

    // ピッチ補正
    juce::Label pitchHeading;
    juce::Label pitchStatusLabel;
    juce::Label keyLabel;
    juce::ComboBox keyBox;
    juce::Label scaleLabel;
    juce::ComboBox scaleBox;
    juce::Label pitchStrengthLabel;
    juce::Slider pitchStrengthSlider;
    juce::ToggleButton correctedPreviewToggle;
    juce::TextButton naturalPitchButton;
    juce::TextButton pitchDetailsButton;

    // MIX / FX
    juce::Label mixHeading;
    juce::Label trackRoleLabel;
    juce::ComboBox trackRoleBox;
    juce::ToggleButton mixEnabledToggle;
    juce::Label presetLabel;
    juce::ComboBox presetBox;
    juce::Label brightnessLabel;
    juce::Slider brightnessSlider;
    juce::Label ambienceLabel;
    juce::Slider ambienceSlider;
    juce::Label stabilityLabel;
    juce::Slider stabilitySlider;
    juce::ToggleButton noiseEnabledToggle;
    juce::Label noiseAmountLabel;
    juce::Slider noiseAmountSlider;
    juce::Label deEssLabel;
    juce::Slider deEssSlider;
    juce::TextButton applyMixButton;
    juce::TextButton vst3BrowserButton;
    juce::TextButton autoMixButton;
    juce::Label autoMixStatusLabel;
    bool autoMixBusy = false;

    // 範囲指定の内蔵特殊FX / 参考曲MIX
    juce::Label specialFxHeading;
    juce::Label specialFxRangeLabel;
    juce::Label specialFxTypeLabel;
    juce::ComboBox specialFxTypeBox;
    juce::Label specialFxAmountLabel;
    juce::Slider specialFxAmountSlider;
    juce::Label specialFxWetLabel;
    juce::Slider specialFxWetSlider;
    juce::Label specialFxFadeLabel;
    juce::Slider specialFxFadeSlider;
    juce::TextButton applySpecialFxButton;
    juce::TextButton clearSpecialFxButton;
    juce::Label referenceMixHeading;
    juce::Label referenceMixHelpLabel;
    juce::TextButton referenceMixButton;
    juce::Label referenceMixStatusLabel;
    bool referenceMixBusy = false;

    // 曲構成
    juce::Label compositionHeading;
    juce::Label rhythmLabel;
    juce::Slider rhythmSlider;
    juce::TextButton rhythmButton;
    juce::TextButton bpmEstimateButton;
    juce::Label tempoPositionLabel;
    juce::Label tempoBpmLabel;
    juce::Slider tempoBpmSlider;
    juce::Label timeSignatureLabel;
    juce::ComboBox timeSignatureBox;
    juce::TextButton addTempoPointButton;
    juce::Label harmonyLabel;
    juce::ComboBox harmonyBox;
    juce::TextButton harmonyButton;

    // 仕上げ
    juce::Label masteringHeading;
    juce::ToggleButton masteringEnabledToggle;
    juce::ToggleButton limiterEnabledToggle;
    juce::Label ceilingLabel;
    juce::Slider ceilingSlider;
    juce::Label loudnessLabel;
    juce::ComboBox loudnessBox;
    juce::ToggleButton masteredPreviewToggle;
    juce::Label masteringMeterLabel;
    juce::Label masteringHelpLabel;
    juce::TextButton applyMasteringButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StudioPanelComponent)
};
