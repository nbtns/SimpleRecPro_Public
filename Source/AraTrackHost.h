#pragma once

#include <JuceHeader.h>

#include "TrackData.h"

#include <functional>
#include <memory>

/**
 * Supplies a SimpleRec Pro track to an ARA-capable plug-in.
 *
 * Conventional VST3 effects only see the small audio blocks that are currently
 * playing. ARA plug-ins such as PitchNet need random access to the complete
 * clips on a track so that they can analyse and display them immediately.
 */
class AraTrackHost final
{
public:
    struct TransportCallbacks
    {
        std::function<void()> startPlayback;
        std::function<void()> stopPlayback;
        std::function<void(double)> setPlaybackPosition;
        std::function<void()> documentChanged;
    };

    AraTrackHost(juce::AudioPluginInstance& editorInstance,
                 juce::AudioPluginInstance* playbackInstance,
                 juce::ARAFactoryWrapper factory,
                 TransportCallbacks transportCallbacks,
                 const juce::String& savedArchiveId = {});
    ~AraTrackHost();

    bool isInitialised() const noexcept;
    bool matchesTrack(const TrackData& track) const;

    /** Builds the host graph and restores its ARA state after VST3 state loading. */
    bool initialiseTrack(const TrackData& track,
                         const juce::MemoryBlock& savedArchive = {});

    /** The plug-in must be unprepared while this function changes ARA regions. */
    bool setTrack(const TrackData& track);

    /** Re-sends analysis and selection after the plug-in editor has been created. */
    void editorOpened();

    /** Stores the complete ARA document and returns the archive format identifier. */
    bool captureArchive(juce::MemoryBlock& archive,
                        juce::String& archiveId) const;

    /** Restores matching ARA objects after the host model graph has been rebuilt. */
    bool restoreArchive(const juce::MemoryBlock& archive);

    /** Returns the result when an archive was supplied during track initialisation. */
    bool wasArchiveRestored() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AraTrackHost)
};
