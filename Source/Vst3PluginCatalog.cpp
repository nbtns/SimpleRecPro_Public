#include "Vst3PluginCatalog.h"

#include <algorithm>
#include <functional>

namespace
{
constexpr int stateSchemaVersion = 1;
constexpr int maximumScanDepth = 24;

void addUniqueRoot(std::vector<juce::File>& roots, const juce::File& candidate)
{
    if (candidate == juce::File())
        return;

    const auto path = candidate.getFullPathName();
    const auto alreadyPresent = std::any_of(
        roots.begin(), roots.end(), [&path](const juce::File& existing)
        {
#if JUCE_WINDOWS
            return existing.getFullPathName().equalsIgnoreCase(path);
#else
            return existing.getFullPathName() == path;
#endif
        });

    if (!alreadyPresent)
        roots.push_back(candidate);
}

bool isFiniteTimestamp(juce::int64 value) noexcept
{
    return value >= 0;
}
}

std::vector<juce::File> Vst3PluginCatalog::getStandardSearchLocations()
{
    std::vector<juce::File> result;

#if JUCE_WINDOWS
    const auto programFiles = juce::SystemStats::getEnvironmentVariable(
        "ProgramFiles", "C:\\Program Files");
    if (programFiles.isNotEmpty())
        addUniqueRoot(result,
                      juce::File(programFiles).getChildFile("Common Files")
                                               .getChildFile("VST3"));

    const auto programFilesX86 = juce::SystemStats::getEnvironmentVariable(
        "ProgramFiles(x86)", {});
    if (programFilesX86.isNotEmpty())
        addUniqueRoot(result,
                      juce::File(programFilesX86).getChildFile("Common Files")
                                                  .getChildFile("VST3"));

    const auto localAppData = juce::SystemStats::getEnvironmentVariable(
        "LOCALAPPDATA", {});
    if (localAppData.isNotEmpty())
        addUniqueRoot(result,
                      juce::File(localAppData).getChildFile("Programs")
                                                   .getChildFile("Common")
                                                   .getChildFile("VST3"));
#elif JUCE_MAC
    addUniqueRoot(result, juce::File("/Library/Audio/Plug-Ins/VST3"));
    addUniqueRoot(result,
                  juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                      .getChildFile("Library")
                      .getChildFile("Audio")
                      .getChildFile("Plug-Ins")
                      .getChildFile("VST3"));
#endif

    return result;
}

void Vst3PluginCatalog::rebuild(const std::vector<juce::File>& additionalRoots)
{
    auto roots = getStandardSearchLocations();
    for (const auto& root : additionalRoots)
        addUniqueRoot(roots, root);

    std::vector<juce::File> discovered;
    juce::StringArray visitedDirectories;

    std::function<void(const juce::File&, int)> scanDirectory;
    scanDirectory = [&](const juce::File& directory, int depth)
    {
        if (depth > maximumScanDepth || !directory.isDirectory())
            return;

        const auto directoryKey = pathKey(directory);
        if (visitedDirectories.contains(directoryKey))
            return;
        visitedDirectories.add(directoryKey);

        juce::DirectoryIterator iterator(directory,
                                         false,
                                         "*",
                                         juce::File::findFilesAndDirectories);
        while (iterator.next())
        {
            const auto child = iterator.getFile();
            if (isVst3Bundle(child))
            {
                const auto childKey = pathKey(child);
                const auto duplicate = std::any_of(
                    discovered.begin(), discovered.end(),
                    [&childKey](const juce::File& existing)
                    {
                        return pathKey(existing) == childKey;
                    });
                if (!duplicate)
                    discovered.push_back(child);
                continue;
            }

            // A symbolic-link directory can point back to an ancestor. VST3
            // bundles themselves were already accepted above, so links can be
            // skipped without opening any plug-in binary.
            if (child.isDirectory() && !child.isSymbolicLink())
                scanDirectory(child, depth + 1);
        }
    };

    for (const auto& root : roots)
        scanDirectory(root, 0);

    std::vector<Entry> rebuilt;
    rebuilt.reserve(discovered.size());
    for (const auto& bundle : discovered)
    {
        Entry entry;
        entry.bundle = bundle;
        entry.name = displayNameFor(bundle);

        if (const auto* existing = findEntry(bundle))
        {
            entry.isFavourite = existing->isFavourite;
            entry.lastUsedUnixMilliseconds = existing->lastUsedUnixMilliseconds;
            entry.quarantineReason = existing->quarantineReason;
        }

        rebuilt.push_back(std::move(entry));
    }

    entries = std::move(rebuilt);
    sortEntries();
}

std::vector<Vst3PluginCatalog::Entry> Vst3PluginCatalog::searchByName(
    const juce::String& query) const
{
    const auto needle = query.trim();
    if (needle.isEmpty())
        return entries;

    std::vector<Entry> result;
    for (const auto& entry : entries)
        if (entry.name.containsIgnoreCase(needle))
            result.push_back(entry);
    return result;
}

std::vector<Vst3PluginCatalog::Entry> Vst3PluginCatalog::getFavourites() const
{
    std::vector<Entry> result;
    for (const auto& entry : entries)
        if (entry.isFavourite)
            result.push_back(entry);
    return result;
}

std::vector<Vst3PluginCatalog::Entry> Vst3PluginCatalog::getRecent(
    std::size_t maximumEntries) const
{
    std::vector<Entry> result;
    for (const auto& entry : entries)
        if (entry.lastUsedUnixMilliseconds > 0)
            result.push_back(entry);

    std::stable_sort(result.begin(), result.end(),
        [](const Entry& left, const Entry& right)
        {
            if (left.lastUsedUnixMilliseconds != right.lastUsedUnixMilliseconds)
                return left.lastUsedUnixMilliseconds > right.lastUsedUnixMilliseconds;
            return left.name.compareIgnoreCase(right.name) < 0;
        });

    if (result.size() > maximumEntries)
        result.resize(maximumEntries);
    return result;
}

std::vector<Vst3PluginCatalog::Entry> Vst3PluginCatalog::getQuarantined() const
{
    std::vector<Entry> result;
    for (const auto& entry : entries)
        if (entry.isQuarantined())
            result.push_back(entry);
    return result;
}

void Vst3PluginCatalog::setFavourite(const juce::File& bundle,
                                     bool shouldBeFavourite)
{
    if (!isVst3Bundle(bundle))
        return;
    findOrCreateEntry(bundle).isFavourite = shouldBeFavourite;
    sortEntries();
}

bool Vst3PluginCatalog::isFavourite(const juce::File& bundle) const
{
    if (const auto* entry = findEntry(bundle))
        return entry->isFavourite;
    return false;
}

void Vst3PluginCatalog::markRecentlyUsed(const juce::File& bundle,
                                         juce::Time when)
{
    if (!isVst3Bundle(bundle))
        return;
    findOrCreateEntry(bundle).lastUsedUnixMilliseconds =
        std::max<juce::int64>(0, when.toMilliseconds());
}

void Vst3PluginCatalog::quarantine(const juce::File& bundle,
                                   const juce::String& reason)
{
    if (!isVst3Bundle(bundle))
        return;
    findOrCreateEntry(bundle).quarantineReason = reason.trim().isNotEmpty()
        ? reason.trim()
        : "Unknown validation failure";
    sortEntries();
}

void Vst3PluginCatalog::clearQuarantine(const juce::File& bundle)
{
    if (auto* entry = findEntry(bundle))
        entry->quarantineReason.clear();
    sortEntries();
}

bool Vst3PluginCatalog::saveState(const juce::File& stateFile) const
{
    juce::Array<juce::var> serializedEntries;
    for (const auto& entry : entries)
    {
        juce::DynamicObject::Ptr object = new juce::DynamicObject();
        object->setProperty("path", entry.bundle.getFullPathName());
        object->setProperty("name", entry.name);
        object->setProperty("favourite", entry.isFavourite);
        object->setProperty("lastUsedUnixMilliseconds",
                            entry.lastUsedUnixMilliseconds);
        object->setProperty("quarantineReason", entry.quarantineReason);
        serializedEntries.add(juce::var(object.get()));
    }

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("schemaVersion", stateSchemaVersion);
    root->setProperty("entries", serializedEntries);
    const auto json = juce::JSON::toString(juce::var(root.get()), true);

    const auto parent = stateFile.getParentDirectory();
    if (parent != juce::File() && !parent.isDirectory()
        && !parent.createDirectory())
        return false;

    juce::TemporaryFile temporaryFile(stateFile);
    {
        juce::FileOutputStream output(temporaryFile.getFile());
        if (output.failedToOpen())
            return false;
        if (!output.writeText(json, false, false, "\n"))
            return false;
        output.flush();
        if (output.getStatus().failed())
            return false;
    }

    return temporaryFile.overwriteTargetFileWithTemporary();
}

bool Vst3PluginCatalog::loadState(const juce::File& stateFile)
{
    clear();
    if (!stateFile.existsAsFile())
        return false;

    const auto parsed = juce::JSON::parse(stateFile.loadFileAsString());
    if (!parsed.isObject()
        || static_cast<int>(parsed.getProperty("schemaVersion", 0))
               != stateSchemaVersion)
        return false;

    const auto serializedEntries = parsed.getProperty("entries", juce::var());
    if (!serializedEntries.isArray())
        return false;

    std::vector<Entry> loaded;
    loaded.reserve(static_cast<std::size_t>(serializedEntries.size()));
    for (int index = 0; index < serializedEntries.size(); ++index)
    {
        const auto item = serializedEntries[index];
        if (!item.isObject())
        {
            clear();
            return false;
        }

        const juce::File bundle(item.getProperty("path", {}).toString());
        const auto name = item.getProperty("name", {}).toString().trim();
        const auto lastUsed = static_cast<juce::int64>(
            item.getProperty("lastUsedUnixMilliseconds", static_cast<juce::int64>(0)));
        if (!isVst3Bundle(bundle) || name.isEmpty() || !isFiniteTimestamp(lastUsed))
        {
            clear();
            return false;
        }

        Entry entry;
        entry.bundle = bundle;
        entry.name = name;
        entry.isFavourite = item.getProperty("favourite", false);
        entry.lastUsedUnixMilliseconds = lastUsed;
        entry.quarantineReason = item.getProperty("quarantineReason", {})
                                     .toString()
                                     .trim();

        const auto key = pathKey(bundle);
        const auto duplicate = std::any_of(
            loaded.begin(), loaded.end(), [&key](const Entry& existing)
            {
                return pathKey(existing.bundle) == key;
            });
        if (!duplicate)
            loaded.push_back(std::move(entry));
    }

    entries = std::move(loaded);
    sortEntries();
    return true;
}

Vst3PluginCatalog::Entry* Vst3PluginCatalog::findEntry(
    const juce::File& bundle)
{
    const auto key = pathKey(bundle);
    const auto iterator = std::find_if(entries.begin(), entries.end(),
        [&key](const Entry& entry)
        {
            return pathKey(entry.bundle) == key;
        });
    return iterator != entries.end() ? &*iterator : nullptr;
}

const Vst3PluginCatalog::Entry* Vst3PluginCatalog::findEntry(
    const juce::File& bundle) const
{
    const auto key = pathKey(bundle);
    const auto iterator = std::find_if(entries.begin(), entries.end(),
        [&key](const Entry& entry)
        {
            return pathKey(entry.bundle) == key;
        });
    return iterator != entries.end() ? &*iterator : nullptr;
}

Vst3PluginCatalog::Entry& Vst3PluginCatalog::findOrCreateEntry(
    const juce::File& bundle)
{
    if (auto* existing = findEntry(bundle))
        return *existing;

    entries.push_back({ bundle, displayNameFor(bundle), false, 0, {} });
    return entries.back();
}

void Vst3PluginCatalog::sortEntries()
{
    std::stable_sort(entries.begin(), entries.end(),
        [](const Entry& left, const Entry& right)
        {
            if (left.isQuarantined() != right.isQuarantined())
                return !left.isQuarantined();
            if (left.isFavourite != right.isFavourite)
                return left.isFavourite;
            const int byName = left.name.compareIgnoreCase(right.name);
            if (byName != 0)
                return byName < 0;
            return pathKey(left.bundle) < pathKey(right.bundle);
        });
}

bool Vst3PluginCatalog::isVst3Bundle(const juce::File& file)
{
    return file.getFileExtension().equalsIgnoreCase(".vst3");
}

juce::String Vst3PluginCatalog::pathKey(const juce::File& file)
{
    auto key = file.getFullPathName().replaceCharacter('\\', '/');
#if JUCE_WINDOWS
    key = key.toLowerCase();
#endif
    return key;
}

juce::String Vst3PluginCatalog::displayNameFor(const juce::File& file)
{
    const auto name = file.getFileNameWithoutExtension().trim();
    return name.isNotEmpty() ? name : file.getFileName();
}

