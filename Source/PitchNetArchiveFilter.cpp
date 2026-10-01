#include "PitchNetArchiveFilter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>

namespace PitchNetArchiveFilter
{
namespace
{
constexpr std::uint32_t pnarVersion = 1;
constexpr double pitchFrameSamples = 512.0;

struct Span
{
    size_t start = 0;
    size_t end = 0;
};

struct Replacement
{
    Span span;
    std::string text;
};

std::uint32_t readLittleEndian32(const unsigned char* data) noexcept
{
    return static_cast<std::uint32_t>(data[0])
        | (static_cast<std::uint32_t>(data[1]) << 8U)
        | (static_cast<std::uint32_t>(data[2]) << 16U)
        | (static_cast<std::uint32_t>(data[3]) << 24U);
}

bool isWhitespace(unsigned char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

void skipWhitespace(const unsigned char* data, size_t limit, size_t& position)
{
    while (position < limit && isWhitespace(data[position]))
        ++position;
}

bool skipString(const unsigned char* data, size_t limit, size_t& position)
{
    if (position >= limit || data[position] != '"')
        return false;

    ++position;
    bool escaped = false;
    while (position < limit)
    {
        const auto value = data[position++];
        if (escaped)
        {
            escaped = false;
            continue;
        }
        if (value == '\\')
        {
            escaped = true;
            continue;
        }
        if (value == '"')
            return true;
    }
    return false;
}

bool skipValue(const unsigned char* data, size_t limit, size_t& position)
{
    skipWhitespace(data, limit, position);
    if (position >= limit)
        return false;

    if (data[position] == '"')
        return skipString(data, limit, position);

    if (data[position] == '{' || data[position] == '[')
    {
        const auto opening = data[position];
        const auto closing = opening == '{' ? '}' : ']';
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        while (position < limit)
        {
            const auto value = data[position++];
            if (inString)
            {
                if (escaped)
                    escaped = false;
                else if (value == '\\')
                    escaped = true;
                else if (value == '"')
                    inString = false;
                continue;
            }
            if (value == '"')
            {
                inString = true;
                continue;
            }
            if (value == opening)
                ++depth;
            else if (value == closing && --depth == 0)
                return true;
        }
        return false;
    }

    const auto start = position;
    while (position < limit && data[position] != ','
           && data[position] != ']' && data[position] != '}')
        ++position;
    auto end = position;
    while (end > start && isWhitespace(data[end - 1]))
        --end;
    return end > start;
}

bool readAsciiKey(const unsigned char* data,
                  size_t limit,
                  size_t& position,
                  std::string& key)
{
    if (position >= limit || data[position] != '"')
        return false;
    const auto start = ++position;
    while (position < limit && data[position] != '"')
    {
        if (data[position] == '\\' || data[position] < 0x20)
            return false;
        ++position;
    }
    if (position >= limit)
        return false;
    key.assign(reinterpret_cast<const char*>(data + start), position - start);
    ++position;
    return true;
}

std::optional<Span> findTopLevelProperty(const unsigned char* data,
                                         Span object,
                                         const std::string& property)
{
    if (object.end <= object.start + 1 || data[object.start] != '{'
        || data[object.end - 1] != '}')
        return std::nullopt;

    size_t position = object.start + 1;
    while (position < object.end - 1)
    {
        skipWhitespace(data, object.end - 1, position);
        if (position >= object.end - 1)
            break;

        std::string key;
        if (!readAsciiKey(data, object.end - 1, position, key))
            return std::nullopt;
        skipWhitespace(data, object.end - 1, position);
        if (position >= object.end - 1 || data[position++] != ':')
            return std::nullopt;
        skipWhitespace(data, object.end - 1, position);
        const auto valueStart = position;
        if (!skipValue(data, object.end - 1, position))
            return std::nullopt;
        const auto valueEnd = position;
        if (key == property)
            return Span { valueStart, valueEnd };

        skipWhitespace(data, object.end - 1, position);
        if (position < object.end - 1)
        {
            if (data[position++] != ',')
                return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<Span>> getArrayElements(const unsigned char* data,
                                                  Span array)
{
    if (array.end <= array.start + 1 || data[array.start] != '['
        || data[array.end - 1] != ']')
        return std::nullopt;

    std::vector<Span> result;
    size_t position = array.start + 1;
    while (position < array.end - 1)
    {
        skipWhitespace(data, array.end - 1, position);
        if (position >= array.end - 1)
            break;
        const auto start = position;
        if (!skipValue(data, array.end - 1, position))
            return std::nullopt;
        result.push_back({ start, position });
        skipWhitespace(data, array.end - 1, position);
        if (position < array.end - 1)
        {
            if (data[position++] != ',')
                return std::nullopt;
        }
    }
    return result;
}

std::vector<ActiveRange> mergeRanges(std::vector<ActiveRange> ranges)
{
    ranges.erase(std::remove_if(ranges.begin(), ranges.end(),
        [](const ActiveRange& range)
        {
            return !std::isfinite(range.startSeconds)
                || !std::isfinite(range.endSeconds)
                || range.startSeconds < 0.0
                || range.endSeconds <= range.startSeconds;
        }), ranges.end());
    std::sort(ranges.begin(), ranges.end(),
        [](const ActiveRange& left, const ActiveRange& right)
        {
            return left.startSeconds < right.startSeconds;
        });

    std::vector<ActiveRange> merged;
    for (const auto& range : ranges)
    {
        if (merged.empty() || range.startSeconds > merged.back().endSeconds + 1.0e-9)
            merged.push_back(range);
        else
            merged.back().endSeconds = std::max(merged.back().endSeconds,
                                                 range.endSeconds);
    }
    return merged;
}

juce::String formatNumber(double value)
{
    auto text = juce::String(value, 15);
    if (text.containsChar('.'))
    {
        text = text.trimCharactersAtEnd("0");
        text = text.trimCharactersAtEnd(".");
    }
    return text.isEmpty() || text == "-0" ? juce::String("0") : text;
}

std::string makePlaybackRangesJson(const std::vector<ActiveRange>& ranges)
{
    juce::String result("[");
    for (size_t index = 0; index < ranges.size(); ++index)
    {
        if (index != 0)
            result += ",";
        result += "[" + formatNumber(ranges[index].startSeconds)
               + "," + formatNumber(ranges[index].endSeconds) + "]";
    }
    result += "]";
    return result.toStdString();
}

bool rangesEqual(const juce::var& value,
                 const std::vector<ActiveRange>& expected)
{
    const auto* ranges = value.getArray();
    if (ranges == nullptr || static_cast<size_t>(ranges->size()) != expected.size())
        return false;
    for (int index = 0; index < ranges->size(); ++index)
    {
        const auto* pair = ranges->getReference(index).getArray();
        if (pair == nullptr || pair->size() != 2)
            return false;
        const auto start = static_cast<double>(pair->getReference(0));
        const auto end = static_cast<double>(pair->getReference(1));
        if (!std::isfinite(start) || !std::isfinite(end)
            || std::abs(start - expected[static_cast<size_t>(index)].startSeconds) > 1.0e-9
            || std::abs(end - expected[static_cast<size_t>(index)].endSeconds) > 1.0e-9)
            return false;
    }
    return true;
}

std::vector<std::pair<int64_t, int64_t>> makeFrameRanges(
    const std::vector<ActiveRange>& ranges,
    double sampleRate)
{
    std::vector<std::pair<int64_t, int64_t>> result;
    for (const auto& range : ranges)
    {
        const auto start = static_cast<int64_t>(std::ceil(
            range.startSeconds * sampleRate / pitchFrameSamples - 1.0e-9));
        const auto end = static_cast<int64_t>(std::floor(
            range.endSeconds * sampleRate / pitchFrameSamples + 1.0e-9));
        if (end > start)
            result.emplace_back(start, end);
    }
    return result;
}

bool readIntegerValue(const juce::var& value, int64_t& result)
{
    if (!value.isInt() && !value.isInt64() && !value.isDouble())
        return false;
    const auto numeric = static_cast<double>(value);
    if (!std::isfinite(numeric)
        || numeric < static_cast<double>(std::numeric_limits<int>::min())
        || numeric > static_cast<double>(std::numeric_limits<int>::max())
        || std::abs(numeric - std::round(numeric)) > 1.0e-9)
        return false;
    result = static_cast<int64_t>(std::llround(numeric));
    return true;
}

bool readIntegerProperty(const juce::var& object,
                         const juce::Identifier& property,
                         int64_t& result)
{
    return object.isObject()
        && readIntegerValue(object.getProperty(property, juce::var()), result);
}

bool isInsideOneRange(int64_t start,
                      int64_t end,
                      const std::vector<std::pair<int64_t, int64_t>>& ranges)
{
    return std::any_of(ranges.begin(), ranges.end(),
        [start, end](const auto& range)
        {
            return start >= range.first && end <= range.second;
        });
}

const ModificationRanges* findModificationForBlock(
    const unsigned char* data,
    size_t magicOffset,
    const std::vector<ModificationRanges>& modifications)
{
    const auto searchStart = magicOffset > 512 ? magicOffset - 512 : 0;
    const ModificationRanges* match = nullptr;
    for (const auto& modification : modifications)
    {
        const auto id = modification.persistentId.toStdString();
        if (id.empty() || id.size() > magicOffset - searchStart)
            continue;
        const auto* found = std::search(data + searchStart,
                                        data + magicOffset,
                                        id.begin(),
                                        id.end());
        if (found != data + magicOffset)
        {
            if (match != nullptr)
                return nullptr;
            match = &modification;
        }
    }
    return match;
}

bool appendReplacement(std::vector<Replacement>& replacements,
                       Span span,
                       std::string text)
{
    if (span.end <= span.start || text.size() > span.end - span.start)
        return false;
    replacements.push_back({ span, std::move(text) });
    return true;
}
}

Result filterInPlace(juce::MemoryBlock& block,
                     const juce::String& documentArchiveId,
                     const std::vector<ModificationRanges>& modifications)
{
    Result result;
    if (documentArchiveId != archiveId || block.isEmpty() || modifications.empty())
        return result;

    std::vector<ModificationRanges> normalised = modifications;
    for (auto& modification : normalised)
    {
        modification.activeRanges = mergeRanges(std::move(modification.activeRanges));
        if (modification.persistentId.isEmpty())
            return result;
    }

    const auto* data = static_cast<const unsigned char*>(block.getData());
    const auto size = block.getSize();
    std::vector<Replacement> replacements;
    bool foundSupportedBlock = false;

    for (size_t magic = 0; magic + 17 <= size; ++magic)
    {
        if (std::memcmp(data + magic, "RANP", 4) != 0)
            continue;
        result.pnarFound = true;
        if (readLittleEndian32(data + magic + 4) != pnarVersion)
            continue;
        const auto jsonByteCount = static_cast<size_t>(
            readLittleEndian32(data + magic + 8));
        const auto jsonStart = magic + 16;
        if (jsonByteCount == 0
            || jsonByteCount > size - jsonStart
            || jsonByteCount
                > static_cast<size_t>(std::numeric_limits<int>::max())
            || readLittleEndian32(data + magic + 12) != 0
            || data[jsonStart] != '{')
            continue;

        const auto declaredJsonEnd = jsonStart + jsonByteCount;
        size_t jsonEnd = jsonStart;
        if (!skipValue(data, declaredJsonEnd, jsonEnd))
            return result;
        skipWhitespace(data, declaredJsonEnd, jsonEnd);
        if (jsonEnd != declaredJsonEnd)
            return result;
        const Span jsonSpan { jsonStart, declaredJsonEnd };
        const auto jsonLength = static_cast<int>(jsonSpan.end - jsonSpan.start);
        if (jsonLength <= 0)
            return result;
        const auto parsed = juce::JSON::parse(juce::String::fromUTF8(
            reinterpret_cast<const char*>(data + jsonSpan.start), jsonLength));
        if (!parsed.isObject()
            || static_cast<int>(parsed.getProperty("formatVersion", 0)) != 1)
            return result;

        const auto sampleRate = static_cast<double>(
            parsed.getProperty("sampleRate", 0.0));
        const auto playbackValue = parsed.getProperty("playbackRegionRanges", juce::var());
        const auto notesValue = parsed.getProperty("notes", juce::var());
        const auto segmentsValue = parsed.getProperty("segmentChunkRanges", juce::var());
        if (!std::isfinite(sampleRate) || sampleRate < 8000.0 || sampleRate > 384000.0
            || !playbackValue.isArray() || !notesValue.isArray()
            || !segmentsValue.isArray())
            return result;
        foundSupportedBlock = true;
        ++result.pnarBlocks;
        const auto* modification = findModificationForBlock(data, magic, normalised);
        if (modification == nullptr)
            continue;

        const auto playbackSpan = findTopLevelProperty(data, jsonSpan,
                                                       "playbackRegionRanges");
        const auto notesSpan = findTopLevelProperty(data, jsonSpan, "notes");
        const auto segmentsSpan = findTopLevelProperty(data, jsonSpan,
                                                       "segmentChunkRanges");
        if (!playbackSpan || !notesSpan || !segmentsSpan)
            return result;
        const auto noteElements = getArrayElements(data, *notesSpan);
        if (!noteElements
            || noteElements->size() != static_cast<size_t>(notesValue.size()))
            return result;

        const auto frameRanges = makeFrameRanges(modification->activeRanges,
                                                 sampleRate);

        std::string retainedNotes("[");
        int removedNotes = 0;
        for (int index = 0; index < notesValue.size(); ++index)
        {
            const auto note = notesValue[index];
            int64_t startFrame = 0;
            int64_t endFrame = 0;
            int64_t sourceStartFrame = 0;
            int64_t sourceEndFrame = 0;
            if (!readIntegerProperty(note, "startFrame", startFrame)
                || !readIntegerProperty(note, "endFrame", endFrame)
                || !readIntegerProperty(note, "srcStartFrame", sourceStartFrame)
                || !readIntegerProperty(note, "srcEndFrame", sourceEndFrame)
                || endFrame <= startFrame || sourceEndFrame <= sourceStartFrame)
                return result;

            if (!isInsideOneRange(sourceStartFrame, sourceEndFrame, frameRanges))
            {
                ++removedNotes;
                continue;
            }
            if (retainedNotes.size() > 1)
                retainedNotes.push_back(',');
            const auto span = (*noteElements)[static_cast<size_t>(index)];
            retainedNotes.append(reinterpret_cast<const char*>(data + span.start),
                                 span.end - span.start);
        }
        retainedNotes.push_back(']');

        std::vector<std::pair<int64_t, int64_t>> retainedSegments;
        for (int index = 0; index < segmentsValue.size(); ++index)
        {
            const auto* pair = segmentsValue[index].getArray();
            if (pair == nullptr || pair->size() != 2)
                return result;
            int64_t start = 0;
            int64_t end = 0;
            if (!readIntegerValue(pair->getReference(0), start)
                || !readIntegerValue(pair->getReference(1), end))
                return result;
            if (end <= start)
                return result;
            for (const auto& allowed : frameRanges)
            {
                const auto clippedStart = std::max(start, allowed.first);
                const auto clippedEnd = std::min(end, allowed.second);
                if (clippedEnd > clippedStart)
                    retainedSegments.emplace_back(clippedStart, clippedEnd);
            }
        }

        juce::String segmentsJson("[");
        for (size_t index = 0; index < retainedSegments.size(); ++index)
        {
            if (index != 0)
                segmentsJson += ",";
            segmentsJson += "[" + juce::String(retainedSegments[index].first)
                         + "," + juce::String(retainedSegments[index].second) + "]";
        }
        segmentsJson += "]";

        const bool playbackChanged = !rangesEqual(playbackValue,
                                                   modification->activeRanges);
        const bool notesChanged = removedNotes > 0;
        const bool segmentsChanged = retainedSegments.size()
            != static_cast<size_t>(segmentsValue.size());
        if (!playbackChanged && !notesChanged && !segmentsChanged)
            continue;

        if (!appendReplacement(replacements, *playbackSpan,
                               makePlaybackRangesJson(modification->activeRanges))
            || !appendReplacement(replacements, *notesSpan, std::move(retainedNotes))
            || !appendReplacement(replacements, *segmentsSpan,
                                  segmentsJson.toStdString()))
            return result;
        result.notesRemoved += removedNotes;
        result.segmentRangesRemoved += std::max(
            0, segmentsValue.size() - static_cast<int>(retainedSegments.size()));
    }

    if (!foundSupportedBlock)
        return result;

    std::sort(replacements.begin(), replacements.end(),
        [](const Replacement& left, const Replacement& right)
        {
            return left.span.start < right.span.start;
        });
    for (size_t index = 1; index < replacements.size(); ++index)
        if (replacements[index - 1].span.end > replacements[index].span.start)
            return result;

    result.recognised = true;
    if (replacements.empty())
        return result;
    auto* destination = static_cast<unsigned char*>(block.getData());
    for (const auto& replacement : replacements)
    {
        std::memcpy(destination + replacement.span.start,
                    replacement.text.data(), replacement.text.size());
        std::memset(destination + replacement.span.start + replacement.text.size(),
                    ' ',
                    replacement.span.end - replacement.span.start
                        - replacement.text.size());
    }
    result.changed = true;
    return result;
}

std::vector<ModificationRanges> makeModificationRanges(const TrackData& track)
{
    if (track.vst3AraHostFormatVersion
        >= TrackData::timelineVst3AraHostFormatVersion)
    {
        ModificationRanges timeline {
            track.id + ":timeline:modification",
            {}
        };
        timeline.activeRanges.reserve(track.clips.size());
        for (const auto& clip : track.clips)
        {
            if (clip.id.isEmpty() || clip.buffer == nullptr
                || clip.sampleRate <= 0 || clip.duration <= 0.0)
                continue;
            const auto sampleCount =
                static_cast<int64_t>(clip.buffer->getNumSamples());
            const auto startSample = juce::jlimit<int64_t>(
                0,
                sampleCount,
                static_cast<int64_t>(std::llround(
                    std::max(0.0, clip.offset) * clip.sampleRate)));
            const auto requestedSamples = static_cast<int64_t>(std::llround(
                clip.duration * clip.sampleRate));
            const auto visibleSamples = juce::jlimit<int64_t>(
                0, sampleCount - startSample, requestedSamples);
            if (visibleSamples <= 0)
                continue;
            const auto start = std::max(0.0, clip.startTime);
            timeline.activeRanges.push_back({
                start,
                start + static_cast<double>(visibleSamples) / clip.sampleRate
            });
        }
        if (timeline.persistentId.isNotEmpty()
            && (!timeline.activeRanges.empty() || track.clips.empty()))
            return { std::move(timeline) };
        return {};
    }

    std::vector<ModificationRanges> result;
    result.reserve(track.clips.size());
    for (const auto& clip : track.clips)
    {
        if (clip.id.isEmpty() || clip.buffer == nullptr || clip.sampleRate <= 0)
            continue;

        const auto sampleCount = static_cast<int64_t>(clip.buffer->getNumSamples());
        const auto startSample = juce::jlimit<int64_t>(
            0,
            sampleCount,
            static_cast<int64_t>(std::llround(
                std::max(0.0, clip.offset) * clip.sampleRate)));
        const auto requestedSamples = static_cast<int64_t>(std::llround(
            std::max(0.0, clip.duration) * clip.sampleRate));
        const auto visibleSamples = juce::jlimit<int64_t>(
            0, sampleCount - startSample, requestedSamples);
        if (visibleSamples <= 0)
            continue;

        result.push_back({
            track.id + ":" + clip.id + ":modification",
            { { static_cast<double>(startSample) / clip.sampleRate,
                static_cast<double>(startSample + visibleSamples)
                    / clip.sampleRate } }
        });
    }
    return result;
}

Result filterVst3StateInPlace(
    juce::MemoryBlock& vst3State,
    const juce::String& documentArchiveId,
    const std::vector<ModificationRanges>& modifications)
{
    if (documentArchiveId != archiveId || vst3State.isEmpty()
        || vst3State.getSize()
            > static_cast<size_t>(std::numeric_limits<int>::max()))
        return {};

    auto xml = juce::AudioProcessor::getXmlFromBinary(
        vst3State.getData(), static_cast<int>(vst3State.getSize()));
    if (xml == nullptr || !xml->hasTagName("VST3PluginState"))
        return {};

    auto* component = xml->getChildByName("IComponent");
    if (component == nullptr)
        return {};

    juce::MemoryBlock componentState;
    if (!componentState.fromBase64Encoding(component->getAllSubText())
        || componentState.isEmpty())
        return {};

    const auto result = filterInPlace(componentState,
                                      documentArchiveId,
                                      modifications);
    if (!result.recognised || !result.changed)
        return result;

    component->deleteAllTextElements();
    component->addTextElement(componentState.toBase64Encoding());

    juce::MemoryBlock filteredState;
    juce::AudioProcessor::copyXmlToBinary(*xml, filteredState);
    if (filteredState.isEmpty())
        return {};
    vst3State = std::move(filteredState);
    return result;
}
}
