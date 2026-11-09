// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2020 Jesse Chappell
// Modifications Copyright (C) 2026 Jason Buecker (Random Planet)

#include "PeersContainerView.h"
#include "SbLatencyColours.h"
#include "SbRateFormat.h"   // Bits everywhere, one rule with the hub
#include "JitterBufferMeter.h"
#include <set>

using namespace SonoAudio;

PeerViewInfo::PeerViewInfo() : smallLnf(12), medLnf(14), sonoSliderLNF(12), panSliderLNF(12)
{
    bgColor = Colour::fromFloatRGBA(0.112f, 0.112f, 0.112f, 1.0f);
    borderColor = Colour::fromFloatRGBA(0.5f, 0.5f, 0.5f, 0.3f);

    sonoSliderLNF.textJustification = Justification::centredLeft;
    panSliderLNF.textJustification = Justification::centredLeft;

    setFocusContainerType(FocusContainerType::focusContainer);

    //Random rcol;
    //itemColor = Colour::fromHSV(rcol.nextFloat(), 0.5f, 0.2f, 1.0f);
}

PeerViewInfo::~PeerViewInfo()
{
    
}

enum {
    LabelTypeRegular = 0,
    LabelTypeSmallDim ,
    LabelTypeSmall
};

enum {
    FillRatioUpdateTimerId = 0
};

void PeerViewInfo::paint(Graphics& g) 
{
    //g.fillAll (Colour(0xff111111));
    //g.fillAll (Colour(0xff202020));
    
    g.setColour(bgColor);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);

    g.setColour(borderColor);
    g.drawRoundedRectangle(getLocalBounds().toFloat(), 6.0f, 0.5f);

}

void PeerViewInfo::resized()
{
    auto localbounds = getLocalBounds();

    int chantargwidth = localbounds.getWidth() + 8;

    if (channelGroups->getEstimatedWidth() != chantargwidth && channelGroups->isVisible()) {
        channelGroups->setEstimatedWidth(chantargwidth);
        channelGroups->updateLayout(false);
    }

    mainbox.performLayout(localbounds);

    // Expanded relayed rows: one meter per channel group, right-aligned in the meter column and spanning the group's
    // rows, so every instrument row is the same height. A lone group has the row to itself and runs to the bottom.
    if (groupMeterColumn && channelGroups && groupMeters.size() > 0) {
        const auto& gbs = channelGroups->getGroupBounds();
        const int colX = groupMeterColumn->getX(), colW = groupMeterColumn->getWidth(), cgY = channelGroups->getY();
        int visibleGroups = 0;
        for (int g = 0; g < groupMeters.size(); ++g) if (groupMeters[g]->isVisible() && g < gbs.size()) ++visibleGroups;
        for (int g = 0; g < groupMeters.size(); ++g) {
            auto* m = groupMeters[g];
            if (!m->isVisible() || g >= gbs.size()) continue;
            const auto gb = gbs.getReference(g);
            const int w = kSbMeterPxPerChannel * (g < groupMeterChans.size() ? groupMeterChans[g] : 1);
            const int y = cgY + gb.getY();
            const int h = jmax(8, visibleGroups == 1 ? getHeight() - y - 2 : gb.getHeight());
            m->setBounds(colX + (colW - w), y, w, h);
        }
    }



    
    if (latActiveButton) {
        //latActiveButton->setBounds(staticPingLabel->getX(), staticPingLabel->getY(), pingLabel->getRight() - staticPingLabel->getX(), latencyLabel->getBottom() - pingLabel->getY());
        latActiveButton->setBounds(staticLatencyLabel->getX(), staticLatencyLabel->getY(), pingLabel->getRight() - staticLatencyLabel->getX(), latencyUpLabel->getBottom() - staticLatencyLabel->getY());

        auto arrwidth = 10;
        auto arrheight = 14;
        auto urect = Rectangle<int>(latencyUpLabel->getX() - arrwidth, latencyUpLabel->getY() + (latencyUpLabel->getHeight() - arrheight)/2, arrwidth, arrheight);
        latUpArrow->setTransformToFit(urect.toFloat(), RectanglePlacement::stretchToFit);
        auto drect = Rectangle<int>(latencyDownLabel->getX() - arrwidth, latencyDownLabel->getY() + (latencyDownLabel->getHeight() - arrheight)/2, arrwidth, arrheight);
        latDownArrow->setTransformToFit(drect.toFloat(), RectanglePlacement::stretchToFit);
    }

    int triwidth = 10;
    int triheight = 6;

    // Relayed rows (hideSendCluster) drop staticSendQualLabel from the layout, so the send cluster's vertical band is
    // bounded by sendQualityLabel (top) and, in narrow mode, sendActualBitrateLabel (bottom; updateLayout puts it in
    // squalbox for those rows) instead of the stale static-label bounds.
    Label * squalTop = hideSendCluster ? sendQualityLabel.get() : staticSendQualLabel.get();
    Label * squalBottomNarrow = hideSendCluster ? sendActualBitrateLabel.get() : sendQualityLabel.get();

    if (recvOptionsButton && latActiveButton) {
        auto leftedge = (sendActualBitrateLabel->getRight() + (recvActualBitrateLabel->getX() - sendActualBitrateLabel->getRight()) / 2) + 2;
        if (isNarrow) {
            if (hideBufferWidgets) {
                // Hosted mode: the buffer labels are not laid out, so their bounds are stale. Anchor the recv-options
                // overlay to the recv bitrate label, which updateLayout puts in their place, and match the send
                // cluster's vertical band.
                leftedge = (squalTop->getRight() + (recvActualBitrateLabel->getX() - squalTop->getRight()) / 2) + 2;
                recvOptionsButton->setBounds(leftedge, squalTop->getY(), recvActualBitrateLabel->getRight() - leftedge + 2, squalBottomNarrow->getBottom() - squalTop->getY());
            } else {
                //leftedge = 4;
                leftedge = (staticSendQualLabel->getRight() + (bufferLabel->getX() - staticSendQualLabel->getRight()) / 2) + 2;

                recvOptionsButton->setBounds(leftedge, staticBufferLabel->getY(), bufferLabel->getRight() - leftedge + 2, bufferLabel->getBottom() - staticBufferLabel->getY());
            }
        }
        else if (hideBufferWidgets) {
            // Hosted mode: buffer labels are not laid out (see above); span the same vertical band as the send
            // cluster so the two overlays stay symmetric. The right edge anchors to the recv bitrate label because
            // netstatbox holds no flex slot in hosted mode, so latActiveButton's bounds are stale too.
            recvOptionsButton->setBounds(leftedge, squalTop->getY(), recvActualBitrateLabel->getRight() - leftedge + 2, sendActualBitrateLabel->getBottom() - squalTop->getY());
        }
        else {
            recvOptionsButton->setBounds(leftedge, staticBufferLabel->getY(), latActiveButton->getX() - leftedge - 5, recvActualBitrateLabel->getBottom() - staticBufferLabel->getY());
        }


        if (recvOptionsButton->getWidth() > 260) {
            int buttw = recvOptionsButton->getHeight() - 4;
            bufferMinFrontButton->setBounds(recvOptionsButton->getRight() - buttw - 2, recvOptionsButton->getY() + 2, buttw, buttw);
            // Gated on hideBufferWidgets so hosted mode keeps the button hidden through every layout pass.
            bufferMinFrontButton->setVisible(false);
        } else {
            bufferMinFrontButton->setVisible(false);
        }

        if (isNarrow) {
            auto rect = Rectangle<int>(recvOptionsButton->getX() +  3, recvOptionsButton->getBottom() - recvOptionsButton->getHeight()/2 - triheight + 2, triwidth, triheight);
            recvButtonImage->setTransformToFit(rect.toFloat(), RectanglePlacement::stretchToFit);
        }
        else {
            auto rect = Rectangle<int>(recvOptionsButton->getX() +  3, recvOptionsButton->getBottom() - triheight - 1, triwidth, triheight);
            recvButtonImage->setTransformToFit(rect.toFloat(), RectanglePlacement::stretchToFit);
        }
    }

    // Relayed rows hide the send-options overlay entirely (hideSendCluster): skip its anchoring, which reads the
    // un-laid-out staticSendQualLabel. updateLayout hides sendStatsBg with it.
    if (sendOptionsButton && !hideSendCluster) {
        int rightedge;
        if (isNarrow) {
            // Hosted mode: staticBufferLabel is not laid out, so anchor the send overlay's right edge to the recv
            // bitrate label that takes its place in the middle cluster.
            if (hideBufferWidgets) {
                rightedge = (sendQualityLabel->getRight() + (recvActualBitrateLabel->getX() - sendQualityLabel->getRight()) / 2) - 3;
            } else {
                rightedge = (sendQualityLabel->getRight() + (staticBufferLabel->getX() - sendQualityLabel->getRight()) / 2) - 3;
            }

            sendOptionsButton->setBounds(staticSendQualLabel->getX(), staticSendQualLabel->getY(), rightedge - staticSendQualLabel->getX(), sendQualityLabel->getBottom() - staticSendQualLabel->getY());

            auto rect = Rectangle<int>(sendOptionsButton->getX() + 3, sendOptionsButton->getBottom() - sendOptionsButton->getHeight()/2 - triheight + 2, triwidth, triheight);
            sendButtonImage->setTransformToFit(rect.toFloat(), RectanglePlacement::stretchToFit);
        }
        else {
            rightedge = (sendActualBitrateLabel->getRight() + (recvActualBitrateLabel->getX() - sendActualBitrateLabel->getRight()) / 2) - 3;

            sendOptionsButton->setBounds(staticSendQualLabel->getX(), staticSendQualLabel->getY(), rightedge - staticSendQualLabel->getX(), sendActualBitrateLabel->getBottom() - staticSendQualLabel->getY());

            auto rect = Rectangle<int>(sendOptionsButton->getX() + 3, sendOptionsButton->getBottom() - triheight - 1, triwidth, triheight);
            sendButtonImage->setTransformToFit(rect.toFloat(), RectanglePlacement::stretchToFit);
        }
    }


    //Rectangle<int> optbounds(staticSendQualLabel->getX(), staticSendQualLabel->getY(), sendQualityLabel->getRight() - staticSendQualLabel->getX(), bufferLabel->getBottom() - sendQualityLabel->getY());
    //optionsButton->setBounds(optbounds);
    
    if (jitterBufferMeter) {
        jitterBufferMeter->setBounds(bufferLabel->getBounds().reduced(0, 1));
    }
              
    if (sendStatsBg) {
        auto sendbounds = sendOptionsButton->getBounds();
        sendStatsBg->setRectangle (sendbounds.toFloat().expanded(1.0f));

        auto recvbounds = recvOptionsButton->getBounds();
        recvStatsBg->setRectangle (recvbounds.toFloat().expanded(1.0f));

        auto pingbounds = latActiveButton->getBounds();
        //if (pingbounds.getBottom() < recvbounds.getY()) {
        //    pingbounds.setBottom(recvbounds.getY() + 10);
        //}
        pingBg->setRectangle (pingbounds.toFloat().expanded(1.0f));
    }

    if (!isNarrow) {
        auto arrwidth = 10;
        auto arrheight = 14;
        auto srect = Rectangle<int>(sendActualBitrateLabel->getX() - arrwidth, sendActualBitrateLabel->getY() + (sendActualBitrateLabel->getHeight() - arrheight)/2, arrwidth, arrheight);
        sendUpArrow->setTransformToFit(srect.toFloat(), RectanglePlacement::stretchToFit);
        auto rrect = Rectangle<int>(recvActualBitrateLabel->getX() - arrwidth, recvActualBitrateLabel->getY() + (recvActualBitrateLabel->getHeight() - arrheight)/2, arrwidth, arrheight);
        recvDownArrow->setTransformToFit(rrect.toFloat(), RectanglePlacement::stretchToFit);
    }
}

/// --------------------------------------------
#pragma PendingPeerViewInfo


PendingPeerViewInfo::PendingPeerViewInfo()
{
    bgColor = Colour::fromFloatRGBA(0.112f, 0.112f, 0.112f, 1.0f);
    borderColor = Colour::fromFloatRGBA(0.5f, 0.5f, 0.5f, 0.3f);

    //Random rcol;
    //itemColor = Colour::fromHSV(rcol.nextFloat(), 0.5f, 0.2f, 1.0f);
}

PendingPeerViewInfo::~PendingPeerViewInfo()
{
    
}

void PendingPeerViewInfo::paint(Graphics& g) 
{
    //g.fillAll (Colour(0xff111111));
    //g.fillAll (Colour(0xff202020));
    
    g.setColour(bgColor);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);

    g.setColour(borderColor);
    g.drawRoundedRectangle(getLocalBounds().toFloat(), 6.0f, 0.5f);

}

void PendingPeerViewInfo::resized()
{
    
    mainbox.performLayout(getLocalBounds());
}


PeersContainerView::PeersContainerView(SonobusAudioProcessor& proc)
 : Component("pcv"),  processor(proc)
{
    mutedTextColor = Colour::fromFloatRGBA(0.8, 0.5, 0.2, 1.0);
    regularTextColor = Colour(0xa0eeeeee);; //Colour(0xc0eeeeee);
    dimTextColor = Colour(0xa0aaaaaa); //Colour(0xc0aaaaaa);
    //soloColor = Colour::fromFloatRGBA(0.2, 0.5, 0.8, 1.0);
    mutedColor = Colour::fromFloatRGBA(0.6, 0.3, 0.1, 1.0);
    soloColor = Colour::fromFloatRGBA(1.0, 1.0, 0.6, 1.0);
    mutedBySoloColor = Colour::fromFloatRGBA(0.25, 0.125, 0.0, 1.0);
    
    droppedTextColor = Colour(0xc0ee8888);
    // Latency per leg green <12 ms, yellow 12-25, red >25; drops/min green 0, yellow 1-5, red >5
    latGoodColor = Colour(0xd06abe4f); latWarnColor = Colour(0xd0d6b03e); latBadColor = Colour(0xd0ee6666);

    outlineColor = Colour::fromFloatRGBA(0.25, 0.25, 0.25, 1.0);
    bgColor = Colours::black;

    peerModeFull = processor.getPeerDisplayMode() == SonobusAudioProcessor::PeerDisplayModeFull;

    //setFocusContainerType(FocusContainerType::focusContainer);

    mDragDrawable = std::make_unique<DrawableImage>();
    mDragDrawable->setAlpha(0.4f);
    mDragDrawable->setAlwaysOnTop(true);
    addChildComponent(mDragDrawable.get());

    mInsertLine = std::make_unique<DrawableRectangle>();
    //mInsertLine->setCornerSize(Point<float>(6,6));
    //mInsertLine->setFill (Colour::fromFloatRGBA(0.07, 0.07, 0.07, 1.0));
    mInsertLine->setFill (Colours::transparentBlack);
    mInsertLine->setStrokeFill (Colour::fromFloatRGBA(0.5, 0.5, 0.5, 0.75));
    mInsertLine->setStrokeThickness(2);
    addChildComponent(mInsertLine.get());

    // HostedMix hub-status header (replaces the SonoBus-Hub peer row).
    mHubStatusLabel = std::make_unique<SbRichLabel>("hubstatus", "");   // Per-field colours
    mHubStatusLabel->setJustificationType(Justification::centredLeft);
    // Large enough to read the hub-connection latency numbers at a glance from across the room. The row height and
    // peersheight below match this size.
    mHubStatusLabel->setFont(20);
    mHubStatusLabel->setColour(Label::textColourId, regularTextColor);
    mHubStatusLabel->setInterceptsMouseClicks(true, false);   // Right-click = reset drop counts
    mHubStatusLabel->addMouseListener(this, false);
    addChildComponent(mHubStatusLabel.get());


    // HostedMix unified receive-buffer controls: one knob for all players, because every feed shares the hub->us
    // network path. Widget config mirrors the per-row bufferTimeSlider / autosizeButton set in createPeerViewInfo.
    // The widgets live in a popup launched from a compact value chip on the hub header row (see
    // showUnifiedBufferOptions), like the per-peer recvOptionsContainer.
    mUnifiedOptionsContainer = std::make_unique<Component>();
    mUnifiedOptionsContainer->setFocusContainerType(FocusContainerType::focusContainer);

    mUnifiedBufStaticLabel = std::make_unique<Label>("unibufst", TRANS("Recv Jitter Buffer (all players):"));
    configLabel(mUnifiedBufStaticLabel.get(), LabelTypeSmallDim);
    mUnifiedBufStaticLabel->setJustificationType(Justification::centredLeft);
    mUnifiedOptionsContainer->addAndMakeVisible(mUnifiedBufStaticLabel.get());

    mUnifiedBufLabel = std::make_unique<Label>("unibuf");
    configLabel(mUnifiedBufLabel.get(), LabelTypeSmall);
    mUnifiedBufLabel->setJustificationType(Justification::centredRight);
    mUnifiedOptionsContainer->addAndMakeVisible(mUnifiedBufLabel.get());

    mUnifiedBufferTimeSlider = std::make_unique<Slider>(Slider::LinearBar, Slider::TextBoxBelow);
    mUnifiedBufferTimeSlider->setName("unibuffer");
    mUnifiedBufferTimeSlider->setTitle(TRANS("Jitter Buffer (all players)"));
    mUnifiedBufferTimeSlider->setRange(0, 5000, 1);
    mUnifiedBufferTimeSlider->setTextValueSuffix(" ms");
    mUnifiedBufferTimeSlider->setSkewFactor(0.25);
    mUnifiedBufferTimeSlider->setDoubleClickReturnValue(true, 20.0);
    mUnifiedBufferTimeSlider->setTextBoxIsEditable(true);
    mUnifiedBufferTimeSlider->setSliderSnapsToMousePosition(false);
    mUnifiedBufferTimeSlider->setChangeNotificationOnlyOnRelease(true);
    mUnifiedBufferTimeSlider->setScrollWheelEnabled(false);
    mUnifiedBufferTimeSlider->setPopupDisplayEnabled(true, false, this);
    mUnifiedBufferTimeSlider->setColour(Slider::trackColourId, Colour::fromFloatRGBA(0.1, 0.4, 0.6, 0.3));
    mUnifiedBufferTimeSlider->addListener(this);
    mUnifiedOptionsContainer->addAndMakeVisible(mUnifiedBufferTimeSlider.get());

    mUnifiedAutosizeButton = std::make_unique<SonoChoiceButton>();
    mUnifiedAutosizeButton->addChoiceListener(this);
#if SONOBUSPLUS_BUILD
    // Two choices. In hub mode every automatic mode runs the same adaptive receive buffer, so "Auto" is the hosted
    // default (measure-commit-pin); Manual pins at the slider.
    mUnifiedAutosizeButton->addItem(TRANS("Manual"), SonobusAudioProcessor::AutoNetBufferModeOff);
    mUnifiedAutosizeButton->addItem(TRANS("Auto"), SonobusAudioProcessor::AutoNetBufferModeMeasureCommitPin);
#else
    mUnifiedAutosizeButton->addItem(TRANS("Manual"), SonobusAudioProcessor::AutoNetBufferModeOff);
    mUnifiedAutosizeButton->addItem(TRANS("Auto Up"), SonobusAudioProcessor::AutoNetBufferModeAutoIncreaseOnly);
    mUnifiedAutosizeButton->addItem(TRANS("Auto"), SonobusAudioProcessor::AutoNetBufferModeAutoFull);
    mUnifiedAutosizeButton->addItem(TRANS("Initial Auto"), SonobusAudioProcessor::AutoNetBufferModeInitAuto);
    mUnifiedAutosizeButton->addItem(TRANS("Smart Pin"), SonobusAudioProcessor::AutoNetBufferModeMeasureCommitPin); // Measure-commit-pin
#endif
    mUnifiedOptionsContainer->addAndMakeVisible(mUnifiedAutosizeButton.get());

    // Reset the hub-wide drops aggregate and every per-row count in one click: same processor call as the per-peer
    // Reset Dropped button, applied to all peers.
    mUnifiedResetDropsButton = std::make_unique<TextButton>(TRANS("Reset Drop Counts"));
    mUnifiedResetDropsButton->addListener(this);
    mUnifiedResetDropsButton->setTooltip(TRANS("Reset the dropped-packet counters for all players"));
    mUnifiedOptionsContainer->addAndMakeVisible(mUnifiedResetDropsButton.get());

    // The compact chip on the right of the hub header row. updatePeerViews keeps its text in sync; a click toggles
    // the popup.
    mUnifiedBufChipButton = std::make_unique<TextButton>("");
    mUnifiedBufChipButton->addListener(this);
    mUnifiedBufChipButton->setTitle(TRANS("Recv Jitter Buffer (all players)"));
    mUnifiedBufChipButton->setTooltip(TRANS("Receive jitter buffer for all players — click to adjust"));
    addChildComponent(mUnifiedBufChipButton.get());

    // Buffer the static hub-header widgets to images. The 8 Hz meter repaints dirty rects that JUCE coalesces, and
    // every unbuffered neighbour intersecting the region re-rasterises its glyphs and paths each time. JUCE
    // re-renders the cache whenever the widget itself calls repaint() (setText/setColour/setValue/toggle all do), so
    // nothing can go stale. Meters are never buffered.
    for (Component* c : std::initializer_list<Component*>{
            mHubStatusLabel.get(), mUnifiedBufStaticLabel.get(), mUnifiedBufLabel.get(),
            mUnifiedBufferTimeSlider.get(), mUnifiedAutosizeButton.get(),
            mUnifiedResetDropsButton.get(), mUnifiedBufChipButton.get() }) {
        if (c) c->setBufferedToImage(true);
    }

    rebuildPeerViews();
}

void PeersContainerView::configLevelSlider(Slider * slider)
{
    //slider->setTextValueSuffix(" dB");
    slider->setColour(Slider::textBoxBackgroundColourId, Colours::transparentBlack);
    slider->setColour(Slider::textBoxOutlineColourId, Colours::transparentBlack);
    slider->setColour(Slider::textBoxTextColourId, Colour(0x90eeeeee));
    slider->setColour(TooltipWindow::textColourId, Colour(0xf0eeeeee));

    
    slider->setTextBoxStyle(Slider::TextBoxAbove, true, 80, 12);

    slider->setRange(0.0, 2.0, 0.0);
    slider->setSkewFactor(0.4);
    slider->setDoubleClickReturnValue(true, 1.0);
    slider->setTextBoxIsEditable(true);
    slider->setSliderSnapsToMousePosition(false);
    slider->setScrollWheelEnabled(false);
    slider->valueFromTextFunction = [](const String& s) -> float { return Decibels::decibelsToGain(s.getFloatValue()); };
    slider->textFromValueFunction = [](float v) -> String { return String(TRANS("Level: ")) + Decibels::toString(Decibels::gainToDecibels(v), 1); };
#if JUCE_IOS
    //slider->setPopupDisplayEnabled(true, false, this);
#endif
}

void PeersContainerView::configKnobSlider(Slider * slider)
{
    slider->setSliderStyle(Slider::SliderStyle::RotaryHorizontalVerticalDrag);
    slider->setTextBoxStyle(Slider::TextBoxAbove, true, 60, 14);
    slider->setMouseDragSensitivity(128);
    slider->setScrollWheelEnabled(false);
    slider->setTextBoxIsEditable(true);
    slider->setSliderSnapsToMousePosition(false);
    //slider->setPopupDisplayEnabled(true, false, this);
    slider->setColour(Slider::textBoxBackgroundColourId, Colours::transparentBlack);
    slider->setColour(Slider::textBoxOutlineColourId, Colours::transparentBlack);
    slider->setColour(Slider::textBoxTextColourId, Colour(0x90eeeeee));
    slider->setColour(TooltipWindow::textColourId, Colour(0xf0eeeeee));

    //slider->setLookAndFeel(&sonoSliderLNF);
    
    //slider->valueFromTextFunction = [](const String& s) -> float { return Decibels::decibelsToGain(s.getFloatValue()); };
    //slider->textFromValueFunction = [](float v) -> String { return Decibels::toString(Decibels::gainToDecibels(v), 1); };
#if JUCE_IOS
    //slider->setPopupDisplayEnabled(true, false, this);
#endif
}

void PeersContainerView::configLabel(Label *label, int ltype)
{
    if (ltype == LabelTypeSmallDim) {
        label->setFont(12);
        label->setColour(Label::textColourId, dimTextColor);
        label->setJustificationType(Justification::centredRight);        
        label->setMinimumHorizontalScale(0.3);
    }
    else if (ltype == LabelTypeSmall) {
        label->setFont(12);
        label->setColour(Label::textColourId, regularTextColor);
        label->setJustificationType(Justification::centredRight);
        label->setMinimumHorizontalScale(0.3);
    }
    else {
        label->setFont(14);
        //label->setColour(Label::textColourId, Colour(0xaaeeeeee));
        label->setJustificationType(Justification::centredLeft);
    }
}





void PeersContainerView::resized()
{
    Rectangle<int> bounds = getLocalBounds().reduced(5, 0);
    bounds.removeFromLeft(3);

    // if the width has changed, need to rebuild layout potentially
    if (mLastWidth != bounds.getWidth()) {
        mLastWidth = bounds.getWidth();
        updateLayout();
    }

    peersBox.performLayout(bounds);

    mPeerViewBounds.clearQuick();

    for (int i=0; i < mPeerViews.size(); ++i) {
        PeerViewInfo * pvf = mPeerViews.getUnchecked(i);
        pvf->resized();

        mPeerViewBounds.add(pvf->getBounds());
    }
    
    Component* dw = nullptr; // this->findParentComponentOfClass<DocumentWindow>();    
    if (!dw)
        dw = this->findParentComponentOfClass<AudioProcessorEditor>();
    if (!dw)
        dw = this->findParentComponentOfClass<Component>();
    if (!dw)
        dw = this;

    if (auto * callout = dynamic_cast<CallOutBox*>(pannerCalloutBox.get())) {
        callout->dismiss();
        pannerCalloutBox = nullptr;
    }
    if (auto * callout = dynamic_cast<CallOutBox*>(sendOptionsCalloutBox.get())) {
        callout->dismiss();
        sendOptionsCalloutBox = nullptr;
    }
    if (auto * callout = dynamic_cast<CallOutBox*>(recvOptionsCalloutBox.get())) {
        callout->dismiss();
        recvOptionsCalloutBox = nullptr;
    }
    if (auto * callout = dynamic_cast<CallOutBox*>(effectsCalloutBox.get())) {
        callout->dismiss();
        effectsCalloutBox = nullptr;
    }
    // Unified receive-buffer popup is dismissed on resize like every other callout above (its chip anchor moves with
    // layout).
    if (auto * callout = dynamic_cast<CallOutBox*>(unifiedBufCalloutBox.get())) {
        callout->dismiss();
        unifiedBufCalloutBox = nullptr;
    }


}

void PeersContainerView::showPopTip(const String & message, int timeoutMs, Component * target, int maxwidth)
{
    popTip.reset(new BubbleMessageComponent());
    popTip->setAllowedPlacement(BubbleComponent::above);
    
    if (target) {
        if (auto * parent = target->findParentComponentOfClass<AudioProcessorEditor>()) {
            parent->addChildComponent (popTip.get());
        } else {
            addChildComponent(popTip.get());            
        }
    }
    else {
        addChildComponent(popTip.get());
    }
    
    AttributedString text(message);
    text.setJustification (Justification::centred);
    text.setColour (findColour (TextButton::textColourOffId));
    text.setFont(Font(12 * SonoLookAndFeel::getFontScale()));
    if (target) {
        popTip->showAt(target, text, timeoutMs);
    }
    else {
        Rectangle<int> topbox(getWidth()/2 - maxwidth/2, 0, maxwidth, 2);
        popTip->showAt(topbox, text, timeoutMs);
    }
    popTip->toFront(false);
    // TODO make sure it is read for accessibility
    popTip->setWantsKeyboardFocus(true);
    popTip->setTitle(message);
    popTip->setAccessible(true);
    popTip->grabKeyboardFocus();
    //AccessibilityHandler::postAnnouncement(message, AccessibilityHandler::AnnouncementPriority::medium);
}

void PeersContainerView::paint(Graphics & g)
{    
    //g.fillAll (Colours::black);
    Rectangle<int> bounds = getLocalBounds();

    bounds.reduce(1, 1);
    bounds.removeFromLeft(3);
    
    g.setColour(bgColor);
    g.fillRoundedRectangle(bounds.toFloat(), 6.0f);
    g.setColour(outlineColor);
    g.drawRoundedRectangle(bounds.toFloat(), 6.0f, 0.5f);

}

PeerViewInfo * PeersContainerView::createPeerViewInfo()
{
    PeerViewInfo * pvf = new PeerViewInfo();

    pvf->addrLabel = std::make_unique<TextEditor>("addr");
    pvf->addrLabel->setJustification(Justification::centred);
    pvf->addrLabel->setColour(TextEditor::textColourId, dimTextColor);
    pvf->addrLabel->setColour(TextEditor::backgroundColourId, Colours::transparentBlack);
    pvf->addrLabel->setColour(TextEditor::outlineColourId, Colours::transparentBlack);
    pvf->addrLabel->setReadOnly(true);
    pvf->addrLabel->setCaretVisible(false);
    pvf->addrLabel->setFont(13 * SonoLookAndFeel::getFontScale());
    pvf->addrLabel->addMouseListener(this, false);

    pvf->staticAddrLabel = std::make_unique<Label>("addr", TRANS("Remote address:"));
    pvf->staticAddrLabel->setJustificationType(Justification::centredRight);
    configLabel(pvf->staticAddrLabel.get(), LabelTypeSmallDim);
    pvf->staticAddrLabel->setFont(13);
    
    
    pvf->sendMutedButton = std::make_unique<ToggleButton>(TRANS("Disable Sending"));
    pvf->sendMutedButton->addListener(this);

    pvf->recvMutedButton = std::make_unique<TextButton>(TRANS("MUTE"));
    pvf->recvMutedButton->addListener(this);
    pvf->recvMutedButton->setLookAndFeel(&pvf->medLnf);
    pvf->recvMutedButton->setClickingTogglesState(true);
    pvf->recvMutedButton->setColour(TextButton::buttonOnColourId, mutedColor);
    pvf->recvMutedButton->setTooltip(TRANS("Toggles receive muting, preventing audio from being heard for this user"));

    pvf->recvSoloButton = std::make_unique<TextButton>(TRANS("SOLO"));
    pvf->recvSoloButton->addListener(this);
    pvf->recvSoloButton->setLookAndFeel(&pvf->medLnf);
    pvf->recvSoloButton->setClickingTogglesState(true);
    pvf->recvSoloButton->setColour(TextButton::buttonOnColourId, soloColor.withAlpha(0.7f));
    pvf->recvSoloButton->setColour(TextButton::textColourOnId, Colours::darkblue);
    pvf->recvSoloButton->setTooltip(TRANS("Listen to only this user, and other soloed users. Alt-click to exclusively solo this user."));

    
    pvf->latActiveButton = std::make_unique<SonoDrawableButton>("", DrawableButton::ButtonStyle::ImageFitted);
    pvf->latActiveButton->setColour(SonoTextButton::outlineColourId, Colours::transparentBlack);
    pvf->latActiveButton->setColour(DrawableButton::backgroundOnColourId, Colour::fromFloatRGBA(0.14, 0.50, 0.46, 0.7));
    pvf->latActiveButton->setColour(DrawableButton::backgroundColourId, Colours::transparentBlack);
    pvf->latActiveButton->setClickingTogglesState(false);
    pvf->latActiveButton->setTriggeredOnMouseDown(false);
    pvf->latActiveButton->setLookAndFeel(&pvf->smallLnf);
    pvf->latActiveButton->addListener(this);
    pvf->latActiveButton->addMouseListener(this, false);
    pvf->latActiveButton->setTitle(TRANS("Latency"));

    pvf->statusLabel = std::make_unique<Label>("status", "");
    configLabel(pvf->statusLabel.get(), LabelTypeRegular);
    pvf->statusLabel->setJustificationType(Justification::centredLeft);

    
    
    pvf->bufferTimeSlider     = std::make_unique<Slider>(Slider::LinearBar,  Slider::TextBoxBelow);
    pvf->bufferTimeSlider->setName("buffer");
    pvf->bufferTimeSlider->setTitle(TRANS("Jitter Buffer"));
    pvf->bufferTimeSlider->setRange(0, 5000, 1);
    pvf->bufferTimeSlider->setTextValueSuffix(" ms");
    //pvf->bufferTimeSlider->getProperties().set ("noFill", true);
    pvf->bufferTimeSlider->setSkewFactor(0.25);
    pvf->bufferTimeSlider->setDoubleClickReturnValue(true, 20.0);
    pvf->bufferTimeSlider->setTextBoxIsEditable(true);
    pvf->bufferTimeSlider->setSliderSnapsToMousePosition(false);
    pvf->bufferTimeSlider->setChangeNotificationOnlyOnRelease(true);
    pvf->bufferTimeSlider->setScrollWheelEnabled(false);
    pvf->bufferTimeSlider->setPopupDisplayEnabled(true, false, this);
    pvf->bufferTimeSlider->setColour(Slider::trackColourId, Colour::fromFloatRGBA(0.1, 0.4, 0.6, 0.3));

    pvf->bufferTimeSlider->addListener(this);

    pvf->autosizeButton = std::make_unique<SonoChoiceButton>();
    pvf->autosizeButton->addChoiceListener(this);
    pvf->autosizeButton->addItem(TRANS("Manual"), SonobusAudioProcessor::AutoNetBufferModeOff);
    pvf->autosizeButton->addItem(TRANS("Auto Up"), SonobusAudioProcessor::AutoNetBufferModeAutoIncreaseOnly);
    pvf->autosizeButton->addItem(TRANS("Auto"), SonobusAudioProcessor::AutoNetBufferModeAutoFull);
    pvf->autosizeButton->addItem(TRANS("Initial Auto"), SonobusAudioProcessor::AutoNetBufferModeInitAuto);
    pvf->autosizeButton->addItem(TRANS("Smart Pin"), SonobusAudioProcessor::AutoNetBufferModeMeasureCommitPin); // Measure-commit-pin
    pvf->autosizeButton->addListener(this);
    
    pvf->bufferMinButton = std::make_unique<SonoDrawableButton>("", DrawableButton::ButtonStyle::ImageFitted);
    std::unique_ptr<Drawable> backimg(Drawable::createFromImageData(BinaryData::reset_buffer_icon_svg, BinaryData::reset_buffer_icon_svgSize));
    pvf->bufferMinButton->setImages(backimg.get());
    pvf->bufferMinButton->addListener(this);
    pvf->bufferMinButton->setTooltip(TRANS("Resets jitter buffer to the minimum. Hold Alt key to reset for all (with auto)."));
    pvf->bufferMinButton->setTitle(TRANS("Reset Jitter Buffer"));
    pvf->bufferMinButton->setAlpha(0.8f);

    pvf->bufferMinFrontButton = std::make_unique<SonoDrawableButton>("", DrawableButton::ButtonStyle::ImageFitted);
    pvf->bufferMinFrontButton->setImages(backimg.get());
    pvf->bufferMinFrontButton->addListener(this);
    pvf->bufferMinFrontButton->setTooltip(TRANS("Resets jitter buffer to the minimum. Hold Alt key to reset for all (with auto)."));
    pvf->bufferMinFrontButton->setTitle(TRANS("Reset Jitter Buffer"));
    pvf->bufferMinFrontButton->setAlpha(0.8f);
    
    pvf->recvButtonImage = Drawable::createFromImageData(BinaryData::triangle_disclosure_svg, BinaryData::triangle_disclosure_svgSize);
    pvf->recvButtonImage->setInterceptsMouseClicks(false, false);
    pvf->recvButtonImage->setAlpha(0.7f);
    
    
    pvf->sendButtonImage = Drawable::createFromImageData(BinaryData::triangle_disclosure_svg, BinaryData::triangle_disclosure_svgSize);
    pvf->sendButtonImage->setInterceptsMouseClicks(false, false);
    pvf->sendButtonImage->setAlpha(0.7f);

    
    pvf->bufferTimeLabel = std::make_unique<Label>("buf", TRANS("Jitter Buffer"));
    pvf->bufferTimeLabel->setTooltip(TRANS("Receive jitter buffer for this player's stream (ms). Auto grows on dropouts and settles back toward what the link needs; Manual pins it."));
    configLabel(pvf->bufferTimeLabel.get(), LabelTypeRegular);
    pvf->bufferTimeLabel->setAccessible(false);

    pvf->recvOptionsButton = std::make_unique<SonoDrawableButton>("menu", DrawableButton::ImageFitted);
    pvf->recvOptionsButton->addListener(this);
    pvf->recvOptionsButton->setColour(SonoTextButton::outlineColourId, Colours::transparentBlack);
    pvf->recvOptionsButton->setColour(DrawableButton::backgroundOnColourId, Colour::fromFloatRGBA(0.14, 0.50, 0.46, 0.7));
    pvf->recvOptionsButton->setColour(DrawableButton::backgroundColourId, Colours::transparentBlack);
    pvf->recvOptionsButton->setTitle(TRANS("Receive Options"));

    
    pvf->sendOptionsButton = std::make_unique<SonoDrawableButton>("settings",  DrawableButton::ButtonStyle::ImageFitted);
    pvf->sendOptionsButton->addListener(this);
    pvf->sendOptionsButton->setColour(SonoTextButton::outlineColourId, Colours::transparentBlack);
    pvf->sendOptionsButton->setColour(DrawableButton::backgroundOnColourId, Colour::fromFloatRGBA(0.14, 0.50, 0.46, 0.7));
    pvf->sendOptionsButton->setColour(DrawableButton::backgroundColourId, Colours::transparentBlack);
    pvf->sendOptionsButton->setTitle(TRANS("Send Options"));


    pvf->changeAllFormatButton = std::make_unique<ToggleButton>(TRANS("Change for all"));
    pvf->changeAllFormatButton->addListener(this);
    pvf->changeAllFormatButton->setLookAndFeel(&pvf->smallLnf);

    
    pvf->formatChoiceButton = std::make_unique<SonoChoiceButton>();
    pvf->formatChoiceButton->setTitle(TRANS("Send Quality"));
    pvf->formatChoiceButton->addChoiceListener(this);
    int numformats = processor.getNumberAudioCodecFormats();
    for (int i=0; i < numformats; ++i) {
        SonobusAudioProcessor::AudioCodecFormatInfo finfo;
        processor.getAudioCodeFormatInfo(i, finfo);
        auto name = finfo.name;
        if (finfo.codec == SonobusAudioProcessor::AudioCodecFormatCodec::CodecOpus && finfo.bitrate < 96000) {
            name += String(" (*)");
        }
        pvf->formatChoiceButton->addItem(name, i);
    }
    pvf->formatChoiceButton->addItem("(*) " + TRANS("not recommended"), -2, true, true);

    pvf->staticFormatChoiceLabel = std::make_unique<Label>("sendfmtst", TRANS("Send Quality"));
    pvf->staticFormatChoiceLabel->setAccessible(false);
    configLabel(pvf->staticFormatChoiceLabel.get(), LabelTypeRegular);


    pvf->remoteSendFormatChoiceButton = std::make_unique<SonoChoiceButton>();
    pvf->remoteSendFormatChoiceButton->addChoiceListener(this);
    pvf->remoteSendFormatChoiceButton->setTitle(TRANS("Preferred Receive Quality"));
    pvf->remoteSendFormatChoiceButton->addItem(TRANS("No Preference"), -1);
    for (int i=0; i < numformats; ++i) {
        pvf->remoteSendFormatChoiceButton->addItem(processor.getAudioCodeFormatName(i), i);
    }

    pvf->staticRemoteSendFormatChoiceLabel = std::make_unique<Label>("recvfmtst", TRANS("Preferred Recv Quality"));
    configLabel(pvf->staticRemoteSendFormatChoiceLabel.get(), LabelTypeRegular);
    pvf->staticRemoteSendFormatChoiceLabel->setAccessible(false);

    pvf->changeAllRecvFormatButton = std::make_unique<ToggleButton>(TRANS("Change all"));
    pvf->changeAllRecvFormatButton->addListener(this);
    pvf->changeAllRecvFormatButton->setLookAndFeel(&pvf->smallLnf);


    pvf->staticLatencyLabel = std::make_unique<Label>("latst", TRANS("Latency (ms)"));
    configLabel(pvf->staticLatencyLabel.get(), LabelTypeSmallDim);
    pvf->staticLatencyLabel->setJustificationType(Justification::centred);
    pvf->staticLatencyLabel->setAccessible(false);

    pvf->staticPingLabel = std::make_unique<Label>("pingst", TRANS("Ping"));
    configLabel(pvf->staticPingLabel.get(), LabelTypeSmallDim);
    pvf->staticPingLabel->setAccessible(false);

    pvf->latencyUpLabel = std::make_unique<Label>("ulat", TRANS("PRESS"));
    configLabel(pvf->latencyUpLabel.get(), LabelTypeSmall);
    pvf->latencyUpLabel->setJustificationType(Justification::left);
    pvf->latencyUpLabel->setFont(15.0f);                    // Row latency numbers were 12 pt and squeezed
    pvf->latencyUpLabel->setMinimumHorizontalScale(0.75f);   // 31 px slot, a 3-digit value squeezes instead of showing dots
    pvf->latencyUpLabel->setAccessible(false);

    pvf->latencyDownLabel = std::make_unique<Label>("dlat", "");
    configLabel(pvf->latencyDownLabel.get(), LabelTypeSmall);
    pvf->latencyDownLabel->setJustificationType(Justification::left);
    pvf->latencyDownLabel->setFont(15.0f);
    pvf->latencyDownLabel->setMinimumHorizontalScale(0.75f);   
    pvf->latencyDownLabel->setAccessible(false);

    pvf->pingLabel = std::make_unique<Label>("ping");
    configLabel(pvf->pingLabel.get(), LabelTypeSmall);
    pvf->pingLabel->setAccessible(false);

    pvf->staticSendQualLabel = std::make_unique<Label>("sendqualst", TRANS("Send Quality:"));
    configLabel(pvf->staticSendQualLabel.get(), LabelTypeSmallDim);
    pvf->staticBufferLabel = std::make_unique<Label>("bufst", TRANS("Recv Jitter Buffer:"));
    configLabel(pvf->staticBufferLabel.get(), LabelTypeSmallDim);
    pvf->staticSendQualLabel->setAccessible(false);
    pvf->staticBufferLabel->setAccessible(false);

    pvf->sendQualityLabel = std::make_unique<Label>("qual", "");
    configLabel(pvf->sendQualityLabel.get(), LabelTypeSmall);
    pvf->sendQualityLabel->setJustificationType(Justification::centredLeft);
    pvf->sendQualityLabel->setAccessible(false);

    pvf->bufferLabel = std::make_unique<Label>("buf");
    configLabel(pvf->bufferLabel.get(), LabelTypeSmall);
    pvf->bufferLabel->setJustificationType(Justification::centredLeft);
    pvf->bufferLabel->setAccessible(false);

    
    pvf->sendActualBitrateLabel = std::make_unique<Label>("sbit");
    configLabel(pvf->sendActualBitrateLabel.get(), LabelTypeSmall);
    pvf->sendActualBitrateLabel->setJustificationType(Justification::centredLeft);
    pvf->sendActualBitrateLabel->setMinimumHorizontalScale(0.75);
    pvf->sendActualBitrateLabel->setAccessible(false);

    pvf->recvActualBitrateLabel = std::make_unique<SbRichLabel>("rbit");   // Drops coloured, throughput not
    configLabel(pvf->recvActualBitrateLabel.get(), LabelTypeSmall);
    pvf->recvActualBitrateLabel->setJustificationType(Justification::centredLeft);
    pvf->recvActualBitrateLabel->setFont(14.0f);            // The row "drops N" readout, unsqueezed
    pvf->recvActualBitrateLabel->setMinimumHorizontalScale(1.0f);
    pvf->recvActualBitrateLabel->setAccessible(false);

    // Hosted mode: per-row dropped-packet readout. Read-only display of the existing per-peer drop counter, refreshed
    // on the updatePeerViews cadence; no new timers, nothing on the audio thread.
    pvf->hubLatLabel = std::make_unique<Label>("hublat");
    configLabel(pvf->hubLatLabel.get(), LabelTypeSmall);
    pvf->hubLatLabel->setJustificationType(Justification::centred);
    pvf->hubLatLabel->setMinimumHorizontalScale(0.75);
    pvf->hubLatLabel->setAccessible(false);

    pvf->hostedDropsLabel = std::make_unique<Label>("rdrops");
    pvf->hostedDropsLabel->setTooltip(TRANS("Blocks the hub lost from this player's uplink (late past the hub's hold, or never arrived)"));
    configLabel(pvf->hostedDropsLabel.get(), LabelTypeSmallDim);
    pvf->hostedDropsLabel->setJustificationType(Justification::centredLeft);
    pvf->hostedDropsLabel->setMinimumHorizontalScale(0.75);
    pvf->hostedDropsLabel->setAccessible(false);
    // Right-click on a row's drops readout resets that row's count.
    pvf->hostedDropsLabel->addMouseListener(this, false);
    if (pvf->recvActualBitrateLabel) pvf->recvActualBitrateLabel->addMouseListener(this, false);

    pvf->sendUpArrow =   Drawable::createFromImageData(BinaryData::arrowupnarrow_svg, BinaryData::arrowupnarrow_svgSize);
    pvf->sendUpArrow->setInterceptsMouseClicks(false, false);

    pvf->recvDownArrow = Drawable::createFromImageData(BinaryData::arrowdownnarrow_svg, BinaryData::arrowdownnarrow_svgSize);
    pvf->recvDownArrow->setInterceptsMouseClicks(false, false);

    pvf->latUpArrow =   pvf->sendUpArrow->createCopy();
    pvf->latUpArrow->setInterceptsMouseClicks(false, false);
    pvf->latDownArrow = pvf->recvDownArrow->createCopy();
    pvf->latDownArrow->setInterceptsMouseClicks(false, false);


    pvf->channelGroups = std::make_unique<ChannelGroupsView>(processor, true);
    pvf->channelGroups->addListener(this);


    pvf->jitterBufferMeter = std::make_unique<JitterBufferMeter>();
    
    pvf->optionsResetDropButton = std::make_unique<TextButton>(TRANS("Reset Dropped"));
    pvf->optionsResetDropButton->addListener(this);
    pvf->optionsResetDropButton->setLookAndFeel(&pvf->medLnf);

    pvf->optionsRemoveButton = std::make_unique<TextButton>(TRANS("Remove"));
    pvf->optionsRemoveButton->addListener(this);
    pvf->optionsRemoveButton->setLookAndFeel(&pvf->smallLnf);
    pvf->optionsRemoveButton->setTooltip(TRANS("Removes user from your own connections, does not affect the whole group"));

    pvf->optionsBlockButton = std::make_unique<TextButton>(TRANS("BLOCK"));
    pvf->optionsBlockButton->addListener(this);
    pvf->optionsBlockButton->setLookAndFeel(&pvf->smallLnf);
    pvf->optionsBlockButton->setTooltip(TRANS("BLOCK any connections from this users IP address for yourself, does not affect other users"));

    
    // meters
    auto flags = foleys::LevelMeter::Minimal;
    
    pvf->recvMeter = std::make_unique<SbChannelTipMeter>(flags);
    {   // Hovering a bar of this row's receive meter names that channel of the player
        PeerViewInfo * rowpvf = pvf;
        pvf->recvMeter->nameForChannel = [this, rowpvf](int ch, int /*n*/) -> String {
            const int i = mPeerViews.indexOf(rowpvf);
            if (i < 0) return {};
            int pos = 0;
            for (int g = 0; g < processor.getRemotePeerChannelGroupCount(i); ++g) {
                int start = 0, count = 0;
                if (!processor.getRemotePeerChannelGroupStartAndCount(i, g, start, count)) continue;
                if (ch >= pos && ch < pos + count) {
                    String tip = processor.getRemotePeerChannelGroupName(i, g);
                    if (tip.isEmpty()) tip << TRANS("Channel") << " " << (ch + 1);
                    if (count > 1) tip << " " << (ch - pos + 1);
                    return tip;
                }
                pos += count;
            }
            String tip; tip << TRANS("Channel") << " " << (ch + 1); return tip;
        };
    }
    pvf->recvMeter->setLookAndFeel(&(pvf->rmeterLnf));
    pvf->groupMeterColumn = std::make_unique<Component>("gmcol");   // Expanded rows' meter column (placeholder, never painted)
    pvf->addChildComponent(pvf->groupMeterColumn.get());
    pvf->recvMeter->setUseBackgroundImage(true); // Cached static layers
    pvf->recvMeter->setRefreshRateHz(0); // XXX
    pvf->recvMeter->addMouseListener(this, false);

    // New rows inherit the window-showing meter gate
    pvf->channelGroups->setMetersSuspended(metersSuspended);


    pvf->sendOptionsContainer = std::make_unique<Component>();
    pvf->sendOptionsContainer->setFocusContainerType(FocusContainerType::focusContainer);

    pvf->recvOptionsContainer = std::make_unique<Component>();
    pvf->recvOptionsContainer->setFocusContainerType(FocusContainerType::focusContainer);

    Colour statsbgcol = Colour::fromFloatRGBA(0.07, 0.07, 0.07, 1.0); // 0.08
    Colour statsbordcol = Colour::fromFloatRGBA(0.5, 0.5, 0.5, 0.0); // 0.5 alpha 0.25
    pvf->sendStatsBg = std::make_unique<DrawableRectangle>();
    pvf->sendStatsBg->setCornerSize(Point<float>(6,6));
    pvf->sendStatsBg->setFill (statsbgcol);
    pvf->sendStatsBg->setStrokeFill (statsbordcol);
    pvf->sendStatsBg->setStrokeThickness(0.5);

    pvf->recvStatsBg = std::make_unique<DrawableRectangle>();
    pvf->recvStatsBg->setCornerSize(Point<float>(6,6));
    pvf->recvStatsBg->setFill (statsbgcol);
    pvf->recvStatsBg->setStrokeFill (statsbordcol);
    pvf->recvStatsBg->setStrokeThickness(0.5);

    
    pvf->pingBg = std::make_unique<DrawableRectangle>();
    pvf->pingBg->setCornerSize(Point<float>(6,6));
    pvf->pingBg->setFill (statsbgcol);
    pvf->pingBg->setStrokeFill (statsbordcol);
    pvf->pingBg->setStrokeThickness(0.5);

    // Every label, button and slider in the row is buffered to an image (see the constructor note). Not buffered:
    // recvMeter and jitterBufferMeter (animated), addrLabel (TextEditor caret/selection), the Drawable SVG arrows and
    // DrawableRectangle backgrounds, and the containers (buffering a parent would freeze its child meters).
    for (Component* c : std::initializer_list<Component*>{
            pvf->staticAddrLabel.get(), pvf->sendMutedButton.get(),
            pvf->recvMutedButton.get(), pvf->recvSoloButton.get(),
            pvf->latActiveButton.get(), pvf->statusLabel.get(),
            pvf->bufferTimeSlider.get(), pvf->autosizeButton.get(),
            pvf->bufferMinButton.get(), pvf->bufferMinFrontButton.get(),
            pvf->bufferTimeLabel.get(), pvf->recvOptionsButton.get(),
            pvf->sendOptionsButton.get(), pvf->changeAllFormatButton.get(),
            pvf->formatChoiceButton.get(), pvf->staticFormatChoiceLabel.get(),
            pvf->remoteSendFormatChoiceButton.get(), pvf->staticRemoteSendFormatChoiceLabel.get(),
            pvf->changeAllRecvFormatButton.get(), pvf->staticLatencyLabel.get(),
            pvf->staticPingLabel.get(), pvf->latencyUpLabel.get(),
            pvf->latencyDownLabel.get(), pvf->pingLabel.get(),
            pvf->staticSendQualLabel.get(), pvf->staticBufferLabel.get(),
            pvf->sendQualityLabel.get(), pvf->bufferLabel.get(),
            pvf->sendActualBitrateLabel.get(), pvf->recvActualBitrateLabel.get(),
            pvf->hostedDropsLabel.get(), pvf->optionsResetDropButton.get(),
            pvf->optionsRemoveButton.get(), pvf->optionsBlockButton.get() }) {
        if (c) c->setBufferedToImage(true);
    }

    pvf->fullMode = peerModeFull;

    return pvf;
}
    
PendingPeerViewInfo * PeersContainerView::createPendingPeerViewInfo()
{
    auto * pvf = new PendingPeerViewInfo();

    pvf->nameLabel = std::make_unique<Label>("name", "");
    pvf->nameLabel->setJustificationType(Justification::centredLeft);
    pvf->nameLabel->setFont(16);

    pvf->messageLabel = std::make_unique<Label>("msg", "");
    pvf->messageLabel->setJustificationType(Justification::centredLeft);
    pvf->messageLabel->setFont(13);
    pvf->messageLabel->setMinimumHorizontalScale(0.8);

    pvf->removeButton = std::make_unique<TextButton>(TRANS("Remove"));
    pvf->removeButton->addListener(this);
    pvf->removeButton->setTooltip(TRANS("Removes pending user from list"));

    pvf->unblockButton = std::make_unique<TextButton>(TRANS("Unblock"));
    pvf->unblockButton->addListener(this);
    pvf->unblockButton->setTooltip(TRANS("Unblocks address from blocked list"));

    // See createPeerViewInfo.
    for (Component* c : std::initializer_list<Component*>{
            pvf->nameLabel.get(), pvf->messageLabel.get(),
            pvf->removeButton.get(), pvf->unblockButton.get() }) {
        if (c) c->setBufferedToImage(true);
    }

    return pvf;
}

void PeersContainerView::channelLayoutChanged(ChannelGroupsView *comp)
{
    updateLayout();

    listeners.call (&PeersContainerView::Listener::internalSizesChanged, this);

    resized();
}

void PeersContainerView::nameLabelClicked(ChannelGroupsView *comp)
{
    if (mIgnoreNameClick) return;

    // toggle full mode
    for (int i=0; i < mPeerViews.size(); ++i) {
        PeerViewInfo * pvf = mPeerViews.getUnchecked(i);
        if (pvf->channelGroups.get() == comp) {
            pvf->fullMode = !pvf->fullMode;
            break;
        }
    }

    rebuildPeerViews();

    listeners.call (&PeersContainerView::Listener::internalSizesChanged, this);
}


void PeersContainerView::resetPendingUsers()
{
    mPendingUsers.clear();
}

void PeersContainerView::peerPendingJoin(String & group, String & user)
{
    mPendingUsers[user] = PendingUserInfo(group, user);
    rebuildPeerViews();    
}

void PeersContainerView::peerFailedJoin(String & group, String & user)
{
    auto found = mPendingUsers.find(user);
    if (found != mPendingUsers.end()) {
        found->second.failed = true;
        updatePeerViews();
    }
}

void PeersContainerView::peerBlockedJoin(String & group, String & user, String & address, int port)
{
    auto found = mPendingUsers.find(user);
    if (found != mPendingUsers.end()) {
        found->second.failed = true;
        found->second.blocked = true;
        found->second.address = address;
        found->second.port = port;
        updatePeerViews();
    }    
}

void PeersContainerView::peerLeftGroup(String & group, String & user)
{
    // check if it was pending and remove it
    auto found = mPendingUsers.find(user);
    if (found != mPendingUsers.end()) {
        auto & peer = found->second;
        mPendingUsers.erase(found);
    }
}


void PeersContainerView::setPeerDisplayMode(SonobusAudioProcessor::PeerDisplayMode mode)
{
    // set full mode for all
    for (int i=0; i < mPeerViews.size(); ++i) {
        PeerViewInfo * pvf = mPeerViews.getUnchecked(i);

        pvf->fullMode = (mode == SonobusAudioProcessor::PeerDisplayModeFull);
    }

    peerModeFull = (mode == SonobusAudioProcessor::PeerDisplayModeFull);

    rebuildPeerViews();
    listeners.call (&PeersContainerView::Listener::internalSizesChanged, this);
}

void PeersContainerView::rebuildPeerViews()
{
    int numpeers = processor.getNumberRemotePeers();
    
    showSendOptions(0, false);
    showRecvOptions(0, false);
    // Dismiss the unified-buffer popup on rebuild like the
    // per-peer ones above (topology may have flipped hosted mode off).
    showUnifiedBufferOptions(false);

    while (mPeerViews.size() < numpeers) {
        mPeerViews.add(createPeerViewInfo());
    }
    while (mPeerViews.size() > numpeers) {
        mPeerViews.removeLast();
    }

    bool anyfull = false;

    // Rows are drawn in mPeerUpdateOrdering, not by raw peer index: resolve the ordering first and set up the peer
    // each row shows.
    for (int pi=0; pi < mPeerViews.size(); ++pi) {
        const int prio = processor.getRemotePeerOrderPriority(pi);
        if (prio >= 0) mPeerPriorityOrdering[processor.getRemotePeerUserName(pi)] = prio;
    }
    updatePeerOrdering();

    for (int di=0; di < mPeerViews.size(); ++di) {
        PeerViewInfo * pvf = mPeerViews.getUnchecked(di);
        const int i = di < (int) mPeerUpdateOrdering.size() ? mPeerUpdateOrdering[di] : di;
        String username = processor.getRemotePeerUserName(i);
        // remove from pending if necessary
        mPendingUsers.erase(username);

        int prio = processor.getRemotePeerOrderPriority(i);
        if (prio >= 0) {
            mPeerPriorityOrdering[username] = prio;
        }


        pvf->channelGroups->getAudioDeviceManager = getAudioDeviceManager;

        pvf->addAndMakeVisible(pvf->channelGroups.get());
        pvf->channelGroups->setPeerMode(true, i);
        pvf->channelGroups->setNarrowMode(isNarrow);
        pvf->channelGroups->rebuildChannelViews();

        pvf->channelGroups->addMouseListener(this, true);
        
        pvf->addAndMakeVisible(pvf->sendStatsBg.get());
        pvf->addAndMakeVisible(pvf->recvStatsBg.get());
        pvf->addAndMakeVisible(pvf->pingBg.get());
        //pvf->addAndMakeVisible(pvf->sendMutedButton.get());
        pvf->addAndMakeVisible(pvf->recvMutedButton.get());
        pvf->addAndMakeVisible(pvf->recvSoloButton.get());
        pvf->addAndMakeVisible(pvf->latActiveButton.get());


        pvf->addAndMakeVisible(pvf->sendOptionsButton.get());
        pvf->addAndMakeVisible(pvf->recvOptionsButton.get());
        pvf->addAndMakeVisible(pvf->staticLatencyLabel.get());
        pvf->addAndMakeVisible(pvf->staticPingLabel.get());
        pvf->addAndMakeVisible(pvf->latencyUpLabel.get());
        pvf->addAndMakeVisible(pvf->latencyDownLabel.get());
        pvf->addAndMakeVisible(pvf->pingLabel.get());

        pvf->addAndMakeVisible(pvf->statusLabel.get());
        pvf->addAndMakeVisible(pvf->jitterBufferMeter.get());
        pvf->addAndMakeVisible(pvf->staticSendQualLabel.get());
        pvf->addAndMakeVisible(pvf->staticBufferLabel.get());
        pvf->addAndMakeVisible(pvf->sendQualityLabel.get());
        pvf->addAndMakeVisible(pvf->bufferLabel.get());
        pvf->addChildComponent(pvf->bufferMinFrontButton.get());
        pvf->addAndMakeVisible(pvf->recvButtonImage.get());
        pvf->addAndMakeVisible(pvf->sendButtonImage.get());

        pvf->recvOptionsContainer->addAndMakeVisible(pvf->addrLabel.get());
        pvf->recvOptionsContainer->addAndMakeVisible(pvf->staticAddrLabel.get());
        pvf->recvOptionsContainer->addAndMakeVisible(pvf->autosizeButton.get());
        pvf->recvOptionsContainer->addAndMakeVisible(pvf->bufferTimeLabel.get());
        pvf->recvOptionsContainer->addAndMakeVisible(pvf->bufferTimeSlider.get());
        pvf->recvOptionsContainer->addAndMakeVisible(pvf->bufferMinButton.get());
        pvf->recvOptionsContainer->addAndMakeVisible(pvf->optionsResetDropButton.get());
        pvf->recvOptionsContainer->addAndMakeVisible(pvf->hubLatLabel.get());
        pvf->recvOptionsContainer->addAndMakeVisible(pvf->remoteSendFormatChoiceButton.get());
        pvf->recvOptionsContainer->addAndMakeVisible(pvf->staticRemoteSendFormatChoiceLabel.get());
        pvf->recvOptionsContainer->addAndMakeVisible(pvf->changeAllRecvFormatButton.get());

        pvf->sendOptionsContainer->addAndMakeVisible(pvf->formatChoiceButton.get());
        pvf->sendOptionsContainer->addAndMakeVisible(pvf->changeAllFormatButton.get());
        pvf->sendOptionsContainer->addAndMakeVisible(pvf->staticFormatChoiceLabel.get());
        pvf->sendOptionsContainer->addAndMakeVisible(pvf->sendMutedButton.get());
        pvf->sendOptionsContainer->addAndMakeVisible(pvf->optionsRemoveButton.get());
        pvf->sendOptionsContainer->addAndMakeVisible(pvf->optionsBlockButton.get());

        pvf->addChildComponent(pvf->recvMeter.get());   // Full-height meter on relayed rows
        pvf->addAndMakeVisible(pvf->sendActualBitrateLabel.get());
        pvf->addAndMakeVisible(pvf->recvActualBitrateLabel.get());
        // Child only: visibility is owned by updateLayout (hosted mode + fullmode gate), so P2P never shows it.
        pvf->addChildComponent(pvf->hostedDropsLabel.get());

        pvf->addAndMakeVisible(pvf->sendUpArrow.get());
        pvf->addAndMakeVisible(pvf->recvDownArrow.get());
        pvf->addAndMakeVisible(pvf->latUpArrow.get());
        pvf->addAndMakeVisible(pvf->latDownArrow.get());

        pvf->infoOnly = processor.getRemotePeerInfoOnly(i);
        auto fullmode = pvf->fullMode && !pvf->infoOnly;

        // In HostedMix the per-peer Latency PRESS test and Ping are misleading: they are designed for direct P2P. The
        // relevant round trip is the hub->peer ping shown in the hub-status header and the hub GUI peer table. Hide
        // the per-row latency/ping cluster; keep the rest of fullmode intact.
        const bool inHostedMix = processor.isHostedMixModeEnabled();
        // Relayed player rows keep the stock latency labels, fed by the hub (/sb/hxlat): no PRESS test, no ping, no
        // options overlay box.
        const bool relayedLat = inHostedMix && !pvf->infoOnly && !processor.isHubPeer(username);
        const bool showLatCluster = fullmode && !inHostedMix;
        const bool showLatLabels = fullmode && (!inHostedMix || relayedLat);

        // Hosted mode: per-row receive-buffer widgets are hidden; the unified header row is the one knob. The jitter
        // meter goes too: it is drawn on bufferLabel's bounds, so it cannot stay without the buffer labels' layout.
        const bool showBufCluster = fullmode && !inHostedMix;

        // visibility based on peerdisplay mode
        pvf->sendQualityLabel->setVisible(fullmode && !relayedLat);   // Redundant on relayed rows
        pvf->bufferLabel->setVisible(showBufCluster);
        pvf->bufferMinFrontButton->setVisible(showBufCluster);
        pvf->latencyUpLabel->setVisible(showLatLabels);
        pvf->latencyDownLabel->setVisible(showLatLabels);
        pvf->latActiveButton->setVisible(showLatCluster);
        pvf->sendActualBitrateLabel->setVisible(fullmode && !relayedLat);
        pvf->recvActualBitrateLabel->setVisible(fullmode);
        pvf->sendUpArrow->setVisible(fullmode && !relayedLat);
        pvf->recvDownArrow->setVisible(fullmode && !relayedLat);
        pvf->latUpArrow->setVisible(showLatLabels);
        pvf->latDownArrow->setVisible(showLatLabels);
        pvf->recvStatsBg->setVisible(fullmode && !relayedLat);
        pvf->sendStatsBg->setVisible(fullmode && !relayedLat);
        pvf->staticSendQualLabel->setVisible(fullmode);
        pvf->staticLatencyLabel->setVisible(showLatLabels);
        pvf->staticPingLabel->setVisible(showLatCluster);
        pvf->staticBufferLabel->setVisible(showBufCluster);
        pvf->pingBg->setVisible(showLatCluster);
        pvf->recvButtonImage->setVisible(fullmode);
        pvf->sendButtonImage->setVisible(fullmode);
        pvf->jitterBufferMeter->setVisible(showBufCluster);
        pvf->pingLabel->setVisible(showLatCluster);
        pvf->sendOptionsButton->setVisible(fullmode);
        pvf->recvOptionsButton->setVisible(fullmode && !relayedLat);   // No "In your mix" overlay box
        pvf->recvButtonImage->setVisible(fullmode && !relayedLat);
        pvf->latActiveButton->setVisible(showLatCluster);
        pvf->recvMeter->setVisible(relayedLat && !pvf->isNarrow && !processor.getRemotePeerViewExpanded(i));   // All display modes; narrow keeps the inline meter; expanded rows show per-channel meters instead

        if (pvf->infoOnly) {
            // Info-only rows (HostedMix non-hub peers): the recvMute button stays active and routes through the
            // hub-mix control channel, letting the listener mute one musician in their own mix without affecting what
            // other peers hear. Other interactive controls (solo, options, latency) are hidden.
            pvf->recvSoloButton->setVisible(false);
            pvf->statusLabel->setVisible(false);
            pvf->sendOptionsButton->setVisible(false);
            pvf->recvOptionsButton->setVisible(false);
            pvf->latActiveButton->setVisible(false);
            pvf->sendOptionsButton->setEnabled(false);
            pvf->recvOptionsButton->setEnabled(false);

            // Reflect the cached hub-mix mute choice so the toggle is accurate
            // after rebuilds (peer joins, group rejoin, etc.).
            pvf->recvMutedButton->setToggleState(
                processor.getHubMixMuteForPeer(username), dontSendNotification);
        }

        if (fullmode) {
            anyfull = true;
        }

        addAndMakeVisible(pvf);
    }
    
    while (mPendingPeerViews.size() < mPendingUsers.size()) {
        mPendingPeerViews.add(createPendingPeerViewInfo());
    }
    while (mPendingPeerViews.size() > mPendingUsers.size()) {
        mPendingPeerViews.removeLast();
    }

    for (int i=0; i < mPendingPeerViews.size(); ++i) {
        PendingPeerViewInfo * ppvf = mPendingPeerViews.getUnchecked(i);
        ppvf->addAndMakeVisible(ppvf->nameLabel.get());
        ppvf->addAndMakeVisible(ppvf->messageLabel.get());
        ppvf->addChildComponent(ppvf->removeButton.get());
        ppvf->addChildComponent(ppvf->unblockButton.get());
        addAndMakeVisible(ppvf);
    }
    
    // The 10 Hz fill-ratio timer only feeds the per-row jitterBufferMeter widgets, which are hidden in hosted mode
    // (showBufCluster above). Each tick takes the core read-lock once per peer, so run it only when at least one of
    // those meters is visible.
    bool anyJitterMeterVisible = false;
    for (int i=0; i < mPeerViews.size(); ++i) {
        PeerViewInfo * pvf = mPeerViews.getUnchecked(i);
        if (pvf->jitterBufferMeter && pvf->jitterBufferMeter->isVisible()) {
            anyJitterMeterVisible = true;
            break;
        }
    }

    if (mPeerViews.size() > 0 && anyfull && anyJitterMeterVisible) {
        startTimer(FillRatioUpdateTimerId, 100);
    } else {
        stopTimer(FillRatioUpdateTimerId);
    }

    updatePeerOrdering();

    updatePeerViews();
    updateLayout();
    resized();
}

void PeersContainerView::updatePeerOrdering()
{
    mPeerUpdateOrdering.clear();

    std::set<int> addedindexes;
    std::map<int,int> priorityIndexes; // key is priority, value is peer index

    for (int i=0; i < processor.getNumberRemotePeers(); ++i) {
        String username = processor.getRemotePeerUserName(i);

        auto found = mPeerPriorityOrdering.find(username);
        if (found != mPeerPriorityOrdering.end()) {
            priorityIndexes[found->second] = i;
        }
    }

    // add the priority ordered ones first to the update order
    for (auto iter : priorityIndexes) {
        mPeerUpdateOrdering.push_back(iter.second);
        addedindexes.insert(iter.second);
    }

    // add the rest
    for (int i=0; i < processor.getNumberRemotePeers(); ++i) {
        if (addedindexes.find(i) == addedindexes.end()) {
            mPeerUpdateOrdering.push_back(i);
        }
    }

}

void PeersContainerView::updateLayout()
{
    int minitemheight = 36;
    int mincheckheight = 32;
    int minPannerWidth = 58;
    int minButtonWidth = 90;
    int maxPannerWidth = 110;
    int minSmallButtonWidth = 55;

    int mutebuttwidth = 52;
    
#if JUCE_IOS || JUCE_ANDROID
    // make the button heights a bit more for touchscreen purposes
    minitemheight = 44;
    mincheckheight = 40;
    minPannerWidth = 60;
#endif

    const int textheight = minitemheight / 2;

    bool servconnected = processor.isConnectedToServer();

    peersBox.items.clear();
    peersBox.flexDirection = FlexBox::Direction::column;
    peersBox.justifyContent = FlexBox::JustifyContent::flexStart;
    int peersheight = 0;
    //const int singleph =  minitemheight*3 + 12;
    const int singleph =  minitemheight*2 + 6;
    
    peersBox.items.add(FlexItem(8, 2).withMargin(0));

    // HostedMix: hub-status header replaces the SonoBus-Hub peer row.
    if (mHubStatusLabel != nullptr) {
        const bool inHostedMix = processor.isHostedMixModeEnabled();
        mHubStatusLabel->setVisible(inHostedMix);
        // One consolidated header row: status text on the left (flex, compresses first), compact buffer-value chip
        // fixed on the right. The unified slider lives in the chip's popup (showUnifiedBufferOptions).
        if (mUnifiedBufChipButton != nullptr) {
            mUnifiedBufChipButton->setVisible(inHostedMix);
        }
        if (inHostedMix) {
            // Row + peersheight matched to the 20pt font set on construction.
            hubHeaderBox.items.clear();
            hubHeaderBox.flexDirection = FlexBox::Direction::row;
            hubHeaderBox.items.add(FlexItem(8, 28, *mHubStatusLabel).withMargin(0).withFlex(1));
            if (mUnifiedBufChipButton != nullptr) {
                hubHeaderBox.items.add(FlexItem(4, 4).withMargin(0).withFlex(0));
                hubHeaderBox.items.add(FlexItem(104, 28, *mUnifiedBufChipButton).withMargin(1).withFlex(0));
                hubHeaderBox.items.add(FlexItem(4, 4).withMargin(0).withFlex(0));
            }
            peersBox.items.add(FlexItem(8, 28, hubHeaderBox).withMargin(2).withFlex(0));
            peersheight += 32;
        }
    }
    peersheight += 2;


    // Main connected peer views
    // Rows are drawn in mPeerUpdateOrdering, so row di shows peer mPeerUpdateOrdering[di]; lay each row out for that
    // peer.
    for (int di=0; di < mPeerViews.size(); ++di) {
        PeerViewInfo * pvf = mPeerViews.getUnchecked(di);
        const int i = di < (int) mPeerUpdateOrdering.size() ? mPeerUpdateOrdering[di] : di;

        // HostedMix: hide the SonoBus-Hub row from the peer list; the hub-status header replaces it.
        const String username_for_hide = processor.getRemotePeerUserName(i);
        if (processor.isHubPeer(username_for_hide)) {
            pvf->setVisible(false);
            continue;
        }
        pvf->setVisible(true);

        //pvf->updateLayout();

        const int ph = singleph;
        const int pw = 200;

        auto fullmode = pvf->fullMode;

        // Hosted mode: per-row receive-buffer widgets are hidden and the unified header row is the only buffer knob.
        // Reflow below so no hole remains, and set the PeerViewInfo flag that gates resized()'s visibility
        // re-assertion + overlay anchoring. The popup widgets (slider / autosize / buffer-min) are hidden here too
        // because rebuildPeerViews re-adds them with addAndMakeVisible.
        const bool hideRowBuf = processor.isHostedMixModeEnabled();
        pvf->hideBufferWidgets = hideRowBuf;
        pvf->bufferTimeSlider->setVisible(!hideRowBuf);
        pvf->bufferTimeLabel->setVisible(!hideRowBuf);
        pvf->bufferMinButton->setVisible(!hideRowBuf);
        pvf->autosizeButton->setVisible(!hideRowBuf);

        // No separate per-row drops line: the count stays in the bitrate line (nonzero only) and the hub-status
        // header carries the aggregate.
        pvf->hostedDropsLabel->setVisible(false);

        // Relayed peer rows (hosted mode, real player, not the hub; hub rows are skipped above) drop the P2P send
        // cluster: the "Send Quality:" static label, the send-options button (format chooser / send-mute / remove /
        // block popup) and its overlay chrome. There is no per-peer upload to them, since audio goes via the hub.
        // sendQualityLabel stays, restyled to the channel count we receive from that peer (text set in
        // updatePeerViews), and the "via hub" line stays where it is. The flag gates resized()'s overlay anchoring.
        const bool relayedRow = hideRowBuf && !pvf->infoOnly;
        pvf->hideSendCluster = relayedRow;
        const bool showSendCluster = fullmode && !pvf->infoOnly && !relayedRow;
        pvf->staticSendQualLabel->setVisible(showSendCluster);
        pvf->sendOptionsButton->setVisible(showSendCluster);
        pvf->sendButtonImage->setVisible(showSendCluster);
        pvf->sendStatsBg->setVisible(showSendCluster);
        // The recv-options popup's "Preferred Recv Quality" row asks the remote to change its send format, which is
        // meaningless for a hub feed.
        pvf->remoteSendFormatChoiceButton->setVisible(!relayedRow);
        pvf->staticRemoteSendFormatChoiceLabel->setVisible(!relayedRow);
        pvf->changeAllRecvFormatButton->setVisible(!relayedRow);
        // Relayed rows: the remote address and the P2P "Reset Dropped" button are meaningless for a hub feed; show
        // that player's to/from-hub latency (from /sb/hxlat) in their place.
        pvf->staticAddrLabel->setVisible(!relayedRow);
        pvf->addrLabel->setVisible(!relayedRow);
        pvf->optionsResetDropButton->setVisible(!relayedRow);
        pvf->hubLatLabel->setVisible(relayedRow);


        pvf->sendmeterbox.items.clear();
        pvf->sendmeterbox.flexDirection = FlexBox::Direction::row;

        pvf->pingbox.items.clear();
        pvf->pingbox.flexDirection = FlexBox::Direction::column;
        pvf->pingbox.items.add(FlexItem(20, textheight, *pvf->staticPingLabel).withMargin(0).withFlex(0.5));
        pvf->pingbox.items.add(FlexItem(20, textheight, *pvf->pingLabel).withMargin(0).withFlex(0.5));

        pvf->latencylabbox.items.clear();
        pvf->latencylabbox.flexDirection = FlexBox::Direction::row;
        pvf->latencylabbox.items.add(FlexItem(12, textheight).withFlex(0.0).withMargin(0));   // Fixed slots 12|23|14|23 = 72
        pvf->latencylabbox.items.add(FlexItem(23, textheight, *pvf->latencyUpLabel).withMargin(0).withFlex(0));   // 36 = arrow + 2 digits snug    // 25 -> 40 (15 pt, no squeeze)
        pvf->latencylabbox.items.add(FlexItem(14, textheight).withFlex(0.0).withMargin(0));   // 10 px arrow + 4 air between up and down
        pvf->latencylabbox.items.add(FlexItem(23, textheight, *pvf->latencyDownLabel).withMargin(0).withFlex(0));

        pvf->latencybox.items.clear();
        pvf->latencybox.flexDirection = FlexBox::Direction::column;
        pvf->latencybox.items.add(FlexItem(96, textheight, *pvf->staticLatencyLabel).withMargin(0).withFlex(0.5));   // 60 -> 96
        pvf->latencybox.items.add(FlexItem(96, textheight, pvf->latencylabbox).withMargin(0).withFlex(0.5));

        
        pvf->netstatbox.items.clear();
        pvf->netstatbox.flexDirection = FlexBox::Direction::row;
        pvf->netstatbox.items.add(FlexItem(96, textheight, pvf->latencybox).withMargin(0).withFlex(3));   // 60 -> 96
        pvf->netstatbox.items.add(FlexItem(20, textheight, pvf->pingbox).withMargin(0).withFlex(1));


        if (isNarrow) {
            pvf->squalbox.items.clear();
            pvf->squalbox.flexDirection = FlexBox::Direction::column;
            if (!relayedRow) {
                pvf->squalbox.items.add(FlexItem(60, textheight, *pvf->staticSendQualLabel).withMargin(0).withFlex(0));
                pvf->squalbox.items.add(FlexItem(60, textheight, *pvf->sendQualityLabel).withMargin(0).withFlex(0));
            } else {
                // Relayed row: recv channel count on top, "via hub" (sendActualBitrateLabel, normally dropped in
                // narrow mode) beneath it, in the same two-line band.
                pvf->squalbox.items.add(FlexItem(60, textheight, *pvf->sendQualityLabel).withMargin(0).withFlex(0));
                pvf->squalbox.items.add(FlexItem(60, textheight, *pvf->sendActualBitrateLabel).withMargin(0).withFlex(0));
                pvf->sendActualBitrateLabel->setJustificationType(Justification::centred);
            }
            pvf->staticSendQualLabel->setJustificationType(Justification::centred);
            pvf->sendQualityLabel->setJustificationType(Justification::centred);


            pvf->netbufbox.items.clear();
            pvf->netbufbox.flexDirection = FlexBox::Direction::column;
            if (!hideRowBuf) {
                //pvf->netbufbox.items.add(FlexItem(0, 10).withFlex(1));
                pvf->netbufbox.items.add(FlexItem(60, textheight, *pvf->staticBufferLabel).withMargin(0).withFlex(0)/*.withMaxWidth(105)*/);
                pvf->netbufbox.items.add(FlexItem(60, textheight, *pvf->bufferLabel).withMargin(0).withFlex(0)/*.withMaxWidth(116)*/);
                //pvf->netbufbox.items.add(FlexItem(0, 10).withFlex(1.5));
                pvf->staticBufferLabel->setJustificationType(Justification::centred);
            } else {
                // Hosted mode: buffer labels are hidden, so show the receive-bitrate line here (narrow mode normally
                // drops it). The cluster keeps live content and the recv-options overlay keeps a real anchor.
                // Centered in the freed vertical space.
                pvf->netbufbox.items.add(FlexItem(60, textheight/2).withMargin(0).withFlex(1));
                pvf->netbufbox.items.add(FlexItem(96, textheight, *pvf->recvActualBitrateLabel).withMargin(0).withFlex(0));   // 60 -> 96
                pvf->netbufbox.items.add(FlexItem(60, textheight/2).withMargin(0).withFlex(1));
                pvf->recvActualBitrateLabel->setJustificationType(Justification::centred);
            }


            pvf->optionsstatbox.items.clear();
            pvf->optionsstatbox.flexDirection = FlexBox::Direction::column;
            pvf->optionsstatbox.items.add(FlexItem(70, 2*textheight, pvf->squalbox).withMargin(0).withFlex(0));
            //pvf->optionsstatbox.items.add(FlexItem(76, textheight, *pvf->sendActualBitrateLabel).withMargin(0).withFlex(0));
            // Relayed rows carry "via hub" inside squalbox (see above), so keep it visible there.
            pvf->sendActualBitrateLabel->setVisible(relayedRow && fullmode);
            pvf->sendUpArrow->setVisible(false);


            pvf->recvstatbox.items.clear();
            pvf->recvstatbox.flexDirection = FlexBox::Direction::row;
            pvf->recvstatbox.items.add(FlexItem(2, 10).withFlex(0));
            pvf->recvstatbox.items.add(FlexItem(60, 2*textheight, pvf->netbufbox).withMargin(0).withFlex(1));
            pvf->recvstatbox.items.add(FlexItem(2, 10).withFlex(0));
            //pvf->recvstatbox.items.add(FlexItem(100, textheight, *pvf->recvActualBitrateLabel).withMargin(0).withFlex(0));
            // In hosted mode the bitrate label stands in for
            // the hidden buffer labels (see netbufbox above), so keep it
            // visible on rows that show the recv cluster at all.
            pvf->recvActualBitrateLabel->setVisible(hideRowBuf && fullmode && !pvf->infoOnly);
            pvf->recvDownArrow->setVisible(false);

        }
        else {
            pvf->squalbox.items.clear();
            pvf->squalbox.flexDirection = FlexBox::Direction::row;
            if (!relayedRow) {
                pvf->squalbox.items.add(FlexItem(60, textheight, *pvf->staticSendQualLabel).withMargin(0).withFlex(1));
                pvf->squalbox.items.add(FlexItem(40, textheight, *pvf->sendQualityLabel).withMargin(0).withFlex(2));
                pvf->staticSendQualLabel->setJustificationType(Justification::centredRight);
                pvf->sendQualityLabel->setJustificationType(Justification::centredLeft);
            } else {
                // Relayed row: no static label, so the recv channel count takes the whole line, centred.
                pvf->squalbox.items.add(FlexItem(40, textheight, *pvf->sendQualityLabel).withMargin(0).withFlex(1));
                pvf->sendQualityLabel->setJustificationType(Justification::centred);
            }
            // Restore after a narrow relayed stint recenters it.
            pvf->sendActualBitrateLabel->setJustificationType(Justification::centredLeft);

            pvf->netbufbox.items.clear();
            pvf->netbufbox.flexDirection = FlexBox::Direction::row;
            // Hosted mode: buffer labels stay out of the layout entirely (recvstatbox below drops this box and
            // re-centers the bitrate line).
            if (!hideRowBuf) {
                pvf->netbufbox.items.add(FlexItem(0, 10).withFlex(1));
                pvf->netbufbox.items.add(FlexItem(88, textheight, *pvf->staticBufferLabel).withMargin(0).withFlex(1).withMaxWidth(105));
                pvf->netbufbox.items.add(FlexItem(40, textheight, *pvf->bufferLabel).withMargin(0).withFlex(3).withMaxWidth(116));
                pvf->netbufbox.items.add(FlexItem(0, 10).withFlex(1.5));
                pvf->staticBufferLabel->setJustificationType(Justification::centredRight);
            }
            // Restore after a narrow-hosted stint recenters it.
            pvf->recvActualBitrateLabel->setJustificationType(Justification::centredLeft);
            pvf->hostedDropsLabel->setJustificationType(Justification::centredLeft);


            pvf->sendlabbox.items.clear();
            pvf->sendlabbox.flexDirection = FlexBox::Direction::row;
            pvf->sendlabbox.items.add(FlexItem(15, 10).withFlex(1));
            pvf->sendlabbox.items.add(FlexItem(76, textheight, *pvf->sendActualBitrateLabel).withMargin(0).withFlex(1));
            pvf->sendlabbox.items.add(FlexItem(0, 10).withFlex(1));

            pvf->recvlabbox.items.clear();
            pvf->recvlabbox.flexDirection = FlexBox::Direction::row;
            if (!relayedRow) pvf->recvlabbox.items.add(FlexItem(15, 10).withFlex(1));
            pvf->recvlabbox.items.add(FlexItem(110, textheight, *pvf->recvActualBitrateLabel).withMargin(0).withFlex(1));   // 100 -> 110
            pvf->recvlabbox.items.add(FlexItem(0, 10).withFlex(1));


            pvf->optionsstatbox.items.clear();
            pvf->optionsstatbox.flexDirection = FlexBox::Direction::column;
            pvf->optionsstatbox.items.add(FlexItem(100, textheight, pvf->squalbox).withMargin(0).withFlex(0));
            //pvf->optionsstatbox.items.add(FlexItem(76, textheight, *pvf->sendActualBitrateLabel).withMargin(0).withFlex(0));
            if (!relayedRow) {   // Relayed rows are ONE text line — no bitrate sub-line
                pvf->optionsstatbox.items.add(FlexItem(90, textheight, pvf->sendlabbox).withMargin(0).withFlex(0));
            }
            pvf->sendActualBitrateLabel->setVisible(!relayedRow);
            pvf->sendUpArrow->setVisible(!relayedRow);
            if (relayedRow) pvf->sendQualityLabel->setJustificationType(Justification::centredLeft);


            pvf->recvstatbox.items.clear();
            pvf->recvstatbox.flexDirection = FlexBox::Direction::column;
            if (!hideRowBuf) {
                pvf->recvstatbox.items.add(FlexItem(100, textheight, pvf->netbufbox).withMargin(0).withFlex(0));
                pvf->recvstatbox.items.add(FlexItem(115, textheight, pvf->recvlabbox).withMargin(0).withFlex(0));
            } else if (relayedRow) {
                // Single text line (drops), no centring padding
                pvf->recvstatbox.items.add(FlexItem(115, textheight, pvf->recvlabbox).withMargin(0).withFlex(0));
            } else {
                // Buffer row hidden in hosted mode: center the receive-bitrate line in the freed space.
                pvf->recvstatbox.items.add(FlexItem(2, textheight/2).withMargin(0).withFlex(1));
                pvf->recvstatbox.items.add(FlexItem(115, textheight, pvf->recvlabbox).withMargin(0).withFlex(0));
                pvf->recvstatbox.items.add(FlexItem(2, textheight/2).withMargin(0).withFlex(1));
            }
            // pvf->recvstatbox.items.add(FlexItem(100, textheight, *pvf->recvActualBitrateLabel).withMargin(0).withFlex(0));
            pvf->recvActualBitrateLabel->setVisible(true);
            pvf->recvDownArrow->setVisible(!relayedRow);

        }

        
        // options
        pvf->optionsNetbufBox.items.clear();
        pvf->optionsNetbufBox.flexDirection = FlexBox::Direction::row;
        // Hosted mode: the popup's buffer row (min button / slider / autosize) is hidden with the rest of the per-row
        // buffer widgets; recvOptionsBox below skips the row so the popup shrinks (showRecvOptions shortens its
        // height).
        if (!hideRowBuf) {
            pvf->optionsNetbufBox.items.add(FlexItem(36, minitemheight, *pvf->bufferMinButton).withMargin(0).withFlex(0));
            pvf->optionsNetbufBox.items.add(FlexItem(3, 12));
            pvf->optionsNetbufBox.items.add(FlexItem(minButtonWidth, minitemheight, *pvf->bufferTimeSlider).withMargin(0).withFlex(1));
            pvf->optionsNetbufBox.items.add(FlexItem(4, 12));
            pvf->optionsNetbufBox.items.add(FlexItem(80, minitemheight, *pvf->autosizeButton).withMargin(0).withFlex(0));
        }
        
        pvf->optionsSendQualBox.items.clear();
        pvf->optionsSendQualBox.flexDirection = FlexBox::Direction::row;
        pvf->optionsSendQualBox.items.add(FlexItem(100, minitemheight, *pvf->staticFormatChoiceLabel).withMargin(0).withFlex(0));
        pvf->optionsSendQualBox.items.add(FlexItem(minButtonWidth, minitemheight, *pvf->formatChoiceButton).withMargin(0).withFlex(2));

        pvf->optionsRemoteQualityBox.items.clear();
        pvf->optionsRemoteQualityBox.flexDirection = FlexBox::Direction::row;
        pvf->optionsRemoteQualityBox.items.add(FlexItem(80, minitemheight, *pvf->staticRemoteSendFormatChoiceLabel).withMargin(0).withFlex(0));
        pvf->optionsRemoteQualityBox.items.add(FlexItem(minButtonWidth, minitemheight, *pvf->remoteSendFormatChoiceButton).withMargin(0).withFlex(2));
        pvf->optionsRemoteQualityBox.items.add(FlexItem(68, minitemheight, *pvf->changeAllRecvFormatButton).withMargin(0).withFlex(0));

        
        pvf->optionsSendMutedBox.items.clear();
        pvf->optionsSendMutedBox.flexDirection = FlexBox::Direction::row;
        pvf->optionsSendMutedBox.items.add(FlexItem(5, 12));
        pvf->optionsSendMutedBox.items.add(FlexItem(100, minitemheight, *pvf->sendMutedButton).withMargin(0).withFlex(1));
        pvf->optionsSendMutedBox.items.add(FlexItem(3, 12));
        pvf->optionsSendMutedBox.items.add(FlexItem(minSmallButtonWidth, minitemheight, *pvf->optionsRemoveButton).withMargin(0).withFlex(0));
        pvf->optionsSendMutedBox.items.add(FlexItem(3, 12));
        pvf->optionsSendMutedBox.items.add(FlexItem(minSmallButtonWidth, minitemheight, *pvf->optionsBlockButton).withMargin(0).withFlex(0));

        
        pvf->optionsChangeAllQualBox.items.clear();
        pvf->optionsChangeAllQualBox.flexDirection = FlexBox::Direction::row;
        pvf->optionsChangeAllQualBox.items.add(FlexItem(10, minitemheight-10).withMargin(0).withFlex(1));
        pvf->optionsChangeAllQualBox.items.add(FlexItem(150, minitemheight-10, *pvf->changeAllFormatButton).withMargin(0).withFlex(0));

        
        pvf->optionsbuttbox.items.clear();
        pvf->optionsbuttbox.flexDirection = FlexBox::Direction::row;
        pvf->optionsbuttbox.items.add(FlexItem(12, 12).withFlex(1));
        pvf->optionsbuttbox.items.add(FlexItem(100, minitemheight, *pvf->optionsResetDropButton).withMargin(0).withFlex(0));
        pvf->optionsbuttbox.items.add(FlexItem(12, 12).withFlex(1));
        
        
        pvf->optionsaddrbox.items.clear();
        pvf->optionsaddrbox.flexDirection = FlexBox::Direction::row;
        pvf->optionsaddrbox.items.add(FlexItem(120, 14, *pvf->staticAddrLabel).withMargin(0).withFlex(1));
        pvf->optionsaddrbox.items.add(FlexItem(100, 14, *pvf->addrLabel).withMargin(0).withFlex(2));

        
        pvf->recvOptionsBox.items.clear();
        pvf->recvOptionsBox.flexDirection = FlexBox::Direction::column;
        pvf->recvOptionsBox.items.add(FlexItem(4, 4));
        if (relayedRow) {   // Latency line instead of the address row
            pvf->recvOptionsBox.items.add(FlexItem(100, minitemheight,  *pvf->hubLatLabel).withMargin(2).withFlex(0));
        } else {
            pvf->recvOptionsBox.items.add(FlexItem(100, 14,  pvf->optionsaddrbox).withMargin(2).withFlex(0));
        }
        pvf->recvOptionsBox.items.add(FlexItem(4, 2));
        if (!hideRowBuf) {  // See optionsNetbufBox above
            pvf->recvOptionsBox.items.add(FlexItem(100, minitemheight,  pvf->optionsNetbufBox).withMargin(2).withFlex(0));
            pvf->recvOptionsBox.items.add(FlexItem(4, 2));
        }
        if (!relayedRow) {  // No remote-format request for a hub feed (widgets hidden above)
            pvf->recvOptionsBox.items.add(FlexItem(100, minitemheight,  pvf->optionsRemoteQualityBox).withMargin(2).withFlex(0));
            pvf->recvOptionsBox.items.add(FlexItem(4, 8));
        }
        if (!relayedRow) {   // No Reset Dropped on relayed rows
            pvf->recvOptionsBox.items.add(FlexItem(100, minitemheight,  pvf->optionsbuttbox).withMargin(2).withFlex(0));
        }


        pvf->sendOptionsBox.items.clear();
        pvf->sendOptionsBox.flexDirection = FlexBox::Direction::column;
        pvf->sendOptionsBox.items.add(FlexItem(4, 4));
        pvf->sendOptionsBox.items.add(FlexItem(100, minitemheight,  pvf->optionsSendQualBox).withMargin(2).withFlex(0));
        pvf->sendOptionsBox.items.add(FlexItem(100, minitemheight-10,  pvf->optionsChangeAllQualBox).withMargin(2).withFlex(0));
        pvf->sendOptionsBox.items.add(FlexItem(4, 4));
        pvf->sendOptionsBox.items.add(FlexItem(100, minitemheight, pvf->optionsSendMutedBox).withMargin(0).withFlex(0));


        pvf->recvbox.items.clear();
        pvf->recvbox.flexDirection = FlexBox::Direction::row;

        pvf->sendbox.items.clear();
        pvf->sendbox.flexDirection = FlexBox::Direction::row;

        if (isNarrow) {

            /*
            pvf->recvbox.items.add(FlexItem(1, 5));
            pvf->recvbox.items.add(FlexItem(60, minitemheight, pvf->optionsstatbox).withMargin(0).withFlex(1));
            pvf->recvbox.items.add(FlexItem(3, 5));
            pvf->recvbox.items.add(FlexItem(100, minitemheight, pvf->netstatbox).withMargin(0).withFlex(1).withMaxWidth(130));
            pvf->recvbox.items.add(FlexItem(3, 5));

            pvf->sendbox.items.add(FlexItem(5, 2).withFlex(0));
            pvf->sendbox.items.add(FlexItem(100, minitemheight, pvf->recvstatbox).withMargin(0).withFlex(1));
            pvf->sendbox.items.add(FlexItem(3, 5));
             */

            pvf->recvbox.items.add(FlexItem(1, 5));
            if (!relayedRow) {   // No per-row format/rate
                pvf->recvbox.items.add(FlexItem(40, minitemheight, pvf->optionsstatbox).withMargin(0).withFlex(1).withMaxWidth(300));
                pvf->recvbox.items.add(FlexItem(3, 5));
            }
            pvf->recvbox.items.add(FlexItem(80, minitemheight, pvf->recvstatbox).withMargin(0).withFlex(1));
            // Hosted mode: the hub-status header carries the hub RTT, so netstatbox gives up its P2P flex slot here
            // and the remaining clusters reflow.
            if (!hideRowBuf) {
                pvf->recvbox.items.add(FlexItem(5, 5));
                pvf->recvbox.items.add(FlexItem(60, minitemheight, pvf->netstatbox).withMargin(0).withFlex(1).withMaxWidth(130));
            } else if (relayedRow) {   // (option D)
                pvf->netstatbox.items.clear();
                pvf->netstatbox.flexDirection = FlexBox::Direction::column;
                pvf->netstatbox.items.add(FlexItem(110, textheight, *pvf->staticLatencyLabel).withMargin(0).withFlex(0));   // 60 -> 110 (15-pt numbers)
                pvf->netstatbox.items.add(FlexItem(110, textheight, pvf->latencylabbox).withMargin(0).withFlex(0));
                pvf->staticLatencyLabel->setJustificationType(Justification::centred);
                pvf->recvbox.items.add(FlexItem(5, 5));
                pvf->recvbox.items.add(FlexItem(120, 2*textheight, pvf->netstatbox).withMargin(0).withFlex(1).withMaxWidth(150));
            }
            pvf->recvbox.items.add(FlexItem(3, 2).withFlex(0));

        }
        else {
            pvf->recvbox.items.add(FlexItem(1, 5));
            if (!relayedRow) {   // Format/rate per row is redundant (hub header says PCM16 once)
                pvf->recvbox.items.add(FlexItem(60, minitemheight, pvf->optionsstatbox).withMargin(0).withFlex(1).withMaxWidth(300));
                pvf->recvbox.items.add(FlexItem(3, 5));
            }
            pvf->recvbox.items.add(FlexItem(100, minitemheight, pvf->recvstatbox).withMargin(0).withFlex(1));
            // Hosted mode: drop the netstatbox flex slot; see narrow branch.
            if (!hideRowBuf) {
                pvf->recvbox.items.add(FlexItem(5, 5));
                pvf->recvbox.items.add(FlexItem(100, minitemheight, pvf->netstatbox).withMargin(0).withFlex(1).withMaxWidth(130));
            } else if (relayedRow) {
                // "Latency (ms)  ↑ x.x  ↓ y.y  Ping z" at the stock spot, one line
                pvf->netstatbox.items.clear();
                pvf->netstatbox.flexDirection = FlexBox::Direction::row;
                pvf->netstatbox.items.add(FlexItem(72, textheight, *pvf->staticLatencyLabel).withMargin(0).withFlex(0));   // Even slots: label 72 | 6 | ↑ 40 | 6 | ↓ 40 | 10 | Ping 30 | 4 | value 30 = 240
                pvf->netstatbox.items.add(FlexItem(72, textheight, pvf->latencylabbox).withMargin(0).withFlex(0));   // Slots 12|23|14|23
                pvf->netstatbox.items.add(FlexItem(2, 2));   // No extra space before Ping
                pvf->netstatbox.items.add(FlexItem(30, textheight, *pvf->staticPingLabel).withMargin(0).withFlex(0));   // Their ping at the end
                pvf->netstatbox.items.add(FlexItem(1, 2));   // "one less space after Ping"
                pvf->netstatbox.items.add(FlexItem(22, textheight, *pvf->pingLabel).withMargin(0).withFlex(0));   // 28 -> 22, line sits further right
                pvf->latencyUpLabel->setJustificationType(Justification::centredLeft);   // The number stays next to its arrow
                pvf->latencyUpLabel->setBorderSize(BorderSize<int>(0, 1, 0, 0));     // Number right next to its arrow
                pvf->latencyDownLabel->setBorderSize(BorderSize<int>(0, 1, 0, 0));
                pvf->pingLabel->setMinimumHorizontalScale(0.75f);
                pvf->latencyDownLabel->setJustificationType(Justification::centredLeft);
                pvf->staticPingLabel->setBorderSize(BorderSize<int>(0, 2, 0, 1));   // No label padding between "Ping" and its value
                pvf->pingLabel->setBorderSize(BorderSize<int>(0, 1, 0, 2));
                pvf->pingLabel->setJustificationType(Justification::centredLeft);   // number right after the word
                pvf->pingLabel->setFont(pvf->latencyDownLabel->getFont());        // same size as the latency numbers
                pvf->staticLatencyLabel->setJustificationType(Justification::centredRight);
                pvf->staticPingLabel->setJustificationType(Justification::centredRight);
                pvf->staticPingLabel->setVisible(true); pvf->pingLabel->setVisible(true);
                pvf->recvbox.items.add(FlexItem(5, 5));
                pvf->recvbox.items.add(FlexItem(199, textheight, pvf->netstatbox).withMargin(0).withFlex(0).withMaxWidth(204));   // Slots 72|72|2|30|1|22 = 199
            }
            pvf->recvbox.items.add(FlexItem(3, 2).withFlex(0));
        }



        int chcnt = processor.getRemotePeerRecvChannelCount(i);
        int meterwidth = kSbMeterPxPerChannel * jmax(1, chcnt);   // Same per-channel width as the main meters
        pvf->laidOutMeterChans = chcnt;   // UpdatePeerViews re-lays the row when this goes stale

        pvf->mainbox.items.clear();
        pvf->mainnarrowbox.items.clear();

        pvf->isNarrow = isNarrow;

        pvf->channelGroups->setNarrowMode(isNarrow, false);
        // Relayed rows draw the full-height meter outside the channel view. The flag must be set before updateLayout
        // below, or the row shows two meters.
        pvf->channelGroups->setRowMeterHidden(relayedRow && !isNarrow);   // minimal (single-row) view too
        pvf->channelGroups->updateLayout(false);

        auto changroupminbounds = pvf->channelGroups->getMinimumContentBounds();


        if (isNarrow) {
            pvf->mainnarrowbox.flexDirection = FlexBox::Direction::column;
            pvf->mainnarrowbox.items.add(FlexItem(2, 3));
            //pvf->mainnarrowbox.items.add(FlexItem(100, minitemheight, pvf->mainsendbox).withMargin(0).withFlex(0));
            pvf->mainnarrowbox.items.add(FlexItem(changroupminbounds.getWidth(), changroupminbounds.getHeight(), *pvf->channelGroups).withMargin(0).withFlex(0).withWidth((float) FlexItem::notAssigned));   // Auto width = stretched to the row (JUCE never stretches an item with an assigned width)

            if (fullmode) {
                pvf->mainnarrowbox.items.add(FlexItem(2, 2));
                pvf->mainnarrowbox.items.add(FlexItem(260, minitemheight, pvf->recvbox).withMargin(0).withFlex(0).withWidth((float) FlexItem::notAssigned));   // Stretched
                //pvf->mainnarrowbox.items.add(FlexItem(2, 4));
                //pvf->mainnarrowbox.items.add(FlexItem(260, minitemheight, pvf->sendbox).withMargin(0).withFlex(0));
            }

            int nbh = 0;
            for (auto & item : pvf->mainnarrowbox.items) {
                nbh += item.minHeight;
            }

            pvf->mainbox.flexDirection = FlexBox::Direction::row;
            pvf->mainbox.items.add(FlexItem(3, 2));
            pvf->mainbox.items.add(FlexItem(150, nbh, pvf->mainnarrowbox).withMargin(0).withFlex(1));
            pvf->recvMeter->setVisible(false);   // Narrow keeps the inline meter (flag set false above)
            pvf->recvMeter->setRefreshRateHz(0.0f);
        } else {
            pvf->mainnarrowbox.flexDirection = FlexBox::Direction::column;
            //pvf->mainnarrowbox.items.add(FlexItem(2, 3));

            //pvf->mainnarrowbox.items.add(FlexItem(100, minitemheight, pvf->mainsendbox).withMargin(0).withFlex(0));
            pvf->mainnarrowbox.items.add(FlexItem(changroupminbounds.getWidth(), changroupminbounds.getHeight(), *pvf->channelGroups).withMargin(0).withFlex(0).withWidth((float) FlexItem::notAssigned));   // Auto width = stretched to the row (JUCE never stretches an item with an assigned width)

            if (fullmode) {
                pvf->mainnarrowbox.items.add(FlexItem(2, 4));
                // Relayed rows carry one text line on line 2
                pvf->mainnarrowbox.items.add(FlexItem(150, relayedRow ? textheight + 4 : minitemheight, pvf->recvbox).withMargin(0).withFlex(0).withWidth((float) FlexItem::notAssigned));   // Stretched to the row, so the latency block sits at the right edge instead of overflowing a 150-px box
            }

            int nbh = 0;
            for (auto & item : pvf->mainnarrowbox.items) {
                nbh += item.minHeight;
            }

            pvf->mainbox.flexDirection = FlexBox::Direction::row;
            pvf->mainbox.items.add(FlexItem(3, 2));
            pvf->mainbox.items.add(FlexItem(150, nbh, pvf->mainnarrowbox).withMargin(0).withFlex(1));
            // The VU meter uses the whole row height: drawn here as a right-hand column spanning both lines; the
            // channel group's inline meter is hidden.
            if (relayedRow) {   // all display modes
                pvf->mainbox.items.add(FlexItem(2, 2));
                {   // Tall grouped meter only while COLLAPSED; expanded rows get ONE meter per channel group
                    // (stereo = two bars side by side, same per-bar width as the top/bottom meters) in the same column.
                    const bool collapsed = !processor.getRemotePeerViewExpanded(i);
                    pvf->recvMeter->setVisible(collapsed && !isNarrow);
                    int ngroups = 0, maxch = 1;
                    if (!collapsed && !isNarrow) {
                        ngroups = processor.getRemotePeerChannelGroupCount(i);
                        for (int g = 0; g < ngroups; ++g) { int st = 0, ct = 0; if (processor.getRemotePeerChannelGroupStartAndCount(i, g, st, ct)) maxch = jmax(maxch, ct); }
                    }
                    pvf->groupMeterChans.resize(ngroups);
                    for (int g = 0; g < ngroups; ++g) {
                        if (g >= pvf->groupMeters.size()) {
                            auto* m = pvf->groupMeters.add(new SbChannelTipMeter(foleys::LevelMeter::Minimal));
                            m->setLookAndFeel(&(pvf->rmeterLnf));
                            m->setUseBackgroundImage(true);
                            m->showChannelNumbers = true;    // Same tiny numbers as the tall meter (absolute channel numbers)
                            m->nameForChannel = pvf->recvMeter->nameForChannel;   // hover names the channel, like the tall meter
                            pvf->addChildComponent(m);
                        }
                        auto* m = pvf->groupMeters[g];
                        int st = 0, ct = 0; processor.getRemotePeerChannelGroupStartAndCount(i, g, st, ct);
                        m->bindSource(processor.getRemotePeerRecvMeterSource(i));
                        m->setSelectedChannel(st);                 // our LnF draws fixedNumChannels bars starting here
                        m->setFixedNumChannels(jmax(1, ct));
                        m->tipFirstChannel = st; m->tipNumChannels = jmax(1, ct);
                        m->setRefreshRateHz(8.0f);
                        m->setVisible(true);
                        pvf->groupMeterChans.set(g, jmax(1, ct));
                    }
                    for (int g = ngroups; g < pvf->groupMeters.size(); ++g) { pvf->groupMeters[g]->setVisible(false); pvf->groupMeters[g]->setRefreshRateHz(0.0f); }
                    pvf->mainbox.items.add(FlexItem(4, 2));   // A little air after Ping   // 10 -> 4, closer to the meter
                    // No shared reserve: each row's block hugs its own meter.
                    if (collapsed) pvf->mainbox.items.add(FlexItem(meterwidth, nbh, *pvf->recvMeter).withMargin(2).withFlex(0));
                    else           pvf->mainbox.items.add(FlexItem(kSbMeterPxPerChannel * maxch, nbh, *pvf->groupMeterColumn).withMargin(2).withFlex(0));
                }
                pvf->recvMeter->setRefreshRateHz(8.0f);   // was 0 (XXX) → the tall meter painted once and froze
            } else {
                pvf->recvMeter->setRefreshRateHz(0.0f);
            }
        }

        peersBox.items.add(FlexItem(8, 4).withMargin(0));
        peersheight += 4;

        int mch = 0;
        for (auto & item : pvf->mainbox.items) {
            mch = jmax(mch, (int)item.minHeight);
        }

        if (isNarrow) {
            peersBox.items.add(FlexItem(pw, mch + 3, *pvf).withMargin(0).withFlex(0));
            peersheight += mch + 3;
        } else {
            peersBox.items.add(FlexItem(pw, mch + 5, *pvf).withMargin(1).withFlex(0));
            peersheight += mch + 5;
        }

    }


    // Pending connection peerviews

    int ppheight = minitemheight;
    int ppw = 120;

    for (int i=0; i < mPendingPeerViews.size(); ++i) {

        peersBox.items.add(FlexItem(8, 4).withMargin(0));
        peersheight += 4;

        PendingPeerViewInfo * ppvf = mPendingPeerViews.getUnchecked(i);
        ppvf->mainbox.items.clear();
        ppvf->mainbox.flexDirection = FlexBox::Direction::row;
        ppvf->mainbox.items.add(FlexItem(110, minitemheight, *ppvf->nameLabel).withMargin(0).withFlex(0));
        ppvf->mainbox.items.add(FlexItem(minButtonWidth-14, minitemheight, *ppvf->removeButton).withMargin(3).withFlex(0));
        ppvf->mainbox.items.add(FlexItem(110, minitemheight, *ppvf->messageLabel).withMargin(0).withFlex(1));
        ppvf->mainbox.items.add(FlexItem(minButtonWidth-14, minitemheight, *ppvf->unblockButton).withMargin(3).withFlex(0));

        peersBox.items.add(FlexItem(ppw, ppheight + 5, *ppvf).withMargin(1).withFlex(0));
        peersheight += ppheight + 5;
    }


    // get total min height on peersbox
    int totPeersHeight = 0;
    for (auto & item : peersBox.items) {
        totPeersHeight += item.minHeight + item.margin.top + item.margin.bottom;
    }

    if (isNarrow) {
        peersMinHeight = totPeersHeight;
        peersMinWidth = 300;
    }
    else {
        peersMinHeight = totPeersHeight;
        peersMinWidth = 180 + 150 + mincheckheight + 50;
    }
    
}


int PeersContainerView::getPeerFromIndex(int index)
{
    if (index >= 0 && index < mPeerUpdateOrdering.size()) {

        return mPeerUpdateOrdering[index];
    }

    return 0;
}

int PeersContainerView::getPeerForPoint(Point<int> pos, bool inbetween)
{
    int i=0;
    for (; i < mPeerViewBounds.size(); ++i) {
        auto bounds = mPeerViewBounds.getUnchecked(i);

        if (inbetween) {
            // round it from midpoints
            auto tophalf = bounds.withTrimmedBottom(bounds.getHeight()/2);
            auto bottomhalf = bounds.withTrimmedTop(bounds.getHeight()/2);
            if (tophalf.contains(pos) || pos.getY() < bounds.getY()) {
                return i;
            }
            else if (bottomhalf.contains(pos)) {
                return i+1;
            }
        }
        else {
            if (bounds.contains(pos)) {
                return i;
            }
            if (pos.getY() < bounds.getY()) {
                // return one less
                return i-1;
            }
        }
    }

    return i;
}

juce::Rectangle<int> PeersContainerView::getBoundsForPeer(int chgroup)
{
    if (chgroup >= 0 && chgroup < mPeerViewBounds.size()) {
        return mPeerViewBounds.getUnchecked(chgroup);
    }
    // otherwise return a line after the last of them
    if (!mPeerViewBounds.isEmpty()) {
        auto lastone = mPeerViewBounds.getLast();
        return Rectangle<int>(lastone.getX(), lastone.getBottom(), lastone.getWidth(), 0);
    }
    return {};
}



void PeersContainerView::mouseDown (const MouseEvent& event)
{
    mIgnoreNameClick = false;

    // Drop counts are cumulative since join and never self-reset in hub
    // mode; right-click the header readout (all rows) or a row's readout (that row) to reset.
    if (event.mods.isPopupMenu()) {
        if (mHubStatusLabel && event.eventComponent == mHubStatusLabel.get()) {
            for (int i = 0; i < processor.getNumberRemotePeers(); ++i) processor.resetRemotePeerPacketStats(i);
            updatePeerViews();
            return;
        }
        for (int i = 0; i < mPeerViews.size(); ++i) {
            auto * pvf = mPeerViews.getUnchecked(i);
            if (event.eventComponent == pvf->hostedDropsLabel.get()
                || (pvf->recvActualBitrateLabel && event.eventComponent == pvf->recvActualBitrateLabel.get())) {
                processor.resetRemotePeerPacketStats(i);
                updatePeerViews();
                return;
            }
        }
    }

    for (int i=0; i < mPeerViews.size(); ++i) {
        auto * pvf = mPeerViews.getUnchecked(i);

        if ( pvf->channelGroups->isDraggable(event.eventComponent)) {
            DBG("Mouse down on peer " << i);
            //int peerindex = mPeerUpdateOrdering[i];
            mDraggingSourcePeer = i; // peerindex;
            break;
        }
    }
}

void PeersContainerView::mouseDrag (const MouseEvent& event)
{
    for (int i=0; i < mPeerViews.size(); ++i) {
        auto * pvf = mPeerViews.getUnchecked(i);

        if ( (pvf->channelGroups->isDraggable(event.eventComponent)
              )) {
            auto adjpos =  getLocalPoint(event.eventComponent, event.getPosition());
            //int peerindex = mPeerUpdateOrdering[i];

            DBG("Dragging peer view: " << adjpos.toString());
            if (abs(event.getDistanceFromDragStartY()) > 4 && !mDraggingActive) {
                // start drag behavior
                mDraggingSourcePeer = i; // peerindex;
                mDraggingActive = true;
                mIgnoreNameClick = true;
                mDraggingGroupPos = getPeerForPoint(adjpos, true);
                auto groupbounds = getBoundsForPeer(mDraggingSourcePeer);
                mDragImage = createComponentSnapshot(groupbounds);
                mDragDrawable->setImage(mDragImage);
                mDragDrawable->setVisible(true);
                mDragDrawable->setBounds(groupbounds.getX(), adjpos.getY() - groupbounds.getHeight()/2, groupbounds.getWidth(), groupbounds.getHeight());
            }
            else if (mDraggingActive) {
                // adjust drag indicator
                int pindex = getPeerForPoint(adjpos, true);
                DBG("In peer: " << pindex);

                mDragDrawable->setBounds(mDragDrawable->getX(), adjpos.getY() - mDragDrawable->getHeight()/2, mDragDrawable->getWidth(), mDragDrawable->getHeight());

                if (auto viewport = findParentComponentOfClass<Viewport>()) {
                    auto vppos = viewport->getLocalPoint(this, adjpos);
                    if (viewport->autoScroll(vppos.getX(), vppos.getY(), 8, 8)) {
                        if (!mAutoscrolling) {
                            event.eventComponent->beginDragAutoRepeat(40);
                            mAutoscrolling = true;
                        }
                    } else if (mAutoscrolling){
                        event.eventComponent->beginDragAutoRepeat(0);
                        mAutoscrolling = false;
                    }
                }

                if (pindex != mDraggingGroupPos) {
                    // insert point changed, update it
                    mDraggingGroupPos = pindex;

                    auto groupbounds = getBoundsForPeer(mDraggingGroupPos);
                    groupbounds.setHeight(0);
                    groupbounds.setWidth(getWidth() - 16);
                    groupbounds.setX(7);
                    mInsertLine->setRectangle (groupbounds.toFloat());

                    int delta = mDraggingGroupPos - mDraggingSourcePeer;
                    bool canmove = delta > 1 || delta < 0;
                    mInsertLine->setVisible(canmove);
                }
            }
            break;
        }
    }
}

void PeersContainerView::mouseUp (const MouseEvent& event)
{
    for (int i=0; i < mPeerViews.size(); ++i) {
        auto * pvf = mPeerViews.getUnchecked(i);

        if (event.eventComponent == pvf->recvMeter.get()) {
            pvf->recvMeter->clearClipIndicator(-1);
            pvf->channelGroups->clearClipIndicators();
            break;
        }
        else if (event.eventComponent == pvf->addrLabel.get()) {
            pvf->addrClicked = true;
            updatePeerViews(i);
            break;
        }
        else if (pvf->channelGroups->isDraggable(event.eventComponent)) {
            if (mDraggingActive) {
                DBG("Mouse up after drag: " << event.getPosition().toString() << " srcpeer: " << mDraggingSourcePeer << " destpos: " << mDraggingGroupPos);
                // commit it
                int delta = mDraggingGroupPos - mDraggingSourcePeer;
                bool canmove = delta > 1 || delta < 0;


                int srcpeer = mPeerUpdateOrdering[mDraggingSourcePeer];

                // need to set priority ordering on all them
                int offs = 0;

                for (int j=0; j < mPeerViews.size(); ++j) {
                    if (j == mDraggingSourcePeer) continue;

                    int peerind = mPeerUpdateOrdering[j];
                    if (j == mDraggingGroupPos) {
                        offs = 1;
                    }

                    String pname = processor.getRemotePeerUserName(peerind);
                    if (pname.isNotEmpty()) {
                        mPeerPriorityOrdering[pname] = j + offs;
                        processor.setRemotePeerOrderPriority(peerind, j+offs);
                        DBG("Setting drag prior for " << pname << " to: " << j + offs);
                    }
                }

                String pname = processor.getRemotePeerUserName(srcpeer);
                if (pname.isNotEmpty()) {
                    mPeerPriorityOrdering[pname] = mDraggingGroupPos;
                    processor.setRemotePeerOrderPriority(srcpeer, mDraggingGroupPos);
                    DBG("Setting drag src for " << pname << " to: " << mDraggingGroupPos);
                }


                rebuildPeerViews();   // A drag changes which player each row shows, so the row setup must follow

                //if (canmove && processor.moveInputChannelGroupTo(mDraggingSourceGroup, mDraggingGroupPos)) {
                //    // moved it
                //    processor.updateRemotePeerUserFormat();
                //    rebuildChannelViews();
                //}

                mInsertLine->setVisible(false);
                mDragDrawable->setVisible(false);
                mDraggingActive = false;
                mAutoscrolling = false;
            }

            break;
        }
    }
}




Rectangle<int> PeersContainerView::getMinimumContentBounds() const
{
    return Rectangle<int>(0,0,peersMinWidth, peersMinHeight);
}

void PeersContainerView::applyToAllSliders(std::function<void(Slider *)> & routine)
{
    for (int i=0; i < mPeerViews.size(); ++i) {
        PeerViewInfo * pvf = mPeerViews.getUnchecked(i);
        pvf->channelGroups->applyToAllSliders(routine);
    }
}

// Show the real buffer/pin value: "2.67 ms", not "3 ms". Whole values stay integers.
static String sbFormatMs(float ms)
{
    const float r = std::round(ms);
    if (std::abs(ms - r) < 0.05f) return String::formatted("%d ms", (int) r);
    String v = String(ms, 2);
    while (v.endsWith("0")) v = v.dropLastCharacters(1);
    if (v.endsWith(".")) v = v.dropLastCharacters(1);
    return v + " ms";
}

void PeersContainerView::updatePeerViews(int specific)
{
    uint32 nowstampms = Time::getMillisecondCounter();
    bool needsUpdateLayout = false;

    // HostedMix: refresh the hub-status header (replaces hub peer row).
    if (mHubStatusLabel != nullptr && processor.isHostedMixModeEnabled()) {
        int hubIdx = -1;
        const int npeers = processor.getNumberRemotePeers();
        for (int i = 0; i < npeers; ++i) {
            if (processor.isHubPeer(processor.getRemotePeerUserName(i))) {
                hubIdx = i;
                break;
            }
        }
        String htext;
        // The hub peer row survives a disconnect or refused join by design (rebind), so the test is the session
        // itself: server + group.
        const bool inSession = processor.isConnectedToServer() && processor.getCurrentJoinedGroup().isNotEmpty();
        if (hubIdx < 0 || !inSession) {
            htext = (hubIdx < 0 && inSession) ? TRANS("Hub: not connected") : String();
            // The line paints from the rich copy whenever one exists, so an empty text changed nothing
            if (auto* rl = dynamic_cast<SbRichLabel*>(mHubStatusLabel.get())) rl->clearRich();
            mHubStatusLabel->setColour(Label::textColourId, mutedTextColor);
        } else {
            bool hubconn = processor.getRemotePeerConnected(hubIdx);
            SonobusAudioProcessor::LatencyInfo linfo;
            bool haveLat = processor.getRemotePeerLatencyInfo(hubIdx, linfo);
            // Show whatever values we have: SonoBus only sets isreal=true after an explicit latency test, so gating
            // on it would leave the header reading "?" indefinitely. Per-peer rows show the values without that gate
            // too.
            //   "<hub> · send X / recv Y / ping Z ms · drops D"
            // A single trailing "ms" covers the latency triple; "connected" is implied (a disconnect shows the word +
            // muted colour). Group name and player count live in the editor's top bar (mMainGroupLabel /
            // mMainPeerLabel), so they are not repeated here.
            // One latency model everywhere: the hub's measured legs for us (same computeHubLatency as every row on
            // every client); the stock P2P estimate (integer ms) is only a fallback before the first report.
            String sendStr = haveLat ? String((int)lrintf(linfo.outgoingMs)) : "?";
            String recvStr = haveLat ? String((int)lrintf(linfo.incomingMs)) : "?";
            String pingStr = haveLat ? String((int)lrintf(linfo.pingMs)) : "?";
            { float sTo = -1, sFrom = -1, sRtt = -1; int64_t sLost = -1;
              if (processor.getSelfHubLatency(sTo, sFrom, sRtt, sLost)) {
                  // Whole milliseconds in the header (tenths stay in the hover tooltip below)
                  sendStr = String((int) std::lround(sTo)); recvStr = String((int) std::lround(sFrom)); pingStr = sRtt >= 0.0f ? String((int) std::lround(sRtt)) : pingStr; }
              else if (hubconn) {   // A dark hub feed is never silent — '~' marks the local estimate
                  sendStr = "~" + sendStr; recvStr = "~" + recvStr; pingStr = "~" + pingStr; } }
            // Aggregate dropped-packet total across the player feeds (sum of the per-row readouts; the hub
            // bookkeeping peer has no audio feed and is skipped). Reads counters the engine already maintains;
            // nothing added on the audio thread.
            int64_t totaldrops = 0;   // Under hub bundling every row loses the same datagram → the link's count is the MAX, not the sum
            for (int pi = 0; pi < npeers; ++pi) {
                if (processor.isHubPeer(processor.getRemotePeerUserName(pi))) continue;
                totaldrops = jmax(totaldrops, processor.getRemotePeerPacketsDropped(pi));
            }
            const String hsep = String::fromUTF8("  \xe2\x94\x82  "); // "  │  " field divider (was a small dot)
            // The line is built in parts so each field can carry its own colour (SbRichLabel):
            // head (hub, port, format) regular; latency legs + ping coloured like the rows; Mb/s regular; drops coloured by the last minute.
            String headPart, ratePart, dropsPart, recPart;
            {   // Show which hub, as the player typed it (DNS name or IP), not the fixed hub name
                // + the SIGNALING port (what a friend types), then the format right after the address
                const String typed = processor.getServerHostAsTyped();
                const int sport = processor.getServerPortAsTyped();
                if (typed.isNotEmpty()) headPart << TRANS("Hub") << " " << typed;
                else                    headPart << processor.getRemotePeerUserName(hubIdx);
                if (sport > 0) headPart << ":" << sport;
            }
            if (!hubconn) headPart << "  " << TRANS("disconnected");
            headPart << hsep << "PCM 16 bit" << hsep << TRANS("Latency (ms)") << " ";
            const String upStr = String::fromUTF8("\xe2\x86\x91 ") + sendStr;
            const String dnStr = String::fromUTF8("\xe2\x86\x93 ") + recvStr;
            const String pingPart = TRANS("Ping") + " " + pingStr;   // No slash, one unit label
            dropsPart << hsep << TRANS("drops from hub") << " " << String(totaldrops);   // your own hub→you link (rows carry each player's uplink)
            {   // Who is recording, anywhere on the hub (you included)
                StringArray recs;
                if (processor.isRecordingToFile()) recs.add(TRANS("you"));
                for (int pi = 0; pi < npeers; ++pi) {
                    if (processor.isHubPeer(processor.getRemotePeerUserName(pi))) continue;
                    if (processor.isRemotePeerRecording(pi)) recs.add(processor.getRemotePeerUserName(pi));
                }
                if (recs.size() > 0) recPart << hsep << String::fromUTF8("\xe2\x97\x8f ") << TRANS("REC") << ": " << recs.joinIntoString(", ");
            }
            htext << headPart << upStr << "  " << dnStr << "  " << pingPart << ratePart << dropsPart << recPart;
            {   // colours
                const uint32 nowms = Time::getMillisecondCounter();
                if (mHubDropsN == 0 || nowms - mHubDropsT[mHubDropsN - 1] >= 5000) {
                    if (mHubDropsN == 12) { for (int k = 1; k < 12; ++k) { mHubDropsT[k-1] = mHubDropsT[k]; mHubDropsC[k-1] = mHubDropsC[k]; } mHubDropsN = 11; }
                    mHubDropsT[mHubDropsN] = nowms; mHubDropsC[mHubDropsN] = totaldrops; mHubDropsN++;
                }
                int64_t recent = 0;
                for (int k = 0; k < mHubDropsN; ++k) { if (nowms - mHubDropsT[k] <= 60000) { recent = totaldrops - mHubDropsC[k]; break; } }
                const Colour reg = hubconn ? regularTextColor : mutedTextColor;
                auto latcol  = [this](float ms){ return ms < sbplus::kLatOneWayGreenMs ? latGoodColor : ms < sbplus::kLatOneWayYellowMs ? latWarnColor : latBadColor; };   // Shared thresholds (SbLatencyColours.h)
                auto pingcol = [this](float ms){ return ms < sbplus::kLatRttGreenMs ? latGoodColor : ms < sbplus::kLatRttYellowMs ? latWarnColor : latBadColor; };   // round-trip scale
                const Colour dropcol = recent < 5 ? latGoodColor : recent <= 20 ? latWarnColor : latBadColor;   // per-minute, same bands as the rows
                float cTo = -1, cFrom = -1, cRtt = -1; int64_t cLost = -1;
                const bool haveSelf = processor.getSelfHubLatency(cTo, cFrom, cRtt, cLost);
                {   // Say why the numbers are '~' (local estimate) when the hub's latency feed is dark
                    const int age = processor.getSelfHubLatencyAgeS();
                    // Explain the fields on hover; the '~' note is appended when the hub's feed is dark
                    String tip = (haveSelf ? "To hub " + String(cTo, 1) + "\nFrom hub " + String(cFrom, 1) + "\nRound trip " + String(cRtt, 1) + " ms"
                                           : String("To hub\nFrom hub\nRound trip")) + "\ndrops = lost on the way to you";   // One data point per line
                    tip = String::fromUTF8(tip.toRawUTF8());
                    if (!(haveSelf || !hubconn))
                        tip << "\n" << (age < 0 ? TRANS("~ = local estimate: no latency report from the hub yet")
                                             : TRANS("~ = local estimate: no latency report from the hub for ") + String(age) + " s");
                    if (mHubStatusLabel->getTooltip() != tip) mHubStatusLabel->setTooltip(tip);
                }
                if (auto* rl = dynamic_cast<SbRichLabel*>(mHubStatusLabel.get())) {
                    AttributedString as; as.setJustification(Justification::centredLeft); as.setWordWrap(AttributedString::none);
                    const Font f = mHubStatusLabel->getFont();
                    as.append(headPart, f, reg);
                    const Colour est = hubconn ? mutedTextColor : reg;   // '~' local-estimate fields are dimmed
                    as.append(upStr, f, haveSelf && hubconn ? latcol(cTo) : est);
                    as.append("  ", f, reg);
                    as.append(dnStr, f, haveSelf && hubconn ? latcol(cFrom) : est);
                    as.append("  ", f, reg);
                    as.append(pingPart, f, haveSelf && hubconn && cRtt >= 0.0f ? pingcol(cRtt) : est);
                    as.append(ratePart, f, reg);
                    as.append(dropsPart, f, hubconn ? dropcol : reg);
                    as.append(recPart, f, reg);
                    rl->setRich(as);
                }
            }
            mHubStatusLabel->setColour(Label::textColourId,
                hubconn ? regularTextColor : mutedTextColor);
        }
        mHubStatusLabel->setText(htext, dontSendNotification);


        // Refresh the unified receive-buffer header row: one value/mode for all player sinks (shared hub->us path).
        // Mirrors the per-row bufferLabel display convention.
        if (mUnifiedBufferTimeSlider != nullptr) {
            float unibufms = processor.getUnifiedReceiveBufferTime();
            int unimode = (int) processor.getUnifiedReceiveAutoresizeBufferMode();

            mUnifiedAutosizeButton->setSelectedId(unimode, dontSendNotification);

            // Display sync: in auto modes the live sinks land wherever the network dictates, not at the stored policy
            // value, so show the median live sink buffer across player peers. Manual mode shows the set value. Rides
            // the updatePeerViews cadence. Hub peer skipped: its sink carries no audio.
            float shownbufms = unibufms;
            if (unimode != SonobusAudioProcessor::AutoNetBufferModeOff) {
                Array<float> livebufs;
                const int nrpeers = processor.getNumberRemotePeers();
                for (int pi = 0; pi < nrpeers; ++pi) {
                    if (processor.isHubPeer(processor.getRemotePeerUserName(pi))) continue;
                    livebufs.add(processor.getRemotePeerBufferTime(pi));
                }
                if (!livebufs.isEmpty()) {
                    livebufs.sort();
                    shownbufms = livebufs[livebufs.size() / 2]; // median (upper on ties)
                }
            }

            String unibuflab = (unimode == SonobusAudioProcessor::AutoNetBufferModeOff ? "" :
                                unimode == SonobusAudioProcessor::AutoNetBufferModeAutoIncreaseOnly ? " (Auto+)" :
                                unimode == SonobusAudioProcessor::AutoNetBufferModeInitAuto ? " (IA)" :
                                unimode == SonobusAudioProcessor::AutoNetBufferModeMeasureCommitPin ? " (Pin)" :
                                " (Auto)");
            String unibuftext = sbFormatMs(shownbufms) + unibuflab;
            // The control has its stock meaning in hub mode: Manual pins the hub feed at the slider value, any Auto
            // mode is adaptive. Show which, plus the depth actually in force. No LAN/WAN wording.
            if (processor.isHostedMixModeEnabled()) {
                const bool manual = unimode == SonobusAudioProcessor::AutoNetBufferModeOff;
                unibuftext = String(manual ? "Manual " : "Auto ") + sbFormatMs(manual ? unibufms : processor.getHostedFeedDepthMs());
            }
            mUnifiedBufLabel->setText(unibuftext, dontSendNotification);
            // Header chip mirrors the value label (same string, same cadence); "▾" hints at the popup holding the
            // slider + auto-mode combo.
            if (mUnifiedBufChipButton != nullptr) {
                mUnifiedBufChipButton->setButtonText(unibuftext + " " + String::fromUTF8("\xe2\x96\xbe"));
            }

            if (!mUnifiedBufferTimeSlider->isMouseButtonDown()) {
                mUnifiedBufferTimeSlider->setValue(unibufms, dontSendNotification);
            }
        }
    }


    //    for (int i=0; i < mPeerViews.size(); ++i) {
    for (int di=0; di < mPeerUpdateOrdering.size(); ++di) {
        int i = mPeerUpdateOrdering[di];

        if (specific >= 0 && specific != i) continue;
        if (di >= mPeerViews.size()) {
            DBG("Shouldnt happen update ordering greater than peerview size!");
            break;
        }
        PeerViewInfo * pvf = mPeerViews.getUnchecked(di);

        pvf->channelGroups->setPeerMode(true, i);

        bool connected = processor.getRemotePeerConnected(i);
        auto fullmode = pvf->fullMode;

        String hostname;
        int port = 0;
        processor.getRemotePeerAddressInfo(i, hostname, port);

        String addrname;
        addrname << hostname << " : " << port;

        if (pvf->addrClicked) {
            pvf->addrLabel->setText(addrname, dontSendNotification);
        } else {
            pvf->addrLabel->setText(TRANS("<Press to show>"), dontSendNotification);
        }

        String sendtext;
        bool sendactive = processor.getRemotePeerSendActive(i);
        bool sendallow = processor.getRemotePeerSendAllow(i);

        bool recvactive = processor.getRemotePeerRecvActive(i);
        bool recvallow = processor.getRemotePeerRecvAllow(i);
        bool latactive = processor.isRemotePeerLatencyTestActive(i);
        bool safetymuted = processor.getRemotePeerSafetyMuted(i);
        bool blocked = processor.getRemotePeerBlockedUs(i);

        const int chcnt = processor.getRemotePeerRecvChannelCount(i);

        double sendrate = 0.0;
        double recvrate = 0.0;
        
        if (lastUpdateTimestampMs > 0) {
            double timedelta = (nowstampms - lastUpdateTimestampMs) * 1e-3;
            int64_t nbs = processor.getRemotePeerBytesSent(i);
            // Relayed rows share the hub socket — count THIS player's stream instead
            const bool relayedRowRate = processor.isHostedMixModeEnabled() && !processor.isHubPeer(processor.getRemotePeerUserName(i)) && !pvf->infoOnly;
            int64_t nbr = relayedRowRate ? processor.getRemotePeerStreamBytesReceived(i) : processor.getRemotePeerBytesReceived(i);
            if (relayedRowRate && nbr < pvf->lastBytesRecv) pvf->lastBytesRecv = nbr;   // 32-bit wrap or stream reset
            
            sendrate = (nbs - pvf->lastBytesSent) / timedelta;
            recvrate = (nbr - pvf->lastBytesRecv) / timedelta;
            
            pvf->lastBytesRecv = nbr;
            pvf->lastBytesSent = nbs;
        }

        // Info-only peer rows have no direct send/recv plumbing: the active/allow/blocked checks all read false, so
        // the chain would end in "SEND DISABLED" / "You muted them" although the row's mute state lives in the
        // hub-mix override. Relayed peers (non-hub, non-info-only peers in HostedMix) have sendActive=false because
        // audio goes via the hub; without this guard the !sendactive + sendallow branch below would show "Other end
        // muted us".
        const bool relayedViaHub = !pvf->infoOnly
            && processor.isHostedMixModeEnabled()
            && !processor.isHubPeer(processor.getRemotePeerUserName(i));
        if (pvf->infoOnly || relayedViaHub) {
            // Show this player's hub-reported latency: their uplink (to hub, including the hub's inbound buffer) and
            // the hub->them one-way. Blank until the first /sb/hxlat arrives and again if it goes stale. The row's
            // latency lives in the stock latency labels below; this hidden label only feeds the popup line.
            float toHub = -1.0f, fromHub = -1.0f;
            if (processor.getRemotePeerHubLatency(i, toHub, fromHub)) {
                // Whole milliseconds
                pvf->hubLatLabel->setText(String(CharPointer_UTF8("\xe2\x86\x91")) + " " + String((int) std::lround(toHub)) + " ms   "
                                          + String(CharPointer_UTF8("\xe2\x86\x93")) + " " + String((int) std::lround(fromHub)) + " ms", dontSendNotification);
            } else {
                pvf->hubLatLabel->setText(TRANS("Latency: waiting for hub"), dontSendNotification);
            }
            if (relayedViaHub && processor.getRemotePeerHubSendMuted(i)) {   // The hub relays their mute
                sendtext = TRANS("Other end muted us");
                pvf->sendActualBitrateLabel->setColour(Label::textColourId, mutedTextColor);
            } else {
                sendtext = "";
                pvf->sendActualBitrateLabel->setColour(Label::textColourId, regularTextColor);
            }
        }
        else if (blocked) {
            sendtext += TRANS("Other end BLOCKED us");
            pvf->sendActualBitrateLabel->setColour(Label::textColourId, mutedTextColor);
        }
        else if (sendactive) {
            //sendtext += String::formatted("%Ld sent", processor.getRemotePeerPacketsSent(i) );
            //sendtext << String(juce::CharPointer_UTF8 ("\xe2\x86\x91")); // up arrow
            sendtext << " " << sbplus::rateText(sendrate * 8.0);   // Bits, shared rule
            pvf->sendActualBitrateLabel->setColour(Label::textColourId, regularTextColor);
        }
        else if (sendallow) {
            sendtext += TRANS("Other end muted us");
            pvf->sendActualBitrateLabel->setColour(Label::textColourId, mutedTextColor);
        }
        else {
            sendtext += TRANS("SEND DISABLED");
            pvf->sendActualBitrateLabel->setColour(Label::textColourId, mutedTextColor);
        }
        
        String recvtext;
        SonobusAudioProcessor::AudioCodecFormatInfo recvfinfo;
        processor.getRemotePeerReceiveAudioCodecFormat(i, recvfinfo);

        // Peers relayed via the hub have no direct sink stream, so the branch below would report "Other side is
        // muted" for them. Relayed peers use the hub-mix text.
        if (pvf->infoOnly || relayedViaHub) {
            String iouname = processor.getRemotePeerUserName(i);
            bool hubmuted = processor.getHubMixMuteForPeer(iouname);
            if (hubmuted) {
                recvtext = TRANS("Muted in your mix");
                pvf->recvActualBitrateLabel->setColour(Label::textColourId, mutedTextColor);
            } else if (relayedViaHub && processor.getRemotePeerHubSendMuted(i)) {
                // The send-stat label is hidden on relayed rows in the full layout, so the other player's self-mute
                // (top MUTE or the send icon, relayed by the hub) is shown here, on the always-visible Receive-rate
                // label. Wording "Muted at their end" mirrors the stock "Muted in your mix"; it is shorter than the
                // normal rate text, so it does not truncate.
                recvtext = TRANS("Muted at their end");
                pvf->recvActualBitrateLabel->setColour(Label::textColourId, mutedTextColor);
            } else {
                // "drops N", coloured by the last minute's rate (green 0, yellow 1-5, red >5), since the lifetime
                // count alone says nothing about the present. Per-player number = what the hub lost on that player's
                // uplink (same for every viewer); the receiver-side count is identical on every row under bundling
                // and lives in the header.
                const int64_t up = processor.getRemotePeerHubUplinkLost(i);
                const int64_t dropped = up >= 0 ? up : processor.getRemotePeerPacketsDropped(i);
                // sliding 60-s window (see PeerViewInfo::dropsSample*)
                if (pvf->dropsSampleN == 0 || nowstampms - pvf->dropsSampleT[pvf->dropsSampleN - 1] >= 5000) {
                    if (pvf->dropsSampleN == 12) { for (int k = 1; k < 12; ++k) { pvf->dropsSampleT[k-1] = pvf->dropsSampleT[k]; pvf->dropsSampleC[k-1] = pvf->dropsSampleC[k]; } pvf->dropsSampleN = 11; }
                    pvf->dropsSampleT[pvf->dropsSampleN] = nowstampms; pvf->dropsSampleC[pvf->dropsSampleN] = dropped; pvf->dropsSampleN++;
                }
                int64_t recent = 0;
                for (int k = 0; k < pvf->dropsSampleN; ++k) {
                    if (nowstampms - pvf->dropsSampleT[k] <= 60000) { recent = dropped - pvf->dropsSampleC[k]; break; }
                }
                // What this player's stream costs you right now, then the drops
                const double mbps = recvrate * 8.0 * 1e-6;
                float prtt = -1.0f, pup = -1.0f; processor.getRemotePeerHubStats(i, prtt, pup);   // Their uplink rate + ping
                // Bits, one rule with the hub (SbRateFormat.h): "328 kb/s" / "2.6 Mb/s"
                const String ratesText = (pup >= 0.0f ? String::fromUTF8("\xe2\x86\x91 ") + sbplus::rateText(pup * 1.0e3) + "  " : String())
                                       + String::fromUTF8("\xe2\x86\x93 ") + sbplus::rateText(mbps * 1.0e6);
                const String dropsText = String::fromUTF8("  \xc2\xb7  ") + TRANS("drops") + " " + String(dropped);
                recvtext = ratesText + dropsText;
                {   // Hover explains the rate + drops fields
                    const String tip = String::fromUTF8("\xe2\x86\x91 Uploading to hub\n\xe2\x86\x93 Downloading from hub\ndrops = lost on the way to you");   // One per line
                    pvf->recvActualBitrateLabel->setInterceptsMouseClicks(true, false);
                    if (pvf->recvActualBitrateLabel->getTooltip() != tip) pvf->recvActualBitrateLabel->setTooltip(tip);
                }
                const Colour dropcol = recent < 5 ? latGoodColor : recent <= 20 ? latWarnColor : latBadColor;   // per-minute rate — <5 green, 5-20 yellow, >20 red
                pvf->recvActualBitrateLabel->setColour(Label::textColourId, regularTextColor);   // Throughput stays regular
                if (auto* rl = dynamic_cast<SbRichLabel*>(pvf->recvActualBitrateLabel.get())) {
                    AttributedString as; as.setJustification(Justification::centredLeft); as.setWordWrap(AttributedString::none);
                    const Font f = pvf->recvActualBitrateLabel->getFont();
                    as.append(ratesText, f, regularTextColor);
                    as.append(dropsText, f, dropcol);
                    rl->setRich(as);
                }
            }
        }
        else if (recvactive) {
            //recvtext << String(juce::CharPointer_UTF8 ("\xe2\x86\x93 ")) // down arrow
            if (processor.getMainBusNumOutputChannels() <= 0)
                recvtext << TRANS("no outputs selected") << " ";   // "0ch / 1-0" meant THIS player has no active output channels
            else
                recvtext << chcnt << "ch ";
            recvtext << recvfinfo.name
            << " | " << sbplus::rateText(recvrate * 8.0);   // Bits, shared rule

            int64_t dropped = processor.getRemotePeerPacketsDropped(i);
            if (dropped > 0) {
                recvtext += String::formatted(" | %d drop", dropped);
            }

            if (dropped > pvf->lastDropped) {
                
                pvf->lastDroppedChangedTimestampMs = nowstampms;
            }

            int64_t resent = processor.getRemotePeerPacketsResent(i);
            if (resent > 0) {
                recvtext += String::formatted(" | %d resent", resent);
            }

            if (nowstampms < pvf->lastDroppedChangedTimestampMs + 1500) {
                pvf->recvActualBitrateLabel->setColour(Label::textColourId, droppedTextColor);
            }
            else {
                pvf->recvActualBitrateLabel->setColour(Label::textColourId, regularTextColor);
            }

            pvf->lastDropped = dropped;
        }
        else if (recvallow) {
            recvtext += TRANS("Other side is muted");    
            pvf->recvActualBitrateLabel->setColour(Label::textColourId, mutedTextColor);
        }
        else {
            recvtext += TRANS("You muted them");    
            pvf->recvActualBitrateLabel->setColour(Label::textColourId, mutedTextColor);
        }


        pvf->changeAllFormatButton->setToggleState(processor.getChangingDefaultAudioCodecSetsExisting(), dontSendNotification);

        pvf->changeAllRecvFormatButton->setToggleState(processor.getChangingDefaultRecvAudioCodecSetsExisting(), dontSendNotification);

        pvf->sendActualBitrateLabel->setText(sendtext, dontSendNotification);
        if (!(pvf->infoOnly || relayedViaHub) || processor.getHubMixMuteForPeer(processor.getRemotePeerUserName(i))
            || (relayedViaHub && processor.getRemotePeerHubSendMuted(i)))   // Plain text while "Muted at their end" shows
            if (auto* rl = dynamic_cast<SbRichLabel*>(pvf->recvActualBitrateLabel.get())) rl->clearRich();   
        pvf->recvActualBitrateLabel->setText(recvtext, dontSendNotification);

        // Hosted mode: per-row dropped-packet readout. Lazy UI-thread read of the drop counter the engine already
        // maintains, on this periodic pass; no new timers, no audio-thread involvement. relayedViaHub is the gate:
        // hosted mode, real player row (not the hub bookkeeping peer, which has no audio feed, and not an info-only
        // row).
        if (pvf->hostedDropsLabel != nullptr && relayedViaHub) {
            pvf->hostedDropsLabel->setText(TRANS("drops:") + " " + String(processor.getRemotePeerPacketsDropped(i)), dontSendNotification);
        }

        SonobusAudioProcessor::LatencyInfo latinfo;
        processor.getRemotePeerLatencyInfo(i, latinfo);
        
        //pvf->pingLabel->setText(String::formatted("%d ms", (int)latinfo.pingMs ), dontSendNotification);
        pvf->pingLabel->setText(String::formatted("%d", (int)lrintf(latinfo.pingMs) ), dontSendNotification);
        
        if (relayedViaHub) {
            // The row shows end-to-end figures, player -> hub -> player, buffers included:
            //   up   = my audio reaching them  = my leg to the hub (network + the hub's hold on my stream) + the hub's leg to them (network + THEIR receive buffer)
            //   down = their audio reaching me = their leg to the hub (network + hub hold on their stream) + the hub's leg to me (network + MY receive buffer)
            //   ping = up + down (the whole round trip through the hub)
            // Both legs come from the hub's 10-s /sb/hxlat line (ours = the self entry). The hub's own per-leg view stays in the
            // hover tooltip so a problem can be placed on one side.
            float toHub = -1.0f, fromHub = -1.0f, sTo = -1.0f, sFrom = -1.0f, sRtt = -1.0f; int64_t sLost = -1;
            const bool havePeer = processor.getRemotePeerHubLatency(i, toHub, fromHub);
            const bool haveSelf = processor.getSelfHubLatency(sTo, sFrom, sRtt, sLost);
            const String peerName = processor.getRemotePeerUserName(i);
            String tip;
            if (havePeer && haveSelf) {
                const float e2eUp = sTo + fromHub, e2eDn = toHub + sFrom, e2ePing = e2eUp + e2eDn;
                auto latcol  = [this](float ms){ return ms < sbplus::kLatE2EGreenMs ? latGoodColor : ms < sbplus::kLatE2EYellowMs ? latWarnColor : latBadColor; };
                auto pingcol = [this](float ms){ return ms < sbplus::kLatE2ERttGreenMs ? latGoodColor : ms < sbplus::kLatE2ERttYellowMs ? latWarnColor : latBadColor; };
                // Whole milliseconds in the row (tenths stay in the hover tooltip below)
                pvf->latencyUpLabel->setText(String((int) std::lround(e2eUp)), dontSendNotification);
                pvf->latencyDownLabel->setText(String((int) std::lround(e2eDn)), dontSendNotification);
                pvf->latencyUpLabel->setColour(Label::textColourId, latcol(e2eUp));
                pvf->latencyDownLabel->setColour(Label::textColourId, latcol(e2eDn));
                pvf->pingLabel->setText(String((int) std::lround(e2ePing)), dontSendNotification);   // Whole ms
                pvf->pingLabel->setColour(Label::textColourId, pingcol(e2ePing));
                float prtt = -1.0f, pup = -1.0f; processor.getRemotePeerHubStats(i, prtt, pup);
                // "Latency to <name> · Latency from <name> · Round-trip ping"
                tip << "Latency to " << peerName << " " << String(e2eUp, 1) << "\nLatency from " << peerName << " " << String(e2eDn, 1)
                    << "\nRound-trip ping " << String(e2ePing, 1) << " ms";   // One data point per line
            } else {
                pvf->latencyUpLabel->setText("-", dontSendNotification);
                pvf->latencyDownLabel->setText("-", dontSendNotification);
                pvf->pingLabel->setText("-", dontSendNotification);
                pvf->latencyUpLabel->setColour(Label::textColourId, dimTextColor);
                pvf->latencyDownLabel->setColour(Label::textColourId, dimTextColor);
                pvf->pingLabel->setColour(Label::textColourId, dimTextColor);
                tip = TRANS("Waiting for the hub's latency report");
            }
            for (Label* l : { pvf->latencyUpLabel.get(), pvf->latencyDownLabel.get(), pvf->pingLabel.get(), pvf->staticLatencyLabel.get(), pvf->staticPingLabel.get() }) {
                if (l == nullptr) continue;
                if (l->getTooltip() != tip) { l->setInterceptsMouseClicks(true, false); l->setTooltip(tip); }   // Make sure the label takes the hover (relayed rows have no latency button over them)
            }
        }
        else if (latinfo.legacy && !latinfo.isreal) {
            if (pvf->stopLatencyTestTimestampMs > 0) {
                pvf->latencyUpLabel->setText("***", dontSendNotification);
            } else {
                pvf->latencyUpLabel->setText(TRANS("PRESS"), dontSendNotification);
            }
        } else {
            String latlab; // = juce::CharPointer_UTF8 ("\xe2\x86\x91"); // up arrow
            latlab << (int)lrintf(latinfo.outgoingMs);
            //latlab  << "   |   ";
            //latlab << String(juce::CharPointer_UTF8 ("\xe2\x86\x93")); // down arrow
            String downlatlab;
            downlatlab << (int)lrintf(latinfo.incomingMs) ;

            pvf->latencyUpLabel->setText(latlab, dontSendNotification);
            pvf->latencyDownLabel->setText(downlatlab, dontSendNotification);
        }

        pvf->latActiveButton->setToggleState(latactive, dontSendNotification);


        bool initCompleted = false;
        int autobufmode = (int)processor.getRemotePeerAutoresizeBufferMode(i, initCompleted);
        float buftimeMs = processor.getRemotePeerBufferTime(i);
        
        pvf->autosizeButton->setSelectedId(autobufmode, dontSendNotification);
        String buflab = (autobufmode == SonobusAudioProcessor::AutoNetBufferModeOff ? "" :
                         autobufmode == SonobusAudioProcessor::AutoNetBufferModeAutoIncreaseOnly ? " (Auto+)" :
                         autobufmode == SonobusAudioProcessor::AutoNetBufferModeInitAuto ? ( initCompleted ? " (IA-Man)" : " (IA-Auto)"  ) :
                         autobufmode == SonobusAudioProcessor::AutoNetBufferModeMeasureCommitPin ? " (Pin)" :
                         " (Auto)");
        pvf->bufferLabel->setText(sbFormatMs(buftimeMs) + buflab, dontSendNotification);

        // Hosted mode hides the per-row buffer widgets: rebuildPeerViews + updateLayout gate visibility/layout, and
        // PeerViewInfo::resized's re-assertion is gated by pvf->hideBufferWidgets, so no enable-state juggling is
        // needed here. Plain upstream enable logic for P2P mode:
        pvf->bufferMinButton->setEnabled(autobufmode != SonobusAudioProcessor::AutoNetBufferModeOff);
        pvf->bufferMinFrontButton->setEnabled(autobufmode != SonobusAudioProcessor::AutoNetBufferModeOff);
        pvf->bufferTimeSlider->setEnabled(true);
        pvf->autosizeButton->setEnabled(true);


        if (!pvf->bufferTimeSlider->isMouseButtonDown()) {
            pvf->bufferTimeSlider->setValue(buftimeMs, dontSendNotification);
        }

        pvf->remoteSendFormatChoiceButton->setSelectedId(processor.getRequestRemotePeerSendAudioCodecFormat(i), dontSendNotification);
        
        
        int formatindex = processor.getRemotePeerAudioCodecFormat(i);
        pvf->formatChoiceButton->setSelectedItemIndex(formatindex >= 0 ? formatindex : processor.getDefaultAudioCodecFormat(), dontSendNotification);
        String sendqual;
        if (relayedViaHub) {
            // "↓ 2ch PCM 16 bit · 1826 kb/s" — their feed as we receive it
            sendqual << String(CharPointer_UTF8("\xe2\x86\x93")) << " " << chcnt << "ch " << recvfinfo.name
                     << "  " << sbplus::rateText(recvrate * 8.0);   // Bits, shared rule
        }
        else if (processor.isHubPeer(processor.getRemotePeerUserName(i))) {
            // The one upload, stated once: actual send channel count + the enforced format name (PCM 16 bit).
            sendqual << TRANS("send") << " " << processor.getRemotePeerActualSendChannelCount(i) << "ch "
                     << processor.getAudioCodeFormatName(formatindex)
                     << " " << String::fromUTF8("\xe2\x86\x92") << " " << TRANS("hub"); // "→ hub"
        }
        else {
            sendqual << processor.getRemotePeerActualSendChannelCount(i) << "ch " << processor.getAudioCodeFormatName(formatindex);
        }
        pvf->sendQualityLabel->setText(sendqual, dontSendNotification);
        
        if (relayedViaHub) {
            // Rows are re-ordered in place when peers join or leave, so a tall meter can be left bound to the old
            // row's source and show no level. Rebind on change.
            auto * src = processor.getRemotePeerRecvMeterSource(i);
            if (src != nullptr && src != pvf->recvMeterSrc) {
                pvf->recvMeter->bindSource (src);   // The tip meter keeps its own source pointer
                pvf->recvMeterSrc = src;
            }
            // Rows are re-ordered in place, so a row can keep the meter width of the player that sat there before.
            // Re-lay when the row's channel count no longer matches its layout.
            if (pvf->laidOutMeterChans != processor.getRemotePeerRecvChannelCount(i)) needsUpdateLayout = true;
        }

        // The hub row is hidden per row index in updateLayout, so a re-order with the same count can leave the wrong
        // row hidden. Re-lay when a row's hidden state is stale.
        if (processor.isHostedMixModeEnabled() && pvf->isVisible() == processor.isHubPeer(processor.getRemotePeerUserName(i)))
            needsUpdateLayout = true;

        if (chcnt != pvf->channelGroups->getGroupViewsCount()) {
            pvf->channelGroups->rebuildChannelViews();
            needsUpdateLayout = true;
        } else {
            pvf->channelGroups->updateChannelViews();
        }


        const float disalpha = 0.4;
        pvf->addrLabel->setAlpha(connected ? 1.0 : 0.8);
        pvf->statusLabel->setAlpha(connected ? 1.0 : disalpha);


        if (latinfo.legacy) {
            if (pvf->stopLatencyTestTimestampMs > 0.0 && nowstampms > pvf->stopLatencyTestTimestampMs
                && !pvf->latActiveButton->isMouseButtonDown()) {

                // only stop if it has actually gotten a real latency
                if (latinfo.isreal) {
                    stopLatencyTest(i);

                    String messagestr = generateLatencyMessage(latinfo);
                    showPopTip(messagestr, 5000, pvf->latActiveButton.get(), 300);

                }
            }
        }
    }
    
    int i=0;
    for (auto & pinfo : mPendingUsers) {
        if (i>= mPendingPeerViews.size()) {
            break; // shouldn't happen
        }
        
        PendingPeerViewInfo * ppvf = mPendingPeerViews.getUnchecked(i);
        ppvf->nameLabel->setText(pinfo.second.user, dontSendNotification);
        
        if (pinfo.second.failed) {
            if (pinfo.second.blocked) {
                ppvf->messageLabel->setText(TRANS("User BLOCKED from address: ") + pinfo.second.address, dontSendNotification);
                ppvf->removeButton->setVisible(true);
                ppvf->unblockButton->setVisible(true);
            }
            else {

                ppvf->messageLabel->setText(TRANS("Could not connect with user, one or both of you may need to configure your internal firewall or network router to allow SonoBus to work between you. See the help documentation to enable port forwarding on your router."), dontSendNotification);
                ppvf->removeButton->setVisible(true);
            }
        }
        else {
            ppvf->messageLabel->setText(TRANS("Connecting..."), dontSendNotification);
            ppvf->removeButton->setVisible(false);
            ppvf->unblockButton->setVisible(false);
        }
        ++i;
    }
    
    if (needsUpdateLayout) {
        updateLayout();
        // updateLayout only rebuilds each row's FlexBox. A row whose bounds did not change gets no resized(), so its
        // children would keep the old widths while the check below reads "up to date".
        for (auto * rowview : mPeerViews) rowview->resized();
        listeners.call (&PeersContainerView::Listener::internalSizesChanged, this);
    }
    
    lastUpdateTimestampMs = nowstampms;
}

void PeersContainerView::startLatencyTest(int di)
{
    if (di >= mPeerViews.size()) return;
    
    PeerViewInfo * pvf = mPeerViews.getUnchecked(di);

    int i = mPeerUpdateOrdering[di];

    pvf->stopLatencyTestTimestampMs = Time::getMillisecondCounter(); // make it stop after the first one  //+ 1500;
    pvf->wasRecvActiveAtLatencyTest = processor.getRemotePeerRecvActive(i);
    pvf->wasSendActiveAtLatencyTest = processor.getRemotePeerSendActive(i);
    
    pvf->latencyUpLabel->setText("***", dontSendNotification);
    pvf->latencyDownLabel->setText("***", dontSendNotification);

    processor.startRemotePeerLatencyTest(i);             
}

void PeersContainerView::stopLatencyTest(int di)
{
    if (di >= mPeerViews.size()) return;
    PeerViewInfo * pvf = mPeerViews.getUnchecked(di);

    int i = mPeerUpdateOrdering[di];

    processor.stopRemotePeerLatencyTest(i);
    
    pvf->stopLatencyTestTimestampMs = 0;
    
    SonobusAudioProcessor::LatencyInfo latinfo;
    processor.getRemotePeerLatencyInfo(i, latinfo);
        
    if (latinfo.legacy && !latinfo.isreal) {
        pvf->latencyUpLabel->setText(TRANS("PRESS"), dontSendNotification);
        pvf->latencyDownLabel->setText("", dontSendNotification);
    } else {
        //pvf->latencyLabel->setText(String::formatted("%d ms", (int)lrintf(latinfo.totalRoundtripMs)) + (latinfo.estimated ? "*" : "") , dontSendNotification);
        updatePeerViews(i);
    }
}

String PeersContainerView::generateLatencyMessage(const SonobusAudioProcessor::LatencyInfo &latinfo)
{
    String messagestr = TRANS("Estimated Round-trip Latency:") + String::formatted(" %d ms", (int) lrintf(latinfo.totalRoundtripMs));
    messagestr += "\n" + TRANS("Round-trip Network Ping:") + String::formatted(" %.1f ms", (latinfo.pingMs));
    messagestr += "\n" + TRANS("Est. Outgoing:") + String::formatted(" %.1f ms", (latinfo.outgoingMs));
    messagestr += "\n" + TRANS("Est. Incoming:") + String::formatted(" %.1f ms", (latinfo.incomingMs));
    //messagestr += "\n" + TRANS("Est. Jitter:") + String::formatted(" %.1f ms", (latinfo.jitterMs));
    if (latinfo.legacy) {
        messagestr += "\n-------------";
        messagestr += "\n" + TRANS("Legacy-mode, ask them \n to install latest version");
    }
    return messagestr;
}


void PeersContainerView::timerCallback(int timerId)
{
    if (timerId == FillRatioUpdateTimerId) {
        // Hosted mode hides every jitterBufferMeter: skip the locked getter entirely; otherwise only poll for visible
        // widgets.
        if (processor.isHostedMixModeEnabled()) {
            return;
        }
        for (int di=0; di < mPeerViews.size(); ++di) {
            PeerViewInfo * pvf = mPeerViews.getUnchecked(di);
            int i = mPeerUpdateOrdering[di];

            if (!pvf->jitterBufferMeter || !pvf->jitterBufferMeter->isVisible()) {
                continue;
            }

            float ratio, stdev;
            if (processor.getRemotePeerReceiveBufferFillRatio(i, ratio, stdev)) {
                pvf->jitterBufferMeter->setFillRatio(ratio, stdev);
            }
        }
    }
}

void PeersContainerView::setMetersSuspended(bool flag)
{
    metersSuspended = flag;
    for (int i=0; i < mPeerViews.size(); ++i) {
        PeerViewInfo * pvf = mPeerViews.getUnchecked(i);
        if (pvf->channelGroups) {
            pvf->channelGroups->setMetersSuspended(flag);
        }
    }
}


void PeersContainerView::choiceButtonSelected(SonoChoiceButton *comp, int index, int ident)
{
    // (removed hub-stream autosize handler —
    //  control deleted, see header comment.)

    // Unified receive-buffer auto mode — one flag for all
    // player sinks in hosted mode.
    if (mUnifiedAutosizeButton != nullptr && mUnifiedAutosizeButton.get() == comp) {
        processor.setUnifiedReceiveAutoresizeBufferMode((SonobusAudioProcessor::AutoNetBufferMode) ident);
        updatePeerViews();
        return;
    }

    for (int di=0; di < mPeerViews.size(); ++di) {
        PeerViewInfo * pvf = mPeerViews.getUnchecked(di);
        int i = mPeerUpdateOrdering[di];
        if (pvf->formatChoiceButton.get() == comp) {
            // set them all if this option is selected
            if (processor.getChangingDefaultAudioCodecSetsExisting()) {
                for (int j=0; j < mPeerViews.size(); ++j) {
                    processor.setRemotePeerAudioCodecFormat(j, ident);                    
                }
            }
            else {
                processor.setRemotePeerAudioCodecFormat(i, ident);
            }
            break;
        }        
        else if (pvf->remoteSendFormatChoiceButton.get() == comp) {
            // set them all if this option is selected

            if (processor.getChangingDefaultRecvAudioCodecSetsExisting()) {
                for (int j=0; j < mPeerViews.size(); ++j) {
                    processor.setRequestRemotePeerSendAudioCodecFormat(j, ident);
                }
            }
            else
            {
                processor.setRequestRemotePeerSendAudioCodecFormat(i, ident); 
            }
            break;
        }        
        else if (pvf->autosizeButton.get() == comp) {
            processor.setRemotePeerAutoresizeBufferMode(i, (SonobusAudioProcessor::AutoNetBufferMode) ident);
            break;
        }        
    }
}

void PeersContainerView::buttonClicked (Button* buttonThatWasClicked)
{
    // Hosted hub-header buffer chip: not a per-peer control, so handle it before the per-peer loop. Toggles the
    // unified receive-buffer popup (same toggle idiom as recvOptionsButton below).
    // Reset the aggregate hub drop count: clears every peer's packet stats (same call as per-peer Reset Dropped), so
    // the header total and all per-row counts restart from zero together.
    if (mUnifiedResetDropsButton != nullptr && mUnifiedResetDropsButton.get() == buttonThatWasClicked) {
        for (int i = 0; i < processor.getNumberRemotePeers(); ++i) {
            processor.resetRemotePeerPacketStats(i);
        }
        updatePeerViews();
        return;
    }

    if (mUnifiedBufChipButton != nullptr && mUnifiedBufChipButton.get() == buttonThatWasClicked) {
        if (!unifiedBufCalloutBox) {
            showUnifiedBufferOptions(true, mUnifiedBufChipButton.get());
        } else {
            showUnifiedBufferOptions(false);
        }
        return;
    }

    for (int di=0; di < mPeerViews.size(); ++di) {
        PeerViewInfo * pvf = mPeerViews.getUnchecked(di);

        int i = mPeerUpdateOrdering[di];

        bool connected = processor.getRemotePeerConnected(i);
        bool isGroupPeer = processor.getRemotePeerUserName(i).isNotEmpty();

        if (pvf->sendMutedButton.get() == buttonThatWasClicked) {
            if (!connected && !isGroupPeer) {
                String hostname;
                int port = 0;
                processor.getRemotePeerAddressInfo(i, hostname, port);
                processor.connectRemotePeer(hostname, port);
            } else {
                // turns on allow and starts sending
                if (!pvf->sendMutedButton->getToggleState()) {
                    processor.setRemotePeerSendActive(i, true);
                } else {
                    // turns off sending and allow
                    processor.setRemotePeerSendAllow(i, false);
                }
            }
            return;
        }
        else if (pvf->recvMutedButton.get() == buttonThatWasClicked) {
            if (pvf->infoOnly) {
                // Info-only rows route mute through the hub's mix control channel. JUCE has already flipped the
                // button's toggle state before this callback runs.
                String name = processor.getRemotePeerUserName(i);
                processor.setHubMixMuteForPeer(name, pvf->recvMutedButton->getToggleState());
            }
            else if (!connected && !isGroupPeer) {
                String hostname;
                int port = 0;
                processor.getRemotePeerAddressInfo(i, hostname, port);
                processor.connectRemotePeer(hostname, port);
            } else {
                if (!pvf->recvMutedButton->getToggleState()) {
                    // allows receiving and invites
                    processor.setRemotePeerRecvActive(i, true);
                } else {
                    // turns off receiving and allow
                    processor.setRemotePeerRecvAllow(i, false);
                }
            }
            return;
        }
        else if (pvf->recvSoloButton.get() == buttonThatWasClicked) {
            if (ModifierKeys::currentModifiers.isAltDown()) {
                // exclusive solo this one
                for (int dj=0; dj < mPeerViews.size(); ++dj) {
                    int j = mPeerUpdateOrdering[dj];

                    if (buttonThatWasClicked->getToggleState()) {
                        processor.setRemotePeerSoloed(j, i == j);                         
                    }
                    else {
                        processor.setRemotePeerSoloed(j, false); 
                    }
                }
                    
                // disable solo for main monitor too
                processor.getValueTreeState().getParameter(SonobusAudioProcessor::paramMainMonitorSolo)->setValueNotifyingHost(0.0);
                
                updatePeerViews();
            } else {
                processor.setRemotePeerSoloed(i, buttonThatWasClicked->getToggleState()); 
                updatePeerViews();
            }
            return;
        }

        else if (pvf->latActiveButton.get() == buttonThatWasClicked) {
            SonobusAudioProcessor::LatencyInfo latinfo;
            processor.getRemotePeerLatencyInfo(i, latinfo);

            if (latinfo.legacy) {
                pvf->latActiveButton->setToggleState(!pvf->latActiveButton->getToggleState(), dontSendNotification);
                if (pvf->latActiveButton->getToggleState()) {
                    startLatencyTest(di);
                    //showPopTip(TRANS("Measuring actual round-trip latency"), 4000, pvf->latActiveButton.get(), 140);
                } else {
                    stopLatencyTest(di);
                }
            }
            else {
                String messagestr = generateLatencyMessage(latinfo);

                showPopTip(messagestr, 8000, pvf->latActiveButton.get(), 300);
                pvf->latActiveButton->setToggleState(false, dontSendNotification);



            }
            return;
        }

        
        else if (pvf->recvOptionsButton.get() == buttonThatWasClicked) {

            if (!recvOptionsCalloutBox) {
                showRecvOptions(di, true, pvf->recvOptionsButton.get());
            } else {
                showRecvOptions(di, false);
            }

            return;
        }
        else if (pvf->sendOptionsButton.get() == buttonThatWasClicked) {
            if (!sendOptionsCalloutBox) {
                showSendOptions(di, true, pvf->sendOptionsButton.get());
            } else {
                showSendOptions(di, false);
            }
            return;
        }
        else if (pvf->optionsResetDropButton.get() == buttonThatWasClicked) {
            processor.resetRemotePeerPacketStats(i);
            return;
        }
        else if (pvf->changeAllFormatButton.get() == buttonThatWasClicked) {
            processor.setChangingDefaultAudioCodecSetsExisting(buttonThatWasClicked->getToggleState());
            return;
        }
        else if (pvf->changeAllRecvFormatButton.get() == buttonThatWasClicked) {
            processor.setChangingDefaultRecvAudioCodecSetsExisting(buttonThatWasClicked->getToggleState());
            return;
        }
        else if (pvf->optionsRemoveButton.get() == buttonThatWasClicked) {
            processor.removeRemotePeer(i);
            showSendOptions(di, false);
            return;
        }
        else if (pvf->optionsBlockButton.get() == buttonThatWasClicked) {
            // TODO: confirm blocking?
            String remhost;
            int remport;
            processor.getRemotePeerAddressInfo(i, remhost, remport);
            processor.addBlockedAddress(remhost);

            processor.removeRemotePeer(i, true);
            showSendOptions(di, false);
            return;
        }
        else if (pvf->bufferMinButton.get() == buttonThatWasClicked || pvf->bufferMinFrontButton.get() == buttonThatWasClicked) {
            // force to minimum
            if (ModifierKeys::currentModifiers.isAltDown()) {
                // do it for everyone (who's on auto, maybe?)
                bool initCompleted = false;
                for (int dj=0; dj < mPeerViews.size(); ++dj) {
                    int j = mPeerUpdateOrdering[dj];

                    if (processor.getRemotePeerAutoresizeBufferMode(j, initCompleted) != SonobusAudioProcessor::AutoNetBufferModeOff) {
                        float buftime = 0.0;
                        processor.setRemotePeerBufferTime(j, buftime);
                        if (i==j) {
                            pvf->bufferTimeSlider->setValue(buftime, dontSendNotification);
                        }                            
                    }
                }
            } else {
                float buftime = 0.0;
                processor.setRemotePeerBufferTime(i, buftime);
                pvf->bufferTimeSlider->setValue(buftime, dontSendNotification);
            }
            
            updatePeerViews();
            return;
        }
    }

    int i = 0;
    for (auto & pinfo : mPendingUsers) {
        if (i>= mPendingPeerViews.size()) {
            break; // shouldn't happen
        }

        PendingPeerViewInfo * ppvf = mPendingPeerViews.getUnchecked(i);
        if (ppvf->removeButton.get() == buttonThatWasClicked) {
            mPendingUsers.erase(pinfo.first);
            rebuildPeerViews();
            return;
        }
        else if (ppvf->unblockButton.get() == buttonThatWasClicked) {
            processor.removeBlockedAddress(pinfo.second.address);

            processor.connectRemotePeer(pinfo.second.address, pinfo.second.port, pinfo.second.user, pinfo.second.group);

            mPendingUsers.erase(pinfo.first);
            rebuildPeerViews();
            return;
        }

        ++i;
    }

}



void PeersContainerView::showRecvOptions(int dindex, bool flag, Component * fromView)
{
    
    if (flag && recvOptionsCalloutBox == nullptr) {
        
        auto wrap = std::make_unique<Viewport>();

        Component* dw = nullptr; // this->findParentComponentOfClass<DocumentWindow>();
        
        if (!dw) {
            dw = this->findParentComponentOfClass<AudioProcessorEditor>();
        }
        if (!dw) {
            dw = this->findParentComponentOfClass<Component>();
        }
        if (!dw) {
            dw = this;
        }
        
        const int defWidth = 300;
#if JUCE_IOS || JUCE_ANDROID
        int defHeight = 180;
        const int netbufRowHeight = 50;  // minitemheight 44 + margins/spacer
#else
        int defHeight = 152;
        const int netbufRowHeight = 42;  // minitemheight 36 + margins/spacer
#endif
        // Hosted mode: updateLayout drops the popup's buffer row (the unified header owns the one buffer knob), so
        // shrink the popup by that row's height.
        if (processor.isHostedMixModeEnabled()) {
            defHeight -= netbufRowHeight;
        }
        // Relayed rows also drop the "Preferred Recv Quality" row (see updateLayout): shrink by that row + its 8px
        // spacer too.
        if (mPeerViews.getUnchecked(dindex)->hideSendCluster) {
#if JUCE_IOS || JUCE_ANDROID
            defHeight -= 56;  // minitemheight 44 + margins 4 + spacer 8
#else
            defHeight -= 48;  // minitemheight 36 + margins 4 + spacer 8
#endif
        }

        wrap->setSize(jmin(defWidth, dw->getWidth() - 20), jmin(defHeight, dw->getHeight() - 24));
        
        auto * pvf = mPeerViews.getUnchecked(dindex);

        pvf->addrClicked = false;
        
        pvf->recvOptionsContainer->setBounds(Rectangle<int>(0,0,defWidth,defHeight));
        
        wrap->setViewedComponent(pvf->recvOptionsContainer.get(), false);
        pvf->recvOptionsContainer->setVisible(true);
        
        pvf->recvOptionsBox.performLayout(pvf->recvOptionsContainer->getLocalBounds());
        pvf->bufferTimeLabel->setBounds(pvf->bufferTimeSlider->getBounds().removeFromLeft(pvf->bufferTimeSlider->getWidth()*0.5));

        updatePeerViews(dindex);
        
        Rectangle<int> bounds =  dw->getLocalArea(nullptr, fromView ? fromView->getScreenBounds() : pvf->recvOptionsButton->getScreenBounds());
        DBG("callout bounds: " << bounds.toString());
        recvOptionsCalloutBox = & CallOutBox::launchAsynchronously (std::move(wrap), bounds , dw, false);
        if (CallOutBox * box = dynamic_cast<CallOutBox*>(recvOptionsCalloutBox.get())) {
            box->setDismissalMouseClicksAreAlwaysConsumed(true);
            box->grabKeyboardFocus();
        }
        pvf->recvOptionsContainer->grabKeyboardFocus();
    }
    else {
        // dismiss it
        if (CallOutBox * box = dynamic_cast<CallOutBox*>(recvOptionsCalloutBox.get())) {
            box->dismiss();
            recvOptionsCalloutBox = nullptr;
        }
    }
}

// Popup for the unified receive-buffer controls, launched from the compact value chip on the hub header row. Follows
// the showRecvOptions CallOutBox pattern above. The slider + auto-mode combo inside drive setUnifiedReceiveBufferTime
// / setUnifiedReceiveAutoresizeBufferMode, and updatePeerViews keeps the title/value labels live while the popup is
// open.
void PeersContainerView::showUnifiedBufferOptions(bool flag, Component * fromView)
{
    if (flag && unifiedBufCalloutBox == nullptr) {

        auto wrap = std::make_unique<Viewport>();

        Component* dw = this->findParentComponentOfClass<AudioProcessorEditor>();
        if (!dw) {
            dw = this->findParentComponentOfClass<Component>();
        }
        if (!dw) {
            dw = this;
        }

        const int defWidth = 300;
#if JUCE_IOS || JUCE_ANDROID
        const int itemheight = 44;
#else
        const int itemheight = 36;
#endif
        // title row (14 + margins) + control row (itemheight + margins)
        // + reset row (itemheight + margins) + spacers
        const int defHeight = 6 + 14 + 4 + 4 + itemheight + 4 + 4 + itemheight + 4 + 8;

        wrap->setSize(jmin(defWidth, dw->getWidth() - 20), jmin(defHeight, dw->getHeight() - 24));

        mUnifiedOptionsContainer->setBounds(Rectangle<int>(0, 0, defWidth, defHeight));

        wrap->setViewedComponent(mUnifiedOptionsContainer.get(), false);
        mUnifiedOptionsContainer->setVisible(true);

        unifiedOptionsTitleBox.items.clear();
        unifiedOptionsTitleBox.flexDirection = FlexBox::Direction::row;
        unifiedOptionsTitleBox.items.add(FlexItem(150, 14, *mUnifiedBufStaticLabel).withMargin(0).withFlex(1));
        unifiedOptionsTitleBox.items.add(FlexItem(72, 14, *mUnifiedBufLabel).withMargin(0).withFlex(0));

        unifiedOptionsNetbufBox.items.clear();
        unifiedOptionsNetbufBox.flexDirection = FlexBox::Direction::row;
        unifiedOptionsNetbufBox.items.add(FlexItem(90, itemheight, *mUnifiedBufferTimeSlider).withMargin(0).withFlex(1));
        unifiedOptionsNetbufBox.items.add(FlexItem(4, 12));
        unifiedOptionsNetbufBox.items.add(FlexItem(90, itemheight, *mUnifiedAutosizeButton).withMargin(0).withFlex(0));

        unifiedOptionsBox.items.clear();
        unifiedOptionsBox.flexDirection = FlexBox::Direction::column;
        unifiedOptionsBox.items.add(FlexItem(4, 6));
        unifiedOptionsBox.items.add(FlexItem(100, 14, unifiedOptionsTitleBox).withMargin(2).withFlex(0));
        unifiedOptionsBox.items.add(FlexItem(4, 4));
        unifiedOptionsBox.items.add(FlexItem(100, itemheight, unifiedOptionsNetbufBox).withMargin(2).withFlex(0));
        unifiedOptionsBox.items.add(FlexItem(4, 4));
        unifiedOptionsBox.items.add(FlexItem(140, itemheight, *mUnifiedResetDropsButton).withMargin(2).withFlex(0));
        unifiedOptionsBox.performLayout(mUnifiedOptionsContainer->getLocalBounds());

        // refresh value/mode displays (and slider position) before showing
        updatePeerViews();

        Rectangle<int> bounds = dw->getLocalArea(nullptr, fromView ? fromView->getScreenBounds()
                                                 : mUnifiedBufChipButton->getScreenBounds());
        unifiedBufCalloutBox = & CallOutBox::launchAsynchronously(std::move(wrap), bounds, dw, false);
        if (CallOutBox * box = dynamic_cast<CallOutBox*>(unifiedBufCalloutBox.get())) {
            box->setDismissalMouseClicksAreAlwaysConsumed(true);
            box->grabKeyboardFocus();
        }
        mUnifiedOptionsContainer->grabKeyboardFocus();
    }
    else {
        // dismiss it
        if (CallOutBox * box = dynamic_cast<CallOutBox*>(unifiedBufCalloutBox.get())) {
            box->dismiss();
            unifiedBufCalloutBox = nullptr;
        }
    }
}

void PeersContainerView::showSendOptions(int dindex, bool flag, Component * fromView)
{
    
    if (flag && sendOptionsCalloutBox == nullptr) {
        
        auto wrap = std::make_unique<Viewport>();

        
        Component* dw = nullptr; // this->findParentComponentOfClass<DocumentWindow>();
        
        if (!dw) {
            dw = this->findParentComponentOfClass<AudioProcessorEditor>();
        }
        if (!dw) {
            dw = this->findParentComponentOfClass<Component>();
        }
        if (!dw) {
            dw = this;
        }
        
        const int defWidth = 245;
#if JUCE_IOS || JUCE_ANDROID
        const int defHeight = 144;
#else
        const int defHeight = 116;
#endif
        
        
        wrap->setSize(jmin(defWidth, dw->getWidth() - 20), jmin(defHeight, dw->getHeight() - 24));

        auto * pvf = mPeerViews.getUnchecked(dindex);
        
        pvf->sendOptionsContainer->setBounds(Rectangle<int>(0,0,defWidth,defHeight));
        
        wrap->setViewedComponent(pvf->sendOptionsContainer.get(), false);
        pvf->sendOptionsContainer->setVisible(true);
        
        pvf->sendOptionsBox.performLayout(pvf->sendOptionsContainer->getLocalBounds());

        
        Rectangle<int> bounds =  dw->getLocalArea(nullptr, fromView ? fromView->getScreenBounds() : pvf->sendOptionsButton->getScreenBounds());
        DBG("callout bounds: " << bounds.toString());
        sendOptionsCalloutBox = & CallOutBox::launchAsynchronously (std::move(wrap), bounds , dw, false);
        if (CallOutBox * box = dynamic_cast<CallOutBox*>(sendOptionsCalloutBox.get())) {
            box->setDismissalMouseClicksAreAlwaysConsumed(true);
        }
        pvf->sendOptionsContainer->grabKeyboardFocus();
    }
    else {
        // dismiss it
        if (CallOutBox * box = dynamic_cast<CallOutBox*>(sendOptionsCalloutBox.get())) {
            box->dismiss();
            sendOptionsCalloutBox = nullptr;
        }
    }
}





void PeersContainerView::clearClipIndicators()
{
    for (int di=0;  di < mPeerViews.size(); ++di) {
        int i = mPeerUpdateOrdering[di];

        PeerViewInfo * pvf = mPeerViews.getUnchecked(i);
        pvf->recvMeter->clearClipIndicator(-1);
        pvf->recvMeter->clearMaxLevelDisplay(-1);
        pvf->channelGroups->clearClipIndicators();

    }
}


void PeersContainerView::showPopupMenu(Component * source, int dindex)
{
    if (dindex >= mPeerViews.size()) return;

    int index = mPeerUpdateOrdering[dindex];


    PeerViewInfo * pvf = mPeerViews.getUnchecked(dindex);
    bool isGroupPeer = processor.getRemotePeerUserName(index).isNotEmpty();

    Array<GenericItemChooserItem> items;
    if (isGroupPeer) {
        if (processor.getRemotePeerRecvAllow(index) && processor.getRemotePeerSendAllow(index)) {
            items.add(GenericItemChooserItem(TRANS("Mute All")));
        } else {
            items.add(GenericItemChooserItem(TRANS("Unmute All")));
        }
    } else {
        if (processor.getRemotePeerConnected(index)) {
            items.add(GenericItemChooserItem(TRANS("Mute All")));
        } else {
            items.add(GenericItemChooserItem(TRANS("Unmute All")));        
        }        
    }
    items.add(GenericItemChooserItem(TRANS("Remove")));
    
    Component* dw = nullptr; // source->findParentComponentOfClass<DocumentWindow>();

    if (!dw) {
        dw = source->findParentComponentOfClass<AudioProcessorEditor>();        
    }
    if (!dw) {
        dw = source->findParentComponentOfClass<Component>();        
    }
    
    Rectangle<int> bounds =  dw->getLocalArea(nullptr, source->getScreenBounds());
    
    GenericItemChooser::launchPopupChooser(items, bounds, dw, this, index+1);
}

void PeersContainerView::genericItemChooserSelected(GenericItemChooser *comp, int index)
{
    // popup menu
    int dvindex = comp->getTag() - 1;
    if (dvindex >= mPeerViews.size()) return;
    //PeerViewInfo * pvf = mPeerViews.getUnchecked(vindex);

    int vindex = mPeerUpdateOrdering[dvindex];


    bool isGroupPeer = processor.getRemotePeerUserName(vindex).isNotEmpty();
    
    if (index == 0) {

        if (isGroupPeer) {
            if (!processor.getRemotePeerRecvAllow(vindex) || !processor.getRemotePeerSendAllow(vindex)) {
                // allow everything
                processor.setRemotePeerSendActive(vindex, true); 
                processor.setRemotePeerRecvActive(vindex, true);                 
            }
            else {
                // disallow everything
                processor.setRemotePeerSendAllow(vindex, false); 
                processor.setRemotePeerRecvAllow(vindex, false); 
            }
        }
        else {
            if (processor.getRemotePeerConnected(vindex)) {
                // disconnect
                processor.disconnectRemotePeer(vindex);  
            } else {
                
                String hostname;
                int port = 0;
                processor.getRemotePeerAddressInfo(vindex, hostname, port);
                processor.connectRemotePeer(hostname, port);            
            }
        }
    }
    else if (index == 1) {
        processor.removeRemotePeer(vindex);
    }
    
    
    if (CallOutBox* const cb = comp->findParentComponentOfClass<CallOutBox>()) {
        cb->dismiss();
    }

}

void PeersContainerView::sliderValueChanged (Slider* slider)
{
   // (removed hub-stream buffer-slider handler —
   //  control deleted, see header comment.)

   // Unified receive-buffer slider: one value for all player sinks in hosted mode. Re-set the slider afterwards so
   // the hosted feed-buffer floor clamp is reflected immediately.
   if (mUnifiedBufferTimeSlider != nullptr && mUnifiedBufferTimeSlider.get() == slider) {
       float buftime = mUnifiedBufferTimeSlider->getValue();
       processor.setUnifiedReceiveBufferTime(buftime);
       mUnifiedBufferTimeSlider->setValue(processor.getUnifiedReceiveBufferTime(), dontSendNotification);
       updatePeerViews();
       return;
   }

   for (int di=0; di < mPeerViews.size(); ++di) {
       PeerViewInfo * pvf = mPeerViews.getUnchecked(di);
       int i = mPeerUpdateOrdering[di];

       if (pvf->bufferTimeSlider.get() == slider) {
           float buftime = pvf->bufferTimeSlider->getValue();
           processor.setRemotePeerBufferTime(i, buftime);
           pvf->bufferTimeSlider->setValue(buftime, dontSendNotification);
           break;
       }
   }

}
