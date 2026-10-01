#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "../Source/AssistantController.h"
#include "../Source/AssistantBridge.h"
#include <iostream>

// Exercises the shipping bridge/controller against real audio data without a device.
int runAssistantIntegrationHost(const juce::File& directory)
{
    if (!directory.createDirectory()) return 2;
    AudioEngine engine;
    engine.prepare(256, 8000.0);
    TrackData track;
    track.id = "e2e-vocal";
    track.name = juce::String::fromUTF8(u8"テスト用ボーカル");
    track.role = TrackRole::mainVocal;
    track.volume = 0.5f;
    AudioClip clip;
    clip.id = "e2e-clip";
    clip.name = "test-tone";
    clip.sampleRate = 8000;
    clip.duration = 2.0;
    clip.buffer = std::make_shared<juce::AudioBuffer<float>>(1, 16000);
    for (int i = 0; i < 16000; ++i)
        clip.buffer->setSample(0, i, 0.2f * std::sin(static_cast<float>(i) * 0.1727876f));
    track.clips.push_back(clip);
    engine.replaceTracks({track});
    engine.markClean();
    const auto referenceFile = directory.getChildFile("reference.wav");
    {
        auto stream = referenceFile.createOutputStream();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream.get(), 8000.0, 1, 16, {}, 0));
        if (writer == nullptr) return 3;
        stream.release();
        if (!writer->writeFromAudioSampleBuffer(*clip.buffer, 0, 16000)) return 4;
    }
    int selection = 0;
    AssistantController controller(engine, [&selection] { return selection; },
        [&selection](int index) { selection = index; }, directory.getChildFile("preferences.json"));
    controller.preparePreview(256, 8000.0);
    AssistantBridge bridge([&controller](const juce::var& request) { return controller.handleRequest(request); });
    bridge.start();
    juce::Timer::callAfterDelay(180000, [] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
    std::cout << "ASSISTANT_HOST_READY" << std::endl;
    juce::MessageManager::getInstance()->runDispatchLoop();
    bridge.stop();
    controller.releasePreview();
    engine.release();
    return 0;
}
