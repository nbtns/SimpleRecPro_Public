#pragma once

#include <JuceHeader.h>

// Keep the transport centred on the whole window, independent of the dock.
struct WorkspaceLayout
{
    juce::Rectangle<int> header, editingToolbar, tracks, studio, comparison, transport;

    static WorkspaceLayout calculate(juce::Rectangle<int> bounds,
                                     int transportHeight, int toolbarHeight,
                                     bool studioVisible)
    {
        WorkspaceLayout result;
        result.header = bounds.removeFromTop(84);
        result.transport = bounds.removeFromBottom(transportHeight);
        result.comparison = bounds.removeFromBottom(60);
        result.editingToolbar = bounds.removeFromTop(toolbarHeight);
        if (studioVisible)
        {
            const auto dockWidth = juce::jlimit(380, 440,
                juce::roundToInt(bounds.getWidth() * 0.30));
            result.studio = bounds.removeFromRight(dockWidth);
        }
        result.tracks = bounds;
        return result;
    }
};
