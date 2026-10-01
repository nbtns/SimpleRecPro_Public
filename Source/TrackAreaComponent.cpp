#include "TrackAreaComponent.h"
#include "UiTheme.h"

#include <cstdint>

TrackAreaComponent::TrackAreaComponent()
{
    addTrackBtn.setButtonText(juce::CharPointer_UTF8(u8"+ トラックを追加"));
    loadFileBtn.setButtonText(juce::CharPointer_UTF8(u8"音声を読み込む"));

    addAndMakeVisible(addTrackBtn);
    addAndMakeVisible(loadFileBtn);
    addAndMakeVisible(snapBtn);
    addAndMakeVisible(studioBtn);
    addChildComponent(trackNameEditor);
    
    addAndMakeVisible(horizontalScrollBar);
    addAndMakeVisible(verticalScrollBar);
    horizontalScrollBar.addListener(this);
    verticalScrollBar.addListener(this);
    
    // キーボードフォーカスを受け取る（ショートカット用）
    setWantsKeyboardFocus(true);
    
    auto btnColor = UiTheme::controlSurface;
    addTrackBtn.setColour(juce::TextButton::buttonColourId, btnColor);
    addTrackBtn.setColour(juce::TextButton::textColourOffId, UiTheme::textPrimary);
    
    loadFileBtn.setColour(juce::TextButton::buttonColourId, UiTheme::accent);
    loadFileBtn.setColour(juce::TextButton::textColourOffId, UiTheme::textPrimary);

    snapBtn.setClickingTogglesState(true);
    snapBtn.setToggleState(true, juce::dontSendNotification);
    snapBtn.setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);
    snapBtn.setColour(juce::TextButton::buttonOnColourId, UiTheme::accentSoft);
    snapBtn.setColour(juce::TextButton::textColourOffId, UiTheme::textSecondary);
    snapBtn.setColour(juce::TextButton::textColourOnId, UiTheme::accentHover);
    snapBtn.setTooltip(juce::CharPointer_UTF8(u8"拍へのスナップをON/OFF"));
    studioBtn.setColour(juce::TextButton::buttonColourId, UiTheme::accent);
    studioBtn.setColour(juce::TextButton::textColourOffId, UiTheme::textPrimary);
    studioBtn.setTooltip(juce::CharPointer_UTF8(
        u8"ピッチ補正、MIX、曲構成、マスタリングを開く"));
    
    horizontalScrollBar.setColour(juce::ScrollBar::thumbColourId, UiTheme::controlHover);
    horizontalScrollBar.setColour(juce::ScrollBar::trackColourId, UiTheme::panelBackground);
    horizontalScrollBar.setColour(juce::ScrollBar::backgroundColourId, UiTheme::panelBackground);
    verticalScrollBar.setColour(juce::ScrollBar::thumbColourId, UiTheme::controlHover);
    verticalScrollBar.setColour(juce::ScrollBar::trackColourId, UiTheme::panelBackground);
    verticalScrollBar.setColour(juce::ScrollBar::backgroundColourId, UiTheme::panelBackground);

    trackNameEditor.setFont(juce::Font(juce::FontOptions(16.0f)));
    trackNameEditor.setEditable(false, false, false);
    trackNameEditor.setJustificationType(juce::Justification::centredLeft);
    trackNameEditor.setColour(juce::Label::backgroundColourId, UiTheme::raisedSurface);
    trackNameEditor.setColour(juce::Label::textColourId, UiTheme::textPrimary);
    trackNameEditor.setColour(juce::Label::outlineColourId, UiTheme::accent);
    trackNameEditor.setColour(juce::TextEditor::backgroundColourId, UiTheme::raisedSurface);
    trackNameEditor.setColour(juce::TextEditor::textColourId, UiTheme::textPrimary);
    trackNameEditor.setColour(juce::TextEditor::outlineColourId, UiTheme::accent);
    trackNameEditor.onEditorHide = [safeThis = juce::Component::SafePointer<TrackAreaComponent>(this)]()
    {
        juce::MessageManager::callAsync([safeThis]()
        {
            if (safeThis != nullptr)
                safeThis->finishTrackNameEdit();
        });
    };
    
    // 新規トラック追加ボタン
    addTrackBtn.onClick = [this]() {
        if (audioEngine == nullptr) return;
        audioEngine->addTrack();
        syncFromEngine();
        selectTrack(static_cast<int>(cachedTracks.size()) - 1);
        resized();
        updateScrollbar();
        repaint();
    };
    
    // ファイル読み込みボタン
    loadFileBtn.onClick = [this]() {
        if (audioEngine == nullptr) return;

        fileChooser = std::make_shared<juce::FileChooser>(
            juce::CharPointer_UTF8(u8"音声ファイルを選択"),
            juce::File::getSpecialLocation(juce::File::userMusicDirectory),
            "*.wav;*.wave;*.aif;*.aiff;*.flac;*.mp3;*.ogg");

        auto chooser = fileChooser;
        auto safeThis = juce::Component::SafePointer<TrackAreaComponent>(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::canSelectMultipleItems,
            [safeThis, chooser](const juce::FileChooser& fc)
            {
                if (safeThis == nullptr) return;
                safeThis->loadAudioFiles(fc.getResults());
                safeThis->fileChooser.reset();
            });
    };

    snapBtn.onClick = [this]()
    {
        snapEnabled = snapBtn.getToggleState();
        updateSnapButtonText();
        repaint();
    };
    studioBtn.onClick = [this]()
    {
        if (onStudioClicked) onStudioClicked(selectedTrackIndex);
    };

    // 全ての子コンポーネントがスペースキー等を横取りしないように設定
    for (auto* comp : getChildren())
    {
        comp->setWantsKeyboardFocus(false);
    }
}

TrackAreaComponent::~TrackAreaComponent()
{
    waveformShutdownRequested.store(true);
    waveformGeneration.fetch_add(1);
    waveformThreadPool.removeAllJobs(true, -1);
}

void TrackAreaComponent::setSelectedTrackIndex(int trackIndex)
{
    const int nextIndex = juce::isPositiveAndBelow(trackIndex, static_cast<int>(cachedTracks.size()))
                              ? trackIndex : -1;
    const auto nextId = nextIndex >= 0 ? cachedTracks[static_cast<size_t>(nextIndex)].id
                                      : juce::String();
    if (nextIndex == selectedTrackIndex && nextId == selectedTrackId) return;
    selectedTrackIndex = nextIndex;
    selectedTrackId = nextId;
    repaint();
}

void TrackAreaComponent::selectTrack(int trackIndex)
{
    const int previousIndex = selectedTrackIndex;
    const auto previousId = selectedTrackId;
    setSelectedTrackIndex(trackIndex);
    if ((previousIndex != selectedTrackIndex || previousId != selectedTrackId)
        && onSelectedTrackChanged)
        onSelectedTrackChanged(selectedTrackIndex);
}

TrackAreaComponent::TrackHeaderLayout TrackAreaComponent::getTrackHeaderLayout(int trackIndex) const
{
    const int x = leftMargin + 12;
    const int y = getTrackStartY() + trackIndex * (trackHeight + trackGap);
    return { { x, y + 3, sidebarWidth - 30, 40 },
             { x, y + 48, 108, 40 }, { x + 116, y + 48, 108, 40 },
             { x, y + 96, 96, 40 }, { x + 104, y + 96, 54, 40 },
             { x + 166, y + 96, 58, 40 } };
}

void TrackAreaComponent::showTrackHeaderMenu(int trackIndex, juce::Rectangle<int> targetBounds)
{
    if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(cachedTracks.size()))) return;
    const auto trackId = cachedTracks[static_cast<size_t>(trackIndex)].id;
    juce::PopupMenu menu;
    menu.addItem(1, juce::CharPointer_UTF8(u8"名前を変更"));
    menu.addItem(2, juce::CharPointer_UTF8(u8"キーを変更"));
    menu.addItem(3, juce::CharPointer_UTF8(u8"再生速度を変更"));
    menu.addItem(4, juce::CharPointer_UTF8(u8"自然にピッチ補正"));
    menu.addItem(5, juce::CharPointer_UTF8(u8"ピッチ補正の詳細"));
    menu.addItem(6, juce::CharPointer_UTF8(u8"右側で調整"));
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(targetBounds)),
        [safeThis = juce::Component::SafePointer<TrackAreaComponent>(this), trackId, targetBounds](int result)
        {
            if (safeThis == nullptr || safeThis->audioEngine == nullptr) return;
            const auto found = std::find_if(safeThis->cachedTracks.begin(), safeThis->cachedTracks.end(),
                [&trackId](const TrackData& track) { return track.id == trackId; });
            if (found == safeThis->cachedTracks.end()) return;
            const int index = static_cast<int>(std::distance(safeThis->cachedTracks.begin(), found));
            switch (result)
            {
                case 1: safeThis->beginTrackNameEdit(index); break;
                case 2: safeThis->showTrackPitchMenu(index, targetBounds); break;
                case 3: safeThis->showTrackSpeedMenu(index, targetBounds); break;
                case 4: safeThis->audioEngine->applyNaturalPitchCorrection(index); break;
                case 5: safeThis->audioEngine->showPitchCorrectionDetails(index); break;
                case 6: if (safeThis->onStudioClicked) safeThis->onStudioClicked(index); break;
                default: break;
            }
        });
}

void TrackAreaComponent::syncFromEngine()
{
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    if (messageManager != nullptr && !messageManager->isThisTheMessageThread())
    {
        juce::Component::SafePointer<TrackAreaComponent> safeThis(this);
        juce::MessageManager::callAsync([safeThis]() mutable
        {
            if (safeThis != nullptr)
                safeThis->syncFromEngine();
        });
        return;
    }

    if (audioEngine == nullptr)
    {
        cachedTracks.clear();
        selectTrack(-1);
        updateWaveformCaches();
        return;
    }

    cachedTracks = audioEngine->getTracksSnapshot();
    if (selectedTrackId.isNotEmpty())
    {
        const auto selected = std::find_if(cachedTracks.begin(), cachedTracks.end(),
            [this](const TrackData& track) { return track.id == selectedTrackId; });
        const int nextIndex = selected != cachedTracks.end()
                                  ? static_cast<int>(std::distance(cachedTracks.begin(), selected))
                                  : (cachedTracks.empty() ? -1
                                      : std::min(selectedTrackIndex, static_cast<int>(cachedTracks.size()) - 1));
        selectTrack(nextIndex);
    }
    else if (!cachedTracks.empty())
    {
        selectTrack(0);
    }
    updateWaveformCaches();
}

std::string TrackAreaComponent::getWaveformCacheKey(const AudioClip& clip)
{
    return "buffer-"
           + std::to_string(reinterpret_cast<std::uintptr_t>(clip.buffer.get()))
           + ":" + std::to_string(clip.buffer != nullptr
                                       ? clip.buffer->getNumSamples() : 0)
           + ":" + std::to_string(clip.sampleRate);
}

int TrackAreaComponent::getWaveformBaseSamplesPerPeak(int numSamples)
{
    constexpr int minimumSamplesPerPeak = 64;
    constexpr size_t maximumBasePeakCount = 262144;

    const int minimumForMemoryLimit = static_cast<int>(
        (static_cast<size_t>(std::max(0, numSamples))
         + maximumBasePeakCount - 1u)
        / maximumBasePeakCount);
    return std::max(
        minimumSamplesPerPeak,
        ((minimumForMemoryLimit + minimumSamplesPerPeak - 1)
         / minimumSamplesPerPeak) * minimumSamplesPerPeak);
}

std::shared_ptr<const TrackAreaComponent::WaveformCache>
TrackAreaComponent::findWaveformCache(const AudioClip& clip) const
{
    if (clip.buffer == nullptr)
        return {};

    const auto entry = waveformCaches.find(getWaveformCacheKey(clip));
    if (entry == waveformCaches.end() || entry->second == nullptr)
        return {};

    const auto& cache = *entry->second;
    if (cache.bufferIdentity != clip.buffer.get()
        || cache.bufferOwner.lock() != clip.buffer
        || cache.numSamples != clip.buffer->getNumSamples()
        || cache.sampleRate != clip.sampleRate)
        return {};

    return entry->second;
}

void TrackAreaComponent::updateWaveformCaches()
{
    std::unordered_set<std::string> newKeys;
    std::vector<const AudioClip*> representativeClips;

    for (const auto& track : cachedTracks)
    {
        for (const auto& clip : track.clips)
        {
            if (clip.buffer == nullptr || clip.buffer->getNumSamples() <= 0)
                continue;

            const auto key = getWaveformCacheKey(clip);
            if (newKeys.insert(key).second)
                representativeClips.push_back(&clip);
        }
    }

    if (newKeys != currentWaveformKeys)
    {
        currentWaveformKeys = newKeys;
        waveformGeneration.fetch_add(1);

        // Queued jobs release their audio-buffer references immediately. The
        // active job is signalled and also observes the generation change.
        waveformThreadPool.removeAllJobs(true, 0);
        waveformJobsInFlight.clear();
    }

    for (auto entry = waveformCaches.begin(); entry != waveformCaches.end();)
    {
        if (newKeys.find(entry->first) == newKeys.end())
            entry = waveformCaches.erase(entry);
        else
            ++entry;
    }

    for (const auto* clip : representativeClips)
    {
        if (clip != nullptr && findWaveformCache(*clip) == nullptr)
        {
            waveformCaches.erase(getWaveformCacheKey(*clip));
            scheduleWaveformCache(*clip);
        }
    }
}

void TrackAreaComponent::scheduleWaveformCache(const AudioClip& clip)
{
    if (waveformShutdownRequested.load() || clip.buffer == nullptr
        || clip.buffer->getNumSamples() <= 0 || clip.buffer->getNumChannels() <= 0)
        return;

    const auto buffer = clip.buffer;
    const auto cacheKey = getWaveformCacheKey(clip);
    const int numSamples = buffer->getNumSamples();
    const int sampleRate = clip.sampleRate;
    const auto generation = waveformGeneration.load();
    const auto jobKey = cacheKey + "@"
                        + std::to_string(reinterpret_cast<std::uintptr_t>(buffer.get()))
                        + ":" + std::to_string(numSamples)
                        + ":" + std::to_string(sampleRate);

    if (!waveformJobsInFlight.insert(jobKey).second)
        return;

    const auto safeThis = juce::Component::SafePointer<TrackAreaComponent>(this);
    waveformThreadPool.addJob([this, safeThis, buffer, cacheKey, jobKey, numSamples,
                               sampleRate, generation]()
    {
        constexpr int levelGrouping = 4;
        const int baseSamplesPerPeak = getWaveformBaseSamplesPerPeak(numSamples);

        auto* currentJob = juce::ThreadPoolJob::getCurrentThreadPoolJob();
        const auto shouldExit = [this, currentJob, generation]()
        {
            return waveformShutdownRequested.load()
                   || waveformGeneration.load() != generation
                   || (currentJob != nullptr && currentJob->shouldExit());
        };

        auto cache = std::make_shared<WaveformCache>();
        cache->bufferIdentity = buffer.get();
        cache->bufferOwner = buffer;
        cache->numSamples = numSamples;
        cache->sampleRate = sampleRate;

        WaveformCacheLevel baseLevel;
        baseLevel.samplesPerPeak = baseSamplesPerPeak;
        baseLevel.peaks.resize(
            (static_cast<size_t>(numSamples)
             + static_cast<size_t>(baseSamplesPerPeak) - 1u)
            / static_cast<size_t>(baseSamplesPerPeak));

        const float* samples = buffer->getReadPointer(0);
        for (size_t block = 0; block < baseLevel.peaks.size(); ++block)
        {
            if (shouldExit())
                return;

            const int start = static_cast<int>(block) * baseSamplesPerPeak;
            const int end = std::min(numSamples, start + baseSamplesPerPeak);
            float peak = 0.0f;
            for (int sample = start; sample < end; ++sample)
            {
                const float value = std::abs(samples[sample]);
                if (std::isfinite(value))
                    peak = std::max(peak, value);
            }
            baseLevel.peaks[block] = peak;
        }
        cache->levels.push_back(std::move(baseLevel));

        while (cache->levels.back().peaks.size() > 1)
        {
            if (shouldExit())
                return;

            const auto& previous = cache->levels.back();
            WaveformCacheLevel next;
            next.samplesPerPeak = previous.samplesPerPeak * levelGrouping;
            next.peaks.resize((previous.peaks.size() + levelGrouping - 1)
                              / levelGrouping);

            for (size_t group = 0; group < next.peaks.size(); ++group)
            {
                if ((group & 0x3ffu) == 0u && shouldExit())
                    return;

                const size_t start = group * levelGrouping;
                const size_t end = std::min(previous.peaks.size(),
                                            start + levelGrouping);
                float peak = 0.0f;
                for (size_t index = start; index < end; ++index)
                    peak = std::max(peak, previous.peaks[index]);
                next.peaks[group] = peak;
            }
            cache->levels.push_back(std::move(next));
        }

        juce::MessageManager::callAsync(
            [safeThis, cacheKey, jobKey, generation,
             cache = std::move(cache)]() mutable
            {
                if (safeThis == nullptr
                    || safeThis->waveformGeneration.load() != generation)
                    return;

                safeThis->waveformJobsInFlight.erase(jobKey);
                if (safeThis->waveformShutdownRequested.load())
                    return;

                bool stillCurrent = false;
                for (const auto& track : safeThis->cachedTracks)
                {
                    for (const auto& clip : track.clips)
                    {
                        if (getWaveformCacheKey(clip) == cacheKey
                            && clip.buffer.get() == cache->bufferIdentity
                            && clip.buffer != nullptr
                            && clip.buffer->getNumSamples() == cache->numSamples
                            && clip.sampleRate == cache->sampleRate)
                        {
                            stillCurrent = true;
                            break;
                        }
                    }
                    if (stillCurrent)
                        break;
                }

                if (stillCurrent)
                {
                    safeThis->waveformCaches[cacheKey] = std::move(cache);
                    safeThis->repaint();
                }
            });
    });
}

int TrackAreaComponent::getTrackStartY() const
{
    return rulerHeight + 8 - static_cast<int>(std::round(scrollY));
}

int TrackAreaComponent::getTrackIndexAtY(int y) const
{
    if (audioEngine == nullptr || y < rulerHeight)
        return -1;

    const int relativeY = y - getTrackStartY();
    if (relativeY < 0)
        return -1;

    const int rowHeight = trackHeight + trackGap;
    const int trackIndex = relativeY / rowHeight;
    if (relativeY % rowHeight >= trackHeight)
        return -1;

    return juce::isPositiveAndBelow(trackIndex,
                                    static_cast<int>(cachedTracks.size()))
               ? trackIndex
               : -1;
}

int TrackAreaComponent::getViewportBottom() const
{
    return std::max(rulerHeight + 8, getHeight() - scrollbarThickness);
}

double TrackAreaComponent::getVerticalContentHeight() const
{
    const int numTracks = audioEngine != nullptr
                              ? static_cast<int>(cachedTracks.size())
                              : 0;
    return static_cast<double>(numTracks * (trackHeight + trackGap)
                               + 16);
}

double TrackAreaComponent::getCachedDuration() const
{
    double duration = 0.0;
    for (const auto& track : cachedTracks)
        duration = std::max(duration, track.getDuration());

    return duration;
}

double TrackAreaComponent::snapTimeToBeat(double time) const
{
    const double clampedTime = std::max(0.0, time);
    if (audioEngine == nullptr)
        return clampedTime;
    if (!snapEnabled)
        return clampedTime;

    const TempoMap tempoMap(audioEngine->getProjectDawSettings().tempoMap);
    return tempoMap.snapToNearestBeat(clampedTime);
}

void TrackAreaComponent::updateSnapButtonText()
{
    snapBtn.setButtonText(
        snapEnabled ? juce::CharPointer_UTF8(u8"スナップ ON")
                    : juce::CharPointer_UTF8(u8"スナップ OFF"));
}

void TrackAreaComponent::beginTrackNameEdit(int trackIndex)
{
    if (audioEngine == nullptr)
        return;

    const auto tracks = cachedTracks;
    if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(tracks.size())))
        return;

    if (trackNameEditor.isBeingEdited())
    {
        trackNameEditor.hideEditor(false);
        finishTrackNameEdit();
    }

    editingTrackIndex = trackIndex;
    trackNameEditor.setText(tracks[static_cast<size_t>(trackIndex)].name,
                            juce::dontSendNotification);

    trackNameEditor.setBounds(getTrackHeaderLayout(trackIndex).title.withTrimmedLeft(16));
    trackNameEditor.setVisible(true);
    trackNameEditor.toFront(false);
    trackNameEditor.showEditor();
}

void TrackAreaComponent::finishTrackNameEdit()
{
    if (editingTrackIndex < 0)
    {
        trackNameEditor.setVisible(false);
        return;
    }

    if (trackNameEditor.isBeingEdited())
        return;

    if (audioEngine != nullptr)
    {
        const auto& tracks = cachedTracks;
        const juce::String newName = trackNameEditor.getText().trim();

        if (juce::isPositiveAndBelow(editingTrackIndex, static_cast<int>(tracks.size()))
            && newName.isNotEmpty()
            && newName != tracks[static_cast<size_t>(editingTrackIndex)].name)
            audioEngine->setTrackName(editingTrackIndex, newName);
    }

    editingTrackIndex = -1;
    trackNameEditor.setVisible(false);
    repaint();
}

bool TrackAreaComponent::isSupportedAudioFile(const juce::File& file)
{
    return file.existsAsFile()
           && file.hasFileExtension("wav;wave;aif;aiff;flac;mp3;ogg");
}

void TrackAreaComponent::loadAudioFiles(const juce::Array<juce::File>& files)
{
    if (audioEngine == nullptr || files.isEmpty())
        return;

    audioEngine->loadAudioFilesAsync(files);
}

void TrackAreaComponent::showVst3Menu(int trackIndex)
{
    if (audioEngine == nullptr
        || !juce::isPositiveAndBelow(trackIndex, static_cast<int>(cachedTracks.size())))
        return;

    const auto track = cachedTracks[static_cast<size_t>(trackIndex)];
    const bool hasPlugin = track.vst3Name.isNotEmpty();
    const bool hasEffects = !track.effectSlots.empty();

    juce::PopupMenu menu;
    menu.addItem(1, juce::CharPointer_UTF8(u8"VST3ブラウザーを開く"));
    menu.addItem(5, juce::CharPointer_UTF8(u8"ファイルからVST3を選択…"));
    menu.addSeparator();
    menu.addItem(2,
                 hasPlugin
                     ? juce::String(juce::CharPointer_UTF8(u8"設定画面を開く: ")) + track.vst3Name
                     : juce::String(juce::CharPointer_UTF8(u8"設定画面を開く")),
                 hasPlugin);
    menu.addItem(3,
                 track.vst3Bypassed
                     ? juce::String(juce::CharPointer_UTF8(u8"エフェクトを有効にする"))
                     : juce::String(juce::CharPointer_UTF8(u8"エフェクトをバイパス")),
                 hasPlugin,
                 track.vst3Bypassed);
    menu.addItem(4, juce::CharPointer_UTF8(u8"VST3を取り外す"), hasPlugin);
    menu.addSeparator();
    menu.addItem(6,
                 track.effectChainBypassed
                     ? juce::CharPointer_UTF8(u8"通常エフェクトをすべて有効にする")
                     : juce::CharPointer_UTF8(u8"通常エフェクトをすべてバイパス"),
                 hasEffects,
                 track.effectChainBypassed);

    const auto safeThis = juce::Component::SafePointer<TrackAreaComponent>(this);
    const auto fxButtonArea = localAreaToGlobal(getTrackHeaderLayout(trackIndex).effects);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(fxButtonArea),
        [safeThis, trackIndex, bypassed = track.vst3Bypassed,
         chainBypassed = track.effectChainBypassed](int result)
        {
            if (safeThis == nullptr || safeThis->audioEngine == nullptr)
                return;

            switch (result)
            {
                case 1:
                    if (safeThis->onVst3BrowserClicked)
                        safeThis->onVst3BrowserClicked(trackIndex);
                    break;
                case 2: safeThis->audioEngine->showVst3PluginEditor(trackIndex); break;
                case 3: safeThis->audioEngine->setTrackVst3Bypassed(trackIndex, !bypassed); break;
                case 4: safeThis->audioEngine->removeVst3PluginFromTrack(trackIndex); break;
                case 5: safeThis->chooseVst3File(trackIndex); break;
                case 6: safeThis->audioEngine->setEffectChainBypassed(
                            trackIndex, !chainBypassed); break;
                default: break;
            }
        });
}

void TrackAreaComponent::showTrackPitchMenu(int trackIndex,
                                            juce::Rectangle<int> targetBounds)
{
    if (audioEngine == nullptr
        || !juce::isPositiveAndBelow(trackIndex, static_cast<int>(cachedTracks.size())))
        return;

    const int currentSemitones = juce::roundToInt(
        cachedTracks[static_cast<size_t>(trackIndex)].pitchSemitones);
    juce::PopupMenu menu;
    const auto& selectedTrack = cachedTracks[static_cast<size_t>(trackIndex)];
    menu.addItem(1000, juce::CharPointer_UTF8(u8"原曲へ戻す（キー・速度）"),
                 currentSemitones != 0
                     || std::abs(selectedTrack.playbackSpeed - 1.0) > 0.001);
    menu.addSeparator();
    for (int semitones = 12; semitones >= -12; --semitones)
    {
        juce::String label;
        if (semitones == 0)
            label = juce::CharPointer_UTF8(u8"\u539f\u66f2 (0)");
        else
            label = juce::String(semitones > 0 ? "+" : "") + juce::String(semitones);
        menu.addItem(semitones + 13,
                     label,
                     true,
                     semitones == currentSemitones);
    }

    const auto safeThis = juce::Component::SafePointer<TrackAreaComponent>(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(targetBounds)),
        [safeThis, trackIndex](int result)
        {
            if (safeThis == nullptr || safeThis->audioEngine == nullptr || result == 0)
                return;
            if (result == 1000)
                safeThis->audioEngine->resetTrackTransform(trackIndex);
            else
                safeThis->audioEngine->setTrackPitchSemitones(trackIndex,
                                                              result - 13);
        });
}

void TrackAreaComponent::showTrackSpeedMenu(int trackIndex,
                                            juce::Rectangle<int> targetBounds)
{
    if (audioEngine == nullptr
        || !juce::isPositiveAndBelow(trackIndex, static_cast<int>(cachedTracks.size())))
        return;

    const double currentSpeed =
        cachedTracks[static_cast<size_t>(trackIndex)].playbackSpeed;
    juce::PopupMenu menu;
    const auto& selectedTrack = cachedTracks[static_cast<size_t>(trackIndex)];
    menu.addItem(1000, juce::CharPointer_UTF8(u8"原曲へ戻す（キー・速度）"),
                 std::abs(currentSpeed - 1.0) > 0.001
                     || std::abs(selectedTrack.pitchSemitones) > 0.001);
    menu.addSeparator();
    for (int step = 0; step <= 15; ++step)
    {
        const double speed = 0.75 + step * 0.05;
        menu.addItem(step + 1,
                     juce::String(speed, 2) + "x",
                     true,
                     std::abs(speed - currentSpeed) < 0.001);
    }

    const auto safeThis = juce::Component::SafePointer<TrackAreaComponent>(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(targetBounds)),
        [safeThis, trackIndex](int result)
        {
            if (safeThis == nullptr || safeThis->audioEngine == nullptr || result == 0)
                return;
            if (result == 1000)
                safeThis->audioEngine->resetTrackTransform(trackIndex);
            else
                safeThis->audioEngine->setTrackPlaybackSpeed(
                    trackIndex,
                    0.75 + (result - 1) * 0.05);
        });
}

void TrackAreaComponent::chooseVst3File(int trackIndex)
{
    if (audioEngine == nullptr)
        return;

    vst3FileChooser = std::make_shared<juce::FileChooser>(
        juce::CharPointer_UTF8(u8"VST3エフェクトを選択"),
        audioEngine->getDefaultVst3PluginDirectory(),
        "*.vst3");

    const auto chooser = vst3FileChooser;
    const auto safeThis = juce::Component::SafePointer<TrackAreaComponent>(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::canSelectDirectories,
        [safeThis, chooser, trackIndex](const juce::FileChooser& fileChooser)
        {
            if (safeThis == nullptr)
                return;
            const auto results = fileChooser.getResults();
            if (safeThis->audioEngine != nullptr && !results.isEmpty())
            {
                if (results[0].getFileNameWithoutExtension()
                        .containsIgnoreCase("PitchNet"))
                    safeThis->audioEngine->loadVst3PluginForTrack(trackIndex,
                                                                  results[0]);
                else
                    safeThis->audioEngine->addEffectPluginForTrack(trackIndex,
                                                                    results[0]);
            }
            safeThis->vst3FileChooser.reset();
        });
}

void TrackAreaComponent::mouseDown(const juce::MouseEvent& event)
{
    // コンポーネントをクリックしたらフォーカスを要求
    grabKeyboardFocus();
    
    if (audioEngine == nullptr) return;
    clipDragMode = ClipDragMode::none;
    draggingClipId.clear();
    dragRhythmMarkerIndex = -1;
    dragStartRhythmMarkers.clear();
    
    // クリック処理中に状態変更コールバックがキャッシュを更新しても参照切れしないよう、
    // この操作中だけ画面キャッシュ由来のコピーを保持する。
    const auto tracks = cachedTracks;
    const int startY = getTrackStartY();
    
    int mx = event.x;
    int my = event.y;
    // 固定ツール行の余白はタイムラインのシーク操作に使わない。
    if (my < actionToolbarHeight) return;
    
    CursorMode mode = audioEngine->getCursorMode();
    double zoomLevel = audioEngine->getZoomLevel();
    loopMarkerDrag = LoopMarkerDrag::none;
    if (my >= actionToolbarHeight && my < rulerHeight
        && mx > leftMargin + sidebarWidth)
    {
        const auto settings = audioEngine->getProjectDawSettings();
        const int rulerOrigin = leftMargin + sidebarWidth + 4
                              - static_cast<int>(scrollX);
        const int startX = rulerOrigin
                         + juce::roundToInt(settings.loopStartSeconds * zoomLevel);
        const int endX = rulerOrigin
                       + juce::roundToInt(settings.loopEndSeconds * zoomLevel);
        if (settings.hasLoopStart && std::abs(mx - startX) <= 9)
        {
            loopMarkerDrag = LoopMarkerDrag::start;
            return;
        }
        if (settings.hasLoopEnd && std::abs(mx - endX) <= 9)
        {
            loopMarkerDrag = LoopMarkerDrag::end;
            return;
        }
    }
    
    // 矢印ツールの処理
    bool clickedOnClip = false;
    
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        if (my < rulerHeight || my >= getViewportBottom())
            break;

        const auto& track = tracks[i];
        int trackTop = startY + static_cast<int>(i) * (trackHeight + trackGap);
        int trackBottom = trackTop + trackHeight;
        
        // このトラックの範囲内か？
        if (my < trackTop || my >= trackBottom) continue;
        selectTrack(static_cast<int>(i));
        
        // --- サイドバー内のクリック判定 ---
        if (mx <= leftMargin + sidebarWidth)
        {
            const int trackIdx = static_cast<int>(i);
            const auto header = getTrackHeaderLayout(trackIdx);
            if (event.mods.isPopupMenu())
            {
                showTrackHeaderMenu(trackIdx, header.title);
                return;
            }
            if (header.mute.contains(mx, my)) audioEngine->toggleMute(trackIdx);
            else if (header.solo.contains(mx, my)) audioEngine->toggleSolo(trackIdx);
            else if (header.arm.contains(mx, my)) audioEngine->toggleArm(trackIdx);
            else if (header.effects.contains(mx, my)) showVst3Menu(trackIdx);
            else if (header.remove.contains(mx, my))
            {
                audioEngine->removeTrack(trackIdx);
                resized();
                updateScrollbar();
            }
            repaint();
            return;
        }
        
        // --- 波形エリア内のクリック判定 ---
        int waveXBase = leftMargin + sidebarWidth + 4 - static_cast<int>(scrollX); // 横スクロールオフセット (4はreducedによる余白)
        const double clickedTime = snapTimeToBeat(
            static_cast<double>(mx - waveXBase) / zoomLevel);
        const double trackSpeed = juce::jlimit(0.75, 1.5, track.playbackSpeed);
        
        for (const auto& clip : track.clips)
        {
            const double displayStart = clip.startTime / trackSpeed;
            const double displayDuration = clip.duration / trackSpeed;
            int clipX = waveXBase + static_cast<int>(displayStart * zoomLevel);
            int clipW = static_cast<int>(displayDuration * zoomLevel);
            
            if (mx >= clipX && mx < clipX + clipW)
            {
                // クリップをクリックした
                audioEngine->setSelectedClipId(clip.id);
                clickedOnClip = true;

                if (event.mods.isPopupMenu())
                {
                    juce::PopupMenu menu;
                    menu.addItem(1, juce::CharPointer_UTF8(u8"ここで分割    S"));
                    menu.addItem(2, juce::CharPointer_UTF8(u8"左端をここまで短くする"));
                    menu.addItem(3, juce::CharPointer_UTF8(u8"右端をここまで短くする"));
                    menu.addSeparator();
                    menu.addItem(4, juce::CharPointer_UTF8(u8"短いフェード（0.1秒）"));
                    menu.addItem(5, juce::CharPointer_UTF8(u8"長いフェード（0.5秒）"));
                    menu.addItem(6, juce::CharPointer_UTF8(u8"フェードを解除"));
                    menu.addSeparator();
                    menu.addItem(7, juce::CharPointer_UTF8(u8"削除    Delete"));
                    const auto safeThis = juce::Component::SafePointer<TrackAreaComponent>(this);
                    const auto clipId = clip.id;
                    const double sourceClickTime = clickedTime * trackSpeed;
                    menu.showMenuAsync(
                        juce::PopupMenu::Options().withTargetScreenArea(
                            localAreaToGlobal(juce::Rectangle<int>(mx, my, 1, 1))),
                        [safeThis, clipId, sourceClickTime](int result)
                        {
                            if (safeThis == nullptr || safeThis->audioEngine == nullptr)
                                return;
                            switch (result)
                            {
                                case 1: safeThis->audioEngine->splitClip(clipId,
                                                                         sourceClickTime); break;
                                case 2: safeThis->audioEngine->trimClipLeft(clipId,
                                                                            sourceClickTime); break;
                                case 3: safeThis->audioEngine->trimClipRight(clipId,
                                                                             sourceClickTime); break;
                                case 4: safeThis->audioEngine->setClipFades(clipId,
                                                                            0.1, 0.1); break;
                                case 5: safeThis->audioEngine->setClipFades(clipId,
                                                                            0.5, 0.5); break;
                                case 6: safeThis->audioEngine->setClipFades(clipId,
                                                                            0.0, 0.0); break;
                                case 7: safeThis->audioEngine->removeClip(clipId); break;
                                default: break;
                            }
                        });
                    return;
                }
                
                const int waveformTop = trackTop + 8;
                const bool inTopHandleBand = my <= waveformTop + 18;
                const bool inRhythmBand = my >= waveformTop + 22
                                       && my <= waveformTop + 44;
                const int fadeInX = clipX + juce::roundToInt(
                    clip.fadeInSeconds / trackSpeed * zoomLevel);
                const int fadeOutX = clipX + clipW - juce::roundToInt(
                    clip.fadeOutSeconds / trackSpeed * zoomLevel);

                if (mode == CursorMode::Arrow && inRhythmBand)
                {
                    for (int markerIndex = static_cast<int>(clip.rhythmMarkers.size()) - 1;
                         markerIndex >= 0; --markerIndex)
                    {
                        const auto& marker = clip.rhythmMarkers[
                            static_cast<size_t>(markerIndex)];
                        const int markerStartX = clipX + juce::roundToInt(
                            marker.targetTime / trackSpeed * zoomLevel);
                        const double visibleEnd = marker.hasDuration()
                            ? marker.targetEndTime
                            : std::min(clip.duration, marker.targetTime + 0.08);
                        const int markerEndX = std::max(markerStartX + 8,
                            clipX + juce::roundToInt(
                                visibleEnd / trackSpeed * zoomLevel));
                        if (mx < markerStartX - 6 || mx > markerEndX + 6)
                            continue;

                        draggingClipId = clip.id;
                        dragStartMouseX = mx;
                        dragTrackSpeed = trackSpeed;
                        dragStartClipTime = clip.startTime;
                        dragStartClipEndTime = clip.startTime + clip.duration;
                        dragRhythmMarkerIndex = markerIndex;
                        dragStartRhythmMarkers = clip.rhythmMarkers;
                        if (std::abs(mx - markerStartX) <= 6)
                            clipDragMode = ClipDragMode::rhythmStart;
                        else if (std::abs(mx - markerEndX) <= 6)
                            clipDragMode = ClipDragMode::rhythmEnd;
                        else
                            clipDragMode = ClipDragMode::rhythmMove;
                        repaint();
                        return;
                    }
                }

                if (mode == CursorMode::Split)
                {
                    audioEngine->splitClip(clip.id, clickedTime * trackSpeed);
                }
                else if (mode == CursorMode::Arrow && inTopHandleBand
                         && std::abs(mx - fadeInX) <= 9)
                {
                    draggingClipId = clip.id;
                    clipDragMode = ClipDragMode::fadeIn;
                    dragStartMouseX = mx;
                    dragStartFadeIn = clip.fadeInSeconds;
                    dragStartFadeOut = clip.fadeOutSeconds;
                    dragTrackSpeed = trackSpeed;
                }
                else if (mode == CursorMode::Arrow && inTopHandleBand
                         && std::abs(mx - fadeOutX) <= 9)
                {
                    draggingClipId = clip.id;
                    clipDragMode = ClipDragMode::fadeOut;
                    dragStartMouseX = mx;
                    dragStartFadeIn = clip.fadeInSeconds;
                    dragStartFadeOut = clip.fadeOutSeconds;
                    dragTrackSpeed = trackSpeed;
                }
                else if (mode == CursorMode::Arrow && mx - clipX <= 9)
                {
                    draggingClipId = clip.id;
                    clipDragMode = ClipDragMode::trimLeft;
                    dragStartMouseX = mx;
                    dragStartClipTime = clip.startTime;
                    dragStartClipEndTime = clip.startTime + clip.duration;
                    dragTrackSpeed = trackSpeed;
                }
                else if (mode == CursorMode::Arrow && clipX + clipW - mx <= 9)
                {
                    draggingClipId = clip.id;
                    clipDragMode = ClipDragMode::trimRight;
                    dragStartMouseX = mx;
                    dragStartClipTime = clip.startTime;
                    dragStartClipEndTime = clip.startTime + clip.duration;
                    dragTrackSpeed = trackSpeed;
                }
                else if (mode == CursorMode::Hand)
                {
                    // ハンドモードでクリップ上ならドラッグ準備
                    draggingClipId = clip.id;
                    dragStartClipTime = displayStart;
                    dragStartMouseX = mx;
                    dragStartTrackIndex = static_cast<int>(i);
                    isDraggingClip = false;
                    clipDragMode = ClipDragMode::move;
                }
                else // Arrow または その他
                {
                    // 矢印モードならシークするだけ
                    audioEngine->setCurrentTime(clickedTime);
                }
                
                repaint();
                return;
            }
        }
        
        break; // 1つのトラックのみ処理
    }
    
    // 波形エリアをクリックしたがクリップ上ではなかった場合、選択を解除してシークまたはパン
    if (!clickedOnClip && mx > leftMargin + sidebarWidth)
    {
        audioEngine->setSelectedClipId("");
        
        if (mode == CursorMode::Hand)
        {
            // ハンドツールでは背景ドラッグによるパンを行わない（スクロールバーに移行）
        }
        else
        {
            int waveXBase = leftMargin + sidebarWidth + 4 - static_cast<int>(scrollX);
            double newTime = snapTimeToBeat(static_cast<double>(mx - waveXBase) / zoomLevel);
            audioEngine->setCurrentTime(newTime);
        }
        
        repaint();
    }
}

void TrackAreaComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    if (audioEngine == nullptr)
        return;

    const int trackIndex = getTrackIndexAtY(event.y);
    if (trackIndex < 0)
        return;

    const auto header = getTrackHeaderLayout(trackIndex);
    if (header.title.contains(event.getPosition()))
        beginTrackNameEdit(trackIndex);
}

void TrackAreaComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (audioEngine == nullptr || event.getMouseDownY() < actionToolbarHeight) return;

    if (loopMarkerDrag != LoopMarkerDrag::none)
    {
        const int rulerOrigin = leftMargin + sidebarWidth + 4
                              - static_cast<int>(scrollX);
        const double time = snapTimeToBeat(
            static_cast<double>(event.x - rulerOrigin)
                / std::max(1.0, audioEngine->getZoomLevel()));
        auto settings = audioEngine->getProjectDawSettings();
        if (loopMarkerDrag == LoopMarkerDrag::start)
        {
            settings.hasLoopStart = true;
            settings.loopStartSeconds = std::max(
                0.0, settings.hasLoopEnd
                    ? std::min(time, settings.loopEndSeconds - 0.01) : time);
        }
        else
        {
            settings.hasLoopEnd = true;
            settings.loopEndSeconds = std::max(
                settings.hasLoopStart ? settings.loopStartSeconds + 0.01 : 0.01,
                time);
        }
        audioEngine->setProjectDawSettings(settings);
        repaint();
        return;
    }
    
    if (!draggingClipId.isEmpty()
        && clipDragMode != ClipDragMode::none
        && clipDragMode != ClipDragMode::move)
    {
        if (!isDraggingClip)
        {
            audioEngine->beginEdit();
            isDraggingClip = true;
        }
        const double sourceDelta = static_cast<double>(event.x - dragStartMouseX)
                                 / std::max(1.0, audioEngine->getZoomLevel())
                                 * dragTrackSpeed;
        switch (clipDragMode)
        {
            case ClipDragMode::trimLeft:
                audioEngine->trimClipLeft(draggingClipId,
                                          dragStartClipTime + sourceDelta);
                break;
            case ClipDragMode::trimRight:
                audioEngine->trimClipRight(draggingClipId,
                                           dragStartClipEndTime + sourceDelta);
                break;
            case ClipDragMode::fadeIn:
                audioEngine->setClipFades(draggingClipId,
                                          std::max(0.0,
                                                   dragStartFadeIn + sourceDelta),
                                          dragStartFadeOut);
                break;
            case ClipDragMode::fadeOut:
                audioEngine->setClipFades(draggingClipId,
                                          dragStartFadeIn,
                                          std::max(0.0,
                                                   dragStartFadeOut - sourceDelta));
                break;
            case ClipDragMode::rhythmMove:
            case ClipDragMode::rhythmStart:
            case ClipDragMode::rhythmEnd:
            {
                if (!juce::isPositiveAndBelow(
                        dragRhythmMarkerIndex,
                        static_cast<int>(dragStartRhythmMarkers.size())))
                    break;
                auto markers = dragStartRhythmMarkers;
                auto& marker = markers[static_cast<size_t>(dragRhythmMarkerIndex)];
                const double originalEnd = marker.hasDuration()
                    ? marker.targetEndTime
                    : std::min(marker.targetTime + 0.08,
                               std::max(marker.targetTime + 0.01,
                                        dragStartClipEndTime - dragStartClipTime));
                marker.sourceEndTime = marker.sourceEndTime > marker.sourceTime
                    ? marker.sourceEndTime
                    : std::min(marker.sourceTime + (originalEnd - marker.targetTime),
                               std::max(marker.sourceTime + 0.01,
                                        dragStartClipEndTime - dragStartClipTime));
                marker.targetEndTime = originalEnd;

                const double clipDuration = std::max(
                    0.0, dragStartClipEndTime - dragStartClipTime);
                const double previousBoundary = dragRhythmMarkerIndex > 0
                    ? std::max(markers[static_cast<size_t>(dragRhythmMarkerIndex - 1)].targetTime,
                               markers[static_cast<size_t>(dragRhythmMarkerIndex - 1)].targetEndTime)
                          + 0.005
                    : 0.0;
                const double nextBoundary = dragRhythmMarkerIndex + 1
                                                < static_cast<int>(markers.size())
                    ? markers[static_cast<size_t>(dragRhythmMarkerIndex + 1)].targetTime
                          - 0.005
                    : clipDuration;
                const double sourceLength = std::max(
                    0.01, marker.sourceEndTime - marker.sourceTime);
                const double minimumTargetLength = sourceLength * 0.5;
                const double maximumTargetLength = sourceLength * 2.0;

                if (clipDragMode == ClipDragMode::rhythmStart)
                {
                    marker.targetTime = juce::jlimit(
                        std::max(previousBoundary,
                                 marker.targetEndTime - maximumTargetLength),
                        std::max(previousBoundary,
                                 marker.targetEndTime - minimumTargetLength),
                        marker.targetTime + sourceDelta);
                }
                else if (clipDragMode == ClipDragMode::rhythmEnd)
                {
                    marker.targetEndTime = juce::jlimit(
                        marker.targetTime + minimumTargetLength,
                        std::max(marker.targetTime + minimumTargetLength,
                                 std::min(nextBoundary,
                                          marker.targetTime
                                              + maximumTargetLength)),
                        marker.targetEndTime + sourceDelta);
                }
                else
                {
                    const double length = juce::jlimit(
                        minimumTargetLength,
                        maximumTargetLength,
                        marker.targetEndTime - marker.targetTime);
                    const double newStart = juce::jlimit(
                        previousBoundary,
                        std::max(previousBoundary, nextBoundary - length),
                        marker.targetTime + sourceDelta);
                    marker.targetTime = newStart;
                    marker.targetEndTime = newStart + length;
                }
                audioEngine->setClipRhythmMarkers(draggingClipId,
                                                  std::move(markers));
                break;
            }
            default:
                break;
        }
        repaint();
        return;
    }

    CursorMode mode = audioEngine->getCursorMode();
    
    if (mode == CursorMode::Hand && draggingClipId.isEmpty())
    {
        // ハンドツールでは背景ドラッグによるパンを行わない
        return;
    }
    
    if (mode == CursorMode::Hand && !draggingClipId.isEmpty()
        && clipDragMode == ClipDragMode::move)
    {
        // クリップの移動
        if (!isDraggingClip)
        {
            audioEngine->beginEdit();
            isDraggingClip = true;
        }
        
        double zoomLevel = audioEngine->getZoomLevel();
        double timeDelta = static_cast<double>(event.x - dragStartMouseX) / zoomLevel;
        double newTime = snapTimeToBeat(dragStartClipTime + timeDelta);
        
        // Y座標から新しいトラックを判定
        int startY = getTrackStartY();
        int destTrackIndex = dragStartTrackIndex;
        
        const auto trackCount = cachedTracks.size();
        for (size_t i = 0;
             event.y >= rulerHeight && event.y < getViewportBottom()
                 && i < trackCount;
             ++i)
        {
            int trackTop = startY + static_cast<int>(i) * (trackHeight + trackGap);
            int trackBottom = trackTop + trackHeight;
            if (event.y >= trackTop && event.y < trackBottom)
            {
                destTrackIndex = static_cast<int>(i);
                break;
            }
        }
        
        double destinationSpeed = 1.0;
        if (juce::isPositiveAndBelow(destTrackIndex,
                                     static_cast<int>(cachedTracks.size())))
        {
            destinationSpeed = juce::jlimit(
                0.75,
                1.5,
                cachedTracks[static_cast<size_t>(destTrackIndex)].playbackSpeed);
        }
        audioEngine->moveClip(draggingClipId,
                              destTrackIndex,
                              newTime * destinationSpeed);
        
        dragStartTrackIndex = destTrackIndex;
        selectTrack(destTrackIndex);
        
        repaint();
        return;
    }
    
    if (mode == CursorMode::Arrow)
    {
        // 矢印モードのドラッグはシーク位置の更新
        if (event.x > leftMargin + sidebarWidth)
        {
            int waveXBase = leftMargin + sidebarWidth + 4 - static_cast<int>(scrollX);
            double newTime = snapTimeToBeat(static_cast<double>(event.x - waveXBase)
                                            / audioEngine->getZoomLevel());
            audioEngine->setCurrentTime(newTime);
            repaint();
        }
    }
}

void TrackAreaComponent::mouseWheelMove(const juce::MouseEvent& event,
                                        const juce::MouseWheelDetails& wheel)
{
    juce::ignoreUnused(event);

    const bool scrollHorizontally = event.mods.isShiftDown()
                                    || std::abs(wheel.deltaX) > std::abs(wheel.deltaY);

    if (scrollHorizontally)
    {
        const double delta = std::abs(wheel.deltaX) > 0.0001f ? wheel.deltaX : wheel.deltaY;
        horizontalScrollBar.setCurrentRangeStart(scrollX - delta * 500.0);
    }
    else
    {
        verticalScrollBar.setCurrentRangeStart(scrollY - wheel.deltaY * 300.0);
    }
}

void TrackAreaComponent::mouseUp(const juce::MouseEvent& event)
{
    juce::ignoreUnused(event);
    if (isDraggingClip)
    {
        if (audioEngine) audioEngine->endEdit();
        isDraggingClip = false;
    }
    
    draggingClipId = "";
    clipDragMode = ClipDragMode::none;
    dragRhythmMarkerIndex = -1;
    dragStartRhythmMarkers.clear();
    loopMarkerDrag = LoopMarkerDrag::none;
}

bool TrackAreaComponent::keyPressed(const juce::KeyPress& key)
{
    if (audioEngine == nullptr) return false;
    
    juce::String selectedId = audioEngine->getSelectedClipId();
    if (selectedId.isEmpty()) return false;
    
    if (key.getKeyCode() == juce::KeyPress::deleteKey || key.getKeyCode() == juce::KeyPress::backspaceKey)
    {
        audioEngine->removeClip(selectedId);
        repaint();
        return true;
    }
    
    if (!key.getModifiers().isCommandDown() && (key.getTextCharacter() == 's' || key.getTextCharacter() == 'S'))
    {
        // 選択されたクリップを現在の再生位置で分割
        double selectedTrackSpeed = 1.0;
        for (const auto& track : cachedTracks)
        {
            const auto selectedClip = std::find_if(
                track.clips.begin(),
                track.clips.end(),
                [&selectedId](const AudioClip& clip) { return clip.id == selectedId; });
            if (selectedClip != track.clips.end())
            {
                selectedTrackSpeed = juce::jlimit(0.75,
                                                  1.5,
                                                  track.playbackSpeed);
                break;
            }
        }
        audioEngine->splitClip(
            selectedId,
            snapTimeToBeat(audioEngine->getCurrentTime()) * selectedTrackSpeed);
        repaint();
        return true;
    }
    
    return false;
}

bool TrackAreaComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& path : files)
    {
        if (isSupportedAudioFile(juce::File(path)))
            return true;
    }

    return false;
}

void TrackAreaComponent::fileDragEnter(const juce::StringArray& files, int x, int y)
{
    juce::ignoreUnused(x, y);
    isFileDragOver = isInterestedInFileDrag(files);
    repaint();
}

void TrackAreaComponent::fileDragExit(const juce::StringArray& files)
{
    juce::ignoreUnused(files);
    isFileDragOver = false;
    repaint();
}

void TrackAreaComponent::filesDropped(const juce::StringArray& files, int x, int y)
{
    juce::ignoreUnused(x, y);
    isFileDragOver = false;

    juce::Array<juce::File> audioFiles;
    for (const auto& path : files)
    {
        const juce::File file(path);
        if (file.existsAsFile())
            audioFiles.add(file);
    }

    loadAudioFiles(audioFiles);
    repaint();
}

void TrackAreaComponent::drawWaveform(juce::Graphics& g, const AudioClip& clip, juce::Rectangle<int> area)
{
    if (clip.buffer == nullptr || clip.buffer->getNumSamples() == 0
        || area.getWidth() <= 0 || area.getHeight() <= 0)
        return;

    const auto visibleArea = area.getIntersection(g.getClipBounds());
    if (visibleArea.isEmpty())
        return;

    const float centerY = area.getCentreY();
    const int sampleRate = clip.sampleRate > 0 ? clip.sampleRate : 44100;
    const double offsetSamples = std::max(0.0, clip.offset * sampleRate);
    const double durationSamples = std::max(0.0, clip.duration * sampleRate);
    const double samplesPerPixel = durationSamples / static_cast<double>(area.getWidth());
    const int totalSamples = clip.buffer->getNumSamples();

    const auto cache = findWaveformCache(clip);
    const auto finestCachedResolution = cache != nullptr && !cache->levels.empty()
        ? cache->levels.front().samplesPerPeak
        : static_cast<int64_t>(getWaveformBaseSamplesPerPeak(totalSamples));
    const bool useVisibleRawSamples = samplesPerPixel
                                      < static_cast<double>(finestCachedResolution);

    if (!useVisibleRawSamples && (cache == nullptr || cache->levels.empty()))
    {
        // The full-resolution scan runs on the waveform worker. Keep the UI
        // responsive and show a baseline until the cache is ready.
        g.fillRect(static_cast<float>(visibleArea.getX()), centerY - 0.5f,
                   static_cast<float>(visibleArea.getWidth()), 1.0f);
        return;
    }

    const WaveformCacheLevel* level = nullptr;
    if (!useVisibleRawSamples)
    {
        level = &cache->levels.front();
        for (const auto& candidate : cache->levels)
        {
            if (candidate.samplesPerPeak > samplesPerPixel)
                break;
            level = &candidate;
        }

        if (level->peaks.empty())
            return;
    }

    const double clipSourceStart = juce::jlimit(
        0.0, static_cast<double>(totalSamples), offsetSamples);
    const double clipSourceEnd = juce::jlimit(
        clipSourceStart, static_cast<double>(totalSamples),
        offsetSamples + durationSamples);
    const float* rawSamples = useVisibleRawSamples
                                  ? clip.buffer->getReadPointer(0) : nullptr;

    juce::Path path;
    for (int x = visibleArea.getX(); x < visibleArea.getRight(); ++x)
    {
        const double relativeStart = static_cast<double>(x - area.getX())
                                     / static_cast<double>(area.getWidth());
        const double relativeEnd = static_cast<double>(x + 1 - area.getX())
                                   / static_cast<double>(area.getWidth());
        const double sourceStart = offsetSamples + relativeStart * durationSamples;
        const double sourceEnd = offsetSamples + relativeEnd * durationSamples;

        float peak = 0.0f;
        if (useVisibleRawSamples)
        {
            // At close zoom levels the bounded cache can be coarser than one
            // pixel. Scan only this visible pixel's source interval so trims
            // stay exact without ever walking the full clip on the UI thread.
            const double clampedStart = juce::jlimit(
                clipSourceStart, clipSourceEnd, sourceStart);
            const double clampedEnd = juce::jlimit(
                clampedStart, clipSourceEnd, sourceEnd);
            const int firstSample = juce::jlimit(
                0, totalSamples - 1,
                static_cast<int>(std::floor(clampedStart)));
            const int endSample = juce::jlimit(
                firstSample + 1, totalSamples,
                static_cast<int>(std::ceil(clampedEnd)));
            for (int sample = firstSample; sample < endSample; ++sample)
            {
                const float value = std::abs(rawSamples[sample]);
                if (std::isfinite(value))
                    peak = std::max(peak, value);
            }
        }
        else
        {
            const int firstPeak = juce::jlimit(
                0, static_cast<int>(level->peaks.size()) - 1,
                static_cast<int>(std::floor(sourceStart / level->samplesPerPeak)));
            const int lastPeak = juce::jlimit(
                firstPeak, static_cast<int>(level->peaks.size()) - 1,
                static_cast<int>(std::floor(std::max(sourceStart, sourceEnd - 1.0)
                                            / level->samplesPerPeak)));
            for (int index = firstPeak; index <= lastPeak; ++index)
                peak = std::max(peak, level->peaks[static_cast<size_t>(index)]);
        }

        float barHeight = peak * (area.getHeight() / 2.0f) * 0.9f;
        if (!std::isfinite(barHeight))
            barHeight = 0.0f;
        barHeight = std::max(0.5f, barHeight);
        path.addRectangle(static_cast<float>(x), centerY - barHeight,
                          1.0f, barHeight * 2.0f);
    }
    g.fillPath(path);
}

void TrackAreaComponent::drawRecordingWaveform(juce::Graphics& g, const std::vector<float>& buffer, int sampleRate, juce::Rectangle<int> area)
{
    int totalBufferSamples = static_cast<int>(buffer.size());
    if (totalBufferSamples == 0 || sampleRate <= 0) return;
    
    int width = area.getWidth();
    int height = area.getHeight();
    float centerY = area.getY() + height / 2.0f;
    
    if (width <= 0) return;
    
    // 全長を描画する
    float samplesPerPixel = static_cast<float>(totalBufferSamples) / static_cast<float>(width);
    
    const float* data = buffer.data();
    
    juce::Path p;
    for (int x = 0; x < width; ++x)
    {
        int startSample = static_cast<int>(x * samplesPerPixel);
        int endSample = static_cast<int>((x + 1) * samplesPerPixel);
        
        // バッファ範囲外アクセスを防ぐ
        startSample = juce::jlimit(0, std::max(0, totalBufferSamples - 1), startSample);
        endSample = juce::jlimit(0, totalBufferSamples, endSample);
        
        float maxVal = 0.0f;
        for (int s = startSample; s < endSample; ++s)
        {
            float val = std::abs(data[s]);
            if (!std::isnan(val) && !std::isinf(val))
                maxVal = std::max(maxVal, val);
        }
        
        float barHeight = maxVal * (height / 2.0f) * 0.9f;
        if (std::isnan(barHeight) || std::isinf(barHeight)) barHeight = 0.0f;
        if (barHeight < 0.5f) barHeight = 0.5f;

        // 上下対称に描画
        p.addRectangle(static_cast<float>(area.getX() + x), centerY - barHeight, 1.0f, barHeight * 2.0f);
    }
    g.fillPath(p);
}

void TrackAreaComponent::paint (juce::Graphics& g)
{
    g.fillAll(UiTheme::windowBackground);
    
    if (audioEngine == nullptr) return;
    
    const auto& tracks = cachedTracks;
    
    double zoomLevel = audioEngine->getZoomLevel();
    int startY = getTrackStartY();

    // 上部の操作行は縦スクロールしても同じ位置に表示する。
    g.setColour(UiTheme::panelBackground);
    g.fillRect(0, 0, getWidth(), actionToolbarHeight);
    g.setColour(UiTheme::borderSoft);
    g.drawHorizontalLine(actionToolbarHeight - 1, 0.0f, static_cast<float>(getWidth()));

    // 固定表示のタイムルーラー。ローカル座標の原点を操作行の下へ移す。
    g.saveState();
    g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(actionToolbarHeight)));
    const int waveAreaX = leftMargin + sidebarWidth + 4;
    const int rulerRight = std::max(
        waveAreaX, getWidth() - leftMargin - scrollbarThickness);
    const auto rulerBounds = juce::Rectangle<int>(leftMargin, 0,
                                                   std::max(0, rulerRight - leftMargin),
                                                   timeRulerHeight);
    g.setColour(UiTheme::panelBackground);
    g.fillRoundedRectangle(rulerBounds.toFloat(), 8.0f);
    g.setColour(UiTheme::border);
    g.drawHorizontalLine(timeRulerHeight - 1, static_cast<float>(leftMargin),
                         static_cast<float>(rulerRight));
    g.setColour(UiTheme::textSecondary);
    g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));

    g.saveState();
    const auto timeRulerArea = juce::Rectangle<int>(waveAreaX, 0,
                                                     std::max(0, rulerRight - waveAreaX),
                                                     timeRulerHeight);
    g.reduceClipRegion(timeRulerArea);

    const double desiredMajorSeconds = 72.0 / std::max(1.0, zoomLevel);
    const double rulerIntervals[] = { 0.25, 0.5, 1.0, 2.0, 5.0, 10.0,
                                      15.0, 30.0, 60.0, 120.0, 300.0 };
    double majorInterval = rulerIntervals[std::size(rulerIntervals) - 1];
    for (const double interval : rulerIntervals)
    {
        if (interval >= desiredMajorSeconds)
        {
            majorInterval = interval;
            break;
        }
    }

    const double visibleStartTime = scrollX / std::max(1.0, zoomLevel);
    const double visibleEndTime = visibleStartTime
                                  + static_cast<double>(timeRulerArea.getWidth())
                                        / std::max(1.0, zoomLevel);
    const double firstMajorTime = std::floor(visibleStartTime / majorInterval) * majorInterval;

    for (double time = firstMajorTime; time <= visibleEndTime + majorInterval; time += majorInterval)
    {
        const int x = waveAreaX + static_cast<int>(std::round(time * zoomLevel - scrollX));
        if (x < timeRulerArea.getX() - 1)
            continue;

        g.setColour(UiTheme::border);
        g.drawVerticalLine(x, 19.0f, static_cast<float>(timeRulerHeight));

        juce::String label;
        if (majorInterval < 1.0)
            label = juce::String(time, 2) + "s";
        else if (time < 60.0)
            label = juce::String(time, majorInterval < 10.0 ? 1 : 0) + "s";
        else
            label = juce::String::formatted("%d:%02d",
                                            static_cast<int>(time) / 60,
                                            static_cast<int>(time) % 60);

        g.setColour(UiTheme::textSecondary);
        g.drawText(label, x + 4, 1, 66, 17, juce::Justification::centredLeft, false);

        const double minorInterval = majorInterval / 4.0;
        for (int minor = 1; minor < 4; ++minor)
        {
            const int minorX = waveAreaX
                               + static_cast<int>(std::round((time + minor * minorInterval)
                                                             * zoomLevel - scrollX));
            g.setColour(UiTheme::borderSoft);
            g.drawVerticalLine(minorX, 24.0f, static_cast<float>(timeRulerHeight));
        }
    }

    // 小節番号もテンポマップと変拍子へ追従させる。
    const TempoMap rulerTempoMap(audioEngine->getProjectDawSettings().tempoMap);
    g.setFont(12.0f);
    for (const double beatTime : rulerTempoMap.getBeatPositions(
             visibleStartTime, visibleEndTime))
    {
        const auto point = rulerTempoMap.getPointAt(beatTime);
        const double localBeatDuration = (60.0 / point.bpm)
                                       * (4.0 / point.denominator);
        const auto beatPosition = rulerTempoMap.getBeatPosition(beatTime);
        if (beatPosition.beatInBar != 0
            || localBeatDuration * point.numerator * zoomLevel < 56.0)
            continue;
        const int x = waveAreaX
                    + juce::roundToInt(beatTime * zoomLevel - scrollX);
        g.setColour(UiTheme::accent);
        g.fillRect(x, 17, 2, timeRulerHeight - 17);
        g.drawText(juce::String(juce::CharPointer_UTF8(u8"小節"))
                       + juce::String(beatPosition.barIndex + 1),
                   x + 3, 24, 58, 18,
                   juce::Justification::centredLeft, false);
    }

    const int rulerPlayheadX = waveAreaX
                               + static_cast<int>(std::round(audioEngine->getCurrentTime()
                                                             * zoomLevel - scrollX));
    if (timeRulerArea.contains(rulerPlayheadX, timeRulerHeight - 1))
    {
        g.setColour(UiTheme::textPrimary);
        g.fillRect(rulerPlayheadX, 16, 2, timeRulerHeight - 16);
    }

    const auto loopSettings = audioEngine->getProjectDawSettings();
    const auto drawLoopMarker = [&](double time, const juce::String& label)
    {
        const int x = waveAreaX
                    + juce::roundToInt(time * zoomLevel - scrollX);
        if (!timeRulerArea.contains(x, timeRulerHeight - 1))
            return;
        g.setColour(UiTheme::accent);
        g.fillRoundedRectangle(static_cast<float>(x - 8), 1.0f,
                               17.0f, 15.0f, 4.0f);
        g.setColour(UiTheme::textPrimary);
        g.setFont(12.0f);
        g.drawText(label, x - 8, 1, 17, 15,
                   juce::Justification::centred, false);
        g.setColour(UiTheme::accent);
        g.fillRect(x - 1, 15, 2, timeRulerHeight - 15);
    };
    if (loopSettings.hasLoopStart)
        drawLoopMarker(loopSettings.loopStartSeconds, "A");
    if (loopSettings.hasLoopEnd)
        drawLoopMarker(loopSettings.loopEndSeconds, "B");
    g.restoreState();

    g.restoreState();

    // スクロールするトラック一覧がルーラーや横スクロールバーへ重ならないようにする
    g.saveState();
    g.reduceClipRegion(juce::Rectangle<int>(0, rulerHeight, getWidth(),
                                             std::max(0, getViewportBottom() - rulerHeight)));
    
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        const auto& track = tracks[i];
        auto trackBounds = juce::Rectangle<int>(
            leftMargin,
            startY + static_cast<int>(i) * (trackHeight + trackGap),
            getWidth() - leftMargin * 2 - scrollbarThickness,
            trackHeight);

        if (trackBounds.getBottom() < rulerHeight || trackBounds.getY() > getViewportBottom())
            continue;
        
        // トラック背景
        g.setColour(UiTheme::panelBackground);
        g.fillRoundedRectangle(trackBounds.toFloat(), 12.0f);
        
        // サイドバー
        auto sidebar = trackBounds.removeFromLeft(sidebarWidth);
        g.setColour(UiTheme::raisedSurface);
        g.fillRoundedRectangle(sidebar.toFloat(), 12.0f);
        g.fillRect(sidebar.withTrimmedLeft(10)); // 角丸の繋ぎ目
        
        // --- メーターの描画（録音待機時のみ） ---
        if (track.isArmed)
        {
            float inputLevel = audioEngine->getCurrentInputLevel();
            inputLevel = juce::jlimit(0.0f, 1.0f, inputLevel); // 0.0 ~ 1.0
            
            auto meterBounds = sidebar.removeFromRight(6).reduced(0, 12); // 上下12px空ける
            
            // 背景（暗いグレー）
            g.setColour(UiTheme::panelBackground);
            g.fillRoundedRectangle(meterBounds.toFloat(), 2.0f);
            
            if (inputLevel > 0.0001f) // 小さな音にも反応するように閾値を下げる
            {
                // dBスケール(-60dB ~ 0dB) を 0.0 ~ 1.0 の高さにマッピング
                float db = 20.0f * std::log10(inputLevel);
                float displayLevel = (db + 60.0f) / 60.0f;
                displayLevel = juce::jlimit(0.0f, 1.0f, displayLevel);
                
                float meterHeight = meterBounds.getHeight() * displayLevel;
                auto levelBounds = meterBounds.withTrimmedTop(meterBounds.getHeight() - static_cast<int>(meterHeight));
                
                // 色（赤〜黄〜緑のグラデーション）
                juce::ColourGradient gradient(juce::Colour(0xffef4444), levelBounds.getX(), meterBounds.getY(),
                                              juce::Colour(0xff10b981), levelBounds.getX(), meterBounds.getBottom(), false);
                gradient.addColour(0.3, juce::Colour(0xfff59e0b)); // 黄色
                
                g.setGradientFill(gradient);
                g.fillRoundedRectangle(levelBounds.toFloat(), 2.0f);
            }
            
            // 少し余白を空ける
            sidebar.removeFromRight(4);
        }

        // 枠線
        g.setColour(static_cast<int>(i) == selectedTrackIndex ? UiTheme::accent
                       : track.isArmed ? UiTheme::success.withAlpha(0.40f)
                                       : UiTheme::border);
        auto fullBounds = juce::Rectangle<int>(
            leftMargin,
            startY + static_cast<int>(i) * (trackHeight + trackGap),
            getWidth() - leftMargin * 2 - scrollbarThickness,
            trackHeight);
        g.drawRoundedRectangle(fullBounds.toFloat(), 12.0f,
                               static_cast<int>(i) == selectedTrackIndex ? 2.0f : 1.0f);
        g.drawVerticalLine(sidebar.getRight() + (track.isArmed ? 10 : 0), sidebar.getY(), sidebar.getBottom());
        
        // --- サイドバーの中身 ---
        const auto header = getTrackHeaderLayout(static_cast<int>(i));
        g.setColour(track.color);
        g.fillEllipse(static_cast<float>(header.title.getX()),
                      static_cast<float>(header.title.getCentreY() - 4), 8.0f, 8.0f);
        g.setColour(UiTheme::textPrimary);
        g.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
        g.drawText(track.name, header.title.withTrimmedLeft(16), juce::Justification::centredLeft);

        const auto drawButton = [&g](juce::Rectangle<int> area, const juce::String& text,
                                     juce::Colour background, juce::Colour foreground)
        {
            g.setColour(background);
            g.fillRoundedRectangle(area.toFloat(), 6.0f);
            g.setColour(foreground);
            g.setFont(15.0f);
            g.drawText(text, area, juce::Justification::centred);
        };
        drawButton(header.mute, juce::CharPointer_UTF8(u8"ミュート"),
                   track.isMuted ? UiTheme::danger.withAlpha(0.22f) : UiTheme::controlSurface,
                   track.isMuted ? UiTheme::danger : UiTheme::textPrimary);
        drawButton(header.solo, juce::CharPointer_UTF8(u8"ソロ"),
                   track.isSolo ? UiTheme::warning.withAlpha(0.22f) : UiTheme::controlSurface,
                   track.isSolo ? UiTheme::warning : UiTheme::textPrimary);
        drawButton(header.arm, juce::CharPointer_UTF8(u8"● 録音待機"),
                   track.isArmed ? UiTheme::danger : UiTheme::controlSurface,
                   UiTheme::textPrimary);
        const bool hasEffects = track.vst3Name.isNotEmpty() || !track.effectSlots.empty();
        drawButton(header.effects, "FX",
                   hasEffects ? (track.vst3Bypassed ? UiTheme::warning.darker(0.25f)
                                                   : UiTheme::accent) : UiTheme::controlSurface,
                   UiTheme::textPrimary);
        drawButton(header.remove, juce::CharPointer_UTF8(u8"削除"),
                   UiTheme::controlSurface, UiTheme::textSecondary);

        // --- 波形エリア ---
        auto waveformArea = trackBounds.reduced(4, 8);
        if (!waveformArea.intersects(g.getClipBounds()))
            continue;
        
        g.saveState();
        g.reduceClipRegion(waveformArea);
        
        // テンポマップと変拍子に追従する拍グリッド。
        const TempoMap visibleTempoMap(
            audioEngine->getProjectDawSettings().tempoMap);
        for (const double beatTime : visibleTempoMap.getBeatPositions(
                 visibleStartTime, visibleEndTime))
        {
            const auto tempoPoint = visibleTempoMap.getPointAt(beatTime);
            const double localBeatDuration = (60.0 / tempoPoint.bpm)
                                           * (4.0 / tempoPoint.denominator);
            if (localBeatDuration * zoomLevel <= 5.0)
                continue;
            const bool isBar = visibleTempoMap.getBeatPosition(beatTime)
                                   .beatInBar == 0;
            const float xPos = waveformArea.getX()
                + static_cast<float>(beatTime * zoomLevel - scrollX);
            g.setColour(isBar ? UiTheme::border.withAlpha(0.75f)
                              : UiTheme::borderSoft.withAlpha(0.75f));
            if (isBar)
                g.fillRect(xPos, static_cast<float>(waveformArea.getY()),
                           1.5f, static_cast<float>(waveformArea.getHeight()));
            else
                g.drawVerticalLine(juce::roundToInt(xPos),
                                   static_cast<float>(waveformArea.getY()),
                                   static_cast<float>(waveformArea.getBottom()));
        }

        if (track.clips.empty()
            && audioEngine->getPlaybackState() != PlaybackState::Recording)
        {
            auto promptArea = waveformArea.reduced(28, 18);
            g.setColour(UiTheme::borderSoft);
            g.drawRoundedRectangle(promptArea.toFloat(), 9.0f, 1.0f);
            g.setColour(UiTheme::textSecondary);
            g.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
            g.drawText(
                juce::CharPointer_UTF8(u8"録音を開始するか、音声ファイルを読み込んでください"),
                promptArea, juce::Justification::centred, false);
        }

        const auto projectSettings = audioEngine->getProjectDawSettings();
        if (projectSettings.hasLoopStart && projectSettings.hasLoopEnd
            && projectSettings.loopEndSeconds > projectSettings.loopStartSeconds)
        {
            const int rangeX = waveformArea.getX()
                + juce::roundToInt(projectSettings.loopStartSeconds * zoomLevel)
                - static_cast<int>(scrollX);
            const int rangeRight = waveformArea.getX()
                + juce::roundToInt(projectSettings.loopEndSeconds * zoomLevel)
                - static_cast<int>(scrollX);
            g.setColour(UiTheme::accent.withAlpha(
                projectSettings.loopEnabled ? 0.13f : 0.07f));
            g.fillRect(rangeX, waveformArea.getY(),
                       std::max(1, rangeRight - rangeX), waveformArea.getHeight());
            g.setColour(UiTheme::accent.withAlpha(0.85f));
            g.fillRect(rangeX, waveformArea.getY(), 2, waveformArea.getHeight());
            g.fillRect(rangeRight - 2, waveformArea.getY(), 2,
                       waveformArea.getHeight());
        }
        
        // 各クリップの波形を描画（横スクロール対応）
        juce::String selectedId = audioEngine->getSelectedClipId();
        
        for (const auto& clip : track.clips)
        {
            if (clip.buffer == nullptr) continue;
            
            const double trackSpeed = juce::jlimit(0.75,
                                                  1.5,
                                                  track.playbackSpeed);
            int clipX = waveformArea.getX()
                        + static_cast<int>((clip.startTime / trackSpeed) * zoomLevel)
                        - static_cast<int>(scrollX);
            int clipW = static_cast<int>((clip.duration / trackSpeed) * zoomLevel);
            
            if (clipW <= 0) continue;
            if (clipX > waveformArea.getRight() || clipX + clipW < waveformArea.getX()) continue;
            
            auto clipRect = juce::Rectangle<int>(clipX, waveformArea.getY(), clipW, waveformArea.getHeight());
            auto clipBounds = clipRect.toFloat().reduced(1.0f, 2.0f); // 左右上下に少し隙間を空けてブロック感を出す
            if (clipBounds.getWidth() < 1.0f) clipBounds.setWidth(1.0f);
            if (clipBounds.getHeight() < 1.0f) clipBounds.setHeight(1.0f);
            
            // 波形の背景と枠線（選択状態なら強調）
            if (clip.id == selectedId)
            {
                g.setColour(track.color.withAlpha(0.4f));
                g.fillRoundedRectangle(clipBounds, 4.0f);
                g.setColour(UiTheme::textPrimary);
                g.drawRoundedRectangle(clipBounds, 4.0f, 2.0f);
            }
            else
            {
                g.setColour(track.color.withAlpha(0.2f));
                g.fillRoundedRectangle(clipBounds, 4.0f);
                g.setColour(track.color.withAlpha(0.7f)); // はっきりした枠線
                g.drawRoundedRectangle(clipBounds, 4.0f, 1.5f);
            }
            
            g.setColour(track.color.withAlpha(0.8f));
            drawWaveform(g, clip, clipBounds.toNearestInt());

            const float fadeInWidth = static_cast<float>(
                clip.fadeInSeconds / trackSpeed * zoomLevel);
            const float fadeOutWidth = static_cast<float>(
                clip.fadeOutSeconds / trackSpeed * zoomLevel);
            g.setColour(UiTheme::textPrimary.withAlpha(0.80f));
            if (fadeInWidth > 0.5f)
                g.drawLine(clipBounds.getX(), clipBounds.getBottom(),
                           clipBounds.getX() + fadeInWidth, clipBounds.getY(), 1.5f);
            if (fadeOutWidth > 0.5f)
                g.drawLine(clipBounds.getRight() - fadeOutWidth, clipBounds.getY(),
                           clipBounds.getRight(), clipBounds.getBottom(), 1.5f);

            for (const auto& marker : clip.rhythmMarkers)
            {
                const float markerX = clipBounds.getX()
                    + static_cast<float>(marker.targetTime / trackSpeed * zoomLevel);
                if (markerX >= clipBounds.getX() && markerX <= clipBounds.getRight())
                {
                    g.setColour(UiTheme::warning.withAlpha(0.75f));
                    g.drawVerticalLine(juce::roundToInt(markerX),
                                       clipBounds.getY() + 5.0f,
                                       clipBounds.getBottom() - 5.0f);
                    if (marker.hasDuration())
                    {
                        const float endX = juce::jlimit(
                            clipBounds.getX(), clipBounds.getRight(),
                            clipBounds.getX()
                                + static_cast<float>(marker.targetEndTime
                                                     / trackSpeed * zoomLevel));
                        const float noteY = clipBounds.getY() + 22.0f;
                        const float noteWidth = std::max(8.0f, endX - markerX);
                        g.setColour(UiTheme::warning.withAlpha(0.28f));
                        g.fillRoundedRectangle(markerX, noteY, noteWidth, 20.0f, 3.0f);
                        g.setColour(UiTheme::warning);
                        g.drawRoundedRectangle(markerX, noteY, noteWidth, 20.0f,
                                               3.0f, 1.2f);
                        g.fillRect(markerX - 2.0f, noteY, 4.0f, 20.0f);
                        g.fillRect(markerX + noteWidth - 2.0f,
                                   noteY, 4.0f, 20.0f);
                    }
                }
            }

            if (clip.id == selectedId)
            {
                const float handleY = clipBounds.getY() + 5.0f;
                g.setColour(UiTheme::textPrimary);
                g.fillRoundedRectangle(clipBounds.getX(),
                                       clipBounds.getCentreY() - 12.0f,
                                       4.0f, 24.0f, 2.0f);
                g.fillRoundedRectangle(clipBounds.getRight() - 4.0f,
                                       clipBounds.getCentreY() - 12.0f,
                                       4.0f, 24.0f, 2.0f);
                g.fillEllipse(clipBounds.getX() + fadeInWidth - 4.0f,
                              handleY - 4.0f, 8.0f, 8.0f);
                g.fillEllipse(clipBounds.getRight() - fadeOutWidth - 4.0f,
                              handleY - 4.0f, 8.0f, 8.0f);
            }
        }

        // 保存済みの範囲FXを、そのトラックだけに薄い帯で表示する。
        for (const auto& region : track.specialFxRegions)
        {
            if (!region.enabled || region.type == SpecialFxType::none
                || region.endSeconds <= region.startSeconds)
                continue;
            const int fxX = waveformArea.getX()
                + juce::roundToInt(region.startSeconds * zoomLevel)
                - static_cast<int>(scrollX);
            const int fxRight = waveformArea.getX()
                + juce::roundToInt(region.endSeconds * zoomLevel)
                - static_cast<int>(scrollX);
            auto fxBounds = juce::Rectangle<int>(
                fxX, waveformArea.getY(), std::max(1, fxRight - fxX),
                waveformArea.getHeight()).getIntersection(waveformArea);
            if (fxBounds.isEmpty())
                continue;
            g.setColour(UiTheme::warning.withAlpha(0.12f));
            g.fillRect(fxBounds);
            g.setColour(UiTheme::warning.withAlpha(0.90f));
            g.drawRect(fxBounds, 1);
            if (fxBounds.getWidth() >= 26)
            {
                g.setFont(juce::Font(juce::FontOptions(14.0f,
                                                       juce::Font::bold)));
                g.drawText("FX", fxBounds.removeFromTop(18).reduced(4, 0),
                           juce::Justification::centredLeft, false);
            }
        }
        
        // 録音中のリアルタイム波形の描画
        if (audioEngine->getPlaybackState() == PlaybackState::Recording && 
            audioEngine->getRecordingTrackIndex() == static_cast<int>(i))
        {
            const auto recBuffer = audioEngine->getRecordingBufferSnapshot();
            const int64_t recSamples = audioEngine->getRecordingSampleCount();
            
            if (recSamples > 0)
            {
                double recStartTime = audioEngine->getRecordingStartTime();
                int sampleRate = static_cast<int>(audioEngine->getSampleRate());
                double recDuration = static_cast<double>(recSamples) / sampleRate;
                
                int clipX = waveformArea.getX() + static_cast<int>(recStartTime * zoomLevel) - static_cast<int>(scrollX);
                int clipW = static_cast<int>(recDuration * zoomLevel);
                
                // 表示幅が最低1pxは確保されるようにする
                if (clipW < 1) clipW = 1;
                
                if (clipX <= waveformArea.getRight() && clipX + clipW >= waveformArea.getX())
                {
                    auto clipRect = juce::Rectangle<int>(clipX, waveformArea.getY(), clipW, waveformArea.getHeight());
                    auto clipBounds = clipRect.toFloat().reduced(1.0f, 2.0f);
                    if (clipBounds.getWidth() < 1.0f) clipBounds.setWidth(1.0f);
                    if (clipBounds.getHeight() < 1.0f) clipBounds.setHeight(1.0f);
                    
                    // 録音中はトラック色で背景と枠線を描画（ユーザー要望：「最初からトラック固有の色でいいよ！」）
                    g.setColour(track.color.withAlpha(0.3f));
                    g.fillRoundedRectangle(clipBounds, 4.0f);
                    g.setColour(track.color.withAlpha(0.9f));
                    g.drawRoundedRectangle(clipBounds, 4.0f, 1.5f);
                    
                    g.setColour(track.color.withAlpha(0.8f));
                    drawRecordingWaveform(g, recBuffer, sampleRate, clipBounds.toNearestInt());
                }
            }
        }
        
        // プレイヘッド（再生位置カーソル）
        if (audioEngine->getPlaybackState() != PlaybackState::Stopped || audioEngine->getCurrentTime() > 0.0)
        {
            int playX = waveformArea.getX() + static_cast<int>(audioEngine->getCurrentTime() * zoomLevel) - static_cast<int>(scrollX);
            
            if (playX >= waveformArea.getX() && playX <= waveformArea.getRight())
            {
                g.setColour(UiTheme::textPrimary);
                g.fillRect(playX, waveformArea.getY(), 2, waveformArea.getHeight());
            }
        }
        
        g.restoreState();
    }

    g.restoreState();

    if (isFileDragOver)
    {
        const auto dropArea = getLocalBounds().reduced(32).toFloat();
        g.setColour(UiTheme::panelBackground.withAlpha(0.94f));
        g.fillRoundedRectangle(dropArea, 14.0f);
        g.setColour(UiTheme::accent);
        g.drawRoundedRectangle(dropArea, 14.0f, 2.0f);
        g.setColour(UiTheme::textPrimary);
        g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
        g.drawText(juce::CharPointer_UTF8(u8"音声ファイルをここにドロップ"),
                   dropArea.toNearestInt(), juce::Justification::centred);
    }
}

void TrackAreaComponent::resized()
{
    auto bounds = getLocalBounds();

    const int waveXBase = leftMargin + sidebarWidth + 4;
    horizontalScrollBar.setBounds(waveXBase,
                                  bounds.getBottom() - scrollbarThickness,
                                  std::max(0, bounds.getWidth() - waveXBase
                                                  - leftMargin - scrollbarThickness),
                                  scrollbarThickness);
    verticalScrollBar.setBounds(bounds.getRight() - scrollbarThickness,
                                rulerHeight,
                                scrollbarThickness,
                                std::max(0, bounds.getHeight() - rulerHeight - scrollbarThickness));
    auto actions = juce::Rectangle<int>(leftMargin, 4,
        std::max(0, getWidth() - leftMargin * 2), actionToolbarHeight - 8);
    loadFileBtn.setBounds(actions.removeFromLeft(172));
    actions.removeFromLeft(8);
    addTrackBtn.setBounds(actions.removeFromLeft(154));
    actions.removeFromLeft(16);
    studioBtn.setBounds(actions.removeFromLeft(114));
    actions.removeFromLeft(8);
    snapBtn.setBounds(actions.removeFromLeft(120));

    if (editingTrackIndex >= 0)
    {
        trackNameEditor.setBounds(getTrackHeaderLayout(editingTrackIndex).title.withTrimmedLeft(16));
    }

    if (!isUpdatingScrollbars)
        updateScrollbar();
}

void TrackAreaComponent::updateScrollbar()
{
    if (audioEngine == nullptr || isUpdatingScrollbars) return;

    const juce::ScopedValueSetter<bool> updatingGuard(isUpdatingScrollbars, true);
    
    double duration = getCachedDuration();
    if (duration < 60.0) duration = 60.0;
    
    double zoomLevel = audioEngine->getZoomLevel();
    double totalWidth = duration * zoomLevel;
    
    const int waveXBase = leftMargin + sidebarWidth + 4;
    double visibleWidth = getWidth() - waveXBase - leftMargin - scrollbarThickness;
    if (visibleWidth < 1.0) visibleWidth = 1.0;
    
    totalWidth += visibleWidth * 0.5; // 余白
    
    const double horizontalLimit = std::max(totalWidth, visibleWidth);
    scrollX = juce::jlimit(0.0, std::max(0.0, horizontalLimit - visibleWidth), scrollX);
    horizontalScrollBar.setRangeLimits(0.0, horizontalLimit, juce::dontSendNotification);
    horizontalScrollBar.setCurrentRange(scrollX, visibleWidth, juce::dontSendNotification);

    const double visibleHeight = std::max(1, getViewportBottom() - (rulerHeight + 8));
    const double totalHeight = std::max(visibleHeight, getVerticalContentHeight());
    const double previousScrollY = scrollY;
    scrollY = juce::jlimit(0.0, std::max(0.0, totalHeight - visibleHeight), scrollY);
    verticalScrollBar.setRangeLimits(0.0, totalHeight, juce::dontSendNotification);
    verticalScrollBar.setCurrentRange(scrollY, visibleHeight, juce::dontSendNotification);

    if (scrollY != previousScrollY)
        resized();
}

void TrackAreaComponent::scrollBarMoved(juce::ScrollBar* scrollBarThatHasMoved, double newRangeStart)
{
    if (scrollBarThatHasMoved == &horizontalScrollBar)
    {
        scrollX = newRangeStart;
        repaint();
    }
    else if (scrollBarThatHasMoved == &verticalScrollBar)
    {
        if (trackNameEditor.isBeingEdited())
        {
            trackNameEditor.hideEditor(false);
            trackNameEditor.setVisible(false);
        }

        scrollY = newRangeStart;
        resized();
        repaint();
    }
}

void TrackAreaComponent::autoScrollToPlayhead()
{
    if (audioEngine == nullptr) return;
    if (audioEngine->getPlaybackState() == PlaybackState::Stopped) return;
    
    double zoomLevel = audioEngine->getZoomLevel();
    double currentTime = audioEngine->getCurrentTime();
    
    const int waveXBase = leftMargin + sidebarWidth + 4;
    double visibleWidth = getWidth() - waveXBase - leftMargin - scrollbarThickness;
    if (visibleWidth < 1.0) return;
    
    double playheadX = currentTime * zoomLevel;
    
    // プレイヘッドが画面の右端を超えた場合（ページスクロールのように動かす）
    if (playheadX > scrollX + visibleWidth)
    {
        scrollX = std::max(0.0, playheadX - (visibleWidth * 0.1));
        horizontalScrollBar.setCurrentRangeStart(scrollX);
    }
    // プレイヘッドが画面の左端より左にある場合
    else if (playheadX < scrollX)
    {
        scrollX = std::max(0.0, playheadX - (visibleWidth * 0.1));
        horizontalScrollBar.setCurrentRangeStart(scrollX);
    }
}

void TrackAreaComponent::repaintInputMeters()
{
    const int meterX = leftMargin + sidebarWidth - 8;
    const int firstTrackY = getTrackStartY();

    for (size_t index = 0; index < cachedTracks.size(); ++index)
    {
        if (!cachedTracks[index].isArmed)
            continue;

        const int trackY = firstTrackY
                           + static_cast<int>(index) * (trackHeight + trackGap);
        if (trackY + trackHeight < rulerHeight || trackY >= getViewportBottom())
            continue;

        repaint(meterX, trackY + 8, 12, trackHeight - 16);
    }
}
