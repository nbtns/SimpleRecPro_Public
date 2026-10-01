#include "ComparisonBarComponent.h"
#include "UiTheme.h"

namespace { juce::String tr(const char* text) { return juce::String::fromUTF8(text); } }

ComparisonBarComponent::ComparisonBarComponent()
{
    title.setText(tr(u8"変更前後の比較"), juce::dontSendNotification);
    title.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
    title.setColour(juce::Label::textColourId, UiTheme::textPrimary);
    status.setFont(juce::Font(juce::FontOptions(14.0f)));
    status.setColour(juce::Label::textColourId, UiTheme::textSecondary);
    addAndMakeVisible(title);
    addAndMakeVisible(status);
    auto configure = [this](juce::TextButton& button, const char* label, const char* mode)
    {
        button.setButtonText(tr(label));
        button.setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);
        button.setColour(juce::TextButton::buttonOnColourId, UiTheme::accentSoft);
        button.onClick = [this, mode] { if (onCompare) onCompare(mode); };
        addAndMakeVisible(button);
    };
    configure(before, u8"変更前を聴く", "before");
    configure(after, u8"変更後を聴く", "after");
    configure(stop, u8"試聴を止める", "stop");
    before.setTooltip(tr(u8"直前にCodexが変更した音の、変更前を試聴します。"));
    after.setTooltip(tr(u8"変更後を同じ区間で試聴します。比較しやすいよう音量を揃えます。"));
    stop.setTooltip(tr(u8"比較試聴を止めます。準備中もキャンセルできます。"));
    setState({});
}

void ComparisonBarComponent::setState(const juce::var& state)
{
    const auto busy = static_cast<bool>(state["busy"]);
    const auto playing = static_cast<bool>(state["playing"]);
    const auto available = static_cast<bool>(state["available"]);
    before.setEnabled(available && !busy);
    after.setEnabled(available && !busy);
    stop.setEnabled(busy || playing);
    const auto mode = state["mode"].toString();
    before.setToggleState((playing || busy) && mode == "before", juce::dontSendNotification);
    after.setToggleState((playing || busy) && mode == "after", juce::dontSendNotification);
    auto message = state["message"].toString();
    if (message.isEmpty()) message = tr(u8"Codexで音を調整すると、ここで聴き比べできます");
    status.setText(message, juce::dontSendNotification);
    status.setTooltip(message);
}

void ComparisonBarComponent::paint(juce::Graphics& g)
{
    g.fillAll(UiTheme::panelBackground);
    g.setColour(UiTheme::borderSoft);
    g.drawHorizontalLine(0, 0.0f, static_cast<float>(getWidth()));
}

void ComparisonBarComponent::resized()
{
    auto area = getLocalBounds().reduced(20, 8);
    title.setBounds(area.removeFromLeft(148));
    auto controls = area.removeFromRight(428);
    for (auto* button : {&before, &after, &stop})
    {
        button->setBounds(controls.removeFromLeft(136));
        controls.removeFromLeft(10);
    }
    area.removeFromRight(16);
    status.setBounds(area);
}
