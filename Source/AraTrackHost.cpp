#include "AraTrackHost.h"
#include "AraTimelineAudio.h"
#include "PitchNetArchiveFilter.h"

#define JUCE_VST3HEADERS_INCLUDE_HEADERS_ONLY 1
#include <juce_audio_processors_headless/format_types/juce_VST3Headers.h>
#undef JUCE_VST3HEADERS_INCLUDE_HEADERS_ONLY

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <vector>

namespace
{
struct ClipSourceRange
{
    int64_t startSample = 0;
    int64_t sampleCount = 0;
    int64_t fullSampleCount = 0;

    bool usesWholeBuffer() const noexcept
    {
        return startSample == 0 && sampleCount == fullSampleCount;
    }
};

ClipSourceRange getClipSourceRange(const AudioClip& clip) noexcept
{
    ClipSourceRange range;
    if (clip.buffer == nullptr || clip.sampleRate <= 0)
        return range;

    range.fullSampleCount = clip.buffer->getNumSamples();
    range.startSample = juce::jlimit<int64_t>(
        0,
        range.fullSampleCount,
        static_cast<int64_t>(std::llround(std::max(0.0, clip.offset)
                                          * clip.sampleRate)));
    const auto requestedSamples = static_cast<int64_t>(
        std::llround(std::max(0.0, clip.duration) * clip.sampleRate));
    range.sampleCount = juce::jlimit<int64_t>(
        0,
        range.fullSampleCount - range.startSample,
        requestedSamples);
    return range;
}

juce::String makeAudioSourcePersistentId(const juce::String& trackId,
                                         const AudioClip& clip)
{
    return trackId + ":" + clip.id;
}

juce::String makeV2AudioSourcePersistentId(const juce::String& trackId,
                                           const AudioClip& clip)
{
    const auto range = getClipSourceRange(clip);
    auto id = trackId + ":" + clip.id;
    if (!range.usesWholeBuffer())
    {
        constexpr double idTicksPerSecond = 1000000000.0;
        const auto offsetTicks = static_cast<int64_t>(std::llround(
            std::max(0.0, clip.offset) * idTicksPerSecond));
        const auto durationTicks = static_cast<int64_t>(std::llround(
            std::max(0.0, clip.duration) * idTicksPerSecond));
        id += ":range:" + juce::String(offsetTicks)
              + ":" + juce::String(durationTicks);
    }
    return id;
}

juce::String makeModificationPersistentId(const juce::String& trackId,
                                          const AudioClip& clip)
{
    return makeAudioSourcePersistentId(trackId, clip) + ":modification";
}

juce::String makeTimelineAudioSourcePersistentId(const juce::String& trackId)
{
    return trackId + ":timeline";
}

juce::String makeTimelineModificationPersistentId(const juce::String& trackId)
{
    return makeTimelineAudioSourcePersistentId(trackId) + ":modification";
}

using AraTimelineAudio::isUsableClip;

class ReadableAudioSource
{
public:
    using Converter = juce::ARAHostModel::ConversionFunctions<
        ReadableAudioSource*, ARA::ARAAudioSourceHostRef>;

    virtual ~ReadableAudioSource() = default;
    virtual bool readFloat(float* const* destination,
                           ARA::ARASamplePosition startSample,
                           ARA::ARASampleCount requestedSamples) const noexcept = 0;
    virtual bool readDouble(double* const* destination,
                            ARA::ARASamplePosition startSample,
                            ARA::ARASampleCount requestedSamples) const noexcept = 0;
};

PitchNetArchiveFilter::Result filterPitchNetArchive(
    juce::MemoryBlock& archive,
    const juce::String& archiveId,
    const TrackData& track)
{
    return PitchNetArchiveFilter::filterInPlace(
        archive, archiveId,
        PitchNetArchiveFilter::makeModificationRanges(track));
}

class MemoryAudioSource final : public ReadableAudioSource
{
public:
    MemoryAudioSource(ARA::Host::DocumentController& documentController,
                      const juce::String& trackId,
                      const AudioClip& clip)
        : name(clip.name.isNotEmpty() ? clip.name : clip.id),
          persistentId(makeAudioSourcePersistentId(trackId, clip)),
          buffer(clip.buffer),
          sampleRate(std::max(1, clip.sampleRate)),
          sourceRange(getClipSourceRange(clip))
    {
        const auto properties = makeProperties();
        sourceRef = documentController.createAudioSource(
                                                         ReadableAudioSource::Converter::toHostRef(this),
                                                         &properties);
        if (sourceRef != nullptr)
            documentController.enableAudioSourceSamplesAccess(sourceRef, true);
    }

    template <typename Sample>
    bool read(Sample* const* destination,
              ARA::ARASamplePosition startSample,
              ARA::ARASampleCount requestedSamples) const noexcept
    {
        if (buffer == nullptr || destination == nullptr || requestedSamples < 0)
            return false;

        const auto totalSamples = sourceRange.fullSampleCount;
        const auto requestStart = static_cast<int64_t>(startSample);
        const auto requestLength = static_cast<int64_t>(requestedSamples);
        if (requestLength > static_cast<int64_t>(std::numeric_limits<int>::max()))
            return false;

        const int length = static_cast<int>(requestLength);
        const int channels = buffer->getNumChannels();
        for (int channel = 0; channel < channels; ++channel)
        {
            if (destination[channel] == nullptr)
                return false;
            std::fill_n(destination[channel], length, Sample{});
        }

        const auto sourceStart = std::max<int64_t>(
            sourceRange.startSample,
            std::max<int64_t>(0, requestStart));
        const auto sourceEnd = std::min<int64_t>(
            sourceRange.startSample + sourceRange.sampleCount,
            std::min<int64_t>(totalSamples, requestStart + requestLength));
        if (sourceEnd <= sourceStart)
            return true;

        const int destinationOffset = static_cast<int>(sourceStart - requestStart);
        const int samplesToCopy = static_cast<int>(sourceEnd - sourceStart);
        for (int channel = 0; channel < channels; ++channel)
        {
            const float* input = buffer->getReadPointer(
                channel,
                static_cast<int>(sourceStart));
            Sample* output = destination[channel] + destinationOffset;
            for (int sample = 0; sample < samplesToCopy; ++sample)
                output[sample] = static_cast<Sample>(input[sample]);
        }
        return true;
    }

    bool readFloat(float* const* destination,
                   ARA::ARASamplePosition startSample,
                   ARA::ARASampleCount requestedSamples) const noexcept override
    {
        return read(destination, startSample, requestedSamples);
    }

    bool readDouble(double* const* destination,
                    ARA::ARASamplePosition startSample,
                    ARA::ARASampleCount requestedSamples) const noexcept override
    {
        return read(destination, startSample, requestedSamples);
    }

    ARA::ARAAudioSourceRef getPluginRef() const noexcept { return sourceRef; }

    bool updateVisibleRange(ARA::Host::DocumentController& documentController,
                            const AudioClip& clip)
    {
        const auto nextRange = getClipSourceRange(clip);
        if (nextRange.startSample == sourceRange.startSample
            && nextRange.sampleCount == sourceRange.sampleCount)
            return false;

        const auto previousRange = sourceRange;
        if (sourceRef != nullptr)
            documentController.enableAudioSourceSamplesAccess(sourceRef, false);
        sourceRange = nextRange;

        // Only the parts whose audible/silent state changed are invalidated.
        // Pitch edits inside the still-visible overlap therefore remain intact.
        const std::array<int64_t, 4> boundaries {
            previousRange.startSample,
            previousRange.startSample + previousRange.sampleCount,
            nextRange.startSample,
            nextRange.startSample + nextRange.sampleCount
        };
        auto sortedBoundaries = boundaries;
        std::sort(sortedBoundaries.begin(), sortedBoundaries.end());
        for (size_t i = 1; i < sortedBoundaries.size(); ++i)
        {
            const auto segmentStart = sortedBoundaries[i - 1];
            const auto segmentEnd = sortedBoundaries[i];
            if (segmentEnd <= segmentStart)
                continue;
            const auto probe = segmentStart;
            const bool wasVisible = probe >= previousRange.startSample
                && probe < previousRange.startSample + previousRange.sampleCount;
            const bool isVisible = probe >= nextRange.startSample
                && probe < nextRange.startSample + nextRange.sampleCount;
            if (wasVisible == isVisible)
                continue;
            notifyContentChanged(documentController, segmentStart, segmentEnd);
        }
        if (sourceRef != nullptr)
            documentController.enableAudioSourceSamplesAccess(sourceRef, true);
        return true;
    }

    void notifyHiddenRangesChanged(
        ARA::Host::DocumentController& documentController) const
    {
        notifyContentChanged(documentController, 0, sourceRange.startSample);
        notifyContentChanged(documentController,
                             sourceRange.startSample + sourceRange.sampleCount,
                             sourceRange.fullSampleCount);
    }

    void destroy(ARA::Host::DocumentController& documentController) noexcept
    {
        if (sourceRef == nullptr)
            return;
        documentController.enableAudioSourceSamplesAccess(sourceRef, false);
        documentController.destroyAudioSource(sourceRef);
        sourceRef = nullptr;
    }

private:
    ARA::ARAAudioSourceProperties makeProperties() const
    {
        auto properties = juce::ARAHostModel::AudioSource::getEmptyProperties();
        properties.name = name.toRawUTF8();
        properties.persistentID = persistentId.toRawUTF8();
        properties.sampleCount = sourceRange.fullSampleCount;
        properties.sampleRate = sampleRate;
        properties.channelCount = buffer != nullptr ? buffer->getNumChannels() : 0;
        properties.merits64BitSamples = false;
        return properties;
    }

    void notifyContentChanged(ARA::Host::DocumentController& documentController,
                              int64_t startSample,
                              int64_t endSample) const
    {
        startSample = juce::jlimit<int64_t>(0, sourceRange.fullSampleCount,
                                             startSample);
        endSample = juce::jlimit<int64_t>(0, sourceRange.fullSampleCount,
                                           endSample);
        if (sourceRef == nullptr || endSample <= startSample || sampleRate <= 0)
            return;

        const ARA::ARAContentTimeRange range {
            static_cast<double>(startSample) / sampleRate,
            static_cast<double>(endSample - startSample) / sampleRate
        };
        documentController.updateAudioSourceContent(
            sourceRef,
            &range,
            ARA::ContentUpdateScopes::everythingIsAffected());
    }

    juce::String name;
    juce::String persistentId;
    std::shared_ptr<juce::AudioBuffer<float>> buffer;
    int sampleRate = 44100;
    ClipSourceRange sourceRange;
    ARA::ARAAudioSourceRef sourceRef = nullptr;
};

class AudioAccessController final : public ARA::Host::AudioAccessControllerInterface
{
public:
    ARA::ARAAudioReaderHostRef createAudioReaderForSource(
        ARA::ARAAudioSourceHostRef audioSourceHostRef,
        bool use64BitSamples) noexcept override
    {
        auto reader = std::make_unique<Reader>(audioSourceHostRef, use64BitSamples);
        auto* readerPointer = reader.get();
        const auto hostRef = Converter::toHostRef(readerPointer);
        const std::lock_guard<std::mutex> lock(readersMutex);
        readers.emplace(readerPointer, std::move(reader));
        return hostRef;
    }

    bool readAudioSamples(ARA::ARAAudioReaderHostRef readerRef,
                          ARA::ARASamplePosition samplePosition,
                          ARA::ARASampleCount samplesPerChannel,
                          void* const* buffers) noexcept override
    {
        auto* reader = Converter::fromHostRef(readerRef);
        if (reader == nullptr)
            return false;
        auto* source =
            ReadableAudioSource::Converter::fromHostRef(reader->sourceHostRef);
        if (source == nullptr)
            return false;
        if (reader->use64Bit)
            return source->readDouble(reinterpret_cast<double* const*>(buffers),
                                      samplePosition,
                                      samplesPerChannel);
        return source->readFloat(reinterpret_cast<float* const*>(buffers),
                                 samplePosition,
                                 samplesPerChannel);
    }

    void destroyAudioReader(ARA::ARAAudioReaderHostRef readerRef) noexcept override
    {
        const std::lock_guard<std::mutex> lock(readersMutex);
        readers.erase(Converter::fromHostRef(readerRef));
    }

private:
    struct Reader
    {
        Reader(ARA::ARAAudioSourceHostRef source, bool useDoublePrecision)
            : sourceHostRef(source), use64Bit(useDoublePrecision)
        {
        }

        ARA::ARAAudioSourceHostRef sourceHostRef;
        bool use64Bit = false;
    };

    using Converter = juce::ARAHostModel::ConversionFunctions<Reader*,
                                                               ARA::ARAAudioReaderHostRef>;
    std::mutex readersMutex;
    std::map<Reader*, std::unique_ptr<Reader>> readers;
};

class ArchivingController final : public ARA::Host::ArchivingControllerInterface
{
public:
    using ReaderConverter = juce::ARAHostModel::ConversionFunctions<juce::MemoryBlock*,
                                                                     ARA::ARAArchiveReaderHostRef>;
    using WriterConverter = juce::ARAHostModel::ConversionFunctions<juce::MemoryOutputStream*,
                                                                     ARA::ARAArchiveWriterHostRef>;

    explicit ArchivingController(juce::String documentArchiveIdToUse)
        : documentArchiveId(std::move(documentArchiveIdToUse))
    {
    }

    void setDocumentArchiveId(juce::String archiveId)
    {
        documentArchiveId = std::move(archiveId);
    }

    ARA::ARASize getArchiveSize(ARA::ARAArchiveReaderHostRef readerRef) noexcept override
    {
        return static_cast<ARA::ARASize>(ReaderConverter::fromHostRef(readerRef)->getSize());
    }

    bool readBytesFromArchive(ARA::ARAArchiveReaderHostRef readerRef,
                              ARA::ARASize position,
                              ARA::ARASize length,
                              ARA::ARAByte* buffer) noexcept override
    {
        auto* reader = ReaderConverter::fromHostRef(readerRef);
        if (reader == nullptr || position + length > reader->getSize())
            return false;
        std::memcpy(buffer,
                    juce::addBytesToPointer(reader->getData(), position),
                    length);
        return true;
    }

    bool writeBytesToArchive(ARA::ARAArchiveWriterHostRef writerRef,
                             ARA::ARASize position,
                             ARA::ARASize length,
                             const ARA::ARAByte* buffer) noexcept override
    {
        auto* writer = WriterConverter::fromHostRef(writerRef);
        return writer != nullptr
            && writer->setPosition(static_cast<int64_t>(position))
            && writer->write(buffer, length);
    }

    void notifyDocumentArchivingProgress(float) noexcept override {}
    void notifyDocumentUnarchivingProgress(float) noexcept override {}

    ARA::ARAPersistentID getDocumentArchiveID(ARA::ARAArchiveReaderHostRef) noexcept override
    {
        return documentArchiveId.isNotEmpty() ? documentArchiveId.toRawUTF8() : nullptr;
    }

private:
    juce::String documentArchiveId;
};

class ModelUpdateController final : public ARA::Host::ModelUpdateControllerInterface
{
public:
    explicit ModelUpdateController(std::function<void()> documentChangedToUse)
        : callbackState(std::make_shared<CallbackState>(
              std::move(documentChangedToUse)))
    {
    }

    void notifyAudioSourceAnalysisProgress(ARA::ARAAudioSourceHostRef,
                                           ARA::ARAAnalysisProgressState,
                                           float) noexcept override
    {
    }

    void notifyAudioSourceContentChanged(ARA::ARAAudioSourceHostRef,
                                         const ARA::ARAContentTimeRange*,
                                         ARA::ContentUpdateScopes) noexcept override
    {
        callDocumentChanged();
    }

    void notifyAudioModificationContentChanged(ARA::ARAAudioModificationHostRef,
                                                const ARA::ARAContentTimeRange*,
                                                ARA::ContentUpdateScopes) noexcept override
    {
        callDocumentChanged();
    }

    void notifyPlaybackRegionContentChanged(ARA::ARAPlaybackRegionHostRef,
                                            const ARA::ARAContentTimeRange*,
                                            ARA::ContentUpdateScopes) noexcept override
    {
        callDocumentChanged();
    }

private:
    struct CallbackState
    {
        explicit CallbackState(std::function<void()> callbackToUse)
            : callback(std::move(callbackToUse))
        {
        }

        std::function<void()> callback;
        std::atomic<bool> pending { false };
    };

    void callDocumentChanged() const
    {
        const auto state = callbackState;
        if (state == nullptr || state->pending.exchange(true))
            return;

        juce::MessageManager::callAsync(
            [state]
            {
                state->pending.store(false);
                if (state->callback)
                    state->callback();
            });
    }

    std::shared_ptr<CallbackState> callbackState;
};

class PlaybackController final : public ARA::Host::PlaybackControllerInterface
{
public:
    explicit PlaybackController(AraTrackHost::TransportCallbacks callbacksToUse)
        : callbacks(std::move(callbacksToUse))
    {
    }

    void requestStartPlayback() noexcept override
    {
        callOnMessageThread(callbacks.startPlayback);
    }

    void requestStopPlayback() noexcept override
    {
        callOnMessageThread(callbacks.stopPlayback);
    }

    void requestSetPlaybackPosition(ARA::ARATimePosition position) noexcept override
    {
        const auto callback = callbacks.setPlaybackPosition;
        const auto seconds = std::max(0.0, static_cast<double>(position));
        juce::MessageManager::callAsync(
            [callback, seconds]
            {
                if (callback)
                    callback(seconds);
            });
    }

    void requestSetCycleRange(ARA::ARATimePosition, ARA::ARATimeDuration) noexcept override {}
    void requestEnableCycle(bool) noexcept override {}

private:
    static void callOnMessageThread(const std::function<void()>& callback)
    {
        juce::MessageManager::callAsync(
            [callback]
            {
                if (callback)
                    callback();
            });
    }

    AraTrackHost::TransportCallbacks callbacks;
};

class MusicalContext final
{
public:
    using Converter = juce::ARAHostModel::ConversionFunctions<MusicalContext*,
                                                               ARA::ARAMusicalContextHostRef>;

    MusicalContext(ARA::Host::DocumentController& documentController,
                   juce::String trackNameToUse)
        : trackName(std::move(trackNameToUse))
    {
        const auto properties = makeProperties();
        contextRef = documentController.createMusicalContext(Converter::toHostRef(this),
                                                             &properties);
    }

    ARA::ARAMusicalContextRef getPluginRef() const noexcept { return contextRef; }

    void destroy(ARA::Host::DocumentController& documentController) noexcept
    {
        if (contextRef == nullptr)
            return;
        documentController.destroyMusicalContext(contextRef);
        contextRef = nullptr;
    }

private:
    ARA::ARAMusicalContextProperties makeProperties() const
    {
        auto properties = juce::ARAHostModel::MusicalContext::getEmptyProperties();
        properties.name = trackName.toRawUTF8();
        properties.orderIndex = 0;
        properties.color = nullptr;
        return properties;
    }

    juce::String trackName;
    ARA::ARAMusicalContextRef contextRef = nullptr;
};

class RegionSequence final
{
public:
    using Converter = juce::ARAHostModel::ConversionFunctions<RegionSequence*,
                                                               ARA::ARARegionSequenceHostRef>;

    RegionSequence(ARA::Host::DocumentController& documentController,
                   MusicalContext& musicalContextToUse,
                   juce::String trackNameToUse)
        : musicalContext(musicalContextToUse),
          trackName(std::move(trackNameToUse))
    {
        const auto properties = makeProperties();
        sequenceRef = documentController.createRegionSequence(Converter::toHostRef(this),
                                                              &properties);
    }

    ARA::ARARegionSequenceRef getPluginRef() const noexcept { return sequenceRef; }
    MusicalContext& getMusicalContext() const noexcept { return musicalContext; }

    void destroy(ARA::Host::DocumentController& documentController) noexcept
    {
        if (sequenceRef == nullptr)
            return;
        documentController.destroyRegionSequence(sequenceRef);
        sequenceRef = nullptr;
    }

private:
    ARA::ARARegionSequenceProperties makeProperties() const
    {
        auto properties = juce::ARAHostModel::RegionSequence::getEmptyProperties();
        properties.name = trackName.toRawUTF8();
        properties.orderIndex = 0;
        properties.musicalContextRef = musicalContext.getPluginRef();
        properties.color = nullptr;
        return properties;
    }

    MusicalContext& musicalContext;
    juce::String trackName;
    ARA::ARARegionSequenceRef sequenceRef = nullptr;
};

class TimelineAudioSource final : public ReadableAudioSource
{
public:
    TimelineAudioSource(ARA::Host::DocumentController& documentController,
                        const TrackData& track)
        : name(track.name.isNotEmpty() ? track.name : juce::String("Track")),
          persistentId(makeTimelineAudioSourcePersistentId(track.id)),
          audio(track)
    {
        const auto properties = makeProperties();
        sourceRef = documentController.createAudioSource(
            ReadableAudioSource::Converter::toHostRef(this), &properties);
        if (sourceRef != nullptr)
            documentController.enableAudioSourceSamplesAccess(sourceRef, true);
    }

    ARA::ARAAudioSourceRef getPluginRef() const noexcept { return sourceRef; }
    double getDuration() const noexcept
    {
        return audio.getDuration();
    }

    bool updateTrack(ARA::Host::DocumentController& documentController,
                     const TrackData& track)
    {
        std::vector<std::pair<double, double>> changedRanges;
        const auto addClipRange = [&changedRanges](const AudioClip& clip)
        {
            if (!isUsableClip(clip))
                return;
            const auto start = std::max(0.0, clip.startTime);
            changedRanges.emplace_back(start, start + clip.duration);
        };
        const auto sameClip = [](const AudioClip& left, const AudioClip& right)
        {
            return left.id == right.id
                && left.buffer.get() == right.buffer.get()
                && left.buffer->getNumSamples() == right.buffer->getNumSamples()
                && left.buffer->getNumChannels() == right.buffer->getNumChannels()
                && left.sampleRate == right.sampleRate
                && left.startTime == right.startTime
                && left.offset == right.offset
                && left.duration == right.duration;
        };

        for (const auto& previous : audio.getClips())
        {
            const auto next = std::find_if(track.clips.begin(), track.clips.end(),
                [&previous](const AudioClip& candidate)
                {
                    return candidate.id == previous.id && isUsableClip(candidate);
                });
            if (next == track.clips.end() || !sameClip(previous, *next))
                addClipRange(previous);
        }
        for (const auto& next : track.clips)
        {
            if (!isUsableClip(next))
                continue;
            const auto previous = std::find_if(audio.getClips().begin(),
                                               audio.getClips().end(),
                [&next](const AudioClip& candidate)
                {
                    return candidate.id == next.id;
                });
            if (previous == audio.getClips().end()
                || !sameClip(*previous, next))
                addClipRange(next);
        }
        if (AraTimelineAudio::hasSameRenderedAudio(audio.getClips(), track.clips))
            changedRanges.clear();

        const auto previousSampleRate = audio.getSampleRate();
        const auto previousSampleCount = audio.getSampleCount();
        const auto previousChannelCount = audio.getChannelCount();
        const auto previousName = name;
        const auto nextName =
            track.name.isNotEmpty() ? track.name : juce::String("Track");
        AraTimelineAudio::Reader nextAudio(track);
        if (previousSampleRate != nextAudio.getSampleRate())
        {
            changedRanges.clear();
            changedRanges.emplace_back(
                0.0,
                std::max(static_cast<double>(previousSampleCount)
                             / std::max(1, previousSampleRate),
                         nextAudio.getDuration()));
        }

        const bool audioChanged = !changedRanges.empty();
        const bool propertiesChanged =
            previousSampleRate != nextAudio.getSampleRate()
            || previousSampleCount != nextAudio.getSampleCount()
            || previousChannelCount != nextAudio.getChannelCount()
            || previousName != nextName;
        if (sourceRef != nullptr && (audioChanged || propertiesChanged))
            documentController.enableAudioSourceSamplesAccess(sourceRef, false);

        name = nextName;
        audio = std::move(nextAudio);
        if (sourceRef != nullptr && propertiesChanged)
        {
            const auto properties = makeProperties();
            documentController.updateAudioSourceProperties(sourceRef, &properties);
        }

        if (sourceRef != nullptr && !changedRanges.empty())
        {
            std::sort(changedRanges.begin(), changedRanges.end());
            std::vector<std::pair<double, double>> merged;
            for (const auto& range : changedRanges)
            {
                if (range.second <= range.first)
                    continue;
                if (merged.empty() || range.first > merged.back().second)
                    merged.push_back(range);
                else
                    merged.back().second =
                        std::max(merged.back().second, range.second);
            }
            for (const auto& range : merged)
            {
                const auto clampedEnd = std::min(range.second, getDuration());
                if (clampedEnd <= range.first)
                    continue;
                const ARA::ARAContentTimeRange contentRange {
                    range.first, clampedEnd - range.first
                };
                documentController.updateAudioSourceContent(
                    sourceRef,
                    &contentRange,
                    ARA::ContentUpdateScopes::everythingIsAffected());
            }
        }
        if (sourceRef != nullptr && (audioChanged || propertiesChanged))
            documentController.enableAudioSourceSamplesAccess(sourceRef, true);
        return audioChanged;
    }

    bool readFloat(float* const* destination,
                   ARA::ARASamplePosition startSample,
                   ARA::ARASampleCount requestedSamples) const noexcept override
    {
        return audio.read(destination,
                          static_cast<int64_t>(startSample),
                          static_cast<int64_t>(requestedSamples));
    }

    bool readDouble(double* const* destination,
                    ARA::ARASamplePosition startSample,
                    ARA::ARASampleCount requestedSamples) const noexcept override
    {
        return audio.read(destination,
                          static_cast<int64_t>(startSample),
                          static_cast<int64_t>(requestedSamples));
    }

    void destroy(ARA::Host::DocumentController& documentController) noexcept
    {
        if (sourceRef == nullptr)
            return;
        documentController.enableAudioSourceSamplesAccess(sourceRef, false);
        documentController.destroyAudioSource(sourceRef);
        sourceRef = nullptr;
    }

private:
    ARA::ARAAudioSourceProperties makeProperties() const
    {
        auto properties = juce::ARAHostModel::AudioSource::getEmptyProperties();
        properties.name = name.toRawUTF8();
        properties.persistentID = persistentId.toRawUTF8();
        properties.sampleCount = audio.getSampleCount();
        properties.sampleRate = audio.getSampleRate();
        properties.channelCount = audio.getChannelCount();
        properties.merits64BitSamples = false;
        return properties;
    }

    juce::String name;
    juce::String persistentId;
    AraTimelineAudio::Reader audio;
    ARA::ARAAudioSourceRef sourceRef = nullptr;
};

class TimelineContext final
{
public:
    using ModificationConverter =
        juce::ARAHostModel::ConversionFunctions<TimelineContext*,
                                                 ARA::ARAAudioModificationHostRef>;
    using RegionConverter =
        juce::ARAHostModel::ConversionFunctions<TimelineContext*,
                                                 ARA::ARAPlaybackRegionHostRef>;

    TimelineContext(ARA::Host::DocumentController& documentController,
                    RegionSequence& regionSequenceToUse,
                    const TrackData& track)
        : regionSequence(regionSequenceToUse),
          modificationId(makeTimelineModificationPersistentId(track.id)),
          audioSource(documentController, track)
    {
        const auto modificationProperties = makeModificationProperties();
        if (audioSource.getPluginRef() != nullptr)
            modificationRef = documentController.createAudioModification(
                audioSource.getPluginRef(),
                ModificationConverter::toHostRef(this),
                &modificationProperties);
        const auto playbackProperties = makePlaybackRegionProperties();
        if (modificationRef != nullptr)
            playbackRegionRef = documentController.createPlaybackRegion(
                modificationRef,
                RegionConverter::toHostRef(this),
                &playbackProperties);
    }

    bool update(ARA::Host::DocumentController& documentController,
                const TrackData& track)
    {
        const bool audioChanged = audioSource.updateTrack(documentController,
                                                          track);
        if (playbackRegionRef != nullptr)
        {
            const auto properties = makePlaybackRegionProperties();
            documentController.updatePlaybackRegionProperties(playbackRegionRef,
                                                               &properties);
        }
        return audioChanged;
    }

    bool recreatePlaybackRegion(
        ARA::Host::DocumentController& documentController)
    {
        if (playbackRegionRef != nullptr)
        {
            documentController.destroyPlaybackRegion(playbackRegionRef);
            playbackRegionRef = nullptr;
        }
        if (modificationRef == nullptr)
            return false;
        const auto properties = makePlaybackRegionProperties();
        playbackRegionRef = documentController.createPlaybackRegion(
            modificationRef,
            RegionConverter::toHostRef(this),
            &properties);
        return playbackRegionRef != nullptr;
    }

    bool isValid() const noexcept
    {
        return audioSource.getPluginRef() != nullptr
            && modificationRef != nullptr
            && playbackRegionRef != nullptr;
    }
    ARA::ARAPlaybackRegionRef getPlaybackRegionRef() const noexcept
    {
        return playbackRegionRef;
    }
    ARA::ARAAudioSourceRef getAudioSourceRef() const noexcept
    {
        return audioSource.getPluginRef();
    }

    void destroy(ARA::Host::DocumentController& documentController) noexcept
    {
        if (playbackRegionRef != nullptr)
        {
            documentController.destroyPlaybackRegion(playbackRegionRef);
            playbackRegionRef = nullptr;
        }
        if (modificationRef != nullptr)
        {
            documentController.destroyAudioModification(modificationRef);
            modificationRef = nullptr;
        }
        audioSource.destroy(documentController);
    }

private:
    ARA::ARAAudioModificationProperties makeModificationProperties() const
    {
        auto properties =
            juce::ARAHostModel::AudioModification::getEmptyProperties();
        properties.persistentID = modificationId.toRawUTF8();
        return properties;
    }

    ARA::ARAPlaybackRegionProperties makePlaybackRegionProperties() const
    {
        auto properties =
            juce::ARAHostModel::PlaybackRegion::getEmptyProperties();
        properties.transformationFlags =
            ARA::kARAPlaybackTransformationNoChanges;
        properties.startInModificationTime = 0.0;
        properties.durationInModificationTime = audioSource.getDuration();
        properties.startInPlaybackTime = 0.0;
        properties.durationInPlaybackTime = audioSource.getDuration();
        properties.musicalContextRef =
            regionSequence.getMusicalContext().getPluginRef();
        properties.regionSequenceRef = regionSequence.getPluginRef();
        properties.name = "Track timeline";
        properties.color = nullptr;
        return properties;
    }

    RegionSequence& regionSequence;
    juce::String modificationId;
    TimelineAudioSource audioSource;
    ARA::ARAAudioModificationRef modificationRef = nullptr;
    ARA::ARAPlaybackRegionRef playbackRegionRef = nullptr;
};

class ClipContext final
{
public:
    using ModificationConverter = juce::ARAHostModel::ConversionFunctions<ClipContext*,
                                                                          ARA::ARAAudioModificationHostRef>;
    using RegionConverter = juce::ARAHostModel::ConversionFunctions<ClipContext*,
                                                                    ARA::ARAPlaybackRegionHostRef>;

    ClipContext(ARA::Host::DocumentController& documentController,
                RegionSequence& regionSequenceToUse,
                const TrackData& track,
                const AudioClip& clip)
        : regionSequence(regionSequenceToUse),
          clipId(clip.id),
          clipName(clip.name.isNotEmpty() ? clip.name : clip.id),
          modificationId(makeModificationPersistentId(track.id, clip)),
          bufferIdentity(clip.buffer.get()),
          bufferSamples(clip.buffer != nullptr ? clip.buffer->getNumSamples() : 0),
          channelCount(clip.buffer != nullptr ? clip.buffer->getNumChannels() : 0),
          sourceSampleRate(clip.sampleRate),
          sourceRange(getClipSourceRange(clip)),
          startInModification(sourceSampleRate > 0
                                ? static_cast<double>(sourceRange.startSample)
                                      / sourceSampleRate
                                : 0.0),
          duration(sourceSampleRate > 0
                       ? static_cast<double>(sourceRange.sampleCount) / sourceSampleRate
                       : 0.0),
          startInPlayback(std::max(0.0, clip.startTime)),
          audioSource(documentController, track.id, clip)
    {
        const auto modificationProperties = makeModificationProperties();
        if (audioSource.getPluginRef() != nullptr)
        {
            modificationRef = documentController.createAudioModification(
                audioSource.getPluginRef(),
                ModificationConverter::toHostRef(this),
                &modificationProperties);
        }
        const auto playbackRegionProperties = makePlaybackRegionProperties();
        if (modificationRef != nullptr)
        {
            playbackRegionRef = documentController.createPlaybackRegion(
                modificationRef,
                RegionConverter::toHostRef(this),
                &playbackRegionProperties);
        }
    }

    const juce::String& getClipId() const noexcept { return clipId; }

    bool matchesAudio(const AudioClip& clip) const noexcept
    {
        return clipId == clip.id
            && bufferIdentity == clip.buffer.get()
            && bufferSamples == (clip.buffer != nullptr ? clip.buffer->getNumSamples() : 0)
            && channelCount == (clip.buffer != nullptr ? clip.buffer->getNumChannels() : 0)
            && sourceSampleRate == clip.sampleRate;
    }

    bool usesRangedAudioSource() const noexcept
    {
        return !sourceRange.usesWholeBuffer();
    }

    bool updatePlaybackProperties(ARA::Host::DocumentController& documentController,
                                  const AudioClip& clip)
    {
        const bool audioRangeChanged = audioSource.updateVisibleRange(documentController,
                                                                      clip);
        sourceRange = getClipSourceRange(clip);
        clipName = clip.name.isNotEmpty() ? clip.name : clip.id;
        startInModification = sourceSampleRate > 0
            ? static_cast<double>(sourceRange.startSample) / sourceSampleRate
            : 0.0;
        duration = sourceSampleRate > 0
            ? static_cast<double>(sourceRange.sampleCount) / sourceSampleRate
            : 0.0;
        startInPlayback = std::max(0.0, clip.startTime);
        if (playbackRegionRef != nullptr)
        {
            const auto properties = makePlaybackRegionProperties();
            documentController.updatePlaybackRegionProperties(playbackRegionRef,
                                                               &properties);
        }
        return audioRangeChanged;
    }

    void notifyHiddenAudioChanged(
        ARA::Host::DocumentController& documentController) const
    {
        audioSource.notifyHiddenRangesChanged(documentController);
    }

    bool recreatePlaybackRegion(
        ARA::Host::DocumentController& documentController)
    {
        if (playbackRegionRef != nullptr)
        {
            documentController.destroyPlaybackRegion(playbackRegionRef);
            playbackRegionRef = nullptr;
        }
        if (modificationRef == nullptr)
            return false;
        const auto properties = makePlaybackRegionProperties();
        playbackRegionRef = documentController.createPlaybackRegion(
            modificationRef,
            RegionConverter::toHostRef(this),
            &properties);
        return playbackRegionRef != nullptr;
    }

    ARA::ARAPlaybackRegionRef getPlaybackRegionRef() const noexcept
    {
        return playbackRegionRef;
    }
    ARA::ARAAudioSourceRef getAudioSourceRef() const noexcept
    {
        return audioSource.getPluginRef();
    }

    bool isValid() const noexcept
    {
        return audioSource.getPluginRef() != nullptr
            && modificationRef != nullptr
            && playbackRegionRef != nullptr;
    }

    void destroy(ARA::Host::DocumentController& documentController) noexcept
    {
        if (playbackRegionRef != nullptr)
        {
            documentController.destroyPlaybackRegion(playbackRegionRef);
            playbackRegionRef = nullptr;
        }
        if (modificationRef != nullptr)
        {
            documentController.destroyAudioModification(modificationRef);
            modificationRef = nullptr;
        }
        audioSource.destroy(documentController);
    }

private:
    ARA::ARAAudioModificationProperties makeModificationProperties() const
    {
        auto properties = juce::ARAHostModel::AudioModification::getEmptyProperties();
        properties.persistentID = modificationId.toRawUTF8();
        return properties;
    }

    ARA::ARAPlaybackRegionProperties makePlaybackRegionProperties() const
    {
        auto properties = juce::ARAHostModel::PlaybackRegion::getEmptyProperties();
        properties.transformationFlags = ARA::kARAPlaybackTransformationNoChanges;
        properties.startInModificationTime = startInModification;
        properties.durationInModificationTime = duration;
        properties.startInPlaybackTime = startInPlayback;
        properties.durationInPlaybackTime = duration;
        properties.musicalContextRef = regionSequence.getMusicalContext().getPluginRef();
        properties.regionSequenceRef = regionSequence.getPluginRef();
        properties.name = clipName.toRawUTF8();
        properties.color = nullptr;
        return properties;
    }

    RegionSequence& regionSequence;
    juce::String clipId;
    juce::String clipName;
    juce::String modificationId;
    const void* bufferIdentity = nullptr;
    int bufferSamples = 0;
    int channelCount = 0;
    int sourceSampleRate = 0;
    ClipSourceRange sourceRange;
    double startInModification = 0.0;
    double duration = 0.0;
    double startInPlayback = 0.0;
    MemoryAudioSource audioSource;
    ARA::ARAAudioModificationRef modificationRef = nullptr;
    ARA::ARAPlaybackRegionRef playbackRegionRef = nullptr;
};

struct ClipSignature
{
    juce::String id;
    const void* bufferIdentity = nullptr;
    int bufferSamples = 0;
    int channels = 0;
    int sampleRate = 0;
    double startTime = 0.0;
    double offset = 0.0;
    double duration = 0.0;

    bool operator==(const ClipSignature& other) const noexcept
    {
        return id == other.id
            && bufferIdentity == other.bufferIdentity
            && bufferSamples == other.bufferSamples
            && channels == other.channels
            && sampleRate == other.sampleRate
            && startTime == other.startTime
            && offset == other.offset
            && duration == other.duration;
    }
};

std::vector<ClipSignature> makeSignatures(const TrackData& track)
{
    std::vector<ClipSignature> result;
    result.reserve(track.clips.size());
    for (const auto& clip : track.clips)
    {
        result.push_back({ clip.id,
                           clip.buffer.get(),
                           clip.buffer != nullptr ? clip.buffer->getNumSamples() : 0,
                           clip.buffer != nullptr ? clip.buffer->getNumChannels() : 0,
                           clip.sampleRate,
                           clip.startTime,
                           clip.offset,
                           clip.duration });
    }
    return result;
}

class TrackContext final
{
public:
    TrackContext(ARA::Host::DocumentController& documentController,
                 const TrackData& track,
                 bool useTimelineToUse)
        : musicalContext(documentController,
                         track.name.isNotEmpty() ? track.name : juce::String("Track")),
          regionSequence(documentController,
                         musicalContext,
                         track.name.isNotEmpty() ? track.name : juce::String("Track")),
          useTimeline(useTimelineToUse)
    {
        if (useTimeline)
        {
            if (std::any_of(track.clips.begin(), track.clips.end(),
                            [](const AudioClip& clip)
                            {
                                return isUsableClip(clip);
                            }))
            {
                timelineContext = std::make_unique<TimelineContext>(
                    documentController, regionSequence, track);
            }
            return;
        }

        clipContexts.reserve(track.clips.size());
        for (const auto& clip : track.clips)
        {
            if (!isUsableClip(clip))
                continue;
            clipContexts.push_back(std::make_unique<ClipContext>(documentController,
                                                                 regionSequence,
                                                                 track,
                                                                 clip));
        }
    }

    bool isValid() const noexcept
    {
        return musicalContext.getPluginRef() != nullptr
            && regionSequence.getPluginRef() != nullptr
            && (!useTimeline || timelineContext == nullptr
                || timelineContext->isValid())
            && std::all_of(clipContexts.begin(), clipContexts.end(),
                [](const std::unique_ptr<ClipContext>& clip)
                {
                    return clip != nullptr && clip->isValid();
                });
    }

    bool update(ARA::Host::DocumentController& documentController,
                ARA::ARAPlaybackRendererRef playbackRendererRef,
                const ARA::ARAPlaybackRendererInterface* playbackRendererInterface,
                ARA::ARAEditorRendererRef editorRendererRef,
                const ARA::ARAEditorRendererInterface* editorRendererInterface,
                const TrackData& track,
                std::vector<ARA::ARAAudioSourceRef>& addedAudioSources)
    {
        addedAudioSources.clear();
        if (playbackRendererRef == nullptr || playbackRendererInterface == nullptr
            || editorRendererRef == nullptr || editorRendererInterface == nullptr)
            return false;

        const bool hasUsableClips =
            std::any_of(track.clips.begin(), track.clips.end(),
                [](const AudioClip& clip)
                {
                    return isUsableClip(clip);
                });
        if (useTimeline)
        {
            if (timelineContext != nullptr && !hasUsableClips)
            {
                if (const auto regionRef =
                        timelineContext->getPlaybackRegionRef();
                    regionRef != nullptr)
                {
                    editorRendererInterface->removePlaybackRegion(
                        editorRendererRef, regionRef);
                    playbackRendererInterface->removePlaybackRegion(
                        playbackRendererRef, regionRef);
                }
                const juce::ARAEditGuard guard(documentController);
                timelineContext->destroy(documentController);
                timelineContext.reset();
                return isValid();
            }

            if (timelineContext == nullptr && hasUsableClips)
            {
                {
                    const juce::ARAEditGuard guard(documentController);
                    timelineContext = std::make_unique<TimelineContext>(
                        documentController, regionSequence, track);
                }
                if (timelineContext == nullptr || !timelineContext->isValid())
                    return false;
                const auto regionRef =
                    timelineContext->getPlaybackRegionRef();
                playbackRendererInterface->addPlaybackRegion(
                    playbackRendererRef, regionRef);
                editorRendererInterface->addPlaybackRegion(
                    editorRendererRef, regionRef);
                addedAudioSources.push_back(
                    timelineContext->getAudioSourceRef());
                return true;
            }

            if (timelineContext != nullptr)
            {
                const juce::ARAEditGuard guard(documentController);
                if (timelineContext->update(documentController, track))
                    addedAudioSources.push_back(
                        timelineContext->getAudioSourceRef());
            }
            return isValid();
        }

        for (size_t i = 0; i < track.clips.size(); ++i)
        {
            if (!::isUsableClip(track.clips[i]))
                continue;
            for (size_t j = i + 1; j < track.clips.size(); ++j)
                if (::isUsableClip(track.clips[j])
                    && track.clips[i].id == track.clips[j].id)
                    return false;
        }

        const auto findClip = [&track](const juce::String& id)
            -> const AudioClip*
        {
            const auto clip = std::find_if(track.clips.begin(), track.clips.end(),
                [&id](const AudioClip& candidate)
                {
                    return ::isUsableClip(candidate) && candidate.id == id;
                });
            return clip != track.clips.end() ? &*clip : nullptr;
        };

        // Renderer references must be removed before their model objects are
        // destroyed. Unchanged clips stay attached, preserving PitchNet edits.
        for (const auto& existing : clipContexts)
        {
            if (existing == nullptr)
                continue;
            const auto* replacement = findClip(existing->getClipId());
            if (replacement != nullptr && existing->matchesAudio(*replacement))
                continue;
            if (const auto regionRef = existing->getPlaybackRegionRef(); regionRef != nullptr)
            {
                editorRendererInterface->removePlaybackRegion(editorRendererRef, regionRef);
                playbackRendererInterface->removePlaybackRegion(playbackRendererRef, regionRef);
            }
        }

        std::vector<std::unique_ptr<ClipContext>> previousContexts;
        previousContexts.swap(clipContexts);
        std::vector<std::unique_ptr<ClipContext>> updatedContexts;
        updatedContexts.reserve(track.clips.size());
        std::vector<ClipContext*> addedContexts;

        {
            const juce::ARAEditGuard guard(documentController);
            for (const auto& clip : track.clips)
            {
                if (!::isUsableClip(clip))
                    continue;

                auto existing = std::find_if(previousContexts.begin(), previousContexts.end(),
                    [&clip](const std::unique_ptr<ClipContext>& candidate)
                    {
                        return candidate != nullptr && candidate->getClipId() == clip.id;
                    });

                if (existing != previousContexts.end()
                    && *existing != nullptr
                    && (*existing)->matchesAudio(clip))
                {
                    if ((*existing)->updatePlaybackProperties(documentController, clip))
                        addedAudioSources.push_back((*existing)->getAudioSourceRef());
                    updatedContexts.push_back(std::move(*existing));
                    continue;
                }

                // Replacing the samples under an existing clip ID must remove
                // the old ARA object before the persistent ID can be reused.
                if (existing != previousContexts.end() && *existing != nullptr)
                {
                    (*existing)->destroy(documentController);
                    existing->reset();
                }

                auto added = std::make_unique<ClipContext>(documentController,
                                                           regionSequence,
                                                           track,
                                                           clip);
                addedContexts.push_back(added.get());
                addedAudioSources.push_back(added->getAudioSourceRef());
                updatedContexts.push_back(std::move(added));
            }

            for (auto& obsolete : previousContexts)
                if (obsolete != nullptr)
                    obsolete->destroy(documentController);
        }

        clipContexts = std::move(updatedContexts);
        for (auto* added : addedContexts)
        {
            if (added == nullptr || added->getPlaybackRegionRef() == nullptr)
                continue;
            playbackRendererInterface->addPlaybackRegion(playbackRendererRef,
                                                         added->getPlaybackRegionRef());
            editorRendererInterface->addPlaybackRegion(editorRendererRef,
                                                       added->getPlaybackRegionRef());
        }
        return isValid();
    }

    void destroy(ARA::Host::DocumentController& documentController) noexcept
    {
        if (timelineContext != nullptr)
        {
            timelineContext->destroy(documentController);
            timelineContext.reset();
        }
        for (auto clip = clipContexts.rbegin(); clip != clipContexts.rend(); ++clip)
            if (*clip != nullptr)
                (*clip)->destroy(documentController);
        clipContexts.clear();
        regionSequence.destroy(documentController);
        musicalContext.destroy(documentController);
    }

    ARA::ARARegionSequenceRef getRegionSequenceRef() const noexcept
    {
        return regionSequence.getPluginRef();
    }

    bool isTimelineMode() const noexcept { return useTimeline; }

    std::vector<ARA::ARAPlaybackRegionRef> getPlaybackRegionRefs() const
    {
        std::vector<ARA::ARAPlaybackRegionRef> result;
        if (timelineContext != nullptr
            && timelineContext->getPlaybackRegionRef() != nullptr)
        {
            result.push_back(timelineContext->getPlaybackRegionRef());
            return result;
        }
        for (const auto& clip : clipContexts)
            if (clip != nullptr && clip->getPlaybackRegionRef() != nullptr)
                result.push_back(clip->getPlaybackRegionRef());
        return result;
    }

    std::vector<ARA::ARAAudioSourceRef> getAudioSourceRefs() const
    {
        std::vector<ARA::ARAAudioSourceRef> result;
        if (timelineContext != nullptr
            && timelineContext->getAudioSourceRef() != nullptr)
        {
            result.push_back(timelineContext->getAudioSourceRef());
            return result;
        }
        for (const auto& clip : clipContexts)
            if (clip != nullptr && clip->getAudioSourceRef() != nullptr)
                result.push_back(clip->getAudioSourceRef());
        return result;
    }

    std::vector<ARA::ARAAudioSourceRef> getRangedAudioSourceRefs() const
    {
        std::vector<ARA::ARAAudioSourceRef> result;
        for (const auto& clip : clipContexts)
        {
            if (clip != nullptr && clip->usesRangedAudioSource()
                && clip->getAudioSourceRef() != nullptr)
                result.push_back(clip->getAudioSourceRef());
        }
        return result;
    }

    std::vector<ARA::ARAAudioSourceRef> getAudioSourceRefsForClipIds(
        const juce::StringArray& clipIds) const
    {
        if (timelineContext != nullptr && !clipIds.isEmpty()
            && timelineContext->getAudioSourceRef() != nullptr)
            return { timelineContext->getAudioSourceRef() };

        std::vector<ARA::ARAAudioSourceRef> result;
        for (const auto& clip : clipContexts)
        {
            if (clip != nullptr && clipIds.contains(clip->getClipId())
                && clip->getAudioSourceRef() != nullptr)
                result.push_back(clip->getAudioSourceRef());
        }
        return result;
    }

    void notifyHiddenAudioChanged(
        ARA::Host::DocumentController& documentController) const
    {
        for (const auto& clip : clipContexts)
            if (clip != nullptr && clip->usesRangedAudioSource())
                clip->notifyHiddenAudioChanged(documentController);
    }

    bool recreatePlaybackRegions(
        ARA::Host::DocumentController& documentController)
    {
        if (timelineContext != nullptr)
            return timelineContext->recreatePlaybackRegion(documentController);
        for (const auto& clip : clipContexts)
            if (clip != nullptr)
                if (!clip->recreatePlaybackRegion(documentController))
                    return false;
        return true;
    }

    std::vector<std::unique_ptr<ClipContext>> clipContexts;

private:
    MusicalContext musicalContext;
    RegionSequence regionSequence;
    const bool useTimeline = false;
    std::unique_ptr<TimelineContext> timelineContext;
};
}

class AraTrackHost::Impl final : private juce::Timer
{
public:
    Impl(juce::AudioPluginInstance& editorInstance,
         juce::AudioPluginInstance* playbackInstance,
         juce::ARAFactoryWrapper factory,
         AraTrackHost::TransportCallbacks transportCallbacks,
         const juce::String& savedArchiveId)
    {
        if (factory.get() == nullptr)
            return;

        const auto* araFactory = factory.get();
        if (araFactory->documentArchiveID != nullptr)
            currentArchiveId = juce::String::fromUTF8(araFactory->documentArchiveID);
        if (araFactory->analyzeableContentTypesCount > 0
            && araFactory->analyzeableContentTypes != nullptr)
        {
            analyzableContentTypes.assign(
                araFactory->analyzeableContentTypes,
                araFactory->analyzeableContentTypes
                    + araFactory->analyzeableContentTypesCount);
        }

        const auto archiveIdForRestore = savedArchiveId.isNotEmpty()
            ? savedArchiveId : currentArchiveId;
        const auto documentChanged = transportCallbacks.documentChanged;
        auto archiveController = std::make_unique<ArchivingController>(archiveIdForRestore);
        archivingController = archiveController.get();
        documentController = juce::ARAHostDocumentController::create(
            std::move(factory),
            "SimpleRec Pro Project",
            std::make_unique<AudioAccessController>(),
            std::move(archiveController),
            nullptr,
            std::make_unique<ModelUpdateController>(documentChanged),
            std::make_unique<PlaybackController>(std::move(transportCallbacks)));
        if (documentController == nullptr)
            return;

        const auto allRoles = ARA::kARAPlaybackRendererRole
                            | ARA::kARAEditorRendererRole
                            | ARA::kARAEditorViewRole;
        const auto editorRoles = ARA::kARAEditorRendererRole
                               | ARA::kARAEditorViewRole;
        const bool useSeparatePlaybackInstance = playbackInstance != nullptr
                                              && playbackInstance != &editorInstance;
        const auto bindInstance = [this, allRoles](juce::AudioPluginInstance& instance,
                                                   auto assignedRoles)
            -> const ARA::ARAPlugInExtensionInstance*
        {
            if (auto* vst3Client = instance.getVST3Client())
            {
                Steinberg::FUnknownPtr<ARA::IPlugInEntryPoint2> entryPoint(
                    vst3Client->getIComponentPtr());
                if (entryPoint)
                {
                    return entryPoint->bindToDocumentControllerWithRoles(
                        documentController->getDocumentController().getRef(),
                        allRoles,
                        assignedRoles);
                }
            }
            return nullptr;
        };

        const auto* editorExtension = bindInstance(
            editorInstance,
            useSeparatePlaybackInstance ? editorRoles : allRoles);
        const auto* playbackExtension = useSeparatePlaybackInstance
            ? bindInstance(*playbackInstance, ARA::kARAPlaybackRendererRole)
            : editorExtension;
        if (editorExtension == nullptr || playbackExtension == nullptr)
        {
            documentController.reset();
            return;
        }

        playbackRendererRef = playbackExtension->playbackRendererRef;
        playbackRendererInterface = playbackExtension->playbackRendererInterface;
        editorRendererRef = editorExtension->editorRendererRef;
        editorRendererInterface = editorExtension->editorRendererInterface;
        editorView = std::make_unique<ARA::Host::EditorView>(editorExtension);
        if (playbackRendererRef == nullptr || playbackRendererInterface == nullptr
            || editorRendererRef == nullptr || editorRendererInterface == nullptr)
        {
            editorView.reset();
            documentController.reset();
            return;
        }
        bindingInitialised = true;
    }

    ~Impl() override
    {
        stopTimer();
        clearContext();
    }

    bool initialiseTrack(const TrackData& track,
                         const juce::MemoryBlock& savedArchive)
    {
        if (!bindingInitialised || initialised)
            return false;
        juce::MemoryBlock filteredArchive;
        const juce::MemoryBlock* archiveToRestore = &savedArchive;
        if (!savedArchive.isEmpty()
            && currentArchiveId == PitchNetArchiveFilter::archiveId)
        {
            filteredArchive = savedArchive;
            const auto filterResult = filterPitchNetArchive(
                filteredArchive, currentArchiveId, track);
            if (filterResult.recognised && filterResult.changed)
                archiveToRestore = &filteredArchive;
        }
        const bool graphCreated = rebuildTrack(track, *archiveToRestore);
        statePreservationFailed = !graphCreated
            || (!savedArchive.isEmpty() && !restoredFromArchive);
        return graphCreated;
    }

    bool setTrack(const TrackData& track)
    {
        if (!initialised || context == nullptr || statePreservationFailed)
            return false;

        const auto previousTrack = trackSnapshot;
        juce::MemoryBlock archive;
        juce::String archiveId;
        if (!captureArchive(archive, archiveId) || archive.isEmpty())
            return false;

        lastKnownGoodArchive = std::move(archive);
        lastKnownGoodArchiveId = archiveId;

        if (archivingController != nullptr)
            archivingController->setDocumentArchiveId(archiveId);

        if (archiveId == PitchNetArchiveFilter::archiveId)
        {
            auto filteredArchive = lastKnownGoodArchive;
            const auto filterResult = filterPitchNetArchive(
                filteredArchive, archiveId, track);
            if (filterResult.recognised && filterResult.changed)
            {
                juce::StringArray sourcesNeedingAnalysis;
                for (const auto& nextClip : track.clips)
                {
                    const auto previousClip = std::find_if(
                        previousTrack.clips.begin(), previousTrack.clips.end(),
                        [&nextClip](const AudioClip& candidate)
                        {
                            return candidate.id == nextClip.id;
                        });
                    if (previousClip == previousTrack.clips.end())
                    {
                        sourcesNeedingAnalysis.addIfNotAlreadyThere(nextClip.id);
                        continue;
                    }

                    const auto previousRange = getClipSourceRange(*previousClip);
                    const auto nextRange = getClipSourceRange(nextClip);
                    const bool samplesChanged = previousClip->buffer.get()
                            != nextClip.buffer.get()
                        || previousClip->sampleRate != nextClip.sampleRate
                        || previousRange.fullSampleCount != nextRange.fullSampleCount;
                    const bool rangeExpanded = nextRange.startSample
                            < previousRange.startSample
                        || nextRange.startSample + nextRange.sampleCount
                            > previousRange.startSample + previousRange.sampleCount;
                    const bool timelinePositionChanged =
                        track.vst3AraHostFormatVersion
                            >= TrackData::timelineVst3AraHostFormatVersion
                        && previousClip->startTime != nextClip.startTime;
                    if (samplesChanged || rangeExpanded || timelinePositionChanged)
                        sourcesNeedingAnalysis.addIfNotAlreadyThere(nextClip.id);
                }

                if (rebuildTrack(track, filteredArchive) && restoredFromArchive)
                {
                    if (context != nullptr && !sourcesNeedingAnalysis.isEmpty())
                    {
                        requestAnalysisForAudioSources(
                            context->getAudioSourceRefsForClipIds(
                                sourcesNeedingAnalysis));
                    }
                    lastKnownGoodArchive.reset();
                    lastKnownGoodArchiveId.clear();
                    statePreservationFailed = false;
                    return true;
                }

                if (archivingController != nullptr)
                    archivingController->setDocumentArchiveId(archiveId);
                const bool rolledBack = rebuildTrack(previousTrack,
                                                      lastKnownGoodArchive)
                                     && restoredFromArchive;
                if (rolledBack)
                {
                    lastKnownGoodArchive.reset();
                    lastKnownGoodArchiveId.clear();
                }
                else
                {
                    clearContext();
                    statePreservationFailed = true;
                }
                return false;
            }
        }

        if (track.name == trackName)
        {
            std::vector<ARA::ARAAudioSourceRef> addedAudioSources;
            auto& controller = documentController->getDocumentController();
            if (context->update(controller,
                                playbackRendererRef,
                                playbackRendererInterface,
                                editorRendererRef,
                                editorRendererInterface,
                                track,
                                addedAudioSources))
            {
                trackSnapshot = track;
                signatures = makeSignatures(track);
                requestAnalysisForAudioSources(addedAudioSources);
                notifySelection();
                controller.notifyModelUpdates();
                lastKnownGoodArchive.reset();
                lastKnownGoodArchiveId.clear();
                statePreservationFailed = false;
                return true;
            }

            const bool rolledBack = rebuildTrack(previousTrack, lastKnownGoodArchive)
                                 && restoredFromArchive;
            if (rolledBack)
            {
                lastKnownGoodArchive.reset();
                lastKnownGoodArchiveId.clear();
            }
            else
            {
                clearContext();
                statePreservationFailed = true;
            }
            return false;
        }

        if (rebuildTrack(track, lastKnownGoodArchive) && restoredFromArchive)
        {
            lastKnownGoodArchive.reset();
            lastKnownGoodArchiveId.clear();
            statePreservationFailed = false;
            return true;
        }

        const bool rolledBack = rebuildTrack(previousTrack, lastKnownGoodArchive)
                             && restoredFromArchive;
        if (rolledBack)
        {
            lastKnownGoodArchive.reset();
            lastKnownGoodArchiveId.clear();
        }
        else
        {
            clearContext();
            statePreservationFailed = true;
        }
        return false;
    }

    void editorOpened()
    {
        if (!initialised)
            return;

        if (restoredFromArchive)
            notifySelection();
        else
        {
            requestAnalysisAndSelection();
            // A request may be unavailable until the plug-in finishes licence
            // or editor initialisation. Retry that missing request once, but
            // never restart analysis that was already accepted for this track.
            delayedEditorRetryTicks = analysisRequestIssued ? 0 : 10;
        }

        if (documentController != nullptr)
            documentController->getDocumentController().notifyModelUpdates();
    }

    bool matchesTrack(const TrackData& track) const
    {
        return trackName == track.name && signatures == makeSignatures(track);
    }

    bool captureArchive(juce::MemoryBlock& archive,
                        juce::String& archiveId) const
    {
        archive.reset();
        archiveId.clear();
        if (statePreservationFailed)
        {
            if (lastKnownGoodArchive.isEmpty()
                || lastKnownGoodArchiveId.isEmpty())
                return false;
            archive = lastKnownGoodArchive;
            archiveId = lastKnownGoodArchiveId;
            return true;
        }

        if (documentController == nullptr || currentArchiveId.isEmpty()
            || context == nullptr || !initialised)
            return false;

        auto& controller = documentController->getDocumentController();
        controller.notifyModelUpdates();
        juce::MemoryOutputStream output(archive, false);
        const auto writerRef = ArchivingController::WriterConverter::toHostRef(&output);
        const bool stored = controller.supportsPartialPersistency()
            ? controller.storeObjectsToArchive(writerRef, nullptr)
            : controller.storeDocumentToArchive(writerRef);
        output.flush();
        if (!stored || archive.isEmpty())
        {
            archive.reset();
            return false;
        }

        archiveId = currentArchiveId;
        return true;
    }

    bool restoreArchive(const juce::MemoryBlock& archive)
    {
        if (!bindingInitialised || archive.isEmpty() || trackSnapshot.id.isEmpty()
            || statePreservationFailed)
            return false;

        const auto trackToRestore = trackSnapshot;
        juce::MemoryBlock rollbackArchive;
        juce::String rollbackArchiveId;
        if (!captureArchive(rollbackArchive, rollbackArchiveId)
            || rollbackArchive.isEmpty())
            return false;

        lastKnownGoodArchive = std::move(rollbackArchive);
        lastKnownGoodArchiveId = rollbackArchiveId;

        if (rebuildTrack(trackToRestore, archive) && restoredFromArchive)
        {
            lastKnownGoodArchive.reset();
            lastKnownGoodArchiveId.clear();
            return true;
        }

        if (archivingController != nullptr)
            archivingController->setDocumentArchiveId(rollbackArchiveId);
        const bool rolledBack = rebuildTrack(trackToRestore, lastKnownGoodArchive)
                             && restoredFromArchive;
        if (rolledBack)
        {
            lastKnownGoodArchive.reset();
            lastKnownGoodArchiveId.clear();
        }
        else
        {
            clearContext();
            statePreservationFailed = true;
        }
        return false;
    }

private:
    void removeContextFromRenderers() noexcept
    {
        if (!renderersAssigned || context == nullptr)
            return;

        const auto regionRefs = context->getPlaybackRegionRefs();
        for (auto region = regionRefs.rbegin();
             region != regionRefs.rend();
             ++region)
        {
            if (*region == nullptr)
                continue;
            editorRendererInterface->removePlaybackRegion(
                editorRendererRef, *region);
            playbackRendererInterface->removePlaybackRegion(
                playbackRendererRef, *region);
        }
        renderersAssigned = false;
    }

    void addContextToRenderers()
    {
        if (context == nullptr)
            return;

        for (const auto regionRef : context->getPlaybackRegionRefs())
        {
            if (regionRef == nullptr)
                continue;
            playbackRendererInterface->addPlaybackRegion(
                playbackRendererRef, regionRef);
            editorRendererInterface->addPlaybackRegion(
                editorRendererRef, regionRef);
        }
        renderersAssigned = true;
    }

    void clearContext()
    {
        if (context == nullptr || documentController == nullptr)
            return;

        removeContextFromRenderers();
        auto& controller = documentController->getDocumentController();
        {
            const juce::ARAEditGuard guard(controller);
            context->destroy(controller);
        }
        context.reset();
        initialised = false;
    }

    bool rebuildTrack(const TrackData& track,
                      const juce::MemoryBlock& archive)
    {
        if (!bindingInitialised || documentController == nullptr)
            return false;

        clearContext();
        analysisRequestIssued = false;
        delayedEditorRetryTicks = 0;
        restoredFromArchive = false;
        auto& controller = documentController->getDocumentController();
        bool graphCreated = false;
        const bool useTimeline =
            archive.isEmpty()
            || track.vst3AraHostFormatVersion
                >= TrackData::timelineVst3AraHostFormatVersion;

        if (archive.isEmpty())
        {
            const juce::ARAEditGuard guard(controller);
            context = std::make_unique<TrackContext>(controller, track,
                                                     useTimeline);
            graphCreated = context->isValid();
        }
        else
        {
            auto* readableArchive = const_cast<juce::MemoryBlock*>(&archive);
            const auto readerRef = ArchivingController::ReaderConverter::toHostRef(
                readableArchive);
            if (controller.supportsPartialPersistency())
            {
                const juce::ARAEditGuard guard(controller);
                context = std::make_unique<TrackContext>(controller, track,
                                                         useTimeline);
                graphCreated = context->isValid();
                if (graphCreated)
                {
                    // Older projects may contain PitchNet data for clips that
                    // were deleted before SimpleRec Pro started synchronising
                    // ARA edits incrementally. Restore only objects that still
                    // exist in the current track graph. This preserves edits on
                    // surviving clips while dropping orphaned corrections.
                    const bool v2RangedIds =
                        track.vst3AraHostFormatVersion == 2;
                    std::vector<juce::String> audioSourceArchiveIdStorage;
                    std::vector<juce::String> audioSourceCurrentIdStorage;
                    std::vector<juce::String> modificationArchiveIdStorage;
                    std::vector<juce::String> modificationCurrentIdStorage;
                    if (useTimeline)
                    {
                        if (!context->getAudioSourceRefs().empty())
                        {
                            const auto sourceId =
                                makeTimelineAudioSourcePersistentId(track.id);
                            audioSourceArchiveIdStorage.push_back(sourceId);
                            audioSourceCurrentIdStorage.push_back(sourceId);
                            modificationArchiveIdStorage.push_back(
                                makeTimelineModificationPersistentId(track.id));
                            modificationCurrentIdStorage.push_back(
                                makeTimelineModificationPersistentId(track.id));
                        }
                    }
                    else
                    {
                        audioSourceArchiveIdStorage.reserve(track.clips.size());
                        audioSourceCurrentIdStorage.reserve(track.clips.size());
                        modificationArchiveIdStorage.reserve(track.clips.size());
                        modificationCurrentIdStorage.reserve(track.clips.size());
                        for (const auto& clip : track.clips)
                        {
                            if (!isUsableClip(clip))
                                continue;
                            const auto currentSourceId =
                                makeAudioSourcePersistentId(track.id, clip);
                            const auto currentModificationId =
                                makeModificationPersistentId(track.id, clip);
                            const auto archiveSourceId = v2RangedIds
                                ? makeV2AudioSourcePersistentId(track.id, clip)
                                : currentSourceId;
                            audioSourceArchiveIdStorage.push_back(archiveSourceId);
                            modificationArchiveIdStorage.push_back(
                                archiveSourceId + ":modification");
                            audioSourceCurrentIdStorage.push_back(currentSourceId);
                            modificationCurrentIdStorage.push_back(
                                currentModificationId);
                        }
                    }

                    const auto makeIdPointers = [](const auto& storage)
                    {
                        std::vector<ARA::ARAPersistentID> result;
                        result.reserve(storage.size());
                        for (const auto& id : storage)
                            result.push_back(id.toRawUTF8());
                        return result;
                    };
                    const auto audioSourceArchiveIds =
                        makeIdPointers(audioSourceArchiveIdStorage);
                    const auto audioSourceCurrentIds =
                        makeIdPointers(audioSourceCurrentIdStorage);
                    const auto modificationArchiveIds =
                        makeIdPointers(modificationArchiveIdStorage);
                    const auto modificationCurrentIds =
                        makeIdPointers(modificationCurrentIdStorage);

                    using RestoreFilter = ARA::SizedStruct<
                        ARA_STRUCT_MEMBER(ARARestoreObjectsFilter,
                                          audioModificationCurrentIDs)>;
                    const RestoreFilter restoreFilter {
                        ARA::kARAFalse,
                        static_cast<ARA::ARASize>(audioSourceArchiveIds.size()),
                        audioSourceArchiveIds.empty() ? nullptr
                                                      : audioSourceArchiveIds.data(),
                        v2RangedIds && !audioSourceCurrentIds.empty()
                            ? audioSourceCurrentIds.data() : nullptr,
                        static_cast<ARA::ARASize>(modificationArchiveIds.size()),
                        modificationArchiveIds.empty() ? nullptr
                                                       : modificationArchiveIds.data(),
                        v2RangedIds && !modificationCurrentIds.empty()
                            ? modificationCurrentIds.data() : nullptr
                    };
                    restoredFromArchive = controller.restoreObjectsFromArchive(
                        readerRef,
                        &restoreFilter);
                }
            }
            else
            {
                // ARA 1 full-document restoration is itself the single edit
                // cycle. The raw graph calls above intentionally avoid JUCE's
                // ARAHostModel wrappers, which would open nested edit guards.
                const bool beganRestore =
                    controller.beginRestoringDocumentFromArchive(readerRef);
                if (beganRestore)
                {
                    context = std::make_unique<TrackContext>(controller, track,
                                                             useTimeline);
                    graphCreated = context->isValid();
                    const bool endedRestore =
                        controller.endRestoringDocumentFromArchive(readerRef);
                    restoredFromArchive = graphCreated && endedRestore;
                }
                else
                {
                    // The plug-in rejected this archive before opening a
                    // restore cycle. Build a clean document instead, so no
                    // unmatched end-restoring call or partially restored graph
                    // can escape into playback.
                    const juce::ARAEditGuard guard(controller);
                    context = std::make_unique<TrackContext>(controller, track,
                                                             useTimeline);
                    graphCreated = context->isValid();
                }
            }
        }

        if (!graphCreated)
        {
            if (context != nullptr)
            {
                const juce::ARAEditGuard guard(controller);
                context->destroy(controller);
                context.reset();
            }
            return false;
        }

        if (restoredFromArchive)
        {
            const juce::ARAEditGuard guard(controller);
            if (track.vst3AraHostFormatVersion < 3)
            {
                // Archives written before masked audio sources were introduced
                // can contain analysis for samples that are now hidden.
                context->notifyHiddenAudioChanged(controller);
            }
            // Playback regions are host-owned and non-persistent. PitchNet
            // nevertheless caches their old ranges inside its modification
            // archive. Recreate them after restoring the modification so the
            // current clip boundaries are the final model edit it receives.
            graphCreated = context->recreatePlaybackRegions(controller);
        }

        if (!graphCreated)
        {
            if (context != nullptr)
            {
                const juce::ARAEditGuard guard(controller);
                context->destroy(controller);
                context.reset();
            }
            return false;
        }

        addContextToRenderers();
        trackSnapshot = track;
        signatures = makeSignatures(track);
        trackName = track.name;
        initialised = true;
        if (restoredFromArchive)
        {
            // Hidden parts of a trimmed source are silent and are re-analysed
            // after a legacy archive restore. Full clips keep their restored
            // PitchNet edits without unnecessary analysis.
            requestAnalysisForAudioSources(context->getRangedAudioSourceRefs());
            notifySelection();
        }
        else
            requestAnalysisAndSelection();
        controller.notifyModelUpdates();
        if (!isTimerRunning())
            startTimerHz(10);
        return true;
    }

    void requestAnalysisAndSelection()
    {
        if (documentController == nullptr || context == nullptr)
            return;

        if (!analysisRequestIssued)
        {
            const auto audioSourceRefs = context->getAudioSourceRefs();
            if (analyzableContentTypes.empty() || audioSourceRefs.empty())
            {
                analysisRequestIssued = true;
            }
            else if (documentController->getDocumentController()
                         .isLicensedForCapabilities(
                             false,
                             static_cast<ARA::ARASize>(analyzableContentTypes.size()),
                             analyzableContentTypes.data(),
                             ARA::kARAPlaybackTransformationNoChanges))
            {
                int requestedSources = 0;
                for (const auto audioSourceRef : audioSourceRefs)
                {
                    if (audioSourceRef == nullptr)
                        continue;
                    documentController->getDocumentController()
                        .requestAudioSourceContentAnalysis(
                            audioSourceRef,
                            static_cast<ARA::ARASize>(analyzableContentTypes.size()),
                            analyzableContentTypes.data());
                    ++requestedSources;
                }
                analysisRequestIssued = requestedSources > 0;
            }
        }

        notifySelection();
    }

    void requestAnalysisForAudioSources(
        const std::vector<ARA::ARAAudioSourceRef>& audioSourceRefs)
    {
        if (documentController == nullptr || audioSourceRefs.empty()
            || analyzableContentTypes.empty())
            return;

        auto& controller = documentController->getDocumentController();
        if (!controller.isLicensedForCapabilities(
                false,
                static_cast<ARA::ARASize>(analyzableContentTypes.size()),
                analyzableContentTypes.data(),
                ARA::kARAPlaybackTransformationNoChanges))
            return;

        for (const auto audioSourceRef : audioSourceRefs)
        {
            if (audioSourceRef == nullptr)
                continue;
            controller.requestAudioSourceContentAnalysis(
                audioSourceRef,
                static_cast<ARA::ARASize>(analyzableContentTypes.size()),
                analyzableContentTypes.data());
        }
    }

    void notifySelection()
    {
        if (documentController == nullptr || context == nullptr)
            return;

        if (editorView != nullptr && editorView->isProvided())
        {
            std::vector<ARA::ARAPlaybackRegionRef> playbackRegionRefs;
            // ARA allows editor views that can focus only one explicitly
            // selected playback region to use the first item in the list.
            // PitchNet follows that behaviour, so explicitly selecting every
            // clip makes only the first clip visible. For a multi-clip track,
            // select the region sequence instead and let the editor derive the
            // complete set of playback regions that belong to the track.
            const auto allPlaybackRegionRefs =
                context->getPlaybackRegionRefs();
            if (context->isTimelineMode()
                || allPlaybackRegionRefs.size() == 1)
            {
                const auto regionRef = allPlaybackRegionRefs.empty()
                    ? nullptr : allPlaybackRegionRefs.front();
                if (regionRef != nullptr)
                    playbackRegionRefs.push_back(regionRef);
            }

            const auto regionSequenceRef = context->getRegionSequenceRef();
            using Selection = ARA::SizedStruct<ARA_STRUCT_MEMBER(ARAViewSelection,
                                                                 timeRange)>;
            const Selection selection {
                static_cast<ARA::ARASize>(playbackRegionRefs.size()),
                playbackRegionRefs.empty() ? nullptr : playbackRegionRefs.data(),
                1U,
                &regionSequenceRef,
                nullptr
            };
            editorView->notifyHideRegionSequences(0, nullptr);
            editorView->notifySelection(&selection);
        }
    }

    void timerCallback() override
    {
        if (documentController != nullptr && initialised)
            documentController->getDocumentController().notifyModelUpdates();

        if (delayedEditorRetryTicks > 0
            && --delayedEditorRetryTicks == 0)
        {
            if (restoredFromArchive)
                notifySelection();
            else if (!analysisRequestIssued)
                requestAnalysisAndSelection();
        }
    }

public:

    std::unique_ptr<juce::ARAHostDocumentController> documentController;
    ArchivingController* archivingController = nullptr;
    ARA::ARAPlaybackRendererRef playbackRendererRef = nullptr;
    const ARA::ARAPlaybackRendererInterface* playbackRendererInterface = nullptr;
    ARA::ARAEditorRendererRef editorRendererRef = nullptr;
    const ARA::ARAEditorRendererInterface* editorRendererInterface = nullptr;
    std::unique_ptr<ARA::Host::EditorView> editorView;
    std::unique_ptr<TrackContext> context;
    TrackData trackSnapshot;
    std::vector<ClipSignature> signatures;
    std::vector<ARA::ARAContentType> analyzableContentTypes;
    juce::String trackName;
    juce::String currentArchiveId;
    juce::MemoryBlock lastKnownGoodArchive;
    juce::String lastKnownGoodArchiveId;
    int delayedEditorRetryTicks = 0;
    bool analysisRequestIssued = false;
    bool bindingInitialised = false;
    bool renderersAssigned = false;
    bool restoredFromArchive = false;
    bool initialised = false;
    bool statePreservationFailed = false;
};

AraTrackHost::AraTrackHost(juce::AudioPluginInstance& editorInstance,
                           juce::AudioPluginInstance* playbackInstance,
                           juce::ARAFactoryWrapper factory,
                           TransportCallbacks transportCallbacks,
                           const juce::String& savedArchiveId)
    : impl(std::make_unique<Impl>(editorInstance,
                                 playbackInstance,
                                 std::move(factory),
                                 std::move(transportCallbacks),
                                 savedArchiveId))
{
}

AraTrackHost::~AraTrackHost() = default;

bool AraTrackHost::isInitialised() const noexcept
{
    return impl != nullptr && impl->initialised;
}

bool AraTrackHost::matchesTrack(const TrackData& track) const
{
    return impl != nullptr && impl->matchesTrack(track);
}

bool AraTrackHost::initialiseTrack(const TrackData& track,
                                   const juce::MemoryBlock& savedArchive)
{
    return impl != nullptr && impl->initialiseTrack(track, savedArchive);
}

bool AraTrackHost::setTrack(const TrackData& track)
{
    return impl != nullptr && impl->setTrack(track);
}

void AraTrackHost::editorOpened()
{
    if (impl != nullptr)
        impl->editorOpened();
}

bool AraTrackHost::captureArchive(juce::MemoryBlock& archive,
                                  juce::String& archiveId) const
{
    return impl != nullptr && impl->captureArchive(archive, archiveId);
}

bool AraTrackHost::restoreArchive(const juce::MemoryBlock& archive)
{
    return impl != nullptr && impl->restoreArchive(archive);
}

bool AraTrackHost::wasArchiveRestored() const noexcept
{
    return impl != nullptr && impl->restoredFromArchive;
}
