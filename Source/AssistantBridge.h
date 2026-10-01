#pragma once

#include <JuceHeader.h>
#include <functional>
#include <memory>
#include <set>

/** Same-user, local-file control bridge. All handlers run on the message thread. */
class AssistantBridge final : private juce::Timer
{
public:
    explicit AssistantBridge(std::function<juce::var(const juce::var&)> handler);
    ~AssistantBridge() override;
    void start();
    void stop();
    juce::String getStatusText() const { return status; }

private:
    void timerCallback() override;
    bool writeManifest();
    bool writeAtomic(const juce::File&, const juce::var&);
    std::function<juce::var(const juce::var&)> handler;
    juce::File root, requests, responses;
    juce::String session, token, status { juce::String::fromUTF8("Codex接続は停止中です") };
    std::unique_ptr<juce::InterProcessLock> instanceLock;
    bool ownsLock = false;
    juce::int64 lastHeartbeat = 0;
    std::set<juce::String> completed;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AssistantBridge)
};
