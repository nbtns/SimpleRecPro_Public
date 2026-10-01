#pragma once
#include <JuceHeader.h>
#include "UiTheme.h"

class ModernLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        ModernLookAndFeel()
        {
            setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);
            setColour(juce::TextButton::buttonOnColourId, UiTheme::accent);
            setColour(juce::TextButton::textColourOffId, UiTheme::textPrimary);
            setColour(juce::TextButton::textColourOnId, UiTheme::textPrimary);
            setColour(juce::Slider::trackColourId, UiTheme::accent);
            setColour(juce::Slider::backgroundColourId, UiTheme::controlSurface);
            setColour(juce::Slider::thumbColourId, UiTheme::textPrimary);
            setColour(juce::PopupMenu::backgroundColourId, UiTheme::raisedSurface);
            setColour(juce::PopupMenu::textColourId, UiTheme::textPrimary);
            setColour(juce::PopupMenu::highlightedBackgroundColourId, UiTheme::accentSoft);
            setColour(juce::PopupMenu::highlightedTextColourId, UiTheme::textPrimary);
            setColour(juce::ScrollBar::thumbColourId, UiTheme::controlHover);
            setColour(juce::ScrollBar::trackColourId, UiTheme::panelBackground);
            setColour(juce::ScrollBar::backgroundColourId, UiTheme::panelBackground);
        }

        void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                   bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
        {
            auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
            auto baseColour = backgroundColour;
            if (!button.isEnabled())
                baseColour = UiTheme::controlSurface.withAlpha(0.45f);
            else if (shouldDrawButtonAsDown)
                baseColour = baseColour.darker(0.16f);
            else if (shouldDrawButtonAsHighlighted)
                baseColour = baseColour == UiTheme::accent
                                 ? UiTheme::accentHover : UiTheme::controlHover;

            g.setColour(baseColour);
            g.fillRoundedRectangle(bounds, 8.0f);
            g.setColour(baseColour == UiTheme::accent
                            ? UiTheme::accentHover.withAlpha(0.75f)
                            : UiTheme::border);
            g.drawRoundedRectangle(bounds, 8.0f, 1.0f);
        }

        juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override
        {
            return juce::Font(juce::FontOptions(
                buttonHeight >= 40 ? 16.0f : 14.0f,
                juce::Font::bold));
        }

        juce::Font getComboBoxFont(juce::ComboBox&) override
        {
            return juce::Font(juce::FontOptions(16.0f));
        }

        juce::Font getPopupMenuFont() override
        {
            return juce::Font(juce::FontOptions(16.0f));
        }

        juce::Label* createSliderTextBox(juce::Slider& slider) override
        {
            auto* label = juce::LookAndFeel_V4::createSliderTextBox(slider);
            label->setFont(juce::Font(juce::FontOptions(16.0f)));
            return label;
        }

        void drawButtonText (juce::Graphics& g, juce::TextButton& button,
                             bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
        {
            juce::LookAndFeel_V4::drawButtonText(
                g, button, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
        }

        void drawLinearSlider(juce::Graphics& g,
                              int x, int y, int width, int height,
                              float sliderPos, float minSliderPos, float maxSliderPos,
                              juce::Slider::SliderStyle style,
                              juce::Slider& slider) override
        {
            if (style != juce::Slider::LinearHorizontal)
            {
                juce::LookAndFeel_V4::drawLinearSlider(
                    g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos,
                    style, slider);
                return;
            }

            const float centreY = static_cast<float>(y)
                                  + static_cast<float>(height) * 0.5f;
            const auto track = juce::Rectangle<float>(
                static_cast<float>(x), centreY - 3.0f,
                static_cast<float>(width), 6.0f);
            g.setColour(slider.findColour(juce::Slider::backgroundColourId));
            g.fillRoundedRectangle(track, 2.0f);
            g.setColour(slider.findColour(juce::Slider::trackColourId));
            g.fillRoundedRectangle(
                track.withWidth(juce::jmax(0.0f, sliderPos - track.getX())),
                2.0f);
            g.setColour(slider.findColour(juce::Slider::thumbColourId));
            g.fillEllipse(sliderPos - 8.0f, centreY - 8.0f, 16.0f, 16.0f);
        }
    };
