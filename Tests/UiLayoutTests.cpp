#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "../Source/HeaderComponent.h"
#include "../Source/TransportBarComponent.h"
#include "../Source/TrackAreaComponent.h"
#include "../Source/StudioPanelComponent.h"
#include "../Source/UiLookAndFeel.h"
#include "../Source/WorkspaceLayout.h"
#include "../Source/ComparisonBarComponent.h"

#include <cmath>
#include <iostream>

namespace
{
bool check(bool condition, const juce::String& message)
{
    if (!condition) std::cerr << "FAILED UI: " << message << std::endl;
    return condition;
}

juce::String describe(juce::Component& component)
{
    if (auto* button = dynamic_cast<juce::Button*>(&component)) return button->getButtonText();
    if (auto* label = dynamic_cast<juce::Label*>(&component)) return label->getText();
    return component.getName().isNotEmpty() ? component.getName() : juce::String("control");
}

// Only direct siblings are compared: a slider's text editor and a viewport's
// scrolled contents intentionally live inside their parent control.
bool checkSiblings(juce::Component& parent, const juce::String& context)
{
    bool ok = true;
    const auto children = parent.getChildren();
    for (int i = 0; i < children.size(); ++i)
    {
        auto* child = children[i];
        if (!child->isVisible()) continue;
        ok &= check(!child->getBounds().isEmpty()
                    && parent.getLocalBounds().contains(child->getBounds()),
                    context + ": outside bounds: " + describe(*child));
        if (dynamic_cast<juce::Button*>(child) != nullptr
            || dynamic_cast<juce::ComboBox*>(child) != nullptr)
            ok &= check(child->getHeight() >= 40, context + ": small button: " + describe(*child));
        for (int j = i + 1; j < children.size(); ++j)
            if (children[j]->isVisible())
                ok &= check(!child->getBounds().intersects(children[j]->getBounds()),
                            context + ": overlap: " + describe(*child) + " / " + describe(*children[j]));
    }
    return ok;
}

template <typename T>
T* directChild(juce::Component& parent, const std::function<bool(T&)>& predicate)
{
    for (auto* child : parent.getChildren())
        if (auto* typed = dynamic_cast<T*>(child); typed != nullptr && predicate(*typed)) return typed;
    return nullptr;
}

std::vector<TrackData> fixtures()
{
    std::vector<TrackData> tracks;
    const char* names[] = { u8"メインボーカル", u8"ハモリ", u8"伴奏" };
    const juce::Colour colours[] = { UiTheme::accent, UiTheme::success, UiTheme::warning };
    for (int i = 0; i < 3; ++i)
    {
        TrackData track;
        track.id = "ui-track-" + juce::String(i);
        track.name = juce::String::fromUTF8(names[i]);
        track.color = colours[i];
        track.volume = 0.8f;
        AudioClip clip;
        clip.id = "ui-clip-" + juce::String(i);
        clip.name = track.name + ".wav";
        clip.sampleRate = 8000;
        clip.duration = 12.0;
        clip.startTime = i * 0.7;
        clip.buffer = std::make_shared<juce::AudioBuffer<float>>(1, 96000);
        for (int sample = 0; sample < clip.buffer->getNumSamples(); ++sample)
        {
            const double t = static_cast<double>(sample) / clip.sampleRate;
            const float value = static_cast<float>(0.5 * std::sin(t * (220 + i * 110) * juce::MathConstants<double>::twoPi)
                                 * (0.55 + 0.45 * std::sin(t * 7.1)));
            clip.buffer->setSample(0, sample, value);
        }
        track.clips.push_back(std::move(clip));
        tracks.push_back(std::move(track));
    }
    return tracks;
}

class UiShell : public juce::Component
{
public:
    ModernLookAndFeel theme;
    AudioEngine engine;
    HeaderComponent header;
    ComparisonBarComponent comparison;
    TransportBarComponent transport;
    TrackAreaComponent tracks;
    StudioPanelComponent studio;

    UiShell()
    {
        setLookAndFeel(&theme);
        engine.replaceTracks(fixtures());
        engine.setZoomLevel(45.0);
        addAndMakeVisible(header);
        addAndMakeVisible(comparison);
        addAndMakeVisible(transport);
        addAndMakeVisible(transport.getEditingToolbar());
        addAndMakeVisible(tracks);
        addAndMakeVisible(studio);
        header.setAudioEngine(&engine);
        header.setProjectDisplayName(juce::String::fromUTF8(u8"歌ってみた / UI確認"), false);
        transport.setAudioEngine(&engine);
        transport.updateTimeDisplay(18.240, 86.0);
        tracks.setAudioEngine(&engine);
        tracks.syncFromEngine();
        tracks.setSelectedTrackIndex(0);
        studio.setDockedMode(true);
        syncStudio();
        tracks.onSelectedTrackChanged = [this](int) { syncStudio(); };
        studio.onSelectedTrackChanged = [this](int index) {
            tracks.setSelectedTrackIndex(index);
            syncStudio();
        };
        studio.onTrackVolumeChanged = [this](int index, float value) { engine.setTrackVolume(index, value); };
        studio.onTrackPanChanged = [this](int index, float value) { engine.setTrackPan(index, value); };
    }

    ~UiShell() override
    {
        tracks.onSelectedTrackChanged = nullptr;
        setLookAndFeel(nullptr);
    }

    void syncStudio()
    {
        StudioPanelState state;
        const auto snapshot = engine.getTracksSnapshot();
        for (const auto& track : snapshot) state.trackNames.add(track.name);
        state.selectedTrackIndex = tracks.getSelectedTrackIndex();
        if (juce::isPositiveAndBelow(state.selectedTrackIndex, static_cast<int>(snapshot.size())))
        {
            const auto& track = snapshot[static_cast<size_t>(state.selectedTrackIndex)];
            state.trackVolume = track.volume;
            state.trackPan = track.pan;
            state.trackPitchSemitones = track.pitchSemitones;
            state.trackPlaybackSpeed = track.playbackSpeed;
        }
        studio.setState(state);
    }

    void resized() override
    {
        const auto layout = WorkspaceLayout::calculate(getLocalBounds(),
            transport.getPreferredHeight(getWidth()), transport.getEditingToolbarHeight(getWidth()), true);
        header.setBounds(layout.header);
        transport.setBounds(layout.transport);
        comparison.setBounds(layout.comparison);
        transport.getEditingToolbar().setBounds(layout.editingToolbar);
        tracks.setBounds(layout.tracks);
        studio.setBounds(layout.studio);
    }

    void paint(juce::Graphics& g) override { g.fillAll(UiTheme::windowBackground); }
};

bool saveSnapshot(juce::Component& component, const juce::File& directory, const juce::String& name)
{
    if (directory == juce::File()) return true;
    if (!check(directory.createDirectory().wasOk(), "create snapshot directory")) return false;
    const auto file = directory.getChildFile(name + ".png");
    auto output = file.createOutputStream();
    if (!check(output != nullptr, "open snapshot " + file.getFullPathName())) return false;
    output->setPosition(0);
    output->truncate();
    juce::PNGImageFormat format;
    return check(format.writeImageToStream(component.createComponentSnapshot(component.getLocalBounds()), *output),
                 "write snapshot " + name);
}
}

bool runUiLayoutTests(const juce::File& snapshotDirectory)
{
    bool ok = true;
    UiShell shell;
    auto* compareBefore = directChild<juce::TextButton>(shell.comparison, [](auto& button) { return button.getButtonText() == juce::String::fromUTF8(u8"変更前を聴く"); });
    auto* compareAfter = directChild<juce::TextButton>(shell.comparison, [](auto& button) { return button.getButtonText() == juce::String::fromUTF8(u8"変更後を聴く"); });
    auto* compareStop = directChild<juce::TextButton>(shell.comparison, [](auto& button) { return button.getButtonText() == juce::String::fromUTF8(u8"試聴を止める"); });
    ok &= check(compareBefore && compareAfter && compareStop, "main workspace comparison controls exist");
    if (compareBefore && compareAfter && compareStop)
    {
        ok &= check(!compareBefore->isEnabled() && !compareAfter->isEnabled() && !compareStop->isEnabled(), "no comparison before an edit");
        shell.comparison.setState(juce::JSON::parse(R"({"available":true,"busy":true,"mode":"before"})"));
        ok &= check(!compareBefore->isEnabled() && !compareAfter->isEnabled() && compareStop->isEnabled(), "preparation can be cancelled in main workspace");
        shell.comparison.setState(juce::JSON::parse(R"({"available":true,"playing":true,"mode":"after"})"));
        ok &= check(compareBefore->isEnabled() && compareAfter->isEnabled() && compareStop->isEnabled()
                    && !compareBefore->getToggleState() && compareAfter->getToggleState(), "active comparison mode shown and switching allowed");
        juce::String requestedMode;
        shell.comparison.onCompare = [&requestedMode](const juce::String& mode) { requestedMode = mode; };
        compareBefore->onClick();
        ok &= check(requestedMode == "before", "before button dispatches comparison");
        compareAfter->onClick();
        ok &= check(requestedMode == "after", "after button dispatches comparison");
        compareStop->onClick();
        ok &= check(requestedMode == "stop", "stop button dispatches cancellation");
        shell.comparison.onCompare = nullptr;
        shell.comparison.setState({});
    }
    if (snapshotDirectory != juce::File())
    {
        // Let the real background waveform cache publish its result before
        // capturing images. This loop is only used by the standalone QA run.
        juce::Timer::callAfterDelay(250, [] {
            juce::MessageManager::getInstance()->stopDispatchLoop();
        });
        juce::MessageManager::getInstance()->runDispatchLoop();
    }
    const juce::Point<int> sizes[] = { { 1100, 700 }, { 1382, 900 }, { 1920, 1080 } };
    for (const auto size : sizes)
    {
        shell.setSize(size.x, size.y);
        const auto context = juce::String(size.x) + "x" + juce::String(size.y);
        ok &= checkSiblings(shell, context + " workspace");
        ok &= checkSiblings(shell.header, context + " header");
        ok &= checkSiblings(shell.comparison, context + " comparison");
        ok &= checkSiblings(shell.tracks, context + " track toolbar");
        ok &= checkSiblings(shell.transport, context + " transport");
        ok &= checkSiblings(shell.transport.getEditingToolbar(), context + " edit toolbar");
        ok &= checkSiblings(shell.studio, context + " studio header");
        auto* time = directChild<juce::Label>(shell.transport, [](auto& label) { return label.getName() == "Time"; });
        auto* stop = directChild<juce::TextButton>(shell.transport, [](auto& button) { return button.getButtonText().contains(juce::String::fromUTF8(u8"停止")); });
        auto* click = directChild<juce::TextButton>(shell.transport, [](auto& button) { return button.getButtonText() == juce::String::fromUTF8(u8"クリック"); });
        auto* master = directChild<juce::Label>(shell.transport, [](auto& label) { return label.getName() == "MasterVolume"; });
        ok &= check(time != nullptr && stop != nullptr && click != nullptr && master != nullptr, context + " transport controls exist");
        if (time && stop && click && master)
        {
            ok &= check(std::abs((time->getX() + stop->getRight()) - shell.getWidth()) <= 2,
                        context + " time and transport group centred on whole window");
            ok &= check(click->getRight() < time->getX() && master->getX() > stop->getRight(),
                        context + " click left and master volume right of transport");
        }
        ok &= saveSnapshot(shell, snapshotDirectory, "workspace-" + context);
    }

    // Every dock category must remain usable at the narrow and wide dock sizes.
    auto* category = directChild<juce::ComboBox>(shell.studio, [](auto& combo) { return combo.getNumItems() == 6; });
    auto* viewport = directChild<juce::Viewport>(shell.studio, [](auto&) { return true; });
    ok &= check(category != nullptr && viewport != nullptr, "studio category and viewport exist");
    if (category && viewport)
    {
        for (const int width : { 380, 440 })
        {
            shell.studio.setSize(width, 532);
            for (int pageId = 1; pageId <= 6; ++pageId)
            {
                category->setSelectedId(pageId, juce::sendNotificationSync);
                auto* page = viewport->getViewedComponent();
                const auto context = "studio-" + juce::String(width) + "-page-" + juce::String(pageId);
                ok &= check(page != nullptr, context + " viewed page exists");
                if (!page) continue;
                ok &= checkSiblings(*page, context);
                ok &= check(page->getHeight() > viewport->getViewHeight(), context + " scroll needed for full size controls");
                ok &= check(page->getWidth() <= viewport->getViewWidth(), context + " no horizontal clipping");
                viewport->setViewPosition(0, 0);
                ok &= saveSnapshot(shell.studio, snapshotDirectory, context + "-top");
                viewport->setViewPosition(0, page->getHeight());
                int lastControlBottom = 0;
                for (auto* child : page->getChildren())
                    if (child->isVisible()) lastControlBottom = std::max(lastControlBottom, child->getBottom());
                ok &= check(viewport->getViewPositionY() > 0
                            && viewport->getViewPositionY() + viewport->getViewHeight() >= lastControlBottom,
                            context + " bottom controls reachable by scroll");
                ok &= saveSnapshot(shell.studio, snapshotDirectory, context + "-bottom");
            }
        }
        category->setSelectedId(1, juce::sendNotificationSync);
        shell.tracks.setSelectedTrackIndex(1);
        shell.syncStudio();
        auto* page = viewport->getViewedComponent();
        auto* volume = directChild<juce::Slider>(*page, [](auto& slider) { return slider.getMinimum() == 0 && slider.getMaximum() == 100; });
        auto* pan = directChild<juce::Slider>(*page, [](auto& slider) { return slider.getMinimum() == -100 && slider.getMaximum() == 100; });
        ok &= check(volume != nullptr && pan != nullptr, "basic page volume and pan exist");
        const auto before = shell.engine.getTracksSnapshot();
        if (volume && pan)
        {
            volume->setValue(37, juce::sendNotificationSync);
            pan->setValue(-42, juce::sendNotificationSync);
            const auto after = shell.engine.getTracksSnapshot();
            ok &= check(std::abs(after[1].volume - 0.37f) < 0.001f && std::abs(after[1].pan + 0.42f) < 0.001f,
                        "basic sliders send selected track and normalized value");
            ok &= check(after[0].volume == before[0].volume && after[0].pan == before[0].pan
                        && after[2].volume == before[2].volume && after[2].pan == before[2].pan,
                        "basic slider edits leave other tracks unchanged");
        }
    }

    int notifications = 0;
    int notifiedIndex = -2;
    shell.tracks.onSelectedTrackChanged = [&](int index) { ++notifications; notifiedIndex = index; };
    shell.tracks.setSelectedTrackIndex(2);
    ok &= check(notifications == 0, "external selection setter does not echo notification");
    const auto selectedId = shell.engine.getTracksSnapshot()[2].id;
    shell.engine.removeTrack(0);
    shell.tracks.syncFromEngine();
    ok &= check(shell.tracks.getSelectedTrackIndex() == 1
                && shell.engine.getTracksSnapshot()[1].id == selectedId
                && notifications == 1 && notifiedIndex == 1,
                "deleting an earlier track preserves selected track identity and updates its index");
    shell.engine.replaceTracks({});
    shell.tracks.syncFromEngine();
    ok &= check(shell.tracks.getSelectedTrackIndex() == -1 && notifiedIndex == -1,
                "removing all tracks clears selection and notifies dock");
    shell.tracks.onSelectedTrackChanged = [&](int index) {
        notifiedIndex = index;
        shell.syncStudio();
    };
    shell.engine.replaceTracks(fixtures());
    shell.tracks.syncFromEngine();
    ok &= check(shell.tracks.getSelectedTrackIndex() == 0 && notifiedIndex == 0
                && shell.studio.getState().selectedTrackIndex == 0,
                "loading tracks after an empty project selects first track in both timeline and dock");
    shell.tracks.onSelectedTrackChanged = nullptr;
    if (ok) std::cout << "UI layout, scrolling and selected-track callback tests passed." << std::endl;
    return ok;
}
