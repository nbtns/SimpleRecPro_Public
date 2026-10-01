#pragma once
#include <JuceHeader.h>

/** In-workspace listening controls. Editing requests are handled in Codex chat. */
class ComparisonBarComponent final : public juce::Component
{
public:
    ComparisonBarComponent();
    void setState(const juce::var& state);
    void paint(juce::Graphics&) override;
    void resized() override;
    std::function<void(const juce::String& mode)> onCompare;

private:
    juce::Label title, status;
    juce::TextButton before, after, stop;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ComparisonBarComponent)
};
