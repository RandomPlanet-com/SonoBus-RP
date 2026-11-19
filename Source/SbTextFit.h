// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)

#pragma once

#include <JuceHeader.h>
#include <cmath>

// Width a control needs to show its text whole, in the font its look and feel draws it with.
// Layouts size fixed captions from these instead of guessed pixel counts.
namespace SbTextFit
{
    inline int textWidth (const juce::Font& f, const juce::String& s)
    {
        return (int) std::ceil (f.getStringWidthFloat (s.trim()));
    }

    // Widest of several strings in one font
    inline int widestText (const juce::Font& f, std::initializer_list<juce::String> strings)
    {
        int w = 0;
        for (const auto& s : strings) w = juce::jmax (w, textWidth (f, s));
        return w;
    }

    // Text button at the given height (side indents of the look and feel included)
    inline int buttonWidth (juce::TextButton& b, int height, const juce::String& text = {})
    {
        const auto f = b.getLookAndFeel().getTextButtonFont (b, height);
        const int indent = juce::roundToInt (f.getHeight() * 0.3f);
        return textWidth (f, text.isNotEmpty() ? text : b.getButtonText()) + 2 * indent + 4;
    }

    // Label with its border
    inline int labelWidth (juce::Label& l, const juce::String& text = {})
    {
        const auto f = l.getLookAndFeel().getLabelFont (l);
        const auto border = l.getLookAndFeel().getLabelBorderSize (l);
        return textWidth (f, text.isNotEmpty() ? text : l.getText()) + border.getLeftAndRight() + 2;
    }

    // Plain combo box at the given height (value label, its border and the arrow area)
    inline int comboWidth ([[maybe_unused]] juce::ComboBox& c, int height, const juce::String& text)
    {
        const auto f = juce::Font (juce::jmin (16.0f, (float) height * 0.9f));
        return textWidth (f, text) + 10 + 30 + 2;
    }

    // Toggle button with its tick box, at the given height
    inline int toggleWidth (juce::ToggleButton& t, int height, const juce::String& text = {})
    {
        const float fontSize = juce::jmin (15.0f, (float) height * 0.75f);
        const auto f = juce::Font (fontSize);
        return (int) (fontSize * 1.1f) + 10 + textWidth (f, text.isNotEmpty() ? text : t.getButtonText()) + 4;
    }

    // Full text in the hover while the control is narrower than its text; `otherwise` when it fits
    template <class C>
    inline void tooltipIfCut (C& c, const juce::String& fullText, int need, const juce::String& otherwise = {})
    {
        c.setTooltip (c.getWidth() < need ? fullText : otherwise);
    }
}
