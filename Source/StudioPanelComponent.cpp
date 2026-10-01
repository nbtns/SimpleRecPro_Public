#include "StudioPanelComponent.h"
#include "SimpleMixProcessor.h"

#include "UiTheme.h"
#include "UiLookAndFeel.h"

#include <cmath>

namespace
{
juce::String uiText(const char* text)
{
    return juce::String::fromUTF8(text);
}

void styleLabel(juce::Label& label, float size = 14.0f, bool bold = false)
{
    label.setColour(juce::Label::textColourId,
                    bold ? UiTheme::textPrimary : UiTheme::textSecondary);
    const auto readableSize = size >= 16.0f ? 19.0f : (bold ? 16.0f : 14.0f);
    label.setFont(juce::Font(juce::FontOptions(readableSize,
        bold ? juce::Font::bold : juce::Font::plain)));
    label.setMinimumHorizontalScale(1.0f);
    label.setJustificationType(juce::Justification::centredLeft);
}

void styleCombo(juce::ComboBox& box)
{
    box.setScrollWheelEnabled(false);
    box.setColour(juce::ComboBox::backgroundColourId, UiTheme::controlSurface);
    box.setColour(juce::ComboBox::outlineColourId, UiTheme::border);
    box.setColour(juce::ComboBox::textColourId, UiTheme::textPrimary);
    box.setColour(juce::ComboBox::arrowColourId, UiTheme::textSecondary);
}

void styleSlider(juce::Slider& slider, double minimum, double maximum,
                 double interval, const juce::String& suffix = {})
{
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 82, 32);
    slider.setScrollWheelEnabled(false);
    slider.setRange(minimum, maximum, interval);
    slider.setTextValueSuffix(suffix);
    slider.setColour(juce::Slider::trackColourId, UiTheme::accent);
    slider.setColour(juce::Slider::backgroundColourId, UiTheme::controlSurface);
    slider.setColour(juce::Slider::thumbColourId, UiTheme::textPrimary);
    slider.setColour(juce::Slider::textBoxTextColourId, UiTheme::textPrimary);
    slider.setColour(juce::Slider::textBoxBackgroundColourId, UiTheme::raisedSurface);
    slider.setColour(juce::Slider::textBoxOutlineColourId, UiTheme::border);
}

juce::String formatPosition(double seconds)
{
    const int total = juce::jmax(0, juce::roundToInt(seconds));
    return juce::String::formatted("%02d:%02d", total / 60, total % 60);
}

MusicalScale scaleForId(int id)
{
    switch (id)
    {
        case 2: return MusicalScale::major;
        case 3: return MusicalScale::naturalMinor;
        case 4: return MusicalScale::harmonicMinor;
        case 5: return MusicalScale::pentatonicMajor;
        case 6: return MusicalScale::pentatonicMinor;
        default: return MusicalScale::chromatic;
    }
}

int idForScale(MusicalScale scale)
{
    return static_cast<int>(scale) + 1;
}

MixPreset presetForId(int id)
{
    return static_cast<MixPreset>(juce::jlimit(0, 4, id - 1));
}

SpecialFxType specialFxForId(int id)
{
    return static_cast<SpecialFxType>(juce::jlimit(
        static_cast<int>(SpecialFxType::muffled),
        static_cast<int>(SpecialFxType::bitcrush), id));
}
}

class StudioPanelComponent::PanelLookAndFeel : public ModernLookAndFeel
{
public:
    int getSliderThumbRadius(juce::Slider&) override { return 10; }

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                          bool highlighted, bool down) override
    {
        const float tickSize = 24.0f;
        drawTickBox(g, button, 0.0f, (button.getHeight() - tickSize) * 0.5f,
                    tickSize, tickSize, button.getToggleState(),
                    button.isEnabled(), highlighted, down);
        g.setColour(button.findColour(juce::ToggleButton::textColourId)
                        .withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.5f));
        g.setFont(juce::Font(juce::FontOptions(16.0f)));
        g.drawFittedText(button.getButtonText(),
                         button.getLocalBounds().withTrimmedLeft(32),
                         juce::Justification::centredLeft, 2, 1.0f);
    }
};

StudioPanelComponent::StudioPanelComponent()
{
    panelLookAndFeel = std::make_unique<PanelLookAndFeel>();
    panelLookAndFeel->setColour(juce::ToggleButton::textColourId, UiTheme::textPrimary);
    panelLookAndFeel->setColour(juce::ToggleButton::tickColourId, UiTheme::accent);
    panelLookAndFeel->setColour(juce::PopupMenu::backgroundColourId, UiTheme::panelBackground);
    panelLookAndFeel->setColour(juce::PopupMenu::textColourId, UiTheme::textPrimary);
    panelLookAndFeel->setColour(juce::TextButton::textColourOffId, UiTheme::textPrimary);
    setLookAndFeel(panelLookAndFeel.get());
    setSize(420, 720);
    configureControls();
    configureCallbacks();
    updateControlsFromState();
    resized();
}

StudioPanelComponent::~StudioPanelComponent()
{
    pageViewport.setViewedComponent(nullptr, false);
    setLookAndFeel(nullptr);
}

void StudioPanelComponent::setDockedMode(bool shouldDock)
{
    dockedMode = shouldDock;
    titleLabel.setText(uiText(dockedMode ? u8"トラック調整" : u8"スタジオ"),
                       juce::dontSendNotification);
    closeButton.setButtonText(uiText(dockedMode ? u8"たたむ" : u8"閉じる"));
    resized();
    repaint();
}

void StudioPanelComponent::configureControls()
{
    titleLabel.setText(uiText(u8"スタジオ"), juce::dontSendNotification);
    titleLabel.setColour(juce::Label::textColourId, UiTheme::textPrimary);
    titleLabel.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    trackLabel.setText(uiText(u8"編集するトラック"), juce::dontSendNotification);
    styleLabel(trackLabel, 12.0f, true);
    styleCombo(trackBox);
    closeButton.setButtonText(uiText(u8"閉じる"));
    closeButton.setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);

    addAndMakeVisible(titleLabel);
    addAndMakeVisible(trackLabel);
    addAndMakeVisible(trackBox);
    addAndMakeVisible(closeButton);
    categoryLabel.setText(uiText(u8"調整する項目"), juce::dontSendNotification);
    styleLabel(categoryLabel, 14.0f, true);
    styleCombo(categoryBox);
    categoryBox.addItem(uiText(u8"基本 / 音量・左右バランス"), 1);
    categoryBox.addItem(uiText(u8"MIX / 音の質感・ノイズ"), 2);
    categoryBox.addItem(uiText(u8"ピッチ補正"), 3);
    categoryBox.addItem(uiText(u8"特殊FX / 参考曲MIX"), 4);
    categoryBox.addItem(uiText(u8"曲構成 / テンポ・ハモリ"), 5);
    categoryBox.addItem(uiText(u8"仕上げ / マスタリング"), 6);
    categoryBox.setSelectedId(1, juce::dontSendNotification);
    categoryBox.setScrollWheelEnabled(false);
    addAndMakeVisible(categoryLabel);
    addAndMakeVisible(categoryBox);
    addAndMakeVisible(pageViewport);
    pageViewport.setScrollBarsShown(true, false);
    pageViewport.setScrollBarThickness(12);
    pageViewport.getVerticalScrollBar().setColour(juce::ScrollBar::thumbColourId,
                                                  UiTheme::textMuted);

    basicHeading.setText(uiText(u8"選んだトラックの音を調整"), juce::dontSendNotification);
    trackVolumeLabel.setText(uiText(u8"トラック音量"), juce::dontSendNotification);
    trackPanLabel.setText(uiText(u8"左右バランス（PAN）"), juce::dontSendNotification);
    basicHelpLabel.setText(uiText(u8"音量と左右バランスは、そのまま再生音に反映されます。\nMIXや音程は上の項目から調整できます。"), juce::dontSendNotification);
    styleLabel(basicHeading, 19.0f, true);
    styleLabel(trackVolumeLabel, 14.0f, true);
    styleLabel(trackPanLabel, 14.0f, true);
    styleLabel(basicHelpLabel);
    styleSlider(trackVolumeSlider, 0.0, 100.0, 1.0, "%");
    styleSlider(trackPanSlider, -100.0, 100.0, 1.0);
    trackVolumeSlider.setDoubleClickReturnValue(true, 100.0);
    trackPanSlider.setDoubleClickReturnValue(true, 0.0);
    trackPanSlider.textFromValueFunction = [](double value)
    {
        return std::abs(value) < 0.5 ? uiText(u8"中央")
            : uiText(value < 0.0 ? u8"左 " : u8"右 ") + juce::String(std::abs(value), 0);
    };
    trackPanSlider.valueFromTextFunction = [](const juce::String& text)
    {
        const auto amount = text.retainCharacters("0123456789.-").getDoubleValue();
        return text.contains(uiText(u8"左")) ? -std::abs(amount) : amount;
    };
    for (auto* component : { &basicHeading, &trackVolumeLabel, &trackPanLabel, &basicHelpLabel })
        basicPage.addAndMakeVisible(component);
    basicPage.addAndMakeVisible(trackVolumeSlider);
    basicPage.addAndMakeVisible(trackPanSlider);
    trackPitchLabel.setText(uiText(u8"キー変更（半音）"), juce::dontSendNotification);
    trackSpeedLabel.setText(uiText(u8"再生速度"), juce::dontSendNotification);
    styleLabel(trackPitchLabel, 14.0f, true);
    styleLabel(trackSpeedLabel, 14.0f, true);
    styleSlider(trackPitchSlider, -12.0, 12.0, 1.0);
    styleSlider(trackSpeedSlider, 0.75, 1.5, 0.05, "x");
    trackSpeedSlider.setNumDecimalPlacesToDisplay(2);
    trackPitchSlider.setDoubleClickReturnValue(true, 0.0);
    trackSpeedSlider.setDoubleClickReturnValue(true, 1.0);
    resetTrackTransformButton.setButtonText(uiText(u8"原曲へ戻す（キー・速度）"));
    resetTrackTransformButton.setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);
    basicPage.addAndMakeVisible(trackPitchLabel);
    basicPage.addAndMakeVisible(trackPitchSlider);
    basicPage.addAndMakeVisible(trackSpeedLabel);
    basicPage.addAndMakeVisible(trackSpeedSlider);
    basicPage.addAndMakeVisible(resetTrackTransformButton);
    showSelectedPage();

    pitchHeading.setText(uiText(u8"声の音程を自然に整える"), juce::dontSendNotification);
    pitchStatusLabel.setText({}, juce::dontSendNotification);
    keyLabel.setText(uiText(u8"曲の調"), juce::dontSendNotification);
    scaleLabel.setText(uiText(u8"音階"), juce::dontSendNotification);
    pitchStrengthLabel.setText(uiText(u8"補正の強さ"), juce::dontSendNotification);
    styleLabel(pitchHeading, 16.0f, true);
    styleLabel(pitchStatusLabel, 12.0f);
    styleLabel(keyLabel, 12.0f, true);
    styleLabel(scaleLabel, 12.0f, true);
    styleLabel(pitchStrengthLabel, 12.0f, true);
    styleCombo(keyBox);
    styleCombo(scaleBox);
    for (int index = 0; index < 12; ++index)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F",
                                       "F#", "G", "G#", "A", "A#", "B" };
        keyBox.addItem(names[index], index + 1);
    }
    scaleBox.addItem(uiText(u8"クロマチック（全音）"), 1);
    scaleBox.addItem(uiText(u8"メジャー"), 2);
    scaleBox.addItem(uiText(u8"ナチュラルマイナー"), 3);
    scaleBox.addItem(uiText(u8"ハーモニックマイナー"), 4);
    scaleBox.addItem(uiText(u8"メジャーペンタトニック"), 5);
    scaleBox.addItem(uiText(u8"マイナーペンタトニック"), 6);
    styleSlider(pitchStrengthSlider, 0.0, 100.0, 1.0, "%");
    correctedPreviewToggle.setButtonText(uiText(u8"補正後を聴く（OFFで原音）"));
    naturalPitchButton.setButtonText(uiText(u8"自然に補正"));
    pitchDetailsButton.setButtonText(uiText(u8"詳細を開く"));
    naturalPitchButton.setColour(juce::TextButton::buttonColourId, UiTheme::accent);
    pitchDetailsButton.setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);
    auto addPitchControl = [this](juce::Component& component)
    {
        pitchPage.addAndMakeVisible(component);
    };
    addPitchControl(pitchHeading);
    addPitchControl(pitchStatusLabel);
    addPitchControl(keyLabel);
    addPitchControl(keyBox);
    addPitchControl(scaleLabel);
    addPitchControl(scaleBox);
    addPitchControl(pitchStrengthLabel);
    addPitchControl(pitchStrengthSlider);
    addPitchControl(correctedPreviewToggle);
    addPitchControl(naturalPitchButton);
    addPitchControl(pitchDetailsButton);

    mixHeading.setText(uiText(u8"役割に合わせて音を整える"), juce::dontSendNotification);
    trackRoleLabel.setText(uiText(u8"トラックの役割"), juce::dontSendNotification);
    mixEnabledToggle.setButtonText(uiText(u8"かんたんMIXを使う"));
    presetLabel.setText(uiText(u8"仕上がり"), juce::dontSendNotification);
    brightnessLabel.setText(uiText(u8"明るさ"), juce::dontSendNotification);
    ambienceLabel.setText(uiText(u8"響き"), juce::dontSendNotification);
    stabilityLabel.setText(uiText(u8"声の安定"), juce::dontSendNotification);
    noiseEnabledToggle.setButtonText(uiText(u8"ノイズをやわらげる"));
    noiseAmountLabel.setText(uiText(u8"ノイズ除去"), juce::dontSendNotification);
    deEssLabel.setText(uiText(u8"サ行の強さ"), juce::dontSendNotification);
    styleLabel(mixHeading, 16.0f, true);
    styleLabel(autoMixStatusLabel, 12.0f);
    autoMixStatusLabel.setColour(juce::Label::textColourId, UiTheme::textSecondary);
    autoMixStatusLabel.setJustificationType(juce::Justification::centredLeft);
    for (auto* label : { &trackRoleLabel, &presetLabel, &brightnessLabel, &ambienceLabel,
                         &stabilityLabel, &noiseAmountLabel, &deEssLabel })
        styleLabel(*label, 12.0f, true);
    styleCombo(trackRoleBox);
    trackRoleBox.addItem(uiText(u8"未設定"), 1);
    trackRoleBox.addItem(uiText(u8"メインボーカル"), 2);
    trackRoleBox.addItem(uiText(u8"コーラス"), 3);
    trackRoleBox.addItem(uiText(u8"ハモリ"), 4);
    trackRoleBox.addItem(uiText(u8"伴奏"), 5);
    trackRoleBox.addItem(uiText(u8"その他"), 6);
    styleCombo(presetBox);
    presetBox.addItem(uiText(u8"そのまま"), 1);
    presetBox.addItem(uiText(u8"ナチュラル"), 2);
    presetBox.addItem(uiText(u8"クリア"), 3);
    presetBox.addItem(uiText(u8"広がり"), 4);
    presetBox.addItem(uiText(u8"ラジオ"), 5);
    styleSlider(brightnessSlider, -100.0, 100.0, 1.0, "%");
    styleSlider(ambienceSlider, 0.0, 100.0, 1.0, "%");
    styleSlider(stabilitySlider, 0.0, 100.0, 1.0, "%");
    styleSlider(noiseAmountSlider, 0.0, 100.0, 1.0, "%");
    styleSlider(deEssSlider, 0.0, 100.0, 1.0, "%");
    applyMixButton.setButtonText(uiText(u8"この設定を反映"));
    vst3BrowserButton.setButtonText(uiText(u8"VST3を選ぶ"));
    autoMixButton.setButtonText(uiText(u8"全トラックをお任せMIX"));
    applyMixButton.setColour(juce::TextButton::buttonColourId, UiTheme::accent);
    vst3BrowserButton.setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);
    autoMixButton.setColour(juce::TextButton::buttonColourId, UiTheme::accent);
    auto addMixControl = [this](juce::Component& component)
    {
        mixPage.addAndMakeVisible(component);
    };
    addMixControl(mixHeading);
    addMixControl(trackRoleLabel);
    addMixControl(trackRoleBox);
    addMixControl(mixEnabledToggle);
    addMixControl(presetLabel);
    addMixControl(presetBox);
    addMixControl(brightnessLabel);
    addMixControl(brightnessSlider);
    addMixControl(ambienceLabel);
    addMixControl(ambienceSlider);
    addMixControl(stabilityLabel);
    addMixControl(stabilitySlider);
    addMixControl(noiseEnabledToggle);
    addMixControl(noiseAmountLabel);
    addMixControl(noiseAmountSlider);
    addMixControl(deEssLabel);
    addMixControl(deEssSlider);
    addMixControl(applyMixButton);
    addMixControl(vst3BrowserButton);
    addMixControl(autoMixButton);
    addMixControl(autoMixStatusLabel);

    specialFxHeading.setText(uiText(u8"A-B範囲だけ音の質感を変える"),
                             juce::dontSendNotification);
    specialFxRangeLabel.setText(uiText(u8"下のタイムラインでAとBを設定してください"),
                                juce::dontSendNotification);
    specialFxTypeLabel.setText(uiText(u8"効果"), juce::dontSendNotification);
    specialFxAmountLabel.setText(uiText(u8"効果の強さ"), juce::dontSendNotification);
    specialFxWetLabel.setText(uiText(u8"原音との割合"), juce::dontSendNotification);
    specialFxFadeLabel.setText(uiText(u8"境界をなじませる"), juce::dontSendNotification);
    referenceMixHeading.setText(uiText(u8"参考曲の雰囲気に近づける"),
                                juce::dontSendNotification);
    referenceMixHelpLabel.setText(
        uiText(u8"参考曲の明るさ・音量差・広がり・質感を解析し、\n完全コピーではなく近い雰囲気のMIXを提案します。"),
        juce::dontSendNotification);
    referenceMixHelpLabel.setMinimumHorizontalScale(0.8f);
    for (auto* label : { &specialFxHeading, &specialFxTypeLabel,
                         &specialFxAmountLabel, &specialFxWetLabel,
                         &specialFxFadeLabel, &referenceMixHeading })
        styleLabel(*label, label == &specialFxHeading
                           || label == &referenceMixHeading ? 16.0f : 12.0f,
                   true);
    styleLabel(specialFxRangeLabel, 12.0f);
    styleLabel(referenceMixHelpLabel, 12.0f);
    styleLabel(referenceMixStatusLabel, 12.0f);
    styleCombo(specialFxTypeBox);
    specialFxTypeBox.addItem(uiText(u8"こもる"),
                             static_cast<int>(SpecialFxType::muffled));
    specialFxTypeBox.addItem(uiText(u8"ラジオ・電話"),
                             static_cast<int>(SpecialFxType::radio));
    specialFxTypeBox.addItem(uiText(u8"ノイズ"),
                             static_cast<int>(SpecialFxType::noise));
    specialFxTypeBox.addItem(uiText(u8"歪み"),
                             static_cast<int>(SpecialFxType::distortion));
    specialFxTypeBox.addItem(uiText(u8"デジタル劣化"),
                             static_cast<int>(SpecialFxType::bitcrush));
    specialFxTypeBox.setSelectedId(static_cast<int>(SpecialFxType::muffled),
                                   juce::dontSendNotification);
    styleSlider(specialFxAmountSlider, 0.0, 100.0, 1.0, "%");
    styleSlider(specialFxWetSlider, 0.0, 100.0, 1.0, "%");
    styleSlider(specialFxFadeSlider, 0.0, 500.0, 5.0, " ms");
    specialFxAmountSlider.setValue(65.0, juce::dontSendNotification);
    specialFxWetSlider.setValue(100.0, juce::dontSendNotification);
    specialFxFadeSlider.setValue(30.0, juce::dontSendNotification);
    applySpecialFxButton.setButtonText(uiText(u8"A-B範囲に追加"));
    clearSpecialFxButton.setButtonText(uiText(u8"このトラックの特殊FXを解除"));
    referenceMixButton.setButtonText(uiText(u8"参考曲を選んでお任せMIX"));
    applySpecialFxButton.setColour(juce::TextButton::buttonColourId,
                                   UiTheme::accent);
    clearSpecialFxButton.setColour(juce::TextButton::buttonColourId,
                                   UiTheme::controlSurface);
    referenceMixButton.setColour(juce::TextButton::buttonColourId,
                                 UiTheme::accent);
    auto addSpecialFxControl = [this](juce::Component& component)
    {
        specialFxPage.addAndMakeVisible(component);
    };
    addSpecialFxControl(specialFxHeading);
    addSpecialFxControl(specialFxRangeLabel);
    addSpecialFxControl(specialFxTypeLabel);
    addSpecialFxControl(specialFxTypeBox);
    addSpecialFxControl(specialFxAmountLabel);
    addSpecialFxControl(specialFxAmountSlider);
    addSpecialFxControl(specialFxWetLabel);
    addSpecialFxControl(specialFxWetSlider);
    addSpecialFxControl(specialFxFadeLabel);
    addSpecialFxControl(specialFxFadeSlider);
    addSpecialFxControl(applySpecialFxButton);
    addSpecialFxControl(clearSpecialFxButton);
    addSpecialFxControl(referenceMixHeading);
    addSpecialFxControl(referenceMixHelpLabel);
    addSpecialFxControl(referenceMixButton);
    addSpecialFxControl(referenceMixStatusLabel);

    compositionHeading.setText(uiText(u8"曲の流れとタイミング"), juce::dontSendNotification);
    rhythmLabel.setText(uiText(u8"タイミング補正"), juce::dontSendNotification);
    tempoBpmLabel.setText("BPM", juce::dontSendNotification);
    timeSignatureLabel.setText(uiText(u8"拍子"), juce::dontSendNotification);
    harmonyLabel.setText(uiText(u8"ハモリの高さ"), juce::dontSendNotification);
    styleLabel(compositionHeading, 16.0f, true);
    styleLabel(tempoPositionLabel, 12.0f);
    for (auto* label : { &rhythmLabel, &tempoBpmLabel,
                         &timeSignatureLabel, &harmonyLabel })
        styleLabel(*label, 12.0f, true);
    styleSlider(rhythmSlider, 0.0, 100.0, 1.0, "%");
    styleSlider(tempoBpmSlider, 20.0, 300.0, 1.0);
    styleCombo(timeSignatureBox);
    timeSignatureBox.addItem("4/4", 1);
    timeSignatureBox.addItem("3/4", 2);
    timeSignatureBox.addItem("6/8", 3);
    timeSignatureBox.addItem("2/4", 4);
    timeSignatureBox.addItem("5/4", 5);
    timeSignatureBox.addItem("7/8", 6);
    styleCombo(harmonyBox);
    for (int semitones = -12; semitones <= 12; ++semitones)
    {
        if (semitones == 0)
            continue;
        const auto label = semitones > 0
            ? uiText(u8"上 ") + juce::String(semitones)
            : uiText(u8"下 ") + juce::String(-semitones);
        harmonyBox.addItem(label, semitones + 13);
    }
    rhythmButton.setButtonText(uiText(u8"タイミングを整える"));
    bpmEstimateButton.setButtonText(uiText(u8"伴奏からBPMを調べる"));
    addTempoPointButton.setButtonText(uiText(u8"現在位置にテンポ点を追加"));
    harmonyButton.setButtonText(uiText(u8"ハモリトラックを作る"));
    rhythmButton.setColour(juce::TextButton::buttonColourId, UiTheme::accent);
    addTempoPointButton.setColour(juce::TextButton::buttonColourId, UiTheme::accent);
    bpmEstimateButton.setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);
    harmonyButton.setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);
    auto addCompositionControl = [this](juce::Component& component)
    {
        compositionPage.addAndMakeVisible(component);
    };
    addCompositionControl(compositionHeading);
    addCompositionControl(rhythmLabel);
    addCompositionControl(rhythmSlider);
    addCompositionControl(rhythmButton);
    addCompositionControl(bpmEstimateButton);
    addCompositionControl(tempoPositionLabel);
    addCompositionControl(tempoBpmLabel);
    addCompositionControl(tempoBpmSlider);
    addCompositionControl(timeSignatureLabel);
    addCompositionControl(timeSignatureBox);
    addCompositionControl(addTempoPointButton);
    addCompositionControl(harmonyLabel);
    addCompositionControl(harmonyBox);
    addCompositionControl(harmonyButton);

    masteringHeading.setText(uiText(u8"配信しやすい音量に仕上げる"), juce::dontSendNotification);
    masteringEnabledToggle.setButtonText(uiText(u8"かんたんマスタリングを使う"));
    limiterEnabledToggle.setButtonText(uiText(u8"大きすぎる音を安全に抑える"));
    ceilingLabel.setText(uiText(u8"最大音量"), juce::dontSendNotification);
    loudnessLabel.setText(uiText(u8"音量の目安"), juce::dontSendNotification);
    masteredPreviewToggle.setButtonText(uiText(u8"仕上げ後を聴く（OFFで元の音）"));
    masteringMeterLabel.setText(
        uiText(u8"再生すると音量の目安を表示します"),
        juce::dontSendNotification);
    masteringHelpLabel.setText(
        uiText(u8"配信向けは -14 LUFS、少し大きめは -12 LUFSが目安です。\n比較するときは聞こえる音量を揃えます。"),
        juce::dontSendNotification);
    masteringHelpLabel.setMinimumHorizontalScale(0.8f);
    styleLabel(masteringHeading, 16.0f, true);
    styleLabel(ceilingLabel, 12.0f, true);
    styleLabel(loudnessLabel, 12.0f, true);
    styleLabel(masteringMeterLabel, 12.0f, true);
    masteringMeterLabel.setColour(juce::Label::textColourId, UiTheme::accent);
    styleLabel(masteringHelpLabel, 12.0f);
    styleSlider(ceilingSlider, -6.0, 0.0, 0.1, " dB");
    styleCombo(loudnessBox);
    loudnessBox.addItem("-16 LUFS", 1);
    loudnessBox.addItem("-14 LUFS", 2);
    loudnessBox.addItem("-12 LUFS", 3);
    loudnessBox.addItem("-9 LUFS", 4);
    applyMasteringButton.setButtonText(uiText(u8"仕上げ設定を反映"));
    applyMasteringButton.setColour(juce::TextButton::buttonColourId, UiTheme::accent);
    auto addMasteringControl = [this](juce::Component& component)
    {
        masteringPage.addAndMakeVisible(component);
    };
    addMasteringControl(masteringHeading);
    addMasteringControl(masteringEnabledToggle);
    addMasteringControl(limiterEnabledToggle);
    addMasteringControl(ceilingLabel);
    addMasteringControl(ceilingSlider);
    addMasteringControl(loudnessLabel);
    addMasteringControl(loudnessBox);
    addMasteringControl(masteredPreviewToggle);
    addMasteringControl(masteringMeterLabel);
    addMasteringControl(masteringHelpLabel);
    addMasteringControl(applyMasteringButton);
}

void StudioPanelComponent::configureCallbacks()
{
    categoryBox.onChange = [this] { showSelectedPage(); };
    trackVolumeSlider.onValueChange = [this]
    {
        if (updatingControls) return;
        state.trackVolume = static_cast<float>(trackVolumeSlider.getValue() / 100.0);
        if (onTrackVolumeChanged) onTrackVolumeChanged(state.selectedTrackIndex, state.trackVolume);
    };
    trackPanSlider.onValueChange = [this]
    {
        if (updatingControls) return;
        state.trackPan = static_cast<float>(trackPanSlider.getValue() / 100.0);
        if (onTrackPanChanged) onTrackPanChanged(state.selectedTrackIndex, state.trackPan);
    };
    trackPitchSlider.onValueChange = [this]
    {
        if (updatingControls) return;
        state.trackPitchSemitones = trackPitchSlider.getValue();
        if (onTrackPitchSemitonesChanged)
            onTrackPitchSemitonesChanged(state.selectedTrackIndex, state.trackPitchSemitones);
    };
    trackSpeedSlider.onValueChange = [this]
    {
        if (updatingControls) return;
        state.trackPlaybackSpeed = trackSpeedSlider.getValue();
        if (onTrackPlaybackSpeedChanged)
            onTrackPlaybackSpeedChanged(state.selectedTrackIndex, state.trackPlaybackSpeed);
    };
    resetTrackTransformButton.onClick = [this]
    {
        if (onResetTrackTransform) onResetTrackTransform(state.selectedTrackIndex);
    };
    for (auto* slider : { &trackVolumeSlider, &trackPanSlider,
                          &trackPitchSlider, &trackSpeedSlider })
    {
        slider->onDragStart = [this] { if (onBeginTrackEdit) onBeginTrackEdit(); };
        slider->onDragEnd = [this] { if (onEndTrackEdit) onEndTrackEdit(); };
    }
    trackBox.onChange = [this]
    {
        if (updatingControls)
            return;
        state.selectedTrackIndex = trackBox.getSelectedId() - 1;
        updateTrackDependentState();
        if (onSelectedTrackChanged)
            onSelectedTrackChanged(state.selectedTrackIndex);
    };

    trackRoleBox.onChange = [this]
    {
        if (updatingControls)
            return;
        state.selectedTrackRole = sanitiseTrackRole(trackRoleBox.getSelectedId() - 1);
        if (onTrackRoleChanged)
            onTrackRoleChanged(state.selectedTrackIndex, state.selectedTrackRole);
    };

    presetBox.onChange = [this]
    {
        if (updatingControls)
            return;
        const auto preset = SimpleMixProcessor::makePresetSettings(
            presetForId(presetBox.getSelectedId()));
        mixEnabledToggle.setToggleState(preset.enabled,
                                        juce::dontSendNotification);
        brightnessSlider.setValue(preset.brightness * 100.0,
                                  juce::dontSendNotification);
        ambienceSlider.setValue(preset.ambience * 100.0,
                                juce::dontSendNotification);
        stabilitySlider.setValue(preset.stability * 100.0,
                                 juce::dontSendNotification);
    };

    correctedPreviewToggle.onClick = [this]
    {
        if (updatingControls)
            return;
        state.pitchCorrection.auditionCorrected = correctedPreviewToggle.getToggleState();
        updatePitchStatus();
        if (onPitchComparisonChanged)
            onPitchComparisonChanged(state.selectedTrackIndex,
                                     state.pitchCorrection.auditionCorrected);
    };

    naturalPitchButton.onClick = [this]
    {
        state.pitchCorrection = collectPitchSettings();
        // The engine turns this on only after PitchNet has really accepted the
        // correction parameters. Keep the panel honest while loading/failing.
        state.pitchCorrection.enabled = false;
        if (onNaturalPitchCorrection)
            onNaturalPitchCorrection(state.selectedTrackIndex, state.pitchCorrection);
    };
    pitchDetailsButton.onClick = [this]
    {
        if (onOpenPitchDetails)
            onOpenPitchDetails(state.selectedTrackIndex);
    };

    applyMixButton.onClick = [this]
    {
        state.simpleMix = collectMixSettings();
        state.noiseReduction = collectNoiseSettings();
        if (onMixSettingsChanged)
            onMixSettingsChanged(state.selectedTrackIndex, state.simpleMix);
        if (onNoiseReductionChanged)
            onNoiseReductionChanged(state.selectedTrackIndex, state.noiseReduction);
    };
    vst3BrowserButton.onClick = [this]
    {
        if (onOpenVst3Browser)
            onOpenVst3Browser(state.selectedTrackIndex);
    };
    autoMixButton.onClick = [this]
    {
        if (autoMixBusy)
            return;
        setAutoMixStatus(uiText(u8"各トラックを解析しています…"), true);
        if (onRunAutoMix)
            onRunAutoMix();
        else
            setAutoMixStatus(uiText(u8"お任せMIXを開始できませんでした"), false);
    };
    applySpecialFxButton.onClick = [this]
    {
        if (onApplySpecialFx)
            onApplySpecialFx(
                state.selectedTrackIndex,
                specialFxForId(specialFxTypeBox.getSelectedId()),
                static_cast<float>(specialFxAmountSlider.getValue() / 100.0),
                static_cast<float>(specialFxWetSlider.getValue() / 100.0),
                specialFxFadeSlider.getValue() / 1000.0);
    };
    clearSpecialFxButton.onClick = [this]
    {
        if (onClearSpecialFx)
            onClearSpecialFx(state.selectedTrackIndex);
    };
    referenceMixButton.onClick = [this]
    {
        if (referenceMixBusy)
            return;
        setReferenceMixStatus(uiText(u8"参考曲を選んでください…"), true);
        if (onRunReferenceMix)
            onRunReferenceMix(state.selectedTrackIndex);
        else
            setReferenceMixStatus(uiText(u8"参考曲解析を開始できませんでした"),
                                  false);
    };

    rhythmButton.onClick = [this]
    {
        state.rhythmStrength = static_cast<float>(rhythmSlider.getValue() / 100.0);
        if (onRhythmCorrection)
            onRhythmCorrection(state.selectedTrackIndex, state.rhythmStrength);
    };
    bpmEstimateButton.onClick = [this]
    {
        if (onBpmEstimate)
            onBpmEstimate(state.selectedTrackIndex);
    };
    addTempoPointButton.onClick = [this]
    {
        state.tempoPoint = collectTempoPoint();
        if (onAddTempoPoint)
            onAddTempoPoint(state.tempoPoint);
    };
    harmonyButton.onClick = [this]
    {
        state.harmonySemitones = harmonyBox.getSelectedId() - 13;
        if (onCreateHarmony)
            onCreateHarmony(state.selectedTrackIndex, state.harmonySemitones);
    };

    applyMasteringButton.onClick = [this]
    {
        state.mastering = collectMasteringSettings();
        if (onMasteringSettingsChanged)
            onMasteringSettingsChanged(state.mastering);
    };
    closeButton.onClick = [this]
    {
        if (onClose)
            onClose();
    };
}

void StudioPanelComponent::setState(const StudioPanelState& newState)
{
    state = newState;
    updateControlsFromState();
}

StudioPanelState StudioPanelComponent::getState() const
{
    auto result = state;
    result.selectedTrackIndex = trackBox.getSelectedId() - 1;
    result.trackVolume = static_cast<float>(trackVolumeSlider.getValue() / 100.0);
    result.trackPan = static_cast<float>(trackPanSlider.getValue() / 100.0);
    result.trackPitchSemitones = trackPitchSlider.getValue();
    result.trackPlaybackSpeed = trackSpeedSlider.getValue();
    result.selectedTrackRole = sanitiseTrackRole(trackRoleBox.getSelectedId() - 1);
    result.pitchCorrection = collectPitchSettings();
    result.pitchCorrection.enabled = state.pitchCorrection.enabled;
    result.simpleMix = collectMixSettings();
    result.noiseReduction = collectNoiseSettings();
    result.mastering = collectMasteringSettings();
    result.rhythmStrength = static_cast<float>(rhythmSlider.getValue() / 100.0);
    result.tempoPoint = collectTempoPoint();
    result.harmonySemitones = harmonyBox.getSelectedId() - 13;
    return result;
}

void StudioPanelComponent::setAutoMixStatus(const juce::String& message, bool isBusy)
{
    autoMixBusy = isBusy;
    autoMixStatusLabel.setText(message, juce::dontSendNotification);
    autoMixStatusLabel.setColour(juce::Label::textColourId,
                                 isBusy ? UiTheme::accent : UiTheme::textSecondary);
    updateTrackDependentState();
}

void StudioPanelComponent::setReferenceMixStatus(const juce::String& message,
                                                  bool isBusy)
{
    referenceMixBusy = isBusy;
    referenceMixStatusLabel.setText(message, juce::dontSendNotification);
    referenceMixStatusLabel.setColour(
        juce::Label::textColourId,
        isBusy ? UiTheme::accent : UiTheme::textSecondary);
    updateTrackDependentState();
}

void StudioPanelComponent::setMasteringMeters(float loudnessLufs,
                                               float outputPeak,
                                               float gainReductionDb)
{
    if (!std::isfinite(loudnessLufs) || loudnessLufs <= -99.0f)
    {
        masteringMeterLabel.setText(
            uiText(u8"再生すると音量の目安を表示します"),
            juce::dontSendNotification);
        return;
    }

    const auto peakDb = juce::Decibels::gainToDecibels(
        juce::jmax(0.0f, outputPeak), -100.0f);
    masteringMeterLabel.setText(
        uiText(u8"現在: ") + juce::String(loudnessLufs, 1) + " LUFS  |  "
            + uiText(u8"ピーク: ") + juce::String(peakDb, 1) + " dBFS  |  "
            + uiText(u8"抑制: ")
            + juce::String(juce::jmax(0.0f, -gainReductionDb), 1) + " dB",
        juce::dontSendNotification);
}

void StudioPanelComponent::updateControlsFromState()
{
    const juce::ScopedValueSetter<bool> guard(updatingControls, true);
    trackVolumeSlider.setValue(state.trackVolume * 100.0, juce::dontSendNotification);
    trackPanSlider.setValue(state.trackPan * 100.0, juce::dontSendNotification);
    trackPitchSlider.setValue(state.trackPitchSemitones, juce::dontSendNotification);
    trackSpeedSlider.setValue(state.trackPlaybackSpeed, juce::dontSendNotification);
    trackBox.clear(juce::dontSendNotification);
    for (int index = 0; index < state.trackNames.size(); ++index)
        trackBox.addItem(state.trackNames[index], index + 1);
    if (juce::isPositiveAndBelow(state.selectedTrackIndex, state.trackNames.size()))
        trackBox.setSelectedId(state.selectedTrackIndex + 1, juce::dontSendNotification);
    else
        trackBox.setSelectedId(0, juce::dontSendNotification);
    trackRoleBox.setSelectedId(static_cast<int>(sanitiseTrackRole(
                                   static_cast<int>(state.selectedTrackRole))) + 1,
                               juce::dontSendNotification);

    keyBox.setSelectedId(juce::jlimit(0, 11, state.pitchCorrection.key) + 1,
                         juce::dontSendNotification);
    scaleBox.setSelectedId(idForScale(state.pitchCorrection.scale),
                           juce::dontSendNotification);
    pitchStrengthSlider.setValue(state.pitchCorrection.strength * 100.0,
                                 juce::dontSendNotification);
    correctedPreviewToggle.setToggleState(state.pitchCorrection.auditionCorrected,
                                           juce::dontSendNotification);

    mixEnabledToggle.setToggleState(state.simpleMix.enabled, juce::dontSendNotification);
    presetBox.setSelectedId(static_cast<int>(state.simpleMix.preset) + 1,
                            juce::dontSendNotification);
    brightnessSlider.setValue(state.simpleMix.brightness * 100.0,
                              juce::dontSendNotification);
    ambienceSlider.setValue(state.simpleMix.ambience * 100.0,
                            juce::dontSendNotification);
    stabilitySlider.setValue(state.simpleMix.stability * 100.0,
                             juce::dontSendNotification);
    noiseEnabledToggle.setToggleState(state.noiseReduction.enabled,
                                      juce::dontSendNotification);
    noiseAmountSlider.setValue(state.noiseReduction.amount * 100.0,
                               juce::dontSendNotification);
    deEssSlider.setValue(state.noiseReduction.deEssAmount * 100.0,
                         juce::dontSendNotification);
    specialFxRangeLabel.setText(
        state.hasSpecialFxRange
            ? uiText(u8"適用範囲: ")
                + formatPosition(state.specialFxRangeStartSeconds) + " - "
                + formatPosition(state.specialFxRangeEndSeconds) + "  |  "
                + uiText(u8"設定済み ") + juce::String(state.specialFxRegionCount)
                + uiText(u8"箇所")
            : uiText(u8"下のタイムラインでAとBを設定してください"),
        juce::dontSendNotification);

    rhythmSlider.setValue(state.rhythmStrength * 100.0, juce::dontSendNotification);
    tempoPositionLabel.setText(uiText(u8"追加位置: ")
                                   + formatPosition(state.tempoPoint.timeSeconds),
                               juce::dontSendNotification);
    tempoBpmSlider.setValue(state.tempoPoint.bpm, juce::dontSendNotification);
    const auto signature = juce::String(state.tempoPoint.numerator) + "/"
                           + juce::String(state.tempoPoint.denominator);
    int signatureId = 1;
    if (signature == "3/4") signatureId = 2;
    else if (signature == "6/8") signatureId = 3;
    else if (signature == "2/4") signatureId = 4;
    else if (signature == "5/4") signatureId = 5;
    else if (signature == "7/8") signatureId = 6;
    timeSignatureBox.setSelectedId(signatureId, juce::dontSendNotification);
    const int harmony = state.harmonySemitones == 0 ? 3
                                                    : juce::jlimit(-12, 12, state.harmonySemitones);
    harmonyBox.setSelectedId(harmony + 13, juce::dontSendNotification);

    masteringEnabledToggle.setToggleState(state.mastering.enabled,
                                           juce::dontSendNotification);
    limiterEnabledToggle.setToggleState(state.mastering.limiterEnabled,
                                         juce::dontSendNotification);
    ceilingSlider.setValue(state.mastering.ceilingDb, juce::dontSendNotification);
    int loudnessId = 2;
    if (state.mastering.targetLufs <= -15.0f) loudnessId = 1;
    else if (state.mastering.targetLufs >= -10.0f) loudnessId = 4;
    else if (state.mastering.targetLufs >= -13.0f) loudnessId = 3;
    loudnessBox.setSelectedId(loudnessId, juce::dontSendNotification);
    masteredPreviewToggle.setToggleState(state.mastering.auditionProcessed,
                                          juce::dontSendNotification);

    updateTrackDependentState();
    updatePitchStatus();
}

void StudioPanelComponent::updateTrackDependentState()
{
    const bool hasTrack = juce::isPositiveAndBelow(trackBox.getSelectedId() - 1,
                                                    state.trackNames.size());
    naturalPitchButton.setEnabled(hasTrack);
    trackVolumeSlider.setEnabled(hasTrack);
    trackPanSlider.setEnabled(hasTrack);
    trackPitchSlider.setEnabled(hasTrack);
    trackSpeedSlider.setEnabled(hasTrack);
    resetTrackTransformButton.setEnabled(hasTrack &&
        (std::abs(state.trackPitchSemitones) > 0.001 || std::abs(state.trackPlaybackSpeed - 1.0) > 0.001));
    pitchDetailsButton.setEnabled(hasTrack);
    correctedPreviewToggle.setEnabled(hasTrack
                                      && state.pitchCorrectionApplied);
    applyMixButton.setEnabled(hasTrack);
    vst3BrowserButton.setEnabled(hasTrack);
    trackRoleBox.setEnabled(hasTrack);
    autoMixButton.setEnabled(!state.trackNames.isEmpty() && !autoMixBusy);
    applySpecialFxButton.setEnabled(hasTrack && state.hasSpecialFxRange);
    clearSpecialFxButton.setEnabled(hasTrack && state.specialFxRegionCount > 0);
    referenceMixButton.setEnabled(!state.trackNames.isEmpty()
                                  && !referenceMixBusy);
    rhythmButton.setEnabled(hasTrack);
    bpmEstimateButton.setEnabled(hasTrack);
    harmonyButton.setEnabled(hasTrack);
}

void StudioPanelComponent::updatePitchStatus()
{
    juce::String message;
    juce::Colour colour = UiTheme::textSecondary;
    if (!state.pitchNetInstalled)
    {
        message = uiText(u8"PitchNetが見つかりません。ボタンを押すと導入案内を開きます。");
        colour = UiTheme::warning;
    }
    else if (state.pitchCorrectionApplied)
    {
        message = state.pitchCorrection.auditionCorrected
            ? uiText(u8"補正ON・補正後の音を再生中")
            : uiText(u8"補正ON・比較用に原音を再生中");
        colour = UiTheme::success;
    }
    else
    {
        message = uiText(u8"設定を選び、「自然に補正」を押してください。");
    }
    pitchStatusLabel.setText(message, juce::dontSendNotification);
    pitchStatusLabel.setColour(juce::Label::textColourId, colour);
}

PitchCorrectionSettings StudioPanelComponent::collectPitchSettings() const
{
    auto result = state.pitchCorrection;
    result.key = juce::jlimit(0, 11, keyBox.getSelectedId() - 1);
    result.scale = scaleForId(scaleBox.getSelectedId());
    result.strength = static_cast<float>(pitchStrengthSlider.getValue() / 100.0);
    result.auditionCorrected = correctedPreviewToggle.getToggleState();
    return result;
}

SimpleMixSettings StudioPanelComponent::collectMixSettings() const
{
    SimpleMixSettings result;
    result.enabled = mixEnabledToggle.getToggleState();
    result.preset = presetForId(presetBox.getSelectedId());
    result.brightness = static_cast<float>(brightnessSlider.getValue() / 100.0);
    result.ambience = static_cast<float>(ambienceSlider.getValue() / 100.0);
    result.stability = static_cast<float>(stabilitySlider.getValue() / 100.0);
    return result;
}

NoiseReductionSettings StudioPanelComponent::collectNoiseSettings() const
{
    NoiseReductionSettings result;
    result.enabled = noiseEnabledToggle.getToggleState();
    result.amount = static_cast<float>(noiseAmountSlider.getValue() / 100.0);
    result.deEssAmount = static_cast<float>(deEssSlider.getValue() / 100.0);
    return result;
}

MasteringSettings StudioPanelComponent::collectMasteringSettings() const
{
    MasteringSettings result;
    result.enabled = masteringEnabledToggle.getToggleState();
    result.limiterEnabled = limiterEnabledToggle.getToggleState();
    result.ceilingDb = static_cast<float>(ceilingSlider.getValue());
    switch (loudnessBox.getSelectedId())
    {
        case 1: result.targetLufs = -16.0f; break;
        case 3: result.targetLufs = -12.0f; break;
        case 4: result.targetLufs = -9.0f; break;
        default: result.targetLufs = -14.0f; break;
    }
    result.auditionProcessed = masteredPreviewToggle.getToggleState();
    return result;
}

TempoPoint StudioPanelComponent::collectTempoPoint() const
{
    auto result = state.tempoPoint;
    result.bpm = tempoBpmSlider.getValue();
    switch (timeSignatureBox.getSelectedId())
    {
        case 2: result.numerator = 3; result.denominator = 4; break;
        case 3: result.numerator = 6; result.denominator = 8; break;
        case 4: result.numerator = 2; result.denominator = 4; break;
        case 5: result.numerator = 5; result.denominator = 4; break;
        case 6: result.numerator = 7; result.denominator = 8; break;
        default: result.numerator = 4; result.denominator = 4; break;
    }
    return result;
}

namespace
{
// A single vertical flow keeps every setting reachable in a narrow dock.
struct StudioPageLayout
{
    explicit StudioPageLayout(juce::Component& component) : page(component) {}

    void add(juce::Component& component, int height, int after = 8)
    {
        component.setBounds(14, y, juce::jmax(0, page.getWidth() - 28), height);
        y += height + after;
    }

    void setting(juce::Label& label, juce::Component& control)
    {
        add(label, 26, 0);
        add(control, 44, 12);
    }

    void finish() { page.setSize(page.getWidth(), y + 12); }
    juce::Component& page;
    int y = 14;
};
}

void StudioPanelComponent::paint(juce::Graphics& g)
{
    g.fillAll(UiTheme::panelBackground);
    g.setColour(UiTheme::border);
    g.drawVerticalLine(0, 0.0f, static_cast<float>(getHeight()));
    g.drawHorizontalLine(218, 0.0f, static_cast<float>(getWidth()));
}

void StudioPanelComponent::showSelectedPage()
{
    juce::Component* page = &basicPage;
    switch (categoryBox.getSelectedId())
    {
        case 2: page = &mixPage; break;
        case 3: page = &pitchPage; break;
        case 4: page = &specialFxPage; break;
        case 5: page = &compositionPage; break;
        case 6: page = &masteringPage; break;
        default: break;
    }
    pageViewport.setViewedComponent(page, false);
    pageViewport.setViewPosition(0, 0);
    resized();
}

void StudioPanelComponent::resized()
{
    auto area = getLocalBounds().reduced(16, 12);
    auto header = area.removeFromTop(44);
    closeButton.setBounds(header.removeFromRight(78));
    header.removeFromRight(8);
    titleLabel.setBounds(header);
    area.removeFromTop(10);
    trackLabel.setBounds(area.removeFromTop(26));
    trackBox.setBounds(area.removeFromTop(44));
    area.removeFromTop(10);
    categoryLabel.setBounds(area.removeFromTop(26));
    categoryBox.setBounds(area.removeFromTop(44));
    area.removeFromTop(10);
    pageViewport.setBounds(area.withHeight(juce::jmax(0, area.getHeight())));

    const int pageWidth = juce::jmax(0, area.getWidth() - pageViewport.getScrollBarThickness());
    for (auto* page : { &basicPage, &pitchPage, &mixPage, &specialFxPage,
                        &compositionPage, &masteringPage })
        page->setSize(pageWidth, page->getHeight());
    resizedBasicPage();
    resizedPitchPage();
    resizedMixPage();
    resizedSpecialFxPage();
    resizedCompositionPage();
    resizedMasteringPage();
}

void StudioPanelComponent::resizedBasicPage()
{
    StudioPageLayout layout(basicPage);
    layout.add(basicHeading, 52);
    layout.setting(trackVolumeLabel, trackVolumeSlider);
    layout.setting(trackPanLabel, trackPanSlider);
    layout.setting(trackPitchLabel, trackPitchSlider);
    layout.setting(trackSpeedLabel, trackSpeedSlider);
    layout.add(resetTrackTransformButton, 44, 14);
    layout.add(basicHelpLabel, 96);
    layout.finish();
}

void StudioPanelComponent::resizedPitchPage()
{
    StudioPageLayout layout(pitchPage);
    layout.add(pitchHeading, 52);
    layout.add(pitchStatusLabel, 80, 12);
    layout.setting(keyLabel, keyBox);
    layout.setting(scaleLabel, scaleBox);
    layout.setting(pitchStrengthLabel, pitchStrengthSlider);
    layout.add(correctedPreviewToggle, 52, 14);
    layout.add(naturalPitchButton, 48);
    layout.add(pitchDetailsButton, 44);
    layout.finish();
}

void StudioPanelComponent::resizedMixPage()
{
    StudioPageLayout layout(mixPage);
    layout.add(mixHeading, 52);
    layout.setting(trackRoleLabel, trackRoleBox);
    layout.add(mixEnabledToggle, 44);
    layout.setting(presetLabel, presetBox);
    layout.setting(brightnessLabel, brightnessSlider);
    layout.setting(ambienceLabel, ambienceSlider);
    layout.setting(stabilityLabel, stabilitySlider);
    layout.add(noiseEnabledToggle, 44);
    layout.setting(noiseAmountLabel, noiseAmountSlider);
    layout.setting(deEssLabel, deEssSlider);
    layout.add(applyMixButton, 48);
    layout.add(vst3BrowserButton, 44, 16);
    layout.add(autoMixButton, 48);
    layout.add(autoMixStatusLabel, 100);
    layout.finish();
}

void StudioPanelComponent::resizedSpecialFxPage()
{
    StudioPageLayout layout(specialFxPage);
    layout.add(specialFxHeading, 58);
    layout.add(specialFxRangeLabel, 64, 14);
    layout.setting(specialFxTypeLabel, specialFxTypeBox);
    layout.setting(specialFxAmountLabel, specialFxAmountSlider);
    layout.setting(specialFxWetLabel, specialFxWetSlider);
    layout.setting(specialFxFadeLabel, specialFxFadeSlider);
    layout.add(applySpecialFxButton, 48);
    layout.add(clearSpecialFxButton, 44, 24);
    layout.add(referenceMixHeading, 52);
    layout.add(referenceMixHelpLabel, 110);
    layout.add(referenceMixButton, 48);
    layout.add(referenceMixStatusLabel, 112);
    layout.finish();
}

void StudioPanelComponent::resizedCompositionPage()
{
    StudioPageLayout layout(compositionPage);
    layout.add(compositionHeading, 52);
    layout.setting(rhythmLabel, rhythmSlider);
    layout.add(rhythmButton, 48);
    layout.add(bpmEstimateButton, 44, 20);
    layout.add(tempoPositionLabel, 32);
    layout.setting(tempoBpmLabel, tempoBpmSlider);
    layout.setting(timeSignatureLabel, timeSignatureBox);
    layout.add(addTempoPointButton, 48, 24);
    layout.setting(harmonyLabel, harmonyBox);
    layout.add(harmonyButton, 48);
    layout.finish();
}

void StudioPanelComponent::resizedMasteringPage()
{
    StudioPageLayout layout(masteringPage);
    layout.add(masteringHeading, 58);
    layout.add(masteringEnabledToggle, 52);
    layout.add(limiterEnabledToggle, 52);
    layout.setting(ceilingLabel, ceilingSlider);
    layout.setting(loudnessLabel, loudnessBox);
    layout.add(masteredPreviewToggle, 58);
    layout.add(masteringMeterLabel, 100);
    layout.add(masteringHelpLabel, 108, 16);
    layout.add(applyMasteringButton, 48);
    layout.finish();
}
