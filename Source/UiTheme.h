#pragma once

#include <JuceHeader.h>

namespace UiTheme
{
inline const juce::Colour windowBackground { 0xff090c12 };
inline const juce::Colour panelBackground { 0xff0f141d };
inline const juce::Colour raisedSurface { 0xff171d28 };
inline const juce::Colour controlSurface { 0xff202938 };
inline const juce::Colour controlHover { 0xff2a3547 };
inline const juce::Colour border { 0xff2a3443 };
inline const juce::Colour borderSoft { 0xff1d2531 };
inline const juce::Colour textPrimary { 0xfff2f5f9 };
inline const juce::Colour textSecondary { 0xff97a3b5 };
inline const juce::Colour textMuted { 0xff667386 };
inline const juce::Colour accent { 0xff5b8cff };
inline const juce::Colour accentHover { 0xff73a0ff };
inline const juce::Colour accentSoft { 0xff1c315f };
inline const juce::Colour success { 0xff31c7a1 };
inline const juce::Colour warning { 0xfff3b45b };
inline const juce::Colour danger { 0xffff5d6c };

inline void fillCard(juce::Graphics& g,
                     juce::Rectangle<float> bounds,
                     float cornerRadius = 10.0f)
{
    g.setColour(raisedSurface);
    g.fillRoundedRectangle(bounds, cornerRadius);
    g.setColour(border);
    g.drawRoundedRectangle(bounds.reduced(0.5f), cornerRadius, 1.0f);
}
}
