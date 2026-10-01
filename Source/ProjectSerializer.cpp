#include "RegionAutomation.h"
#include "ProjectSerializer.h"
#include "AudioEngine.h"
#include "RhythmWarp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
constexpr char projectSignatureV3[8] = { 'S', 'R', 'E', 'C', '_', 'V', '3', '\0' };
constexpr char projectSignatureV4[8] = { 'S', 'R', 'E', 'C', '_', 'V', '4', '\0' };

void setLoadError(juce::String* errorMessage, const juce::String& message)
{
    if (errorMessage != nullptr)
        *errorMessage = message;
}

bool readSchemaVersion(const juce::var& parsed,
                       bool isV3,
                       int& schemaVersion,
                       juce::String* errorMessage)
{
    const auto* root = parsed.getDynamicObject();
    if (root == nullptr)
        return false;

    const juce::Identifier schemaProperty("schemaVersion");
    const juce::Identifier legacyVersionProperty("version");
    const bool hasSchemaVersion = root->hasProperty(schemaProperty);
    const bool hasLegacyVersion = root->hasProperty(legacyVersionProperty);
    schemaVersion = isV3 ? 3 : 4;
    if (!hasSchemaVersion && !hasLegacyVersion)
        return true;

    const auto declaredVersion = root->getProperty(
        hasSchemaVersion ? schemaProperty : legacyVersionProperty);
    if (!(declaredVersion.isInt() || declaredVersion.isInt64()
          || declaredVersion.isDouble()))
    {
        setLoadError(errorMessage, juce::String::fromUTF8(
            u8"プロジェクトの保存形式番号が壊れています。"));
        return false;
    }

    const double numericVersion = static_cast<double>(declaredVersion);
    if (!std::isfinite(numericVersion)
        || std::floor(numericVersion) != numericVersion
        || numericVersion < 1.0
        || numericVersion > static_cast<double>(std::numeric_limits<int>::max()))
    {
        setLoadError(errorMessage, juce::String::fromUTF8(
            u8"プロジェクトの保存形式番号が壊れています。"));
        return false;
    }

    schemaVersion = static_cast<int>(numericVersion);
    if (schemaVersion > ProjectSerializer::currentSchemaVersion)
    {
        setLoadError(errorMessage,
            juce::String::fromUTF8(
                u8"このプロジェクトはより新しい保存形式（schema ")
                + juce::String(schemaVersion)
                + juce::String::fromUTF8(
                    u8"）で作成されています。SimpleRec Proを更新してから開いてください。"));
        return false;
    }
    return true;
}

struct BinaryWriteJob
{
    enum class Kind
    {
        rawBytes,
        floatSamples
    };

    const void* data = nullptr;
    size_t byteCount = 0;
    Kind kind = Kind::rawBytes;
};

bool appendBinaryJob(std::vector<BinaryWriteJob>& jobs,
                     juce::int64& currentOffset,
                     const void* data,
                     size_t byteCount,
                     BinaryWriteJob::Kind kind)
{
    if (byteCount > static_cast<size_t>(std::numeric_limits<juce::int64>::max())
        || currentOffset > std::numeric_limits<juce::int64>::max()
                                - static_cast<juce::int64>(byteCount))
        return false;

    if (byteCount > 0)
        jobs.push_back({ data, byteCount, kind });
    currentOffset += static_cast<juce::int64>(byteCount);
    return true;
}

bool addBinaryBlockProperty(juce::DynamicObject& target,
                            const juce::Identifier& propertyName,
                            const std::shared_ptr<const juce::MemoryBlock>& block,
                            std::vector<BinaryWriteJob>& jobs,
                            juce::int64& currentOffset)
{
    if (block == nullptr)
        return true;

    const auto blockSize = block->getSize();
    juce::DynamicObject::Ptr reference = new juce::DynamicObject();
    reference->setProperty("byteOffset", currentOffset);
    reference->setProperty("byteLength", static_cast<juce::int64>(blockSize));
    target.setProperty(propertyName, juce::var(reference.get()));
    return appendBinaryJob(jobs,
                           currentOffset,
                           blockSize > 0 ? block->getData() : nullptr,
                           blockSize,
                           BinaryWriteJob::Kind::rawBytes);
}

bool readBinaryBlock(const juce::var& reference,
                     const char* mappedData,
                     size_t mappedSize,
                     size_t binaryBaseOffset,
                     std::shared_ptr<const juce::MemoryBlock>& destination)
{
    destination.reset();
    if (reference.isVoid())
        return true;
    if (!reference.isObject())
        return false;

    const auto byteOffset = static_cast<juce::int64>(
        reference.getProperty("byteOffset", static_cast<juce::int64>(-1)));
    const auto byteLength = static_cast<juce::int64>(
        reference.getProperty("byteLength", static_cast<juce::int64>(-1)));
    if (byteOffset < 0 || byteLength < 0)
        return false;

    const auto offset = static_cast<juce::uint64>(byteOffset);
    const auto length = static_cast<juce::uint64>(byteLength);
    const auto base = static_cast<juce::uint64>(binaryBaseOffset);
    const auto total = static_cast<juce::uint64>(mappedSize);
    if (base > total || offset > total - base || length > total - base - offset
        || length > static_cast<juce::uint64>(std::numeric_limits<size_t>::max()))
        return false;

    auto block = std::make_shared<juce::MemoryBlock>();
    if (length > 0)
        *block = juce::MemoryBlock(mappedData + base + offset,
                                   static_cast<size_t>(length));
    destination = std::move(block);
    return true;
}

bool decodeLegacyBase64(const juce::var& encodedValue,
                        std::shared_ptr<const juce::MemoryBlock>& destination)
{
    destination.reset();
    const auto encoded = encodedValue.toString();
    if (encoded.isEmpty())
        return true;

    auto decoded = std::make_shared<juce::MemoryBlock>();
    if (!decoded->fromBase64Encoding(encoded))
        return false;
    destination = std::move(decoded);
    return true;
}
}

juce::String ProjectSerializer::colourToHex(juce::Colour c)
{
    return "#" + c.toDisplayString(true).toLowerCase();
}

juce::Colour ProjectSerializer::hexToColour(const juce::String& hex)
{
    juce::String h = hex;
    if (h.startsWithChar('#')) h = h.substring(1);

    if (h.length() == 8) return juce::Colour::fromString(h);
    if (h.length() == 6) return juce::Colour::fromString("ff" + h);

    static juce::Random random;
    return getTrackColor(random.nextInt(10));
}

ProjectSerializer::ProjectSnapshot ProjectSerializer::captureProjectSnapshot(
    AudioEngine& audioEngine)
{
    ProjectSnapshot snapshot;
    snapshot.bpm = audioEngine.getMetronome().getBpm();
    snapshot.timeSignatureNumerator = audioEngine.getMetronome().getNumerator();
    snapshot.timeSignatureDenominator = audioEngine.getMetronome().getDenominator();
    snapshot.zoomLevel = audioEngine.getZoomLevel();
    snapshot.masterVolume = audioEngine.getMasterVolume();
    snapshot.metronomeEnabled = audioEngine.getMetronome().getEnabled();
    snapshot.metronomeVolume = audioEngine.getMetronome().getVolume();
    snapshot.countInEnabled = audioEngine.isCountInEnabled();
    snapshot.dawSettings = audioEngine.getProjectDawSettings();
    snapshot.tracks = audioEngine.getTracksSnapshot();
    return snapshot;
}

bool ProjectSerializer::saveProject(const juce::File& file, AudioEngine& audioEngine)
{
    return saveProject(file, captureProjectSnapshot(audioEngine));
}

bool ProjectSerializer::saveProject(const juce::File& file,
                                    const ProjectSnapshot& snapshot)
{
    juce::DynamicObject::Ptr projectObj = new juce::DynamicObject();
    projectObj->setProperty("name", file.getFileNameWithoutExtension());
    projectObj->setProperty("bpm", snapshot.bpm);
    projectObj->setProperty("timeSignatureNumerator", snapshot.timeSignatureNumerator);
    projectObj->setProperty("timeSignatureDenominator", snapshot.timeSignatureDenominator);
    projectObj->setProperty("zoomLevel", snapshot.zoomLevel);
    projectObj->setProperty("masterVolume", snapshot.masterVolume);
    projectObj->setProperty("metronomeEnabled", snapshot.metronomeEnabled);
    projectObj->setProperty("metronomeVolume", snapshot.metronomeVolume);
    projectObj->setProperty("countInEnabled", snapshot.countInEnabled);
    projectObj->setProperty("musicalKey", snapshot.dawSettings.musicalKey);
    projectObj->setProperty("musicalScale",
                            static_cast<int>(snapshot.dawSettings.musicalScale));
    projectObj->setProperty("pitchCorrectionStrength",
                            snapshot.dawSettings.pitchCorrectionStrength);
    projectObj->setProperty("hasLoopStart", snapshot.dawSettings.hasLoopStart);
    projectObj->setProperty("hasLoopEnd", snapshot.dawSettings.hasLoopEnd);
    projectObj->setProperty("loopStartSeconds", snapshot.dawSettings.loopStartSeconds);
    projectObj->setProperty("loopEndSeconds", snapshot.dawSettings.loopEndSeconds);
    projectObj->setProperty("loopEnabled", snapshot.dawSettings.loopEnabled);

    juce::Array<juce::var> tempoPoints;
    for (const auto& point : snapshot.dawSettings.tempoMap)
    {
        juce::DynamicObject::Ptr pointObject = new juce::DynamicObject();
        pointObject->setProperty("timeSeconds", point.timeSeconds);
        pointObject->setProperty("bpm", point.bpm);
        pointObject->setProperty("numerator", point.numerator);
        pointObject->setProperty("denominator", point.denominator);
        tempoPoints.add(juce::var(pointObject.get()));
    }
    projectObj->setProperty("tempoMap", tempoPoints);

    const auto& exportDefaults = snapshot.dawSettings.exportDefaults;
    juce::DynamicObject::Ptr exportObject = new juce::DynamicObject();
    exportObject->setProperty("format", static_cast<int>(exportDefaults.format));
    exportObject->setProperty("channels", static_cast<int>(exportDefaults.channels));
    exportObject->setProperty("wavBits", exportDefaults.wavBits);
    exportObject->setProperty("mp3Kbps", exportDefaults.mp3Kbps);
    exportObject->setProperty("sampleRate", exportDefaults.sampleRate);
    exportObject->setProperty("target", static_cast<int>(exportDefaults.target));
    exportObject->setProperty("trackIndex", exportDefaults.trackIndex);
    exportObject->setProperty("rangeStartSeconds",
                              exportDefaults.rangeStartSeconds);
    exportObject->setProperty("rangeEndSeconds",
                              exportDefaults.rangeEndSeconds);
    projectObj->setProperty("exportDefaults", juce::var(exportObject.get()));

    const auto& mastering = snapshot.dawSettings.mastering;
    juce::DynamicObject::Ptr masteringObject = new juce::DynamicObject();
    masteringObject->setProperty("enabled", mastering.enabled);
    masteringObject->setProperty("limiterEnabled", mastering.limiterEnabled);
    masteringObject->setProperty("ceilingDb", mastering.ceilingDb);
    masteringObject->setProperty("targetLufs", mastering.targetLufs);
    masteringObject->setProperty("auditionProcessed", mastering.auditionProcessed);
    projectObj->setProperty("mastering", juce::var(masteringObject.get()));

    juce::Array<juce::var> tracksArray;
    juce::int64 currentBinaryOffset = 0;
    std::vector<BinaryWriteJob> binaryJobs;

    for (const auto& track : snapshot.tracks)
    {
        juce::DynamicObject::Ptr trackObj = new juce::DynamicObject();
        trackObj->setProperty("id", track.id.isEmpty() ? juce::Uuid().toString() : track.id);
        trackObj->setProperty("name", track.name);
        trackObj->setProperty("role", static_cast<int>(
            sanitiseTrackRole(static_cast<int>(track.role))));
        trackObj->setProperty("color", colourToHex(track.color));
        trackObj->setProperty("volume", track.volume);
        trackObj->setProperty("pan", track.pan);
        trackObj->setProperty("pitchSemitones", track.pitchSemitones);
        trackObj->setProperty("playbackSpeed", track.playbackSpeed);
        trackObj->setProperty("isMuted", track.isMuted);
        trackObj->setProperty("isSolo", track.isSolo);
        trackObj->setProperty("isArmed", track.isArmed);
        trackObj->setProperty("vst3Name", track.vst3Name);
        trackObj->setProperty("vst3DescriptionXml", track.vst3DescriptionXml);
        trackObj->setProperty("vst3AraArchiveId", track.vst3AraArchiveId);
        trackObj->setProperty("vst3AraHostFormatVersion",
                              track.vst3AraHostFormatVersion);
        trackObj->setProperty("vst3AraPlaybackEnabled", track.vst3AraPlaybackEnabled);
        trackObj->setProperty("vst3Bypassed", track.vst3Bypassed);
        trackObj->setProperty("effectChainBypassed", track.effectChainBypassed);
        trackObj->setProperty("vst3TailSeconds", track.vst3TailSeconds);

        juce::DynamicObject::Ptr pitchObject = new juce::DynamicObject();
        pitchObject->setProperty("enabled", track.pitchCorrection.enabled);
        pitchObject->setProperty("auditionCorrected",
                                 track.pitchCorrection.auditionCorrected);
        pitchObject->setProperty("key", track.pitchCorrection.key);
        pitchObject->setProperty("scale",
                                 static_cast<int>(track.pitchCorrection.scale));
        pitchObject->setProperty("strength", track.pitchCorrection.strength);
        trackObj->setProperty("pitchCorrection", juce::var(pitchObject.get()));

        juce::DynamicObject::Ptr mixObject = new juce::DynamicObject();
        mixObject->setProperty("enabled", track.simpleMix.enabled);
        mixObject->setProperty("preset", static_cast<int>(track.simpleMix.preset));
        mixObject->setProperty("brightness", track.simpleMix.brightness);
        mixObject->setProperty("ambience", track.simpleMix.ambience);
        mixObject->setProperty("stability", track.simpleMix.stability);
        trackObj->setProperty("simpleMix", juce::var(mixObject.get()));

        juce::DynamicObject::Ptr noiseObject = new juce::DynamicObject();
        noiseObject->setProperty("enabled", track.noiseReduction.enabled);
        noiseObject->setProperty("amount", track.noiseReduction.amount);
        noiseObject->setProperty("deEssAmount", track.noiseReduction.deEssAmount);
        trackObj->setProperty("noiseReduction", juce::var(noiseObject.get()));

        juce::Array<juce::var> specialFxArray;
        for (const auto& region : track.specialFxRegions)
        {
            juce::DynamicObject::Ptr regionObject = new juce::DynamicObject();
            regionObject->setProperty("id", region.id);
            regionObject->setProperty("type", static_cast<int>(region.type));
            regionObject->setProperty("startSeconds", region.startSeconds);
            regionObject->setProperty("endSeconds", region.endSeconds);
            regionObject->setProperty("amount", region.amount);
            regionObject->setProperty("wet", region.wet);
            regionObject->setProperty("fadeSeconds", region.fadeSeconds);
            regionObject->setProperty("enabled", region.enabled);
            specialFxArray.add(juce::var(regionObject.get()));
        }
        trackObj->setProperty("specialFxRegions", specialFxArray);
        juce::Array<juce::var> automationArray;
        for (const auto& region : track.automationRegions)
            automationArray.add(RegionAutomation::toVar(region));
        trackObj->setProperty("automationRegions", automationArray);

        if (!addBinaryBlockProperty(*trackObj,
                                    juce::Identifier("vst3State"),
                                    track.vst3State,
                                    binaryJobs,
                                    currentBinaryOffset)
            || !addBinaryBlockProperty(*trackObj,
                                       juce::Identifier("vst3AraArchive"),
                                       track.vst3AraArchive,
                                       binaryJobs,
                                       currentBinaryOffset))
            return false;

        juce::Array<juce::var> effectsArray;
        for (const auto& effect : track.effectSlots)
        {
            juce::DynamicObject::Ptr effectObject = new juce::DynamicObject();
            effectObject->setProperty("id", effect.id);
            effectObject->setProperty("name", effect.name);
            effectObject->setProperty("descriptionXml", effect.descriptionXml);
            effectObject->setProperty("bypassed", effect.bypassed);
            effectObject->setProperty("latencySamples", effect.latencySamples);
            effectObject->setProperty("tailSeconds", effect.tailSeconds);
            if (!addBinaryBlockProperty(*effectObject,
                                        juce::Identifier("state"),
                                        effect.state,
                                        binaryJobs,
                                        currentBinaryOffset))
                return false;
            effectsArray.add(juce::var(effectObject.get()));
        }
        trackObj->setProperty("effects", effectsArray);

        juce::Array<juce::var> clipsArray;
        for (const auto& clip : track.clips)
        {
            if (clip.buffer == nullptr) continue;

            const int numChannels = clip.buffer->getNumChannels();
            const int numSamples = clip.buffer->getNumSamples();
            const auto byteLength = static_cast<juce::int64>(numSamples)
                                  * static_cast<juce::int64>(sizeof(float))
                                  * static_cast<juce::int64>(numChannels);
            if (numChannels <= 0 || numSamples <= 0 || byteLength < 0)
                return false;

            juce::DynamicObject::Ptr audioObj = new juce::DynamicObject();
            audioObj->setProperty("sampleRate", clip.sampleRate);
            audioObj->setProperty("numberOfChannels", numChannels);
            audioObj->setProperty("length", numSamples);
            audioObj->setProperty("byteOffset", currentBinaryOffset);
            audioObj->setProperty("byteLength", byteLength);

            juce::DynamicObject::Ptr clipObj = new juce::DynamicObject();
            clipObj->setProperty("id", clip.id);
            clipObj->setProperty("name", clip.name);
            clipObj->setProperty("startTime", clip.startTime);
            clipObj->setProperty("offset", clip.offset);
            clipObj->setProperty("duration", clip.duration);
            clipObj->setProperty("fadeInSeconds", clip.fadeInSeconds);
            clipObj->setProperty("fadeOutSeconds", clip.fadeOutSeconds);
            juce::Array<juce::var> rhythmMarkers;
            for (const auto& marker : clip.rhythmMarkers)
            {
                juce::DynamicObject::Ptr markerObject = new juce::DynamicObject();
                markerObject->setProperty("sourceTime", marker.sourceTime);
                markerObject->setProperty("targetTime", marker.targetTime);
                markerObject->setProperty("sourceEndTime", marker.sourceEndTime);
                markerObject->setProperty("targetEndTime", marker.targetEndTime);
                rhythmMarkers.add(juce::var(markerObject.get()));
            }
            clipObj->setProperty("rhythmMarkers", rhythmMarkers);
            clipObj->setProperty("audio", juce::var(audioObj.get()));
            clipsArray.add(juce::var(clipObj.get()));

            for (int channel = 0; channel < numChannels; ++channel)
            {
                if (!appendBinaryJob(binaryJobs,
                                     currentBinaryOffset,
                                     clip.buffer->getReadPointer(channel),
                                     static_cast<size_t>(numSamples) * sizeof(float),
                                     BinaryWriteJob::Kind::floatSamples))
                    return false;
            }
        }
        trackObj->setProperty("clips", clipsArray);
        tracksArray.add(juce::var(trackObj.get()));
    }

    juce::DynamicObject::Ptr rootObj = new juce::DynamicObject();
    rootObj->setProperty("version", currentSchemaVersion);
    rootObj->setProperty("schemaVersion", currentSchemaVersion);
    rootObj->setProperty("project", juce::var(projectObj.get()));
    rootObj->setProperty("tracks", tracksArray);

    const juce::String jsonString = juce::JSON::toString(juce::var(rootObj.get()));
    const int jsonLength = jsonString.getNumBytesAsUTF8();
    if (jsonLength < 0)
        return false;
    const int paddingLength = (4 - (jsonLength % 4)) % 4;

    // Write beside the destination first. The previous project stays intact if
    // disk space runs out or any write fails midway.
    juce::TemporaryFile temporaryFile(file);
    {
        juce::FileOutputStream out(temporaryFile.getFile());
        if (out.failedToOpen()) return false;

        if (!out.write(projectSignatureV4, sizeof(projectSignatureV4)))
            return false;
        out.writeInt(jsonLength);
        if (!out.write(jsonString.toRawUTF8(), static_cast<size_t>(jsonLength)))
            return false;

        for (int i = 0; i < paddingLength; ++i)
            out.writeByte(0);

        for (const auto& job : binaryJobs)
        {
            if (job.byteCount == 0)
                continue;

            if (job.kind == BinaryWriteJob::Kind::rawBytes)
            {
                if (!out.write(job.data, job.byteCount))
                    return false;
                continue;
            }

#if JUCE_LITTLE_ENDIAN
            if (!out.write(job.data, job.byteCount))
                return false;
#else
            const auto* samples = static_cast<const float*>(job.data);
            const auto numSamples = job.byteCount / sizeof(float);
            for (size_t sample = 0; sample < numSamples; ++sample)
            {
                uint32_t bits = 0;
                std::memcpy(&bits, samples + sample, sizeof(bits));
                out.writeInt(juce::ByteOrder::swapIfBigEndian(bits));
            }
#endif
        }

        out.flush();
        if (out.getStatus().failed())
            return false;
    }

    return temporaryFile.overwriteTargetFileWithTemporary();
}

bool ProjectSerializer::loadProject(const juce::File& file, AudioEngine& audioEngine)
{
    LoadedProjectData loadedProject;
    if (!loadProject(file, loadedProject))
        return false;
    applyLoadedProject(std::move(loadedProject), audioEngine);
    return true;
}

bool ProjectSerializer::loadProject(const juce::File& file,
                                    LoadedProjectData& loadedProject,
                                    juce::String* errorMessage)
{
    if (errorMessage != nullptr)
        errorMessage->clear();

    juce::MemoryMappedFile mappedFile(file, juce::MemoryMappedFile::readOnly);
    if (!mappedFile.getSize()) return false;

    const auto* data = static_cast<const char*>(mappedFile.getData());
    const size_t dataSize = mappedFile.getSize();
    if (dataSize < 12) return false;

    const bool isV3 = std::memcmp(data, projectSignatureV3, sizeof(projectSignatureV3)) == 0;
    const bool isV4 = std::memcmp(data, projectSignatureV4, sizeof(projectSignatureV4)) == 0;
    if (!isV3 && !isV4)
        return false;

    uint32_t storedJsonLength = 0;
    std::memcpy(&storedJsonLength, data + 8, sizeof(storedJsonLength));
    const uint32_t jsonLength = juce::ByteOrder::swapIfBigEndian(storedJsonLength);
    if (jsonLength > static_cast<uint32_t>(std::numeric_limits<int>::max())
        || static_cast<juce::uint64>(jsonLength) > static_cast<juce::uint64>(dataSize - 12))
        return false;

    const juce::String jsonString = juce::String::fromUTF8(data + 12,
                                                           static_cast<int>(jsonLength));
    const juce::var parsed = juce::JSON::parse(jsonString);
    if (!parsed.isObject()) return false;

    int schemaVersion = 0;
    if (!readSchemaVersion(parsed, isV3, schemaVersion, errorMessage))
        return false;

    const int paddingLength = (4 - (jsonLength % 4)) % 4;
    const size_t binaryBaseOffset = 12 + static_cast<size_t>(jsonLength)
                                      + static_cast<size_t>(paddingLength);
    if (binaryBaseOffset > dataSize)
        return false;

    const auto tracksArray = parsed.getProperty("tracks", juce::var());
    if (!tracksArray.isArray()) return false;

    LoadedProjectData candidate;
    candidate.tracks.reserve(static_cast<size_t>(tracksArray.size()));

    for (int i = 0; i < tracksArray.size(); ++i)
    {
        const auto trackObj = tracksArray[i];
        if (!trackObj.isObject()) return false;

        TrackData newTrack;
        newTrack.id = trackObj.getProperty("id", juce::Uuid().toString()).toString();
        newTrack.name = trackObj.getProperty("name", "Track").toString();
        if (schemaVersion >= 7)
            newTrack.role = sanitiseTrackRole(
                static_cast<int>(trackObj.getProperty("role", 0)));
        newTrack.color = hexToColour(trackObj.getProperty("color", "#10b981").toString());
        newTrack.volume = juce::jlimit(0.0f, 1.0f,
            static_cast<float>(trackObj.getProperty("volume", 1.0f)));
        newTrack.pan = juce::jlimit(-1.0f, 1.0f,
            static_cast<float>(trackObj.getProperty("pan", 0.0f)));
        newTrack.pitchSemitones = juce::jlimit(
            -12.0,
            12.0,
            static_cast<double>(trackObj.getProperty("pitchSemitones", 0.0)));
        newTrack.playbackSpeed = juce::jlimit(
            0.75,
            1.5,
            static_cast<double>(trackObj.getProperty("playbackSpeed", 1.0)));
        newTrack.isMuted = trackObj.getProperty("isMuted", false);
        newTrack.isSolo = trackObj.getProperty("isSolo", false);
        newTrack.isArmed = trackObj.getProperty("isArmed", false);
        newTrack.vst3Name = trackObj.getProperty("vst3Name", {}).toString();
        newTrack.vst3DescriptionXml = trackObj.getProperty("vst3DescriptionXml", {}).toString();
        newTrack.vst3AraArchiveId = trackObj.getProperty("vst3AraArchiveId", {}).toString();
        newTrack.vst3AraHostFormatVersion = static_cast<int>(
            trackObj.getProperty("vst3AraHostFormatVersion", 1));
        newTrack.vst3AraPlaybackEnabled = trackObj.getProperty("vst3AraPlaybackEnabled", false);
        newTrack.vst3Bypassed = trackObj.getProperty("vst3Bypassed", false);
        newTrack.effectChainBypassed = trackObj.getProperty(
            "effectChainBypassed", false);
        newTrack.vst3TailSeconds = juce::jlimit(
            0.0,
            60.0,
            static_cast<double>(trackObj.getProperty("vst3TailSeconds", 0.0)));

        const auto pitchObject = trackObj.getProperty("pitchCorrection", juce::var());
        if (pitchObject.isObject())
        {
            newTrack.pitchCorrection.enabled = pitchObject.getProperty("enabled", false);
            newTrack.pitchCorrection.auditionCorrected =
                pitchObject.getProperty("auditionCorrected", true);
            newTrack.pitchCorrection.key = juce::jlimit(
                0, 11, static_cast<int>(pitchObject.getProperty("key", 0)));
            newTrack.pitchCorrection.scale = static_cast<MusicalScale>(
                juce::jlimit(0, 5,
                    static_cast<int>(pitchObject.getProperty("scale", 0))));
            newTrack.pitchCorrection.strength = juce::jlimit(
                0.0f, 1.0f,
                static_cast<float>(pitchObject.getProperty("strength", 0.60f)));
        }

        const auto mixObject = trackObj.getProperty("simpleMix", juce::var());
        if (mixObject.isObject())
        {
            newTrack.simpleMix.enabled = mixObject.getProperty("enabled", false);
            newTrack.simpleMix.preset = static_cast<MixPreset>(juce::jlimit(
                0, 4, static_cast<int>(mixObject.getProperty("preset", 0))));
            newTrack.simpleMix.brightness = juce::jlimit(
                -1.0f, 1.0f,
                static_cast<float>(mixObject.getProperty("brightness", 0.0f)));
            newTrack.simpleMix.ambience = juce::jlimit(
                0.0f, 1.0f,
                static_cast<float>(mixObject.getProperty("ambience", 0.0f)));
            newTrack.simpleMix.stability = juce::jlimit(
                0.0f, 1.0f,
                static_cast<float>(mixObject.getProperty("stability", 0.0f)));
        }

        const auto noiseObject = trackObj.getProperty("noiseReduction", juce::var());
        if (noiseObject.isObject())
        {
            newTrack.noiseReduction.enabled = noiseObject.getProperty("enabled", false);
            newTrack.noiseReduction.amount = juce::jlimit(
                0.0f, 1.0f,
                static_cast<float>(noiseObject.getProperty("amount", 0.0f)));
            newTrack.noiseReduction.deEssAmount = juce::jlimit(
                0.0f, 1.0f,
                static_cast<float>(noiseObject.getProperty("deEssAmount", 0.0f)));
        }

        const auto automationArray = trackObj.getProperty("automationRegions", juce::var());
        if (automationArray.isArray())
        {
            juce::StringArray regionIds;
            for (int i = 0; i < std::min(static_cast<int>(RegionAutomation::maximumRegions),
                                        automationArray.size()); ++i)
            {
                AudioAutomationRegion region;
                if (RegionAutomation::fromVar(automationArray[i], region)
                    && !regionIds.contains(region.id))
                {
                    regionIds.add(region.id);
                    newTrack.automationRegions.push_back(std::move(region));
                }
            }
        }

        const auto specialFxArray = trackObj.getProperty("specialFxRegions",
                                                         juce::var());
        if (specialFxArray.isArray())
        {
            constexpr int maximumSpecialFxRegions = 256;
            const int regionCount = std::min(maximumSpecialFxRegions,
                                             specialFxArray.size());
            newTrack.specialFxRegions.reserve(static_cast<size_t>(regionCount));
            for (int regionIndex = 0; regionIndex < regionCount; ++regionIndex)
            {
                const auto regionObject = specialFxArray[regionIndex];
                if (!regionObject.isObject())
                    continue;

                SpecialFxRegion region;
                region.id = regionObject.getProperty(
                    "id", "special-fx-" + juce::Uuid().toString()).toString();
                region.type = static_cast<SpecialFxType>(juce::jlimit(
                    static_cast<int>(SpecialFxType::none),
                    static_cast<int>(SpecialFxType::bitcrush),
                    static_cast<int>(regionObject.getProperty("type", 0))));
                region.startSeconds = juce::jlimit(
                    0.0, 24.0 * 60.0 * 60.0,
                    static_cast<double>(regionObject.getProperty(
                        "startSeconds", 0.0)));
                region.endSeconds = juce::jlimit(
                    region.startSeconds, 24.0 * 60.0 * 60.0,
                    static_cast<double>(regionObject.getProperty(
                        "endSeconds", region.startSeconds)));
                region.amount = juce::jlimit(
                    0.0f, 1.0f,
                    static_cast<float>(regionObject.getProperty("amount", 0.65f)));
                region.wet = juce::jlimit(
                    0.0f, 1.0f,
                    static_cast<float>(regionObject.getProperty("wet", 1.0f)));
                region.fadeSeconds = juce::jlimit(
                    0.0, 10.0,
                    static_cast<double>(regionObject.getProperty(
                        "fadeSeconds", 0.03)));
                region.enabled = regionObject.getProperty("enabled", true);
                if (region.type != SpecialFxType::none
                    && region.endSeconds > region.startSeconds + 1.0e-6)
                    newTrack.specialFxRegions.push_back(std::move(region));
            }
        }

        if (isV4)
        {
            if (!readBinaryBlock(trackObj.getProperty("vst3State", juce::var()),
                                 data,
                                 dataSize,
                                 binaryBaseOffset,
                                 newTrack.vst3State)
                || !readBinaryBlock(trackObj.getProperty("vst3AraArchive", juce::var()),
                                    data,
                                    dataSize,
                                    binaryBaseOffset,
                                    newTrack.vst3AraArchive))
                return false;
        }
        else
        {
            if (!decodeLegacyBase64(trackObj.getProperty("vst3StateBase64", juce::var()),
                                    newTrack.vst3State)
                || !decodeLegacyBase64(
                    trackObj.getProperty("vst3AraArchiveBase64", juce::var()),
                    newTrack.vst3AraArchive))
                return false;
        }

        const auto effectsArray = trackObj.getProperty("effects", juce::var());
        if (effectsArray.isArray())
        {
            newTrack.effectSlots.reserve(static_cast<size_t>(effectsArray.size()));
            for (int effectIndex = 0; effectIndex < effectsArray.size(); ++effectIndex)
            {
                const auto effectObject = effectsArray[effectIndex];
                if (!effectObject.isObject())
                    return false;

                EffectSlotData effect;
                effect.id = effectObject.getProperty(
                    "id", "effect-" + juce::Uuid().toString()).toString();
                effect.name = effectObject.getProperty("name", {}).toString();
                effect.descriptionXml = effectObject.getProperty(
                    "descriptionXml", {}).toString();
                effect.bypassed = effectObject.getProperty("bypassed", false);
                effect.latencySamples = juce::jlimit(
                    0, 1920000,
                    static_cast<int>(effectObject.getProperty("latencySamples", 0)));
                effect.tailSeconds = juce::jlimit(
                    0.0, 60.0,
                    static_cast<double>(effectObject.getProperty("tailSeconds", 0.0)));
                if (isV4
                    && !readBinaryBlock(effectObject.getProperty("state", juce::var()),
                                        data,
                                        dataSize,
                                        binaryBaseOffset,
                                        effect.state))
                    return false;
                if (effect.name.isNotEmpty()
                    || effect.descriptionXml.isNotEmpty()
                    || effect.state != nullptr)
                    newTrack.effectSlots.push_back(std::move(effect));
            }
        }

        // Projects created before the multi-slot chain stored their only
        // ordinary VST3 in the dedicated plug-in fields.  Move that metadata
        // into slot zero during parsing so the migration is complete even if
        // the plug-in is not installed on this machine.  PitchNet/ARA stays in
        // the dedicated correction slot.
        const bool legacyOrdinaryPlugin = newTrack.effectSlots.empty()
            && newTrack.vst3Name.isNotEmpty()
            && !newTrack.vst3Name.containsIgnoreCase("PitchNet")
            && !newTrack.vst3AraPlaybackEnabled
            && (newTrack.vst3AraArchive == nullptr
                || newTrack.vst3AraArchive->isEmpty());
        if (legacyOrdinaryPlugin)
        {
            EffectSlotData migrated;
            migrated.id = "effect-" + juce::Uuid().toString();
            migrated.name = newTrack.vst3Name;
            migrated.descriptionXml = newTrack.vst3DescriptionXml;
            migrated.state = newTrack.vst3State;
            migrated.bypassed = newTrack.vst3Bypassed;
            migrated.latencySamples = newTrack.vst3LatencySamples;
            migrated.tailSeconds = newTrack.vst3TailSeconds;
            newTrack.effectSlots.push_back(std::move(migrated));
            newTrack.vst3Name.clear();
            newTrack.vst3DescriptionXml.clear();
            newTrack.vst3State.reset();
            newTrack.vst3Bypassed = false;
            newTrack.vst3LatencySamples = 0;
            newTrack.vst3TailSeconds = 0.0;
        }

        const auto clipsArray = trackObj.getProperty("clips", juce::var());
        if (!clipsArray.isArray()) return false;

        for (int j = 0; j < clipsArray.size(); ++j)
        {
            const auto clipObj = clipsArray[j];
            if (!clipObj.isObject()) return false;
            const auto audioObj = clipObj.getProperty("audio", juce::var());
            if (!audioObj.isObject()) return false;

            const int numChannels = static_cast<int>(
                audioObj.getProperty("numberOfChannels", 1));
            const auto numSamples64 = static_cast<juce::int64>(
                audioObj.getProperty("length", 0));
            const auto byteOffset64 = static_cast<juce::int64>(
                audioObj.getProperty("byteOffset", 0));
            const int sampleRate = static_cast<int>(audioObj.getProperty("sampleRate", 44100));

            if (numChannels < 1 || numChannels > 8
                || numSamples64 <= 0 || numSamples64 > std::numeric_limits<int>::max()
                || byteOffset64 < 0 || sampleRate < 8000 || sampleRate > 384000)
                return false;

            const auto bytesNeeded = static_cast<juce::uint64>(numSamples64)
                                   * static_cast<juce::uint64>(numChannels)
                                   * sizeof(float);
            if (isV4)
            {
                const auto declaredByteLength = static_cast<juce::int64>(
                    audioObj.getProperty("byteLength", static_cast<juce::int64>(-1)));
                if (declaredByteLength < 0
                    || static_cast<juce::uint64>(declaredByteLength) != bytesNeeded)
                    return false;
            }

            const auto audioOffset = static_cast<juce::uint64>(binaryBaseOffset)
                                   + static_cast<juce::uint64>(byteOffset64);
            if (audioOffset > static_cast<juce::uint64>(dataSize)
                || bytesNeeded > static_cast<juce::uint64>(dataSize) - audioOffset)
                return false;

            const int numSamples = static_cast<int>(numSamples64);
            auto buffer = std::make_shared<juce::AudioBuffer<float>>(numChannels, numSamples);
            for (int channel = 0; channel < numChannels; ++channel)
            {
                const auto* source = reinterpret_cast<const unsigned char*>(data)
                                   + audioOffset
                                   + static_cast<juce::uint64>(channel)
                                         * static_cast<juce::uint64>(numSamples)
                                         * sizeof(float);
                auto* destination = buffer->getWritePointer(channel);
#if JUCE_LITTLE_ENDIAN
                std::memcpy(destination,
                            source,
                            static_cast<size_t>(numSamples) * sizeof(float));
#else
                for (int sample = 0; sample < numSamples; ++sample)
                {
                    uint32_t bits = 0;
                    std::memcpy(&bits,
                                source + static_cast<size_t>(sample) * sizeof(float),
                                sizeof(bits));
                    bits = juce::ByteOrder::swapIfBigEndian(bits);
                    std::memcpy(destination + sample, &bits, sizeof(bits));
                }
#endif
            }

            AudioClip clip;
            clip.id = clipObj.getProperty("id", juce::Uuid().toString()).toString();
            clip.name = clipObj.getProperty("name", "Audio").toString();
            clip.startTime = std::max(0.0,
                static_cast<double>(clipObj.getProperty("startTime", 0.0)));
            clip.offset = std::max(0.0,
                static_cast<double>(clipObj.getProperty("offset", 0.0)));
            clip.duration = static_cast<double>(clipObj.getProperty(
                "duration", static_cast<double>(numSamples) / sampleRate));
            clip.fadeInSeconds = std::max(0.0,
                static_cast<double>(clipObj.getProperty("fadeInSeconds", 0.0)));
            clip.fadeOutSeconds = std::max(0.0,
                static_cast<double>(clipObj.getProperty("fadeOutSeconds", 0.0)));
            clip.sampleRate = sampleRate;
            clip.buffer = std::move(buffer);

            const double availableDuration = static_cast<double>(numSamples) / sampleRate;
            if (clip.duration <= 0.0 || clip.offset > availableDuration + 1.0e-6
                || clip.offset + clip.duration > availableDuration + 1.0e-4)
                return false;

            clip.fadeInSeconds = std::min(clip.fadeInSeconds, clip.duration);
            clip.fadeOutSeconds = std::min(clip.fadeOutSeconds, clip.duration);
            const double fadeTotal = clip.fadeInSeconds + clip.fadeOutSeconds;
            if (fadeTotal > clip.duration && fadeTotal > 0.0)
            {
                const double scale = clip.duration / fadeTotal;
                clip.fadeInSeconds *= scale;
                clip.fadeOutSeconds *= scale;
            }

            const auto rhythmArray = clipObj.getProperty("rhythmMarkers", juce::var());
            if (rhythmArray.isArray())
            {
                for (int markerIndex = 0; markerIndex < rhythmArray.size(); ++markerIndex)
                {
                    const auto markerObject = rhythmArray[markerIndex];
                    if (!markerObject.isObject())
                        continue;
                    RhythmMarker marker;
                    marker.sourceTime = juce::jlimit(
                        0.0, clip.duration,
                        static_cast<double>(markerObject.getProperty("sourceTime", 0.0)));
                    marker.targetTime = juce::jlimit(
                        0.0, clip.duration,
                        static_cast<double>(markerObject.getProperty("targetTime", 0.0)));
                    const auto* markerProperties = markerObject.getDynamicObject();
                    marker.sourceEndTime = juce::jlimit(
                        marker.sourceTime, clip.duration,
                        static_cast<double>(markerObject.getProperty(
                            "sourceEndTime", marker.sourceTime)));
                    marker.targetEndTime = juce::jlimit(
                        marker.targetTime, clip.duration,
                        static_cast<double>(markerObject.getProperty(
                            "targetEndTime", marker.targetTime)));
                    if (markerProperties == nullptr
                        || !markerProperties->hasProperty("sourceEndTime")
                        || !markerProperties->hasProperty("targetEndTime"))
                    {
                        marker.sourceEndTime = marker.sourceTime;
                        marker.targetEndTime = marker.targetTime;
                    }
                    clip.rhythmMarkers.push_back(marker);
                }
            }
            clip.rhythmMarkers = RhythmWarp::sanitise(
                std::move(clip.rhythmMarkers), clip.duration);

            newTrack.clips.push_back(std::move(clip));
        }

        candidate.tracks.push_back(std::move(newTrack));
    }

    const auto projectObj = parsed.getProperty("project", juce::var());
    if (projectObj.isObject())
    {
        candidate.bpm = projectObj.getProperty("bpm", 120);
        candidate.timeSignatureNumerator = projectObj.getProperty(
            "timeSignatureNumerator", 4);
        candidate.timeSignatureDenominator = projectObj.getProperty(
            "timeSignatureDenominator", 4);
        candidate.zoomLevel = projectObj.getProperty("zoomLevel", 50.0);
        candidate.masterVolume = static_cast<float>(
            projectObj.getProperty("masterVolume", 0.8f));
        candidate.metronomeEnabled = projectObj.getProperty("metronomeEnabled", false);
        candidate.metronomeVolume = static_cast<float>(
            projectObj.getProperty("metronomeVolume", 0.5f));
        candidate.countInEnabled = projectObj.getProperty("countInEnabled", false);

        auto& daw = candidate.dawSettings;
        daw.musicalKey = juce::jlimit(
            0, 11, static_cast<int>(projectObj.getProperty("musicalKey", 0)));
        daw.musicalScale = static_cast<MusicalScale>(juce::jlimit(
            0, 5, static_cast<int>(projectObj.getProperty("musicalScale", 0))));
        daw.pitchCorrectionStrength = juce::jlimit(
            0.0f, 1.0f,
            static_cast<float>(projectObj.getProperty(
                "pitchCorrectionStrength", 0.60f)));
        daw.hasLoopStart = projectObj.getProperty("hasLoopStart", false);
        daw.hasLoopEnd = projectObj.getProperty("hasLoopEnd", false);
        daw.loopStartSeconds = std::max(0.0,
            static_cast<double>(projectObj.getProperty("loopStartSeconds", 0.0)));
        daw.loopEndSeconds = std::max(0.0,
            static_cast<double>(projectObj.getProperty("loopEndSeconds", 0.0)));
        if (!daw.hasLoopStart || !daw.hasLoopEnd
            || daw.loopEndSeconds <= daw.loopStartSeconds)
        {
            daw.loopEnabled = false;
            if (daw.loopEndSeconds <= daw.loopStartSeconds)
            {
                daw.hasLoopStart = false;
                daw.hasLoopEnd = false;
            }
        }
        else
        {
            daw.loopEnabled = projectObj.getProperty("loopEnabled", false);
        }

        const auto tempoPoints = projectObj.getProperty("tempoMap", juce::var());
        if (tempoPoints.isArray())
        {
            daw.tempoMap.clear();
            for (int pointIndex = 0; pointIndex < tempoPoints.size(); ++pointIndex)
            {
                const auto pointObject = tempoPoints[pointIndex];
                if (!pointObject.isObject())
                    continue;
                TempoPoint point;
                point.timeSeconds = std::max(0.0,
                    static_cast<double>(pointObject.getProperty("timeSeconds", 0.0)));
                point.bpm = juce::jlimit(20.0, 300.0,
                    static_cast<double>(pointObject.getProperty("bpm", 120.0)));
                point.numerator = juce::jlimit(1, 12,
                    static_cast<int>(pointObject.getProperty("numerator", 4)));
                const int denominator = static_cast<int>(
                    pointObject.getProperty("denominator", 4));
                point.denominator = denominator == 8 ? 8 : 4;
                daw.tempoMap.push_back(point);
            }
        }
        if (daw.tempoMap.empty())
            daw.tempoMap.push_back({ 0.0,
                                     static_cast<double>(candidate.bpm),
                                     candidate.timeSignatureNumerator,
                                     candidate.timeSignatureDenominator });
        std::stable_sort(daw.tempoMap.begin(), daw.tempoMap.end(),
            [](const TempoPoint& left, const TempoPoint& right)
            {
                return left.timeSeconds < right.timeSeconds;
            });
        if (daw.tempoMap.front().timeSeconds > 1.0e-9)
            daw.tempoMap.insert(daw.tempoMap.begin(),
                TempoPoint { 0.0,
                             static_cast<double>(candidate.bpm),
                             candidate.timeSignatureNumerator,
                             candidate.timeSignatureDenominator });

        const auto exportObject = projectObj.getProperty("exportDefaults", juce::var());
        if (exportObject.isObject())
        {
            auto& options = daw.exportDefaults;
            options.format = static_cast<ExportFormat>(juce::jlimit(
                0, 1, static_cast<int>(exportObject.getProperty("format", 0))));
            options.channels = static_cast<ExportChannels>(
                static_cast<int>(exportObject.getProperty("channels", 2)) == 1 ? 1 : 2);
            options.wavBits = static_cast<int>(
                exportObject.getProperty("wavBits", 16)) == 24 ? 24 : 16;
            options.mp3Kbps = 192;
            options.sampleRate = static_cast<int>(
                exportObject.getProperty("sampleRate", 44100)) == 48000
                    ? 48000 : 44100;
            options.target = static_cast<ExportTarget>(juce::jlimit(
                0, 2, static_cast<int>(exportObject.getProperty("target", 0))));
            options.trackIndex = static_cast<int>(
                exportObject.getProperty("trackIndex", -1));
            options.rangeStartSeconds = std::max(
                0.0,
                static_cast<double>(exportObject.getProperty(
                    "rangeStartSeconds", 0.0)));
            options.rangeEndSeconds = std::max(
                0.0,
                static_cast<double>(exportObject.getProperty(
                    "rangeEndSeconds", 0.0)));
        }

        const auto masteringObject = projectObj.getProperty("mastering", juce::var());
        if (masteringObject.isObject())
        {
            auto& mastering = daw.mastering;
            mastering.enabled = masteringObject.getProperty("enabled", false);
            mastering.limiterEnabled = masteringObject.getProperty(
                "limiterEnabled", true);
            mastering.ceilingDb = juce::jlimit(
                -12.0f, 0.0f,
                static_cast<float>(masteringObject.getProperty("ceilingDb", -1.0f)));
            mastering.targetLufs = juce::jlimit(
                -24.0f, -8.0f,
                static_cast<float>(masteringObject.getProperty("targetLufs", -14.0f)));
            mastering.auditionProcessed = masteringObject.getProperty(
                "auditionProcessed", true);
        }
    }

    loadedProject = std::move(candidate);
    return true;
}

void ProjectSerializer::applyLoadedProject(LoadedProjectData loadedProject,
                                           AudioEngine& audioEngine)
{
    audioEngine.getMetronome().setBpm(loadedProject.bpm);
    audioEngine.getMetronome().setTimeSignature(loadedProject.timeSignatureNumerator,
                                                loadedProject.timeSignatureDenominator);
    audioEngine.setZoomLevel(loadedProject.zoomLevel);
    audioEngine.setMasterVolume(loadedProject.masterVolume);
    audioEngine.getMetronome().setEnabled(loadedProject.metronomeEnabled);
    audioEngine.getMetronome().setVolume(loadedProject.metronomeVolume);
    audioEngine.setCountInEnabled(loadedProject.countInEnabled);
    audioEngine.setProjectDawSettings(loadedProject.dawSettings, false);
    audioEngine.replaceTracks(std::move(loadedProject.tracks));
}
