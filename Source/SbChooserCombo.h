// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// A combo box whose list opens in the app's pop-up chooser (the stage menu's look) instead of the plain menu.
// Typing in an editable box works as before.
#pragma once

#include "JuceHeader.h"
#include "GenericItemChooser.h"

class SbChooserCombo : public juce::ComboBox
{
public:
    using juce::ComboBox::ComboBox;

    void showPopup() override
    {
        juce::Array<GenericItemChooserItem> items;
        int selected = -1;
        for (int i = 0; i < getNumItems(); ++i) {
            const auto text = getItemText (i);
            if (text == getText()) selected = i;
            items.add (GenericItemChooserItem (text));
        }
        if (items.isEmpty()) return;

        juce::Component* dw = findParentComponentOfClass<juce::AudioProcessorEditor>();
        if (dw == nullptr) dw = getTopLevelComponent();
        const auto bounds = dw->getLocalArea (nullptr, getScreenBounds());

        juce::Component::SafePointer<SbChooserCombo> safe (this);
        GenericItemChooser::launchPopupChooser (items, bounds, dw, [safe] (GenericItemChooser*, int index) {
            if (safe == nullptr || index < 0 || index >= safe->getNumItems()) return;
            if (safe->isTextEditable()) safe->setText (safe->getItemText (index), juce::sendNotificationAsync);
            else safe->setSelectedItemIndex (index, juce::sendNotificationAsync);
        }, selected, juce::jmax (200, dw->getHeight() - 60));
        hidePopup();   // the box is free again at once; the chooser closes itself on a pick or a click outside
    }
};
