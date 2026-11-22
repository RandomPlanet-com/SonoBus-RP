// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// The row above a list of stages: who you are on the left, the selected stage and Connect on the right.
// A narrow window puts the name and instrument on their own row, and under each other when even that is too wide.
#pragma once

#include "JuceHeader.h"
#include "SbTextFit.h"
#include "SbChooserCombo.h"
#include "SonoLookAndFeel.h"
#include <functional>

class SbConnectBar : public juce::Component
{
public:
    static constexpr int kMaxNameLength = 30;

    static juce::StringArray instrumentChoices()
    {
        return { "Drums", "Percussion", "Bass", "Guitar", "Acoustic Guitar", "Keys", "Piano", "Organ", "Synth", "Vocals",
                 "Sax", "Trumpet", "Trombone", "Flute", "Clarinet", "Violin", "Viola", "Cello", "Double Bass",
                 "Mandolin", "Banjo", "Pedal Steel", "Harmonica", "Accordion", "DJ / Electronics" };
    }

    explicit SbConnectBar (const juce::String& connectTip)
    {
        const float sc = SonoLookAndFeel::getFontScale();
        const juce::Colour dim (0xff8f8f8f);

        for (auto* l : { &mNameLabel, &mInstrumentLabel }) {
            l->setFont (juce::Font (14.0f * sc));
            l->setColour (juce::Label::textColourId, dim);
            l->setJustificationType (juce::Justification::centredLeft);
            l->setMinimumHorizontalScale (1.0f);
            l->setBorderSize (juce::BorderSize<int> (0, 0, 0, 6));
            addAndMakeVisible (*l);
        }
        mNameLabel.setText (TRANS("Your name"), juce::dontSendNotification);
        mInstrumentLabel.setText (TRANS("Instrument"), juce::dontSendNotification);

        mName.setFont (juce::Font (15.0f * sc));
        mName.setInputRestrictions (kMaxNameLength);
        mName.setTooltip (TRANS("How other players see you"));
        mName.setTitle (TRANS("Your name"));
        mName.onTextChange = [this] {
            setNameMissing (false);
            if (onNameChanged) onNameChanged (getPlayerName());
        };
        mName.onReturnKey = [this] { mName.giveAwayKeyboardFocus(); };   // leaving the field commits
        mName.onFocusLost = [this] { commitName(); };
        mName.onEscapeKey = [this] {
            if (mName.getText() != mCommitted) {
                mName.setText (mCommitted, false);
                if (onNameChanged) onNameChanged (getPlayerName());
            }
            mName.giveAwayKeyboardFocus();
        };
        addAndMakeVisible (mName);

        mInstrument.setEditableText (true);   // the list is a help; anything can be typed
        mInstrument.addItemList (instrumentChoices(), 1);
        mInstrument.setTextWhenNothingSelected (TRANS("choose or type"));
        mInstrument.setTooltip (TRANS("Shown to other players, for example in the stage list"));
        mInstrument.setTitle (TRANS("Instrument"));
        mInstrument.onChange = [this] { if (onInstrumentChanged) onInstrumentChanged (mInstrument.getText()); };
        addAndMakeVisible (mInstrument);

        mConnect.setButtonText (TRANS("Connect"));
        mConnect.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f8f86));
        mConnect.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        mConnect.setTooltip (connectTip);
        mConnect.onClick = [this] { if (onConnect) onConnect(); };
        mConnect.setEnabled (false);
        addAndMakeVisible (mConnect);

        mSelected.setJustificationType (juce::Justification::centredRight);
        mSelected.setFont (juce::Font (14.0f * sc));
        mSelected.setColour (juce::Label::textColourId, juce::Colour (0xffcfcfcf));
        mSelected.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (mSelected);

        mNotice.setJustificationType (juce::Justification::centredLeft);
        mNotice.setFont (juce::Font (14.0f * sc));
        mNotice.setColour (juce::Label::textColourId, juce::Colour (0xffd6b03e));
        mNotice.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (mNotice);
    }

    std::function<void (const juce::String&)> onNameChanged;         // every edit
    std::function<void (const juce::String&)> onNameCommitted;       // Enter, or the field was left
    std::function<void (const juce::String&)> onInstrumentChanged;
    std::function<void()> onConnect;
    std::function<void()> onHeightChanged;                           // the bar needs another height at this width

    juce::String getPlayerName() const { return mName.getText().trim(); }
    juce::String getNotice() const { return mNotice.getText(); }

    // A value from outside never overwrites what is being typed
    void setPlayerName (const juce::String& name)
    {
        if (mName.hasKeyboardFocus (true)) return;
        mCommitted = name;
        if (mName.getText() != name) mName.setText (name, false);
        if (name.trim().isNotEmpty()) setNameMissing (false);
    }

    // Puts a value back even while the field has the focus
    void resetPlayerName (const juce::String& name)
    {
        mCommitted = name;
        if (mName.getText() != name) mName.setText (name, false);
    }

    void setInstrument (const juce::String& instrument, bool evenWhileTyping = false)
    {
        if (evenWhileTyping || ! mInstrument.hasKeyboardFocus (true)) mInstrument.setText (instrument, juce::dontSendNotification);
    }

    void setNameMissing (bool missing)
    {
        if (mMissing == missing) return;
        mMissing = missing;
        if (missing) mName.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff880000));
        else mName.removeColour (juce::TextEditor::backgroundColourId);
        mName.repaint();
    }

    void setSelected (const juce::String& text)
    {
        if (mSelected.getText() == text) return;
        mSelected.setText (text, juce::dontSendNotification);
        laidOutAgain();
    }

    void setNotice (const juce::String& text)
    {
        if (mNotice.getText() == text) return;
        mNotice.setText (text, juce::dontSendNotification);
        laidOutAgain();
    }

    void setCanConnect (bool can) { mConnect.setEnabled (can); }

    int heightFor (int width) const
    {
        const int rows = rowsFor (width);
        return 2 * kPad + rows * kRow + (rows - 1) * kLineGap;
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (kSide, kPad);
        const Need n = need();
        const int rows = rowsFor (getWidth());
        const int width = r.getWidth();

        // Every line is one group, centred in the bar
        auto centred = [] (juce::Rectangle<int> line, int w) { return line.withSizeKeepingCentre (juce::jmin (w, line.getWidth()), line.getHeight()); };
        auto fieldsInOneLine = [this, &n, &centred] (juce::Rectangle<int> line) {
            const int nameW = juce::jlimit (n.nameMin, n.namePref, line.getWidth() - n.nameLabel - kGroupGap - n.instLabel - n.inst);
            line = centred (line, n.nameLabel + nameW + kGroupGap + n.instLabel + n.inst);
            mNameLabel.setBounds (line.removeFromLeft (n.nameLabel));
            mName.setBounds (line.removeFromLeft (nameW));
            line.removeFromLeft (kGroupGap);
            mInstrumentLabel.setBounds (line.removeFromLeft (n.instLabel));
            mInstrument.setBounds (line.removeFromLeft (n.inst));
        };

        if (rows == 1) {
            auto g = centred (r, oneLineWidth (n));
            fieldsInOneLine (g.removeFromLeft (n.fields));
            if (n.notice > 0) { g.removeFromLeft (kGap); mNotice.setBounds (g.removeFromLeft (n.notice)); }
            else mNotice.setBounds ({});
            g.removeFromLeft (kGroupGap);
            mSelected.setBounds (g.removeFromLeft (n.selected));
            mConnect.setBounds (g.removeFromLeft (n.connect));
        } else {
            if (rows == 2) {
                fieldsInOneLine (r.removeFromTop (kRow));
            } else {
                const int labelW = juce::jmax (n.nameLabel, n.instLabel);
                auto line = centred (r.removeFromTop (kRow), labelW + n.namePref);
                mNameLabel.setBounds (line.removeFromLeft (labelW));
                mName.setBounds (line);
                r.removeFromTop (kLineGap);
                line = centred (r.removeFromTop (kRow), labelW + n.namePref);
                mInstrumentLabel.setBounds (line.removeFromLeft (labelW));
                mInstrument.setBounds (line.removeFromLeft (juce::jmin (n.inst, line.getWidth())));
            }
            r.removeFromTop (kLineGap);
            const int connectW = juce::jmin (n.connect, width);
            auto line = centred (r.removeFromTop (kRow), (n.notice > 0 ? n.notice + kGap : 0) + n.selected + connectW);
            mConnect.setBounds (line.removeFromRight (connectW));
            mSelected.setBounds (line.removeFromRight (juce::jmin (n.selected, juce::jmax (0, line.getWidth()))));
            mNotice.setBounds (line.withTrimmedRight (line.getWidth() > kGap ? kGap : 0));
        }

        // The full text in the hover when a label has to wrap past its lines or end in "..."
        auto tipIfCut = [] (juce::Label& l) {
            const auto font = l.getFont();
            const int lines = juce::jmax (1, (int) ((float) (l.getHeight() - 2) / font.getHeight()));
            const int room = lines * (l.getWidth() - 10) - (lines > 1 ? juce::roundToInt (font.getHeight() * 3.0f) : 0);
            l.setTooltip (SbTextFit::textWidth (font, l.getText()) > room ? l.getText() : juce::String());
        };
        tipIfCut (mSelected);
        tipIfCut (mNotice);
    }

private:
    enum { kSide = 10, kPad = 7, kRow = 32, kGap = 12, kGroupGap = 24, kLineGap = 6 };

    struct Need { int nameLabel = 0, nameMin = 0, namePref = 0, instLabel = 0, inst = 0, fields = 0, fieldsMin = 0, selected = 0, connect = 0, notice = 0; };

    Need need() const
    {
        const float sc = SonoLookAndFeel::getFontScale();
        Need n;
        n.nameLabel = SbTextFit::labelWidth (const_cast<juce::Label&> (mNameLabel));
        n.instLabel = SbTextFit::labelWidth (const_cast<juce::Label&> (mInstrumentLabel));
        n.nameMin = juce::roundToInt (120.0f * sc);
        n.namePref = juce::roundToInt (230.0f * sc);
        auto& combo = const_cast<SbChooserCombo&> (mInstrument);
        n.inst = SbTextFit::comboWidth (combo, kRow, mInstrument.getTextWhenNothingSelected());
        for (int i = 0; i < mInstrument.getNumItems(); ++i) n.inst = juce::jmax (n.inst, SbTextFit::comboWidth (combo, kRow, mInstrument.getItemText (i)));
        n.fields = n.nameLabel + n.namePref + kGroupGap + n.instLabel + n.inst;
        n.fieldsMin = n.fields - n.namePref + n.nameMin;
        n.selected = mSelected.getText().isEmpty() ? 0 : SbTextFit::textWidth (mSelected.getFont(), mSelected.getText()) + 16;
        n.connect = juce::roundToInt (150.0f * sc);
        n.notice = mNotice.getText().isEmpty() ? 0 : juce::jmin (420, SbTextFit::textWidth (mNotice.getFont(), mNotice.getText()) + 16);
        return n;
    }

    static int oneLineWidth (const Need& n)
    {
        return n.fields + (n.notice > 0 ? kGap + n.notice : 0) + kGroupGap + n.selected + n.connect;
    }

    int rowsFor (int width) const
    {
        const Need n = need();
        const int inner = width - 2 * kSide;
        if (inner >= oneLineWidth (n)) return 1;
        return inner >= n.fieldsMin ? 2 : 3;
    }

    void laidOutAgain()
    {
        if (getWidth() <= 0) return;
        if (heightFor (getWidth()) != getHeight() && onHeightChanged) onHeightChanged();
        else resized();
    }

    void commitName()
    {
        const juce::String name = getPlayerName();
        if (name == mCommitted) return;
        mCommitted = name;
        if (onNameCommitted) onNameCommitted (name);
    }

    juce::Label mNameLabel, mInstrumentLabel, mSelected, mNotice;
    juce::TextEditor mName;
    SbChooserCombo mInstrument;
    juce::TextButton mConnect;
    juce::String mCommitted;
    bool mMissing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SbConnectBar)
};
