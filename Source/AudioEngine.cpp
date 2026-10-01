#include "AudioEngine.h"
#include "RegionAutomation.h"
#include "AraTrackHost.h"
#include "ClipRenderMath.h"
#include "PitchNetArchiveFilter.h"
#include "RhythmRender.h"
#include "RhythmWarp.h"
#include "SimpleMixProcessor.h"
#include "WindowsMp3Encoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
juce::String utf8(const char* text)
{
    return juce::String::fromUTF8(text);
}

juce::String makeTrackId()
{
    return "track-" + juce::Uuid().toString();
}

juce::String makeClipId()
{
    return "clip-" + juce::Uuid().toString();
}

struct OfflinePluginExpectation
{
    // Empty only for the dedicated main VST3/ARA slot.
    juce::String effectSlotId;
    juce::String displayName;
};

bool buildOfflinePluginExpectations(
    const TrackData& track,
    std::vector<OfflinePluginExpectation>& expectations,
    juce::String& errorMessage)
{
    expectations.clear();
    errorMessage.clear();

    const auto trackName = track.name.isNotEmpty()
        ? track.name : utf8(u8"名称未設定");
    const bool hasMainPluginMetadata = track.vst3Name.isNotEmpty()
        || track.vst3DescriptionXml.isNotEmpty()
        || track.vst3State != nullptr
        || track.vst3AraArchive != nullptr;
    if (hasMainPluginMetadata && !track.vst3Bypassed)
    {
        expectations.push_back({ {},
                                 track.vst3Name.isNotEmpty()
                                     ? track.vst3Name
                                     : utf8(u8"メインVST3") });
    }

    if (track.effectChainBypassed)
        return true;

    for (const auto& effect : track.effectSlots)
    {
        if (effect.bypassed)
            continue;
        if (effect.id.isEmpty())
        {
            errorMessage = utf8(u8"トラック「") + trackName
                + utf8(u8"」のVST3エフェクト情報が不完全です。VST3を読み込み直してください。");
            return false;
        }
        if (std::any_of(expectations.begin(), expectations.end(),
                        [&effect](const OfflinePluginExpectation& existing)
                        {
                            return existing.effectSlotId == effect.id;
                        }))
        {
            errorMessage = utf8(u8"トラック「") + trackName
                + utf8(u8"」に同じVST3エフェクト情報が重複しています。VST3を読み込み直してください。");
            return false;
        }
        expectations.push_back({ effect.id,
                                 effect.name.isNotEmpty()
                                     ? effect.name
                                     : utf8(u8"VST3エフェクト") });
    }
    return true;
}

void delayBufferInPlace(juce::AudioBuffer<float>& buffer,
                        int delaySamples) noexcept
{
    const int samples = buffer.getNumSamples();
    const int delay = std::clamp(delaySamples, 0, samples);
    if (delay <= 0)
        return;
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        auto* data = buffer.getWritePointer(channel);
        if (delay < samples)
        {
            std::memmove(data + delay,
                         data,
                         static_cast<size_t>(samples - delay) * sizeof(float));
        }
        juce::FloatVectorOperations::clear(data, delay);
    }
}

bool processSpecialFxOffline(
    const TrackData& track,
    juce::AudioBuffer<float>& buffer,
    double sampleRate,
    const std::function<bool()>& shouldCancel = {},
    int64_t timelineOffsetSamples = 0)
{
    constexpr int processingChunk = 8192;
    for (const auto& region : track.specialFxRegions)
    {
        if (shouldCancel && shouldCancel())
            return false;
        SpecialFxProcessor processor;
        if (!processor.prepare(sampleRate, processingChunk,
                               std::min(2, buffer.getNumChannels())))
            return false;
        SpecialFxProcessor::Settings settings;
        settings.enabled = region.enabled;
        settings.type = region.type;
        settings.amount = region.amount;
        settings.wet = region.wet;
        settings.limitToRegion = true;
        settings.regionStartSample = static_cast<int64_t>(std::llround(
            std::max(0.0, region.startSeconds) * sampleRate));
        settings.regionEndSampleExclusive = static_cast<int64_t>(std::llround(
            std::max(region.startSeconds, region.endSeconds) * sampleRate));
        settings.fadeSamples = juce::roundToInt(
            juce::jlimit(0.0, 10.0, region.fadeSeconds) * sampleRate);
        settings.noiseSeed = static_cast<std::uint32_t>(region.id.hashCode())
                           ^ 0x53a9b4d1u;
        processor.setSettings(settings);
        for (int position = 0; position < buffer.getNumSamples();
             position += processingChunk)
        {
            if (shouldCancel && shouldCancel())
                return false;
            processor.process(buffer,
                              position,
                              std::min(processingChunk,
                                       buffer.getNumSamples() - position),
                              timelineOffsetSamples + position);
        }
    }
    for (const auto& region : track.automationRegions)
    {
        RegionAutomationProcessor processor;
        if (!processor.prepare(sampleRate, std::min(2, buffer.getNumChannels()), region))
            return false;
        for (int position = 0; position < buffer.getNumSamples(); position += processingChunk)
        {
            if (shouldCancel && shouldCancel()) return false;
            processor.process(buffer, position,
                              std::min(processingChunk, buffer.getNumSamples() - position),
                              timelineOffsetSamples + position);
        }
    }
    return true;
}

bool resampleClipInternal(AudioClip& clip,
                          double targetSampleRate,
                          const std::function<bool()>& shouldCancel = {})
{
    if (clip.buffer == nullptr || targetSampleRate <= 0.0)
        return false;

    const double sourceRate = clip.sampleRate > 0 ? static_cast<double>(clip.sampleRate)
                                                   : targetSampleRate;
    if (std::abs(sourceRate - targetSampleRate) < 0.5)
    {
        clip.sampleRate = static_cast<int>(std::llround(targetSampleRate));
        return false;
    }

    const int sourceSamples = clip.buffer->getNumSamples();
    const int channels = clip.buffer->getNumChannels();
    if (sourceSamples <= 0 || channels <= 0)
        return false;

    const int64_t targetSamples64 = std::max<int64_t>(1,
        static_cast<int64_t>(std::llround(static_cast<double>(sourceSamples)
                                          * targetSampleRate
                                          / sourceRate)));
    if (targetSamples64 > std::numeric_limits<int>::max())
        return false;

    if (shouldCancel && shouldCancel())
        return false;

    const int targetSamples = static_cast<int>(targetSamples64);
    auto converted = std::make_shared<juce::AudioBuffer<float>>(channels, targetSamples);
    const double sourcePerTarget = sourceRate / targetSampleRate;

    // Linear interpolation adds no filter latency, so transients keep their
    // exact timeline position after sample-rate conversion.
    for (int ch = 0; ch < channels; ++ch)
    {
        const float* source = clip.buffer->getReadPointer(ch);
        float* destination = converted->getWritePointer(ch);
        for (int i = 0; i < targetSamples; ++i)
        {
            if ((i & 0x3fff) == 0 && shouldCancel && shouldCancel())
                return false;

            const double sourcePosition = static_cast<double>(i) * sourcePerTarget;
            const int index0 = juce::jlimit(0, sourceSamples - 1,
                                            static_cast<int>(std::floor(sourcePosition)));
            const int index1 = std::min(index0 + 1, sourceSamples - 1);
            const float fraction = static_cast<float>(sourcePosition - std::floor(sourcePosition));
            destination[i] = source[index0] + (source[index1] - source[index0]) * fraction;
        }
    }

    clip.buffer = std::move(converted);
    clip.sampleRate = static_cast<int>(std::llround(targetSampleRate));
    clip.rhythmRenderCache.reset();
    return true;
}

int configurePluginLayout(juce::AudioPluginInstance& instance)
{
    if (instance.getBusCount(true) <= 0 || instance.getBusCount(false) <= 0)
        return 0;

    const auto tryLayout = [&instance](const juce::AudioChannelSet& channelSet)
    {
        auto layout = instance.getBusesLayout();
        for (int bus = 0; bus < instance.getBusCount(true); ++bus)
            layout.getChannelSet(true, bus) = bus == 0 ? channelSet
                                                        : juce::AudioChannelSet::disabled();
        for (int bus = 0; bus < instance.getBusCount(false); ++bus)
            layout.getChannelSet(false, bus) = bus == 0 ? channelSet
                                                         : juce::AudioChannelSet::disabled();
        return instance.setBusesLayout(layout);
    };

    int channelCount = 0;
    if (tryLayout(juce::AudioChannelSet::stereo()))
        channelCount = 2;
    else if (tryLayout(juce::AudioChannelSet::mono()))
        channelCount = 1;

    if (channelCount == 0
        || instance.getTotalNumInputChannels() != channelCount
        || instance.getTotalNumOutputChannels() != channelCount)
        return 0;

    return channelCount;
}

bool shouldDuplicateLeftOutput(const juce::PluginDescription& description)
{
    return description.hasARAExtension
        && description.name.containsIgnoreCase("PitchNet");
}

int preparePluginInstance(juce::AudioPluginInstance& instance,
                          double sampleRate,
                          int blockSize)
{
    const int channelCount = configurePluginLayout(instance);
    if (channelCount <= 0)
        return 0;

    instance.setRateAndBufferSizeDetails(sampleRate, blockSize);
    instance.prepareToPlay(sampleRate, blockSize);
    instance.reset();
    return channelCount;
}

class TrackPluginPlayHead final : public juce::AudioPlayHead
{
public:
    juce::Optional<PositionInfo> getPosition() const override
    {
        const auto rate = std::max(1.0, sampleRate.load());
        const auto samples = timeInSamples.load();
        const double seconds = static_cast<double>(samples) / rate;
        constexpr double bpm = 120.0;
        const double ppq = seconds * bpm / 60.0;

        PositionInfo position;
        position.setTimeInSamples(samples);
        position.setTimeInSeconds(seconds);
        position.setBpm(bpm);
        position.setTimeSignature(TimeSignature { 4, 4 });
        position.setPpqPosition(ppq);
        position.setPpqPositionOfLastBarStart(std::floor(ppq / 4.0) * 4.0);
        position.setBarCount(static_cast<int64_t>(std::floor(ppq / 4.0)));
        position.setIsPlaying(playing.load());
        position.setIsRecording(recording.load());
        return position;
    }

    void update(int64_t samples,
                double rate,
                PlaybackState state) noexcept
    {
        timeInSamples.store(std::max<int64_t>(0, samples));
        sampleRate.store(std::max(1.0, rate));
        const bool isTransportRunning = state != PlaybackState::Stopped;
        playing.store(isTransportRunning);
        recording.store(state == PlaybackState::Recording);
    }

private:
    std::atomic<int64_t> timeInSamples { 0 };
    std::atomic<double> sampleRate { 44100.0 };
    std::atomic<bool> playing { false };
    std::atomic<bool> recording { false };
};

class PluginEditorWindow final : public juce::DocumentWindow,
                                 private juce::Timer
{
public:
    PluginEditorWindow(const juce::String& title,
                       juce::AudioProcessorEditor* editor,
                       std::function<void()> closeCallbackToUse,
                       std::function<void()> togglePlaybackCallbackToUse,
                       std::function<void(int)> movePlaybackCallbackToUse)
        : juce::DocumentWindow(title,
                               juce::Colour(0xff18181b),
                               juce::DocumentWindow::closeButton),
          closeCallback(std::move(closeCallbackToUse)),
          togglePlaybackCallback(std::move(togglePlaybackCallbackToUse)),
          movePlaybackCallback(std::move(movePlaybackCallbackToUse))
    {
        jassert(editor != nullptr);
        setUsingNativeTitleBar(true);
        setResizable(true, true);
        setContentOwned(editor, true);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    ~PluginEditorWindow() override
    {
        stopTimer();
        clearContentComponent();
    }

    void closeButtonPressed() override
    {
        if (closeCallback)
            closeCallback();
    }

private:
    void visibilityChanged() override
    {
        spaceWasDown = juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey);
        leftWasDown = juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::leftKey);
        rightWasDown = juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::rightKey);

        if (isVisible())
            startTimerHz(60);
        else
            stopTimer();
    }

    void timerCallback() override
    {
        const bool spaceIsDown =
            juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey);
        const bool leftIsDown =
            juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::leftKey);
        const bool rightIsDown =
            juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::rightKey);

        if (isActiveWindow())
        {
            if (spaceIsDown && !spaceWasDown && togglePlaybackCallback)
                togglePlaybackCallback();
            if (leftIsDown && !leftWasDown && movePlaybackCallback)
                movePlaybackCallback(-1);
            if (rightIsDown && !rightWasDown && movePlaybackCallback)
                movePlaybackCallback(1);
        }

        spaceWasDown = spaceIsDown;
        leftWasDown = leftIsDown;
        rightWasDown = rightIsDown;
    }

    std::function<void()> closeCallback;
    std::function<void()> togglePlaybackCallback;
    std::function<void(int)> movePlaybackCallback;
    bool spaceWasDown = false;
    bool leftWasDown = false;
    bool rightWasDown = false;
};
}

struct AudioEngine::ImportBatchResult
{
    struct Item
    {
        juce::String trackName;
        AudioClip clip;
    };

    std::uint64_t generation = 0;
    int requestedCount = 0;
    std::vector<Item> importedItems;
    juce::StringArray failedFiles;
};

struct AudioEngine::TrackPluginSlot final : public juce::AudioProcessorListener
{
    TrackPluginSlot(AudioEngine& engine,
                    juce::String id,
                    juce::String normalEffectSlotId,
                    juce::PluginDescription pluginDescription,
                    std::shared_ptr<juce::AudioPluginInstance> pluginInstance,
                    std::shared_ptr<juce::AudioPluginInstance> audioProcessingInstance,
                     int channels,
                     bool processingUsesAraDocument,
                     bool araPlaybackIsEnabled,
                     bool shouldBeBypassed,
                     std::shared_ptr<const juce::MemoryBlock> savedState,
                     std::shared_ptr<const juce::MemoryBlock> savedAraArchive,
                     juce::String savedAraArchiveId,
                     bool savedAraDataIsCurrent)
        : owner(engine),
          trackId(std::move(id)),
          effectSlotId(std::move(normalEffectSlotId)),
          description(std::move(pluginDescription)),
          instance(std::move(pluginInstance)),
          processingInstance(std::move(audioProcessingInstance)),
          persistedState(std::move(savedState)),
          persistedAraArchive(std::move(savedAraArchive)),
          persistedAraArchiveId(std::move(savedAraArchiveId)),
          editorChannels(channels),
          processingChannels(channels),
          processingUsesAra(processingUsesAraDocument),
          araPlaybackEnabled(araPlaybackIsEnabled),
          bypassed(shouldBeBypassed)
    {
        if (persistedState != nullptr
            && (!description.hasARAExtension || savedAraDataIsCurrent))
            capturedStateRevision = stateRevision.load();
        if (!araPlaybackEnabled.load()
            || (persistedAraArchive != nullptr
                && !persistedAraArchive->isEmpty()
                && savedAraDataIsCurrent))
            capturedAraRevision = araRevision.load();
        duplicateLeftOutput = shouldDuplicateLeftOutput(description);
        if (instance != nullptr)
        {
            instance->setPlayHead(&playHead);
            instance->addListener(this);
        }
        if (processingInstance != nullptr && processingInstance != instance)
            processingInstance->setPlayHead(&playHead);
    }

    ~TrackPluginSlot() override
    {
        if (instance != nullptr)
        {
            instance->removeListener(this);
            instance->releaseResources();
            instance->setPlayHead(nullptr);
        }
        if (processingInstance != nullptr && processingInstance != instance)
        {
            processingInstance->releaseResources();
            processingInstance->setPlayHead(nullptr);
        }
    }

    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override
    {
        if (suppressStateChangeNotifications.load())
            return;
        processingStateDirty.store(true);
        stateRevision.fetch_add(1);
        owner.markDirty();
    }

    void audioProcessorChanged(juce::AudioProcessor*,
                               const juce::AudioProcessorListener::ChangeDetails&) override
    {
        if (suppressStateChangeNotifications.load())
            return;
        processingStateDirty.store(true);
        stateRevision.fetch_add(1);
        owner.requestPluginLatencyRefresh();
        owner.markDirty();
    }

    AudioEngine& owner;
    juce::String trackId;
    // Empty for the dedicated PitchNet/ARA slot. Normal VST3 effects use the
    // stable ID stored in TrackData::effectSlots.
    juce::String effectSlotId;
    juce::PluginDescription description;
    TrackPluginPlayHead playHead;
    std::shared_ptr<juce::AudioPluginInstance> instance;
    // ARA editing and timeline playback share one document controller while
    // using separate renderer instances. This also keeps legacy ARA states out
    // of the much slower conventional real-time processing path.
    std::shared_ptr<juce::AudioPluginInstance> processingInstance;
    std::unique_ptr<AraTrackHost> araHost;
    std::atomic<bool> processingStateDirty { false };
    std::atomic<bool> araEditorWasOpened { false };
    std::atomic<bool> auditionWithEditorInstance { false };
    std::atomic<bool> useEditorInstanceForPlayback { false };
    std::atomic<bool> editorTransportActive { false };
    std::atomic<bool> suppressStateChangeNotifications { false };
    std::atomic<std::uint64_t> stateRevision { 1 };
    std::atomic<std::uint64_t> araRevision { 1 };
    std::uint64_t capturedStateRevision = 0;
    std::uint64_t capturedAraRevision = 0;
    std::shared_ptr<const juce::MemoryBlock> persistedState;
    std::shared_ptr<const juce::MemoryBlock> persistedAraArchive;
    juce::String persistedAraArchiveId;
    int editorChannels = 2;
    int processingChannels = 2;
    std::atomic<int> latencySamples { 0 };
    bool processingUsesAra = false;
    bool duplicateLeftOutput = false;
    std::atomic<bool> araPlaybackEnabled { false };
    std::atomic<bool> bypassed { false };
    std::atomic<bool> effectChainBypassed { false };
};

struct AudioEngine::TrackPlaybackSlot
{
    TrackPlaybackSlot(juce::String id, double sampleRate)
        : trackId(std::move(id))
    {
        transform.prepare(2, sampleRate);
        simpleMix.prepare(sampleRate, 8192, 2);
        preparedSpecialFxSampleRate = sampleRate;
        prepareLatencyCompensation(sampleRate);
    }

    void syncSpecialFx(const std::vector<SpecialFxRegion>& regions,
                       double sampleRate)
    {
        if (specialFxMetadata == regions
            && std::abs(preparedSpecialFxSampleRate - sampleRate) < 1.0e-6)
            return;

        specialFxMetadata = regions;
        preparedSpecialFxSampleRate = sampleRate;
        specialFxProcessors.clear();
        specialFxProcessors.reserve(regions.size());
        for (const auto& region : regions)
        {
            auto processor = std::make_unique<SpecialFxProcessor>();
            if (!processor->prepare(sampleRate, 8192, 2))
                continue;
            SpecialFxProcessor::Settings settings;
            settings.enabled = region.enabled;
            settings.type = region.type;
            settings.amount = region.amount;
            settings.wet = region.wet;
            settings.limitToRegion = true;
            settings.regionStartSample = static_cast<int64_t>(std::llround(
                std::max(0.0, region.startSeconds) * sampleRate));
            settings.regionEndSampleExclusive = static_cast<int64_t>(std::llround(
                std::max(region.startSeconds, region.endSeconds) * sampleRate));
            settings.fadeSamples = juce::roundToInt(
                juce::jlimit(0.0, 10.0, region.fadeSeconds) * sampleRate);
            settings.noiseSeed = static_cast<std::uint32_t>(region.id.hashCode())
                               ^ 0x53a9b4d1u;
            processor->setSettings(settings);
            specialFxProcessors.push_back(std::move(processor));
        }
    }

    void syncAutomation(const std::vector<AudioAutomationRegion>& regions, double sampleRate)
    {
        if (automationMetadata == regions && automationSampleRate == sampleRate) return;
        automationMetadata = regions;
        automationSampleRate = sampleRate;
        automationProcessors.clear();
        for (const auto& region : regions)
        {
            auto processor = std::make_unique<RegionAutomationProcessor>();
            if (processor->prepare(sampleRate, 2, region))
                automationProcessors.push_back(std::move(processor));
        }
    }

    void processSpecialFx(juce::AudioBuffer<float>& buffer,
                          int numSamples,
                          int64_t timelineStartSample) noexcept
    {
        for (auto& processor : specialFxProcessors)
            if (processor != nullptr)
                processor->process(buffer, 0, numSamples, timelineStartSample);
    }

    void prepareLatencyCompensation(double sampleRate)
    {
        const auto safeRate = std::clamp(sampleRate, 8000.0, 384000.0);
        maximumCompensationSamples = std::max(
            1, static_cast<int>(std::ceil(safeRate * 2.0)));
        latencyDelay.setSize(2,
                             maximumCompensationSamples + 1,
                             false,
                             false,
                             true);
        latencyDelay.clear();
        resetLatencyCompensation();
    }

    void resetLatencyCompensation() noexcept
    {
        latencyWritePosition = 0;
        latencyValidSamples = 0;
        activeCompensationSamples = -1;
    }

    void processLatencyCompensation(juce::AudioBuffer<float>& buffer,
                                    int numSamples,
                                    int requestedDelaySamples) noexcept
    {
        const int channels = std::min(2, buffer.getNumChannels());
        const int samples = std::clamp(numSamples, 0, buffer.getNumSamples());
        const int delay = std::clamp(requestedDelaySamples,
                                     0,
                                     maximumCompensationSamples);
        const int capacity = latencyDelay.getNumSamples();
        if (channels <= 0 || samples <= 0 || capacity <= 1)
            return;

        if (delay != activeCompensationSamples)
        {
            latencyWritePosition = 0;
            latencyValidSamples = 0;
            activeCompensationSamples = delay;
        }

        std::array<float*, 2> output {
            buffer.getWritePointer(0),
            channels > 1 ? buffer.getWritePointer(1) : nullptr
        };
        std::array<float*, 2> history {
            latencyDelay.getWritePointer(0),
            latencyDelay.getWritePointer(1)
        };

        for (int sample = 0; sample < samples; ++sample)
        {
            int readPosition = latencyWritePosition - delay;
            if (readPosition < 0)
                readPosition += capacity;

            for (int channel = 0; channel < channels; ++channel)
            {
                const auto input = output[static_cast<size_t>(channel)][sample];
                history[static_cast<size_t>(channel)][latencyWritePosition] = input;
                if (delay > 0)
                {
                    output[static_cast<size_t>(channel)][sample] =
                        latencyValidSamples >= delay
                            ? history[static_cast<size_t>(channel)][readPosition]
                            : 0.0f;
                }
            }

            if (++latencyWritePosition >= capacity)
                latencyWritePosition = 0;
            latencyValidSamples = std::min(maximumCompensationSamples,
                                           latencyValidSamples + 1);
        }
    }

    juce::String trackId;
    PlaybackTransform transform;
    int64_t transformInputPosition = 0;
    bool transformInputPositionValid = false;
    SimpleMixProcessor simpleMix;
    std::vector<AudioAutomationRegion> automationMetadata;
    std::vector<std::unique_ptr<RegionAutomationProcessor>> automationProcessors;
    double automationSampleRate = 0.0;
    std::vector<SpecialFxRegion> specialFxMetadata;
    std::vector<std::unique_ptr<SpecialFxProcessor>> specialFxProcessors;
    double preparedSpecialFxSampleRate = 0.0;
    std::atomic<int> chainLatencySamples { 0 };
    juce::AudioBuffer<float> latencyDelay;
    int maximumCompensationSamples = 1;
    int latencyWritePosition = 0;
    int latencyValidSamples = 0;
    int activeCompensationSamples = -1;
};

void AudioEngine::rebuildTrackPlaybackSlotsLocked()
{
    std::vector<std::unique_ptr<TrackPlaybackSlot>> updatedSlots;
    updatedSlots.reserve(tracks.size());

    for (const auto& track : tracks)
    {
        const auto existing = std::find_if(
            trackPlaybackSlots.begin(),
            trackPlaybackSlots.end(),
            [&track](const std::unique_ptr<TrackPlaybackSlot>& slot)
            {
                return slot != nullptr && slot->trackId == track.id;
            });

        if (existing != trackPlaybackSlots.end())
        {
            auto slot = std::move(*existing);
            slot->syncSpecialFx(track.specialFxRegions,
                                std::max(1.0, currentSampleRate.load()));
            slot->syncAutomation(track.automationRegions,
                                 std::max(1.0, currentSampleRate.load()));
            updatedSlots.push_back(std::move(slot));
        }
        else
        {
            auto slot = std::make_unique<TrackPlaybackSlot>(
                track.id, std::max(1.0, currentSampleRate.load()));
            slot->syncSpecialFx(track.specialFxRegions,
                                std::max(1.0, currentSampleRate.load()));
            slot->syncAutomation(track.automationRegions,
                                 std::max(1.0, currentSampleRate.load()));
            updatedSlots.push_back(std::move(slot));
        }
    }

    trackPlaybackSlots = std::move(updatedSlots);
}

void AudioEngine::resetTrackPlaybackTransformsLocked()
{
    for (auto& slot : trackPlaybackSlots)
        if (slot != nullptr)
        {
            slot->transform.reset();
            slot->transformInputPosition = 0;
            slot->transformInputPositionValid = false;
            slot->simpleMix.reset();
            for (auto& processor : slot->automationProcessors)
                processor->reset();
            for (auto& processor : slot->specialFxProcessors)
                if (processor != nullptr)
                    processor->reset();
            slot->resetLatencyCompensation();
        }
}

void AudioEngine::reorderTrackPluginSlotsLocked(
    const juce::String& trackId,
    const std::vector<juce::String>& effectOrder)
{
    std::vector<std::unique_ptr<TrackPluginSlot>> trackSlots;
    trackSlots.reserve(trackPlugins.size());
    for (auto& slot : trackPlugins)
        if (slot != nullptr && slot->trackId == trackId)
            trackSlots.push_back(std::move(slot));

    trackPlugins.erase(std::remove(trackPlugins.begin(),
                                   trackPlugins.end(),
                                   nullptr),
                       trackPlugins.end());
    if (trackSlots.empty())
        return;

    std::vector<std::unique_ptr<TrackPluginSlot>> ordered;
    ordered.reserve(trackSlots.size());
    const auto moveMatchingSlot = [&trackSlots, &ordered](
        const std::function<bool(const TrackPluginSlot&)>& predicate)
    {
        const auto match = std::find_if(
            trackSlots.begin(), trackSlots.end(),
            [&predicate](const std::unique_ptr<TrackPluginSlot>& slot)
            {
                return slot != nullptr && predicate(*slot);
            });
        if (match != trackSlots.end())
        {
            ordered.push_back(std::move(*match));
            trackSlots.erase(match);
        }
    };

    // PitchNet/ARA is always first and remains outside the normal effect list.
    moveMatchingSlot([](const TrackPluginSlot& slot)
    {
        return slot.effectSlotId.isEmpty();
    });
    for (const auto& effectId : effectOrder)
        moveMatchingSlot([&effectId](const TrackPluginSlot& slot)
        {
            return slot.effectSlotId == effectId;
        });

    // Keep an asynchronously loaded slot alive until metadata reconciliation;
    // it is never placed ahead of a persisted effect.
    for (auto& slot : trackSlots)
        if (slot != nullptr)
            ordered.push_back(std::move(slot));
    for (auto& slot : ordered)
        trackPlugins.push_back(std::move(slot));
}

void AudioEngine::requestPluginLatencyRefresh() noexcept
{
    pluginLatencyRefreshPending.store(true, std::memory_order_release);
    triggerAsyncUpdate();
}

void AudioEngine::refreshTrackPluginLatencies()
{
    struct RuntimeSlot
    {
        juce::String trackId;
        juce::String effectSlotId;
        int latencySamples = 0;
        double tailSeconds = 0.0;
        bool bypassed = false;
        bool chainBypassed = false;
    };

    std::vector<RuntimeSlot> runtimeSlots;
    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        runtimeSlots.reserve(trackPlugins.size());
        for (auto& slot : trackPlugins)
        {
            if (slot == nullptr)
                continue;
            const auto instance = slot->processingInstance != nullptr
                ? slot->processingInstance : slot->instance;
            const int latency = instance != nullptr
                ? juce::jlimit(0, 1920000, instance->getLatencySamples()) : 0;
            const double reportedTail = instance != nullptr
                ? instance->getTailLengthSeconds() : 0.0;
            const double tail = std::isfinite(reportedTail)
                ? juce::jlimit(0.0, 60.0, reportedTail) : 0.0;
            slot->latencySamples.store(latency, std::memory_order_relaxed);
            runtimeSlots.push_back({ slot->trackId,
                                     slot->effectSlotId,
                                     latency,
                                     tail,
                                     slot->bypassed.load(
                                         std::memory_order_relaxed),
                                     slot->effectChainBypassed.load(
                                         std::memory_order_relaxed) });
        }
    }

    {
        const juce::ScopedLock lock(tracksLock);
        for (auto& track : tracks)
        {
            int totalLatency = 0;
            for (const auto& runtime : runtimeSlots)
            {
                if (runtime.trackId != track.id)
                    continue;

                if (runtime.effectSlotId.isEmpty())
                {
                    track.vst3LatencySamples = runtime.latencySamples;
                    track.vst3TailSeconds = runtime.tailSeconds;
                }
                else
                {
                    const auto effect = std::find_if(
                        track.effectSlots.begin(), track.effectSlots.end(),
                        [&runtime](const EffectSlotData& candidate)
                        {
                            return candidate.id == runtime.effectSlotId;
                        });
                    if (effect != track.effectSlots.end())
                    {
                        effect->latencySamples = runtime.latencySamples;
                        effect->tailSeconds = runtime.tailSeconds;
                    }
                }

                if (!runtime.bypassed
                    && (runtime.effectSlotId.isEmpty()
                        || !runtime.chainBypassed))
                {
                    const auto remaining = 1920000 - totalLatency;
                    totalLatency += std::min(remaining,
                                             runtime.latencySamples);
                }
            }

            const auto playback = std::find_if(
                trackPlaybackSlots.begin(), trackPlaybackSlots.end(),
                [&track](const std::unique_ptr<TrackPlaybackSlot>& slot)
                {
                    return slot != nullptr && slot->trackId == track.id;
                });
            if (playback != trackPlaybackSlots.end())
                (*playback)->chainLatencySamples.store(
                    totalLatency, std::memory_order_release);
        }
        cachedDuration.store(getTracksDuration(tracks));
    }
    playbackTransformResetRequested.store(true, std::memory_order_release);
}

void AudioEngine::ChunkedRecordingBuffer::ensureChunk(size_t chunkIndex)
{
    while (chunks.size() <= chunkIndex)
        chunks.emplace_back(std::unique_ptr<float[]>(new float[chunkSize]));
}

void AudioEngine::ChunkedRecordingBuffer::preallocate(size_t samples)
{
    const size_t requiredChunks = (samples + chunkSize - 1) / chunkSize;
    // Reserving only the pointer table is cheap and prevents that table itself
    // from reallocating during very long recordings.
    constexpr size_t longRecordingPointerCapacity = 32768;
    if (chunks.capacity() < std::max(requiredChunks, longRecordingPointerCapacity))
        chunks.reserve(std::max(requiredChunks, longRecordingPointerCapacity));

    if (requiredChunks > 0)
        ensureChunk(requiredChunks - 1);
}

void AudioEngine::ChunkedRecordingBuffer::append(const float* source, size_t numSamples)
{
    jassert(source != nullptr || numSamples == 0);
    size_t sourceOffset = 0;
    while (sourceOffset < numSamples)
    {
        const size_t chunkIndex = sampleCount / chunkSize;
        const size_t chunkOffset = sampleCount % chunkSize;
        ensureChunk(chunkIndex);
        const size_t toCopy = std::min(numSamples - sourceOffset, chunkSize - chunkOffset);
        std::copy_n(source + sourceOffset, toCopy, chunks[chunkIndex].get() + chunkOffset);
        sampleCount += toCopy;
        sourceOffset += toCopy;
    }
}

void AudioEngine::ChunkedRecordingBuffer::appendValue(float value, size_t numSamples)
{
    size_t appended = 0;
    while (appended < numSamples)
    {
        const size_t chunkIndex = sampleCount / chunkSize;
        const size_t chunkOffset = sampleCount % chunkSize;
        ensureChunk(chunkIndex);
        const size_t toFill = std::min(numSamples - appended, chunkSize - chunkOffset);
        std::fill_n(chunks[chunkIndex].get() + chunkOffset, toFill, value);
        sampleCount += toFill;
        appended += toFill;
    }
}

void AudioEngine::ChunkedRecordingBuffer::resize(size_t newSize, float value)
{
    if (newSize <= sampleCount)
    {
        sampleCount = newSize;
        return;
    }

    appendValue(value, newSize - sampleCount);
}

void AudioEngine::ChunkedRecordingBuffer::copyTo(juce::AudioBuffer<float>& destination,
                                                  int destinationChannel,
                                                  int numSamples) const
{
    const size_t samplesToCopy = std::min(sampleCount,
        static_cast<size_t>(std::max(0, numSamples)));
    size_t copied = 0;
    while (copied < samplesToCopy)
    {
        const size_t chunkIndex = copied / chunkSize;
        const size_t chunkOffset = copied % chunkSize;
        const size_t toCopy = std::min(samplesToCopy - copied, chunkSize - chunkOffset);
        destination.copyFrom(destinationChannel,
                             static_cast<int>(copied),
                             chunks[chunkIndex].get() + chunkOffset,
                             static_cast<int>(toCopy));
        copied += toCopy;
    }
}

//==============================================================================
class TrackStateAction : public juce::UndoableAction
{
public:
    TrackStateAction(AudioEngine& engineToUse,
                     const std::vector<TrackData>& stateBefore,
                     const std::vector<TrackData>& stateAfter)
        : engine(engineToUse), beforeState(stateBefore), afterState(stateAfter)
    {
    }

    bool perform() override
    {
        engine.restoreTracksState(afterState);
        return true;
    }

    bool undo() override
    {
        engine.restoreTracksState(beforeState);
        return true;
    }

    int getSizeInUnits() override
    {
        return static_cast<int>(sizeof(TrackStateAction)
                                + beforeState.size() * sizeof(TrackData)
                                + afterState.size() * sizeof(TrackData));
    }

private:
    AudioEngine& engine;
    std::vector<TrackData> beforeState;
    std::vector<TrackData> afterState;
};

AudioEngine::AudioEngine()
    : importThreadPool(juce::ThreadPool::Options{}
                           .withNumberOfThreads(1)
                           .withThreadName("Audio Import")
                           .withDesiredThreadPriority(juce::Thread::Priority::background)),
      pluginScanThreadPool(juce::ThreadPool::Options{}
                               .withNumberOfThreads(1)
                               .withThreadName("VST3 Scan")
                               .withDesiredThreadPriority(juce::Thread::Priority::background))
{
    formatManager.registerBasicFormats();
    juce::addDefaultFormatsToManager(pluginFormatManager);
    tempoMapSnapshot = std::make_shared<const TempoMap>(projectDawSettings.tempoMap);
}

AudioEngine::~AudioEngine()
{
    exportCancelRequested.store(true);
    exportGeneration.fetch_add(1);
    exportThreadPool.removeAllJobs(true, -1);
    rhythmRenderShutdownRequested.store(true);
    rhythmRenderGeneration.fetch_add(1);
    rhythmRenderThreadPool.removeAllJobs(true, -1);
    importShutdownRequested.store(true);
    pluginScanShutdownRequested.store(true);
    projectGeneration.fetch_add(1);
    pluginGeneration.fetch_add(1);
    pluginScanGeneration.fetch_add(1);
    closeVst3PluginEditor(false);

    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        trackPlugins.clear();
    }

    // Jobs read and resample in small chunks, so interruption is prompt. Wait
    // until every job has actually stopped before any engine-owned state dies.
    importThreadPool.removeAllJobs(true, -1);
    cancelPendingUpdate();

    const juce::ScopedLock lock(completedImportsLock);
    completedImports.clear();
}

//==============================================================================
// Audio callback

void AudioEngine::prepare(int samplesPerBlock, double sampleRate)
{
    if (sampleRate <= 0.0)
        sampleRate = 44100.0;

    currentBlockSize = std::max(1, samplesPerBlock);
    currentSampleRate.store(sampleRate);
    metronome.prepare(sampleRate);
    masterBusProcessor.prepare(sampleRate,
                               std::max(currentBlockSize, 8192),
                               2);
    masterBusProcessor.setSettings(getProjectDawSettings().mastering);
    trackProcessingBuffer.setSize(2, std::max(currentBlockSize, 8192), false, false, true);
    trackProcessingBuffer.clear();
    pluginEditorTransportBuffer.setSize(2,
                                        std::max(currentBlockSize, 8192),
                                        false,
                                        false,
                                        true);
    pluginEditorTransportBuffer.clear();
    const int maximumTransformOutputSamples = std::max(currentBlockSize, 8192);
    PlaybackTransform transformBufferSizer;
    transformBufferSizer.prepare(2, sampleRate);
    int maximumTransformInputSamples = std::max(
        std::max(currentBlockSize * 2, 8192),
        transformBufferSizer.getMaximumInputSamplesForOutput(
            maximumTransformOutputSamples, 1.5));
    {
        const juce::ScopedLock lock(tracksLock);
        rebuildTrackPlaybackSlotsLocked();
        for (auto& slot : trackPlaybackSlots)
        {
            slot->transform.prepare(2, sampleRate);
            maximumTransformInputSamples = std::max(
                maximumTransformInputSamples,
                slot->transform.getMaximumInputSamplesForOutput(
                    maximumTransformOutputSamples, 1.5));
            slot->simpleMix.prepare(sampleRate,
                                    std::max(currentBlockSize, 8192),
                                    2);
            for (auto& processor : slot->automationProcessors)
                processor->reset();
            slot->prepareLatencyCompensation(sampleRate);
        }
    }
    trackTransformInputBuffer.setSize(2,
                                      maximumTransformInputSamples,
                                      false,
                                      false,
                                      true);
    trackTransformInputBuffer.clear();

    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        for (auto& slot : trackPlugins)
        {
            if (slot->instance == nullptr)
                continue;
            slot->instance->releaseResources();
            slot->editorChannels = preparePluginInstance(*slot->instance,
                                                          sampleRate,
                                                          currentBlockSize);
            if (slot->processingInstance != nullptr
                && slot->processingInstance != slot->instance)
            {
                slot->processingInstance->releaseResources();
                slot->processingChannels = preparePluginInstance(
                    *slot->processingInstance,
                    sampleRate,
                    currentBlockSize);
            }
            else
            {
                slot->processingChannels = slot->editorChannels;
            }
        }
    }
    refreshTrackPluginLatencies();

    // Keep five minutes of chunks ready per channel. Longer takes add small
    // chunks without ever reallocating and copying the full recorded history.
    const auto reserveSamples = static_cast<size_t>(sampleRate * 60.0 * 5.0);
    {
        const juce::ScopedLock lock(recordingLock);
        recordingBuffer.preallocate(reserveSamples);
        recordingBufferRight.preallocate(reserveSamples);
    }

    // Existing clips keep their samples when the audio device changes. The
    // mixer already handles differing rates, so repeated 44.1/48 kHz switches
    // cannot accumulate destructive resampling loss.
}

void AudioEngine::processBlock(const juce::AudioSourceChannelInfo& bufferToFill,
                               const float* const* inputChannelData,
                               int numInputChannels)
{
    if (bufferToFill.buffer == nullptr || bufferToFill.numSamples <= 0)
        return;

    const int numSamples = bufferToFill.numSamples;
    const int outputChannels = bufferToFill.buffer->getNumChannels();
    juce::AudioBuffer<float> outputView(bufferToFill.buffer->getArrayOfWritePointers(),
                                        outputChannels,
                                        bufferToFill.startSample,
                                        numSamples);
    outputView.clear();

    updateInputMeter(inputChannelData, numInputChannels, numSamples);

    // PitchNet uses a separate ARA renderer for audible playback. Keep its
    // editor-side VST3 instance receiving process-context updates as well so
    // the playhead in the open plug-in window follows the shared transport.
    pumpOpenPluginEditorTransport(numSamples);

    const auto state = playbackState.load();
    switch (state)
    {
        case PlaybackState::Playing:
            if (playbackState.load() == PlaybackState::Playing)
                processPlayback(outputView, numSamples);
            break;

        case PlaybackState::Recording:
            if (playbackState.load() == PlaybackState::Recording)
            {
                processRecording(inputChannelData, numInputChannels, numSamples);
                processPlayback(outputView, numSamples);
            }
            break;

        case PlaybackState::CountIn:
        {
            const int64_t remaining = std::max<int64_t>(0, countInSamplesRemaining.load());
            const int countInLength = static_cast<int>(std::min<int64_t>(remaining, numSamples));

            if (countInLength > 0)
            {
                juce::AudioBuffer<float> countInView(outputView.getArrayOfWritePointers(),
                                                     outputChannels,
                                                     0,
                                                     countInLength);
                const int64_t countInPosition = countInSamplesTotal.load() - remaining;
                metronome.processBlock(countInView, countInLength, countInPosition, true);
                countInSamplesRemaining.store(remaining - countInLength);
            }

            const int recordingLength = numSamples - countInLength;
            if (countInSamplesRemaining.load() <= 0)
            {
                auto expectedState = PlaybackState::CountIn;
                const bool beganRecording = playbackState.compare_exchange_strong(
                    expectedState, PlaybackState::Recording);
                if (beganRecording) triggerAsyncUpdate();

                if (beganRecording && recordingLength > 0)
                {
                    juce::AudioBuffer<float> recordingView(outputView.getArrayOfWritePointers(),
                                                           outputChannels,
                                                           countInLength,
                                                           recordingLength);
                    processRecording(inputChannelData,
                                     numInputChannels,
                                     recordingLength,
                                     countInLength);
                    processPlayback(recordingView, recordingLength);
                }
            }
            break;
        }

        case PlaybackState::Stopped:
        default:
            break;
    }

}

void AudioEngine::updateInputMeter(const float* const* inputChannelData,
                                   int numInputChannels,
                                   int numSamples) noexcept
{
    int validInputChannels = 0;
    float peak = 0.0f;
    if (inputChannelData != nullptr && numSamples > 0)
    {
        for (int channel = 0; channel < std::min(numInputChannels, 2); ++channel)
        {
            if (inputChannelData[channel] == nullptr)
                continue;

            ++validInputChannels;
            for (int sample = 0; sample < numSamples; ++sample)
                peak = std::max(peak, std::abs(inputChannelData[channel][sample]));
        }
    }
    lastKnownInputChannels.store(validInputChannels);

    float level = currentInputLevel.load();
    level = peak > level ? peak : level * 0.9f + peak * 0.1f;
    currentInputLevel.store(level);
}

void AudioEngine::release()
{
    stop();
    masterBusProcessor.reset();
    const juce::SpinLock::ScopedLockType lock(pluginLock);
    for (auto& slot : trackPlugins)
    {
        if (slot->instance != nullptr)
            slot->instance->releaseResources();
        if (slot->processingInstance != nullptr && slot->processingInstance != slot->instance)
            slot->processingInstance->releaseResources();
    }
}

//==============================================================================
// Transport

void AudioEngine::play()
{
    if (playbackState.load() != PlaybackState::Stopped)
        return;

    if (araTrackSyncPending.exchange(false))
        syncAraTracksToModel();

    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        for (auto& slot : trackPlugins)
            slot->auditionWithEditorInstance.store(false);
    }

    if (pluginEditorTrackId.isNotEmpty())
        syncAraProcessingState(pluginEditorTrackId);
    else
        syncAraProcessingState({});
    startPlayback();
}

void AudioEngine::startPlayback()
{
    if (playbackState.load() != PlaybackState::Stopped)
        return;

    {
        const juce::ScopedLock lock(tracksLock);
        resetTrackPlaybackTransformsLocked();
    }
    playbackTransformResetRequested.store(false);
    loopFadeInSamplesRemaining.store(0);
    masterBusProcessor.reset();
    const auto position = currentPlaybackPosition.load();
    playbackPositionInSamples.store(static_cast<int64_t>(std::llround(position * currentSampleRate.load())));
    playbackState.store(PlaybackState::Playing);

    if (onStateChanged) onStateChanged();
}

void AudioEngine::playFromAraEditor(const juce::String& trackId)
{
    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        for (auto& slot : trackPlugins)
        {
            slot->auditionWithEditorInstance.store(
                slot->trackId == trackId && !slot->processingUsesAra);
        }
    }
    syncAraProcessingState(trackId);
    startPlayback();
}

void AudioEngine::record(int trackIndex)
{
    if (playbackState.load() != PlaybackState::Stopped)
        return;

    {
        const juce::ScopedLock lock(tracksLock);
        if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
            return;

        recordingTrackId = tracks[static_cast<size_t>(trackIndex)].id;
        if (recordingTrackId.isEmpty())
        {
            recordingTrackId = makeTrackId();
            tracks[static_cast<size_t>(trackIndex)].id = recordingTrackId;
        }
    }

    recordingTrackIndex.store(trackIndex);
    recordingStartTime = currentPlaybackPosition.load();
    recordingSampleRate = currentSampleRate.load();
    recordingLatencySamples = getEffectiveRecordingLatencySamples();
    recordingHadValidInput.store(false);
    recordingPeak.store(0.0f);
    recordingSampleCount.store(0);
    recordingNumChannels = 0;
    missingInputWarningSent.store(false);
    pendingMissingInputWarning.store(false);

    {
        const juce::ScopedLock lock(recordingLock);
        recordingBuffer.clear();
        recordingBufferRight.clear();
    }
    {
        const juce::SpinLock::ScopedLockType lock(recordingPreviewLock);
        recordingPreview.fill(0.0f);
        recordingPreviewSize = 0;
        recordingPreviewBlocksPerPoint = 1;
        recordingPreviewBlocksAccumulated = 0;
        recordingPreviewAccumulator = 0.0f;
    }

    playbackPositionInSamples.store(static_cast<int64_t>(std::llround(recordingStartTime * recordingSampleRate)));

    const auto countInTempo = getTempoPointAt(recordingStartTime);
    metronome.setBpm(juce::roundToInt(countInTempo.bpm));
    metronome.setTimeSignature(countInTempo.numerator,
                               countInTempo.denominator);

    if (countInEnabled.load())
    {
        const double quarterNoteSeconds = 60.0 / countInTempo.bpm;
        const double measureInQuarterNotes = static_cast<double>(countInTempo.numerator)
                                             * 4.0
                                             / static_cast<double>(countInTempo.denominator);
        const int64_t samples = std::max<int64_t>(1,
            static_cast<int64_t>(std::llround(quarterNoteSeconds
                                              * measureInQuarterNotes
                                              * recordingSampleRate)));
        countInSamplesTotal.store(samples);
        countInSamplesRemaining.store(samples);
        playbackState.store(PlaybackState::CountIn);
    }
    else
    {
        countInSamplesTotal.store(0);
        countInSamplesRemaining.store(0);
        playbackState.store(PlaybackState::Recording);
    }

    if (onStateChanged) onStateChanged();
}

void AudioEngine::stop()
{
    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        for (auto& slot : trackPlugins)
            slot->auditionWithEditorInstance.store(false);
    }
    const auto previousState = playbackState.exchange(PlaybackState::Stopped);
    if (previousState == PlaybackState::Stopped)
    {
        currentPlaybackPosition.store(0.0);
        playbackPositionInSamples.store(0);
        if (onStateChanged) onStateChanged();
        triggerAsyncUpdate();
        return;
    }

    bool clipAdded = false;
    bool hadCapturedSamples = false;
    AudioClip recordedClip;

    if (previousState == PlaybackState::Recording)
    {
        {
            const juce::ScopedLock recordingGuard(recordingLock);
            hadCapturedSamples = recordingHadValidInput.load() && !recordingBuffer.empty();

            if (hadCapturedSamples)
            {
                const int numSamples = static_cast<int>(recordingBuffer.size());
                const bool hasStereo = recordingNumChannels >= 2
                                       && recordingBufferRight.size() == recordingBuffer.size();
                const int numChannels = hasStereo ? 2 : 1;

                auto audioBuffer = std::make_shared<juce::AudioBuffer<float>>(numChannels, numSamples);
                recordingBuffer.copyTo(*audioBuffer, 0, numSamples);
                if (hasStereo)
                    recordingBufferRight.copyTo(*audioBuffer, 1, numSamples);

                const int64_t uncorrectedStart = static_cast<int64_t>(std::llround(recordingStartTime
                                                                                   * recordingSampleRate));
                const int64_t correctedStart = uncorrectedStart
                                               - static_cast<int64_t>(recordingLatencySamples);
                const int trimSamples = correctedStart < 0
                                            ? static_cast<int>(std::min<int64_t>(-correctedStart, numSamples))
                                            : 0;

                if (trimSamples < numSamples)
                {
                    recordedClip.id = makeClipId();
                    recordedClip.buffer = std::move(audioBuffer);
                    recordedClip.startTime = static_cast<double>(std::max<int64_t>(0, correctedStart))
                                             / recordingSampleRate;
                    recordedClip.offset = static_cast<double>(trimSamples) / recordingSampleRate;
                    recordedClip.duration = static_cast<double>(numSamples - trimSamples) / recordingSampleRate;
                    recordedClip.sampleRate = static_cast<int>(std::llround(recordingSampleRate));
                }
            }
        }

        if (recordedClip.buffer != nullptr)
        {
            beginEdit();
            {
                const juce::ScopedLock trackGuard(tracksLock);
                auto trackIt = std::find_if(tracks.begin(), tracks.end(),
                    [this](const TrackData& track) { return track.id == recordingTrackId; });

                if (trackIt != tracks.end())
                {
                    recordedClip.name = "Audio "
                                        + juce::String(trackIt->clips.size() + 1).paddedLeft('0', 2);
                    trackIt->clips.push_back(std::move(recordedClip));
                    recordingTrackIndex.store(static_cast<int>(std::distance(tracks.begin(), trackIt)));
                    clipAdded = true;
                }
            }

            if (clipAdded)
                endEdit();
            else
                editInProgress = false;
        }

        if (!clipAdded)
        {
            if (!hadCapturedSamples)
                postStatusMessage(utf8(u8"入力が見つからなかったため、録音データは保存されませんでした。"), true);
            else
                postStatusMessage(utf8(u8"レイテンシー補正後に有効な録音範囲が残りませんでした。"), true);
        }
    }

    {
        const juce::ScopedLock recordingGuard(recordingLock);
        recordingBuffer.clear();
        recordingBufferRight.clear();
        recordingNumChannels = 0;
    }
    recordingSampleCount.store(0);
    recordingTrackIndex.store(-1);
    countInSamplesRemaining.store(0);

    const double rate = std::max(1.0, currentSampleRate.load());
    currentPlaybackPosition.store(static_cast<double>(playbackPositionInSamples.load()) / rate);

    if (clipAdded)
    {
        markDirty();
        constexpr float nearSilenceThreshold = 0.0002511886f; // -72 dBFS
        if (recordingPeak.load() < nearSilenceThreshold)
            postStatusMessage(utf8(u8"録音は保存されましたが、入力信号がほぼありません。入力機器・端子・ゲインを確認してください。"), true);
    }
    if (onStateChanged) onStateChanged();
    triggerAsyncUpdate();
}

void AudioEngine::setCurrentTime(double time)
{
    const double newTime = std::max(0.0, time);
    currentPlaybackPosition.store(newTime);
    playbackPositionInSamples.store(static_cast<int64_t>(std::llround(newTime * currentSampleRate.load())));
    playbackTransformResetRequested.store(true);
    loopFadeInSamplesRemaining.store(0);
    masterBusProcessor.reset();
    const auto tempo = getTempoPointAt(newTime);
    metronome.setBpm(juce::roundToInt(tempo.bpm));
    metronome.setTimeSignature(tempo.numerator, tempo.denominator);
    if (onStateChanged) onStateChanged();
}

//==============================================================================
// Tracks and files

int AudioEngine::addTrack(const juce::String& name)
{
    int newIndex = -1;
    {
        const juce::ScopedLock lock(tracksLock);
        TrackData track;
        track.id = makeTrackId();
        track.name = name.isEmpty() ? ("Track " + juce::String(tracks.size() + 1)) : name;
        track.color = getTrackColor(nextTrackColorIndex++);
        tracks.push_back(std::move(track));
        rebuildTrackPlaybackSlotsLocked();
        newIndex = static_cast<int>(tracks.size()) - 1;
    }

    markDirty();
    if (onStateChanged) onStateChanged();
    return newIndex;
}

void AudioEngine::removeTrack(int index)
{
    bool removed = false;
    double updatedDuration = cachedDuration.load();
    juce::String removedTrackId;
    {
        const juce::ScopedLock lock(tracksLock);
        if (index < 0 || index >= static_cast<int>(tracks.size()))
            return;

        const auto state = playbackState.load();
        if ((state == PlaybackState::Recording || state == PlaybackState::CountIn)
            && tracks[static_cast<size_t>(index)].id == recordingTrackId)
        {
            postStatusMessage(utf8(u8"録音先のトラックは録音中に削除できません。"), true);
            return;
        }

        removedTrackId = tracks[static_cast<size_t>(index)].id;
        tracks.erase(tracks.begin() + index);
        rebuildTrackPlaybackSlotsLocked();
        const int currentRecordingIndex = recordingTrackIndex.load();
        if (currentRecordingIndex > index)
            recordingTrackIndex.store(currentRecordingIndex - 1);
        updatedDuration = getTracksDuration(tracks);
        removed = true;
    }

    if (removed)
    {
        if (pluginEditorTrackId == removedTrackId)
            closeVst3PluginEditor(false);
        {
            const juce::SpinLock::ScopedLockType lock(pluginLock);
            trackPlugins.erase(std::remove_if(trackPlugins.begin(), trackPlugins.end(),
                [&removedTrackId](const std::unique_ptr<TrackPluginSlot>& slot)
                {
                    return slot->trackId == removedTrackId;
                }), trackPlugins.end());
        }
        cachedDuration.store(updatedDuration);
        markDirty();
        if (onStateChanged) onStateChanged();
    }
}

bool AudioEngine::loadAudioFile(const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr)
    {
        postStatusMessage(utf8(u8"音声ファイルを読み込めませんでした: ") + file.getFileName(), true);
        return false;
    }

    if (reader->lengthInSamples <= 0
        || reader->lengthInSamples > static_cast<juce::int64>(std::numeric_limits<int>::max()))
    {
        postStatusMessage(utf8(u8"音声ファイルの長さを扱えません: ") + file.getFileName(), true);
        return false;
    }

    const int sourceNumSamples = static_cast<int>(reader->lengthInSamples);
    const int sourceChannels = juce::jlimit(1, 2, static_cast<int>(reader->numChannels));
    const double sourceRate = reader->sampleRate > 0.0 ? reader->sampleRate : currentSampleRate.load();

    auto audioBuffer = std::make_shared<juce::AudioBuffer<float>>(sourceChannels, sourceNumSamples);
    audioBuffer->clear();
    if (!reader->read(audioBuffer.get(), 0, sourceNumSamples, 0, true, sourceChannels > 1))
    {
        postStatusMessage(utf8(u8"音声ファイルの読み込み中にエラーが発生しました: ") + file.getFileName(), true);
        return false;
    }

    AudioClip clip;
    clip.id = makeClipId();
    clip.buffer = std::move(audioBuffer);
    clip.startTime = 0.0;
    clip.offset = 0.0;
    clip.duration = static_cast<double>(sourceNumSamples) / sourceRate;
    clip.sampleRate = static_cast<int>(std::llround(sourceRate));
    clip.name = file.getFileNameWithoutExtension();
    resampleClip(clip, currentSampleRate.load());

    beginEdit();
    {
        const juce::ScopedLock lock(tracksLock);
        TrackData track;
        track.id = makeTrackId();
        track.name = file.getFileNameWithoutExtension();
        track.color = getTrackColor(nextTrackColorIndex++);
        track.clips.push_back(std::move(clip));
        tracks.push_back(std::move(track));
        rebuildTrackPlaybackSlotsLocked();
    }
    endEdit();

    markDirty();
    if (onStateChanged) onStateChanged();
    return true;
}

int AudioEngine::loadAudioFiles(const juce::Array<juce::File>& files)
{
    int loaded = 0;
    for (const auto& file : files)
        if (loadAudioFile(file))
            ++loaded;

    if (loaded > 0)
        postStatusMessage(juce::String(loaded) + utf8(u8"個の音声ファイルを読み込みました。"), false);
    return loaded;
}

void AudioEngine::loadAudioFilesAsync(const juce::Array<juce::File>& files)
{
    if (files.isEmpty() || importShutdownRequested.load())
        return;

    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

    const auto filesToImport = files;
    const auto generation = projectGeneration.load();
    const double requestedRate = currentSampleRate.load();
    const double targetRate = requestedRate > 0.0 ? requestedRate : 44100.0;

    postStatusMessage(juce::String(filesToImport.size())
                          + utf8(u8"個の音声ファイルを読み込んでいます…"),
                      false);

    importThreadPool.addJob([this, filesToImport, generation, targetRate]()
    {
        auto result = std::make_unique<ImportBatchResult>();
        result->generation = generation;
        result->requestedCount = filesToImport.size();

        auto* currentJob = juce::ThreadPoolJob::getCurrentThreadPoolJob();
        const auto shouldCancel = [this, generation, currentJob]()
        {
            return importShutdownRequested.load()
                   || projectGeneration.load() != generation
                   || (currentJob != nullptr && currentJob->shouldExit());
        };

        juce::AudioFormatManager workerFormatManager;
        workerFormatManager.registerBasicFormats();

        for (const auto& file : filesToImport)
        {
            if (shouldCancel())
                return;

            const auto markFailed = [&result, &file]()
            {
                const auto displayName = file.getFileName().isNotEmpty()
                                             ? file.getFileName()
                                             : file.getFullPathName();
                result->failedFiles.addIfNotAlreadyThere(displayName);
            };

            try
            {
                std::unique_ptr<juce::AudioFormatReader> reader(
                    workerFormatManager.createReaderFor(file));
                if (reader == nullptr
                    || reader->lengthInSamples <= 0
                    || reader->lengthInSamples > static_cast<juce::int64>(
                           std::numeric_limits<int>::max()))
                {
                    markFailed();
                    continue;
                }

                const int sourceNumSamples = static_cast<int>(reader->lengthInSamples);
                const int sourceChannels = juce::jlimit(1, 2,
                    static_cast<int>(reader->numChannels));
                const double sourceRate = reader->sampleRate > 0.0
                                              ? reader->sampleRate
                                              : targetRate;

                auto audioBuffer = std::make_shared<juce::AudioBuffer<float>>(
                    sourceChannels, sourceNumSamples);
                audioBuffer->clear();

                constexpr int readChunkSize = 32768;
                bool readSucceeded = true;
                for (int position = 0; position < sourceNumSamples; position += readChunkSize)
                {
                    if (shouldCancel())
                        return;

                    const int samplesToRead = std::min(readChunkSize,
                                                       sourceNumSamples - position);
                    if (!reader->read(audioBuffer.get(),
                                      position,
                                      samplesToRead,
                                      position,
                                      true,
                                      sourceChannels > 1))
                    {
                        readSucceeded = false;
                        break;
                    }
                }

                if (!readSucceeded)
                {
                    markFailed();
                    continue;
                }

                AudioClip clip;
                clip.buffer = std::move(audioBuffer);
                clip.startTime = 0.0;
                clip.offset = 0.0;
                clip.duration = static_cast<double>(sourceNumSamples) / sourceRate;
                clip.sampleRate = static_cast<int>(std::llround(sourceRate));
                clip.name = file.getFileNameWithoutExtension();

                const bool needsResampling = std::abs(sourceRate - targetRate) >= 0.5;
                resampleClipInternal(clip, targetRate, shouldCancel);
                if (shouldCancel())
                    return;

                if (needsResampling
                    && std::abs(static_cast<double>(clip.sampleRate) - targetRate) >= 0.5)
                {
                    markFailed();
                    continue;
                }

                ImportBatchResult::Item imported;
                imported.trackName = file.getFileNameWithoutExtension();
                imported.clip = std::move(clip);
                result->importedItems.push_back(std::move(imported));
            }
            catch (...)
            {
                markFailed();
            }
        }

        if (shouldCancel())
            return;

        {
            const juce::ScopedLock lock(completedImportsLock);
            if (importShutdownRequested.load())
                return;
            completedImports.push_back(std::move(result));
        }
        if (playbackState.load() == PlaybackState::Stopped)
            triggerAsyncUpdate();
    });
}

void AudioEngine::loadAudioFileWithDialog()
{
    const juce::WeakReference<AudioEngine> safeThis(this);
    auto chooser = std::make_shared<juce::FileChooser>(
        utf8(u8"音声ファイルを選択"),
        juce::File::getSpecialLocation(juce::File::userMusicDirectory),
        formatManager.getWildcardForAllFormats());

    chooser->launchAsync(juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::canSelectMultipleItems,
        [safeThis, chooser](const juce::FileChooser& fc)
        {
            const auto results = fc.getResults();
            if (safeThis != nullptr && !results.isEmpty())
                safeThis->loadAudioFilesAsync(results);
        });
}

void AudioEngine::toggleMute(int trackIndex)
{
    {
        const juce::ScopedLock lock(tracksLock);
        if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size())) return;
        auto& track = tracks[static_cast<size_t>(trackIndex)];
        track.isMuted = !track.isMuted;
    }
    playbackTransformResetRequested.store(true);
    markDirty();
    if (onStateChanged) onStateChanged();
}

void AudioEngine::toggleSolo(int trackIndex)
{
    {
        const juce::ScopedLock lock(tracksLock);
        if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size())) return;
        auto& track = tracks[static_cast<size_t>(trackIndex)];
        track.isSolo = !track.isSolo;
    }
    playbackTransformResetRequested.store(true);
    markDirty();
    if (onStateChanged) onStateChanged();
}

void AudioEngine::toggleArm(int trackIndex)
{
    {
        const juce::ScopedLock lock(tracksLock);
        if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size())) return;

        for (size_t i = 0; i < tracks.size(); ++i)
            if (static_cast<int>(i) != trackIndex)
                tracks[i].isArmed = false;

        auto& track = tracks[static_cast<size_t>(trackIndex)];
        track.isArmed = !track.isArmed;
    }
    markDirty();
    if (onStateChanged) onStateChanged();
}

void AudioEngine::setTrackName(int trackIndex, const juce::String& name)
{
    const juce::String newName = name.trim();
    if (newName.isEmpty()) return;

    const juce::ScopedLock lock(tracksLock);
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size())) return;
    if (tracks[static_cast<size_t>(trackIndex)].name == newName) return;

    const bool ownsTransaction = !editInProgress;
    if (ownsTransaction) beginEdit();
    tracks[static_cast<size_t>(trackIndex)].name = newName;
    if (ownsTransaction) endEdit();

    if (onStateChanged) onStateChanged();
}

void AudioEngine::setTrackRole(int trackIndex, TrackRole role)
{
    const bool ownsTransaction = !editInProgress;
    if (ownsTransaction) beginEdit();
    bool changed = false;
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(tracks.size())))
        {
            if (ownsTransaction) editInProgress = false;
            return;
        }

        const auto bounded = sanitiseTrackRole(static_cast<int>(role));
        auto& current = tracks[static_cast<size_t>(trackIndex)].role;
        changed = current != bounded;
        current = bounded;
    }
    if (changed)
    {
        if (ownsTransaction) endEdit();
        else
        {
            markDirty();
            if (onStateChanged) onStateChanged();
        }
    }
    else if (ownsTransaction)
        editInProgress = false;
}

void AudioEngine::setTrackVolume(int trackIndex, float volume)
{
    const bool ownsTransaction = !editInProgress;
    if (ownsTransaction) beginEdit();
    bool changed = false;
    {
        const juce::ScopedLock lock(tracksLock);
        if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        {
            if (ownsTransaction) editInProgress = false;
            return;
        }
        auto& track = tracks[static_cast<size_t>(trackIndex)];
        const float newVolume = juce::jlimit(0.0f, 1.0f, volume);
        changed = !juce::approximatelyEqual(track.volume, newVolume);
        track.volume = newVolume;
    }
    if (changed)
    {
        if (ownsTransaction) endEdit();
        else
        {
            markDirty();
            if (onStateChanged) onStateChanged();
        }
    }
    else if (ownsTransaction)
        editInProgress = false;
}

void AudioEngine::setTrackPan(int trackIndex, float pan)
{
    const bool ownsTransaction = !editInProgress;
    if (ownsTransaction) beginEdit();
    bool changed = false;
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(tracks.size())))
        {
            if (ownsTransaction) editInProgress = false;
            return;
        }
        auto& track = tracks[static_cast<size_t>(trackIndex)];
        const float value = juce::jlimit(-1.0f, 1.0f, pan);
        changed = !juce::approximatelyEqual(track.pan, value);
        track.pan = value;
    }
    if (changed)
    {
        if (ownsTransaction) endEdit();
        else
        {
            markDirty();
            if (onStateChanged) onStateChanged();
        }
    }
    else if (ownsTransaction)
        editInProgress = false;
}

void AudioEngine::setTrackPitchSemitones(int trackIndex, double semitones)
{
    const bool ownsTransaction = !editInProgress;
    if (ownsTransaction) beginEdit();
    bool changed = false;
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(tracks.size())))
        {
            if (ownsTransaction) editInProgress = false;
            return;
        }

        auto& track = tracks[static_cast<size_t>(trackIndex)];
        const double newSemitones = juce::jlimit(-12.0, 12.0, semitones);
        changed = !juce::approximatelyEqual(track.pitchSemitones, newSemitones);
        track.pitchSemitones = newSemitones;
    }

    if (changed)
    {
        playbackTransformResetRequested.store(true);
        if (ownsTransaction) endEdit();
        else
        {
            markDirty();
            if (onStateChanged) onStateChanged();
        }
    }
    else if (ownsTransaction)
        editInProgress = false;
}

void AudioEngine::setTrackPlaybackSpeed(int trackIndex, double speed)
{
    const bool ownsTransaction = !editInProgress;
    if (ownsTransaction) beginEdit();
    bool changed = false;
    double updatedDuration = cachedDuration.load();
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(tracks.size())))
        {
            if (ownsTransaction) editInProgress = false;
            return;
        }

        auto& track = tracks[static_cast<size_t>(trackIndex)];
        const double newSpeed = juce::jlimit(0.75, 1.5, speed);
        changed = !juce::approximatelyEqual(track.playbackSpeed, newSpeed);
        track.playbackSpeed = newSpeed;
        if (changed)
            updatedDuration = getTracksDuration(tracks);
    }

    if (changed)
    {
        cachedDuration.store(updatedDuration);
        playbackTransformResetRequested.store(true);
        if (ownsTransaction) endEdit();
        else
        {
            markDirty();
            if (onStateChanged) onStateChanged();
        }
    }
    else if (ownsTransaction)
        editInProgress = false;
}

void AudioEngine::resetTrackTransform(int trackIndex)
{
    beginEdit();
    setTrackPitchSemitones(trackIndex, 0.0);
    setTrackPlaybackSpeed(trackIndex, 1.0);
    endEdit();
}

void AudioEngine::setTrackPitchCorrectionSettings(
    int trackIndex, const PitchCorrectionSettings& settings)
{
    const bool ownsTransaction = !editInProgress;
    if (ownsTransaction) beginEdit();
    bool changed = false;
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(tracks.size())))
        {
            if (ownsTransaction) editInProgress = false;
            return;
        }
        auto bounded = settings;
        bounded.key = juce::jlimit(0, 11, bounded.key);
        bounded.strength = juce::jlimit(0.0f, 1.0f, bounded.strength);
        auto& current = tracks[static_cast<size_t>(trackIndex)].pitchCorrection;
        changed = !(current == bounded);
        current = bounded;
    }
    if (changed)
    {
        if (ownsTransaction) endEdit();
        else
        {
            markDirty();
            if (onStateChanged) onStateChanged();
        }
    }
    else if (ownsTransaction)
        editInProgress = false;
}

void AudioEngine::setTrackPitchCorrectionAudition(int trackIndex, bool corrected)
{
    auto snapshot = getTracksSnapshot();
    if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(snapshot.size())))
        return;
    if (!isPitchCorrectionActive(trackIndex))
    {
        postStatusMessage(
            utf8(u8"PitchNetが実行中ではないため、補正音へ切り替えられません。"),
            true);
        return;
    }
    beginEdit();
    auto settings = snapshot[static_cast<size_t>(trackIndex)].pitchCorrection;
    settings.auditionCorrected = corrected;
    settings.enabled = true;
    setTrackPitchCorrectionSettings(trackIndex, settings);
    setTrackVst3Bypassed(trackIndex, !corrected);
    endEdit();
}

void AudioEngine::setTrackSimpleMixSettings(
    int trackIndex, const SimpleMixSettings& settings)
{
    const bool ownsTransaction = !editInProgress;
    if (ownsTransaction) beginEdit();
    bool changed = false;
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(tracks.size())))
        {
            if (ownsTransaction) editInProgress = false;
            return;
        }
        auto bounded = settings;
        bounded.brightness = juce::jlimit(-1.0f, 1.0f, bounded.brightness);
        bounded.ambience = juce::jlimit(0.0f, 1.0f, bounded.ambience);
        bounded.stability = juce::jlimit(0.0f, 1.0f, bounded.stability);
        auto& current = tracks[static_cast<size_t>(trackIndex)].simpleMix;
        changed = !(current == bounded);
        current = bounded;
    }
    if (changed)
    {
        cachedDuration.store(getTracksDuration(getTracksSnapshot()));
        if (ownsTransaction) endEdit();
        else
        {
            markDirty();
            if (onStateChanged) onStateChanged();
        }
    }
    else if (ownsTransaction)
        editInProgress = false;
}

void AudioEngine::setTrackNoiseReductionSettings(
    int trackIndex, const NoiseReductionSettings& settings)
{
    const bool ownsTransaction = !editInProgress;
    if (ownsTransaction) beginEdit();
    bool changed = false;
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(tracks.size())))
        {
            if (ownsTransaction) editInProgress = false;
            return;
        }
        auto bounded = settings;
        bounded.amount = juce::jlimit(0.0f, 1.0f, bounded.amount);
        bounded.deEssAmount = juce::jlimit(0.0f, 1.0f, bounded.deEssAmount);
        auto& current = tracks[static_cast<size_t>(trackIndex)].noiseReduction;
        changed = !(current == bounded);
        current = bounded;
    }
    if (changed)
    {
        if (ownsTransaction) endEdit();
        else
        {
            markDirty();
            if (onStateChanged) onStateChanged();
        }
    }
    else if (ownsTransaction)
        editInProgress = false;
}

bool AudioEngine::addTrackSpecialFxRegion(int trackIndex,
                                          SpecialFxType type,
                                          double startSeconds,
                                          double endSeconds,
                                          float amount,
                                          float wet,
                                          double fadeSeconds)
{
    if (type == SpecialFxType::none || !std::isfinite(startSeconds)
        || !std::isfinite(endSeconds) || endSeconds <= startSeconds + 1.0e-6)
        return false;

    const bool ownsTransaction = !editInProgress;
    if (ownsTransaction) beginEdit();
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(tracks.size())))
        {
            if (ownsTransaction) editInProgress = false;
            return false;
        }
        auto& regions = tracks[static_cast<size_t>(trackIndex)].specialFxRegions;
        if (regions.size() >= 256)
        {
            if (ownsTransaction) editInProgress = false;
            return false;
        }
        SpecialFxRegion region;
        region.id = "special-fx-" + juce::Uuid().toString();
        region.type = type;
        region.startSeconds = std::max(0.0, startSeconds);
        region.endSeconds = std::max(region.startSeconds, endSeconds);
        region.amount = juce::jlimit(0.0f, 1.0f, amount);
        region.wet = juce::jlimit(0.0f, 1.0f, wet);
        region.fadeSeconds = juce::jlimit(
            0.0, std::min(10.0, (region.endSeconds - region.startSeconds) * 0.5),
            fadeSeconds);
        regions.push_back(std::move(region));
        rebuildTrackPlaybackSlotsLocked();
    }
    if (ownsTransaction)
        endEdit();
    else
    {
        markDirty();
        if (onStateChanged) onStateChanged();
    }
    return true;
}

void AudioEngine::clearTrackSpecialFxRegions(int trackIndex)
{
    const bool ownsTransaction = !editInProgress;
    if (ownsTransaction) beginEdit();
    bool changed = false;
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(tracks.size())))
        {
            if (ownsTransaction) editInProgress = false;
            return;
        }
        auto& regions = tracks[static_cast<size_t>(trackIndex)].specialFxRegions;
        changed = !regions.empty();
        regions.clear();
        if (changed)
            rebuildTrackPlaybackSlotsLocked();
    }
    if (changed)
    {
        if (ownsTransaction) endEdit();
        else
        {
            markDirty();
            if (onStateChanged) onStateChanged();
        }
    }
    else if (ownsTransaction)
        editInProgress = false;
}

bool AudioEngine::setTrackAutomationRegions(
    int trackIndex, const std::vector<AudioAutomationRegion>& requested)
{
    if (requested.size() > RegionAutomation::maximumRegions
        || !std::all_of(requested.begin(), requested.end(), RegionAutomation::isValid))
        return false;
    auto regions = requested;
    juce::StringArray ids;
    for (auto& region : regions)
    {
        if (region.id.isEmpty()) region.id = "automation-" + juce::Uuid().toString();
        if (ids.contains(region.id)) return false;
        ids.add(region.id);
    }
    const bool ownsTransaction = !editInProgress;
    if (ownsTransaction) beginEdit();
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(tracks.size())))
        {
            if (ownsTransaction) editInProgress = false;
            return false;
        }
        auto& current = tracks[static_cast<size_t>(trackIndex)].automationRegions;
        if (current == regions)
        {
            if (ownsTransaction) editInProgress = false;
            return true;
        }
        current = std::move(regions);
        rebuildTrackPlaybackSlotsLocked();
    }
    if (ownsTransaction) endEdit();
    else
    {
        markDirty();
        if (onStateChanged) onStateChanged();
    }
    return true;
}

int AudioEngine::duplicateTrackAsHarmony(int trackIndex,
                                         int semitones,
                                         float volume,
                                         float pan)
{
    TrackData harmony;
    int newIndex = -1;
    beginEdit();
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(tracks.size())))
        {
            editInProgress = false;
            return -1;
        }

        harmony = tracks[static_cast<size_t>(trackIndex)];
        harmony.id = makeTrackId();
        harmony.role = TrackRole::harmony;
        harmony.name = harmony.name + utf8(u8" ハモリ ")
            + (semitones >= 0 ? "+" : "") + juce::String(semitones);
        harmony.pitchSemitones = juce::jlimit(
            -12.0, 12.0, harmony.pitchSemitones + semitones);
        harmony.volume = juce::jlimit(0.0f, 1.0f, volume);
        harmony.pan = juce::jlimit(-1.0f, 1.0f, pan);
        harmony.isArmed = false;
        harmony.isSolo = false;
        harmony.color = getTrackColor(nextTrackColorIndex++);
        for (auto& clip : harmony.clips)
            clip.id = makeClipId();
        for (auto& effect : harmony.effectSlots)
            effect.id = "effect-" + juce::Uuid().toString();

        tracks.push_back(harmony);
        newIndex = static_cast<int>(tracks.size()) - 1;
        rebuildTrackPlaybackSlotsLocked();
    }
    cachedDuration.store(getTracksDuration(getTracksSnapshot()));
    endEdit();
    restoreTrackPluginsFromMetadata({ harmony });
    postStatusMessage(utf8(u8"ハモリトラックを作成しました: ") + harmony.name,
                      false);
    return newIndex;
}

void AudioEngine::loadVst3PluginForTrack(int trackIndex, const juce::File& pluginFile)
{
    scanVst3PluginForTrack(trackIndex, pluginFile, false);
}

void AudioEngine::addEffectPluginForTrack(int trackIndex,
                                          const juce::File& pluginFile)
{
    scanVst3PluginForTrack(trackIndex, pluginFile, true);
}

void AudioEngine::scanVst3PluginForTrack(int trackIndex,
                                         const juce::File& pluginFile,
                                         bool addAsEffectSlot,
                                         bool applyNaturalAfterLoad)
{
    juce::String trackId;
    bool validTrack = false;
    {
        const juce::ScopedLock lock(tracksLock);
        validTrack = juce::isPositiveAndBelow(
            trackIndex, static_cast<int>(tracks.size()));
        if (validTrack)
            trackId = tracks[static_cast<size_t>(trackIndex)].id;
    }

    if (!validTrack)
    {
        const auto reason = utf8(
            u8"追加先のトラックが見つからないため、VST3の読み込みを開始できませんでした。");
        postStatusMessage(reason, true);
        if (onPluginScanFinished)
            onPluginScanFinished(pluginFile, false, reason);
        return;
    }

    if (!pluginFile.exists() || !pluginFile.hasFileExtension("vst3"))
    {
        const auto reason = utf8(u8"VST3ファイルを選択してください。");
        postStatusMessage(reason, true);
        if (onPluginScanFinished)
            onPluginScanFinished(pluginFile, false, reason);
        return;
    }

    postStatusMessage(utf8(u8"VST3を確認しています: ") + pluginFile.getFileName(), false);

    const auto scanGeneration = pluginScanGeneration.fetch_add(1) + 1;
    const auto engineGeneration = pluginGeneration.load();
    const auto safeThis = juce::WeakReference<AudioEngine>(this);

    // Drop queued scans and ask a currently-running stale scan to stop as soon
    // as the third-party scanner returns control. Never wait on the UI thread.
    pluginScanThreadPool.removeAllJobs(true, 0);
    pluginScanThreadPool.addJob(
        [this, safeThis, trackId, pluginFile, scanGeneration,
         engineGeneration, addAsEffectSlot, applyNaturalAfterLoad]() mutable
        {
            auto* currentJob = juce::ThreadPoolJob::getCurrentThreadPoolJob();
            const auto shouldCancel = [&]()
            {
                // The destructor waits for this pool before any of these
                // atomics are destroyed, so the worker never needs to
                // dereference a cross-thread WeakReference.
                return pluginScanShutdownRequested.load()
                    || pluginGeneration.load() != engineGeneration
                    || pluginScanGeneration.load() != scanGeneration
                    || (currentJob != nullptr && currentJob->shouldExit());
            };

            if (shouldCancel())
                return;

            bool vst3FormatAvailable = false;
            bool scanFailed = false;
            bool foundEffect = false;
            juce::PluginDescription selectedDescription;

            try
            {
                // The scanner owns a separate format manager, so a slow plug-in
                // cannot contend with message-thread instance creation.
                juce::AudioPluginFormatManager scanFormatManager;
                juce::addDefaultFormatsToManager(scanFormatManager);

                for (int i = 0; i < scanFormatManager.getNumFormats(); ++i)
                {
                    auto* format = scanFormatManager.getFormat(i);
                    if (format == nullptr || !format->getName().equalsIgnoreCase("VST3"))
                        continue;

                    vst3FormatAvailable = true;
                    juce::OwnedArray<juce::PluginDescription> descriptions;
                    format->findAllTypesForFile(descriptions,
                                                pluginFile.getFullPathName());
                    for (auto* description : descriptions)
                    {
                        if (description != nullptr && !description->isInstrument)
                        {
                            selectedDescription = *description;
                            foundEffect = true;
                            break;
                        }
                    }
                    break;
                }
            }
            catch (...)
            {
                scanFailed = true;
            }

            if (shouldCancel())
                return;

            juce::MessageManager::callAsync(
                [safeThis, trackId, scanGeneration, engineGeneration,
                 vst3FormatAvailable, scanFailed, foundEffect,
                 selectedDescription, addAsEffectSlot, pluginFile,
                 applyNaturalAfterLoad]() mutable
                {
                    if (safeThis == nullptr
                        || safeThis->pluginScanShutdownRequested.load()
                        || safeThis->pluginGeneration.load() != engineGeneration
                        || safeThis->pluginScanGeneration.load() != scanGeneration)
                        return;

                    bool trackStillExists = false;
                    {
                        const juce::ScopedLock lock(safeThis->tracksLock);
                        trackStillExists = std::any_of(
                            safeThis->tracks.begin(), safeThis->tracks.end(),
                            [&trackId](const TrackData& track)
                            {
                                return track.id == trackId;
                            });
                    }
                    if (!vst3FormatAvailable)
                    {
                        const auto reason = utf8(
                            u8"このビルドではVST3を読み込めません。");
                        safeThis->postStatusMessage(reason, true);
                        if (safeThis->onPluginScanFinished)
                            safeThis->onPluginScanFinished(pluginFile,
                                                           false,
                                                           reason);
                        return;
                    }
                    if (scanFailed || !foundEffect)
                    {
                        const auto reason = scanFailed
                            ? utf8(u8"VST3の確認中にエラーが発生しました。")
                            : utf8(u8"音声エフェクトとして使えるVST3が見つかりませんでした。");
                        safeThis->postStatusMessage(reason, true);
                        if (safeThis->onPluginScanFinished)
                            safeThis->onPluginScanFinished(pluginFile,
                                                           false,
                                                           reason);
                        return;
                    }

                    const bool dedicatedPitchOrAra =
                        selectedDescription.hasARAExtension
                        || selectedDescription.name.containsIgnoreCase("PitchNet");
                    if (addAsEffectSlot && dedicatedPitchOrAra)
                    {
                        const auto reason = utf8(
                            u8"PitchNet／ARAプラグインはピッチ補正専用スロットで使用してください。");
                        safeThis->postStatusMessage(reason, true);
                        if (safeThis->onPluginScanFinished)
                            safeThis->onPluginScanFinished(pluginFile,
                                                           false,
                                                           reason);
                        return;
                    }

                    if (!trackStillExists)
                    {
                        if (safeThis->onPluginScanFinished)
                            safeThis->onPluginScanFinished(
                                pluginFile,
                                true,
                                utf8(u8"追加先のトラックがなくなったため処理を終了しました。"));
                        return;
                    }

                    if (addAsEffectSlot || !dedicatedPitchOrAra)
                    {
                        safeThis->loadEffectPlugin(
                            trackId,
                            "effect-" + juce::Uuid().toString(),
                            selectedDescription,
                            {},
                            false,
                            true,
                            true,
                            true);
                        return;
                    }

                    if (safeThis->onPluginScanFinished)
                        safeThis->onPluginScanFinished(pluginFile, true, {});

                    safeThis->loadTrackPlugin(
                        trackId,
                        selectedDescription,
                        {},
                        {},
                        {},
                        selectedDescription.hasARAExtension,
                        false,
                        true,
                        true,
                        applyNaturalAfterLoad);
                });
        });
}

void AudioEngine::removeVst3PluginFromTrack(int trackIndex)
{
    juce::String trackId;
    bool changed = false;
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(tracks.size())))
            return;

        auto& track = tracks[static_cast<size_t>(trackIndex)];
        trackId = track.id;
        changed = track.vst3Name.isNotEmpty()
            || track.vst3DescriptionXml.isNotEmpty()
            || track.pitchCorrection.enabled
            || track.pitchCorrection.auditionCorrected;
        track.vst3Name.clear();
        track.vst3DescriptionXml.clear();
        track.vst3State.reset();
        track.vst3AraArchive.reset();
        track.vst3AraArchiveId.clear();
        track.vst3AraHostFormatVersion = 1;
        track.vst3AraPlaybackEnabled = false;
        track.vst3Bypassed = false;
        track.vst3LatencySamples = 0;
        track.vst3TailSeconds = 0.0;
        track.pitchCorrection.enabled = false;
        track.pitchCorrection.auditionCorrected = false;
    }

    if (pluginEditorTrackId == trackId && pluginEditorEffectSlotId.isEmpty())
        closeVst3PluginEditor(false);

    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        trackPlugins.erase(std::remove_if(trackPlugins.begin(), trackPlugins.end(),
            [&trackId](const std::unique_ptr<TrackPluginSlot>& slot)
            {
                return slot->trackId == trackId
                    && slot->effectSlotId.isEmpty();
            }), trackPlugins.end());
    }

    refreshTrackPluginLatencies();

    if (changed)
    {
        cachedDuration.store(getTracksDuration(getTracksSnapshot()));
        markDirty();
        postStatusMessage(utf8(u8"VST3エフェクトを取り外しました。"), false);
        if (onStateChanged) onStateChanged();
    }
}

void AudioEngine::setTrackVst3Bypassed(int trackIndex, bool shouldBeBypassed)
{
    juce::String trackId;
    bool changed = false;
    bool isPitchNet = false;
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(tracks.size())))
            return;
        auto& track = tracks[static_cast<size_t>(trackIndex)];
        if (track.vst3Name.isEmpty())
            return;
        trackId = track.id;
        isPitchNet = track.vst3Name.containsIgnoreCase("PitchNet");
        changed = track.vst3Bypassed != shouldBeBypassed;
        track.vst3Bypassed = shouldBeBypassed;
        if (isPitchNet)
        {
            // Bypass only changes comparison after correction has really been
            // applied. Loading or un-bypassing a dry PitchNet must not invent
            // an ON state in the project/UI.
            if (track.pitchCorrection.enabled)
                track.pitchCorrection.auditionCorrected = !shouldBeBypassed;
        }
    }

    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        for (auto& slot : trackPlugins)
            if (slot->trackId == trackId && slot->effectSlotId.isEmpty())
                slot->bypassed.store(shouldBeBypassed,
                                     std::memory_order_relaxed);
    }

    refreshTrackPluginLatencies();

    if (changed)
    {
        cachedDuration.store(getTracksDuration(getTracksSnapshot()));
        markDirty();
        postStatusMessage(isPitchNet
                              ? (shouldBeBypassed
                                     ? utf8(u8"原音へ切り替えました。")
                                     : utf8(u8"補正後の音へ切り替えました。"))
                              : (shouldBeBypassed
                                     ? utf8(u8"VST3エフェクトをバイパスしました。")
                                     : utf8(u8"VST3エフェクトを有効にしました。")),
                          false);
        if (onStateChanged) onStateChanged();
    }
}

void AudioEngine::showVst3PluginEditor(int trackIndex)
{
    juce::String trackId;
    juce::String pluginName;
    TrackData trackSnapshot;
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(tracks.size())))
            return;
        const auto& track = tracks[static_cast<size_t>(trackIndex)];
        trackId = track.id;
        pluginName = track.vst3Name;
        trackSnapshot = track;
    }

    bool araTrackUpdateFailed = false;
    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        for (auto& slot : trackPlugins)
        {
            if (slot->trackId != trackId || slot->effectSlotId.isNotEmpty()
                || slot->instance == nullptr || slot->araHost == nullptr
                || slot->araHost->matchesTrack(trackSnapshot))
                continue;

            slot->instance->releaseResources();
            if (slot->processingInstance != nullptr
                && slot->processingInstance != slot->instance)
                slot->processingInstance->releaseResources();
            if (!slot->araHost->setTrack(trackSnapshot))
            {
                postStatusMessage(utf8(u8"更新したトラック音声をARAへ渡せませんでした。"), true);
                araTrackUpdateFailed = true;
            }
            else
            {
                slot->araRevision.fetch_add(1);
            }
            slot->editorChannels = preparePluginInstance(
                *slot->instance,
                std::max(1.0, currentSampleRate.load()),
                std::max(1, currentBlockSize));
            if (slot->processingInstance != nullptr
                && slot->processingInstance != slot->instance)
            {
                slot->processingChannels = preparePluginInstance(
                    *slot->processingInstance,
                    std::max(1.0, currentSampleRate.load()),
                    std::max(1, currentBlockSize));
            }
            else
            {
                slot->processingChannels = slot->editorChannels;
            }
            break;
        }
    }

    if (araTrackUpdateFailed)
        return;

    if (pluginEditorWindow != nullptr && pluginEditorTrackId == trackId
        && pluginEditorEffectSlotId.isEmpty())
    {
        setPluginEditorTransportActive(trackId, true);
        pluginEditorWindow->setVisible(true);
        pluginEditorWindow->toFront(true);
        return;
    }

    closeVst3PluginEditor();

    juce::AudioProcessorEditor* editor = nullptr;
    bool migrateLegacyAraState = false;
    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        for (auto& slot : trackPlugins)
        {
            if (slot->trackId == trackId && slot->effectSlotId.isEmpty()
                && slot->instance != nullptr)
            {
                editor = slot->instance->createEditorIfNeeded();
                if (editor != nullptr && slot->araHost != nullptr)
                {
                    migrateLegacyAraState = !slot->araPlaybackEnabled.load();
                    slot->araEditorWasOpened.store(true);
                    slot->araHost->editorOpened();
                }
                break;
            }
        }
    }

    if (editor == nullptr)
    {
        postStatusMessage(utf8(u8"このVST3には開ける設定画面がありません。"), true);
        return;
    }

    if (migrateLegacyAraState)
        handleAraDocumentChanged(trackId);

    const auto safeThis = juce::WeakReference<AudioEngine>(this);
    pluginEditorWindow = std::make_unique<PluginEditorWindow>(
        pluginName.isNotEmpty() ? pluginName : utf8(u8"VST3設定"),
        editor,
        [safeThis, trackId]() mutable
        {
            if (safeThis != nullptr)
                safeThis->hideVst3PluginEditor(trackId);
        },
        [safeThis, trackId]
        {
            if (safeThis == nullptr)
                return;

            if (safeThis->getPlaybackState() == PlaybackState::Stopped)
                safeThis->playFromAraEditor(trackId);
            else
                safeThis->stop();
        },
        [safeThis](int direction)
        {
            if (safeThis == nullptr)
                return;

            const auto& metronome = safeThis->getMetronome();
            const double bpm = std::max(1, metronome.getBpm());
            const double numerator = std::max(1, metronome.getNumerator());
            const double denominator = std::max(1, metronome.getDenominator());
            const double quarterMeasureSeconds =
                (60.0 / bpm) * numerator / denominator;
            const double newTime = safeThis->getCurrentTime()
                                   + direction * quarterMeasureSeconds;
            safeThis->setCurrentTime(
                juce::jlimit(0.0, safeThis->getDuration(), newTime));
        });
    pluginEditorTrackId = trackId;
    pluginEditorEffectSlotId.clear();
    setPluginEditorTransportActive(trackId, true);
}

void AudioEngine::removeEffectSlot(int trackIndex, int slotIndex)
{
    juce::String trackId;
    juce::String effectSlotId;
    juce::String effectName;
    std::vector<juce::String> effectOrder;
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex,
                                      static_cast<int>(tracks.size())))
            return;
        auto& track = tracks[static_cast<size_t>(trackIndex)];
        if (!juce::isPositiveAndBelow(slotIndex,
                                      static_cast<int>(track.effectSlots.size())))
            return;
        trackId = track.id;
        effectSlotId = track.effectSlots[static_cast<size_t>(slotIndex)].id;
        effectName = track.effectSlots[static_cast<size_t>(slotIndex)].name;
        track.effectSlots.erase(track.effectSlots.begin() + slotIndex);
        effectOrder.reserve(track.effectSlots.size());
        for (const auto& effect : track.effectSlots)
            effectOrder.push_back(effect.id);
    }

    if (pluginEditorTrackId == trackId
        && pluginEditorEffectSlotId == effectSlotId)
        closeVst3PluginEditor(false);

    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        trackPlugins.erase(
            std::remove_if(
                trackPlugins.begin(), trackPlugins.end(),
                [&trackId, &effectSlotId](
                    const std::unique_ptr<TrackPluginSlot>& slot)
                {
                    return slot != nullptr && slot->trackId == trackId
                        && slot->effectSlotId == effectSlotId;
                }),
            trackPlugins.end());
        reorderTrackPluginSlotsLocked(trackId, effectOrder);
    }

    refreshTrackPluginLatencies();
    cachedDuration.store(getTracksDuration(getTracksSnapshot()));
    markDirty();
    postStatusMessage(utf8(u8"VST3エフェクトを取り外しました: ")
                          + effectName,
                      false);
    if (onStateChanged)
        onStateChanged();
}

void AudioEngine::moveEffectSlot(int trackIndex, int fromIndex, int toIndex)
{
    juce::String trackId;
    std::vector<juce::String> effectOrder;
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex,
                                      static_cast<int>(tracks.size())))
            return;
        auto& track = tracks[static_cast<size_t>(trackIndex)];
        const int slotCount = static_cast<int>(track.effectSlots.size());
        if (!juce::isPositiveAndBelow(fromIndex, slotCount)
            || !juce::isPositiveAndBelow(toIndex, slotCount)
            || fromIndex == toIndex)
            return;

        auto moving = std::move(track.effectSlots[static_cast<size_t>(fromIndex)]);
        track.effectSlots.erase(track.effectSlots.begin() + fromIndex);
        track.effectSlots.insert(track.effectSlots.begin() + toIndex,
                                 std::move(moving));
        trackId = track.id;
        effectOrder.reserve(track.effectSlots.size());
        for (const auto& effect : track.effectSlots)
            effectOrder.push_back(effect.id);
    }

    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        reorderTrackPluginSlotsLocked(trackId, effectOrder);
    }
    playbackTransformResetRequested.store(true, std::memory_order_release);
    markDirty();
    postStatusMessage(utf8(u8"VST3エフェクトの順番を変更しました。"), false);
    if (onStateChanged)
        onStateChanged();
}

void AudioEngine::setEffectSlotBypassed(int trackIndex,
                                        int slotIndex,
                                        bool shouldBeBypassed)
{
    juce::String trackId;
    juce::String effectSlotId;
    bool changed = false;
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex,
                                      static_cast<int>(tracks.size())))
            return;
        auto& track = tracks[static_cast<size_t>(trackIndex)];
        if (!juce::isPositiveAndBelow(slotIndex,
                                      static_cast<int>(track.effectSlots.size())))
            return;
        auto& effect = track.effectSlots[static_cast<size_t>(slotIndex)];
        trackId = track.id;
        effectSlotId = effect.id;
        changed = effect.bypassed != shouldBeBypassed;
        effect.bypassed = shouldBeBypassed;
    }
    if (!changed)
        return;

    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        for (auto& slot : trackPlugins)
            if (slot != nullptr && slot->trackId == trackId
                && slot->effectSlotId == effectSlotId)
            {
                slot->bypassed.store(shouldBeBypassed,
                                     std::memory_order_relaxed);
                break;
            }
    }

    refreshTrackPluginLatencies();
    cachedDuration.store(getTracksDuration(getTracksSnapshot()));
    markDirty();
    postStatusMessage(shouldBeBypassed
                          ? utf8(u8"VST3エフェクトをバイパスしました。")
                          : utf8(u8"VST3エフェクトを有効にしました。"),
                      false);
    if (onStateChanged)
        onStateChanged();
}

void AudioEngine::setEffectChainBypassed(int trackIndex,
                                         bool shouldBeBypassed)
{
    juce::String trackId;
    bool changed = false;
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex,
                                      static_cast<int>(tracks.size())))
            return;
        auto& track = tracks[static_cast<size_t>(trackIndex)];
        trackId = track.id;
        changed = track.effectChainBypassed != shouldBeBypassed;
        track.effectChainBypassed = shouldBeBypassed;
    }
    if (!changed)
        return;

    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        for (auto& slot : trackPlugins)
            if (slot != nullptr && slot->trackId == trackId
                && slot->effectSlotId.isNotEmpty())
            {
                slot->effectChainBypassed.store(shouldBeBypassed,
                                                std::memory_order_relaxed);
            }
    }

    refreshTrackPluginLatencies();
    cachedDuration.store(getTracksDuration(getTracksSnapshot()));
    markDirty();
    postStatusMessage(shouldBeBypassed
                          ? utf8(u8"VST3エフェクトチェーンをバイパスしました。")
                          : utf8(u8"VST3エフェクトチェーンを有効にしました。"),
                      false);
    if (onStateChanged)
        onStateChanged();
}

void AudioEngine::showEffectSlotEditor(int trackIndex, int slotIndex)
{
    juce::String trackId;
    juce::String effectSlotId;
    juce::String pluginName;
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex,
                                      static_cast<int>(tracks.size())))
            return;
        const auto& track = tracks[static_cast<size_t>(trackIndex)];
        if (!juce::isPositiveAndBelow(slotIndex,
                                      static_cast<int>(track.effectSlots.size())))
            return;
        const auto& effect = track.effectSlots[static_cast<size_t>(slotIndex)];
        trackId = track.id;
        effectSlotId = effect.id;
        pluginName = effect.name;
    }

    if (pluginEditorWindow != nullptr && pluginEditorTrackId == trackId
        && pluginEditorEffectSlotId == effectSlotId)
    {
        pluginEditorWindow->setVisible(true);
        pluginEditorWindow->toFront(true);
        return;
    }

    closeVst3PluginEditor();
    juce::AudioProcessorEditor* editor = nullptr;
    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        for (auto& slot : trackPlugins)
            if (slot != nullptr && slot->trackId == trackId
                && slot->effectSlotId == effectSlotId
                && slot->instance != nullptr)
            {
                editor = slot->instance->createEditorIfNeeded();
                break;
            }
    }
    if (editor == nullptr)
    {
        postStatusMessage(
            utf8(u8"このVST3には開ける設定画面がありません。"), true);
        return;
    }

    const auto safeThis = juce::WeakReference<AudioEngine>(this);
    pluginEditorWindow = std::make_unique<PluginEditorWindow>(
        pluginName.isNotEmpty() ? pluginName : utf8(u8"VST3設定"),
        editor,
        [safeThis, trackId, effectSlotId]() mutable
        {
            if (safeThis != nullptr)
                safeThis->hideVst3PluginEditor(trackId, effectSlotId);
        },
        [safeThis]
        {
            if (safeThis == nullptr)
                return;
            if (safeThis->getPlaybackState() == PlaybackState::Stopped)
                safeThis->play();
            else
                safeThis->stop();
        },
        [safeThis](int direction)
        {
            if (safeThis == nullptr)
                return;
            const auto& metronome = safeThis->getMetronome();
            const double bpm = std::max(1, metronome.getBpm());
            const double numerator = std::max(1, metronome.getNumerator());
            const double denominator = std::max(1, metronome.getDenominator());
            const double step = (60.0 / bpm) * numerator / denominator;
            safeThis->setCurrentTime(juce::jlimit(
                0.0,
                safeThis->getDuration(),
                safeThis->getCurrentTime() + direction * step));
        });
    pluginEditorTrackId = trackId;
    pluginEditorEffectSlotId = effectSlotId;
}

juce::File AudioEngine::getDefaultVst3PluginDirectory() const
{
    juce::File firstExistingDirectory;

    for (int i = 0; i < pluginFormatManager.getNumFormats(); ++i)
    {
        auto* format = pluginFormatManager.getFormat(i);
        if (format == nullptr || !format->getName().equalsIgnoreCase("VST3"))
            continue;

        const auto paths = format->getDefaultLocationsToSearch();
        for (int pathIndex = 0; pathIndex < paths.getNumPaths(); ++pathIndex)
        {
            const auto path = paths[pathIndex];
            if (!path.isDirectory())
                continue;

            if (firstExistingDirectory == juce::File())
                firstExistingDirectory = path;

            if (!path.findChildFiles(juce::File::findFilesAndDirectories,
                                     true,
                                     "*.vst3").isEmpty())
                return path;
        }
    }

    if (firstExistingDirectory != juce::File())
        return firstExistingDirectory;

    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
}

juce::File AudioEngine::findInstalledPitchNet() const
{
    juce::Array<juce::File> candidates;
#if JUCE_WINDOWS
    candidates.add(juce::File("C:\\Program Files\\Common Files\\VST3")
        .getChildFile("Session Loops").getChildFile("PitchNet.vst3"));
    candidates.add(juce::File("C:\\Program Files\\Common Files\\VST3")
        .getChildFile("PitchNet.vst3"));
    candidates.add(juce::File("C:\\Program Files (x86)\\Common Files\\VST3")
        .getChildFile("PitchNet.vst3"));
#elif JUCE_MAC
    candidates.add(juce::File("/Library/Audio/Plug-Ins/VST3/PitchNet.vst3"));
    candidates.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
        .getChildFile("Library/Audio/Plug-Ins/VST3/PitchNet.vst3"));
#endif
    for (const auto& candidate : candidates)
        if (candidate.exists())
            return candidate;

    for (const auto& root : { getDefaultVst3PluginDirectory() })
    {
        if (!root.isDirectory())
            continue;
        juce::Array<juce::File> found;
        root.findChildFiles(found,
                            juce::File::findFilesAndDirectories,
                            true,
                            "*PitchNet*.vst3");
        if (!found.isEmpty())
            return found.getFirst();
    }
    return {};
}

bool AudioEngine::isPitchCorrectionActive(int trackIndex) const
{
    juce::String trackId;
    {
        const juce::ScopedLock lock(tracksLock);
        if (!juce::isPositiveAndBelow(trackIndex,
                                      static_cast<int>(tracks.size())))
            return false;
        const auto& track = tracks[static_cast<size_t>(trackIndex)];
        if (!track.pitchCorrection.enabled
            || !track.vst3Name.containsIgnoreCase("PitchNet"))
            return false;
        trackId = track.id;
    }

    const juce::SpinLock::ScopedLockType lock(pluginLock);
    return std::any_of(trackPlugins.begin(), trackPlugins.end(),
        [&trackId](const std::unique_ptr<TrackPluginSlot>& slot)
        {
            return slot != nullptr
                && slot->trackId == trackId
                && slot->effectSlotId.isEmpty()
                && slot->instance != nullptr
                && slot->processingInstance != nullptr
                && slot->processingChannels > 0
                && slot->description.name.containsIgnoreCase("PitchNet")
                && (!slot->description.hasARAExtension
                    || (slot->araHost != nullptr
                        && slot->araHost->isInitialised()));
        });
}

void AudioEngine::markPitchCorrectionUnavailable(const juce::String& trackId)
{
    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        const bool runtimeStillAvailable = std::any_of(
            trackPlugins.begin(), trackPlugins.end(),
            [&trackId](const std::unique_ptr<TrackPluginSlot>& slot)
            {
                return slot != nullptr
                    && slot->trackId == trackId
                    && slot->effectSlotId.isEmpty()
                    && slot->instance != nullptr
                    && slot->processingInstance != nullptr
                    && slot->processingChannels > 0
                    && slot->description.name.containsIgnoreCase("PitchNet");
            });
        if (runtimeStillAvailable)
            return;
    }

    bool changed = false;
    {
        const juce::ScopedLock lock(tracksLock);
        auto track = std::find_if(tracks.begin(), tracks.end(),
            [&trackId](const TrackData& candidate)
            {
                return candidate.id == trackId;
            });
        if (track == tracks.end()
            || !track->vst3Name.containsIgnoreCase("PitchNet"))
            return;

        changed = track->pitchCorrection.enabled
            || track->pitchCorrection.auditionCorrected
            || track->vst3AraPlaybackEnabled
            || !track->vst3Bypassed;
        track->pitchCorrection.enabled = false;
        track->pitchCorrection.auditionCorrected = false;
        track->vst3AraPlaybackEnabled = false;
        track->vst3Bypassed = true;
    }

    if (changed)
    {
        markDirty();
        if (onStateChanged)
            onStateChanged();
    }
}

void AudioEngine::applyNaturalPitchCorrection(int trackIndex)
{
    const auto snapshot = getTracksSnapshot();
    if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(snapshot.size())))
        return;

    auto settings = snapshot[static_cast<size_t>(trackIndex)].pitchCorrection;
    const auto project = getProjectDawSettings();
    settings.enabled = true;
    settings.auditionCorrected = true;
    settings.key = project.musicalKey;
    settings.scale = project.musicalScale;
    settings.strength = project.pitchCorrectionStrength;

    const bool hasPitchNetMetadata =
        snapshot[static_cast<size_t>(trackIndex)].vst3Name
            .containsIgnoreCase("PitchNet");
    if (!hasPitchNetMetadata)
    {
        const auto pitchNet = findInstalledPitchNet();
        if (!pitchNet.exists())
        {
            postStatusMessage(
                utf8(u8"PitchNetが見つかりません。公式サイトからPitchNet VST3をインストールしてください: https://sessionloops.com/pitchnet"),
                true);
            return;
        }

        postStatusMessage(
            utf8(u8"PitchNetを読み込み、自然な補正を準備しています。"), false);
        scanVst3PluginForTrack(trackIndex, pitchNet, false, true);
        return;
    }

    const auto trackId = snapshot[static_cast<size_t>(trackIndex)].id;
    bool runtimeReady = false;
    bool automationSupported = false;
    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        for (auto& slot : trackPlugins)
        {
            if (slot->trackId != trackId || slot->effectSlotId.isNotEmpty()
                || slot->instance == nullptr
                || !slot->description.name.containsIgnoreCase("PitchNet"))
                continue;

            runtimeReady = slot->processingInstance != nullptr
                && slot->processingChannels > 0
                && (!slot->description.hasARAExtension
                    || (slot->araHost != nullptr
                        && slot->araHost->isInitialised()));
            bool hasPitchCenter = false;
            bool hasSnap = false;
            bool hasKey = false;
            bool hasScale = false;
            bool hasMacro = false;
            for (auto* parameter : slot->instance->getParameters())
            {
                if (parameter == nullptr)
                    continue;
                const auto name = parameter->getName(128).trim();
                if (name.containsIgnoreCase("Pitch Center"))
                    hasPitchCenter = true;
                else if (name.containsIgnoreCase("Snap to Scale"))
                    hasSnap = true;
                else if (name.containsIgnoreCase("Root")
                         || name.equalsIgnoreCase("Key"))
                    hasKey = true;
                else if (name.equalsIgnoreCase("Scale")
                         || name.containsIgnoreCase("Scale Type"))
                    hasScale = true;
                else if (name.containsIgnoreCase("Correct Pitch Macro"))
                    hasMacro = true;
            }
            automationSupported = hasMacro && hasPitchCenter
                && (settings.scale == MusicalScale::chromatic
                    || (hasSnap && hasKey && hasScale));
            break;
        }
    }

    if (!runtimeReady)
    {
        const auto pitchNet = findInstalledPitchNet();
        if (pitchNet.exists())
        {
            postStatusMessage(
                utf8(u8"PitchNetを再読み込みしています。補正は準備完了後にONになります。"),
                false);
            scanVst3PluginForTrack(trackIndex, pitchNet, false, true);
        }
        else
        {
            markPitchCorrectionUnavailable(trackId);
            postStatusMessage(
                utf8(u8"PitchNetを実行できないため、補正はONにしていません。"),
                true);
        }
        return;
    }

    if (!automationSupported)
    {
        postStatusMessage(
            utf8(u8"このPitchNet版では補正マクロを自動実行できないため、補正はONにしていません。詳細画面で「Correct Pitch Macro」→「Pitch Center / Snap to Scale」→「OK」を確認してください。"),
            false);
        showVst3PluginEditor(trackIndex);
        return;
    }

    // Capture the exact pre-change state so Undo can restore what was heard,
    // not only the labels shown in SimpleRec Pro.
    capturePluginStates();
    beginEdit();

    bool parametersApplied = false;
    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        for (auto& slot : trackPlugins)
        {
            if (slot->trackId != trackId || slot->effectSlotId.isNotEmpty()
                || slot->instance == nullptr
                || !slot->description.name.containsIgnoreCase("PitchNet"))
                continue;

            auto setNamedParameter = [&slot](const juce::String& exactName,
                                             const juce::String& partialName,
                                             float value)
            {
                for (auto* parameter : slot->instance->getParameters())
                {
                    if (parameter == nullptr)
                        continue;
                    const auto name = parameter->getName(128).trim();
                    if ((!exactName.isEmpty() && name.equalsIgnoreCase(exactName))
                        || (!partialName.isEmpty()
                            && name.containsIgnoreCase(partialName)))
                    {
                        parameter->beginChangeGesture();
                        parameter->setValueNotifyingHost(
                            juce::jlimit(0.0f, 1.0f, value));
                        parameter->endChangeGesture();
                        return true;
                    }
                }
                return false;
            };

            const bool pitchCenterApplied = setNamedParameter(
                {}, "Pitch Center", settings.strength);
            bool scaleSettingsApplied = true;
            if (settings.scale != MusicalScale::chromatic)
            {
                scaleSettingsApplied = setNamedParameter(
                    {}, "Snap to Scale", 1.0f);
                scaleSettingsApplied = setNamedParameter(
                    "Key", "Root", static_cast<float>(settings.key) / 11.0f)
                    && scaleSettingsApplied;
                scaleSettingsApplied = setNamedParameter(
                    "Scale", "Scale Type",
                    static_cast<float>(static_cast<int>(settings.scale)) / 5.0f)
                    && scaleSettingsApplied;
            }
            else
            {
                setNamedParameter({}, "Snap to Scale", 0.0f);
            }
            const bool macroTriggered = setNamedParameter(
                {}, "Correct Pitch Macro", 1.0f);
            parametersApplied = pitchCenterApplied
                && scaleSettingsApplied && macroTriggered;
            if (parametersApplied)
            {
                slot->bypassed.store(false, std::memory_order_relaxed);
                slot->stateRevision.fetch_add(1);
                slot->processingStateDirty.store(true);
            }
            break;
        }
    }

    if (!parametersApplied)
    {
        {
            const juce::ScopedLock lock(tracksLock);
            editInProgress = false;
        }
        postStatusMessage(
            utf8(u8"PitchNetの状態が変わったため、補正をONにできませんでした。もう一度お試しください。"),
            true);
        return;
    }

    setTrackPitchCorrectionSettings(trackIndex, settings);
    setTrackVst3Bypassed(trackIndex, false);
    syncAraProcessingState(trackId);
    capturePluginStates();
    endEdit();

    postStatusMessage(
        utf8(u8"PitchNetへ調・スケール・補正強度を渡し、自然な補正を実行しました。"),
        false);
}

void AudioEngine::showPitchCorrectionDetails(int trackIndex)
{
    const auto snapshot = getTracksSnapshot();
    if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(snapshot.size())))
        return;
    if (snapshot[static_cast<size_t>(trackIndex)].vst3Name
            .containsIgnoreCase("PitchNet"))
    {
        showVst3PluginEditor(trackIndex);
        return;
    }
    const auto pitchNet = findInstalledPitchNet();
    if (!pitchNet.exists())
    {
        postStatusMessage(
            utf8(u8"PitchNetが見つかりません。https://sessionloops.com/pitchnet からインストールしてください。"),
            true);
        return;
    }
    loadVst3PluginForTrack(trackIndex, pitchNet);
}

bool AudioEngine::capturePluginStates()
{
    struct CapturedPluginState
    {
        juce::String trackId;
        juce::String effectSlotId;
        std::shared_ptr<const juce::MemoryBlock> state;
        std::shared_ptr<const juce::MemoryBlock> araArchive;
        juce::String araArchiveId;
        bool araStateIsCurrent = false;
        bool araArchiveIsCurrent = false;
        bool araPlaybackEnabled = false;
        bool bypassed = false;
        bool pitchCorrectionRuntimeReady = false;
        int latencySamples = 0;
        double tailSeconds = 0.0;
    };
    std::vector<CapturedPluginState> capturedStates;
    bool allRequiredAraArchivesCaptured = true;
    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        capturedStates.reserve(trackPlugins.size());
        for (const auto& slot : trackPlugins)
        {
            if (slot == nullptr || slot->instance == nullptr)
                continue;

            const auto stateRevision = slot->stateRevision.load();
            if (slot->persistedState == nullptr
                || slot->capturedStateRevision != stateRevision)
            {
                auto state = std::make_shared<juce::MemoryBlock>();
                slot->instance->getStateInformation(*state);
                slot->persistedState = std::move(state);
                // If a plug-in reports another change while serialising, its
                // newer revision remains uncaptured for the next save.
                slot->capturedStateRevision = stateRevision;
            }
            const bool stateIsCurrent = slot->persistedState != nullptr
                && slot->capturedStateRevision == slot->stateRevision.load();

            if (slot->processingInstance != nullptr
                && slot->processingInstance != slot->instance
                && !slot->useEditorInstanceForPlayback.load()
                && slot->processingStateDirty.load())
            {
                slot->processingInstance->releaseResources();
                const auto* state = slot->persistedState.get();
                if (state != nullptr
                    && state->getSize() <= static_cast<size_t>(std::numeric_limits<int>::max()))
                {
                    slot->processingInstance->setStateInformation(
                        state->getData(), static_cast<int>(state->getSize()));
                }
                slot->processingChannels = preparePluginInstance(
                    *slot->processingInstance,
                    std::max(1.0, currentSampleRate.load()),
                    std::max(1, currentBlockSize));
                slot->processingStateDirty.store(false);
            }
            const bool araArchiveRequired = slot->araHost != nullptr
                && slot->araPlaybackEnabled.load();
            bool archiveIsCurrent = false;
            if (araArchiveRequired)
            {
                const auto araRevision = slot->araRevision.load();
                archiveIsCurrent = slot->persistedAraArchive != nullptr
                    && !slot->persistedAraArchive->isEmpty()
                    && slot->capturedAraRevision == araRevision;
                if (!archiveIsCurrent)
                {
                    auto archive = std::make_shared<juce::MemoryBlock>();
                    juce::String archiveId;
                    if (slot->araHost->captureArchive(*archive, archiveId)
                        && !archive->isEmpty())
                    {
                        slot->persistedAraArchive = std::move(archive);
                        slot->persistedAraArchiveId = std::move(archiveId);
                        slot->capturedAraRevision = araRevision;
                    }
                }

                archiveIsCurrent = slot->persistedAraArchive != nullptr
                    && !slot->persistedAraArchive->isEmpty()
                    && slot->capturedAraRevision == slot->araRevision.load();
                if (!archiveIsCurrent)
                    allRequiredAraArchivesCaptured = false;
            }
            const auto tailInstance = slot->processingInstance != nullptr
                ? slot->processingInstance : slot->instance;
            const double reportedTail = tailInstance->getTailLengthSeconds();
            const double tailSeconds = std::isfinite(reportedTail)
                ? juce::jlimit(0.0, 60.0, reportedTail) : 0.0;
            const int latencySamples = juce::jlimit(
                0, 1920000, tailInstance->getLatencySamples());
            slot->latencySamples.store(latencySamples,
                                       std::memory_order_relaxed);
            capturedStates.push_back({
                slot->trackId,
                slot->effectSlotId,
                slot->persistedState,
                slot->persistedAraArchive,
                slot->persistedAraArchiveId,
                araArchiveRequired && stateIsCurrent,
                archiveIsCurrent,
                slot->araPlaybackEnabled.load(),
                slot->bypassed.load(std::memory_order_relaxed),
                slot->effectSlotId.isEmpty()
                    && slot->description.name.containsIgnoreCase("PitchNet")
                    && slot->processingInstance != nullptr
                    && slot->processingChannels > 0
                    && (!slot->description.hasARAExtension
                        || (slot->araHost != nullptr
                            && slot->araHost->isInitialised())),
                latencySamples,
                tailSeconds
            });
        }
    }

    {
        const juce::ScopedLock lock(tracksLock);
        for (auto& track : tracks)
        {
            if (!track.pitchCorrection.enabled
                || !track.vst3Name.containsIgnoreCase("PitchNet"))
                continue;
            const bool hasRuntime = std::any_of(
                capturedStates.begin(), capturedStates.end(),
                [&track](const CapturedPluginState& captured)
                {
                    return captured.trackId == track.id
                        && captured.effectSlotId.isEmpty()
                        && captured.pitchCorrectionRuntimeReady;
                });
            if (!hasRuntime)
            {
                track.pitchCorrection.enabled = false;
                track.pitchCorrection.auditionCorrected = false;
                track.vst3AraPlaybackEnabled = false;
                track.vst3Bypassed = true;
            }
        }
        for (const auto& captured : capturedStates)
        {
            auto track = std::find_if(tracks.begin(), tracks.end(),
                [&captured](const TrackData& candidate)
                {
                    return candidate.id == captured.trackId;
            });
            if (track != tracks.end())
            {
                if (captured.effectSlotId.isNotEmpty())
                {
                    const auto effect = std::find_if(
                        track->effectSlots.begin(), track->effectSlots.end(),
                        [&captured](const EffectSlotData& candidate)
                        {
                            return candidate.id == captured.effectSlotId;
                        });
                    if (effect != track->effectSlots.end())
                    {
                        effect->state = captured.state;
                        effect->bypassed = captured.bypassed;
                        effect->latencySamples = captured.latencySamples;
                        effect->tailSeconds = captured.tailSeconds;
                    }
                }
                else
                {
                    track->vst3State = captured.state;
                    if (captured.araArchiveIsCurrent)
                    {
                        track->vst3AraArchive = captured.araArchive;
                        track->vst3AraArchiveId = captured.araArchiveId;
                        if (captured.araStateIsCurrent)
                        {
                            track->vst3AraHostFormatVersion =
                                TrackData::currentVst3AraHostFormatVersion;
                        }
                    }
                    track->vst3AraPlaybackEnabled =
                        captured.araPlaybackEnabled;
                    track->vst3Bypassed = captured.bypassed;
                    track->vst3LatencySamples = captured.latencySamples;
                    track->vst3TailSeconds = captured.tailSeconds;
                }
            }
        }
    }
    refreshTrackPluginLatencies();
    cachedDuration.store(getTracksDuration(getTracksSnapshot()));
    return allRequiredAraArchivesCaptured;
}

void AudioEngine::syncAraProcessingState(const juce::String& trackId)
{
    const juce::SpinLock::ScopedLockType lock(pluginLock);
    for (auto& slot : trackPlugins)
    {
        if (slot->instance == nullptr || slot->processingInstance == nullptr
            || slot->processingInstance == slot->instance
            || slot->useEditorInstanceForPlayback.load()
            || slot->auditionWithEditorInstance.load()
            || (trackId.isNotEmpty() && slot->trackId != trackId)
            || !slot->processingStateDirty.load())
            continue;

        const auto stateRevision = slot->stateRevision.load();
        auto state = std::make_shared<juce::MemoryBlock>();
        slot->instance->getStateInformation(*state);
        if (state->getSize() > static_cast<size_t>(std::numeric_limits<int>::max()))
            continue;

        slot->processingInstance->releaseResources();
        slot->processingInstance->setStateInformation(
            state->getData(), static_cast<int>(state->getSize()));
        slot->processingChannels = preparePluginInstance(
            *slot->processingInstance,
            std::max(1.0, currentSampleRate.load()),
            std::max(1, currentBlockSize));
        slot->persistedState = std::move(state);
        slot->capturedStateRevision = stateRevision;
        slot->processingStateDirty.store(slot->stateRevision.load() != stateRevision);
    }
}

void AudioEngine::queueAraTrackSync()
{
    if (araTrackSyncPending.exchange(true))
        return;

    const auto safeThis = juce::WeakReference<AudioEngine>(this);
    if (!juce::MessageManager::callAsync(
            [safeThis]() mutable
            {
                if (safeThis == nullptr || !safeThis->araTrackSyncPending.exchange(false))
                    return;
                safeThis->syncAraTracksToModel();
            }))
        araTrackSyncPending.store(false);
}

void AudioEngine::syncAraTracksToModel()
{
    const auto trackSnapshots = getTracksSnapshot();
    juce::StringArray failedPlugins;

    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        for (auto& slot : trackPlugins)
        {
            if (slot->araHost == nullptr || slot->instance == nullptr)
                continue;

            const auto track = std::find_if(trackSnapshots.begin(), trackSnapshots.end(),
                [&slot](const TrackData& candidate)
                {
                    return candidate.id == slot->trackId;
                });
            if (track == trackSnapshots.end() || slot->araHost->matchesTrack(*track))
                continue;

            slot->instance->releaseResources();
            if (slot->processingInstance != nullptr
                && slot->processingInstance != slot->instance)
                slot->processingInstance->releaseResources();

            const bool updated = slot->araHost->setTrack(*track);
            slot->editorChannels = preparePluginInstance(
                *slot->instance,
                std::max(1.0, currentSampleRate.load()),
                std::max(1, currentBlockSize));
            if (slot->processingInstance != nullptr
                && slot->processingInstance != slot->instance)
            {
                slot->processingChannels = preparePluginInstance(
                    *slot->processingInstance,
                    std::max(1.0, currentSampleRate.load()),
                    std::max(1, currentBlockSize));
            }
            else
            {
                slot->processingChannels = slot->editorChannels;
            }

            if (updated)
            {
                slot->araRevision.fetch_add(1);
            }
            else
            {
                failedPlugins.addIfNotAlreadyThere(slot->description.name);
            }
        }
    }

    for (const auto& pluginName : failedPlugins)
    {
        postStatusMessage(
            utf8(u8"編集した音源をARAプラグインへ更新できませんでした: ") + pluginName,
            true);
    }
}

void AudioEngine::setPluginEditorTransportActive(const juce::String& trackId, bool active)
{
    const juce::SpinLock::ScopedLockType lock(pluginLock);
    for (auto& slot : trackPlugins)
    {
        if (slot->trackId == trackId)
        {
            slot->editorTransportActive.store(active);
            break;
        }
    }
}

void AudioEngine::pumpOpenPluginEditorTransport(int numSamples) noexcept
{
    if (numSamples <= 0 || pluginEditorTransportBuffer.getNumSamples() < numSamples)
        return;

    const juce::SpinLock::ScopedTryLockType lock(pluginLock);
    if (!lock.isLocked())
        return;

    for (auto& slot : trackPlugins)
    {
        const bool isPitchNet = slot->description.hasARAExtension
            && slot->description.name.containsIgnoreCase("PitchNet");
        if (!isPitchNet
            || !slot->editorTransportActive.load()
            || slot->instance == nullptr
            || slot->processingInstance == slot->instance
            || slot->editorChannels <= 0)
            continue;

        slot->playHead.update(playbackPositionInSamples.load(),
                              std::max(1.0, currentSampleRate.load()),
                              playbackState.load());
        pluginEditorTransportBuffer.clear(0, numSamples);
        juce::MidiBuffer midi;

        if (slot->editorChannels == 1)
        {
            juce::AudioBuffer<float> monoView(
                pluginEditorTransportBuffer.getArrayOfWritePointers(),
                1,
                0,
                numSamples);
            slot->instance->processBlock(monoView, midi);
        }
        else
        {
            juce::AudioBuffer<float> stereoView(
                pluginEditorTransportBuffer.getArrayOfWritePointers(),
                2,
                0,
                numSamples);
            slot->instance->processBlock(stereoView, midi);
        }

        // SimpleRec Pro only displays one plug-in editor at a time.
        break;
    }
}

void AudioEngine::handleAraDocumentChanged(const juce::String& trackId)
{
    bool acceptedUpdate = false;
    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        for (auto& slot : trackPlugins)
        {
            if (slot->trackId != trackId)
                continue;
            if (!slot->araEditorWasOpened.load())
                return;
            slot->araPlaybackEnabled.store(true);
            slot->araRevision.fetch_add(1);
            // Some plug-ins mirror ARA edits into their conventional state.
            // Mark both payloads so the next save cannot retain stale data.
            slot->stateRevision.fetch_add(1);
            if (!slot->processingUsesAra)
                slot->useEditorInstanceForPlayback.store(true);
            acceptedUpdate = true;
            break;
        }
    }

    if (!acceptedUpdate)
        return;

    {
        const juce::ScopedLock lock(tracksLock);
        auto track = std::find_if(tracks.begin(), tracks.end(),
            [&trackId](const TrackData& candidate) { return candidate.id == trackId; });
        if (track != tracks.end())
        {
            track->vst3AraPlaybackEnabled = true;
            track->pitchCorrection.enabled = true;
            track->pitchCorrection.auditionCorrected = true;
        }
    }

    markDirty();
    if (onStateChanged)
        onStateChanged();
}

void AudioEngine::loadTrackPlugin(const juce::String& trackId,
                                  const juce::PluginDescription& description,
                                  std::shared_ptr<const juce::MemoryBlock> savedState,
                                  std::shared_ptr<const juce::MemoryBlock> savedAraArchive,
                                  const juce::String& savedAraArchiveId,
                                  bool araPlaybackEnabled,
                                  bool shouldBeBypassed,
                                  bool markProjectDirty,
                                  bool openEditorAfterLoad,
                                  bool applyNaturalAfterLoad)
{
    const auto generation = pluginGeneration.load();
    const auto safeThis = juce::WeakReference<AudioEngine>(this);
    pluginFormatManager.createPluginInstanceAsync(
        description,
        std::max(1.0, currentSampleRate.load()),
        std::max(1, currentBlockSize),
        [safeThis, trackId, description, savedState, savedAraArchive,
         savedAraArchiveId, araPlaybackEnabled, shouldBeBypassed,
         markProjectDirty, openEditorAfterLoad, applyNaturalAfterLoad,
         generation]
        (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error) mutable
        {
            if (safeThis == nullptr || safeThis->pluginGeneration.load() != generation)
                return;

            if (instance == nullptr || error.isNotEmpty())
            {
                if (description.name.containsIgnoreCase("PitchNet"))
                    safeThis->markPitchCorrectionUnavailable(trackId);
                safeThis->postStatusMessage(
                    utf8(u8"VST3を読み込めませんでした: ")
                        + (error.isNotEmpty() ? error : description.name),
                    true);
                return;
            }

            const int channels = configurePluginLayout(*instance);
            if (channels == 0)
            {
                if (description.name.containsIgnoreCase("PitchNet"))
                    safeThis->markPitchCorrectionUnavailable(trackId);
                safeThis->postStatusMessage(
                    utf8(u8"このVST3はモノラルまたはステレオの音声エフェクトとして使えません。"),
                    true);
                return;
            }

            auto sharedInstance = std::shared_ptr<juce::AudioPluginInstance>(std::move(instance));
            auto finishInstallation =
                [safeThis, sharedInstance, trackId, description, savedState,
                 savedAraArchive, savedAraArchiveId, araPlaybackEnabled,
                 shouldBeBypassed, markProjectDirty, openEditorAfterLoad,
                 applyNaturalAfterLoad, generation, channels]
                (juce::ARAFactoryWrapper araFactory,
                                       std::shared_ptr<juce::AudioPluginInstance>
                                           processingInstance) mutable
                {
                    if (safeThis == nullptr || safeThis->pluginGeneration.load() != generation)
                        return;

                    TrackData trackSnapshot;
                    int trackIndex = -1;
                    {
                        const juce::ScopedLock lock(safeThis->tracksLock);
                        for (size_t i = 0; i < safeThis->tracks.size(); ++i)
                        {
                            if (safeThis->tracks[i].id == trackId)
                            {
                                trackSnapshot = safeThis->tracks[i];
                                trackIndex = static_cast<int>(i);
                                break;
                            }
                        }
                    }
                    if (trackIndex < 0)
                        return;

                    const bool hasSavedAraArchive = savedAraArchive != nullptr
                        && !savedAraArchive->isEmpty();
                    const bool needsAraDataMigration = description.hasARAExtension
                        && araPlaybackEnabled
                        && hasSavedAraArchive
                        && trackSnapshot.vst3AraHostFormatVersion
                            < TrackData::currentVst3AraHostFormatVersion;
                    std::shared_ptr<const juce::MemoryBlock> stateForRestore = savedState;
                    if (savedState != nullptr && !savedState->isEmpty()
                        && savedAraArchiveId == PitchNetArchiveFilter::archiveId)
                    {
                        auto filteredState = std::make_shared<juce::MemoryBlock>(
                            *savedState);
                        const auto filterResult =
                            PitchNetArchiveFilter::filterVst3StateInPlace(
                                *filteredState,
                                savedAraArchiveId,
                                PitchNetArchiveFilter::makeModificationRanges(
                                    trackSnapshot));
                        if (filterResult.recognised && filterResult.changed)
                            stateForRestore = std::move(filteredState);
                    }
                    const bool processingUsesAra = description.hasARAExtension;
                    auto newSlot = std::make_unique<TrackPluginSlot>(*safeThis,
                                                                     trackId,
                                                                     juce::String(),
                                                                     description,
                                                                     sharedInstance,
                                                                     processingInstance,
                                                                     channels,
                                                                     processingUsesAra,
                                                                     araPlaybackEnabled,
                                                                     shouldBeBypassed,
                                                                     stateForRestore,
                                                                     savedAraArchive,
                                                                     savedAraArchiveId,
                                                                     !needsAraDataMigration);
                    if (description.hasARAExtension)
                    {
                        if (araFactory.get() == nullptr)
                        {
                            if (description.name.containsIgnoreCase("PitchNet"))
                                safeThis->markPitchCorrectionUnavailable(trackId);
                            safeThis->postStatusMessage(
                                utf8(u8"このプラグインのARA機能を開始できませんでした: ")
                                    + description.name,
                                true);
                            return;
                        }

                        newSlot->araHost = std::make_unique<AraTrackHost>(*sharedInstance,
                                                                         processingUsesAra
                                                                             ? processingInstance.get()
                                                                             : nullptr,
                                                                         std::move(araFactory),
                                                                         AraTrackHost::TransportCallbacks {
                                                                             [safeThis, trackId]
                                                                             {
                                                                                 if (safeThis != nullptr)
                                                                                     safeThis->playFromAraEditor(trackId);
                                                                             },
                                                                             [safeThis]
                                                                             {
                                                                                 if (safeThis != nullptr)
                                                                                     safeThis->stop();
                                                                             },
                                                                             [safeThis](double seconds)
                                                                             {
                                                                                 if (safeThis != nullptr)
                                                                                     safeThis->setCurrentTime(seconds);
                                                                             },
                                                                             [safeThis, trackId]
                                                                             {
                                                                                 if (safeThis != nullptr)
                                                                                     safeThis->handleAraDocumentChanged(trackId);
                                                                              }
                                                                          },
                                                                         savedAraArchiveId);
                    }

                    if (stateForRestore != nullptr && !stateForRestore->isEmpty())
                    {
                        if (stateForRestore->getSize()
                            > static_cast<size_t>(std::numeric_limits<int>::max()))
                        {
                            if (description.name.containsIgnoreCase("PitchNet"))
                                safeThis->markPitchCorrectionUnavailable(trackId);
                            safeThis->postStatusMessage(
                                utf8(u8"保存されたVST3状態が大きすぎます: ")
                                    + description.name,
                                true);
                            return;
                        }

                        newSlot->suppressStateChangeNotifications.store(true);
                        sharedInstance->setStateInformation(
                            stateForRestore->getData(),
                            static_cast<int>(stateForRestore->getSize()));
                        if (processingInstance != nullptr
                            && processingInstance != sharedInstance)
                        {
                            processingInstance->setStateInformation(
                                stateForRestore->getData(),
                                static_cast<int>(stateForRestore->getSize()));
                        }
                        newSlot->suppressStateChangeNotifications.store(false);
                    }

                    const juce::MemoryBlock emptyArchive;
                    const auto& archiveForRestore = savedAraArchive != nullptr
                        ? *savedAraArchive : emptyArchive;
                    if (newSlot->araHost != nullptr
                        && !newSlot->araHost->initialiseTrack(trackSnapshot,
                                                              archiveForRestore))
                    {
                        if (description.name.containsIgnoreCase("PitchNet"))
                            safeThis->markPitchCorrectionUnavailable(trackId);
                        safeThis->postStatusMessage(
                            utf8(u8"トラック音声をARAプラグインへ渡せませんでした: ")
                                + description.name,
                            true);
                        return;
                    }
                    if (newSlot->araHost != nullptr
                        && savedAraArchive != nullptr
                        && !savedAraArchive->isEmpty()
                        && !newSlot->araHost->wasArchiveRestored())
                    {
                        if (description.name.containsIgnoreCase("PitchNet"))
                            safeThis->markPitchCorrectionUnavailable(trackId);
                        safeThis->postStatusMessage(
                            utf8(u8"保存されたARA編集内容を復元できませんでした: ")
                                + description.name,
                            true);
                        return;
                    }

                    if (needsAraDataMigration)
                    {
                        // Restoring an older PitchNet document can update both
                        // its ordinary VST3 state and its ARA document. Force
                        // both payloads to be recaptured before format 4 is
                        // written, even though the editor is not open yet.
                        newSlot->stateRevision.fetch_add(1);
                        newSlot->araRevision.fetch_add(1);
                    }

                    newSlot->editorChannels = preparePluginInstance(
                        *sharedInstance,
                        std::max(1.0, safeThis->currentSampleRate.load()),
                        std::max(1, safeThis->currentBlockSize));
                    newSlot->processingChannels = processingInstance == sharedInstance
                        ? newSlot->editorChannels
                        : preparePluginInstance(
                            *processingInstance,
                            std::max(1.0, safeThis->currentSampleRate.load()),
                            std::max(1, safeThis->currentBlockSize));
                    if (newSlot->processingChannels == 0)
                    {
                        if (description.name.containsIgnoreCase("PitchNet"))
                            safeThis->markPitchCorrectionUnavailable(trackId);
                        safeThis->postStatusMessage(
                            utf8(u8"VST3の音声処理を開始できませんでした: ") + description.name,
                            true);
                        return;
                    }

                    newSlot->processingStateDirty.store(false);
                    const double reportedTail = processingInstance->getTailLengthSeconds();
                    const double tailSeconds = std::isfinite(reportedTail)
                        ? juce::jlimit(0.0, 60.0, reportedTail)
                        : 0.0;
                    const int latencySamples = juce::jlimit(
                        0, 1920000, processingInstance->getLatencySamples());
                    newSlot->latencySamples.store(latencySamples,
                                                  std::memory_order_relaxed);
                    juce::String descriptionXml;
                    if (auto xml = description.createXml())
                        descriptionXml = xml->toString();
                    const int hostFormatVersion =
                        TrackData::hostFormatVersionForPluginLoad(
                            description.hasARAExtension,
                            hasSavedAraArchive,
                            trackSnapshot.vst3AraHostFormatVersion);

                    std::vector<juce::String> effectOrder;
                    {
                        const juce::ScopedLock lock(safeThis->tracksLock);
                        auto track = std::find_if(safeThis->tracks.begin(),
                                                  safeThis->tracks.end(),
                            [&trackId](const TrackData& candidate)
                            {
                                return candidate.id == trackId;
                            });
                        if (track == safeThis->tracks.end())
                            return;
                        track->vst3Name = description.name;
                        track->vst3DescriptionXml = descriptionXml;
                        track->vst3State = stateForRestore;
                        track->vst3AraArchive = savedAraArchive;
                        track->vst3AraArchiveId = savedAraArchiveId;
                        track->vst3AraHostFormatVersion = hostFormatVersion;
                        track->vst3AraPlaybackEnabled = araPlaybackEnabled;
                        track->vst3Bypassed = shouldBeBypassed;
                        track->vst3LatencySamples = latencySamples;
                        track->vst3TailSeconds = tailSeconds;
                        effectOrder.reserve(track->effectSlots.size());
                        for (const auto& effect : track->effectSlots)
                            effectOrder.push_back(effect.id);
                    }

                    if (safeThis->pluginEditorTrackId == trackId)
                        safeThis->closeVst3PluginEditor(false);

                    {
                        const juce::SpinLock::ScopedLockType lock(safeThis->pluginLock);
                        safeThis->trackPlugins.erase(
                            std::remove_if(safeThis->trackPlugins.begin(),
                                           safeThis->trackPlugins.end(),
                                [&trackId](const std::unique_ptr<TrackPluginSlot>& slot)
                                {
                                    return slot->trackId == trackId
                                        && slot->effectSlotId.isEmpty();
                                }),
                        safeThis->trackPlugins.end());
                        safeThis->trackPlugins.push_back(std::move(newSlot));
                        safeThis->reorderTrackPluginSlotsLocked(trackId,
                                                                 effectOrder);
                    }

                    safeThis->refreshTrackPluginLatencies();

                    if (markProjectDirty || needsAraDataMigration)
                        safeThis->markDirty();
                    safeThis->cachedDuration.store(
                        getTracksDuration(safeThis->getTracksSnapshot()));
                    safeThis->postStatusMessage(
                        (description.hasARAExtension
                             ? utf8(u8"ARAでトラック音声を渡しました: ")
                             : utf8(u8"VST3を読み込みました: "))
                            + description.name,
                        false);
                    if (safeThis->onStateChanged)
                        safeThis->onStateChanged();
                    if (description.name.containsIgnoreCase("PitchNet")
                        && (applyNaturalAfterLoad
                            || (markProjectDirty
                                && trackSnapshot.pitchCorrection.enabled)))
                    {
                        juce::MessageManager::callAsync(
                            [safeThis, trackIndex]
                            {
                                if (safeThis != nullptr)
                                    safeThis->applyNaturalPitchCorrection(trackIndex);
                            });
                    }
                    if (openEditorAfterLoad)
                        safeThis->showVst3PluginEditor(trackIndex);
                };

            if (description.hasARAExtension)
            {
                safeThis->postStatusMessage(
                    utf8(u8"ARAでトラック音声を準備しています: ") + description.name,
                    false);
                safeThis->pluginFormatManager.createPluginInstanceAsync(
                    description,
                    std::max(1.0, safeThis->currentSampleRate.load()),
                    std::max(1, safeThis->currentBlockSize),
                    [safeThis, sharedInstance, description, finishInstallation,
                     generation, trackId]
                    (std::unique_ptr<juce::AudioPluginInstance> processingInstance,
                     const juce::String& processingError) mutable
                    {
                        if (safeThis == nullptr
                            || safeThis->pluginGeneration.load() != generation)
                            return;
                        if (processingInstance == nullptr || processingError.isNotEmpty()
                            || configurePluginLayout(*processingInstance) == 0)
                        {
                            if (description.name.containsIgnoreCase("PitchNet"))
                                safeThis->markPitchCorrectionUnavailable(trackId);
                            safeThis->postStatusMessage(
                                utf8(u8"ARAプラグインのタイムライン再生を開始できませんでした: ")
                                    + (processingError.isNotEmpty()
                                           ? processingError : description.name),
                                true);
                            return;
                        }

                        auto sharedProcessingInstance =
                            std::shared_ptr<juce::AudioPluginInstance>(
                                std::move(processingInstance));
                        juce::createARAFactoryAsync(
                            *sharedInstance,
                            [finishInstallation, sharedProcessingInstance]
                            (juce::ARAFactoryWrapper factory) mutable
                            {
                                finishInstallation(std::move(factory),
                                                   sharedProcessingInstance);
                            });
                    });
            }
            else
            {
                finishInstallation(juce::ARAFactoryWrapper{}, sharedInstance);
            }
        });
}

void AudioEngine::loadEffectPlugin(
    const juce::String& trackId,
    const juce::String& effectSlotId,
    const juce::PluginDescription& description,
    std::shared_ptr<const juce::MemoryBlock> savedState,
    bool shouldBeBypassed,
    bool createMetadataIfMissing,
    bool markProjectDirty,
    bool openEditorAfterLoad)
{
    const juce::File pluginFile(description.fileOrIdentifier);
    if (trackId.isEmpty() || effectSlotId.isEmpty()
        || description.hasARAExtension
        || description.name.containsIgnoreCase("PitchNet"))
    {
        const auto reason = utf8(
            u8"このプラグインは通常エフェクトスロットへ追加できません。");
        postStatusMessage(reason, true);
        if (onPluginScanFinished)
            onPluginScanFinished(pluginFile, false, reason);
        return;
    }

    const auto generation = pluginGeneration.load();
    const auto safeThis = juce::WeakReference<AudioEngine>(this);
    pluginFormatManager.createPluginInstanceAsync(
        description,
        std::max(1.0, currentSampleRate.load()),
        std::max(1, currentBlockSize),
        [safeThis, trackId, effectSlotId, description, savedState,
         shouldBeBypassed, createMetadataIfMissing, markProjectDirty,
         openEditorAfterLoad, generation, pluginFile]
        (std::unique_ptr<juce::AudioPluginInstance> instance,
         const juce::String& error) mutable
        {
            if (safeThis == nullptr)
                return;

            const auto notifyScanFinished =
                [&safeThis, &pluginFile](bool succeeded,
                                         const juce::String& reason)
                {
                    if (safeThis != nullptr
                        && safeThis->onPluginScanFinished)
                    {
                        safeThis->onPluginScanFinished(pluginFile,
                                                       succeeded,
                                                       reason);
                    }
                };

            if (safeThis->pluginGeneration.load() != generation)
            {
                notifyScanFinished(
                    false,
                    utf8(u8"プロジェクトが切り替わったため、VST3エフェクトの追加を完了できませんでした。"));
                return;
            }

            if (instance == nullptr || error.isNotEmpty())
            {
                const auto reason =
                    utf8(u8"VST3エフェクトを読み込めませんでした: ")
                    + (error.isNotEmpty() ? error : description.name);
                safeThis->postStatusMessage(reason, true);
                notifyScanFinished(false, reason);
                return;
            }

            if (configurePluginLayout(*instance) == 0)
            {
                const auto reason = utf8(
                    u8"このVST3はモノラルまたはステレオの音声エフェクトとして使えません。");
                safeThis->postStatusMessage(reason, true);
                notifyScanFinished(false, reason);
                return;
            }

            auto sharedInstance =
                std::shared_ptr<juce::AudioPluginInstance>(std::move(instance));
            auto newSlot = std::make_unique<TrackPluginSlot>(
                *safeThis,
                trackId,
                effectSlotId,
                description,
                sharedInstance,
                sharedInstance,
                2,
                false,
                false,
                shouldBeBypassed,
                savedState,
                std::shared_ptr<const juce::MemoryBlock>(),
                juce::String(),
                true);

            if (savedState != nullptr && !savedState->isEmpty())
            {
                if (savedState->getSize()
                    > static_cast<size_t>(std::numeric_limits<int>::max()))
                {
                    const auto reason =
                        utf8(u8"保存されたVST3状態が大きすぎます: ")
                        + description.name;
                    safeThis->postStatusMessage(reason, true);
                    notifyScanFinished(false, reason);
                    return;
                }
                newSlot->suppressStateChangeNotifications.store(true);
                sharedInstance->setStateInformation(
                    savedState->getData(),
                    static_cast<int>(savedState->getSize()));
                newSlot->suppressStateChangeNotifications.store(false);
            }

            const int channels = preparePluginInstance(
                *sharedInstance,
                std::max(1.0, safeThis->currentSampleRate.load()),
                std::max(1, safeThis->currentBlockSize));
            if (channels <= 0)
            {
                const auto reason =
                    utf8(u8"VST3エフェクトの音声処理を開始できませんでした: ")
                    + description.name;
                safeThis->postStatusMessage(reason, true);
                notifyScanFinished(false, reason);
                return;
            }

            newSlot->editorChannels = channels;
            newSlot->processingChannels = channels;
            const int latencySamples = juce::jlimit(
                0, 1920000, sharedInstance->getLatencySamples());
            newSlot->latencySamples.store(latencySamples,
                                          std::memory_order_relaxed);
            const double reportedTail = sharedInstance->getTailLengthSeconds();
            const double tailSeconds = std::isfinite(reportedTail)
                ? juce::jlimit(0.0, 60.0, reportedTail) : 0.0;
            juce::String descriptionXml;
            if (auto xml = description.createXml())
                descriptionXml = xml->toString();
            if (descriptionXml.isEmpty())
            {
                const auto reason =
                    utf8(u8"VST3エフェクトの保存情報を作成できませんでした: ")
                    + description.name;
                safeThis->postStatusMessage(reason, true);
                notifyScanFinished(false, reason);
                return;
            }

            bool trackFound = false;
            bool metadataFound = false;
            int installedTrackIndex = -1;
            int installedSlotIndex = -1;
            bool effectiveBypass = shouldBeBypassed;
            bool effectiveChainBypass = false;
            std::vector<juce::String> effectOrder;
            {
                const juce::ScopedLock lock(safeThis->tracksLock);
                for (size_t index = 0; index < safeThis->tracks.size(); ++index)
                {
                    auto& track = safeThis->tracks[index];
                    if (track.id != trackId)
                        continue;
                    trackFound = true;
                    installedTrackIndex = static_cast<int>(index);
                    effectiveChainBypass = track.effectChainBypassed;
                    auto effect = std::find_if(
                        track.effectSlots.begin(), track.effectSlots.end(),
                        [&effectSlotId](const EffectSlotData& candidate)
                        {
                            return candidate.id == effectSlotId;
                        });
                    if (effect == track.effectSlots.end())
                    {
                        if (!createMetadataIfMissing)
                            break;
                        EffectSlotData metadata;
                        metadata.id = effectSlotId;
                        metadata.name = description.name;
                        metadata.descriptionXml = descriptionXml;
                        metadata.state = savedState;
                        metadata.bypassed = shouldBeBypassed;
                        metadata.latencySamples = latencySamples;
                        metadata.tailSeconds = tailSeconds;
                        track.effectSlots.push_back(std::move(metadata));
                        effect = std::prev(track.effectSlots.end());
                    }

                    metadataFound = true;
                    effect->name = description.name;
                    effect->descriptionXml = descriptionXml;
                    effect->state = savedState;
                    effect->latencySamples = latencySamples;
                    effect->tailSeconds = tailSeconds;
                    effectiveBypass = effect->bypassed;
                    installedSlotIndex = static_cast<int>(
                        std::distance(track.effectSlots.begin(), effect));
                    effectOrder.reserve(track.effectSlots.size());
                    for (const auto& candidate : track.effectSlots)
                        effectOrder.push_back(candidate.id);
                    break;
                }
            }
            if (!trackFound || !metadataFound)
            {
                notifyScanFinished(
                    false,
                    utf8(u8"追加先のトラックがなくなったため、VST3エフェクトの追加を完了できませんでした。"));
                return;
            }

            newSlot->bypassed.store(effectiveBypass,
                                    std::memory_order_relaxed);
            newSlot->effectChainBypassed.store(effectiveChainBypass,
                                               std::memory_order_relaxed);
            if (safeThis->pluginEditorTrackId == trackId
                && safeThis->pluginEditorEffectSlotId == effectSlotId)
                safeThis->closeVst3PluginEditor(false);
            {
                const juce::SpinLock::ScopedLockType lock(safeThis->pluginLock);
                safeThis->trackPlugins.erase(
                    std::remove_if(
                        safeThis->trackPlugins.begin(),
                        safeThis->trackPlugins.end(),
                        [&trackId, &effectSlotId](
                            const std::unique_ptr<TrackPluginSlot>& slot)
                        {
                            return slot != nullptr
                                && slot->trackId == trackId
                                && slot->effectSlotId == effectSlotId;
                        }),
                    safeThis->trackPlugins.end());
                safeThis->trackPlugins.push_back(std::move(newSlot));
                safeThis->reorderTrackPluginSlotsLocked(trackId, effectOrder);
            }

            safeThis->refreshTrackPluginLatencies();
            safeThis->cachedDuration.store(
                getTracksDuration(safeThis->getTracksSnapshot()));
            if (markProjectDirty)
                safeThis->markDirty();
            safeThis->postStatusMessage(
                utf8(u8"VST3エフェクトを追加しました: ") + description.name,
                false);
            notifyScanFinished(true, {});
            if (safeThis->onStateChanged)
                safeThis->onStateChanged();
            if (openEditorAfterLoad && installedTrackIndex >= 0
                && installedSlotIndex >= 0)
            {
                safeThis->showEffectSlotEditor(installedTrackIndex,
                                               installedSlotIndex);
            }
        });
}

void AudioEngine::restoreTrackPluginsFromMetadata(const std::vector<TrackData>& tracksToRestore)
{
    for (const auto& savedTrack : tracksToRestore)
    {
        std::vector<EffectSlotData> effectsToRestore = savedTrack.effectSlots;
        juce::StringArray effectIds;
        bool normalisedEffectIds = false;
        for (auto& effect : effectsToRestore)
        {
            if (effect.id.isEmpty() || effectIds.contains(effect.id))
            {
                effect.id = "effect-" + juce::Uuid().toString();
                normalisedEffectIds = true;
            }
            effectIds.add(effect.id);
        }
        if (normalisedEffectIds)
        {
            const juce::ScopedLock lock(tracksLock);
            const auto track = std::find_if(
                tracks.begin(), tracks.end(),
                [&savedTrack](const TrackData& candidate)
                {
                    return candidate.id == savedTrack.id;
                });
            if (track != tracks.end())
                track->effectSlots = effectsToRestore;
            const auto safeThis = juce::WeakReference<AudioEngine>(this);
            juce::MessageManager::callAsync([safeThis]
            {
                if (safeThis != nullptr)
                    safeThis->markDirty();
            });
        }
        if (savedTrack.vst3DescriptionXml.isNotEmpty())
        {
            auto xml = juce::XmlDocument::parse(savedTrack.vst3DescriptionXml);
            juce::PluginDescription description;
            if (xml == nullptr || !description.loadFromXml(*xml))
            {
                if (savedTrack.vst3Name.containsIgnoreCase("PitchNet"))
                    markPitchCorrectionUnavailable(savedTrack.id);
                postStatusMessage(
                    utf8(u8"保存されたVST3情報を読み取れませんでした: ")
                        + savedTrack.vst3Name,
                    true);
            }
            else
            {
                const bool dedicatedPitchOrAra = description.hasARAExtension
                    || description.name.containsIgnoreCase("PitchNet");
                if (dedicatedPitchOrAra)
                {
                    if (description.hasARAExtension
                        && savedTrack.vst3AraPlaybackEnabled
                        && (savedTrack.vst3AraArchive == nullptr
                            || savedTrack.vst3AraArchive->isEmpty()))
                    {
                        if (savedTrack.vst3Name.containsIgnoreCase("PitchNet"))
                            markPitchCorrectionUnavailable(savedTrack.id);
                        postStatusMessage(
                            utf8(u8"保存されたプラグイン編集データを読み取れませんでした: ")
                                + savedTrack.vst3Name,
                            true);
                    }
                    else
                    {
                        loadTrackPlugin(savedTrack.id,
                                        description,
                                        savedTrack.vst3State,
                                        savedTrack.vst3AraArchive,
                                        savedTrack.vst3AraArchiveId,
                                        savedTrack.vst3AraPlaybackEnabled,
                                        savedTrack.vst3Bypassed,
                                        false,
                                        false);
                    }
                }
                else
                {
                    // Projects created before effectSlots stored their single
                    // ordinary VST3 in the PitchNet/ARA metadata fields.
                    const auto duplicate = std::find_if(
                        effectsToRestore.begin(), effectsToRestore.end(),
                        [&savedTrack](const EffectSlotData& effect)
                        {
                            return effect.descriptionXml
                                == savedTrack.vst3DescriptionXml;
                        });
                    if (duplicate == effectsToRestore.end())
                    {
                        EffectSlotData migrated;
                        migrated.id = "effect-" + juce::Uuid().toString();
                        migrated.name = savedTrack.vst3Name;
                        migrated.descriptionXml = savedTrack.vst3DescriptionXml;
                        migrated.state = savedTrack.vst3State;
                        migrated.bypassed = savedTrack.vst3Bypassed;
                        migrated.latencySamples = savedTrack.vst3LatencySamples;
                        migrated.tailSeconds = savedTrack.vst3TailSeconds;
                        effectsToRestore.insert(effectsToRestore.begin(),
                                                std::move(migrated));
                    }

                    {
                        const juce::ScopedLock lock(tracksLock);
                        const auto track = std::find_if(
                            tracks.begin(), tracks.end(),
                            [&savedTrack](const TrackData& candidate)
                            {
                                return candidate.id == savedTrack.id;
                            });
                        if (track != tracks.end())
                        {
                            track->effectSlots = effectsToRestore;
                            track->vst3Name.clear();
                            track->vst3DescriptionXml.clear();
                            track->vst3State.reset();
                            track->vst3AraArchive.reset();
                            track->vst3AraArchiveId.clear();
                            track->vst3AraHostFormatVersion = 1;
                            track->vst3AraPlaybackEnabled = false;
                            track->vst3Bypassed = false;
                            track->vst3LatencySamples = 0;
                            track->vst3TailSeconds = 0.0;
                        }
                    }
                    markDirty();
                    const auto safeThis = juce::WeakReference<AudioEngine>(this);
                    juce::MessageManager::callAsync([safeThis]
                    {
                        if (safeThis != nullptr)
                            safeThis->markDirty();
                    });
                    postStatusMessage(
                        utf8(u8"旧プロジェクトのVST3をエフェクトチェーンへ移行しました: ")
                            + savedTrack.vst3Name,
                        false);
                }
            }
        }
        else if (savedTrack.vst3Name.containsIgnoreCase("PitchNet"))
        {
            markPitchCorrectionUnavailable(savedTrack.id);
            postStatusMessage(
                utf8(u8"PitchNetの保存情報が不足しているため、補正はOFFで復元しました。"),
                true);
        }

        for (auto effect : effectsToRestore)
        {
            auto xml = juce::XmlDocument::parse(effect.descriptionXml);
            juce::PluginDescription description;
            if (xml == nullptr || !description.loadFromXml(*xml))
            {
                postStatusMessage(
                    utf8(u8"保存されたVST3エフェクト情報を読み取れませんでした: ")
                        + effect.name,
                    true);
                continue;
            }
            if (description.hasARAExtension
                || description.name.containsIgnoreCase("PitchNet"))
            {
                postStatusMessage(
                    utf8(u8"PitchNet／ARAは通常エフェクトチェーンから復元できません: ")
                        + effect.name,
                    true);
                continue;
            }
            if (effect.id.isEmpty())
            {
                effect.id = "effect-" + juce::Uuid().toString();
                const juce::ScopedLock lock(tracksLock);
                const auto track = std::find_if(
                    tracks.begin(), tracks.end(),
                    [&savedTrack](const TrackData& candidate)
                    {
                        return candidate.id == savedTrack.id;
                    });
                if (track != tracks.end())
                {
                    const auto metadata = std::find_if(
                        track->effectSlots.begin(), track->effectSlots.end(),
                        [&effect](const EffectSlotData& candidate)
                        {
                            return candidate.id.isEmpty()
                                && candidate.descriptionXml
                                    == effect.descriptionXml;
                        });
                    if (metadata != track->effectSlots.end())
                        metadata->id = effect.id;
                }
            }
            loadEffectPlugin(savedTrack.id,
                             effect.id,
                             description,
                             effect.state,
                             effect.bypassed,
                             false,
                             false,
                             false);
        }
    }
}

void AudioEngine::hideVst3PluginEditor(
    const juce::String& expectedTrackId,
    const juce::String& expectedEffectSlotId)
{
    if (pluginEditorWindow == nullptr || pluginEditorTrackId != expectedTrackId
        || pluginEditorEffectSlotId != expectedEffectSlotId)
        return;

    if (pluginEditorEffectSlotId.isEmpty())
        setPluginEditorTransportActive(pluginEditorTrackId, false);
    pluginEditorWindow->setVisible(false);
    if (pluginEditorEffectSlotId.isEmpty())
        syncAraProcessingState(pluginEditorTrackId);
}

void AudioEngine::closeVst3PluginEditor(bool syncProcessingState)
{
    const bool dedicatedEditor = pluginEditorEffectSlotId.isEmpty();
    if (syncProcessingState && dedicatedEditor
        && pluginEditorTrackId.isNotEmpty())
        syncAraProcessingState(pluginEditorTrackId);
    if (dedicatedEditor && pluginEditorTrackId.isNotEmpty())
        setPluginEditorTransportActive(pluginEditorTrackId, false);
    if (pluginEditorWindow != nullptr)
    {
        pluginEditorWindow->setVisible(false);
        pluginEditorWindow.reset();
    }
    pluginEditorTrackId.clear();
    pluginEditorEffectSlotId.clear();
}

std::vector<TrackData> AudioEngine::getTracksSnapshot() const
{
    const juce::ScopedLock lock(tracksLock);
    return tracks;
}

void AudioEngine::queueMissingRhythmRenders()
{
    std::vector<AudioClip> clipsToRender;
    {
        const juce::ScopedLock lock(tracksLock);
        for (const auto& track : tracks)
        {
            for (const auto& clip : track.clips)
            {
                if (clip.rhythmMarkers.empty() || clip.buffer == nullptr)
                    continue;
                if (clip.rhythmRenderCache != nullptr
                    && clip.rhythmRenderCache->matches(clip))
                    continue;
                clipsToRender.push_back(clip);
            }
        }
    }

    const auto generation = rhythmRenderGeneration.fetch_add(1) + 1;
    const auto expectedProjectGeneration = projectGeneration.load();
    rhythmRenderThreadPool.removeAllJobs(true, 0);
    if (clipsToRender.empty() || rhythmRenderShutdownRequested.load())
        return;

    const auto safeThis = juce::WeakReference<AudioEngine>(this);
    rhythmRenderThreadPool.addJob(
        [this, safeThis, clipsToRender = std::move(clipsToRender),
         generation, expectedProjectGeneration]() mutable
        {
            using CompletedRender = std::pair<
                juce::String, std::shared_ptr<const RhythmRenderCache>>;
            std::vector<CompletedRender> completed;
            completed.reserve(clipsToRender.size());
            auto* currentJob = juce::ThreadPoolJob::getCurrentThreadPoolJob();
            const auto shouldCancel = [this, currentJob, generation,
                                       expectedProjectGeneration]
            {
                return rhythmRenderShutdownRequested.load()
                    || rhythmRenderGeneration.load() != generation
                    || projectGeneration.load() != expectedProjectGeneration
                    || (currentJob != nullptr && currentJob->shouldExit());
            };

            for (const auto& clip : clipsToRender)
            {
                if (shouldCancel())
                    return;
                auto cache = RhythmRender::render(clip, shouldCancel);
                if (cache != nullptr)
                    completed.emplace_back(clip.id, std::move(cache));
            }
            if (shouldCancel())
                return;

            juce::MessageManager::callAsync(
                [safeThis, completed = std::move(completed), generation,
                 expectedProjectGeneration]() mutable
                {
                    if (safeThis == nullptr
                        || safeThis->rhythmRenderShutdownRequested.load()
                        || safeThis->rhythmRenderGeneration.load() != generation
                        || safeThis->projectGeneration.load()
                            != expectedProjectGeneration)
                        return;

                    const juce::ScopedLock lock(safeThis->tracksLock);
                    for (auto& track : safeThis->tracks)
                    {
                        for (auto& clip : track.clips)
                        {
                            const auto result = std::find_if(
                                completed.begin(), completed.end(),
                                [&clip](const CompletedRender& candidate)
                                {
                                    return candidate.first == clip.id;
                                });
                            if (result == completed.end()
                                || result->second == nullptr
                                || !result->second->matches(clip))
                                continue;
                            clip.rhythmRenderCache = result->second;
                        }
                    }
                });
        });
}

void AudioEngine::replaceTracks(std::vector<TrackData> newTracks)
{
    // In-flight imports belong to the project that was active when they began.
    // Advancing the generation makes workers stop and prevents queued results
    // from being applied to the replacement project.
    projectGeneration.fetch_add(1);
    pluginGeneration.fetch_add(1);
    pluginScanGeneration.fetch_add(1);
    pluginScanThreadPool.removeAllJobs(true, 0);
    closeVst3PluginEditor(false);
    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        trackPlugins.clear();
    }

    const double targetRate = currentSampleRate.load();
    for (auto& track : newTracks)
    {
        if (track.id.isEmpty()) track.id = makeTrackId();
        for (auto& clip : track.clips)
        {
            if (clip.id.isEmpty()) clip.id = makeClipId();
            clip.rhythmRenderCache.reset();
        }
    }
    prepareTracksForSampleRate(newTracks, targetRate);
    const double updatedDuration = getTracksDuration(newTracks);
    const auto pluginMetadata = newTracks;

    {
        const juce::ScopedLock lock(tracksLock);
        tracks = std::move(newTracks);
        rebuildTrackPlaybackSlotsLocked();
        for (auto& slot : trackPlaybackSlots)
            if (slot != nullptr)
                slot->chainLatencySamples.store(0,
                                                std::memory_order_relaxed);
        resetTrackPlaybackTransformsLocked();
        nextTrackColorIndex = static_cast<int>(tracks.size());
        selectedClipId.clear();
        lastSavedState.clear();
        editInProgress = false;
    }
    cachedDuration.store(updatedDuration);

    // Undo actions contain complete track snapshots. Keeping them across a
    // project load could restore clips from the previously-open project.
    undoManager.clearUndoHistory();

    restoreTrackPluginsFromMetadata(pluginMetadata);
    queueMissingRhythmRenders();

    markDirty();
    if (onStateChanged) onStateChanged();
}

//==============================================================================
// Undo / redo

void AudioEngine::beginEdit()
{
    const juce::ScopedLock lock(tracksLock);
    lastSavedState = tracks;
    editInProgress = true;
    undoManager.beginNewTransaction();
}

void AudioEngine::endEdit()
{
    std::vector<TrackData> before;
    std::vector<TrackData> after;
    {
        const juce::ScopedLock lock(tracksLock);
        if (!editInProgress)
            return;
        before = lastSavedState;
        after = tracks;
        editInProgress = false;
    }

    undoManager.perform(new TrackStateAction(*this, before, after));
    queueAraTrackSync();
    markDirty();
}

void AudioEngine::undo()
{
    if (undoManager.undo())
        markDirty();
}

void AudioEngine::redo()
{
    if (undoManager.redo())
        markDirty();
}

void AudioEngine::restoreTracksState(const std::vector<TrackData>& state)
{
    {
        const juce::ScopedLock lock(tracksLock);
        tracks = state;
        rebuildTrackPlaybackSlotsLocked();
        resetTrackPlaybackTransformsLocked();
    }
    juce::StringArray failedPitchStateRestores;
    {
        const juce::SpinLock::ScopedLockType lock(pluginLock);
        for (auto& slot : trackPlugins)
        {
            const auto track = std::find_if(state.begin(), state.end(),
                [&slot](const TrackData& candidate)
                {
                    return candidate.id == slot->trackId;
                });
            if (track != state.end())
            {
                if (slot->effectSlotId.isEmpty())
                {
                    slot->bypassed.store(track->vst3Bypassed,
                                         std::memory_order_relaxed);
                    bool stateRestored = true;
                    if (track->vst3State != nullptr
                        && !track->vst3State->isEmpty()
                        && slot->persistedState != track->vst3State)
                    {
                        if (track->vst3State->getSize()
                            > static_cast<size_t>(std::numeric_limits<int>::max()))
                        {
                            stateRestored = false;
                        }
                        else
                        {
                            try
                            {
                                slot->suppressStateChangeNotifications.store(true);
                                slot->instance->setStateInformation(
                                    track->vst3State->getData(),
                                    static_cast<int>(track->vst3State->getSize()));
                                if (slot->processingInstance != nullptr
                                    && slot->processingInstance != slot->instance)
                                {
                                    slot->processingInstance->releaseResources();
                                    slot->processingInstance->setStateInformation(
                                        track->vst3State->getData(),
                                        static_cast<int>(track->vst3State->getSize()));
                                    slot->processingChannels = preparePluginInstance(
                                        *slot->processingInstance,
                                        std::max(1.0, currentSampleRate.load()),
                                        std::max(1, currentBlockSize));
                                    stateRestored = slot->processingChannels > 0;
                                }
                                slot->persistedState = track->vst3State;
                                const auto restoredRevision =
                                    slot->stateRevision.fetch_add(1) + 1;
                                slot->capturedStateRevision = restoredRevision;
                                slot->processingStateDirty.store(false);
                                slot->suppressStateChangeNotifications.store(false);
                            }
                            catch (...)
                            {
                                slot->suppressStateChangeNotifications.store(false);
                                stateRestored = false;
                            }
                        }
                    }

                    bool archiveRestored = true;
                    if (slot->araHost != nullptr
                        && track->vst3AraPlaybackEnabled
                        && slot->persistedAraArchive != track->vst3AraArchive)
                    {
                        archiveRestored = track->vst3AraArchive != nullptr
                            && !track->vst3AraArchive->isEmpty()
                            && slot->araHost->restoreArchive(
                                *track->vst3AraArchive);
                        if (archiveRestored)
                        {
                            slot->persistedAraArchive = track->vst3AraArchive;
                            slot->persistedAraArchiveId =
                                track->vst3AraArchiveId;
                            const auto restoredRevision =
                                slot->araRevision.fetch_add(1) + 1;
                            slot->capturedAraRevision = restoredRevision;
                        }
                    }
                    slot->araPlaybackEnabled.store(
                        track->vst3AraPlaybackEnabled && archiveRestored);

                    if ((!stateRestored || !archiveRestored)
                        && slot->description.name.containsIgnoreCase("PitchNet"))
                    {
                        failedPitchStateRestores.addIfNotAlreadyThere(track->id);
                        slot->bypassed.store(true, std::memory_order_relaxed);
                        slot->araPlaybackEnabled.store(false);
                    }
                }
                else
                {
                    const auto effect = std::find_if(
                        track->effectSlots.begin(), track->effectSlots.end(),
                        [&slot](const EffectSlotData& candidate)
                        {
                            return candidate.id == slot->effectSlotId;
                        });
                    if (effect != track->effectSlots.end())
                    {
                        slot->bypassed.store(effect->bypassed,
                                             std::memory_order_relaxed);
                        slot->effectChainBypassed.store(
                            track->effectChainBypassed,
                            std::memory_order_relaxed);
                    }
                }
            }
        }
        for (const auto& track : state)
        {
            std::vector<juce::String> order;
            order.reserve(track.effectSlots.size());
            for (const auto& effect : track.effectSlots)
                order.push_back(effect.id);
            reorderTrackPluginSlotsLocked(track.id, order);
        }
    }
    if (!failedPitchStateRestores.isEmpty())
    {
        const juce::ScopedLock lock(tracksLock);
        for (auto& track : tracks)
        {
            if (!failedPitchStateRestores.contains(track.id))
                continue;
            track.pitchCorrection.enabled = false;
            track.pitchCorrection.auditionCorrected = false;
            track.vst3AraPlaybackEnabled = false;
            track.vst3Bypassed = true;
        }
    }
    refreshTrackPluginLatencies();
    cachedDuration.store(getTracksDuration(state));
    queueAraTrackSync();
    queueMissingRhythmRenders();
    markDirty();
    if (onStateChanged) onStateChanged();
}

//==============================================================================
// Clip editing

void AudioEngine::moveClip(const juce::String& clipId, int destTrackIndex, double newStartTime)
{
    const juce::ScopedLock lock(tracksLock);
    if (destTrackIndex < 0 || destTrackIndex >= static_cast<int>(tracks.size())) return;

    const bool ownsTransaction = !editInProgress;
    if (ownsTransaction) beginEdit();
    AudioClip movingClip;
    bool found = false;
    for (auto& track : tracks)
    {
        const auto it = std::find_if(track.clips.begin(), track.clips.end(),
            [&clipId](const AudioClip& clip) { return clip.id == clipId; });
        if (it != track.clips.end())
        {
            movingClip = *it;
            track.clips.erase(it);
            found = true;
            break;
        }
    }

    if (!found)
    {
        if (ownsTransaction) editInProgress = false;
        return;
    }

    movingClip.startTime = std::max(0.0, newStartTime);
    tracks[static_cast<size_t>(destTrackIndex)].clips.push_back(std::move(movingClip));
    if (ownsTransaction) endEdit();
    if (onStateChanged) onStateChanged();
}

void AudioEngine::splitClip(const juce::String& clipId, double splitTime)
{
    const juce::ScopedLock lock(tracksLock);
    for (auto& track : tracks)
    {
        const auto it = std::find_if(track.clips.begin(), track.clips.end(),
            [&clipId](const AudioClip& clip) { return clip.id == clipId; });
        if (it == track.clips.end()) continue;

        const AudioClip original = *it;
        if (splitTime <= original.startTime
            || splitTime >= original.startTime + original.duration)
            return;

        beginEdit();
        const double firstDuration = splitTime - original.startTime;
        const double secondDuration = original.duration - firstDuration;
        it->duration = firstDuration;
        it->rhythmMarkers.clear();
        it->rhythmRenderCache.reset();

        AudioClip secondClip = original;
        secondClip.id = makeClipId();
        secondClip.startTime = splitTime;
        secondClip.offset = original.offset + firstDuration;
        secondClip.duration = secondDuration;
        secondClip.rhythmMarkers.clear();
        secondClip.rhythmRenderCache.reset();
        track.clips.push_back(std::move(secondClip));

        if (selectedClipId == clipId)
            selectedClipId.clear();

        endEdit();
        if (onStateChanged) onStateChanged();
        return;
    }
}

void AudioEngine::removeClip(const juce::String& clipId)
{
    const juce::ScopedLock lock(tracksLock);
    for (auto& track : tracks)
    {
        const auto it = std::find_if(track.clips.begin(), track.clips.end(),
            [&clipId](const AudioClip& clip) { return clip.id == clipId; });
        if (it == track.clips.end()) continue;

        beginEdit();
        track.clips.erase(it);
        if (selectedClipId == clipId)
            selectedClipId.clear();
        endEdit();
        if (onStateChanged) onStateChanged();
        return;
    }
}

void AudioEngine::trimClipLeft(const juce::String& clipId, double newStartTime)
{
    const bool ownsTransaction = !editInProgress;
    if (ownsTransaction)
        beginEdit();
    bool changed = false;
    {
        const juce::ScopedLock lock(tracksLock);
        for (auto& track : tracks)
        {
            auto clip = std::find_if(
                track.clips.begin(), track.clips.end(),
                [&clipId](const AudioClip& candidate)
                {
                    return candidate.id == clipId;
                });
            if (clip == track.clips.end() || clip->buffer == nullptr)
                continue;

            constexpr double minimumDuration = 0.010;
            const double earliestStart = std::max(
                0.0, clip->startTime - clip->offset);
            const double latestStart = clip->startTime + clip->duration
                                     - minimumDuration;
            const double boundedStart = juce::jlimit(
                earliestStart, latestStart, newStartTime);
            if (std::abs(boundedStart - clip->startTime) < 1.0e-9)
                break;

            const double delta = boundedStart - clip->startTime;
            clip->startTime = boundedStart;
            clip->offset = std::max(0.0, clip->offset + delta);
            clip->duration = std::max(minimumDuration,
                                      clip->duration - delta);
            clip->fadeInSeconds = std::min(clip->fadeInSeconds,
                                           clip->duration);
            clip->fadeOutSeconds = std::min(clip->fadeOutSeconds,
                                            clip->duration);
            clip->rhythmMarkers.clear();
            clip->rhythmRenderCache.reset();
            changed = true;
            break;
        }
    }
    if (changed)
    {
        cachedDuration.store(getTracksDuration(getTracksSnapshot()));
        playbackTransformResetRequested.store(true);
        if (ownsTransaction)
            endEdit();
        else
        {
            markDirty();
            if (onStateChanged)
                onStateChanged();
        }
    }
    else if (ownsTransaction)
        editInProgress = false;
}

void AudioEngine::trimClipRight(const juce::String& clipId, double newEndTime)
{
    const bool ownsTransaction = !editInProgress;
    if (ownsTransaction)
        beginEdit();
    bool changed = false;
    {
        const juce::ScopedLock lock(tracksLock);
        for (auto& track : tracks)
        {
            auto clip = std::find_if(
                track.clips.begin(), track.clips.end(),
                [&clipId](const AudioClip& candidate)
                {
                    return candidate.id == clipId;
                });
            if (clip == track.clips.end() || clip->buffer == nullptr)
                continue;

            constexpr double minimumDuration = 0.010;
            const double sourceRemaining = std::max(
                0.0, clip->getSourceDuration() - clip->offset);
            const double earliestEnd = clip->startTime + minimumDuration;
            const double latestEnd = clip->startTime + sourceRemaining;
            const double boundedEnd = juce::jlimit(
                earliestEnd, latestEnd, newEndTime);
            const double newDuration = boundedEnd - clip->startTime;
            if (std::abs(newDuration - clip->duration) < 1.0e-9)
                break;

            clip->duration = newDuration;
            clip->fadeInSeconds = std::min(clip->fadeInSeconds,
                                           clip->duration);
            clip->fadeOutSeconds = std::min(clip->fadeOutSeconds,
                                            clip->duration);
            clip->rhythmMarkers.clear();
            clip->rhythmRenderCache.reset();
            changed = true;
            break;
        }
    }
    if (changed)
    {
        cachedDuration.store(getTracksDuration(getTracksSnapshot()));
        playbackTransformResetRequested.store(true);
        if (ownsTransaction)
            endEdit();
        else
        {
            markDirty();
            if (onStateChanged)
                onStateChanged();
        }
    }
    else if (ownsTransaction)
        editInProgress = false;
}

void AudioEngine::setClipFades(const juce::String& clipId,
                               double fadeInSeconds,
                               double fadeOutSeconds)
{
    const bool ownsTransaction = !editInProgress;
    if (ownsTransaction) beginEdit();
    bool found = false;
    bool changed = false;
    {
        const juce::ScopedLock lock(tracksLock);
        for (auto& track : tracks)
        {
            auto clip = std::find_if(track.clips.begin(), track.clips.end(),
                [&clipId](const AudioClip& candidate) { return candidate.id == clipId; });
            if (clip == track.clips.end())
                continue;
            found = true;
            double fadeIn = juce::jlimit(0.0, clip->duration, fadeInSeconds);
            double fadeOut = juce::jlimit(0.0, clip->duration, fadeOutSeconds);
            const double total = fadeIn + fadeOut;
            if (total > clip->duration && total > 0.0)
            {
                const double scale = clip->duration / total;
                fadeIn *= scale;
                fadeOut *= scale;
            }
            changed = std::abs(clip->fadeInSeconds - fadeIn) > 1.0e-9
                || std::abs(clip->fadeOutSeconds - fadeOut) > 1.0e-9;
            clip->fadeInSeconds = fadeIn;
            clip->fadeOutSeconds = fadeOut;
            break;
        }
    }
    if (found && changed)
    {
        if (ownsTransaction) endEdit();
        else
        {
            markDirty();
            if (onStateChanged) onStateChanged();
        }
    }
    else if (ownsTransaction)
        editInProgress = false;
}

void AudioEngine::setClipRhythmMarkers(const juce::String& clipId,
                                       std::vector<RhythmMarker> markers)
{
    const bool ownsTransaction = !editInProgress;
    if (ownsTransaction) beginEdit();
    bool found = false;
    {
        const juce::ScopedLock lock(tracksLock);
        for (auto& track : tracks)
        {
            auto clip = std::find_if(track.clips.begin(), track.clips.end(),
                [&clipId](const AudioClip& candidate) { return candidate.id == clipId; });
            if (clip == track.clips.end())
                continue;
            found = true;
            clip->rhythmMarkers = RhythmWarp::sanitise(std::move(markers),
                                                        clip->duration);
            clip->rhythmRenderCache.reset();
            break;
        }
    }
    if (found)
    {
        playbackTransformResetRequested.store(true);
        if (ownsTransaction) endEdit();
        else
        {
            markDirty();
            if (onStateChanged) onStateChanged();
        }
    }
    else if (ownsTransaction)
        editInProgress = false;
}

int AudioEngine::quantizeClipRhythm(const juce::String& clipId, float strength)
{
    AudioClip clip;
    {
        const juce::ScopedLock lock(tracksLock);
        bool found = false;
        for (const auto& track : tracks)
        {
            const auto candidate = std::find_if(track.clips.begin(), track.clips.end(),
                [&clipId](const AudioClip& value) { return value.id == clipId; });
            if (candidate != track.clips.end())
            {
                clip = *candidate;
                found = true;
                break;
            }
        }
        if (!found || clip.buffer == nullptr || clip.sampleRate <= 0)
            return 0;
    }

    const float amount = juce::jlimit(0.0f, 1.0f, strength);
    const int hop = std::max(1, clip.sampleRate / 100);
    const int startSample = juce::jlimit(
        0, clip.buffer->getNumSamples(),
        juce::roundToInt(clip.offset * clip.sampleRate));
    const int visibleSamples = juce::jlimit(
        0, clip.buffer->getNumSamples() - startSample,
        juce::roundToInt(clip.duration * clip.sampleRate));
    if (visibleSamples < hop * 8)
        return 0;

    std::vector<float> envelope;
    envelope.reserve(static_cast<size_t>(visibleSamples / hop + 1));
    for (int offset = 0; offset < visibleSamples; offset += hop)
    {
        const int count = std::min(hop, visibleSamples - offset);
        double sum = 0.0;
        for (int sample = 0; sample < count; ++sample)
        {
            double mono = 0.0;
            for (int channel = 0; channel < clip.buffer->getNumChannels(); ++channel)
                mono += clip.buffer->getSample(channel, startSample + offset + sample);
            mono /= std::max(1, clip.buffer->getNumChannels());
            sum += mono * mono;
        }
        envelope.push_back(static_cast<float>(std::sqrt(sum / std::max(1, count))));
    }
    const float peak = *std::max_element(envelope.begin(), envelope.end());
    if (peak < 1.0e-5f)
        return 0;

    const auto rhythmTempoMap = std::atomic_load(&tempoMapSnapshot);
    const TempoMap fallbackTempoMap;
    const TempoMap& tempoMap = rhythmTempoMap != nullptr
        ? *rhythmTempoMap : fallbackTempoMap;
    std::vector<double> onsets;
    double lastOnset = -1.0;
    for (size_t index = 2; index < envelope.size(); ++index)
    {
        const float current = envelope[index];
        const float previous = envelope[index - 1];
        const double onsetTime = static_cast<double>(index * hop) / clip.sampleRate;
        if (current < peak * 0.12f
            || current < previous * 1.45f
            || onsetTime - lastOnset < 0.080)
            continue;
        onsets.push_back(onsetTime);
        lastOnset = onsetTime;
    }

    std::vector<RhythmMarker> markers;
    markers.reserve(onsets.size());
    for (size_t onsetNumber = 0; onsetNumber < onsets.size(); ++onsetNumber)
    {
        const double onsetTime = onsets[onsetNumber];
        const double absolute = clip.startTime + onsetTime;
        const double quantizedAbsolute = tempoMap.snapToNearestBeat(absolute);
        const double target = juce::jlimit(
            0.0, clip.duration,
            onsetTime + amount * (quantizedAbsolute - absolute));

        const size_t onsetIndex = std::min(
            envelope.size() - 1,
            static_cast<size_t>(std::llround(
                onsetTime * clip.sampleRate / hop)));
        const size_t minimumEndIndex = std::min(
            envelope.size(), onsetIndex + static_cast<size_t>(8));
        const size_t nextOnsetIndex = onsetNumber + 1 < onsets.size()
            ? std::min(envelope.size(), static_cast<size_t>(std::llround(
                onsets[onsetNumber + 1] * clip.sampleRate / hop)))
            : envelope.size();
        size_t endIndex = nextOnsetIndex;
        const float releaseThreshold = std::max(
            peak * 0.035f, envelope[onsetIndex] * 0.08f);
        for (size_t index = minimumEndIndex;
             index + 2 < nextOnsetIndex; ++index)
        {
            if (envelope[index] <= releaseThreshold
                && envelope[index + 1] <= releaseThreshold
                && envelope[index + 2] <= releaseThreshold)
            {
                endIndex = index;
                break;
            }
        }
        const double sourceEnd = juce::jlimit(
            std::min(clip.duration, onsetTime + 0.01),
            clip.duration,
            std::max(onsetTime + 0.08,
                     static_cast<double>(endIndex * hop) / clip.sampleRate));
        const double targetEnd = juce::jlimit(
            target, clip.duration, target + (sourceEnd - onsetTime));
        markers.push_back({ onsetTime, target, sourceEnd, targetEnd });
    }

    markers = RhythmWarp::sanitise(std::move(markers), clip.duration);
    const int markerCount = static_cast<int>(markers.size());

    setClipRhythmMarkers(clipId, markers);
    postStatusMessage(utf8(u8"編集できるリズムノートを作成しました: ")
                          + juce::String(markerCount),
                      false);
    return markerCount;
}

double AudioEngine::getDuration() const
{
    return cachedDuration.load();
}

ProjectDawSettings AudioEngine::getProjectDawSettings() const
{
    const juce::ScopedLock lock(projectSettingsLock);
    return projectDawSettings;
}

void AudioEngine::setProjectDawSettings(const ProjectDawSettings& settings,
                                        bool shouldMarkDirty)
{
    auto bounded = settings;
    bounded.musicalKey = juce::jlimit(0, 11, bounded.musicalKey);
    bounded.pitchCorrectionStrength = juce::jlimit(
        0.0f, 1.0f, bounded.pitchCorrectionStrength);
    bounded.loopStartSeconds = std::max(0.0, bounded.loopStartSeconds);
    bounded.loopEndSeconds = std::max(0.0, bounded.loopEndSeconds);
    if (!bounded.hasLoopStart || !bounded.hasLoopEnd
        || bounded.loopEndSeconds <= bounded.loopStartSeconds)
        bounded.loopEnabled = false;

    TempoMap normalisedMap(std::move(bounded.tempoMap));
    bounded.tempoMap = normalisedMap.getPoints();
    bounded.exportDefaults.wavBits = bounded.exportDefaults.wavBits == 24 ? 24 : 16;
    bounded.exportDefaults.mp3Kbps = 192;
    bounded.exportDefaults.sampleRate = bounded.exportDefaults.sampleRate == 48000
        ? 48000 : 44100;
    bounded.mastering.ceilingDb = juce::jlimit(
        -12.0f, 0.0f, bounded.mastering.ceilingDb);
    bounded.mastering.targetLufs = juce::jlimit(
        -24.0f, -8.0f, bounded.mastering.targetLufs);

    bool changed = false;
    {
        const juce::ScopedLock lock(projectSettingsLock);
        changed = !(projectDawSettings == bounded);
        projectDawSettings = bounded;
        std::atomic_store(&tempoMapSnapshot,
                          std::make_shared<const TempoMap>(normalisedMap));
    }

    hasLoopStart.store(bounded.hasLoopStart);
    hasLoopEnd.store(bounded.hasLoopEnd);
    loopStartSeconds.store(bounded.loopStartSeconds);
    loopEndSeconds.store(bounded.loopEndSeconds);
    loopEnabled.store(bounded.loopEnabled);
    masterBusProcessor.setSettings(bounded.mastering);

    const auto currentTempo = normalisedMap.getPointAt(currentPlaybackPosition.load());
    metronome.setBpm(juce::roundToInt(currentTempo.bpm));
    metronome.setTimeSignature(currentTempo.numerator, currentTempo.denominator);

    if (changed && shouldMarkDirty)
        markDirty();
    if (changed && onStateChanged)
        onStateChanged();
}

void AudioEngine::setLoopStartAtCurrentTime()
{
    auto settings = getProjectDawSettings();
    settings.hasLoopStart = true;
    settings.loopStartSeconds = juce::jlimit(
        0.0, getDuration(), currentPlaybackPosition.load());
    if (settings.hasLoopEnd
        && settings.loopEndSeconds <= settings.loopStartSeconds)
        settings.hasLoopEnd = false;
    setProjectDawSettings(settings);
}

void AudioEngine::setLoopEndAtCurrentTime()
{
    auto settings = getProjectDawSettings();
    settings.hasLoopEnd = true;
    settings.loopEndSeconds = juce::jlimit(
        0.0, getDuration(), currentPlaybackPosition.load());
    if (settings.hasLoopStart
        && settings.loopEndSeconds <= settings.loopStartSeconds)
        std::swap(settings.loopStartSeconds, settings.loopEndSeconds);
    setProjectDawSettings(settings);
}

void AudioEngine::clearLoopRange()
{
    auto settings = getProjectDawSettings();
    settings.hasLoopStart = false;
    settings.hasLoopEnd = false;
    settings.loopEnabled = false;
    settings.loopStartSeconds = 0.0;
    settings.loopEndSeconds = 0.0;
    setProjectDawSettings(settings);
}

void AudioEngine::setLoopEnabled(bool enabled)
{
    auto settings = getProjectDawSettings();
    settings.loopEnabled = enabled && settings.hasLoopStart && settings.hasLoopEnd
        && settings.loopEndSeconds > settings.loopStartSeconds;
    setProjectDawSettings(settings);
}

void AudioEngine::setTempoPoint(double timeSeconds,
                                double bpm,
                                int numerator,
                                int denominator)
{
    auto settings = getProjectDawSettings();
    const double time = std::max(0.0, timeSeconds);
    settings.tempoMap.erase(
        std::remove_if(settings.tempoMap.begin(), settings.tempoMap.end(),
            [time](const TempoPoint& point)
            {
                return std::abs(point.timeSeconds - time) < 0.001;
            }),
        settings.tempoMap.end());
    settings.tempoMap.push_back({ time,
                                  juce::jlimit(20.0, 300.0, bpm),
                                  juce::jlimit(1, 12, numerator),
                                  denominator == 8 ? 8 : 4 });
    setProjectDawSettings(settings);
}

void AudioEngine::removeTempoPointAt(double timeSeconds)
{
    auto settings = getProjectDawSettings();
    const double time = std::max(0.0, timeSeconds);
    settings.tempoMap.erase(
        std::remove_if(settings.tempoMap.begin(), settings.tempoMap.end(),
            [time](const TempoPoint& point)
            {
                return point.timeSeconds > 1.0e-9
                    && std::abs(point.timeSeconds - time) < 0.050;
            }),
        settings.tempoMap.end());
    setProjectDawSettings(settings);
}

TempoPoint AudioEngine::getTempoPointAt(double timeSeconds) const
{
    const auto map = std::atomic_load(&tempoMapSnapshot);
    return map != nullptr ? map->getPointAt(timeSeconds) : TempoPoint{};
}

//==============================================================================
// Latency and sample-rate handling

void AudioEngine::setDeviceLatencySamples(int inputLatency, int outputLatency)
{
    inputLatencySamples.store(std::max(0, inputLatency));
    outputLatencySamples.store(std::max(0, outputLatency));
    if (auto* manager = juce::MessageManager::getInstanceWithoutCreating();
        manager != nullptr && manager->isThisTheMessageThread())
    {
        if (onStateChanged) onStateChanged();
    }
    else
    {
        triggerAsyncUpdate();
    }
}

void AudioEngine::setManualRecordingOffsetSamples(int samples)
{
    const int maxOffset = static_cast<int>(std::max(1.0, currentSampleRate.load()) * 10.0);
    manualRecordingOffsetSamples.store(juce::jlimit(-maxOffset, maxOffset, samples));
    // This is an application/device preference, not project content.
    if (auto* manager = juce::MessageManager::getInstanceWithoutCreating();
        manager != nullptr && manager->isThisTheMessageThread())
    {
        if (onStateChanged) onStateChanged();
    }
    else
    {
        triggerAsyncUpdate();
    }
}

void AudioEngine::setCalibratedRecordingLatencySamples(int samples)
{
    const int maxLatency = static_cast<int>(std::max(1.0, currentSampleRate.load()) * 2.0);
    calibratedRecordingLatencySamples.store(juce::jlimit(0, maxLatency, samples));
    if (auto* manager = juce::MessageManager::getInstanceWithoutCreating();
        manager != nullptr && manager->isThisTheMessageThread())
    {
        if (onStateChanged) onStateChanged();
    }
    else
    {
        triggerAsyncUpdate();
    }
}

void AudioEngine::clearCalibratedRecordingLatency()
{
    calibratedRecordingLatencySamples.store(-1);
    if (auto* manager = juce::MessageManager::getInstanceWithoutCreating();
        manager != nullptr && manager->isThisTheMessageThread())
    {
        if (onStateChanged) onStateChanged();
    }
    else
    {
        triggerAsyncUpdate();
    }
}

int AudioEngine::getAutomaticRecordingLatencySamples() const
{
    const int measured = calibratedRecordingLatencySamples.load();
    if (measured >= 0)
        return measured;

    const int64_t reported = static_cast<int64_t>(inputLatencySamples.load())
                             + static_cast<int64_t>(outputLatencySamples.load());
    return static_cast<int>(juce::jlimit<int64_t>(0,
                                                  std::numeric_limits<int>::max(),
                                                  reported));
}

int AudioEngine::getEffectiveRecordingLatencySamples() const
{
    const int64_t total = static_cast<int64_t>(getAutomaticRecordingLatencySamples())
                          + static_cast<int64_t>(manualRecordingOffsetSamples.load());
    return static_cast<int>(juce::jlimit<int64_t>(std::numeric_limits<int>::min(),
                                                  std::numeric_limits<int>::max(),
                                                  total));
}

bool AudioEngine::resampleAllClipsToCurrentSampleRate()
{
    bool changed = false;
    const double targetRate = currentSampleRate.load();
    {
        const juce::ScopedLock lock(tracksLock);
        for (auto& track : tracks)
            for (auto& clip : track.clips)
                changed = resampleClip(clip, targetRate) || changed;
    }

    if (changed)
    {
        queueMissingRhythmRenders();
        markDirty();
        if (onStateChanged) onStateChanged();
    }
    return changed;
}

bool AudioEngine::prepareTracksForSampleRate(std::vector<TrackData>& tracksToPrepare,
                                             double targetSampleRate)
{
    bool changed = false;
    for (auto& track : tracksToPrepare)
        for (auto& clip : track.clips)
            changed = resampleClipInternal(clip, targetSampleRate) || changed;
    return changed;
}

bool AudioEngine::resampleClip(AudioClip& clip, double targetSampleRate)
{
    return resampleClipInternal(clip, targetSampleRate);
}

//==============================================================================
// Export

void AudioEngine::exportTrackToWav(int trackIndex)
{
    const auto snapshot = getTracksSnapshot();
    if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(snapshot.size()))
        || snapshot[static_cast<size_t>(trackIndex)].clips.empty())
    {
        postStatusMessage(utf8(u8"書き出せるトラックが見つかりません。"), true);
        return;
    }
    const auto& track = snapshot[static_cast<size_t>(trackIndex)];
    const auto trackId = track.id;
    auto chooser = std::make_shared<juce::FileChooser>(
        utf8(u8"エクスポート先を選択"),
        juce::File::getSpecialLocation(juce::File::userDesktopDirectory)
            .getChildFile(track.name + ".wav"), "*.wav");
    auto safeThis = juce::WeakReference<AudioEngine>(this);
    chooser->launchAsync(juce::FileBrowserComponent::saveMode
                             | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::warnAboutOverwriting,
        [safeThis, chooser, trackId](const juce::FileChooser& fc)
        {
            if (safeThis == nullptr || fc.getResults().isEmpty()) return;
            const auto current = safeThis->getTracksSnapshot();
            const auto found = std::find_if(current.begin(), current.end(),
                [&trackId](const TrackData& item) { return item.id == trackId; });
            if (found == current.end()) return;
            ExportSettings settings;
            settings.target = ExportTarget::selectedTrack;
            settings.trackIndex = static_cast<int>(std::distance(current.begin(), found));
            settings.sampleRate = juce::roundToInt(safeThis->currentSampleRate.load());
            safeThis->startExport(fc.getResults()[0], settings);
        });
}

void AudioEngine::exportMixdownToWav()
{
    if (getTracksDuration(getTracksSnapshot()) <= 0.0)
    {
        postStatusMessage(utf8(u8"ミックスダウンできる音声がありません。"), true);
        return;
    }
    auto chooser = std::make_shared<juce::FileChooser>(
        utf8(u8"ミックスダウンを保存"),
        juce::File::getSpecialLocation(juce::File::userDesktopDirectory)
            .getChildFile("mixdown.wav"), "*.wav");
    auto safeThis = juce::WeakReference<AudioEngine>(this);
    chooser->launchAsync(juce::FileBrowserComponent::saveMode
                             | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::warnAboutOverwriting,
        [safeThis, chooser](const juce::FileChooser& fc)
        {
            if (safeThis == nullptr || fc.getResults().isEmpty()) return;
            ExportSettings settings;
            settings.target = ExportTarget::fullMix;
            settings.sampleRate = juce::roundToInt(safeThis->currentSampleRate.load());
            safeThis->startExport(fc.getResults()[0], settings);
        });
}

bool AudioEngine::startExport(const juce::File& outputFile,
                              const ExportSettings& requestedSettings)
{
    if (outputFile == juce::File() || outputFile.isDirectory())
        return false;
    if (playbackState.load() != PlaybackState::Stopped)
    {
        postStatusMessage(utf8(u8"書き出す前に再生または録音を停止してください。"), true);
        return false;
    }

    {
        const juce::ScopedLock lock(exportStatusLock);
        if (exportStatus.active)
            return false;
    }

    refreshTrackPluginLatencies();

    auto settings = requestedSettings;
    settings.sampleRate = settings.sampleRate == 48000 ? 48000 : 44100;
    settings.wavBits = settings.wavBits == 24 ? 24 : 16;
    settings.mp3Kbps = 192;
    settings.trackIndex = std::max(-1, settings.trackIndex);

    const auto snapshot = getTracksSnapshot();
    const double projectDuration = getTracksDuration(snapshot);
    if (snapshot.empty() || projectDuration <= 0.0)
    {
        postStatusMessage(utf8(u8"書き出せる音声がありません。"), true);
        return false;
    }
    if (settings.target == ExportTarget::selectedTrack
        && !juce::isPositiveAndBelow(settings.trackIndex,
                                     static_cast<int>(snapshot.size())))
    {
        postStatusMessage(utf8(u8"書き出すトラックを選んでください。"), true);
        return false;
    }

    double rangeStart = 0.0;
    double rangeEnd = projectDuration;
    if (settings.target == ExportTarget::abRange)
    {
        rangeStart = std::max(0.0, settings.rangeStartSeconds);
        rangeEnd = std::min(projectDuration, settings.rangeEndSeconds);
        if (rangeEnd <= rangeStart + 0.001)
        {
            postStatusMessage(utf8(u8"A/B範囲を先に設定してください。"), true);
            return false;
        }
    }
    else if (settings.target == ExportTarget::selectedTrack)
    {
        rangeEnd = snapshot[static_cast<size_t>(settings.trackIndex)].getDuration();
    }

    const auto totalSamples64 = static_cast<int64_t>(
        std::ceil((rangeEnd - rangeStart) * settings.sampleRate));
    if (totalSamples64 <= 0 || totalSamples64 > std::numeric_limits<int>::max())
    {
        postStatusMessage(utf8(u8"指定した長さを1つの音声ファイルへ書き出せません。"), true);
        return false;
    }

    exportCancelRequested.store(false);
    const auto generation = exportGeneration.fetch_add(1) + 1;
    {
        const juce::ScopedLock lock(exportStatusLock);
        exportStatus = {};
        exportStatus.active = true;
        exportStatus.progress = 0.0f;
        exportStatus.message = utf8(u8"音声を準備しています…");
        exportStatus.outputFile = outputFile;
    }

    const auto masterSettings = getProjectDawSettings().mastering;
    const float savedMasterVolume = masterVolume.load();
    const int renderSamples = static_cast<int>(totalSamples64);

    exportThreadPool.addJob(
        [this, snapshot, settings, outputFile, generation, rangeStart,
         rangeEnd, renderSamples, masterSettings, savedMasterVolume]() mutable
        {
            const auto isCancelled = [this, generation]
            {
                return exportCancelRequested.load()
                    || exportGeneration.load() != generation;
            };
            const auto publish = [this](float progress,
                                        const juce::String& message)
            {
                const juce::ScopedLock lock(exportStatusLock);
                exportStatus.progress = juce::jlimit(0.0f, 1.0f, progress);
                exportStatus.message = message;
            };
            const auto finish = [this, &isCancelled](bool succeeded,
                                                      const juce::String& message)
            {
                const juce::ScopedLock lock(exportStatusLock);
                exportStatus.active = false;
                exportStatus.completed = true;
                exportStatus.cancelled = isCancelled();
                exportStatus.succeeded = succeeded && !exportStatus.cancelled;
                exportStatus.progress = exportStatus.succeeded ? 1.0f
                                                                : exportStatus.progress;
                exportStatus.message = message;
            };

            try
            {
                juce::AudioBuffer<float> mix(2, renderSamples);
                mix.clear();
                const bool hasSolo = std::any_of(
                    snapshot.begin(), snapshot.end(),
                    [](const TrackData& track) { return track.isSolo; });

                std::vector<size_t> includedTracks;
                for (size_t i = 0; i < snapshot.size(); ++i)
                {
                    const auto& track = snapshot[i];
                    if (settings.target == ExportTarget::selectedTrack)
                    {
                        if (static_cast<int>(i) == settings.trackIndex)
                            includedTracks.push_back(i);
                    }
                    else if (!track.isMuted && (!hasSolo || track.isSolo))
                    {
                        includedTracks.push_back(i);
                    }
                }

                std::vector<int> offlinePluginLatencies(snapshot.size(), 0);
                int maximumEffectiveLatency = 0;
                for (const auto included : includedTracks)
                {
                    const auto& track = snapshot[included];
                    if (isCancelled())
                    {
                        finish(false, utf8(u8"書き出しをキャンセルしました。"));
                        return;
                    }

                    juce::String pluginError;
                    int pluginLatency = 0;
                    if (!measureTrackPluginLatencyForOffline(
                            track,
                            settings.sampleRate,
                            pluginLatency,
                            pluginError))
                    {
                        finish(false,
                               utf8(u8"書き出しを中止しました: ")
                                   + pluginError);
                        return;
                    }
                    offlinePluginLatencies[included] = pluginLatency;
                    const double speed = juce::jlimit(0.75, 1.5,
                                                      track.playbackSpeed);
                    const double scaledLatency = pluginLatency / speed;
                    maximumEffectiveLatency = std::max(
                        maximumEffectiveLatency,
                        juce::jlimit(0, 1920000,
                                     juce::roundToInt(scaledLatency)));
                }

                for (size_t rendered = 0; rendered < includedTracks.size(); ++rendered)
                {
                    if (isCancelled())
                    {
                        finish(false, utf8(u8"書き出しをキャンセルしました。"));
                        return;
                    }

                    const auto& track = snapshot[includedTracks[rendered]];
                    publish(0.05f + 0.65f
                                    * static_cast<float>(rendered)
                                    / static_cast<float>(std::max<size_t>(1,
                                                                  includedTracks.size())),
                            utf8(u8"トラックを処理中: ") + track.name);
                    const auto speed = juce::jlimit(0.75, 1.5,
                                                    track.playbackSpeed);
                    const int sourcePadding = juce::roundToInt(
                        std::ceil(maximumEffectiveLatency * speed));
                    const auto trackSourceSamples64 = static_cast<int64_t>(
                        std::ceil(track.getSourceDuration()
                                  * settings.sampleRate))
                        + sourcePadding;
                    if (trackSourceSamples64 <= 0
                        || trackSourceSamples64 > std::numeric_limits<int>::max())
                    {
                        continue;
                    }

                    auto renderedTrack = std::make_shared<juce::AudioBuffer<float>>(
                        2, static_cast<int>(trackSourceSamples64));
                    renderedTrack->clear();
                    for (const auto& clip : track.clips)
                    {
                        if (isCancelled())
                        {
                            finish(false, utf8(u8"書き出しをキャンセルしました。"));
                            return;
                        }
                        mixClipIntoBuffer(clip, *renderedTrack, 0,
                                          settings.sampleRate, 1.0f,
                                          true, isCancelled);
                    }

                    juce::String pluginError;
                    if (!processTrackPluginOffline(track,
                                                   *renderedTrack,
                                                   settings.sampleRate,
                                                   isCancelled,
                                                   pluginError))
                    {
                        finish(false,
                               isCancelled()
                                   ? utf8(u8"書き出しをキャンセルしました。")
                                   : utf8(u8"書き出しを中止しました: ")
                                         + pluginError);
                        return;
                    }
                    const auto semitones = juce::jlimit(-12.0, 12.0,
                                                        track.pitchSemitones);
                    if (std::abs(speed - 1.0) > 1.0e-6
                        || std::abs(semitones) > 1.0e-6)
                    {
                        renderedTrack = PlaybackTransform::processOffline(
                            *renderedTrack,
                            settings.sampleRate,
                            speed,
                            semitones,
                            isCancelled);
                    }
                    if (renderedTrack == nullptr || isCancelled())
                    {
                        finish(false, utf8(u8"書き出しをキャンセルしました。"));
                        return;
                    }

                    const int trackEffectiveLatency = juce::jlimit(
                        0,
                        maximumEffectiveLatency,
                        juce::roundToInt(
                            offlinePluginLatencies[includedTracks[rendered]]
                            / speed));
                    delayBufferInPlace(
                        *renderedTrack,
                        maximumEffectiveLatency - trackEffectiveLatency);

                    if (!processSpecialFxOffline(track,
                                                 *renderedTrack,
                                                 settings.sampleRate,
                                                 isCancelled,
                                                 -maximumEffectiveLatency))
                    {
                        finish(false,
                               isCancelled()
                                   ? utf8(u8"書き出しをキャンセルしました。")
                                   : utf8(u8"特殊FXの処理に失敗したため、書き出しを中止しました。"));
                        return;
                    }

                    SimpleMixProcessor simpleMix;
                    simpleMix.prepare(settings.sampleRate,
                                      std::max(1, currentBlockSize), 2);
                    simpleMix.setSettings(track.simpleMix);
                    simpleMix.setNoiseReductionSettings(track.noiseReduction);
                    constexpr int offlineProcessingChunk = 8192;
                    for (int position = 0;
                         position < renderedTrack->getNumSamples();
                         position += offlineProcessingChunk)
                    {
                        if (isCancelled())
                        {
                            finish(false,
                                   utf8(u8"書き出しをキャンセルしました。"));
                            return;
                        }
                        simpleMix.process(
                            *renderedTrack,
                            position,
                            std::min(offlineProcessingChunk,
                                     renderedTrack->getNumSamples()
                                         - position));
                    }

                    const auto pan = ClipRenderMath::getConstantPowerPanGains(track.pan);
                    renderedTrack->applyGain(0, 0, renderedTrack->getNumSamples(),
                                             track.volume * pan.left
                                                 * savedMasterVolume);
                    renderedTrack->applyGain(1, 0, renderedTrack->getNumSamples(),
                                             track.volume * pan.right
                                                 * savedMasterVolume);

                    const int sourceStart = std::max(
                        0,
                        juce::roundToInt(rangeStart * settings.sampleRate)
                            + maximumEffectiveLatency);
                    const int available = std::max(
                        0, renderedTrack->getNumSamples() - sourceStart);
                    const int samplesToMix = std::min(renderSamples, available);
                    for (int channel = 0; channel < 2; ++channel)
                        mix.addFrom(channel, 0, *renderedTrack, channel,
                                    sourceStart, samplesToMix);
                }

                if (includedTracks.empty())
                {
                    finish(false, utf8(u8"書き出し対象のトラックがありません。"));
                    return;
                }

                const float preMasterPeak = mix.getMagnitude(0, mix.getNumSamples());
                publish(0.74f, utf8(u8"ピークを保護しています…"));
                MasterBusProcessor offlineMaster;
                offlineMaster.prepare(settings.sampleRate,
                                      std::max(1, currentBlockSize), 2);
                auto exportMasterSettings = masterSettings;
                // 画面で「元の音」を試聴中でも、書き出しは常に
                // 確定したマスタリング結果を使う。
                exportMasterSettings.auditionProcessed = true;
                offlineMaster.setSettings(exportMasterSettings);
                const int masterLatency = offlineMaster.getLatencySamples();
                if (masterLatency > 0)
                {
                    // ルックアヘッド分の無音を末尾に足してから戻す。
                    // これで先頭の無音化と末尾の切れを防ぐ。
                    juce::AudioBuffer<float> masterWork(
                        2, renderSamples + masterLatency);
                    masterWork.clear();
                    for (int channel = 0; channel < 2; ++channel)
                        masterWork.copyFrom(channel, 0, mix, channel, 0,
                                            renderSamples);
                    constexpr int masterProcessingChunk = 8192;
                    for (int position = 0;
                         position < masterWork.getNumSamples();
                         position += masterProcessingChunk)
                    {
                        if (isCancelled())
                        {
                            finish(false,
                                   utf8(u8"書き出しをキャンセルしました。"));
                            return;
                        }
                        offlineMaster.process(
                            masterWork,
                            position,
                            std::min(masterProcessingChunk,
                                     masterWork.getNumSamples() - position));
                    }
                    for (int channel = 0; channel < 2; ++channel)
                        mix.copyFrom(channel, 0, masterWork, channel,
                                     masterLatency, renderSamples);
                }
                else
                {
                    constexpr int masterProcessingChunk = 8192;
                    for (int position = 0;
                         position < mix.getNumSamples();
                         position += masterProcessingChunk)
                    {
                        if (isCancelled())
                        {
                            finish(false,
                                   utf8(u8"書き出しをキャンセルしました。"));
                            return;
                        }
                        offlineMaster.process(
                            mix,
                            position,
                            std::min(masterProcessingChunk,
                                     mix.getNumSamples() - position));
                    }
                }

                // マスタリングOFFやリミッターOFFでも、最終ファイルは
                // 0 dBFSを超えないように最後の安全ガードを通す。
                float protectedPeak = 0.0f;
                for (int channel = 0; channel < mix.getNumChannels(); ++channel)
                {
                    auto* samples = mix.getWritePointer(channel);
                    for (int sample = 0; sample < mix.getNumSamples(); ++sample)
                    {
                        if ((sample & 0x3fff) == 0 && isCancelled())
                        {
                            finish(false,
                                   utf8(u8"書き出しをキャンセルしました。"));
                            return;
                        }
                        if (!std::isfinite(samples[sample]))
                            samples[sample] = 0.0f;
                        protectedPeak = std::max(protectedPeak,
                                                 std::abs(samples[sample]));
                    }
                }
                const bool finalPeakProtectionApplied = protectedPeak > 0.999f;
                if (finalPeakProtectionApplied)
                    mix.applyGain(0.999f / protectedPeak);

                if (isCancelled())
                {
                    finish(false, utf8(u8"書き出しをキャンセルしました。"));
                    return;
                }

                juce::AudioBuffer<float> output(
                    static_cast<int>(settings.channels), renderSamples);
                if (settings.channels == ExportChannels::mono)
                {
                    output.clear();
                    output.addFrom(0, 0, mix, 0, 0, renderSamples, 0.5f);
                    output.addFrom(0, 0, mix, 1, 0, renderSamples, 0.5f);
                }
                else
                {
                    output.makeCopyOf(mix, true);
                }

                // MP3の不可逆圧縮では、PCMが0 dBFS未満でも符号化後に
                // ピークが少し増えることがある。配信用の安全余裕として
                // MP3だけは-1 dBFSを超える場合に追加で抑える。
                bool mp3HeadroomApplied = false;
                if (settings.format == ExportFormat::mp3)
                {
                    const float mp3Ceiling = juce::Decibels::decibelsToGain(
                        -1.0f);
                    const float outputPeak = output.getMagnitude(
                        0, output.getNumSamples());
                    if (std::isfinite(outputPeak) && outputPeak > mp3Ceiling)
                    {
                        output.applyGain(mp3Ceiling / outputPeak);
                        mp3HeadroomApplied = true;
                    }
                }

                const auto writeWav = [&output, &settings, &isCancelled](
                                          const juce::File& file,
                                          int bits)
                {
                    auto stream = file.createOutputStream();
                    if (stream == nullptr || !stream->openedOk())
                        return false;
                    juce::WavAudioFormat wav;
                    std::unique_ptr<juce::AudioFormatWriter> writer(
                        wav.createWriterFor(stream.release(), settings.sampleRate,
                                            output.getNumChannels(), bits, {}, 0));
                    if (writer == nullptr)
                        return false;

                    constexpr int writeChunkSamples = 65536;
                    for (int position = 0; position < output.getNumSamples();
                         position += writeChunkSamples)
                    {
                        if (isCancelled())
                            return false;
                        const int count = std::min(
                            writeChunkSamples,
                            output.getNumSamples() - position);
                        if (!writer->writeFromAudioSampleBuffer(
                                output, position, count))
                            return false;
                    }
                    return !isCancelled();
                };

                publish(0.82f, utf8(u8"ファイルを作成しています…"));
                bool success = false;
                juce::String failureDetail;
                if (settings.format == ExportFormat::wav)
                {
                    juce::TemporaryFile temporary(outputFile);
                    success = writeWav(temporary.getFile(), settings.wavBits)
                        && temporary.overwriteTargetFileWithTemporary();
                }
                else
                {
                    const auto result = WindowsMp3Encoder::encode(
                        output, settings.sampleRate, settings.mp3Kbps,
                        outputFile, isCancelled);
                    success = result.succeeded;
                    failureDetail = result.errorMessage;
                }

                if (isCancelled())
                {
                    finish(false, utf8(u8"書き出しをキャンセルしました。"));
                    return;
                }
                if (!success)
                {
                    auto message = utf8(u8"音声ファイルの作成に失敗しました。 ")
                                 + failureDetail.substring(0, 300);
                    finish(false, message);
                    postStatusMessage(message, true);
                    return;
                }

                auto message = utf8(u8"書き出しました: ") + outputFile.getFileName();
                if (preMasterPeak > 1.0f || finalPeakProtectionApplied)
                    message += utf8(u8"（処理前に音割れの可能性があったためピーク保護を適用）");
                else if (mp3HeadroomApplied)
                    message += utf8(u8"（MP3変換後の音割れを防ぐ安全余裕を適用）");
                finish(true, message);
                postStatusMessage(message, false);
            }
            catch (const std::exception& error)
            {
                const auto message = utf8(u8"書き出し中にエラーが発生しました: ")
                                   + error.what();
                finish(false, message);
                postStatusMessage(message, true);
            }
            catch (...)
            {
                const auto message = utf8(u8"書き出し中に不明なエラーが発生しました。");
                finish(false, message);
                postStatusMessage(message, true);
            }
        });
    return true;
}

void AudioEngine::cancelExport()
{
    exportCancelRequested.store(true);
    const juce::ScopedLock lock(exportStatusLock);
    if (exportStatus.active)
        exportStatus.message = utf8(u8"キャンセルしています…");
}

AudioEngine::ExportStatus AudioEngine::getExportStatus() const
{
    const juce::ScopedLock lock(exportStatusLock);
    return exportStatus;
}

//==============================================================================
// Realtime internals

bool AudioEngine::processTrackPlugin(const juce::String& trackId,
                                     juce::AudioBuffer<float>& buffer,
                                     int numSamples,
                                     int64_t timelineStartSample) noexcept
{
    if (trackId.isEmpty() || buffer.getNumChannels() < 2
        || numSamples <= 0 || numSamples > buffer.getNumSamples())
        return false;

    const juce::SpinLock::ScopedTryLockType lock(pluginLock);
    if (!lock.isLocked())
        return false;

    bool processedAny = false;
    juce::ScopedNoDenormals noDenormals;
    for (auto& slot : trackPlugins)
    {
        if (slot == nullptr || slot->trackId != trackId
            || slot->bypassed.load(std::memory_order_relaxed)
            || (slot->effectSlotId.isNotEmpty()
                && slot->effectChainBypassed.load(std::memory_order_relaxed)))
            continue;

        const bool useEditorInstance = !slot->processingUsesAra
            && slot->araHost != nullptr
            && (slot->auditionWithEditorInstance.load()
                || slot->useEditorInstanceForPlayback.load());
        auto processingInstance = useEditorInstance ? slot->instance
                                                    : slot->processingInstance;
        const int processingChannels = useEditorInstance
            ? slot->editorChannels : slot->processingChannels;
        if (processingInstance == nullptr || processingChannels <= 0)
            continue;

        auto& instance = *processingInstance;
        for (int position = 0; position < numSamples;
             position += currentBlockSize)
        {
            const int blockLength = std::min(currentBlockSize,
                                             numSamples - position);
            slot->playHead.update(timelineStartSample + position,
                                  std::max(1.0, currentSampleRate.load()),
                                  playbackState.load());
            juce::MidiBuffer midi;
            if (processingChannels == 1)
            {
                auto* left = buffer.getWritePointer(0, position);
                const auto* right = buffer.getReadPointer(1, position);
                for (int sample = 0; sample < blockLength; ++sample)
                    left[sample] = (left[sample] + right[sample]) * 0.5f;

                juce::AudioBuffer<float> monoView(
                    buffer.getArrayOfWritePointers(), 1, position, blockLength);
                instance.processBlock(monoView, midi);
                buffer.copyFrom(1, position, buffer, 0, position, blockLength);
            }
            else
            {
                juce::AudioBuffer<float> stereoView(
                    buffer.getArrayOfWritePointers(), 2, position, blockLength);
                instance.processBlock(stereoView, midi);
                if (slot->duplicateLeftOutput)
                    buffer.copyFrom(1, position, buffer, 0, position,
                                    blockLength);
            }
        }
        processedAny = true;
    }
    return processedAny;
}

bool AudioEngine::measureTrackPluginLatencyForOffline(
    const TrackData& track,
    double sampleRate,
    int& latencySamples,
    juce::String& errorMessage)
{
    latencySamples = 0;
    errorMessage.clear();
    if (track.id.isEmpty() || !std::isfinite(sampleRate)
        || sampleRate < 8000.0 || sampleRate > 384000.0)
    {
        errorMessage = utf8(u8"VST3の書き出し設定が正しくありません。");
        return false;
    }

    std::vector<OfflinePluginExpectation> expectations;
    if (!buildOfflinePluginExpectations(track, expectations, errorMessage))
        return false;
    if (expectations.empty())
        return true;

    const auto trackName = track.name.isNotEmpty()
        ? track.name : utf8(u8"名称未設定");
    const juce::SpinLock::ScopedLockType lock(pluginLock);
    const double restoreSampleRate = juce::jlimit(
        8000.0, 384000.0, std::max(1.0, currentSampleRate.load()));
    const int blockSize = std::max(1, currentBlockSize);

    for (const auto& expected : expectations)
    {
        const auto runtime = std::find_if(
            trackPlugins.begin(), trackPlugins.end(),
            [&track, &expected](const std::unique_ptr<TrackPluginSlot>& slot)
            {
                return slot != nullptr
                    && slot->trackId == track.id
                    && slot->effectSlotId == expected.effectSlotId;
            });
        if (runtime == trackPlugins.end())
        {
            errorMessage = utf8(u8"トラック「") + trackName
                + utf8(u8"」のVST3「") + expected.displayName
                + utf8(u8"」が読み込まれていません。VST3を再読み込みしてから書き出してください。");
            return false;
        }

        auto& slot = **runtime;
        const bool useEditorInstance = !slot.processingUsesAra
            && slot.araHost != nullptr
            && slot.useEditorInstanceForPlayback.load();
        auto selectedInstance = useEditorInstance ? slot.instance
                                                  : slot.processingInstance;
        if (selectedInstance == nullptr)
        {
            errorMessage = utf8(u8"トラック「") + trackName
                + utf8(u8"」のVST3「") + expected.displayName
                + utf8(u8"」を処理できません。VST3を再読み込みしてから書き出してください。");
            return false;
        }

        auto& instance = *selectedInstance;
        const auto restoreForRealtime = [&]() noexcept
        {
            bool restored = true;
            try
            {
                instance.releaseResources();
            }
            catch (...)
            {
                restored = false;
            }
            try
            {
                instance.setNonRealtime(false);
            }
            catch (...)
            {
                restored = false;
            }

            int restoredChannels = 0;
            try
            {
                restoredChannels = preparePluginInstance(
                    instance, restoreSampleRate, blockSize);
            }
            catch (...)
            {
                restoredChannels = 0;
            }
            restored = restored && restoredChannels > 0;
            if (selectedInstance == slot.instance)
                slot.editorChannels = restoredChannels;
            if (selectedInstance == slot.processingInstance)
                slot.processingChannels = restoredChannels;

            int restoredLatency = 0;
            if (restoredChannels > 0)
            {
                try
                {
                    restoredLatency = juce::jlimit(
                        0, 1920000, instance.getLatencySamples());
                }
                catch (...)
                {
                    restored = false;
                }
            }
            slot.latencySamples.store(restoredLatency,
                                      std::memory_order_relaxed);
            slot.playHead.update(playbackPositionInSamples.load(),
                                 restoreSampleRate,
                                 playbackState.load());
            return restored;
        };

        bool measured = false;
        int measuredLatency = 0;
        try
        {
            instance.releaseResources();
            instance.setNonRealtime(true);
            const int offlineChannels = preparePluginInstance(
                instance, sampleRate, blockSize);
            if (offlineChannels > 0)
            {
                measuredLatency = juce::jlimit(
                    0, 1920000, instance.getLatencySamples());
                measured = true;
            }
        }
        catch (...)
        {
            measured = false;
        }

        const bool restored = restoreForRealtime();
        if (!restored)
        {
            errorMessage = utf8(u8"VST3「") + expected.displayName
                + utf8(u8"」を通常再生用に戻せませんでした。アプリを再起動してVST3を読み込み直してください。");
            return false;
        }
        if (!measured)
        {
            errorMessage = utf8(u8"トラック「") + trackName
                + utf8(u8"」のVST3「") + expected.displayName
                + utf8(u8"」を選択したサンプルレートで準備できませんでした。");
            return false;
        }

        const int remaining = 1920000 - latencySamples;
        latencySamples += std::min(remaining, measuredLatency);
    }

    return true;
}

bool AudioEngine::processTrackPluginOffline(
    const TrackData& track,
    juce::AudioBuffer<float>& buffer,
    double sampleRate,
    const std::function<bool()>& shouldCancel,
    juce::String& errorMessage)
{
    errorMessage.clear();
    const auto isCancelled = [&shouldCancel]
    {
        return shouldCancel && shouldCancel();
    };
    if (isCancelled())
    {
        errorMessage = utf8(u8"書き出しをキャンセルしました。");
        return false;
    }
    if (track.id.isEmpty() || !std::isfinite(sampleRate)
        || sampleRate < 8000.0 || sampleRate > 384000.0
        || buffer.getNumChannels() < 2 || buffer.getNumSamples() <= 0)
    {
        errorMessage = utf8(u8"VST3で処理する音声または書き出し設定が正しくありません。");
        return false;
    }

    std::vector<OfflinePluginExpectation> expectations;
    if (!buildOfflinePluginExpectations(track, expectations, errorMessage))
        return false;
    if (expectations.empty())
        return true;

    const auto trackName = track.name.isNotEmpty()
        ? track.name : utf8(u8"名称未設定");
    const juce::SpinLock::ScopedLockType lock(pluginLock);
    const double restoreSampleRate = juce::jlimit(
        8000.0, 384000.0, std::max(1.0, currentSampleRate.load()));
    const int blockSize = std::max(1, currentBlockSize);
    juce::ScopedNoDenormals noDenormals;

    for (const auto& expected : expectations)
    {
        if (isCancelled())
        {
            errorMessage = utf8(u8"書き出しをキャンセルしました。");
            return false;
        }

        const auto runtime = std::find_if(
            trackPlugins.begin(), trackPlugins.end(),
            [&track, &expected](const std::unique_ptr<TrackPluginSlot>& slot)
            {
                return slot != nullptr
                    && slot->trackId == track.id
                    && slot->effectSlotId == expected.effectSlotId;
            });
        if (runtime == trackPlugins.end())
        {
            errorMessage = utf8(u8"トラック「") + trackName
                + utf8(u8"」のVST3「") + expected.displayName
                + utf8(u8"」が読み込まれていません。VST3を再読み込みしてから書き出してください。");
            return false;
        }

        auto& slot = **runtime;
        const bool useEditorInstance = !slot.processingUsesAra
            && slot.araHost != nullptr
            && slot.useEditorInstanceForPlayback.load();
        auto selectedInstance = useEditorInstance ? slot.instance
                                                  : slot.processingInstance;
        if (selectedInstance == nullptr)
        {
            errorMessage = utf8(u8"トラック「") + trackName
                + utf8(u8"」のVST3「") + expected.displayName
                + utf8(u8"」を処理できません。VST3を再読み込みしてから書き出してください。");
            return false;
        }

        auto& instance = *selectedInstance;
        const auto restoreForRealtime = [&]() noexcept
        {
            bool restored = true;
            try
            {
                instance.releaseResources();
            }
            catch (...)
            {
                restored = false;
            }
            try
            {
                instance.setNonRealtime(false);
            }
            catch (...)
            {
                restored = false;
            }

            int restoredChannels = 0;
            try
            {
                restoredChannels = preparePluginInstance(
                    instance, restoreSampleRate, blockSize);
            }
            catch (...)
            {
                restoredChannels = 0;
            }
            restored = restored && restoredChannels > 0;
            if (selectedInstance == slot.instance)
                slot.editorChannels = restoredChannels;
            if (selectedInstance == slot.processingInstance)
                slot.processingChannels = restoredChannels;

            int restoredLatency = 0;
            if (restoredChannels > 0)
            {
                try
                {
                    restoredLatency = juce::jlimit(
                        0, 1920000, instance.getLatencySamples());
                }
                catch (...)
                {
                    restored = false;
                }
            }
            slot.latencySamples.store(restoredLatency,
                                      std::memory_order_relaxed);
            slot.playHead.update(playbackPositionInSamples.load(),
                                 restoreSampleRate,
                                 playbackState.load());
            return restored;
        };

        int offlineChannels = 0;
        bool processingSucceeded = true;
        try
        {
            instance.releaseResources();
            instance.setNonRealtime(true);
            offlineChannels = preparePluginInstance(
                instance, sampleRate, blockSize);
        }
        catch (...)
        {
            processingSucceeded = false;
        }
        if (offlineChannels <= 0)
            processingSucceeded = false;

        if (processingSucceeded)
        {
            try
            {
                for (int position = 0; position < buffer.getNumSamples();
                     position += blockSize)
                {
                    if (isCancelled())
                    {
                        processingSucceeded = false;
                        errorMessage = utf8(u8"書き出しをキャンセルしました。");
                        break;
                    }

                    const int blockLength = std::min(
                        blockSize, buffer.getNumSamples() - position);
                    slot.playHead.update(position,
                                         sampleRate,
                                         PlaybackState::Playing);
                    juce::MidiBuffer midi;
                    if (offlineChannels == 1)
                    {
                        auto* left = buffer.getWritePointer(0, position);
                        const auto* right = buffer.getReadPointer(1, position);
                        for (int sample = 0; sample < blockLength; ++sample)
                            left[sample] = (left[sample] + right[sample]) * 0.5f;

                        juce::AudioBuffer<float> monoView(
                            buffer.getArrayOfWritePointers(),
                            1,
                            position,
                            blockLength);
                        instance.processBlock(monoView, midi);
                        buffer.copyFrom(1, position, buffer, 0, position,
                                        blockLength);
                    }
                    else
                    {
                        juce::AudioBuffer<float> stereoView(
                            buffer.getArrayOfWritePointers(),
                            2,
                            position,
                            blockLength);
                        instance.processBlock(stereoView, midi);
                        if (slot.duplicateLeftOutput)
                            buffer.copyFrom(1, position, buffer, 0, position,
                                            blockLength);
                    }
                }
            }
            catch (...)
            {
                processingSucceeded = false;
            }
        }

        const bool restored = restoreForRealtime();
        if (!restored)
        {
            errorMessage = utf8(u8"VST3「") + expected.displayName
                + utf8(u8"」を通常再生用に戻せませんでした。アプリを再起動してVST3を読み込み直してください。");
            return false;
        }
        if (!processingSucceeded)
        {
            if (errorMessage.isEmpty())
            {
                errorMessage = utf8(u8"トラック「") + trackName
                    + utf8(u8"」のVST3「") + expected.displayName
                    + utf8(u8"」を使った音声処理に失敗しました。");
            }
            return false;
        }
    }

    return true;
}

void AudioEngine::processPlayback(juce::AudioBuffer<float>& outputBuffer, int numSamples)
{
    if (playbackState.load() == PlaybackState::Stopped)
        return;

    const double sampleRate = std::max(1.0, currentSampleRate.load());
    const double duration = cachedDuration.load();
    const auto playbackTempoMap = std::atomic_load(&tempoMapSnapshot);
    int64_t position = playbackPositionInSamples.load();
    int outputOffset = 0;
    int remaining = numSamples;

    const bool useLoop = loopEnabled.load()
        && hasLoopStart.load() && hasLoopEnd.load()
        && playbackState.load() == PlaybackState::Playing;
    const int64_t loopStart = static_cast<int64_t>(
        std::llround(loopStartSeconds.load() * sampleRate));
    const int64_t loopEnd = static_cast<int64_t>(
        std::llround(loopEndSeconds.load() * sampleRate));
    const int loopFadeSamples = useLoop
        ? std::max(1, std::min(
              juce::roundToInt(sampleRate * 0.005),
              static_cast<int>(std::max<int64_t>(1,
                  (loopEnd - loopStart) / 2))))
        : 0;
    if (!useLoop)
        loopFadeInSamplesRemaining.store(0);

    while (remaining > 0 && playbackState.load() != PlaybackState::Stopped)
    {
        const auto segment = ClipRenderMath::decideLoopSegment(
            position,
            remaining,
            useLoop ? loopStart : 0,
            useLoop ? loopEnd : 0);
        if (segment.samplesToRender <= 0)
            break;

        if (segment.startSample != position)
        {
            if (useLoop)
                loopFadeInSamplesRemaining.store(loopFadeSamples);
            const juce::CriticalSection::ScopedTryLockType lock(tracksLock);
            if (lock.isLocked())
                resetTrackPlaybackTransformsLocked();
        }
        position = segment.startSample;
        const int segmentSamples = segment.samplesToRender;

        {
            const juce::CriticalSection::ScopedTryLockType lock(tracksLock);
            if (lock.isLocked())
            {
                if (playbackTransformResetRequested.exchange(false))
                    resetTrackPlaybackTransformsLocked();

                const bool hasSolo = std::any_of(tracks.begin(), tracks.end(),
                    [](const TrackData& track) { return track.isSolo; });
                int maximumChainLatency = 0;
                for (size_t latencyTrackIndex = 0;
                     latencyTrackIndex < tracks.size();
                     ++latencyTrackIndex)
                {
                    const auto& latencyTrack = tracks[latencyTrackIndex];
                    if (latencyTrack.isMuted
                        || (hasSolo && !latencyTrack.isSolo)
                        || (playbackState.load() == PlaybackState::Recording
                            && latencyTrack.id == recordingTrackId)
                        || latencyTrackIndex >= trackPlaybackSlots.size()
                        || trackPlaybackSlots[latencyTrackIndex] == nullptr
                        || trackPlaybackSlots[latencyTrackIndex]->trackId
                            != latencyTrack.id)
                        continue;
                    const double latencySpeed = juce::jlimit(
                        0.75, 1.5, latencyTrack.playbackSpeed);
                    const int effectiveLatency = juce::roundToInt(
                        trackPlaybackSlots[latencyTrackIndex]
                            ->chainLatencySamples.load(
                                std::memory_order_acquire)
                        / latencySpeed);
                    maximumChainLatency = std::max(maximumChainLatency,
                                                   effectiveLatency);
                }
                for (size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex)
                {
                    const auto& track = tracks[trackIndex];
                    if (track.isMuted || (hasSolo && !track.isSolo)) continue;
                    if (playbackState.load() == PlaybackState::Recording
                        && track.id == recordingTrackId)
                        continue;

                    TrackPlaybackSlot* playbackSlot = nullptr;
                    if (trackIndex < trackPlaybackSlots.size()
                        && trackPlaybackSlots[trackIndex] != nullptr
                        && trackPlaybackSlots[trackIndex]->trackId == track.id)
                        playbackSlot = trackPlaybackSlots[trackIndex].get();

                    const double speed = juce::jlimit(0.75, 1.5, track.playbackSpeed);
                    const double semitones = juce::jlimit(-12.0,
                                                          12.0,
                                                          track.pitchSemitones);
                    const bool transformActive = playbackSlot != nullptr
                        && (std::abs(speed - 1.0) > 1.0e-6
                            || std::abs(semitones) > 1.0e-6);
                    const int sourceSamples = transformActive
                        ? playbackSlot->transform.getInputSamplesForOutput(
                            segmentSamples, speed)
                        : segmentSamples;
                    if ((transformActive
                            && sourceSamples
                                > trackTransformInputBuffer.getNumSamples())
                        || (!transformActive
                            && sourceSamples
                                > trackProcessingBuffer.getNumSamples())
                        || segmentSamples > trackProcessingBuffer.getNumSamples())
                        continue;

                    int64_t sourceStart = position;
                    if (transformActive)
                    {
                        if (!playbackSlot->transformInputPositionValid)
                        {
                            playbackSlot->transformInputPosition =
                                static_cast<int64_t>(std::llround(
                                    static_cast<double>(position) * speed));
                            playbackSlot->transformInputPositionValid = true;
                        }
                        sourceStart = playbackSlot->transformInputPosition;
                    }
                    auto& sourceBuffer = transformActive
                        ? trackTransformInputBuffer
                        : trackProcessingBuffer;
                    sourceBuffer.clear(0, sourceSamples);
                    for (const auto& clip : track.clips)
                        mixClipIntoBuffer(clip,
                                          sourceBuffer,
                                          sourceStart,
                                          sampleRate,
                                          1.0f);
                    processTrackPlugin(track.id,
                                       sourceBuffer,
                                       sourceSamples,
                                       sourceStart);

                    if (transformActive)
                    {
                        trackProcessingBuffer.clear(0, segmentSamples);
                        playbackSlot->transform.process(sourceBuffer,
                                                        sourceSamples,
                                                        trackProcessingBuffer,
                                                        segmentSamples,
                                                        speed,
                                                        semitones);
                        playbackSlot->transformInputPosition += sourceSamples;
                    }

                    applyTrackOutputProcessing(track,
                                               playbackSlot,
                                               trackProcessingBuffer,
                                               segmentSamples,
                                               position);
                    if (playbackSlot != nullptr)
                    {
                        const int trackLatency = juce::roundToInt(
                            playbackSlot->chainLatencySamples.load(
                                std::memory_order_acquire)
                            / speed);
                        playbackSlot->processLatencyCompensation(
                            trackProcessingBuffer,
                            segmentSamples,
                            std::max(0, maximumChainLatency - trackLatency));
                    }
                    const auto panGains =
                        ClipRenderMath::getConstantPowerPanGains(track.pan);
                    const float gain = track.volume * masterVolume.load();
                    if (outputBuffer.getNumChannels() == 1)
                    {
                        outputBuffer.addFrom(0, outputOffset,
                                             trackProcessingBuffer, 0, 0,
                                             segmentSamples,
                                             gain * panGains.left);
                        outputBuffer.addFrom(0, outputOffset,
                                             trackProcessingBuffer, 1, 0,
                                             segmentSamples,
                                             gain * panGains.right);
                    }
                    else
                    {
                        outputBuffer.addFrom(0, outputOffset,
                                             trackProcessingBuffer, 0, 0,
                                             segmentSamples,
                                             gain * panGains.left);
                        outputBuffer.addFrom(1, outputOffset,
                                             trackProcessingBuffer, 1, 0,
                                             segmentSamples,
                                             gain * panGains.right);
                        for (int channel = 2;
                             channel < outputBuffer.getNumChannels(); ++channel)
                            outputBuffer.addFrom(channel, outputOffset,
                                                 trackProcessingBuffer,
                                                 channel % 2, 0,
                                                 segmentSamples,
                                                 gain * 0.5f);
                    }
                }
            }
        }

        juce::AudioBuffer<float> metronomeView(
            outputBuffer.getArrayOfWritePointers(),
            outputBuffer.getNumChannels(),
            outputOffset,
            segmentSamples);
        if (useLoop && loopFadeSamples > 0)
        {
            int fadeInRemaining = loopFadeInSamplesRemaining.load();
            constexpr double halfPi = juce::MathConstants<double>::halfPi;
            for (int sample = 0; sample < segmentSamples; ++sample)
            {
                double gain = 1.0;
                if (fadeInRemaining > 0)
                {
                    const int elapsed = loopFadeSamples - fadeInRemaining;
                    gain *= std::sin(halfPi * std::clamp(
                        static_cast<double>(elapsed) / loopFadeSamples,
                        0.0,
                        1.0));
                    --fadeInRemaining;
                }

                const int64_t samplesToLoopEnd = loopEnd
                    - (position + static_cast<int64_t>(sample));
                if (samplesToLoopEnd <= loopFadeSamples)
                {
                    gain *= std::sin(halfPi * std::clamp(
                        static_cast<double>(samplesToLoopEnd) / loopFadeSamples,
                        0.0,
                        1.0));
                }

                const float sampleGain = static_cast<float>(gain);
                for (int channel = 0;
                     channel < metronomeView.getNumChannels();
                     ++channel)
                    metronomeView.getWritePointer(channel)[sample] *= sampleGain;
            }
            loopFadeInSamplesRemaining.store(fadeInRemaining);
        }
        const auto realtimeMasterSettings = masterBusProcessor.getSettings();
        if (realtimeMasterSettings.enabled)
        {
            // 前ブロックで測った元音と、比較ゲイン適用前の仕上げ音を使う。
            // 最終出力を再び候補にすると補正量が往復するため使用しない。
            masterBusProcessor.setComparisonLoudness(
                masterBusProcessor.getApproximateInputLoudnessLufs(),
                masterBusProcessor.getApproximateProcessedLoudnessLufs());
        }
        else
        {
            masterBusProcessor.clearComparisonLoudness();
        }
        masterBusProcessor.process(metronomeView, 0, segmentSamples);

        const auto tempo = getTempoPointAt(
            static_cast<double>(position) / sampleRate);
        metronome.setBpm(juce::roundToInt(tempo.bpm));
        metronome.setTimeSignature(tempo.numerator, tempo.denominator);
        if (playbackTempoMap != nullptr)
            metronome.processBlock(metronomeView, segmentSamples, position,
                                   *playbackTempoMap);
        else
            metronome.processBlock(metronomeView, segmentSamples, position);

        outputOffset += segmentSamples;
        remaining -= segmentSamples;
        position = segment.nextSample;
        if (segment.wrapped)
        {
            loopFadeInSamplesRemaining.store(loopFadeSamples);
            const juce::CriticalSection::ScopedTryLockType lock(tracksLock);
            if (lock.isLocked())
                resetTrackPlaybackTransformsLocked();
        }
    }

    playbackPositionInSamples.store(position);
    currentPlaybackPosition.store(static_cast<double>(position) / sampleRate);

    if (!useLoop && duration > 0.0
        && currentPlaybackPosition.load() >= duration
        && playbackState.load() == PlaybackState::Playing)
    {
        playbackState.store(PlaybackState::Stopped);
        currentPlaybackPosition.store(0.0);
        playbackPositionInSamples.store(0);
        triggerAsyncUpdate();
    }
}

void AudioEngine::processRecording(const float* const* inputChannelData,
                                   int numInputChannels,
                                   int numSamples,
                                   int inputStartOffset)
{
    const juce::CriticalSection::ScopedTryLockType lock(recordingLock);
    if (!lock.isLocked())
        return;

    const float* left = inputChannelData != nullptr && numInputChannels > 0
                            ? inputChannelData[0]
                            : nullptr;
    const float* right = inputChannelData != nullptr && numInputChannels > 1
                             ? inputChannelData[1]
                             : nullptr;
    if (left == nullptr && right != nullptr)
        left = right;

    if (left == nullptr)
    {
        recordingBuffer.appendValue(0.0f, static_cast<size_t>(numSamples));
        if (recordingNumChannels >= 2)
            recordingBufferRight.appendValue(0.0f, static_cast<size_t>(numSamples));
        recordingSampleCount.fetch_add(numSamples);
        appendRecordingPreviewPoint(0.0f);
        queueMissingInputWarning();
        return;
    }

    recordingHadValidInput.store(true);
    float blockPeak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        blockPeak = std::max(blockPeak, std::abs(left[inputStartOffset + i]));
        if (right != nullptr)
            blockPeak = std::max(blockPeak, std::abs(right[inputStartOffset + i]));
    }
    recordingPeak.store(std::max(recordingPeak.load(), blockPeak));
    recordingSampleCount.fetch_add(numSamples);
    appendRecordingPreviewPoint(blockPeak);

    if (recordingNumChannels == 0)
    {
        recordingNumChannels = right != nullptr ? 2 : 1;
        if (recordingNumChannels >= 2)
            recordingBufferRight.resize(recordingBuffer.size(), 0.0f);
    }
    else if (recordingNumChannels == 1 && right != nullptr)
    {
        recordingNumChannels = 2;
        recordingBufferRight.resize(recordingBuffer.size(), 0.0f);
    }

    recordingBuffer.append(left + inputStartOffset, static_cast<size_t>(numSamples));

    if (recordingNumChannels >= 2)
    {
        const float* rightSource = right != nullptr ? right : left;
        recordingBufferRight.append(rightSource + inputStartOffset,
                                    static_cast<size_t>(numSamples));
    }
}

void AudioEngine::applyTrackOutputProcessing(
    const TrackData& track,
    TrackPlaybackSlot* playbackSlot,
    juce::AudioBuffer<float>& buffer,
    int numSamples,
    int64_t timelineStartSample) noexcept
{
    if (playbackSlot == nullptr || numSamples <= 0)
        return;
    playbackSlot->processSpecialFx(buffer, numSamples, timelineStartSample);
    // Plugin output is delayed relative to the source timeline. The following
    // cross-track compensation runs later; offline export uses the equivalent
    // common-latency offset after alignment.
    const int effectiveLatency = juce::roundToInt(
        playbackSlot->chainLatencySamples.load(std::memory_order_acquire)
        / juce::jlimit(0.75, 1.5, track.playbackSpeed));
    for (auto& processor : playbackSlot->automationProcessors)
        processor->process(buffer, 0, numSamples, timelineStartSample - effectiveLatency);
    playbackSlot->simpleMix.setSettings(track.simpleMix);
    playbackSlot->simpleMix.setNoiseReductionSettings(track.noiseReduction);
    playbackSlot->simpleMix.process(buffer, 0, numSamples);
}

double AudioEngine::mapRhythmTimeToSource(const AudioClip& clip,
                                          double targetTime) noexcept
{
    return RhythmWarp::mapTargetToSource(clip.rhythmMarkers,
                                         clip.duration,
                                         targetTime);
}

void AudioEngine::mixClipIntoBuffer(const AudioClip& clip,
                                    juce::AudioBuffer<float>& destination,
                                    int64_t timelineStartSample,
                                    double timelineSampleRate,
                                    float gain,
                                    bool ensureRhythmRender,
                                    const std::function<bool()>& shouldCancel) const
{
    if (clip.buffer == nullptr || timelineSampleRate <= 0.0 || clip.duration <= 0.0)
        return;

    std::shared_ptr<const RhythmRenderCache> synchronousRhythmRender;
    const RhythmRenderCache* rhythmRender = nullptr;
    if (!clip.rhythmMarkers.empty())
    {
        if (clip.rhythmRenderCache != nullptr
            && clip.rhythmRenderCache->matches(clip))
        {
            rhythmRender = clip.rhythmRenderCache.get();
        }
        else if (ensureRhythmRender)
        {
            synchronousRhythmRender = RhythmRender::render(clip, shouldCancel);
            if (synchronousRhythmRender != nullptr
                && synchronousRhythmRender->matches(clip))
                rhythmRender = synchronousRhythmRender.get();
        }
    }

    const auto* sourceBuffer = rhythmRender != nullptr
        ? &rhythmRender->buffer : clip.buffer.get();
    const int sourceChannels = sourceBuffer->getNumChannels();
    const int sourceSamples = sourceBuffer->getNumSamples();
    const int destinationChannels = destination.getNumChannels();
    if (sourceChannels <= 0 || sourceSamples <= 0 || destinationChannels <= 0)
        return;

    const double sourceRate = rhythmRender != nullptr
        ? static_cast<double>(rhythmRender->sampleRate)
        : clip.sampleRate > 0 ? static_cast<double>(clip.sampleRate)
                              : timelineSampleRate;
    const double sourceOffset = rhythmRender != nullptr ? 0.0 : clip.offset;
    const int64_t clipStart = static_cast<int64_t>(std::llround(clip.startTime * timelineSampleRate));
    const int64_t clipEnd = static_cast<int64_t>(std::llround((clip.startTime + clip.duration)
                                                              * timelineSampleRate));
    const int64_t timelineEnd = timelineStartSample + destination.getNumSamples();
    const int64_t overlapStart = std::max(timelineStartSample, clipStart);
    const int64_t overlapEnd = std::min(timelineEnd, clipEnd);
    if (overlapEnd <= overlapStart)
        return;

    const int destinationOffset = static_cast<int>(overlapStart - timelineStartSample);
    const int samplesToMix = static_cast<int>(overlapEnd - overlapStart);
    const double firstTargetTime = static_cast<double>(overlapStart - clipStart)
        / timelineSampleRate;
    const double targetStep = 1.0 / timelineSampleRate;

    for (int i = 0; i < samplesToMix; ++i)
    {
        const double targetTime = firstTargetTime + static_cast<double>(i) * targetStep;
        // Until the asynchronous render is ready, play the unwarped original.
        // This keeps the audio thread free of Signalsmith work and avoids the
        // old resampling path that changed note pitch while dragging.
        const double sourcePosition = (sourceOffset + targetTime) * sourceRate;
        if (sourcePosition < 0.0 || sourcePosition >= sourceSamples)
            continue;

        const int index0 = static_cast<int>(std::floor(sourcePosition));
        const int index1 = std::min(index0 + 1, sourceSamples - 1);
        const float fraction = static_cast<float>(sourcePosition - index0);
        const float envelope = ClipRenderMath::getFadeGain(
            targetTime,
            clip.duration,
            clip.fadeInSeconds,
            clip.fadeOutSeconds);

        auto readInterpolated = [&](int channel)
        {
            const float* source = sourceBuffer->getReadPointer(channel);
            return source[index0] + (source[index1] - source[index0]) * fraction;
        };

        if (destinationChannels == 1 && sourceChannels > 1)
        {
            destination.getWritePointer(0)[destinationOffset + i]
                += (readInterpolated(0) + readInterpolated(1))
                    * 0.5f * gain * envelope;
        }
        else
        {
            for (int ch = 0; ch < destinationChannels; ++ch)
            {
                const int sourceChannel = sourceChannels == 1 ? 0 : std::min(ch, sourceChannels - 1);
                destination.getWritePointer(ch)[destinationOffset + i]
                    += readInterpolated(sourceChannel) * gain * envelope;
            }
        }
    }
}

double AudioEngine::getTracksDuration(const std::vector<TrackData>& tracksToMeasure)
{
    double duration = 0.0;
    for (const auto& track : tracksToMeasure)
        duration = std::max(duration, track.getDuration());
    return duration;
}

std::vector<float> AudioEngine::getRecordingBufferSnapshot() const
{
    const juce::SpinLock::ScopedLockType lock(recordingPreviewLock);
    std::vector<float> snapshot(recordingPreview.begin(),
                                recordingPreview.begin()
                                    + static_cast<std::ptrdiff_t>(recordingPreviewSize));
    if (recordingPreviewBlocksAccumulated > 0 && snapshot.size() < recordingPreviewCapacity)
        snapshot.push_back(recordingPreviewAccumulator);
    return snapshot;
}

//==============================================================================
// Notifications and dirty state

void AudioEngine::postStatusMessage(const juce::String& message, bool isError)
{
    if (onStatusMessage)
        onStatusMessage(message, isError);
}

void AudioEngine::queueMissingInputWarning()
{
    if (!missingInputWarningSent.exchange(true))
    {
        pendingMissingInputWarning.store(true);
        triggerAsyncUpdate();
    }
}

void AudioEngine::appendRecordingPreviewPoint(float peak) noexcept
{
    const juce::SpinLock::ScopedTryLockType lock(recordingPreviewLock);
    if (!lock.isLocked())
        return;

    recordingPreviewAccumulator = std::max(recordingPreviewAccumulator, peak);
    ++recordingPreviewBlocksAccumulated;
    if (recordingPreviewBlocksAccumulated < recordingPreviewBlocksPerPoint)
        return;

    if (recordingPreviewSize >= recordingPreviewCapacity)
    {
        // Collapse adjacent peaks so the fixed-size preview always represents
        // the whole take rather than only its most recent seconds.
        for (size_t i = 0; i < recordingPreviewCapacity / 2; ++i)
            recordingPreview[i] = std::max(recordingPreview[i * 2],
                                           recordingPreview[i * 2 + 1]);
        recordingPreviewSize = recordingPreviewCapacity / 2;
        recordingPreviewBlocksPerPoint *= 2;

        // The current accumulator covers the old interval. Keep it and finish
        // the now-doubled interval on subsequent callbacks.
        return;
    }

    recordingPreview[recordingPreviewSize++] = recordingPreviewAccumulator;
    recordingPreviewBlocksAccumulated = 0;
    recordingPreviewAccumulator = 0.0f;
}

void AudioEngine::markDirty()
{
    contentRevision.fetch_add(1);
    setDirtyState(true);
}

void AudioEngine::markClean()
{
    setDirtyState(false);
}

void AudioEngine::setDirtyState(bool shouldBeDirty)
{
    if (dirty.exchange(shouldBeDirty) == shouldBeDirty)
        return;

    if (auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
        messageManager != nullptr && messageManager->isThisTheMessageThread())
    {
        if (onDirtyChanged) onDirtyChanged(shouldBeDirty);
        return;
    }

    pendingDirtyValue.store(shouldBeDirty);
    dirtyNotificationPending.store(true);
    triggerAsyncUpdate();
}

void AudioEngine::applyCompletedImports()
{
    // Applying a batch briefly locks the track list and creates an undo
    // snapshot. Defer that work until transport is stopped so the audio thread
    // never has to skip a playback/recording block for an import result.
    if (playbackState.load() != PlaybackState::Stopped)
        return;

    std::vector<std::unique_ptr<ImportBatchResult>> ready;
    {
        const juce::ScopedLock lock(completedImportsLock);
        ready.swap(completedImports);
    }

    for (auto& result : ready)
    {
        if (result == nullptr || result->generation != projectGeneration.load())
            continue;

        const int loadedCount = static_cast<int>(result->importedItems.size());
        const int failedCount = std::max(0, result->requestedCount - loadedCount);

        if (loadedCount > 0)
        {
            beginEdit();
            {
                const juce::ScopedLock lock(tracksLock);
                for (auto& item : result->importedItems)
                {
                    item.clip.id = makeClipId();

                    TrackData track;
                    track.id = makeTrackId();
                    track.name = item.trackName;
                    track.color = getTrackColor(nextTrackColorIndex++);
                    track.clips.push_back(std::move(item.clip));
                    tracks.push_back(std::move(track));
                }
                rebuildTrackPlaybackSlotsLocked();
            }
            endEdit();
        }

        if (failedCount == 0)
        {
            postStatusMessage(juce::String(loadedCount)
                                  + utf8(u8"個の音声ファイルを読み込みました。"),
                              false);
        }
        else
        {
            const auto failedNames = result->failedFiles.joinIntoString(", ");
            if (loadedCount > 0)
            {
                auto message = juce::String(loadedCount)
                               + utf8(u8"個を読み込み、")
                               + juce::String(failedCount)
                               + utf8(u8"個は読み込めませんでした。");
                if (failedNames.isNotEmpty())
                    message += " " + failedNames;
                postStatusMessage(message, true);
            }
            else
            {
                auto message = utf8(u8"音声ファイルを読み込めませんでした。");
                if (failedNames.isNotEmpty())
                    message += " " + failedNames;
                postStatusMessage(message, true);
            }
        }
    }
}

void AudioEngine::handleAsyncUpdate()
{
    applyCompletedImports();

    if (pluginLatencyRefreshPending.exchange(false,
                                             std::memory_order_acq_rel))
        refreshTrackPluginLatencies();

    if (pendingMissingInputWarning.exchange(false))
        postStatusMessage(utf8(u8"録音入力が見つかりません。オーディオ設定の入力機器を確認してください。"), true);

    if (dirtyNotificationPending.exchange(false) && onDirtyChanged)
        onDirtyChanged(pendingDirtyValue.load());

    if (onStateChanged) onStateChanged();
}
