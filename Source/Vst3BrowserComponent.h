#pragma once

#include <JuceHeader.h>

#include "DawFeatureTypes.h"
#include "Vst3PluginCatalog.h"

#include <functional>
#include <memory>
#include <vector>

/**
 * VST3の検索・お気に入り管理と、選択トラックのエフェクト順を編集する画面。
 *
 * カタログの表示とお気に入り切替だけをこの部品が担当する。プラグインの
 * 読み込みやエフェクトチェーン変更は、公開コールバックを通して呼び出し側が行う。
 */
class Vst3BrowserComponent : public juce::Component
{
public:
    enum class Filter
    {
        all = 0,
        favourites,
        recent,
        quarantined
    };

    explicit Vst3BrowserComponent(Vst3PluginCatalog& pluginCatalog);
    ~Vst3BrowserComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void setCatalog(Vst3PluginCatalog& pluginCatalog);
    void refreshCatalogView();

    void setTracks(const juce::StringArray& trackNames, int selectedTrackIndex);
    void setSelectedTrackIndex(int trackIndex);
    int getSelectedTrackIndex() const noexcept;

    void setEffectSlots(const std::vector<EffectSlotData>& slots);
    const std::vector<EffectSlotData>& getEffectSlots() const noexcept
    {
        return effectSlots;
    }

    void setFilter(Filter newFilter);
    Filter getFilter() const noexcept { return activeFilter; }
    void setStatusMessage(const juce::String& message, bool isError = false);

    std::function<void(int)> onSelectedTrackChanged;
    std::function<void()> onRequestCatalogRebuild;
    std::function<void(int, const juce::File&)> onAddPlugin;
    std::function<void(int, int)> onRemoveSlot;
    std::function<void(int, int, int)> onMoveSlot;
    std::function<void(int, int, bool)> onSetSlotBypassed;
    std::function<void(int, int)> onOpenSlotEditor;
    std::function<void(const juce::File&, bool)> onFavouriteChanged;
    std::function<void()> onClose;

private:
    class PluginListModel;
    class SlotListModel;

    friend class PluginListModel;
    friend class SlotListModel;

    void configureControls();
    void configureCallbacks();
    void rebuildFilteredEntries();
    void updateSelectedPluginDetails();
    void updateSlotButtons();
    void requestAddSelectedPlugin();
    void toggleSelectedFavourite();
    void selectFilterButton();

    const Vst3PluginCatalog::Entry* getSelectedPlugin() const noexcept;
    int getSelectedPluginRow() const noexcept;
    int getSelectedSlotRow() const noexcept;

    Vst3PluginCatalog* catalog = nullptr;
    std::vector<Vst3PluginCatalog::Entry> filteredEntries;
    std::vector<EffectSlotData> effectSlots;
    juce::StringArray availableTrackNames;
    Filter activeFilter = Filter::all;
    bool updatingControls = false;

    std::unique_ptr<PluginListModel> pluginListModel;
    std::unique_ptr<SlotListModel> slotListModel;

    juce::Label titleLabel;
    juce::Label trackLabel;
    juce::ComboBox trackBox;
    juce::TextButton rebuildButton;
    juce::TextButton closeButton;

    juce::TextEditor searchEditor;
    juce::TextButton allButton;
    juce::TextButton favouritesButton;
    juce::TextButton recentButton;
    juce::TextButton quarantinedButton;

    juce::Label pluginListLabel;
    juce::ListBox pluginList;
    juce::Label pluginDetailsLabel;
    juce::TextButton favouriteButton;
    juce::TextButton addButton;

    juce::Label slotListLabel;
    juce::ListBox slotList;
    juce::TextButton moveUpButton;
    juce::TextButton moveDownButton;
    juce::TextButton bypassButton;
    juce::TextButton editorButton;
    juce::TextButton removeButton;

    juce::Label statusLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Vst3BrowserComponent)
};

