#include "ExportDialogComponent.h"

#include "UiTheme.h"

namespace
{
juce::String uiText(const char* text)
{
    return juce::String::fromUTF8(text);
}

void styleLabel(juce::Label& label, float size = 13.0f, bool bold = false)
{
    label.setColour(juce::Label::textColourId, UiTheme::textSecondary);
    label.setFont(juce::Font(juce::FontOptions(size,
        bold ? juce::Font::bold : juce::Font::plain)));
    label.setJustificationType(juce::Justification::centredLeft);
}

void styleCombo(juce::ComboBox& box)
{
    box.setColour(juce::ComboBox::backgroundColourId, UiTheme::controlSurface);
    box.setColour(juce::ComboBox::outlineColourId, UiTheme::border);
    box.setColour(juce::ComboBox::textColourId, UiTheme::textPrimary);
    box.setColour(juce::ComboBox::arrowColourId, UiTheme::textSecondary);
}

juce::String formatRangeTime(double seconds)
{
    const int totalSeconds = juce::jmax(0, juce::roundToInt(seconds));
    return juce::String::formatted("%02d:%02d",
                                   totalSeconds / 60,
                                   totalSeconds % 60);
}
}

ExportDialogComponent::ExportDialogComponent(
    const juce::StringArray& trackNames,
    const ExportSettings& initialSettings)
    : availableTrackNames(trackNames),
      settings(initialSettings)
{
    setSize(520, 570);

    titleLabel.setText(uiText(u8"書き出し"), juce::dontSendNotification);
    titleLabel.setColour(juce::Label::textColourId, UiTheme::textPrimary);
    titleLabel.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    descriptionLabel.setText(
        uiText(u8"保存する音声の形式と範囲を選んでください。"),
        juce::dontSendNotification);
    styleLabel(descriptionLabel, 12.0f);

    formatLabel.setText(uiText(u8"形式"), juce::dontSendNotification);
    channelsLabel.setText(uiText(u8"チャンネル"), juce::dontSendNotification);
    qualityLabel.setText(uiText(u8"音質"), juce::dontSendNotification);
    sampleRateLabel.setText(uiText(u8"サンプルレート"), juce::dontSendNotification);
    targetLabel.setText(uiText(u8"書き出す範囲"), juce::dontSendNotification);
    trackLabel.setText(uiText(u8"トラック"), juce::dontSendNotification);
    for (auto* label : { &formatLabel, &channelsLabel, &qualityLabel,
                         &sampleRateLabel, &targetLabel, &trackLabel })
        styleLabel(*label, 12.0f, true);

    rangeLabel.setColour(juce::Label::textColourId, UiTheme::textSecondary);
    rangeLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    rangeLabel.setJustificationType(juce::Justification::centredLeft);

    for (auto* box : { &formatBox, &channelsBox, &qualityBox,
                       &sampleRateBox, &targetBox, &trackBox })
        styleCombo(*box);

    progressBar.setColour(juce::ProgressBar::backgroundColourId,
                          UiTheme::controlSurface);
    progressBar.setColour(juce::ProgressBar::foregroundColourId,
                          UiTheme::accent);
    progressBar.setPercentageDisplay(true);
    progressLabel.setText(uiText(u8"準備できています"), juce::dontSendNotification);
    progressLabel.setColour(juce::Label::textColourId, UiTheme::textSecondary);
    progressLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    progressLabel.setJustificationType(juce::Justification::centredLeft);

    startButton.setButtonText(uiText(u8"書き出しを開始"));
    cancelButton.setButtonText(uiText(u8"キャンセル"));
    closeButton.setButtonText(uiText(u8"閉じる"));
    startButton.setColour(juce::TextButton::buttonColourId, UiTheme::accent);
    cancelButton.setColour(juce::TextButton::buttonColourId, UiTheme::danger);
    closeButton.setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);

    auto addControl = [this](juce::Component& component)
    {
        addAndMakeVisible(component);
    };
    addControl(titleLabel);
    addControl(descriptionLabel);
    addControl(formatLabel);
    addControl(formatBox);
    addControl(channelsLabel);
    addControl(channelsBox);
    addControl(qualityLabel);
    addControl(qualityBox);
    addControl(sampleRateLabel);
    addControl(sampleRateBox);
    addControl(targetLabel);
    addControl(targetBox);
    addControl(trackLabel);
    addControl(trackBox);
    addControl(rangeLabel);
    addControl(progressBar);
    addControl(progressLabel);
    addControl(startButton);
    addControl(cancelButton);
    addControl(closeButton);

    configureComboBoxes();

    formatBox.onChange = [this]
    {
        if (!updatingControls)
            updateDependentControls();
    };
    targetBox.onChange = [this]
    {
        if (!updatingControls)
            updateDependentControls();
    };

    startButton.onClick = [this]
    {
        if (exportRunning)
            return;

        const auto chosen = collectSettings();
        if (chosen.target == ExportTarget::selectedTrack
            && !juce::isPositiveAndBelow(chosen.trackIndex,
                                         availableTrackNames.size()))
        {
            progressLabel.setText(uiText(u8"書き出すトラックを選んでください"),
                                  juce::dontSendNotification);
            progressLabel.setColour(juce::Label::textColourId, UiTheme::warning);
            return;
        }

        if (chosen.target == ExportTarget::abRange && !hasValidABRange())
        {
            progressLabel.setText(uiText(u8"先にA/B範囲を設定してください"),
                                  juce::dontSendNotification);
            progressLabel.setColour(juce::Label::textColourId, UiTheme::warning);
            return;
        }

        settings = chosen;
        progressLabel.setColour(juce::Label::textColourId, UiTheme::textSecondary);
        if (onStartExport)
            onStartExport(settings);
    };

    cancelButton.onClick = [this]
    {
        if (exportRunning && onCancelExport)
            onCancelExport();
    };
    closeButton.onClick = [this]
    {
        if (!exportRunning && onClose)
            onClose();
    };

    updateControlsFromSettings();
    setExportRunning(false);
}

void ExportDialogComponent::configureComboBoxes()
{
    formatBox.clear(juce::dontSendNotification);
    formatBox.addItem("WAV", 1);
    formatBox.addItem("MP3", 2);

    channelsBox.clear(juce::dontSendNotification);
    channelsBox.addItem(uiText(u8"モノラル"), 1);
    channelsBox.addItem(uiText(u8"ステレオ"), 2);

    sampleRateBox.clear(juce::dontSendNotification);
    sampleRateBox.addItem("44.1 kHz", 1);
    sampleRateBox.addItem("48 kHz", 2);

    targetBox.clear(juce::dontSendNotification);
    targetBox.addItem(uiText(u8"全体"), 1);
    targetBox.addItem(uiText(u8"A/B範囲"), 2);
    targetBox.addItem(uiText(u8"選択トラック"), 3);

    setTrackNames(availableTrackNames);
}

void ExportDialogComponent::setTrackNames(const juce::StringArray& newTrackNames)
{
    const juce::ScopedValueSetter<bool> guard(updatingControls, true);
    availableTrackNames = newTrackNames;
    trackBox.clear(juce::dontSendNotification);
    for (int index = 0; index < availableTrackNames.size(); ++index)
        trackBox.addItem(availableTrackNames[index], index + 1);

    if (juce::isPositiveAndBelow(settings.trackIndex, availableTrackNames.size()))
        trackBox.setSelectedId(settings.trackIndex + 1, juce::dontSendNotification);
    else if (!availableTrackNames.isEmpty())
        trackBox.setSelectedId(1, juce::dontSendNotification);

    updateDependentControls();
}

void ExportDialogComponent::setSettings(const ExportSettings& newSettings)
{
    settings = newSettings;
    updateControlsFromSettings();
}

ExportSettings ExportDialogComponent::getSettings() const
{
    return collectSettings();
}

void ExportDialogComponent::updateControlsFromSettings()
{
    const juce::ScopedValueSetter<bool> guard(updatingControls, true);
    formatBox.setSelectedId(settings.format == ExportFormat::mp3 ? 2 : 1,
                            juce::dontSendNotification);
    channelsBox.setSelectedId(settings.channels == ExportChannels::mono ? 1 : 2,
                              juce::dontSendNotification);
    sampleRateBox.setSelectedId(settings.sampleRate == 48000 ? 2 : 1,
                                juce::dontSendNotification);

    int targetId = 1;
    if (settings.target == ExportTarget::abRange)
        targetId = 2;
    else if (settings.target == ExportTarget::selectedTrack)
        targetId = 3;
    targetBox.setSelectedId(targetId, juce::dontSendNotification);

    if (juce::isPositiveAndBelow(settings.trackIndex, availableTrackNames.size()))
        trackBox.setSelectedId(settings.trackIndex + 1, juce::dontSendNotification);

    updateDependentControls();
}

void ExportDialogComponent::updateDependentControls()
{
    const int selectedWavBits = qualityBox.getSelectedId() == 2 ? 24
                                                                 : settings.wavBits;
    const juce::ScopedValueSetter<bool> guard(updatingControls, true);
    const bool mp3 = formatBox.getSelectedId() == 2;
    const int previousBits = selectedWavBits == 24 ? 24 : 16;

    qualityBox.clear(juce::dontSendNotification);
    if (mp3)
    {
        qualityBox.addItem("192 kbps", 1);
        qualityBox.setSelectedId(1, juce::dontSendNotification);
        qualityBox.setEnabled(false);
        qualityLabel.setText(uiText(u8"MP3音質"), juce::dontSendNotification);
    }
    else
    {
        qualityBox.addItem("16 bit", 1);
        qualityBox.addItem("24 bit", 2);
        qualityBox.setSelectedId(previousBits == 24 ? 2 : 1,
                                 juce::dontSendNotification);
        qualityBox.setEnabled(!exportRunning);
        qualityLabel.setText(uiText(u8"WAV音質"), juce::dontSendNotification);
    }

    const bool trackTarget = targetBox.getSelectedId() == 3;
    trackLabel.setVisible(trackTarget);
    trackBox.setVisible(trackTarget);

    juce::String rangeText;
    if (targetBox.getSelectedId() == 2)
    {
        rangeText = hasValidABRange()
            ? uiText(u8"A/B: ") + formatRangeTime(settings.rangeStartSeconds)
                + " - " + formatRangeTime(settings.rangeEndSeconds)
            : uiText(u8"A/B範囲はまだ設定されていません");
    }
    else if (targetBox.getSelectedId() == 3)
    {
        rangeText = uiText(u8"選んだトラックだけを書き出します");
    }
    else
    {
        rangeText = uiText(u8"プロジェクト全体を書き出します");
    }
    rangeLabel.setText(rangeText, juce::dontSendNotification);

    updateEnabledState();
    resized();
}

void ExportDialogComponent::updateEnabledState()
{
    for (auto* box : { &formatBox, &channelsBox, &sampleRateBox, &targetBox })
        box->setEnabled(!exportRunning);

    qualityBox.setEnabled(!exportRunning && formatBox.getSelectedId() != 2);
    trackBox.setEnabled(!exportRunning && !availableTrackNames.isEmpty());
    startButton.setEnabled(!exportRunning);
    cancelButton.setEnabled(exportRunning);
    closeButton.setEnabled(!exportRunning);
}

ExportSettings ExportDialogComponent::collectSettings() const
{
    auto result = settings;
    const bool mp3 = formatBox.getSelectedId() == 2;
    result.format = mp3 ? ExportFormat::mp3 : ExportFormat::wav;
    result.channels = channelsBox.getSelectedId() == 1
        ? ExportChannels::mono : ExportChannels::stereo;
    if (!mp3)
        result.wavBits = qualityBox.getSelectedId() == 2 ? 24 : 16;
    result.mp3Kbps = 192;
    result.sampleRate = sampleRateBox.getSelectedId() == 2 ? 48000 : 44100;

    switch (targetBox.getSelectedId())
    {
        case 2: result.target = ExportTarget::abRange; break;
        case 3: result.target = ExportTarget::selectedTrack; break;
        default: result.target = ExportTarget::fullMix; break;
    }
    result.trackIndex = trackBox.getSelectedId() - 1;
    return result;
}

bool ExportDialogComponent::hasValidABRange() const noexcept
{
    return settings.rangeEndSeconds > settings.rangeStartSeconds;
}

void ExportDialogComponent::setProgress(double progress,
                                        const juce::String& message)
{
    if (auto* manager = juce::MessageManager::getInstanceWithoutCreating();
        manager != nullptr && !manager->isThisTheMessageThread())
    {
        juce::Component::SafePointer<ExportDialogComponent> safeThis(this);
        juce::MessageManager::callAsync([safeThis, progress, message]
        {
            if (safeThis != nullptr)
                safeThis->setProgress(progress, message);
        });
        return;
    }

    progressValue = juce::jlimit(0.0, 1.0, progress);
    progressBar.repaint();
    if (message.isNotEmpty())
        progressLabel.setText(message, juce::dontSendNotification);
}

void ExportDialogComponent::setExportRunning(bool shouldBeRunning)
{
    if (auto* manager = juce::MessageManager::getInstanceWithoutCreating();
        manager != nullptr && !manager->isThisTheMessageThread())
    {
        juce::Component::SafePointer<ExportDialogComponent> safeThis(this);
        juce::MessageManager::callAsync([safeThis, shouldBeRunning]
        {
            if (safeThis != nullptr)
                safeThis->setExportRunning(shouldBeRunning);
        });
        return;
    }

    const bool starting = shouldBeRunning && !exportRunning;
    exportRunning = shouldBeRunning;
    if (starting)
    {
        progressValue = 0.0;
        progressLabel.setText(uiText(u8"書き出しを準備しています…"),
                              juce::dontSendNotification);
    }
    updateEnabledState();
    progressBar.repaint();
}

void ExportDialogComponent::paint(juce::Graphics& g)
{
    g.fillAll(UiTheme::windowBackground);
    UiTheme::fillCard(g, getLocalBounds().toFloat().reduced(16.0f), 12.0f);
}

void ExportDialogComponent::resized()
{
    auto area = getLocalBounds().reduced(30, 24);
    titleLabel.setBounds(area.removeFromTop(30));
    descriptionLabel.setBounds(area.removeFromTop(28));
    area.removeFromTop(10);

    auto placeRow = [&area](juce::Label& label, juce::Component& control)
    {
        auto row = area.removeFromTop(42);
        label.setBounds(row.removeFromLeft(142));
        control.setBounds(row.reduced(0, 4));
        area.removeFromTop(4);
    };

    placeRow(formatLabel, formatBox);
    placeRow(channelsLabel, channelsBox);
    placeRow(qualityLabel, qualityBox);
    placeRow(sampleRateLabel, sampleRateBox);
    placeRow(targetLabel, targetBox);

    if (trackBox.isVisible())
        placeRow(trackLabel, trackBox);
    else
    {
        trackLabel.setBounds({});
        trackBox.setBounds({});
    }

    rangeLabel.setBounds(area.removeFromTop(28));
    area.removeFromTop(10);
    progressBar.setBounds(area.removeFromTop(24));
    progressLabel.setBounds(area.removeFromTop(28));

    auto buttons = area.removeFromBottom(42);
    closeButton.setBounds(buttons.removeFromLeft(92));
    buttons.removeFromLeft(10);
    cancelButton.setBounds(buttons.removeFromRight(112));
    buttons.removeFromRight(10);
    startButton.setBounds(buttons.removeFromRight(154));
}
