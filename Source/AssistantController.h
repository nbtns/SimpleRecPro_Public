#pragma once

#include "AudioEngine.h"
#include <functional>
#include <memory>

/** Validated, message-thread command boundary shared by the assistant UI and IPC. */
class AssistantController
{
public:
    AssistantController(AudioEngine&,
                        std::function<int()> selectedTrack,
                        std::function<void(int)> selectTrack,
                        juce::File preferencesFile);
    ~AssistantController();

    juce::var handleRequest(const juce::var& request);
    juce::String getSummaryText() const;
    /** Lightweight message-thread state for the inline comparison controls. */
    juce::var getComparisonState() const;
    void preparePreview(int maximumBlockSize, double sampleRate);
    void releasePreview();
    /** Returns true only when the block contains assistant preview audio. */
    bool renderPreview(const juce::AudioSourceChannelInfo&);
    bool isPreviewBusy() const;
    bool isComparing() const;
    void setExternalBusyCheck(std::function<bool()>);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AssistantController)
};
