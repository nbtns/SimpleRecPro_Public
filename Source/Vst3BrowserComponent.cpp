#include "Vst3BrowserComponent.h"

#include "UiTheme.h"

#include <algorithm>

namespace
{
juce::String uiText(const char* text)
{
    return juce::String::fromUTF8(text);
}

void styleLabel(juce::Label& label, float size = 12.0f, bool bold = false)
{
    label.setColour(juce::Label::textColourId,
                    bold ? UiTheme::textPrimary : UiTheme::textSecondary);
    label.setFont(juce::Font(juce::FontOptions(size,
        bold ? juce::Font::bold : juce::Font::plain)));
    label.setJustificationType(juce::Justification::centredLeft);
}

void styleCombo(juce::ComboBox& box)
{
    box.setColour(juce::ComboBox::backgroundColourId, UiTheme::controlSurface);
    box.setColour(juce::ComboBox::outlineColourId, UiTheme::border);
    box.setColour(juce::ComboBox::textColourId, UiTheme::textPrimary);
    box.setColour(juce::ComboBox::arrowColourId, UiTheme::textSecondary);
}

void styleList(juce::ListBox& list)
{
    list.setColour(juce::ListBox::backgroundColourId, UiTheme::panelBackground);
    list.setColour(juce::ListBox::outlineColourId, UiTheme::border);
    list.setColour(juce::ListBox::textColourId, UiTheme::textPrimary);
    list.setOutlineThickness(1);
    list.setRowHeight(36);
    list.setMultipleSelectionEnabled(false);
}

juce::Colour rowBackground(bool selected, int row)
{
    if (selected)
        return UiTheme::accentSoft;
    return row % 2 == 0 ? UiTheme::panelBackground : UiTheme::raisedSurface;
}
}

class Vst3BrowserComponent::PluginListModel final : public juce::ListBoxModel
{
public:
    explicit PluginListModel(Vst3BrowserComponent& ownerToUse)
        : owner(ownerToUse)
    {
    }

    int getNumRows() override
    {
        return static_cast<int>(owner.filteredEntries.size());
    }

    void paintListBoxItem(int rowNumber, juce::Graphics& g,
                          int width, int height, bool rowIsSelected) override
    {
        if (!juce::isPositiveAndBelow(rowNumber,
                                     static_cast<int>(owner.filteredEntries.size())))
            return;

        const auto& entry = owner.filteredEntries[static_cast<size_t>(rowNumber)];
        g.fillAll(rowBackground(rowIsSelected, rowNumber));

        const int iconWidth = 28;
        g.setColour(entry.isFavourite ? UiTheme::warning : UiTheme::textMuted);
        g.setFont(16.0f);
        g.drawText(entry.isFavourite ? juce::String::fromUTF8(u8"★")
                                     : juce::String::fromUTF8(u8"☆"),
                   5, 0, iconWidth, height, juce::Justification::centred);

        g.setColour(entry.isQuarantined() ? UiTheme::danger : UiTheme::textPrimary);
        g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        g.drawText(entry.name, iconWidth + 8, 2,
                   juce::jmax(0, width - iconWidth - 88), height - 4,
                   juce::Justification::centredLeft, true);

        if (entry.isQuarantined())
        {
            g.setColour(UiTheme::danger);
            g.setFont(10.0f);
            g.drawText(uiText(u8"隔離"), width - 64, 0, 52, height,
                       juce::Justification::centredRight);
        }
        else if (entry.lastUsedUnixMilliseconds > 0)
        {
            g.setColour(UiTheme::textMuted);
            g.setFont(10.0f);
            g.drawText(uiText(u8"最近"), width - 64, 0, 52, height,
                       juce::Justification::centredRight);
        }
    }

    void selectedRowsChanged(int) override
    {
        owner.updateSelectedPluginDetails();
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
    {
        owner.pluginList.selectRow(row);
        owner.requestAddSelectedPlugin();
    }

    void returnKeyPressed(int row) override
    {
        owner.pluginList.selectRow(row);
        owner.requestAddSelectedPlugin();
    }

private:
    Vst3BrowserComponent& owner;
};

class Vst3BrowserComponent::SlotListModel final : public juce::ListBoxModel
{
public:
    explicit SlotListModel(Vst3BrowserComponent& ownerToUse)
        : owner(ownerToUse)
    {
    }

    int getNumRows() override
    {
        return static_cast<int>(owner.effectSlots.size());
    }

    void paintListBoxItem(int rowNumber, juce::Graphics& g,
                          int width, int height, bool rowIsSelected) override
    {
        if (!juce::isPositiveAndBelow(rowNumber,
                                     static_cast<int>(owner.effectSlots.size())))
            return;

        const auto& slot = owner.effectSlots[static_cast<size_t>(rowNumber)];
        g.fillAll(rowBackground(rowIsSelected, rowNumber));

        g.setColour(UiTheme::textMuted);
        g.setFont(11.0f);
        g.drawText(juce::String(rowNumber + 1), 6, 0, 24, height,
                   juce::Justification::centred);

        g.setColour(slot.bypassed ? UiTheme::textMuted : UiTheme::textPrimary);
        g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        g.drawText(slot.name, 36, 2, juce::jmax(0, width - 150), height - 4,
                   juce::Justification::centredLeft, true);

        juce::String stateText;
        juce::Colour stateColour = UiTheme::success;
        if (slot.bypassed)
        {
            stateText = uiText(u8"バイパス");
            stateColour = UiTheme::warning;
        }
        else if (slot.latencySamples > 0)
        {
            stateText = juce::String(slot.latencySamples) + " samples";
            stateColour = UiTheme::textSecondary;
        }
        else
        {
            stateText = "ON";
        }
        g.setColour(stateColour);
        g.setFont(10.0f);
        g.drawText(stateText, width - 108, 0, 96, height,
                   juce::Justification::centredRight, true);
    }

    void selectedRowsChanged(int) override
    {
        owner.updateSlotButtons();
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
    {
        owner.slotList.selectRow(row);
        if (owner.onOpenSlotEditor)
            owner.onOpenSlotEditor(owner.getSelectedTrackIndex(), row);
    }

    void returnKeyPressed(int row) override
    {
        owner.slotList.selectRow(row);
        if (owner.onOpenSlotEditor)
            owner.onOpenSlotEditor(owner.getSelectedTrackIndex(), row);
    }

private:
    Vst3BrowserComponent& owner;
};

Vst3BrowserComponent::Vst3BrowserComponent(Vst3PluginCatalog& pluginCatalog)
    : catalog(&pluginCatalog),
      pluginListModel(std::make_unique<PluginListModel>(*this)),
      slotListModel(std::make_unique<SlotListModel>(*this)),
      pluginList("VST3 results", pluginListModel.get()),
      slotList("Effect slots", slotListModel.get())
{
    setSize(800, 640);
    configureControls();
    configureCallbacks();
    refreshCatalogView();
    resized();
}

Vst3BrowserComponent::~Vst3BrowserComponent()
{
    pluginList.setModel(nullptr);
    slotList.setModel(nullptr);
}

void Vst3BrowserComponent::configureControls()
{
    titleLabel.setText(uiText(u8"VST3ブラウザー"), juce::dontSendNotification);
    titleLabel.setColour(juce::Label::textColourId, UiTheme::textPrimary);
    titleLabel.setFont(juce::Font(juce::FontOptions(21.0f, juce::Font::bold)));
    trackLabel.setText(uiText(u8"追加先"), juce::dontSendNotification);
    styleLabel(trackLabel, 12.0f, true);
    styleCombo(trackBox);

    rebuildButton.setButtonText(uiText(u8"再スキャン"));
    closeButton.setButtonText(uiText(u8"閉じる"));
    rebuildButton.setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);
    closeButton.setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);

    searchEditor.setTextToShowWhenEmpty(uiText(u8"プラグイン名を検索"),
                                        UiTheme::textMuted);
    searchEditor.setColour(juce::TextEditor::backgroundColourId, UiTheme::controlSurface);
    searchEditor.setColour(juce::TextEditor::textColourId, UiTheme::textPrimary);
    searchEditor.setColour(juce::TextEditor::outlineColourId, UiTheme::border);
    searchEditor.setColour(juce::TextEditor::focusedOutlineColourId, UiTheme::accent);

    allButton.setButtonText(uiText(u8"全件"));
    favouritesButton.setButtonText(uiText(u8"お気に入り"));
    recentButton.setButtonText(uiText(u8"最近"));
    quarantinedButton.setButtonText(uiText(u8"隔離"));
    constexpr int filterGroupId = 3107;
    for (auto* button : { &allButton, &favouritesButton,
                          &recentButton, &quarantinedButton })
    {
        button->setClickingTogglesState(true);
        button->setRadioGroupId(filterGroupId);
        button->setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);
        button->setColour(juce::TextButton::buttonOnColourId, UiTheme::accentSoft);
    }
    allButton.setToggleState(true, juce::dontSendNotification);

    pluginListLabel.setText(uiText(u8"見つかったプラグイン"),
                            juce::dontSendNotification);
    slotListLabel.setText(uiText(u8"このトラックのエフェクト順"),
                          juce::dontSendNotification);
    styleLabel(pluginListLabel, 13.0f, true);
    styleLabel(slotListLabel, 13.0f, true);
    styleList(pluginList);
    styleList(slotList);

    pluginDetailsLabel.setText(uiText(u8"プラグインを選んでください"),
                               juce::dontSendNotification);
    pluginDetailsLabel.setColour(juce::Label::textColourId, UiTheme::textSecondary);
    pluginDetailsLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    pluginDetailsLabel.setJustificationType(juce::Justification::centredLeft);
    pluginDetailsLabel.setMinimumHorizontalScale(0.75f);

    favouriteButton.setButtonText(uiText(u8"☆ お気に入り"));
    addButton.setButtonText(uiText(u8"このトラックへ追加"));
    favouriteButton.setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);
    addButton.setColour(juce::TextButton::buttonColourId, UiTheme::accent);

    moveUpButton.setButtonText(uiText(u8"上へ"));
    moveDownButton.setButtonText(uiText(u8"下へ"));
    bypassButton.setButtonText(uiText(u8"バイパス"));
    editorButton.setButtonText(uiText(u8"設定を開く"));
    removeButton.setButtonText(uiText(u8"削除"));
    for (auto* button : { &moveUpButton, &moveDownButton,
                          &bypassButton, &editorButton, &removeButton })
        button->setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);
    removeButton.setColour(juce::TextButton::buttonColourId,
                           UiTheme::danger.darker(0.35f));

    statusLabel.setText({}, juce::dontSendNotification);
    statusLabel.setColour(juce::Label::textColourId, UiTheme::textSecondary);
    statusLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    statusLabel.setJustificationType(juce::Justification::centredLeft);

    auto addControl = [this](juce::Component& component)
    {
        addAndMakeVisible(component);
    };
    addControl(titleLabel);
    addControl(trackLabel);
    addControl(trackBox);
    addControl(rebuildButton);
    addControl(closeButton);
    addControl(searchEditor);
    addControl(allButton);
    addControl(favouritesButton);
    addControl(recentButton);
    addControl(quarantinedButton);
    addControl(pluginListLabel);
    addControl(pluginList);
    addControl(pluginDetailsLabel);
    addControl(favouriteButton);
    addControl(addButton);
    addControl(slotListLabel);
    addControl(slotList);
    addControl(moveUpButton);
    addControl(moveDownButton);
    addControl(bypassButton);
    addControl(editorButton);
    addControl(removeButton);
    addControl(statusLabel);

    updateSelectedPluginDetails();
    updateSlotButtons();
}

void Vst3BrowserComponent::configureCallbacks()
{
    searchEditor.onTextChange = [this]
    {
        rebuildFilteredEntries();
    };
    allButton.onClick = [this] { setFilter(Filter::all); };
    favouritesButton.onClick = [this] { setFilter(Filter::favourites); };
    recentButton.onClick = [this] { setFilter(Filter::recent); };
    quarantinedButton.onClick = [this] { setFilter(Filter::quarantined); };

    trackBox.onChange = [this]
    {
        if (!updatingControls && onSelectedTrackChanged)
            onSelectedTrackChanged(getSelectedTrackIndex());
        updateSelectedPluginDetails();
        updateSlotButtons();
    };
    rebuildButton.onClick = [this]
    {
        if (onRequestCatalogRebuild)
            onRequestCatalogRebuild();
    };
    closeButton.onClick = [this]
    {
        if (onClose)
            onClose();
    };
    favouriteButton.onClick = [this] { toggleSelectedFavourite(); };
    addButton.onClick = [this] { requestAddSelectedPlugin(); };

    moveUpButton.onClick = [this]
    {
        const int slot = getSelectedSlotRow();
        if (slot > 0 && onMoveSlot)
            onMoveSlot(getSelectedTrackIndex(), slot, slot - 1);
    };
    moveDownButton.onClick = [this]
    {
        const int slot = getSelectedSlotRow();
        if (juce::isPositiveAndBelow(slot, static_cast<int>(effectSlots.size()) - 1)
            && onMoveSlot)
            onMoveSlot(getSelectedTrackIndex(), slot, slot + 1);
    };
    bypassButton.onClick = [this]
    {
        const int slot = getSelectedSlotRow();
        if (juce::isPositiveAndBelow(slot, static_cast<int>(effectSlots.size()))
            && onSetSlotBypassed)
        {
            onSetSlotBypassed(getSelectedTrackIndex(), slot,
                              !effectSlots[static_cast<size_t>(slot)].bypassed);
        }
    };
    editorButton.onClick = [this]
    {
        const int slot = getSelectedSlotRow();
        if (slot >= 0 && onOpenSlotEditor)
            onOpenSlotEditor(getSelectedTrackIndex(), slot);
    };
    removeButton.onClick = [this]
    {
        const int slot = getSelectedSlotRow();
        if (slot >= 0 && onRemoveSlot)
            onRemoveSlot(getSelectedTrackIndex(), slot);
    };
}

void Vst3BrowserComponent::setCatalog(Vst3PluginCatalog& pluginCatalog)
{
    catalog = &pluginCatalog;
    refreshCatalogView();
}

void Vst3BrowserComponent::refreshCatalogView()
{
    rebuildFilteredEntries();
}

void Vst3BrowserComponent::setTracks(const juce::StringArray& trackNames,
                                     int selectedTrackIndex)
{
    const juce::ScopedValueSetter<bool> guard(updatingControls, true);
    availableTrackNames = trackNames;
    trackBox.clear(juce::dontSendNotification);
    for (int index = 0; index < availableTrackNames.size(); ++index)
        trackBox.addItem(availableTrackNames[index], index + 1);
    setSelectedTrackIndex(selectedTrackIndex);
}

void Vst3BrowserComponent::setSelectedTrackIndex(int trackIndex)
{
    const bool previousGuard = updatingControls;
    updatingControls = true;
    if (juce::isPositiveAndBelow(trackIndex, availableTrackNames.size()))
        trackBox.setSelectedId(trackIndex + 1, juce::dontSendNotification);
    else
        trackBox.setSelectedId(0, juce::dontSendNotification);
    updatingControls = previousGuard;
    updateSelectedPluginDetails();
    updateSlotButtons();
}

int Vst3BrowserComponent::getSelectedTrackIndex() const noexcept
{
    return trackBox.getSelectedId() - 1;
}

void Vst3BrowserComponent::setEffectSlots(const std::vector<EffectSlotData>& slots)
{
    const int previouslySelected = getSelectedSlotRow();
    effectSlots = slots;
    slotList.updateContent();
    if (juce::isPositiveAndBelow(previouslySelected,
                                 static_cast<int>(effectSlots.size())))
        slotList.selectRow(previouslySelected);
    else if (!effectSlots.empty())
        slotList.selectRow(0);
    else
        slotList.deselectAllRows();
    updateSlotButtons();
}

void Vst3BrowserComponent::setFilter(Filter newFilter)
{
    activeFilter = newFilter;
    selectFilterButton();
    rebuildFilteredEntries();
}

void Vst3BrowserComponent::selectFilterButton()
{
    allButton.setToggleState(activeFilter == Filter::all, juce::dontSendNotification);
    favouritesButton.setToggleState(activeFilter == Filter::favourites,
                                    juce::dontSendNotification);
    recentButton.setToggleState(activeFilter == Filter::recent,
                                juce::dontSendNotification);
    quarantinedButton.setToggleState(activeFilter == Filter::quarantined,
                                     juce::dontSendNotification);
}

void Vst3BrowserComponent::rebuildFilteredEntries()
{
    if (catalog == nullptr)
    {
        filteredEntries.clear();
    }
    else
    {
        switch (activeFilter)
        {
            case Filter::favourites: filteredEntries = catalog->getFavourites(); break;
            case Filter::recent: filteredEntries = catalog->getRecent(50); break;
            case Filter::quarantined: filteredEntries = catalog->getQuarantined(); break;
            case Filter::all:
            default: filteredEntries = catalog->getEntries(); break;
        }

        const auto query = searchEditor.getText().trim();
        if (query.isNotEmpty())
        {
            filteredEntries.erase(
                std::remove_if(filteredEntries.begin(), filteredEntries.end(),
                    [&query](const Vst3PluginCatalog::Entry& entry)
                    {
                        return !entry.name.containsIgnoreCase(query);
                    }),
                filteredEntries.end());
        }
    }

    pluginList.updateContent();
    if (!filteredEntries.empty())
        pluginList.selectRow(0);
    else
        pluginList.deselectAllRows();
    updateSelectedPluginDetails();

    pluginListLabel.setText(
        uiText(u8"見つかったプラグイン（")
            + juce::String(static_cast<int>(filteredEntries.size())) + uiText(u8"件）"),
        juce::dontSendNotification);
}

const Vst3PluginCatalog::Entry* Vst3BrowserComponent::getSelectedPlugin() const noexcept
{
    const int row = getSelectedPluginRow();
    return juce::isPositiveAndBelow(row, static_cast<int>(filteredEntries.size()))
        ? &filteredEntries[static_cast<size_t>(row)] : nullptr;
}

int Vst3BrowserComponent::getSelectedPluginRow() const noexcept
{
    return pluginList.getSelectedRow();
}

int Vst3BrowserComponent::getSelectedSlotRow() const noexcept
{
    return slotList.getSelectedRow();
}

void Vst3BrowserComponent::updateSelectedPluginDetails()
{
    const auto* entry = getSelectedPlugin();
    const bool hasTrack = juce::isPositiveAndBelow(getSelectedTrackIndex(),
                                                   availableTrackNames.size());
    if (entry == nullptr)
    {
        pluginDetailsLabel.setText(uiText(u8"該当するプラグインがありません"),
                                   juce::dontSendNotification);
        pluginDetailsLabel.setColour(juce::Label::textColourId, UiTheme::textSecondary);
        favouriteButton.setEnabled(false);
        addButton.setEnabled(false);
        return;
    }

    juce::String details = entry->bundle.getFullPathName();
    if (entry->isQuarantined())
        details = uiText(u8"隔離理由: ") + entry->quarantineReason;
    pluginDetailsLabel.setText(details, juce::dontSendNotification);
    pluginDetailsLabel.setColour(juce::Label::textColourId,
                                  entry->isQuarantined() ? UiTheme::danger
                                                         : UiTheme::textSecondary);
    favouriteButton.setButtonText(entry->isFavourite
        ? uiText(u8"★ お気に入り解除") : uiText(u8"☆ お気に入り"));
    favouriteButton.setEnabled(true);
    addButton.setButtonText(entry->isQuarantined()
        ? uiText(u8"隔離を解除して再確認")
        : uiText(u8"このトラックへ追加"));
    addButton.setEnabled(hasTrack);
    addButton.setTooltip(entry->isQuarantined()
        ? uiText(u8"更新・修復したプラグインを明示的に再検査します")
        : juce::String());
}

void Vst3BrowserComponent::requestAddSelectedPlugin()
{
    const auto* entry = getSelectedPlugin();
    const int trackIndex = getSelectedTrackIndex();
    if (entry == nullptr
        || !juce::isPositiveAndBelow(trackIndex, availableTrackNames.size()))
        return;

    const auto bundle = entry->bundle;
    if (entry->isQuarantined() && catalog != nullptr)
    {
        catalog->clearQuarantine(bundle);
        rebuildFilteredEntries();
    }
    if (onAddPlugin)
        onAddPlugin(trackIndex, bundle);
}

void Vst3BrowserComponent::toggleSelectedFavourite()
{
    const auto* selected = getSelectedPlugin();
    if (selected == nullptr || catalog == nullptr)
        return;

    const auto bundle = selected->bundle;
    const bool shouldBeFavourite = !selected->isFavourite;
    catalog->setFavourite(bundle, shouldBeFavourite);
    if (onFavouriteChanged)
        onFavouriteChanged(bundle, shouldBeFavourite);
    rebuildFilteredEntries();
}

void Vst3BrowserComponent::updateSlotButtons()
{
    const int slot = getSelectedSlotRow();
    const bool hasSlot = juce::isPositiveAndBelow(slot,
                                                  static_cast<int>(effectSlots.size()));
    const bool hasTrack = juce::isPositiveAndBelow(getSelectedTrackIndex(),
                                                   availableTrackNames.size());
    moveUpButton.setEnabled(hasTrack && hasSlot && slot > 0);
    moveDownButton.setEnabled(hasTrack && hasSlot
                             && slot + 1 < static_cast<int>(effectSlots.size()));
    bypassButton.setEnabled(hasTrack && hasSlot);
    editorButton.setEnabled(hasTrack && hasSlot);
    removeButton.setEnabled(hasTrack && hasSlot);

    if (hasSlot)
    {
        bypassButton.setButtonText(effectSlots[static_cast<size_t>(slot)].bypassed
            ? uiText(u8"ONにする") : uiText(u8"バイパス"));
    }
    else
    {
        bypassButton.setButtonText(uiText(u8"バイパス"));
    }
}

void Vst3BrowserComponent::setStatusMessage(const juce::String& message,
                                            bool isError)
{
    if (auto* manager = juce::MessageManager::getInstanceWithoutCreating();
        manager != nullptr && !manager->isThisTheMessageThread())
    {
        juce::Component::SafePointer<Vst3BrowserComponent> safeThis(this);
        juce::MessageManager::callAsync([safeThis, message, isError]
        {
            if (safeThis != nullptr)
                safeThis->setStatusMessage(message, isError);
        });
        return;
    }

    statusLabel.setText(message, juce::dontSendNotification);
    statusLabel.setColour(juce::Label::textColourId,
                          isError ? UiTheme::danger : UiTheme::success);
}

void Vst3BrowserComponent::paint(juce::Graphics& g)
{
    g.fillAll(UiTheme::windowBackground);
    UiTheme::fillCard(g, getLocalBounds().toFloat().reduced(12.0f), 12.0f);

    const int middle = getWidth() / 2;
    g.setColour(UiTheme::borderSoft);
    g.drawVerticalLine(middle, 122.0f,
                       static_cast<float>(getHeight() - 56));
}

void Vst3BrowserComponent::resized()
{
    auto area = getLocalBounds().reduced(26, 20);
    auto header = area.removeFromTop(42);
    titleLabel.setBounds(header.removeFromLeft(230));
    closeButton.setBounds(header.removeFromRight(76).reduced(0, 3));
    header.removeFromRight(8);
    rebuildButton.setBounds(header.removeFromRight(92).reduced(0, 3));
    header.removeFromRight(12);
    trackBox.setBounds(header.removeFromRight(190).reduced(0, 3));
    trackLabel.setBounds(header.removeFromRight(58));
    area.removeFromTop(10);

    auto filters = area.removeFromTop(40);
    searchEditor.setBounds(filters.removeFromLeft(270).reduced(0, 3));
    filters.removeFromLeft(10);
    constexpr int filterWidth = 82;
    allButton.setBounds(filters.removeFromLeft(64).reduced(0, 3));
    filters.removeFromLeft(5);
    favouritesButton.setBounds(filters.removeFromLeft(96).reduced(0, 3));
    filters.removeFromLeft(5);
    recentButton.setBounds(filters.removeFromLeft(64).reduced(0, 3));
    filters.removeFromLeft(5);
    quarantinedButton.setBounds(filters.removeFromLeft(filterWidth).reduced(0, 3));
    area.removeFromTop(8);

    statusLabel.setBounds(area.removeFromBottom(28));
    area.removeFromBottom(6);
    const int gap = 18;
    auto left = area.removeFromLeft((area.getWidth() - gap) / 2);
    area.removeFromLeft(gap);
    auto right = area;

    pluginListLabel.setBounds(left.removeFromTop(28));
    auto pluginButtons = left.removeFromBottom(40);
    favouriteButton.setBounds(pluginButtons.removeFromLeft(150).reduced(0, 3));
    pluginButtons.removeFromLeft(8);
    addButton.setBounds(pluginButtons.reduced(0, 3));
    pluginDetailsLabel.setBounds(left.removeFromBottom(44));
    left.removeFromBottom(6);
    pluginList.setBounds(left);

    slotListLabel.setBounds(right.removeFromTop(28));
    auto slotButtons = right.removeFromBottom(78);
    auto firstRow = slotButtons.removeFromTop(36);
    moveUpButton.setBounds(firstRow.removeFromLeft(68).reduced(0, 2));
    firstRow.removeFromLeft(6);
    moveDownButton.setBounds(firstRow.removeFromLeft(68).reduced(0, 2));
    firstRow.removeFromLeft(6);
    bypassButton.setBounds(firstRow.removeFromLeft(94).reduced(0, 2));
    auto secondRow = slotButtons.removeFromBottom(36);
    editorButton.setBounds(secondRow.removeFromLeft(124).reduced(0, 2));
    secondRow.removeFromLeft(8);
    removeButton.setBounds(secondRow.removeFromLeft(76).reduced(0, 2));
    right.removeFromBottom(6);
    slotList.setBounds(right);
}
