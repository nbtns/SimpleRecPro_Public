#include <JuceHeader.h>

#include "../Source/PitchNetArchiveFilter.h"
#include "../Source/ProjectSerializer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>

namespace
{
bool expectFilter(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAILED: " << message << std::endl;
    return condition;
}

void appendLittleEndian32(juce::MemoryOutputStream& output, std::uint32_t value)
{
    const unsigned char bytes[] {
        static_cast<unsigned char>(value & 0xffU),
        static_cast<unsigned char>((value >> 8U) & 0xffU),
        static_cast<unsigned char>((value >> 16U) & 0xffU),
        static_cast<unsigned char>((value >> 24U) & 0xffU)
    };
    output.write(bytes, sizeof(bytes));
}

void appendPnarBlock(juce::MemoryOutputStream& output,
                     const juce::String& modificationId,
                     std::uint32_t declaredLengthOverride)
{
    const juce::String json = R"json({
  "formatVersion": 1,
  "sampleRate": 44100,
  "playbackRegionRanges": [[0.0, 2.0]],
  "notes": [
    {"startFrame":10,"endFrame":20,"srcStartFrame":10,"srcEndFrame":20,"midiNote":60},
    {"startFrame":90,"endFrame":100,"srcStartFrame":90,"srcEndFrame":100,"midiNote":62}
  ],
  "segmentChunkRanges": [[0,80],[80,120],[130,150]]
})json";
    if (modificationId.isNotEmpty())
    {
        output.write(modificationId.toRawUTF8(),
                     static_cast<size_t>(modificationId.getNumBytesAsUTF8()));
        output.writeByte(0);
    }
    output.write("RANP", 4);
    appendLittleEndian32(output, 1);
    appendLittleEndian32(
        output,
        declaredLengthOverride != 0
            ? declaredLengthOverride
            : static_cast<std::uint32_t>(json.getNumBytesAsUTF8()));
    appendLittleEndian32(output, 0);
    output.write(json.toRawUTF8(), static_cast<size_t>(json.getNumBytesAsUTF8()));
    appendLittleEndian32(output, 1);
    output.writeByte(0x5a);
}

void appendTimelinePnarBlock(juce::MemoryOutputStream& output,
                             const juce::String& modificationId,
                             const juce::String& json)
{
    output.write(modificationId.toRawUTF8(),
                 static_cast<size_t>(modificationId.getNumBytesAsUTF8()));
    output.writeByte(0);
    output.write("RANP", 4);
    appendLittleEndian32(output, 1);
    appendLittleEndian32(
        output,
        static_cast<std::uint32_t>(json.getNumBytesAsUTF8()));
    appendLittleEndian32(output, 0);
    output.write(json.toRawUTF8(), static_cast<size_t>(json.getNumBytesAsUTF8()));
    output.writeByte(0x5a);
}

juce::MemoryBlock makeTimelineArchive(const juce::String& modificationId)
{
    const juce::String json = R"json({
  "formatVersion": 1,
  "sampleRate": 51200,
  "playbackRegionRanges": [[1,7],[15,21]],
  "notes": [
    {"startFrame":150,"endFrame":200,"srcStartFrame":150,"srcEndFrame":200},
    {"startFrame":550,"endFrame":600,"srcStartFrame":550,"srcEndFrame":600},
    {"startFrame":800,"endFrame":850,"srcStartFrame":800,"srcEndFrame":850},
    {"startFrame":1600,"endFrame":1650,"srcStartFrame":1600,"srcEndFrame":1650}
  ],
  "segmentChunkRanges": [[50,250],[500,800],[800,900],[1500,1800],[2050,2200]]
})json";
    juce::MemoryBlock archive;
    juce::MemoryOutputStream output(archive, false);
    appendTimelinePnarBlock(output, modificationId, json);
    output.flush();
    return archive;
}

bool blocksEqual(const juce::MemoryBlock& left, const juce::MemoryBlock& right)
{
    return left.getSize() == right.getSize()
        && std::memcmp(left.getData(), right.getData(), left.getSize()) == 0;
}

bool containsAscii(const juce::MemoryBlock& block, const std::string& text)
{
    const auto* begin = static_cast<const char*>(block.getData());
    return std::search(begin, begin + block.getSize(), text.begin(), text.end())
        != begin + block.getSize();
}

int countAscii(const juce::MemoryBlock& block, const std::string& text)
{
    const auto* begin = static_cast<const char*>(block.getData());
    const auto* end = begin + block.getSize();
    int count = 0;
    while (begin != end)
    {
        const auto* found = std::search(begin, end, text.begin(), text.end());
        if (found == end)
            break;
        ++count;
        begin = found + text.size();
    }
    return count;
}

bool replaceAscii(juce::MemoryBlock& block,
                  const std::string& before,
                  const std::string& after)
{
    if (before.size() != after.size())
        return false;
    auto* begin = static_cast<char*>(block.getData());
    auto* found = std::search(begin, begin + block.getSize(),
                              before.begin(), before.end());
    if (found == begin + block.getSize())
        return false;
    std::memcpy(found, after.data(), after.size());
    return true;
}

bool testSafePitchNetRangeFilter()
{
    const juce::String modificationId("track:clip:modification");
    juce::MemoryBlock archive;
    juce::MemoryOutputStream output(archive, false);
    appendPnarBlock(output, {}, 0);
    appendPnarBlock(output, modificationId, 0);
    output.flush();
    const auto originalSize = archive.getSize();

    const auto result = PitchNetArchiveFilter::filterInPlace(
        archive,
        PitchNetArchiveFilter::archiveId,
        { { modificationId, { { 0.0, 1.0 } } } });
    return expectFilter(result.recognised && result.changed,
                        "valid PitchNet PNAR blocks should be filtered")
        && expectFilter(result.pnarBlocks == 2 && result.notesRemoved == 1,
                        "only the explicitly identified modification PNAR should be filtered")
        && expectFilter(archive.getSize() == originalSize,
                        "filtering must preserve the surrounding archive size")
        && expectFilter(containsAscii(archive, "\"startFrame\":10")
                            && countAscii(archive, "\"startFrame\":90") == 1,
                        "only the identified modification's out-of-range note should disappear")
        && expectFilter(containsAscii(archive, "[[0,1]]")
                            && containsAscii(archive, "[80,86]")
                            && countAscii(archive, "[130,150]") == 1,
                        "document PNAR should stay intact while modification ranges are clipped")
        && expectFilter(containsAscii(archive, "Z"),
                        "binary data following PNAR JSON must remain present");
}

bool testUnknownPitchNetDataIsUntouched()
{
    const juce::String modificationId("track:clip:modification");
    juce::MemoryBlock archive;
    juce::MemoryOutputStream output(archive, false);
    appendPnarBlock(output, modificationId, 0);
    output.flush();
    const auto original = archive;

    const auto wrongIdResult = PitchNetArchiveFilter::filterInPlace(
        archive,
        "com.example.not-pitchnet",
        { { modificationId, { { 0.0, 1.0 } } } });
    if (!expectFilter(!wrongIdResult.recognised && blocksEqual(archive, original),
                      "non-PitchNet archive IDs must remain byte-for-byte unchanged"))
        return false;

    juce::MemoryBlock unsupported;
    juce::MemoryOutputStream unsupportedOutput(unsupported, false);
    appendPnarBlock(unsupportedOutput, modificationId,
                    std::numeric_limits<std::uint32_t>::max());
    unsupportedOutput.flush();
    const auto unsupportedOriginal = unsupported;
    const auto unsupportedResult = PitchNetArchiveFilter::filterInPlace(
        unsupported,
        PitchNetArchiveFilter::archiveId,
        { { modificationId, { { 0.0, 1.0 } } } });
    return expectFilter(!unsupportedResult.recognised
                            && blocksEqual(unsupported, unsupportedOriginal),
                        "invalid PNAR payload lengths must fail closed without mutation");
}

bool testPitchNetVst3ComponentStateFilter()
{
    const juce::String modificationId("track:clip:modification");
    juce::MemoryBlock componentState;
    juce::MemoryOutputStream componentOutput(componentState, false);
    appendPnarBlock(componentOutput, modificationId, 0);
    componentOutput.flush();

    juce::XmlElement xml("VST3PluginState");
    xml.createNewChildElement("IComponent")
        ->addTextElement(componentState.toBase64Encoding());
    const juce::MemoryBlock controllerState("controller", 10);
    xml.createNewChildElement("IEditController")
        ->addTextElement(controllerState.toBase64Encoding());

    juce::MemoryBlock wrappedState;
    juce::AudioProcessor::copyXmlToBinary(xml, wrappedState);
    const auto result = PitchNetArchiveFilter::filterVst3StateInPlace(
        wrappedState,
        PitchNetArchiveFilter::archiveId,
        { { modificationId, { { 0.0, 1.0 } } } });
    auto filteredXml = juce::AudioProcessor::getXmlFromBinary(
        wrappedState.getData(), static_cast<int>(wrappedState.getSize()));
    if (!expectFilter(result.recognised && result.changed
                          && result.notesRemoved == 1,
                      "PitchNet's VST3 component PNAR should be filtered")
        || !expectFilter(filteredXml != nullptr,
                         "filtered VST3 state must remain a valid JUCE wrapper"))
        return false;

    juce::MemoryBlock filteredComponent;
    juce::MemoryBlock filteredController;
    const auto* component = filteredXml->getChildByName("IComponent");
    const auto* controller = filteredXml->getChildByName("IEditController");
    return expectFilter(component != nullptr && controller != nullptr
                            && filteredComponent.fromBase64Encoding(
                                component->getAllSubText())
                            && filteredController.fromBase64Encoding(
                                controller->getAllSubText()),
                        "both VST3 state sections must remain decodable")
        && expectFilter(containsAscii(filteredComponent, "\"startFrame\":10")
                            && !containsAscii(filteredComponent,
                                              "\"startFrame\":90"),
                        "only the out-of-range VST3 note should be removed")
        && expectFilter(blocksEqual(filteredController, controllerState),
                        "the unrelated VST3 controller state must stay unchanged");
}

bool testTimelineModificationRanges()
{
    auto buffer = std::make_shared<juce::AudioBuffer<float>>(1, 400);
    buffer->clear();

    AudioClip first;
    first.id = "clip-a";
    first.buffer = buffer;
    first.sampleRate = 10;
    first.startTime = 1.0;
    first.offset = 0.5;
    first.duration = 6.0;

    AudioClip second = first;
    second.id = "clip-b";
    second.startTime = 15.0;
    second.offset = 2.0;
    second.duration = 6.0;

    TrackData track;
    track.id = "track-a";
    track.clips = { first, second };

    track.vst3AraHostFormatVersion =
        TrackData::timelineVst3AraHostFormatVersion - 1;
    const auto legacyRanges =
        PitchNetArchiveFilter::makeModificationRanges(track);

    track.vst3AraHostFormatVersion =
        TrackData::timelineVst3AraHostFormatVersion;
    const auto ranges =
        PitchNetArchiveFilter::makeModificationRanges(track);
    return expectFilter(
               legacyRanges.size() == 2
                   && legacyRanges[0].persistentId
                       == "track-a:clip-a:modification"
                   && legacyRanges[1].persistentId
                       == "track-a:clip-b:modification",
               "version 5 archives should keep clip-specific modifications")
        && expectFilter(ranges.size() == 1,
                        "timeline archives should use one modification")
        && expectFilter(
            ranges.front().persistentId
                == "track-a:timeline:modification",
            "timeline modification ID should remain stable across saves")
        && expectFilter(ranges.front().activeRanges.size() == 2,
                        "both clips should remain active in the timeline archive")
        && expectFilter(
            std::abs(ranges.front().activeRanges[0].startSeconds - 1.0)
                    < 1.0e-9
                && std::abs(ranges.front().activeRanges[0].endSeconds - 7.0)
                    < 1.0e-9
                && std::abs(ranges.front().activeRanges[1].startSeconds - 15.0)
                    < 1.0e-9
                && std::abs(ranges.front().activeRanges[1].endSeconds - 21.0)
                    < 1.0e-9,
            "timeline archive ranges should use each clip's playback position");
}

bool testTimelineDeletionAndSilenceFilter()
{
    const juce::String modificationId("track-a:timeline:modification");
    auto archive = makeTimelineArchive(modificationId);
    const auto result = PitchNetArchiveFilter::filterInPlace(
        archive,
        PitchNetArchiveFilter::archiveId,
        { { modificationId, { { 1.0, 7.0 } } } });

    return expectFilter(result.recognised && result.changed,
                        "deleting a timeline clip should filter its PNAR data")
        && expectFilter(result.notesRemoved == 2,
                        "deleted and silent timeline notes should be removed")
        && expectFilter(containsAscii(archive, "\"startFrame\":150")
                            && containsAscii(archive, "\"startFrame\":550")
                            && !containsAscii(archive, "\"startFrame\":800")
                            && !containsAscii(archive, "\"startFrame\":1600"),
                        "only notes inside the surviving clip should remain")
        && expectFilter(containsAscii(archive, "\"playbackRegionRanges\": [[1,7]]")
                            && containsAscii(archive, "[100,250]")
                            && containsAscii(archive, "[500,700]")
                            && !containsAscii(archive, "[1500,1800]"),
                        "playback and segment ranges should match the surviving clip");
}

bool testTimelineFrontAndBackTrimFilter()
{
    const juce::String modificationId("track-a:timeline:modification");
    auto backTrimmed = makeTimelineArchive(modificationId);
    const auto backResult = PitchNetArchiveFilter::filterInPlace(
        backTrimmed,
        PitchNetArchiveFilter::archiveId,
        { { modificationId, { { 1.0, 5.0 } } } });

    auto frontTrimmed = makeTimelineArchive(modificationId);
    const auto frontResult = PitchNetArchiveFilter::filterInPlace(
        frontTrimmed,
        PitchNetArchiveFilter::archiveId,
        { { modificationId, { { 3.0, 7.0 } } } });

    return expectFilter(backResult.recognised && backResult.changed
                            && containsAscii(backTrimmed, "\"startFrame\":150")
                            && !containsAscii(backTrimmed, "\"startFrame\":550"),
                        "back trim should keep only notes before the new end")
        && expectFilter(frontResult.recognised && frontResult.changed
                            && !containsAscii(frontTrimmed, "\"startFrame\":150")
                            && containsAscii(frontTrimmed, "\"startFrame\":550"),
                        "front trim should keep only notes after the new start");
}

bool testTimelineLastClipDeletion()
{
    const juce::String modificationId("track-a:timeline:modification");
    auto archive = makeTimelineArchive(modificationId);
    const auto result = PitchNetArchiveFilter::filterInPlace(
        archive,
        PitchNetArchiveFilter::archiveId,
        { { modificationId, {} } });

    TrackData emptyTimelineTrack;
    emptyTimelineTrack.id = "track-a";
    emptyTimelineTrack.vst3AraHostFormatVersion =
        TrackData::timelineVst3AraHostFormatVersion;
    const auto ranges =
        PitchNetArchiveFilter::makeModificationRanges(emptyTimelineTrack);

    return expectFilter(result.recognised && result.changed
                            && result.notesRemoved == 4,
                        "deleting the last timeline clip should remove every note")
        && expectFilter(containsAscii(archive, "\"playbackRegionRanges\": []")
                            && containsAscii(archive, "\"notes\": [")
                            && !containsAscii(archive, "\"startFrame\":"),
                        "an empty timeline should retain no PitchNet ranges or notes")
        && expectFilter(ranges.size() == 1
                            && ranges.front().persistentId == modificationId
                            && ranges.front().activeRanges.empty(),
                        "an empty timeline should keep its stable modification ID");
}

bool testMalformedPitchNetDataIsUntouched()
{
    const juce::String modificationId("track-a:timeline:modification");
    const juce::String missingFieldJson = R"json({
  "formatVersion":1,
  "sampleRate":51200,
  "playbackRegionRanges":[[1,7]],
  "notes":[],
  "notSegmentChunkRanges":[]
})json";
    juce::MemoryBlock malformed;
    juce::MemoryOutputStream output(malformed, false);
    appendTimelinePnarBlock(output, modificationId, missingFieldJson);
    output.flush();
    const auto original = malformed;

    const auto result = PitchNetArchiveFilter::filterInPlace(
        malformed,
        PitchNetArchiveFilter::archiveId,
        { { modificationId, { { 1.0, 7.0 } } } });
    return expectFilter(!result.recognised && blocksEqual(malformed, original),
                        "missing required PNAR fields must fail closed");
}

bool testInvalidPitchNetHeadersAndJsonAreUntouched()
{
    const juce::String modificationId("track-a:timeline:modification");
    const auto ranges = std::vector<PitchNetArchiveFilter::ModificationRanges> {
        { modificationId, { { 1.0, 7.0 } } }
    };

    auto invalidMagic = makeTimelineArchive(modificationId);
    replaceAscii(invalidMagic, "RANP", "XANP");
    const auto invalidMagicOriginal = invalidMagic;
    const auto invalidMagicResult = PitchNetArchiveFilter::filterInPlace(
        invalidMagic, PitchNetArchiveFilter::archiveId, ranges);

    auto invalidVersion = makeTimelineArchive(modificationId);
    replaceAscii(invalidVersion,
                 std::string("RANP\x01\x00\x00\x00", 8),
                 std::string("RANP\x02\x00\x00\x00", 8));
    const auto invalidVersionOriginal = invalidVersion;
    const auto invalidVersionResult = PitchNetArchiveFilter::filterInPlace(
        invalidVersion, PitchNetArchiveFilter::archiveId, ranges);

    auto invalidJson = makeTimelineArchive(modificationId);
    replaceAscii(invalidJson, "\"formatVersion\": 1",
                              "\"formatVersion\": x");
    const auto invalidJsonOriginal = invalidJson;
    const auto invalidJsonResult = PitchNetArchiveFilter::filterInPlace(
        invalidJson, PitchNetArchiveFilter::archiveId, ranges);

    return expectFilter(
               !invalidMagicResult.recognised
                   && blocksEqual(invalidMagic, invalidMagicOriginal),
               "invalid PNAR magic must remain untouched")
        && expectFilter(
               !invalidVersionResult.recognised
                   && blocksEqual(invalidVersion, invalidVersionOriginal),
               "unsupported PNAR versions must remain untouched")
        && expectFilter(
               !invalidJsonResult.recognised
                   && blocksEqual(invalidJson, invalidJsonOriginal),
               "malformed PNAR JSON must remain untouched");
}

bool testVst3StateSafetyChecks()
{
    const juce::String modificationId("track-a:timeline:modification");
    const auto componentState = makeTimelineArchive(modificationId);
    juce::XmlElement xml("VST3PluginState");
    xml.createNewChildElement("IComponent")
        ->addTextElement(componentState.toBase64Encoding());
    juce::MemoryBlock wrappedState;
    juce::AudioProcessor::copyXmlToBinary(xml, wrappedState);

    const auto original = wrappedState;
    const auto wrongIdResult =
        PitchNetArchiveFilter::filterVst3StateInPlace(
            wrappedState,
            "com.example.not-pitchnet",
            { { modificationId, { { 1.0, 7.0 } } } });

    juce::MemoryBlock malformed("not-a-vst3-state", 16);
    const auto malformedOriginal = malformed;
    const auto malformedResult =
        PitchNetArchiveFilter::filterVst3StateInPlace(
            malformed,
            PitchNetArchiveFilter::archiveId,
            { { modificationId, { { 1.0, 7.0 } } } });

    const juce::MemoryBlock plainComponent("ordinary-vst-state", 18);
    juce::XmlElement plainXml("VST3PluginState");
    plainXml.createNewChildElement("IComponent")
        ->addTextElement(plainComponent.toBase64Encoding());
    juce::MemoryBlock plainState;
    juce::AudioProcessor::copyXmlToBinary(plainXml, plainState);
    const auto plainOriginal = plainState;
    const auto plainResult =
        PitchNetArchiveFilter::filterVst3StateInPlace(
            plainState,
            PitchNetArchiveFilter::archiveId,
            { { modificationId, { { 1.0, 7.0 } } } });

    auto unsupportedComponent = makeTimelineArchive(modificationId);
    replaceAscii(unsupportedComponent,
                 std::string("RANP\x01\x00\x00\x00", 8),
                 std::string("RANP\x02\x00\x00\x00", 8));
    juce::XmlElement unsupportedXml("VST3PluginState");
    unsupportedXml.createNewChildElement("IComponent")
        ->addTextElement(unsupportedComponent.toBase64Encoding());
    juce::MemoryBlock unsupportedState;
    juce::AudioProcessor::copyXmlToBinary(unsupportedXml, unsupportedState);
    const auto unsupportedOriginal = unsupportedState;
    const auto unsupportedResult =
        PitchNetArchiveFilter::filterVst3StateInPlace(
            unsupportedState,
            PitchNetArchiveFilter::archiveId,
            { { modificationId, { { 1.0, 7.0 } } } });

    return expectFilter(!wrongIdResult.recognised
                            && blocksEqual(wrappedState, original),
                        "non-PitchNet VST3 state must remain untouched")
        && expectFilter(!malformedResult.recognised
                            && blocksEqual(malformed, malformedOriginal),
                        "malformed VST3 state must remain untouched")
        && expectFilter(!plainResult.pnarFound && !plainResult.recognised
                            && blocksEqual(plainState, plainOriginal),
                        "ordinary VST3 state without PNAR must remain usable")
        && expectFilter(unsupportedResult.pnarFound
                            && !unsupportedResult.recognised
                            && blocksEqual(unsupportedState,
                                           unsupportedOriginal),
                        "unsupported PNAR inside VST3 state must fail closed");
}

bool testTracksBuildIndependentTimelineRanges()
{
    auto buffer = std::make_shared<juce::AudioBuffer<float>>(1, 100);
    AudioClip clip;
    clip.id = "clip";
    clip.buffer = buffer;
    clip.sampleRate = 10;
    clip.duration = 2.0;

    TrackData first;
    first.id = "track-a";
    first.vst3AraHostFormatVersion =
        TrackData::timelineVst3AraHostFormatVersion;
    clip.startTime = 1.0;
    first.clips = { clip };

    TrackData second = first;
    second.id = "track-b";
    second.clips.front().startTime = 15.0;

    const auto firstRanges =
        PitchNetArchiveFilter::makeModificationRanges(first);
    const auto secondRanges =
        PitchNetArchiveFilter::makeModificationRanges(second);
    return expectFilter(
        firstRanges.size() == 1 && secondRanges.size() == 1
            && firstRanges.front().persistentId
                == "track-a:timeline:modification"
            && secondRanges.front().persistentId
                == "track-b:timeline:modification"
            && firstRanges.front().activeRanges.front().startSeconds == 1.0
            && secondRanges.front().activeRanges.front().startSeconds == 15.0,
        "timeline filtering ranges must remain isolated per track");
}
}

bool runPitchNetArchiveFilterTests()
{
    return testSafePitchNetRangeFilter()
        && testUnknownPitchNetDataIsUntouched()
        && testPitchNetVst3ComponentStateFilter()
        && testTimelineModificationRanges()
        && testTimelineDeletionAndSilenceFilter()
        && testTimelineFrontAndBackTrimFilter()
        && testTimelineLastClipDeletion()
        && testMalformedPitchNetDataIsUntouched()
        && testInvalidPitchNetHeadersAndJsonAreUntouched()
        && testVst3StateSafetyChecks()
        && testTracksBuildIndependentTimelineRanges();
}

int inspectPitchNetProjectArchive(const juce::File& projectFile)
{
    ProjectSerializer::LoadedProjectData project;
    if (!ProjectSerializer::loadProject(projectFile, project))
    {
        std::cerr << "Could not load project for read-only PitchNet inspection."
                  << std::endl;
        return 2;
    }

    for (const auto& track : project.tracks)
    {
        if (!track.vst3Name.containsIgnoreCase("PitchNet")
            || track.vst3AraArchive == nullptr
            || track.vst3AraArchive->isEmpty())
            continue;

        const auto modifications =
            PitchNetArchiveFilter::makeModificationRanges(track);

        const auto printPnarBlocks = [](const juce::MemoryBlock& dataBlock,
                                        const char* label)
        {
            const auto* data = static_cast<const unsigned char*>(
                dataBlock.getData());
            for (size_t offset = 0; offset + 16 <= dataBlock.getSize();
                 ++offset)
            {
                if (std::memcmp(data + offset, "RANP", 4) != 0)
                    continue;
                const auto read32 = [data](size_t position)
                {
                    return static_cast<std::uint32_t>(data[position])
                        | (static_cast<std::uint32_t>(data[position + 1]) << 8U)
                        | (static_cast<std::uint32_t>(data[position + 2]) << 16U)
                        | (static_cast<std::uint32_t>(data[position + 3]) << 24U);
                };
                const auto version = read32(offset + 4);
                const auto jsonBytes = static_cast<size_t>(read32(offset + 8));
                std::cout << label << " RANP offset=" << offset
                          << " version=" << version
                          << " jsonBytes=" << jsonBytes
                          << " prefix=";
                if (jsonBytes <= dataBlock.getSize() - offset - 16)
                {
                    const auto prefixBytes = std::min<size_t>(200, jsonBytes);
                    for (size_t byte = 0; byte < prefixBytes; ++byte)
                    {
                        const auto value = data[offset + 16 + byte];
                        std::cout << (value >= 0x20 && value <= 0x7e
                                          ? static_cast<char>(value)
                                          : ' ');
                    }
                }
                std::cout << std::endl;
            }
        };
        printPnarBlocks(*track.vst3AraArchive, "ARA");

        juce::MemoryBlock workingCopy(*track.vst3AraArchive);
        const auto result = PitchNetArchiveFilter::filterInPlace(
            workingCopy, track.vst3AraArchiveId, modifications);
        auto filteredState = track.vst3State != nullptr
            ? juce::MemoryBlock(*track.vst3State) : juce::MemoryBlock();
        const auto stateResult = PitchNetArchiveFilter::filterVst3StateInPlace(
            filteredState, track.vst3AraArchiveId, modifications);
        auto simulatedTrack = track;
        if (!simulatedTrack.clips.empty())
            simulatedTrack.clips.front().duration *= 0.5;
        const auto simulatedRanges =
            PitchNetArchiveFilter::makeModificationRanges(simulatedTrack);
        auto simulatedArchive = juce::MemoryBlock(*track.vst3AraArchive);
        const auto simulatedResult = PitchNetArchiveFilter::filterInPlace(
            simulatedArchive, track.vst3AraArchiveId, simulatedRanges);
        auto simulatedState = track.vst3State != nullptr
            ? juce::MemoryBlock(*track.vst3State) : juce::MemoryBlock();
        const auto simulatedStateResult =
            PitchNetArchiveFilter::filterVst3StateInPlace(
                simulatedState, track.vst3AraArchiveId, simulatedRanges);
        std::cout << "PitchNet track=" << track.name
                  << " recognised=" << result.recognised
                  << " pnarFound=" << result.pnarFound
                  << " changed=" << result.changed
                  << " pnarBlocks=" << result.pnarBlocks
                  << " notesRemoved=" << result.notesRemoved
                  << " segmentRangesRemoved=" << result.segmentRangesRemoved
                  << " stateRecognised=" << stateResult.recognised
                  << " statePnarFound=" << stateResult.pnarFound
                  << " stateChanged=" << stateResult.changed
                  << " stateNotesRemoved=" << stateResult.notesRemoved
                  << " vstStateBytes="
                  << (track.vst3State != nullptr ? track.vst3State->getSize() : 0)
                  << " originalBytes=" << track.vst3AraArchive->getSize()
                  << " filteredBytes=" << workingCopy.getSize()
                  << std::endl;
        std::cout << "Simulated first-clip trim recognised="
                  << simulatedResult.recognised
                  << " changed=" << simulatedResult.changed
                  << " notesRemoved=" << simulatedResult.notesRemoved
                  << " segmentRangesRemoved="
                  << simulatedResult.segmentRangesRemoved
                  << " stateRecognised="
                  << simulatedStateResult.recognised
                  << " stateChanged=" << simulatedStateResult.changed
                  << std::endl;

        if (track.vst3State != nullptr && !track.vst3State->isEmpty())
        {
            const auto* bytes = static_cast<const unsigned char*>(
                track.vst3State->getData());
            const auto byteCount = track.vst3State->getSize();
            std::cout << "VST3 state prefix=";
            const auto prefixLength = std::min<size_t>(64, byteCount);
            for (size_t index = 0; index < prefixLength; ++index)
                std::cout << std::hex << std::setfill('0') << std::setw(2)
                          << static_cast<int>(bytes[index]);
            std::cout << std::dec << std::endl;

            if (auto stateXml = juce::AudioProcessor::getXmlFromBinary(
                    track.vst3State->getData(),
                    static_cast<int>(track.vst3State->getSize())))
            {
                juce::MemoryBlock componentState;
                if (const auto* component =
                        stateXml->getChildByName("IComponent");
                    component != nullptr
                    && componentState.fromBase64Encoding(
                        component->getAllSubText()))
                {
                    printPnarBlocks(componentState, "VST3 component");
                    const auto* componentBytes =
                        static_cast<const unsigned char*>(
                            componentState.getData());
                    for (size_t offset = 0;
                         offset + 12 <= componentState.getSize();
                         ++offset)
                    {
                        if (std::memcmp(componentBytes + offset,
                                        "RANP", 4) != 0)
                            continue;
                        std::cout << "VST3 component RANP offset=" << offset
                                  << " version="
                                  << static_cast<int>(
                                         componentBytes[offset + 4])
                                  << " layoutBytes="
                                  << std::hex
                                  << static_cast<int>(
                                         componentBytes[offset + 8])
                                  << ","
                                  << static_cast<int>(
                                         componentBytes[offset + 9])
                                  << ","
                                  << static_cast<int>(
                                         componentBytes[offset + 10])
                                  << ","
                                  << static_cast<int>(
                                         componentBytes[offset + 11])
                                  << std::dec
                                  << " componentBytes="
                                  << componentState.getSize()
                                  << " prefix=";
                        const auto diagnosticBytes = std::min<size_t>(
                            64, componentState.getSize() - offset);
                        for (size_t byte = 0; byte < diagnosticBytes; ++byte)
                        {
                            std::cout << std::hex << std::setfill('0')
                                      << std::setw(2)
                                      << static_cast<int>(
                                             componentBytes[offset + byte]);
                        }
                        std::cout << std::dec << std::endl;
                    }
                }
            }

            int printedStrings = 0;
            for (size_t index = 0; index < byteCount && printedStrings < 40;)
            {
                if (bytes[index] < 0x20 || bytes[index] > 0x7e)
                {
                    ++index;
                    continue;
                }
                const auto start = index;
                while (index < byteCount && bytes[index] >= 0x20
                       && bytes[index] <= 0x7e)
                    ++index;
                if (index - start >= 8)
                {
                    const auto length = std::min<size_t>(index - start, 160);
                    std::cout << "VST3 ascii offset=" << start << " text="
                              << std::string(reinterpret_cast<const char*>(bytes + start),
                                             length)
                              << std::endl;
                    ++printedStrings;
                }
            }
        }
        return result.recognised
            && simulatedResult.recognised
            && simulatedResult.changed
            && workingCopy.getSize() == track.vst3AraArchive->getSize()
            && simulatedArchive.getSize() == track.vst3AraArchive->getSize()
            ? 0 : 3;
    }

    std::cerr << "No PitchNet ARA archive was found in the project." << std::endl;
    return 4;
}
