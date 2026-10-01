#pragma once

#include <JuceHeader.h>

#include <cstddef>
#include <vector>

/**
 * Builds a lightweight VST3 catalogue without opening or instantiating any
 * plug-in binary. Only bundle/file paths and display names are inspected.
 */
class Vst3PluginCatalog
{
public:
    struct Entry
    {
        juce::File bundle;
        juce::String name;
        bool isFavourite = false;
        juce::int64 lastUsedUnixMilliseconds = 0;
        juce::String quarantineReason;

        bool isQuarantined() const noexcept
        {
            return quarantineReason.isNotEmpty();
        }
    };

    /** Returns the conventional VST3 locations for Windows or macOS. */
    static std::vector<juce::File> getStandardSearchLocations();

    /**
     * Rebuilds the catalogue from the standard locations and any additional
     * roots. Existing favourite/recent/quarantine metadata is retained for
     * bundles that are found again.
     */
    void rebuild(const std::vector<juce::File>& additionalRoots = {});

    const std::vector<Entry>& getEntries() const noexcept { return entries; }
    std::vector<Entry> searchByName(const juce::String& query) const;
    std::vector<Entry> getFavourites() const;
    std::vector<Entry> getRecent(std::size_t maximumEntries = 10) const;
    std::vector<Entry> getQuarantined() const;

    void setFavourite(const juce::File& bundle, bool shouldBeFavourite);
    bool isFavourite(const juce::File& bundle) const;

    void markRecentlyUsed(
        const juce::File& bundle,
        juce::Time when = juce::Time::getCurrentTime());

    void quarantine(const juce::File& bundle, const juce::String& reason);
    void clearQuarantine(const juce::File& bundle);

    /** Atomically stores the catalogue metadata as JSON. */
    bool saveState(const juce::File& stateFile) const;

    /**
     * Loads JSON state without touching any plug-in binary. Invalid or corrupt
     * JSON clears the catalogue and returns false.
     */
    bool loadState(const juce::File& stateFile);

    void clear() noexcept { entries.clear(); }

private:
    Entry* findEntry(const juce::File& bundle);
    const Entry* findEntry(const juce::File& bundle) const;
    Entry& findOrCreateEntry(const juce::File& bundle);
    void sortEntries();

    static bool isVst3Bundle(const juce::File& file);
    static juce::String pathKey(const juce::File& file);
    static juce::String displayNameFor(const juce::File& file);

    std::vector<Entry> entries;
};

