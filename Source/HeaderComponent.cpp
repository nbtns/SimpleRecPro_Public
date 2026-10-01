#include "HeaderComponent.h"
#include "AudioEngine.h"
#include "UiTheme.h"

HeaderComponent::HeaderComponent()
{
    // Title
    titleLabel.setText("SimpleRec Pro", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(20.0f, juce::Font::bold)));
    titleLabel.setColour(juce::Label::textColourId, UiTheme::textPrimary);
    addAndMakeVisible(titleLabel);

    // Project Name
    projectLabel.setText(juce::CharPointer_UTF8(u8"新規プロジェクト"), juce::dontSendNotification);
    projectLabel.setJustificationType(juce::Justification::centredLeft);
    projectLabel.setFont(juce::Font(juce::FontOptions(16.0f)));
    projectLabel.setColour(juce::Label::textColourId, UiTheme::textSecondary);
    addAndMakeVisible(projectLabel);

    // Buttons
    openButton.setButtonText(juce::CharPointer_UTF8(u8"開く"));
    saveButton.setButtonText(juce::CharPointer_UTF8(u8"保存"));
    saveAsButton.setButtonText(juce::CharPointer_UTF8(u8"別名保存"));
    recentButton.setButtonText(juce::CharPointer_UTF8(u8"履歴"));

    openButton.onClick = [this]() { if (onOpenClicked) onOpenClicked(); };
    saveButton.onClick = [this]() { if (onSaveClicked) onSaveClicked(); };
    saveAsButton.onClick = [this]() { if (onSaveAsClicked) onSaveAsClicked(); };
    recentButton.onClick = [this]() {
        juce::PopupMenu menu;
        if (recentProjectPaths.isEmpty())
        {
            menu.addItem(1, juce::CharPointer_UTF8(u8"最近使ったプロジェクトはありません"), false, false);
        }
        else
        {
            for (int i = 0; i < recentProjectPaths.size(); ++i)
            {
                const juce::File file(recentProjectPaths[i]);
                menu.addItem(i + 1, file.getFileNameWithoutExtension());
            }
        }

        const auto safeThis = juce::Component::SafePointer<HeaderComponent>(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&recentButton),
            [safeThis](int result) {
                if (safeThis == nullptr || result <= 0) return;
                const int index = result - 1;
                if (index < safeThis->recentProjectPaths.size() && safeThis->onRecentProjectSelected)
                    safeThis->onRecentProjectSelected(juce::File(safeThis->recentProjectPaths[index]));
            });
    };
    
    undoButton.setButtonText(juce::CharPointer_UTF8(u8"戻す"));
    redoButton.setButtonText(juce::CharPointer_UTF8(u8"やり直す"));
    undoButton.setTooltip(juce::String::fromUTF8(u8"元に戻す (Ctrl+Z)"));
    redoButton.setTooltip(juce::String::fromUTF8(u8"やり直す (Ctrl+Y)"));
    
    undoButton.onClick = [this]() { if (audioEngine) audioEngine->undo(); };
    redoButton.onClick = [this]() { if (audioEngine) audioEngine->redo(); };

    // 全ての子コンポーネントがスペースキー等を横取りしないように設定
    for (auto* comp : getChildren())
    {
        comp->setWantsKeyboardFocus(false);
    }
    
    // ツールボタンは TransportBarComponent へ移動
    // BPM / Tap Tempo は TransportBarComponent へ移動
    // Zoom Slider は TransportBarComponent へ移動
    
    settingsButton.setName("Settings");
    settingsButton.setButtonText(juce::CharPointer_UTF8(u8"設定"));
    settingsButton.onClick = [this]() {
        juce::Logger::writeToLog("Settings button clicked!");
        if (onSettingsClicked) onSettingsClicked();
    };

    addAndMakeVisible(openButton);
    addAndMakeVisible(saveButton);
    addAndMakeVisible(saveAsButton);
    addAndMakeVisible(recentButton);
    addAndMakeVisible(undoButton);
    addAndMakeVisible(redoButton);
    
    exportButton.setButtonText(juce::String::fromUTF8(u8"書き出し"));
    exportButton.onClick = [this]() { if (onExportClicked) onExportClicked(); };
    addAndMakeVisible(exportButton);
    
    addAndMakeVisible(settingsButton);
    
    auto btnColor = UiTheme::controlSurface;
    openButton.setColour(juce::TextButton::buttonColourId, btnColor);
    saveButton.setColour(juce::TextButton::buttonColourId, btnColor);
    saveAsButton.setColour(juce::TextButton::buttonColourId, btnColor);
    recentButton.setColour(juce::TextButton::buttonColourId, btnColor);
    undoButton.setColour(juce::TextButton::buttonColourId, btnColor);
    redoButton.setColour(juce::TextButton::buttonColourId, btnColor);
    settingsButton.setColour(juce::TextButton::buttonColourId, btnColor);
    saveButton.setColour(juce::TextButton::buttonColourId, UiTheme::accent);

    for (auto* child : getChildren())
        child->setWantsKeyboardFocus(false);
}

HeaderComponent::~HeaderComponent()
{
}

void HeaderComponent::setAudioEngine(AudioEngine* engine)
{
    audioEngine = engine;
    syncFromEngine();
}

void HeaderComponent::setProjectDisplayName(const juce::String& name, bool isDirty)
{
    projectLabel.setText(name + (isDirty ? " *" : ""), juce::dontSendNotification);
    projectLabel.setColour(juce::Label::textColourId,
                           isDirty ? UiTheme::warning : UiTheme::textSecondary);
}

void HeaderComponent::setRecentProjects(const juce::StringArray& paths)
{
    recentProjectPaths = paths;
}

void HeaderComponent::syncFromEngine()
{
    // Master volume lives beside the transport; project state is set by Main.
}

void HeaderComponent::paint (juce::Graphics& g)
{
    g.fillAll(UiTheme::panelBackground);
    g.setColour(UiTheme::accent);
    g.fillRoundedRectangle(18.0f, 22.0f, 5.0f, 28.0f, 2.5f);
    g.setColour(UiTheme::borderSoft);
    g.fillRect(0, getHeight() - 1, getWidth(), 1);
}

void HeaderComponent::resized()
{
    auto bounds = getLocalBounds().reduced(28, 12);
    auto place = [&bounds](juce::Component& control, int width) {
        control.setBounds(bounds.removeFromRight(width).withSizeKeepingCentre(width, 44));
        bounds.removeFromRight(8);
    };
    place(settingsButton, 76);
    place(exportButton, 104);
    place(redoButton, 90);
    place(undoButton, 68);
    place(saveAsButton, 96);
    place(saveButton, 68);
    place(recentButton, 68);
    place(openButton, 68);
    bounds.removeFromRight(8);
    titleLabel.setBounds(bounds.removeFromTop(30));
    projectLabel.setBounds(bounds);
}
