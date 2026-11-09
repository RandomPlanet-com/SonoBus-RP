// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2020 Jesse Chappell
// Modifications Copyright (C) 2026 Jason Buecker (Random Planet)


#pragma once

#include <JuceHeader.h>

#include "SonoLookAndFeel.h"
#include "SonoDrawableButton.h"
#include "EffectsBaseView.h"
#include "EffectParams.h"

//==============================================================================
/*
*/
class ExpanderView    : public EffectsBaseView, public Slider::Listener, public Button::Listener
{
public:
    ExpanderView() 
    {
        // In your constructor, you should add any child components, and
        // initialise any special settings that your component needs.

        auto threshname = TRANS("Noise Floor");
        thresholdSlider.setName("thresh");
        thresholdSlider.setTitle(threshname);
        thresholdSlider.setRange(-96.0f, 0.0f, 1);
        thresholdSlider.setSkewFactor(1.5);
        thresholdSlider.setTextValueSuffix(" dB");
        thresholdSlider.setDoubleClickReturnValue(true, -60.0);
        configKnobSlider(thresholdSlider);
        thresholdSlider.addListener(this);

        thresholdLabel.setText(threshname, dontSendNotification);
        thresholdLabel.setAccessible(false);
        configLabel(thresholdLabel);

        auto rationame = TRANS("Ratio");
        ratioSlider.setName("ratio");
        ratioSlider.setTitle(rationame);
        ratioSlider.setRange(1.0f, 20.0f, 0.1);
        ratioSlider.setSkewFactor(0.5);
        ratioSlider.setTextValueSuffix(" : 1");
        ratioSlider.setDoubleClickReturnValue(true, 2.0);
        configKnobSlider(ratioSlider);
        ratioSlider.addListener(this);
        
        ratioLabel.setText(rationame, dontSendNotification);
        ratioLabel.setAccessible(false);
        configLabel(ratioLabel);

        auto attackname = TRANS("Attack");
        attackSlider.setName("attack");
        attackSlider.setTitle(attackname);
        attackSlider.setRange(1.0f, 1000.0f, 1);
        attackSlider.setSkewFactor(0.5);
        attackSlider.setTextValueSuffix(" ms");
        attackSlider.setDoubleClickReturnValue(true, 1.0);
        configKnobSlider(attackSlider);
        attackSlider.addListener(this);
        
        attackLabel.setText(attackname, dontSendNotification);
        attackLabel.setAccessible(false);
        configLabel(attackLabel);

           
        auto relname = TRANS("Release");
        releaseSlider.setName("release");
        releaseSlider.setTitle(relname);
        releaseSlider.setRange(1.0f, 1000.0f, 1);
        releaseSlider.setSkewFactor(0.5);
        releaseSlider.setTextValueSuffix(" ms");
        releaseSlider.setDoubleClickReturnValue(true, 200.0);
        configKnobSlider(releaseSlider);
        releaseSlider.addListener(this);
        
        releaseLabel.setText(relname, dontSendNotification);
        releaseLabel.setAccessible(false);
        configLabel(releaseLabel);

           
        // HOLD knob + HARD GATE toggle (closes to exact zero → idle-channel mask drops the channel)
        auto holdname = TRANS("Hold");
        holdSlider.setName("hold");
        holdSlider.setTitle(holdname);
        holdSlider.setRange(10.0f, 2000.0f, 1);
        holdSlider.setSkewFactor(0.5);
        holdSlider.setTextValueSuffix(" ms");
        holdSlider.setDoubleClickReturnValue(true, 150.0);
        configKnobSlider(holdSlider);
        holdSlider.addListener(this);
        holdLabel.setText(holdname, dontSendNotification);
        holdLabel.setAccessible(false);
        configLabel(holdLabel);
        gateButton.setButtonText(TRANS("Hard gate"));
        gateButton.setTooltip(TRANS("Close fully to digital silence after the hold time. Silent channels leave the network packet entirely."));
        gateButton.addListener(this);
        gateStateLabel.setText(String::fromUTF8("\xe2\x97\x8f ") + TRANS("off"), dontSendNotification);   // Always-visible off/open/closed indicator
        gateStateLabel.setJustificationType(Justification::centredLeft);
        gateStateLabel.setAccessible(false);
        configLabel(gateStateLabel);
        resetButton.setButtonText(TRANS("Reset"));
        resetButton.setTooltip(TRANS("Back to the default gate settings"));
        resetButton.addListener(this);
        // these are in the header component
        auto noisename = TRANS("Noise Gate");
        enableButton.addListener(this);
        enableButton.setTitle(noisename);
        titleLabel.setText(noisename, dontSendNotification);
        titleLabel.setAccessible(false);
           

        addAndMakeVisible(thresholdSlider);
        addAndMakeVisible(thresholdLabel);
        addAndMakeVisible(ratioSlider);
        addAndMakeVisible(ratioLabel);
        addAndMakeVisible(attackSlider);
        addAndMakeVisible(attackLabel);
        addAndMakeVisible(releaseSlider);
        addAndMakeVisible(releaseLabel);
        addAndMakeVisible(holdSlider);
        addAndMakeVisible(holdLabel);
        addAndMakeVisible(gateButton);
        addAndMakeVisible(resetButton);
        addAndMakeVisible(gateStateLabel);
        
        setupLayout();
        
        updateParams(mParams);
    }

    ~ExpanderView()
    {
    }

   
    
    class Listener {
    public:
        virtual ~Listener() {}
        virtual void expanderParamsChanged(ExpanderView *comp, SonoAudio::CompressorParams &params) {}
    };
    
    void addListener(Listener * listener) { listeners.add(listener); }
    void removeListener(Listener * listener) { listeners.remove(listener); }
    
    
    void setupLayout()
    {
        int minKnobWidth = 54;
        int minitemheight = 32;
        int knoblabelheight = 18;
        int knobitemheight = 62;
        int enablewidth = 44;

#if JUCE_IOS || JUCE_ANDROID
        // make the button heights a bit more for touchscreen purposes
        minitemheight = 40;
        knobitemheight = 80;
#endif
        
        threshBox.items.clear();
        threshBox.flexDirection = FlexBox::Direction::column;
        threshBox.items.add(FlexItem(minKnobWidth, knoblabelheight, thresholdLabel).withMargin(0).withFlex(0));
        threshBox.items.add(FlexItem(minKnobWidth, knobitemheight, thresholdSlider).withMargin(0).withFlex(1));
        
        ratioBox.items.clear();
        ratioBox.flexDirection = FlexBox::Direction::column;
        ratioBox.items.add(FlexItem(minKnobWidth, knoblabelheight, ratioLabel).withMargin(0).withFlex(0));
        ratioBox.items.add(FlexItem(minKnobWidth, knobitemheight, ratioSlider).withMargin(0).withFlex(1));
        
        attackBox.items.clear();
        attackBox.flexDirection = FlexBox::Direction::column;
        attackBox.items.add(FlexItem(minKnobWidth, knoblabelheight, attackLabel).withMargin(0).withFlex(0));
        attackBox.items.add(FlexItem(minKnobWidth, knobitemheight, attackSlider).withMargin(0).withFlex(1));
        
        releaseBox.items.clear();
        releaseBox.flexDirection = FlexBox::Direction::column;
        releaseBox.items.add(FlexItem(minKnobWidth, knoblabelheight, releaseLabel).withMargin(0).withFlex(0));
        releaseBox.items.add(FlexItem(minKnobWidth, knobitemheight, releaseSlider).withMargin(0).withFlex(1));
        holdBox.items.clear();
        holdBox.flexDirection = FlexBox::Direction::column;
        holdBox.items.add(FlexItem(minKnobWidth, knoblabelheight, holdLabel).withMargin(0).withFlex(0));
        holdBox.items.add(FlexItem(minKnobWidth, knobitemheight, holdSlider).withMargin(0).withFlex(1));

        
        
        checkBox.items.clear();
        checkBox.flexDirection = FlexBox::Direction::row;
        //checkBox.items.add(FlexItem(5, 5).withMargin(0).withFlex(0));
        checkBox.items.add(FlexItem(enablewidth, minitemheight, enableButton).withMargin(0).withFlex(0));
        checkBox.items.add(FlexItem(2, 5).withMargin(0).withFlex(0));
        checkBox.items.add(FlexItem(100, minitemheight, titleLabel).withMargin(0).withFlex(1).withMaxWidth(120));
        //checkBox.items.add(FlexItem(2, 5).withMargin(0).withFlex(0.1));
        checkBox.items.add(FlexItem(24, minitemheight, dragButton).withMargin(0).withFlex(0));
        checkBox.items.add(FlexItem(2, 5).withMargin(0).withFlex(0.1));

        headerComponent.headerBox.items.clear();
        headerComponent.headerBox.flexDirection = FlexBox::Direction::column;
        headerComponent.headerBox.items.add(FlexItem(150, minitemheight, checkBox).withMargin(0).withFlex(1));

        
        knobBox.items.clear();
        knobBox.flexDirection = FlexBox::Direction::row;
        knobBox.items.add(FlexItem(6, 5).withMargin(0).withFlex(0));
        knobBox.items.add(FlexItem(minKnobWidth, knobitemheight + knoblabelheight, threshBox).withMargin(0).withFlex(1));
        knobBox.items.add(FlexItem(minKnobWidth, knobitemheight + knoblabelheight, ratioBox).withMargin(0).withFlex(1));
        knobBox.items.add(FlexItem(minKnobWidth, knobitemheight + knoblabelheight, attackBox).withMargin(0).withFlex(1));
        knobBox.items.add(FlexItem(minKnobWidth, knobitemheight + knoblabelheight, releaseBox).withMargin(0).withFlex(1));
        knobBox.items.add(FlexItem(minKnobWidth, knobitemheight + knoblabelheight, holdBox).withMargin(0).withFlex(1));
        knobBox.items.add(FlexItem(6, 5).withMargin(0).withFlex(0));
        
        mainBox.items.clear();
        mainBox.flexDirection = FlexBox::Direction::column;
        //mainBox.items.add(FlexItem(150, minitemheight, checkBox).withMargin(0).withFlex(0));
        //mainBox.items.add(FlexItem(6, 2).withMargin(0).withFlex(0));
        mainBox.items.add(FlexItem(100, knoblabelheight + knobitemheight, knobBox).withMargin(0).withFlex(1));
        mainBox.items.add(FlexItem(6, 2).withMargin(0).withFlex(0));
        gateRowBox.items.clear();
        gateRowBox.flexDirection = FlexBox::Direction::row;
        gateRowBox.items.add(FlexItem(6, 2));
        gateRowBox.items.add(FlexItem(100, minitemheight, gateButton).withMargin(0).withFlex(0));
        gateRowBox.items.add(FlexItem(70, minitemheight, gateStateLabel).withMargin(0).withFlex(1));   
        gateRowBox.items.add(FlexItem(60, minitemheight - 6, resetButton).withMargin(3).withFlex(0));
        gateRowBox.items.add(FlexItem(6, 2));
        mainBox.items.add(FlexItem(100, minitemheight, gateRowBox).withMargin(0).withFlex(0));   
        mainBox.items.add(FlexItem(6, 2).withMargin(0).withFlex(0));
        
        minBounds.setSize(jmax(180, minKnobWidth * 5 + 16), knobitemheight + knoblabelheight + minitemheight + 6);   // 5 knobs + gate row
        minHeaderBounds.setSize(jmax(180, minKnobWidth * 5 + 16),  minitemheight + 8);

    }

   
    void resized() override
    {
        mainBox.performLayout(getLocalBounds());
    }

    void buttonClicked (Button* buttonThatWasClicked) override
    {
        if (buttonThatWasClicked == &enableButton) {
            mParams.enabled = enableButton.getToggleState();
            headerComponent.repaint();
            lastGateState = -2;
        }
        else if (buttonThatWasClicked == &gateButton) {
            mParams.gate = gateButton.getToggleState();
            lastGateState = -2;   // force the indicator to redraw on the next poll
        }
        else if (buttonThatWasClicked == &resetButton) {   // Start over (same defaults as a new channel)
            const bool wasEnabled = mParams.enabled;
            mParams = SonoAudio::CompressorParams();
            mParams.thresholdDb = -60.0f; mParams.ratio = 2.0f; mParams.attackMs = 1.0f; mParams.releaseMs = 200.0f; mParams.holdMs = 150.0f; mParams.gate = false;
            mParams.enabled = wasEnabled;
            updateParams(mParams);
        }
        listeners.call (&ExpanderView::Listener::expanderParamsChanged, this, mParams);
        
    }
    
    void sliderValueChanged (Slider* slider) override
    {
        if (slider == &thresholdSlider) {
            mParams.thresholdDb = slider->getValue();
        }
        else if (slider == &ratioSlider) {
            mParams.ratio = slider->getValue();
        }
        else if (slider == &attackSlider) {
            mParams.attackMs = slider->getValue();
        }
        else if (slider == &releaseSlider) {
            mParams.releaseMs = slider->getValue();
        }
        else if (slider == &holdSlider) {
            mParams.holdMs = slider->getValue();
        }
        listeners.call (&ExpanderView::Listener::expanderParamsChanged, this, mParams);
        
    }

    void updateParams(const SonoAudio::CompressorParams & params) {
        mParams = params;
        
        thresholdSlider.setValue(mParams.thresholdDb, dontSendNotification);
        ratioSlider.setValue(mParams.ratio, dontSendNotification);
        attackSlider.setValue(mParams.attackMs, dontSendNotification);
        releaseSlider.setValue(mParams.releaseMs, dontSendNotification);
        holdSlider.setValue(mParams.holdMs, dontSendNotification);
        gateButton.setToggleState(mParams.gate, dontSendNotification);
        
        enableButton.setAlpha(mParams.enabled ? 1.0 : 0.5);
        enableButton.setToggleState(mParams.enabled, dontSendNotification);
        headerComponent.repaint();
    }
    
    // Live indicator next to the Hard gate box — always shown: dim "off", teal "open", grey "closed"
    void setGateState(int state) {
        if (state == lastGateState) return;
        lastGateState = state;
        // Always visible — off / open / closed
        if (state < 0 || !mParams.gate) {
            gateStateLabel.setText(String::fromUTF8("\xe2\x97\x8f ") + TRANS("off"), dontSendNotification);
            gateStateLabel.setColour(Label::textColourId, Colour::fromFloatRGBA(0.5f, 0.5f, 0.5f, 0.6f));
            return;
        }
        gateStateLabel.setText(String::fromUTF8("\xe2\x97\x8f ") + (state > 0 ? TRANS("open") : TRANS("closed")), dontSendNotification);
        gateStateLabel.setColour(Label::textColourId, state > 0 ? Colour::fromFloatRGBA(0.18f, 0.76f, 0.70f, 1.0f) : Colour::fromFloatRGBA(0.5f, 0.5f, 0.5f, 0.8f));
    }
    // Hard gate is a SEND-side feature (it is what lets the idle-channel mask drop the channel).
    // On a player row this panel processes audio already received, so the box and its indicator are hidden there.
    void setHardGateAvailable(bool avail) {
        gateButton.setVisible(avail); gateStateLabel.setVisible(avail);
        if (!avail && mParams.gate) { mParams.gate = false; gateButton.setToggleState(false, dontSendNotification); }
    }
    const SonoAudio::CompressorParams & getParams() const {
        return mParams;         
    }
    
   
    
private:
    

    ListenerList<Listener> listeners;
    
    Slider thresholdSlider;
    Slider ratioSlider;
    Slider attackSlider;
    Slider releaseSlider;
    Slider holdSlider;          
    ToggleButton gateButton;    
    TextButton resetButton;     
    Label gateStateLabel;       
    int lastGateState = -2;
    FlexBox gateRowBox;
    Label holdLabel;
    FlexBox holdBox;

    Label thresholdLabel;
    Label ratioLabel;
    Label attackLabel;
    Label releaseLabel;

    
    FlexBox mainBox;
    FlexBox checkBox;
    FlexBox knobBox;
    FlexBox threshBox;
    FlexBox ratioBox;
    FlexBox attackBox;
    FlexBox releaseBox;


    SonoAudio::CompressorParams mParams;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExpanderView)
};
