#pragma once

#include <JuceHeader.h>

#include "TrackData.h"

#include <vector>

namespace PitchNetArchiveFilter
{
inline constexpr const char* archiveId =
    "com.sessionloops.pitchnet.aradocumentarchive.1";

struct ActiveRange
{
    double startSeconds = 0.0;
    double endSeconds = 0.0;
};

struct ModificationRanges
{
    juce::String persistentId;
    std::vector<ActiveRange> activeRanges;
};

struct Result
{
    bool pnarFound = false;
    bool recognised = false;
    bool changed = false;
    int pnarBlocks = 0;
    int notesRemoved = 0;
    int segmentRangesRemoved = 0;
};

/**
 * Removes PitchNet analysis outside the supplied source ranges without changing
 * the size or offsets of the surrounding plug-in archive.
 *
 * The block is changed only after every recognised PNAR JSON block has passed
 * strict validation. Unknown archive IDs and unsupported PNAR layouts are left
 * byte-for-byte unchanged.
 */
Result filterInPlace(juce::MemoryBlock& block,
                     const juce::String& documentArchiveId,
                     const std::vector<ModificationRanges>& modifications);

/** Builds the exact source-time ranges used by PitchNet's persistent IDs. */
std::vector<ModificationRanges> makeModificationRanges(const TrackData& track);

/**
 * Filters the PNAR payload stored inside JUCE's VST3 IComponent state wrapper.
 * Unsupported or malformed wrappers are left byte-for-byte unchanged.
 */
Result filterVst3StateInPlace(
    juce::MemoryBlock& vst3State,
    const juce::String& documentArchiveId,
    const std::vector<ModificationRanges>& modifications);
}
