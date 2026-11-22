// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2020 Jesse Chappell
// Modifications Copyright (C) 2026 Jason Buecker (Random Planet)

#include "ChannelGroupsView.h"
#include "SbTextFit.h"
#include <cmath>


using namespace SonoAudio;

struct DestChannelListItemData : public GenericItemChooserItem::UserData
{
public:
    DestChannelListItemData(const DestChannelListItemData & other) : startIndex(other.startIndex), count(other.count) {}
    DestChannelListItemData(int start, int cnt) : startIndex(start), count(cnt) {}

    int startIndex;
    int count;
};

struct CmdListItemData : public GenericItemChooserItem::UserData
{
public:
    CmdListItemData(const CmdListItemData & other) : command(other.command) {}
    CmdListItemData(int cmd) : command(cmd) {}

    int command;
};

ChannelGroupEffectsView::ChannelGroupEffectsView(SonobusAudioProcessor& proc, bool peermode)
: Component(), peerMode(peermode), processor(proc)
{
    effectsConcertina =  std::make_unique<ConcertinaPanel>();

    compressorView =  std::make_unique<CompressorView>();
    compressorView->addListener(this);
    compressorView->addHeaderListener(this);

    expanderView =  std::make_unique<ExpanderView>();
    expanderView->addListener(this);
    expanderView->addHeaderListener(this);

    eqView =  std::make_unique<ParametricEqView>();
    eqView->addListener(this);
    eqView->addHeaderListener(this);

    reverbSendView =  std::make_unique<ReverbSendView>(processor, false, true);
    reverbSendView->addListener(this);
    reverbSendView->addHeaderListener(this);

    polarityInvertView =  std::make_unique<PolarityInvertView>(processor, false, true);
    polarityInvertView->addListener(this);
    polarityInvertView->addHeaderListener(this);


    effectsConcertina->addPanel(-1, expanderView.get(), false);
    effectsConcertina->addPanel(-1, compressorView.get(), false);
    effectsConcertina->addPanel(-1, eqView.get(), false);
    effectsConcertina->addPanel(-1, reverbSendView.get(), false);
    effectsConcertina->addPanel(-1, polarityInvertView.get(), false);

    effectsConcertina->setCustomPanelHeader(compressorView.get(), compressorView->getHeaderComponent(), false);
    effectsConcertina->setCustomPanelHeader(expanderView.get(), expanderView->getHeaderComponent(), false);
    effectsConcertina->setCustomPanelHeader(eqView.get(), eqView->getHeaderComponent(), false);

    effectsConcertina->setCustomPanelHeader(reverbSendView.get(), reverbSendView->getHeaderComponent(), false);
    effectsConcertina->setCustomPanelHeader(polarityInvertView.get(), polarityInvertView->getHeaderComponent(), false);

    addAndMakeVisible (effectsConcertina.get());

    setFocusContainerType(FocusContainerType::focusContainer);

    updateLayout();
}

ChannelGroupEffectsView::~ChannelGroupEffectsView() {

    compressorView->removeListener(this);
    expanderView->removeListener(this);
    expanderView->removeHeaderListener(this);
    reverbSendView->removeHeaderListener(this);
    reverbSendView->removeListener(this);
    polarityInvertView->removeHeaderListener(this);
    polarityInvertView->removeListener(this);

    effectsConcertina.reset();
}

juce::Rectangle<int> ChannelGroupEffectsView::getMinimumContentBounds() const {
    auto minbounds = compressorView->getMinimumContentBounds();
    auto minexpbounds = expanderView->getMinimumContentBounds();
    auto mineqbounds = eqView->getMinimumContentBounds();
    auto headbounds = compressorView->getMinimumHeaderBounds();
    auto minrevbounds = reverbSendView->getMinimumContentBounds();
    auto minprvbounds = polarityInvertView->getMinimumContentBounds();

    int defWidth = jmax(minbounds.getWidth(), minexpbounds.getWidth(), mineqbounds.getWidth(), jmax(minrevbounds.getWidth(),  minprvbounds.getWidth())) + 12;
    int defHeight = 0;

    // Size the popup for the two tallest panels open at once (gate + compressor is the common pair) so neither is
    // clipped.
    defHeight = minexpbounds.getHeight() + minbounds.getHeight() + 5*headbounds.getHeight() + 8;

    return Rectangle<int>(0,0,defWidth,defHeight);
}


void ChannelGroupEffectsView::updateState()
{
    if (peerMode) {
        updateStateForRemotePeer();
    } else {
        updateStateForInput();
    }

}

void ChannelGroupEffectsView::updateStateForRemotePeer()
{
    if (expanderView) expanderView->setHardGateAvailable(false);   // Receive side — no hard gate
    CompressorParams compParams;
    if (processor.getRemotePeerCompressorParams(peerIndex, groupIndex, compParams)) {
        compressorView->updateParams(compParams);
    }

    CompressorParams expParams;
    if (processor.getRemotePeerExpanderParams(peerIndex, groupIndex, expParams)) {
        expanderView->updateParams(expParams);
    }

    ParametricEqParams eqparams;
    if (processor.getRemotePeerEqParams(peerIndex, groupIndex, eqparams)) {
        eqView->updateParams(eqparams);
    }

    if (!reverbSendView->isVisible()) {
        reverbSendView->setVisible(true);
        reverbSendView->getHeaderComponent()->setVisible(true);
    }

    reverbSendView->updateParams(processor.getRemotePeerChannelReverbSend(peerIndex, groupIndex));

    if (!polarityInvertView->isVisible()) {
        polarityInvertView->setVisible(true);
        polarityInvertView->getHeaderComponent()->setVisible(true);
    }

    polarityInvertView->updateParams(processor.getRemotePeerPolarityInvert(peerIndex, groupIndex));


    if (firstShow) {
        if (eqparams.enabled && !(compParams.enabled || expParams.enabled)) {
            effectsConcertina->expandPanelFully(eqView.get(), false);
        }
        else {
            effectsConcertina->setPanelSize(polarityInvertView.get(), 0, false);
            effectsConcertina->setPanelSize(reverbSendView.get(), 0, false);
            effectsConcertina->setPanelSize(eqView.get(), 0, false);
            effectsConcertina->expandPanelFully(expanderView.get(), false);
            effectsConcertina->expandPanelFully(compressorView.get(), false);
        }

        firstShow = false;
    }

}

// 5 Hz refresh of the Noise Gate open/closed indicator while the popup shows an input group
void ChannelGroupEffectsView::timerCallback()
{
    if (!peerMode && expanderView) expanderView->setGateState(processor.getInputGroupGateState(groupIndex));
}

void ChannelGroupEffectsView::updateStateForInput()
{
    if (!isTimerRunning()) startTimer(200);   
    if (expanderView) expanderView->setHardGateAvailable(true);

    CompressorParams compParams;
    if (processor.getInputCompressorParams(groupIndex, compParams)) {
        compressorView->updateParams(compParams);
    }

    CompressorParams expParams;
    if (processor.getInputExpanderParams(groupIndex, expParams)) {
        expanderView->updateParams(expParams);
    }

    ParametricEqParams eqparams;
    if (processor.getInputEqParams(groupIndex, eqparams)) {
        eqView->updateParams(eqparams);
    }

    reverbSendView->updateParams(processor.getInputReverbSend(groupIndex, true));
    polarityInvertView->updateParams(processor.getInputPolarityInvert(groupIndex));

    if (firstShow) {
        if (eqparams.enabled && !(compParams.enabled || expParams.enabled)) {
            effectsConcertina->expandPanelFully(eqView.get(), false);
        }
        else {
            effectsConcertina->setPanelSize(eqView.get(), 0, false);
            effectsConcertina->expandPanelFully(expanderView.get(), false);
            effectsConcertina->expandPanelFully(compressorView.get(), false);
        }

        firstShow = false;
    }
}

void ChannelGroupEffectsView::updateLayout()
{
    int minitemheight = 32;
#if JUCE_IOS || JUCE_ANDROID
    // make the button heights a bit more for touchscreen purposes
    minitemheight = 40;
#endif

    auto mincompbounds = compressorView->getMinimumContentBounds();
    auto mincompheadbounds = compressorView->getMinimumHeaderBounds();
    auto minexpbounds = expanderView->getMinimumContentBounds();
    auto minexpheadbounds = compressorView->getMinimumHeaderBounds();
    auto mineqbounds = eqView->getMinimumContentBounds();
    auto mineqheadbounds = eqView->getMinimumHeaderBounds();
    auto minrevbounds = reverbSendView->getMinimumContentBounds();
    auto minrevheadbounds = reverbSendView->getMinimumHeaderBounds();
    auto minprvheadbounds = polarityInvertView->getMinimumHeaderBounds();
    auto minprvbounds = polarityInvertView->getMinimumContentBounds();

    int gcount = 5 ;

    effectsBox.items.clear();
    effectsBox.flexDirection = FlexBox::Direction::column;
    effectsBox.items.add(FlexItem(4, 2));
    effectsBox.items.add(FlexItem(minexpbounds.getWidth(), minexpbounds.getHeight() + mincompbounds.getHeight() + gcount*minitemheight, *effectsConcertina).withMargin(1).withFlex(1));

    effectsConcertina->setPanelHeaderSize(compressorView.get(), mincompheadbounds.getHeight());
    effectsConcertina->setPanelHeaderSize(expanderView.get(), minexpheadbounds.getHeight());
    effectsConcertina->setPanelHeaderSize(eqView.get(), mineqheadbounds.getHeight());

    effectsConcertina->setPanelHeaderSize(reverbSendView.get(), minrevheadbounds.getHeight());
    effectsConcertina->setPanelHeaderSize(polarityInvertView.get(), minprvheadbounds.getHeight());

    effectsConcertina->setMaximumPanelSize(compressorView.get(), mincompbounds.getHeight()+5);
    effectsConcertina->setMaximumPanelSize(expanderView.get(), minexpbounds.getHeight()+5);
    effectsConcertina->setMaximumPanelSize(eqView.get(), mineqbounds.getHeight());

    effectsConcertina->setMaximumPanelSize(reverbSendView.get(), minrevbounds.getHeight());
    effectsConcertina->setMaximumPanelSize(polarityInvertView.get(), minprvbounds.getHeight());

}

void ChannelGroupEffectsView::resized()  {

    effectsBox.performLayout(getLocalBounds().reduced(2, 2));

}

void ChannelGroupEffectsView::compressorParamsChanged(CompressorView *comp, SonoAudio::CompressorParams & params)
{
    if (peerMode) {
        bool wason = processor.getRemotePeerEffectsActive(peerIndex, groupIndex);

        processor.setRemotePeerCompressorParams(peerIndex, groupIndex, params);

        bool ison = processor.getRemotePeerEffectsActive(peerIndex, groupIndex);
        if (wason != ison) {
            listeners.call (&ChannelGroupEffectsView::Listener::effectsEnableChanged, this);
        }
    }
    else {
        bool wason = processor.getInputEffectsActive(groupIndex);

        processor.setInputCompressorParams(groupIndex, params);

        bool ison = processor.getInputEffectsActive(groupIndex);
        if (wason != ison) {
            listeners.call (&ChannelGroupEffectsView::Listener::effectsEnableChanged, this);
        }
    }

}

void ChannelGroupEffectsView::expanderParamsChanged(ExpanderView *comp, SonoAudio::CompressorParams & params)
{
    if (peerMode) {
        bool wason = processor.getRemotePeerEffectsActive(peerIndex, groupIndex);

        processor.setRemotePeerExpanderParams(peerIndex, groupIndex, params);

        bool ison = processor.getRemotePeerEffectsActive(peerIndex, groupIndex);
        if (wason != ison) {
            listeners.call (&ChannelGroupEffectsView::Listener::effectsEnableChanged, this);
        }
    }
    else {
        bool wason = processor.getInputEffectsActive(groupIndex);

        processor.setInputExpanderParams(groupIndex, params);

        bool ison = processor.getInputEffectsActive(groupIndex);
        if (wason != ison) {
            listeners.call (&ChannelGroupEffectsView::Listener::effectsEnableChanged, this);
        }
    }
}

void ChannelGroupEffectsView::parametricEqParamsChanged(ParametricEqView *comp, SonoAudio::ParametricEqParams & params)
{
    if (peerMode) {
        bool wason = processor.getRemotePeerEffectsActive(peerIndex, groupIndex);

        processor.setRemotePeerEqParams(peerIndex, groupIndex, params);

        bool ison = processor.getRemotePeerEffectsActive(peerIndex, groupIndex);
        if (wason != ison) {
            listeners.call (&ChannelGroupEffectsView::Listener::effectsEnableChanged, this);
        }
    } else {
        bool wason = processor.getInputEffectsActive(groupIndex);

        processor.setInputEqParams(groupIndex, params);

        bool ison = processor.getInputEffectsActive(groupIndex);
        if (wason != ison) {
            listeners.call (&ChannelGroupEffectsView::Listener::effectsEnableChanged, this);
        }
    }
}

void ChannelGroupEffectsView::reverbSendLevelChanged(ReverbSendView *comp, float revlevel)
{
    if (peerMode) {
        bool wason = processor.getRemotePeerEffectsActive(peerIndex, groupIndex);

        processor.setRemotePeerChannelReverbSend(peerIndex, groupIndex, revlevel);

        bool ison = processor.getRemotePeerEffectsActive(peerIndex, groupIndex);
        if (wason != ison) {
            listeners.call (&ChannelGroupEffectsView::Listener::effectsEnableChanged, this);
        }
    }
    else {
        bool wason = processor.getInputEffectsActive(groupIndex);

        // input mode
        processor.setInputReverbSend(groupIndex, revlevel, true);

        bool ison = processor.getInputEffectsActive(groupIndex);
        if (wason != ison) {
            listeners.call (&ChannelGroupEffectsView::Listener::effectsEnableChanged, this);
        }
    }
}

void ChannelGroupEffectsView::polarityInvertChanged(PolarityInvertView *comp, bool polinv)
{
    if (peerMode) {
        processor.setRemotePeerPolarityInvert(peerIndex, groupIndex, polinv);
    }
    else {
        // input mode
        processor.setInputPolarityInvert(groupIndex, polinv);
    }
    listeners.call (&ChannelGroupEffectsView::Listener::effectsEnableChanged, this);
}


void ChannelGroupEffectsView::effectsHeaderClicked(EffectsBaseView *comp)
{
    if (comp == compressorView.get()) {
        bool changed = effectsConcertina->setPanelSize(eqView.get(), 0, true);
        changed |= effectsConcertina->expandPanelFully(expanderView.get(), true);
        changed |= effectsConcertina->expandPanelFully(compressorView.get(), true);
        if (!changed) {
            // toggle it
            CompressorParams params;

            if (peerMode) {
                processor.getRemotePeerCompressorParams(peerIndex, 0, params);
                params.enabled = !params.enabled;
                processor.setInputCompressorParams(0, params);
            } else {
                processor.getInputCompressorParams(groupIndex, params);
                params.enabled = !params.enabled;
                processor.setInputCompressorParams(groupIndex, params);
            }
            updateState();

            listeners.call (&ChannelGroupEffectsView::Listener::effectsEnableChanged, this);
        }
    }
    else if (comp == expanderView.get()) {
        bool changed = effectsConcertina->setPanelSize(eqView.get(), 0, true);
        changed |= effectsConcertina->expandPanelFully(compressorView.get(), true);
        changed |= effectsConcertina->expandPanelFully(expanderView.get(), true);
        if (!changed) {
            // toggle it
            CompressorParams params;
            if (peerMode) {
                processor.getRemotePeerExpanderParams(peerIndex, groupIndex, params);
                params.enabled = !params.enabled;
                processor.setRemotePeerExpanderParams(peerIndex, groupIndex, params);
            } else {
                processor.getInputExpanderParams(groupIndex, params);
                params.enabled = !params.enabled;
                processor.setInputExpanderParams(groupIndex, params);
            }
            updateState();

            listeners.call (&ChannelGroupEffectsView::Listener::effectsEnableChanged, this);
        }
    }
    else if (comp == eqView.get()) {
        bool changed = effectsConcertina->expandPanelFully(eqView.get(), true);
        if (!changed) {
            // toggle it
            ParametricEqParams params;
            if (peerMode) {
                processor.getRemotePeerEqParams(peerIndex, groupIndex, params);
                params.enabled = !params.enabled;
                processor.setRemotePeerEqParams(peerIndex, groupIndex, params);
            } else {
                processor.getInputEqParams(groupIndex, params);
                params.enabled = !params.enabled;
                processor.setInputEqParams(groupIndex, params);
            }
            updateState();

            listeners.call (&ChannelGroupEffectsView::Listener::effectsEnableChanged, this);
        }
    }
    else if (comp == reverbSendView.get()) {
        bool changed = effectsConcertina->expandPanelFully(reverbSendView.get(), true);
    }
}


#pragma ChannelGroupMonitorEffectsView

ChannelGroupMonitorEffectsView::ChannelGroupMonitorEffectsView(SonobusAudioProcessor& proc, bool peermode)
: Component(), peerMode(peermode), processor(proc)
{
    effectsConcertina =  std::make_unique<ConcertinaPanel>();

    delayView =  std::make_unique<MonitorDelayView>(processor);
    delayView->addListener(this);
    delayView->addHeaderListener(this);

    reverbSendView =  std::make_unique<ReverbSendView>(processor, false);
    reverbSendView->addListener(this);
    //reverbSendView->addHeaderListener(this);


    effectsConcertina->addPanel(-1, delayView.get(), false);
    effectsConcertina->setCustomPanelHeader(delayView.get(), delayView->getHeaderComponent(), false);

    effectsConcertina->addPanel(-1, reverbSendView.get(), false);
    effectsConcertina->setCustomPanelHeader(reverbSendView.get(), reverbSendView->getHeaderComponent(), false);


    addAndMakeVisible (effectsConcertina.get());

    setFocusContainerType(FocusContainerType::focusContainer);

    updateLayout();
}

ChannelGroupMonitorEffectsView::~ChannelGroupMonitorEffectsView()
{
    reverbSendView->removeListener(this);
    delayView->removeListener(this);
    delayView->removeHeaderListener(this);

    effectsConcertina.reset();
}

juce::Rectangle<int> ChannelGroupMonitorEffectsView::getMinimumContentBounds() const {
    auto minbounds = delayView->getMinimumContentBounds();
    auto headbounds = delayView->getMinimumHeaderBounds();

    auto minrevbounds = reverbSendView->getMinimumContentBounds();
    auto headrevbounds = reverbSendView->getMinimumHeaderBounds();


    int defWidth = jmax(minbounds.getWidth(), 0) + 12;
    int defHeight = 0;

    if (groupIndex < 0) {
        // don't include reverb
        defHeight = jmax(minbounds.getHeight() , 0) + headbounds.getHeight() + 8;
    } else {
        defHeight = jmax(minbounds.getHeight() + minrevbounds.getHeight(), 0) + headbounds.getHeight() + headrevbounds.getHeight() + 8;
    }

    return Rectangle<int>(0,0,defWidth,defHeight);
}


void ChannelGroupMonitorEffectsView::updateState()
{
    if (peerMode) {
        updateStateForRemotePeer();
    } else {
        updateStateForInput();
    }

}

void ChannelGroupMonitorEffectsView::updateStateForRemotePeer()
{

}

void ChannelGroupMonitorEffectsView::updateStateForInput()
{
    DelayParams monDelayParams;

    if (groupIndex == -1) {
        // met
        if (processor.getMetronomeMonitorDelayParams(monDelayParams)) {
            delayView->updateParams(monDelayParams);
        }

        if (reverbSendView->isVisible()) {
            reverbSendView->setVisible(false);
            reverbSendView->getHeaderComponent()->setVisible(false);
        }
    }
    else if (groupIndex == -2) {
        // file playback
        if (processor.getFilePlaybackMonitorDelayParams(monDelayParams)) {
            delayView->updateParams(monDelayParams);
        }

        if (reverbSendView->isVisible()) {
            reverbSendView->setVisible(false);
            reverbSendView->getHeaderComponent()->setVisible(false);
        }
    }
    else if (groupIndex == -3) {
        // soundboard
        monDelayParams = processor.getSoundboardProcessor()->getMonitorDelayParams();
        delayView->updateParams(monDelayParams);

        if (reverbSendView->isVisible()) {
            reverbSendView->setVisible(false);
            reverbSendView->getHeaderComponent()->setVisible(false);
        }
    }
    else {
        if (processor.getInputMonitorDelayParams(groupIndex, monDelayParams)) {
            delayView->updateParams(monDelayParams);
        }
        reverbSendView->updateParams(processor.getInputReverbSend(groupIndex, false));

        if (!reverbSendView->isVisible()) {
            reverbSendView->setVisible(true);
            reverbSendView->getHeaderComponent()->setVisible(true);
        }
    }

    if (firstShow) {
        effectsConcertina->setPanelSize(delayView.get(), 0, false);
        effectsConcertina->setPanelSize(reverbSendView.get(), 0, false);
        //effectsConcertina->expandPanelFully(expanderView.get(), false);
        firstShow = false;
    }
}

void ChannelGroupMonitorEffectsView::updateLayout()
{
    int minitemheight = 32;
#if JUCE_IOS || JUCE_ANDROID
    // make the button heights a bit more for touchscreen purposes
    minitemheight = 40;
#endif

    auto mindelaybounds = delayView->getMinimumContentBounds();
    auto mindelayheadbounds = delayView->getMinimumHeaderBounds();
    auto minrevbounds = reverbSendView->getMinimumContentBounds();
    auto minrevheadbounds = reverbSendView->getMinimumHeaderBounds();
    int gcount = 2;


    effectsBox.items.clear();
    effectsBox.flexDirection = FlexBox::Direction::column;
    effectsBox.items.add(FlexItem(4, 2));
    effectsBox.items.add(FlexItem(mindelaybounds.getWidth(), jmax(mindelaybounds.getHeight() + minrevbounds.getHeight(), 0) + gcount*minitemheight, *effectsConcertina).withMargin(1).withFlex(1));

    effectsConcertina->setPanelHeaderSize(delayView.get(), mindelayheadbounds.getHeight());
    effectsConcertina->setPanelHeaderSize(reverbSendView.get(), minrevheadbounds.getHeight());

    effectsConcertina->setMaximumPanelSize(delayView.get(), mindelaybounds.getHeight());
    effectsConcertina->setMaximumPanelSize(reverbSendView.get(), minrevbounds.getHeight());

}

void ChannelGroupMonitorEffectsView::resized()  {

    effectsBox.performLayout(getLocalBounds().reduced(2, 2));

}

void ChannelGroupMonitorEffectsView::reverbSendLevelChanged(ReverbSendView *comp, float revlevel)
{
    if (peerMode) {

        bool wason = processor.getRemotePeerEffectsActive(peerIndex, groupIndex);

        processor.setRemotePeerChannelReverbSend(peerIndex, groupIndex, revlevel);

        bool ison = processor.getRemotePeerEffectsActive(peerIndex, groupIndex);
        if (wason != ison) {
            listeners.call (&ChannelGroupMonitorEffectsView::Listener::monitorEffectsEnableChanged, this);
        }

    }
    else {
        bool wason = processor.getInputMonitorEffectsActive(groupIndex);

        processor.setInputReverbSend(groupIndex, revlevel, false);

        bool ison = processor.getInputMonitorEffectsActive(groupIndex);
        if (wason != ison) {
            listeners.call (&ChannelGroupMonitorEffectsView::Listener::monitorEffectsEnableChanged, this);
        }
    }
}


void ChannelGroupMonitorEffectsView::monitorDelayParamsChanged(MonitorDelayView *comp, SonoAudio::DelayParams & params)
{
    if (peerMode) {
        /*
        bool wason = processor.getRemotePeerEffectsActive(peerIndex, groupIndex);

        processor.setRemotePeerCompressorParams(peerIndex, groupIndex, params);

        bool ison = processor.getRemotePeerEffectsActive(peerIndex, groupIndex);
        if (wason != ison) {
            listeners.call (&ChannelGroupEffectsView::Listener::effectsEnableChanged, this);
        }
         */
    }
    else {
        bool wason = false;
        bool ison = params.enabled;

        auto deltimems = params.delayTimeMs;
        DelayParams eparam;

        if (groupIndex == -1) {
            processor.getMetronomeMonitorDelayParams(eparam);
            wason = eparam.enabled;
            processor.setMetronomeMonitorDelayParams(params);
        } else if (groupIndex == -2) {
            processor.getFilePlaybackMonitorDelayParams(eparam);
            wason = eparam.enabled;
            processor.setFilePlaybackMonitorDelayParams(params);
        } else if (groupIndex == -3) {
            eparam = processor.getSoundboardProcessor()->getMonitorDelayParams();
            wason = eparam.enabled;
            processor.getSoundboardProcessor()->setMonitorDelayParams(params);
        } else {
            wason = processor.getInputMonitorEffectsActive(groupIndex);
            processor.setInputMonitorDelayParams(groupIndex, params);
        }

        if (processor.getLinkMonitoringDelayTimes()) {
            // set for all the others too
            int numgroups = processor.getInputGroupCount();
            for (int i=0; i < numgroups; ++i) {
                processor.getInputMonitorDelayParams(i, eparam);
                if (eparam.delayTimeMs != deltimems) {
                    eparam.delayTimeMs = deltimems;
                    processor.setInputMonitorDelayParams(i, eparam);
                }
            }

            processor.getMetronomeMonitorDelayParams(eparam);
            if (eparam.delayTimeMs != deltimems) {
                eparam.delayTimeMs = deltimems;
                processor.setMetronomeMonitorDelayParams(eparam);
            }

            processor.getFilePlaybackMonitorDelayParams(eparam);
            if (eparam.delayTimeMs != deltimems) {
                eparam.delayTimeMs = deltimems;
                processor.setFilePlaybackMonitorDelayParams(eparam);
            }

            eparam = processor.getSoundboardProcessor()->getMonitorDelayParams();
            if (eparam.delayTimeMs != deltimems) {
                eparam.delayTimeMs = deltimems;
                processor.getSoundboardProcessor()->setMonitorDelayParams(eparam);
            }
        }

        if (wason != ison) {
            listeners.call (&ChannelGroupMonitorEffectsView::Listener::monitorEffectsEnableChanged, this);
        }
    }

}


void ChannelGroupMonitorEffectsView::effectsHeaderClicked(EffectsBaseView *comp)
{
    if (comp == delayView.get()) {
        bool changed = effectsConcertina->setPanelSize(delayView.get(), 0, true);
        //changed |= effectsConcertina->expandPanelFully(expanderView.get(), true);
        //changed |= effectsConcertina->expandPanelFully(compressorView.get(), true);
        //if (!changed)
        {
            // toggle it
            DelayParams params;

            if (peerMode) {
                //processor.getRemotePeerCompressorParams(peerIndex, 0, params);
                //params.enabled = !params.enabled;
                //processor.setInputCompressorParams(0, params);
            } else {
                if (groupIndex == -1) {
                    processor.getMetronomeMonitorDelayParams(params);
                    params.enabled = !params.enabled;
                    processor.setMetronomeMonitorDelayParams(params);
                } else if (groupIndex == -2) {
                    processor.getFilePlaybackMonitorDelayParams(params);
                    params.enabled = !params.enabled;
                    processor.setFilePlaybackMonitorDelayParams(params);
                }
                else if (groupIndex == -3) {
                    params = processor.getSoundboardProcessor()->getMonitorDelayParams();
                    params.enabled = !params.enabled;
                    processor.getSoundboardProcessor()->setMonitorDelayParams(params);
                }
                else {
                    processor.getInputMonitorDelayParams(groupIndex, params);
                    params.enabled = !params.enabled;
                    processor.setInputMonitorDelayParams(groupIndex, params);
                }
            }
            updateState();

            listeners.call (&ChannelGroupMonitorEffectsView::Listener::monitorEffectsEnableChanged, this);
        }
    }
}

#pragma ChannelGroupReverbEffectsView

ChannelGroupReverbEffectsView::ChannelGroupReverbEffectsView(SonobusAudioProcessor& proc)
: Component(), processor(proc)
{
    effectsConcertina =  std::make_unique<ConcertinaPanel>();

    reverbView =  std::make_unique<ReverbView>(processor, true);
    reverbView->addListener(this);
    //reverbSendView->addHeaderListener(this);


    effectsConcertina->addPanel(-1, reverbView.get(), false);
    effectsConcertina->setCustomPanelHeader(reverbView.get(), reverbView->getHeaderComponent(), false);


    addAndMakeVisible (effectsConcertina.get());

    setFocusContainerType(FocusContainerType::focusContainer);

    updateLayout();
}

ChannelGroupReverbEffectsView::~ChannelGroupReverbEffectsView()
{
    reverbView->removeListener(this);
    effectsConcertina.reset();
}

juce::Rectangle<int> ChannelGroupReverbEffectsView::getMinimumContentBounds() const {
    auto minrevbounds = reverbView->getMinimumContentBounds();
    auto headrevbounds = reverbView->getMinimumHeaderBounds();


    int defWidth = jmax(minrevbounds.getWidth(), 0) + 12;
    int defHeight = 0;

    defHeight = jmax(minrevbounds.getHeight() , 0) + headrevbounds.getHeight() + 8;

    return Rectangle<int>(0,0,defWidth,defHeight);
}


void ChannelGroupReverbEffectsView::updateState()
{
    reverbView->updateParams();

    if (firstShow) {
        effectsConcertina->setPanelSize(reverbView.get(), 0, false);
        firstShow = false;
    }
}




void ChannelGroupReverbEffectsView::updateLayout()
{
    int minitemheight = 32;
#if JUCE_IOS || JUCE_ANDROID
    // make the button heights a bit more for touchscreen purposes
    minitemheight = 40;
#endif

    auto minrevbounds = reverbView->getMinimumContentBounds();
    auto minrevheadbounds = reverbView->getMinimumHeaderBounds();
    int gcount = 1;


    effectsBox.items.clear();
    effectsBox.flexDirection = FlexBox::Direction::column;
    effectsBox.items.add(FlexItem(4, 2));
    effectsBox.items.add(FlexItem(minrevbounds.getWidth(), jmax(minrevbounds.getHeight(), 0) + gcount*minitemheight, *effectsConcertina).withMargin(1).withFlex(1));

    effectsConcertina->setPanelHeaderSize(reverbView.get(), minrevheadbounds.getHeight());

    effectsConcertina->setMaximumPanelSize(reverbView.get(), minrevbounds.getHeight());

}

void ChannelGroupReverbEffectsView::resized()  {

    effectsBox.performLayout(getLocalBounds().reduced(2, 2));

}




void ChannelGroupReverbEffectsView::effectsHeaderClicked(EffectsBaseView *comp)
{
    /*
    if (comp == reverbView.get()) {
        bool changed = effectsConcertina->setPanelSize(reverbView.get(), 0, true);


        updateState();

        //listeners.call (&ChannelGroupMonitorEffectsView::Listener::monitorEffectsEnableChanged, this);
    }
     */
}


#pragma ChannelGroupView


ChannelGroupView::ChannelGroupView() : smallLnf(12), medLnf(14), sonoSliderLNF(12), panSliderLNF(12)
{
    bgColor = Colour::fromFloatRGBA(0.112f, 0.112f, 0.112f, 1.0f);
    borderColor = Colour::fromFloatRGBA(0.5f, 0.5f, 0.5f, 0.3f);

    sonoSliderLNF.textJustification = Justification::centredLeft;
    panSliderLNF.textJustification = Justification::centredLeft;

    //setFocusContainerType(FocusContainerType::keyboardFocusContainer);

    //Random rcol;
    //itemColor = Colour::fromHSV(rcol.nextFloat(), 0.5f, 0.2f, 1.0f);
}

ChannelGroupView::~ChannelGroupView()
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

void ChannelGroupView::paint(Graphics& g)
{
    //g.fillAll (Colour(0xff111111));
    //g.fillAll (Colour(0xff202020));

    if (useBgColor) {
        g.fillAll (bgColor);
        //g.setColour(bgColor);
        //g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
    }

    //g.drawRoundedRectangle(getLocalBounds().toFloat(), 6.0f, 0.5f);

    if (showDivider) {
        g.setColour(borderColor);
        g.drawLine(0, 0, getWidth(), 0, 1);
    }

}

int ChannelGroupView::headerHeight (const HeaderLayout & h)
{
    int height = kHeaderTop + kHeaderLineH;                                     // wide: one line
    if (h.lower) height += kHeaderGap + kHeaderLowerH;                          // and the lower line
    if (h.form == HeaderWide) return height;
    height += kHeaderGap + (h.form == HeaderNarrowTall ? kHeaderKnobH : kHeaderLineH);   // name line above the level line
    if (h.form != HeaderMedium && h.showDest) height += kHeaderGap + kHeaderLineH;       // output line
    return height;
}

void ChannelGroupView::layoutHeader()
{
    const auto & h = header;
    const int right = getWidth() - 2;
    int y = kHeaderTop;

    // Channel numbers whole: the link icon beside them gives up width first
    if (linkButton->getStyle() == DrawableButton::ImageRightOfTextLabel && linkButton->getNormalImage() != nullptr) {
        const float w = (float) (kHeaderIndent - 3);
        const float tw = (float) SbTextFit::textWidth(smallLnf.getDrawableButtonTextFont(kHeaderLineH), linkButton->getButtonText());
        const float ratio = jlimit(0.12f, 0.4f, 1.0f - (tw + 5.0f) / w);
        if (std::abs(ratio - linkButton->getForegroundImageRatio()) > 0.001f) {
            linkButton->setForegroundImageRatio(ratio);
            linkButton->resized();
            linkButton->repaint();
        }
    }
    linkButton->setBounds(0, y, kHeaderIndent - 3, kHeaderLineH);
    monoButton->setBounds(linkButton->getBounds());

    int knobX = 0, knobY = 0, lowerY = 0, lowerRight = 0;

    if (h.form == HeaderWide) {
        // Name, level and knobs are columns that end where the row with the widest meter ends them, so every
        // level slider ends at the same place; the Output button takes the room from the knobs to this row's meter.
        int xr = right - h.meterSlack;
        int destRight = right;
        if (h.showMeter) {
            meter->setBounds(right - h.meterW, y, h.meterW, kHeaderLineH);
            destRight = right - h.meterW - 6;
            xr -= jmax(h.meterW, h.meterCol) + 6;
        }
        if (h.destCol > 0) xr -= h.destCol + 6;
        knobX = xr - h.knobCol;
        knobY = y - 1;
        if (h.destCol > 0) {
            const int destX = knobX + h.knobCol + 6;
            destButton->setBounds(destX, y, jmax(0, destRight - destX), kHeaderLineH);
        }
        const int nameW = jmax(0, jmin(h.nameCol, knobX - 6 - kHeaderIndent - 6));
        nameLabel->setBounds(kHeaderIndent, y, nameW, kHeaderLineH);
        const int levelX = kHeaderIndent + nameW + 6;
        levelSlider->setBounds(levelX, y, jmax(0, knobX - 6 - levelX), kHeaderLineH);
        lowerY = y + kHeaderLineH + kHeaderGap;
        lowerRight = knobX - 6;
    }
    else {
        int xr = right;
        if (h.showMeter) {
            meter->setBounds(xr - h.meterW, y, h.meterW, kHeaderLineH);
            xr -= h.meterW + 6;
        }
        nameLabel->setBounds(kHeaderIndent, y, jmax(0, xr - kHeaderIndent), kHeaderLineH);

        y += kHeaderLineH + kHeaderGap;
        const bool tall = h.form == HeaderNarrowTall;
        knobX = right - h.knobCol;
        knobY = y - 1;
        const int levelY = tall ? y + (kHeaderKnobH - kHeaderLineH) / 2 : y;
        int levelRight = knobX - 6;
        if (h.form == HeaderMedium && h.destCol > 0) {
            destButton->setBounds(levelRight - h.destCol, levelY, h.destCol, kHeaderLineH);
            levelRight -= h.destCol + 6;
        }
        levelSlider->setBounds(0, levelY, jmax(0, levelRight), kHeaderLineH);   // from the row's left edge

        y += (tall ? kHeaderKnobH : kHeaderLineH) + kHeaderGap;
        lowerY = y;
        lowerRight = tall ? right : knobX - 6;
        if (h.form != HeaderMedium && h.showDest)
            destButton->setBounds(0, lowerY + (h.lower ? kHeaderLowerH + kHeaderGap : 0), jmax(0, jmin(h.destOwn, right)), kHeaderLineH);
    }

    // Knobs: pan alone in the middle of its column, or pan and width side by side (a row without the lower line
    // has a small knob without caption)
    const int knobH = jmax(0, jmin(kHeaderKnobH, getHeight() - knobY));
    if (panMode == PanPair) {
        const int panW = 46, widthW = 40, both = panW + 4 + widthW;
        const int kx = knobX + (h.knobCol - both) / 2;
        panSlider->setBounds(kx, knobY, panW, knobH);
        widthSlider->setBounds(kx + panW + 4, knobY, widthW, knobH);
    } else {
        const int panW = jmin(46, h.knobCol);
        panSlider->setBounds(knobX + (h.knobCol - panW) / 2, knobY, panW, knobH);
    }

    // Lower line, from the row's left edge: status text, then the buttons, then room for a note
    int x = 0;
    statSlot.setBounds(0, lowerY, h.statW, kHeaderLowerH);
    if (h.statW > 0) x = h.statW + 8;
    const int buttonY = lowerY + (kHeaderLowerH - kHeaderButtonH) / 2;
    // Never narrower than the widest caption, when the line has the room
    int buttonW = h.buttonW;
    {
        int need = 0;
        for (TextButton * b : { muteButton.get(), soloButton.get(), fxButton.get() })
            need = jmax(need, SbTextFit::buttonWidth(*b, kHeaderButtonH));
        if (need > buttonW && x + 3 * (need + 3) <= lowerRight) buttonW = need;
    }
    for (Button * b : { (Button *) muteButton.get(), (Button *) soloButton.get(), (Button *) fxButton.get() }) {
        b->setBounds(x, buttonY, buttonW, kHeaderButtonH);
        if (b->isVisible()) x += buttonW + 3;
    }
    x += 5;
    noteSlot.setBounds(x, lowerY, jmax(0, lowerRight - x), kHeaderLowerH);
}

void ChannelGroupView::resized()
{
    if (header.active) layoutHeader();
    else mainbox.performLayout(getLocalBounds());
    
    if (panLabel) {
        panLabel->setBounds(panSlider->getBounds().removeFromTop(16).translated(0, -2));
    }

    // Caption in the gap above the meter, clear of its top edge (the layout leaves that gap on caption rows)
    const int capH = meterCaptionHeight();
    auto placeCaption = [capH](Label * cap, Rectangle<int> over) {
        if (cap) cap->setBounds(over.getCentreX() - 20, over.getY() - 1 - capH, 40, capH);
    };
    if (premeter) {
        const bool pair = premeter2 && premeter2->isVisible();
        placeCaption(inCaption.get(), pair ? premeter->getBounds().getUnion(premeter2->getBounds()) : premeter->getBounds());
    }
    if (meter) placeCaption(outCaption.get(), meter->getBounds());
    

    int triwidth = 10;
    int triheight = 6;
    
    if (levelSlider) {
        levelSlider->setMouseDragSensitivity(jmax(128, levelSlider->getWidth()));
    }

}

int ChannelGroupView::meterCaptionHeight() const
{
    if (! inCaption) return 0;
    return (int) std::ceil(inCaption->getLookAndFeel().getLabelFont(*inCaption).getHeight());
}

//
// ==================================================
//

ChannelGroupsView::ChannelGroupsView(SonobusAudioProcessor& proc, bool peerMode, int peerIndex)
 : Component("pcv"),  addLnf(20), processor(proc), mPeerMode(peerMode), mPeerIndex(peerIndex)
{
    mutedTextColor = Colour::fromFloatRGBA(0.8, 0.5, 0.2, 1.0);
    regularTextColor = Colour(0xa0eeeeee);; //Colour(0xc0eeeeee);
    dimTextColor = Colour(0xa0aaaaaa); //Colour(0xc0aaaaaa);
    //soloColor = Colour::fromFloatRGBA(0.2, 0.5, 0.8, 1.0);
    mutedColor = Colour::fromFloatRGBA(0.6, 0.3, 0.1, 1.0);
    soloColor = Colour::fromFloatRGBA(1.0, 1.0, 0.6, 1.0);
    mutedBySoloColor = Colour::fromFloatRGBA(0.24, 0.24, 0.26, 1.0);   // A quiet gray
    
    droppedTextColor = Colour(0xc0ee8888);

    outlineColor = Colour::fromFloatRGBA(0.25, 0.25, 0.25, 1.0);

    //bgColor = Colour::fromFloatRGBA(0.045f, 0.045f, 0.05f, 1.0f);
    bgColor = Colour::fromFloatRGBA(0.025f, 0.06f, 0.055f, 1.0f);   // Very dark teal

    mInGainSlider     = std::make_unique<Slider>(Slider::LinearHorizontal,  Slider::TextBoxAbove);
    mInGainSlider->setName("ingain");
    mInGainSlider->setSliderSnapsToMousePosition(processor.getSlidersSnapToMousePosition());
    mInGainSlider->setTextBoxIsEditable(true);
    mInGainSlider->setScrollWheelEnabled(false);

    mAddButton = std::make_unique<TextButton>("+");
    mAddButton->setTitle(TRANS("Add Input Group"));
    mAddButton->onClick = [this] {  addGroupPressed(); };
    mAddButton->setLookAndFeel(&addLnf);
    mAddButton->setTooltip(TRANS("Add New Input Group"));
    addChildComponent(mAddButton.get());

    mClearButton = std::make_unique<TextButton>(TRANS("CLEAR"));
    mClearButton->onClick = [this] {  clearGroupsPressed(); };
    //mClearButton->setLookAndFeel(&addLnf);
    mClearButton->setTooltip(TRANS("Remove all input groups"));
    addChildComponent(mClearButton.get());

    mInReverbButton = std::make_unique<TextButton>(TRANS("In Reverb"));
    //mClearButton->setLookAndFeel(&addLnf);
    mInReverbButton->setTooltip(TRANS("Configure input reverb parameters"));
    addChildComponent(mInReverbButton.get());
    mInReverbButton->onClick = [this]() {
        if (!inReverbCalloutBox) {
            showInputReverbView(true);
        } else {
            showInputReverbView(false);
        }
    };

    mMonDelayButton = std::make_unique<TextButton>(TRANS("Monitor Delay"));
    mMonDelayButton->setColour(TextButton::buttonOnColourId, Colour::fromFloatRGBA(0.2, 0.5, 0.7, 0.5));
    mMonDelayButton->setTooltip(TRANS("Toggle monitor delay enabled on all input groups"));
    addChildComponent(mMonDelayButton.get());
    mMonDelayButton->onClick = [this]() {
        toggleAllMonitorDelay();
    };


    mInsertLine = std::make_unique<DrawableRectangle>();
    //mInsertLine->setCornerSize(Point<float>(6,6));
    //mInsertLine->setFill (Colour::fromFloatRGBA(0.07, 0.07, 0.07, 1.0));
    mInsertLine->setFill (Colours::transparentBlack);
    mInsertLine->setStrokeFill (Colour::fromFloatRGBA(0.5, 0.5, 0.5, 0.75));
    mInsertLine->setStrokeThickness(2);
    addChildComponent(mInsertLine.get());

    mDragDrawable = std::make_unique<DrawableImage>();
    mDragDrawable->setAlpha(0.4f);
    mDragDrawable->setAlwaysOnTop(true);
    addChildComponent(mDragDrawable.get());

    mMetFileBg = std::make_unique<DrawableRectangle>();
    mMetFileBg->setFill (Colour::fromFloatRGBA(0.0, 0.0, 0.0, 0.75));
    //mMetFileBg->setStrokeFill (Colour::fromFloatRGBA(0.4, 0.4, 0.4, 0.3));
    mMetFileBg->setStrokeFill (Colours::transparentBlack);
    //mMetFileBg->setStrokeThickness(0.75f);
    mMetFileBg->setCornerSize(Point<float>(8.0f, 8.0f));
    addChildComponent(mMetFileBg.get());

    // Buffer the static header widgets to images so meter-driven dirty rects blit a cached image instead of
    // re-rasterising text/paths. JUCE re-renders the cache on the widget's own repaint() (toggle/setText/setValue),
    // so nothing goes stale.
    for (Component* c : std::initializer_list<Component*>{
            mInGainSlider.get(), mAddButton.get(), mClearButton.get(),
            mInReverbButton.get(), mMonDelayButton.get() }) {
        if (c) c->setBufferedToImage(true);
    }

    rebuildChannelViews();
}

ChannelGroupsView::~ChannelGroupsView()
{
    if (mEffectsView) {
        mEffectsView->removeListener(this);
    }
    // An open FX pop-up of this row would stay behind as an empty box
    if (auto * callout = dynamic_cast<CallOutBox*>(effectsCalloutBox.get())) {
        callout->dismiss();
    }
}

void ChannelGroupsView::configLevelSlider(Slider * slider, bool monmode)
{
    //slider->setTextValueSuffix(" dB");
    slider->setColour(Slider::textBoxBackgroundColourId, Colours::transparentBlack);
    slider->setColour(Slider::textBoxOutlineColourId, Colours::transparentBlack);
    slider->setColour(Slider::textBoxTextColourId, Colour(0x90eeeeee));
    slider->setColour(TooltipWindow::textColourId, Colour(0xf0eeeeee));

    
    slider->setTextBoxStyle(Slider::TextBoxAbove, true, 100, 12);

    if (monmode) {
        slider->setRange(0.0, 1.0, 0.0);
        slider->setMouseDragSensitivity(90);
    } else {
        slider->setRange(0.0, 2.0, 0.0);
    }
    slider->setSkewFactor(0.5);
    slider->setDoubleClickReturnValue(true, 1.0);
    slider->setTextBoxIsEditable(true);
    slider->setSliderSnapsToMousePosition(processor.getSlidersSnapToMousePosition());
    slider->setScrollWheelEnabled(false);
    slider->setWantsKeyboardFocus(true);
    slider->valueFromTextFunction = [](const String& s) -> float { return Decibels::decibelsToGain(s.getFloatValue()); };

    if (mPeerMode) {
        if (monmode) {
            slider->textFromValueFunction = [](float v) -> String { return String(TRANS("Level: ")) + Decibels::toString(Decibels::gainToDecibels(v), 1); };
        }
        else {
            slider->textFromValueFunction = [](float v) -> String { return String(TRANS("Level: ")) + Decibels::toString(Decibels::gainToDecibels(v), 1); };
        }
    } else {
        if (monmode) {
            slider->textFromValueFunction = [](float v) -> String { return String(TRANS("Monitor: ")) + Decibels::toString(Decibels::gainToDecibels(v), 1); };
        }
        else {
            slider->textFromValueFunction = [](float v) -> String { return String(TRANS("Pre Level: ")) + Decibels::toString(Decibels::gainToDecibels(v), 1); };
        }
    }


#if JUCE_IOS
    //slider->setPopupDisplayEnabled(true, false, this);
#endif
    slider->updateText();
}

void ChannelGroupsView::configKnobSlider(Slider * slider)
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
    slider->setWantsKeyboardFocus(true);

    //slider->setLookAndFeel(&sonoSliderLNF);
    
    //slider->valueFromTextFunction = [](const String& s) -> float { return Decibels::decibelsToGain(s.getFloatValue()); };
    //slider->textFromValueFunction = [](float v) -> String { return Decibels::toString(Decibels::gainToDecibels(v), 1); };
#if JUCE_IOS
    //slider->setPopupDisplayEnabled(true, false, this);
#endif
    slider->updateText();
}

void ChannelGroupsView::configLabel(Label *label, int ltype)
{
    if (ltype == LabelTypeSmallDim) {
        label->setFont(12);
        label->setColour(Label::textColourId, dimTextColor);
        label->setJustificationType(Justification::centredRight);        
        label->setMinimumHorizontalScale(1.0f);
    }
    else if (ltype == LabelTypeSmall) {
        label->setFont(12 );
        label->setColour(Label::textColourId, regularTextColor);
        label->setJustificationType(Justification::centredRight);
        label->setMinimumHorizontalScale(1.0f);
    }
    else {
        label->setFont(14);
        //label->setColour(Label::textColourId, Colour(0xaaeeeeee));
        label->setJustificationType(Justification::centredLeft);
    }
}


void ChannelGroupsView::setPeerMode(bool peermode, int index)
{
    if (mPeerMode != peermode || index != mPeerIndex) {
        mPeerMode = peermode;
        mPeerIndex = index;
        updateLayout();
    }
}



void ChannelGroupsView::resized()
{
    mLayoutChanged = false;
    Rectangle<int> bounds = getLocalBounds();

    if (!mPeerMode) {
        bounds = bounds.reduced(5, 0);
        bounds.removeFromLeft(3);
    }
    channelsBox.performLayout(bounds);

    int startind = -1;
    Point<int> topleft;
    for (int i=0; i < mChannelViews.size(); ++i) {
        ChannelGroupView * pvf = mChannelViews.getUnchecked(i);
        pvf->resized();

        if (pvf->groupChanCount == 2 && pvf->chanIndex == 1) continue;   // a stereo pair is one row

        if (startind < 0) {
            startind = 0;
            topleft = pvf->getPosition();
        }

        if (pvf->chanIndex == pvf->groupChanCount-1 || pvf->groupChanCount == 2) {
            // last in the group finish the bounds
            if (pvf->group >= mChanGroupBounds.size()) {
                mChanGroupBounds.resize(pvf->group+1);
            }
            mChanGroupBounds.getReference(pvf->group) = Rectangle<int>(topleft.getX(), topleft.getY(), pvf->getRight() - topleft.getX(), pvf->getBottom() - topleft.getY());
            startind = -1;
        }
    }

    if (mMainChannelView) {
        mMainChannelView->resized();
    }
    // The fixed rows too: a new name column moves their columns even when their own size stays
    for (auto * v : { mMetChannelView.get(), mFileChannelView.get(), mSoundboardChannelView.get() })
        if (v != nullptr) v->resized();

    if (mMetChannelView && mMetChannelView->isVisible()) {
        // resize bg border
        auto mfbounds = Rectangle<int>(mMetChannelView->getX() - 3, mMetChannelView->getY(), mMetChannelView->getWidth() + 6, mSoundboardChannelView->getBottom() - mMetChannelView->getY() + 4);
        mMetFileBg->setRectangle (mfbounds.toFloat());
    }

    Component* dw = nullptr; // this->findParentComponentOfClass<DocumentWindow>();    
    if (!dw)
        dw = this->findParentComponentOfClass<AudioProcessorEditor>();
    if (!dw)
        dw = this->findParentComponentOfClass<Component>();
    if (!dw)
        dw = this;

    if (auto * callout = dynamic_cast<CallOutBox*>(effectsCalloutBox.get())) {
        callout->dismiss();
        effectsCalloutBox = nullptr;
    }

    updateCutTooltips();
}

void ChannelGroupsView::showPopTip(const String & message, int timeoutMs, Component * target, int maxwidth)
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
    text.setFont(Font(12* SonoLookAndFeel::getFontScale()));
    if (target) {
        popTip->showAt(target, text, timeoutMs);
    }
    else {
        Rectangle<int> topbox(getWidth()/2 - maxwidth/2, 0, maxwidth, 2);
        popTip->showAt(topbox, text, timeoutMs);
    }
    popTip->toFront(false);
    //AccessibilityHandler::postAnnouncement(message, AccessibilityHandler::AnnouncementPriority::high);
}

void ChannelGroupsView::paint(Graphics & g)
{    
    //g.fillAll (Colours::black);
    Rectangle<int> bounds = getLocalBounds();

    if (!mPeerMode) {
        bounds.reduce(1, 1);
        bounds.removeFromLeft(3);

        g.setColour(bgColor);
        g.fillRoundedRectangle(bounds.toFloat(), 6.0f);
        g.setColour(outlineColor);
        g.drawRoundedRectangle(bounds.toFloat(), 6.0f, 0.5f);
    }
}

ChannelGroupView * ChannelGroupsView::createChannelGroupView(bool first)
{
    ChannelGroupView * pvf = new ChannelGroupView();

    pvf->nameLabel = std::make_unique<Label>("name", "");
    pvf->nameLabel->setJustificationType(Justification::centredLeft);
    pvf->nameLabel->setFont(15);
    pvf->nameLabel->setMinimumHorizontalScale(1.0f);   // Never squish a long player name; the row widens instead

    pvf->nameEditor = std::make_unique<TextEditor>("name");
    pvf->nameEditor->setFont(15 * SonoLookAndFeel::getFontScale());
    //pvf->nameEditor->setReadOnly(mPeerMode);
    auto edcb = [this,pvf]() {
        auto changroup = pvf->group;
        nameLabelChanged(changroup, pvf->nameEditor->getText());
    };
    pvf->nameEditor->onFocusLost = edcb;

    pvf->nameEditor->onReturnKey = [this,pvf]() {
        auto changroup = pvf->group;
        nameLabelChanged(changroup, pvf->nameEditor->getText());
        pvf->nameEditor->giveAwayKeyboardFocus();
    };

    pvf->nameEditor->onEscapeKey = [this,pvf]() {
        auto changroup = pvf->group;
        if (!mPeerMode) {
            pvf->nameEditor->setText(processor.getInputGroupName(changroup), dontSendNotification);
        }
        
        pvf->nameEditor->giveAwayKeyboardFocus();
    };


    /*
    pvf->nameLabel->setEditable(!mPeerMode);
    pvf->nameLabel->onTextChange = [this,pvf]() {
        auto changroup = pvf->group;
        nameLabelChanged(changroup, pvf->nameLabel->getText());
    };
     */



    if (!mPeerMode) {
        pvf->nameEditor->setColour(TextEditor::outlineColourId, Colour(0x66666666));
        pvf->nameEditor->setColour(TextEditor::backgroundColourId, Colours::black);
        // The name is a label that edits on a click: a long name ends in "..." and keeps its start in view
        pvf->nameLabel->setColour(Label::outlineColourId, Colour(0x66666666));
        pvf->nameLabel->setColour(Label::backgroundColourId, Colours::black);
        pvf->nameLabel->setColour(Label::outlineWhenEditingColourId, Colour(0x66666666));
        pvf->nameLabel->setColour(Label::backgroundWhenEditingColourId, Colours::black);
        pvf->nameLabel->setEditable(true, false, false);
        pvf->nameLabel->onTextChange = [this,pvf]() {
            nameLabelChanged(pvf->group, pvf->nameLabel->getText());
            // the name column follows the new name at once, growing or shrinking
            updateLayout(true);
            resized();
            updateCutTooltips();
        };
        pvf->nameLabel->setTooltip(TRANS("Set name for this group that others will see"));
    } else {
        pvf->nameLabel->setTooltip({});
        //pvf->nameLabel->setColour(TextEditor::outlineColourId, Colours::transparentBlack);
        //pvf->nameLabel->setColour(TextEditor::backgroundColourId, Colours::transparentBlack);
    }



    pvf->muteButton = std::make_unique<TextButton>(TRANS("MUTE"));
    pvf->muteButton->addListener(this);
    pvf->muteButton->setLookAndFeel(&pvf->medLnf);
    pvf->muteButton->setClickingTogglesState(true);
    pvf->muteButton->setColour(TextButton::buttonOnColourId, mutedColor);

    if (mPeerMode) {
        if (first) {
            pvf->muteButton->setTooltip(TRANS("Toggles receive muting, preventing audio from being heard for this user"));
        }
        else {
            pvf->muteButton->setTooltip(TRANS("Toggles receive muting, preventing audio from being heard for this user"));
        }
    } else {
        pvf->muteButton->setTooltip(TRANS("Mute this channel for both sending and monitoring"));
    }

    pvf->soloButton = std::make_unique<TextButton>(TRANS("SOLO"));
    pvf->soloButton->addListener(this);
    pvf->soloButton->setLookAndFeel(&pvf->medLnf);
    pvf->soloButton->setClickingTogglesState(true);
    pvf->soloButton->setColour(TextButton::buttonOnColourId, soloColor.withAlpha(0.7f));
    pvf->soloButton->setColour(TextButton::textColourOnId, Colours::darkblue);
    if (mPeerMode) {
        if (first) {
            pvf->soloButton->setTooltip(TRANS("Solo - Listen to only this user, and other soloed users. Alt-click to exclusively solo this user."));
        } else {
            pvf->soloButton->setTooltip(TRANS("Solo - Listen to only this channel for this user"));
        }
    } else {
        pvf->soloButton->setTooltip(TRANS("Solo - Listen to only this channel, does not affect sending"));
    }

    
    pvf->chanLabel = std::make_unique<Label>("status", "");
    configLabel(pvf->chanLabel.get(), LabelTypeRegular);
    pvf->chanLabel->setJustificationType(Justification::centredLeft);

    
    pvf->levelSlider     = std::make_unique<Slider>(Slider::LinearHorizontal,  Slider::TextBoxRight);
    pvf->levelSlider->setName("level");
    pvf->levelSlider->addListener(this);

    configLevelSlider(pvf->levelSlider.get(), false);
    pvf->levelSlider->setLookAndFeel(&pvf->sonoSliderLNF);

    pvf->monitorSlider     = std::make_unique<Slider>(Slider::RotaryHorizontalVerticalDrag,  Slider::TextBoxRight);
    pvf->monitorSlider->setName("monitor");
    pvf->monitorSlider->addListener(this);

    configLevelSlider(pvf->monitorSlider.get(), true);
    pvf->monitorSlider->setLookAndFeel(&pvf->panSliderLNF);
    pvf->monitorSlider->setTextBoxStyle(Slider::NoTextBox, true, 60, 14);
    //pvf->monitorSlider->setTooltip(TRANS("Monitor output level"));


    pvf->panLabel = std::make_unique<Label>("pan", TRANS("Pan"));
    configLabel(pvf->panLabel.get(), LabelTypeSmall);
    pvf->panLabel->setJustificationType(Justification::centredTop);
    pvf->panLabel->setAccessible(false);

    pvf->panSlider     = std::make_unique<Slider>(Slider::LinearHorizontal,  Slider::NoTextBox);
    //pvf->panSlider->setTextBoxStyle(Slider::TextBoxAbove, true, 60, 12);
    pvf->panSlider->setTitle(TRANS("Pan"));
    pvf->panSlider->setName(first ? "firstpan1": "pan1");
    pvf->panSlider->addListener(this);
    pvf->panSlider->getProperties().set ("fromCentre", true);
    pvf->panSlider->getProperties().set ("noFill", true);
    pvf->panSlider->setRange(-1, 1, 0.0f);
    pvf->panSlider->setDoubleClickReturnValue(true, 0.0);
    pvf->panSlider->setTextBoxIsEditable(true);
    pvf->panSlider->setSliderSnapsToMousePosition(false);
    pvf->panSlider->setScrollWheelEnabled(false);
    pvf->panSlider->setMouseDragSensitivity(100);
    pvf->panSlider->setWantsKeyboardFocus(true);

    pvf->panSlider->textFromValueFunction =  [](double v) -> String { if (fabs(v) < 0.01) return String(TRANS("Pan: Center")); return String(TRANS("Pan: ")) +  String((int)rint(abs(v*100.0f))) + ((v > 0 ? "% R" : "% L")) ; };
    pvf->panSlider->valueFromTextFunction =  [](const String& s) -> double { return s.getDoubleValue()*1e-2f; };
    pvf->panSlider->setValue(0.1, dontSendNotification);
    pvf->panSlider->setValue(0.0, dontSendNotification);
    pvf->panSlider->setLookAndFeel(&pvf->panSliderLNF);

    pvf->widthSlider = std::make_unique<Slider>(Slider::RotaryHorizontalVerticalDrag,  Slider::NoTextBox);
    pvf->widthSlider->setTitle(TRANS("Width"));
    pvf->widthSlider->setName("width");
    pvf->widthSlider->addListener(this);
    pvf->widthSlider->setLookAndFeel(&pvf->panSliderLNF);
    // Knobs instead of the pan slider: player rows, and the Input Mixer (a little smaller there)
    configKnob(pvf, pvf->panSlider.get(), false);
    configKnob(pvf, pvf->widthSlider.get(), true);
    if (!mPeerMode) {
        pvf->panSlider->getProperties().set ("knobSize", kMixerKnob);
        pvf->widthSlider->getProperties().set ("knobSize", kMixerKnob);
        configMonitorKnob(pvf->monitorSlider.get());
    }

    std::unique_ptr<Drawable> destimg(Drawable::createFromImageData(BinaryData::chevron_forward_svg, BinaryData::chevron_forward_svgSize));
    std::unique_ptr<Drawable> linkimg(Drawable::createFromImageData(BinaryData::link_svg, BinaryData::link_svgSize));
    std::unique_ptr<Drawable> trirightimg(Drawable::createFromImageData(BinaryData::expand_arrow_inactive_svg, BinaryData::expand_arrow_inactive_svgSize));
    std::unique_ptr<Drawable> triimg(Drawable::createFromImageData(BinaryData::expand_arrow_active_svg, BinaryData::expand_arrow_active_svgSize));

    pvf->linkButton = std::make_unique<SonoDrawableButton>("", DrawableButton::ButtonStyle::ImageRightOfTextLabel);


    pvf->linkButton->setColour(SonoTextButton::outlineColourId, Colours::transparentBlack);
    pvf->linkButton->setColour(DrawableButton::backgroundOnColourId, Colour::fromFloatRGBA(0.4, 0.35, 0.4, 0.7));
    pvf->linkButton->setColour(DrawableButton::backgroundColourId, Colour::fromFloatRGBA(0.2, 0.2, 0.2, 0.6));
    pvf->linkButton->setClickingTogglesState(false);
    pvf->linkButton->setTriggeredOnMouseDown(false);
    pvf->smallLnf.drawableTextJustification = Justification::centredLeft;   // "3 › BH Talkback" starts at the left edge, no centring gap
    pvf->linkButton->setLookAndFeel(&pvf->smallLnf);
    pvf->linkButton->addListener(this);
    pvf->linkButton->setForegroundImageRatio(0.35f);


    //pvf->linkButton->setAlpha(0.6f);
    if (mPeerMode) {
        pvf->linkButton->setTooltip(TRANS("Change channel layout"));
        pvf->linkButton->setTitle(TRANS("Channel Layout"));

        pvf->nameLabel->setInterceptsMouseClicks(true, false);
        pvf->nameLabel->addMouseListener(this, false);
    } else {
        // Input rows — no arrow glyph at all, the button is just "3 › BH Talkback" (still clickable)
        pvf->linkButton->setImages(nullptr);
        pvf->linkButton->setForegroundImageRatio(0.0f);
        pvf->linkButton->setTooltip(TRANS("Select Input channel source (or drag to rearrange)"));
        pvf->linkButton->setTitle(TRANS("Input Source"));
        pvf->linkButton->addMouseListener(this, false);
        pvf->nameLabel->addMouseListener(this, false);
        pvf->nameEditor->addMouseListener(this, false);
    }

    if (mPeerMode) {
        if (!first) {
            pvf->linkButton->setForegroundImageRatio(0.4f);
            pvf->linkButton->setImages(linkimg.get());
        }
        else {
            // expand button
            pvf->linkButton->setButtonStyle(DrawableButton::ButtonStyle::ImageLeftOfTextLabel);
            pvf->linkButton->setForegroundImageRatio(0.35f);
            pvf->linkButton->setImages(trirightimg.get(), nullptr, nullptr, nullptr,triimg.get());
        }
    }
    else {
        pvf->linkButton->setImages(destimg.get());
    }



    pvf->monoButton = std::make_unique<SonoDrawableButton>("", DrawableButton::ButtonStyle::ImageRightOfTextLabel);
    pvf->monoButton->setForegroundImageRatio(0.1f);
    pvf->monoButton->setColour(SonoTextButton::outlineColourId, Colours::transparentBlack);
    pvf->monoButton->setColour(DrawableButton::backgroundOnColourId, Colour::fromFloatRGBA(0.4, 0.35, 0.4, 0.7));
    pvf->monoButton->setColour(DrawableButton::backgroundColourId, Colour::fromFloatRGBA(0.2, 0.2, 0.2, 0.6));
    pvf->monoButton->setLookAndFeel(&pvf->smallLnf);
    //pvf->linkButton->addListener(this);
    //pvf->linkButton->addMouseListener(this, false);
    pvf->monoButton->setAlpha(0.8f);
    pvf->monoButton->setButtonText(TRANS("mono"));
    pvf->monoButton->setEnabled(false);

    pvf->destButton = std::make_unique<SonoDrawableButton>("", DrawableButton::ButtonStyle::ImageLeftOfTextLabel);

    pvf->destButton->setImages(destimg.get());

    pvf->destButton->setForegroundImageRatio(0.0f);
    pvf->destButton->getProperties().set ("leadText", TRANS("Output"));
    //pvf->destButton->setColour(SonoTextButton::outlineColourId, Colour::fromFloatRGBA(0.4, 0.4, 0.4, 0.6));
    //pvf->destButton->setColour(DrawableButton::backgroundOnColourId, Colour::fromFloatRGBA(0.4, 0.35, 0.4, 0.7));
    pvf->destButton->setColour(DrawableButton::backgroundColourId, Colour::fromFloatRGBA(0.2, 0.2, 0.2, 0.6));
    pvf->destButton->setLookAndFeel(&pvf->smallLnf);
    pvf->destButton->addListener(this);
    if (mPeerMode) {
        pvf->destButton->setTooltip(TRANS("Choose destination output channels"));
        pvf->destButton->setTitle(TRANS("Output Destination"));
    } else {
        pvf->destButton->setTooltip(TRANS("Choose destination monitoring channels"));
        pvf->destButton->setTitle(TRANS("Monitor Destination"));
    }


    

#if JUCE_IOS
    //pvf->panSlider1->setPopupDisplayEnabled(true, false, pvf->pannersContainer.get());
    //pvf->panSlider2->setPopupDisplayEnabled(true, false, pvf->pannersContainer.get());
#endif

    pvf->fxButton = std::make_unique<TextButton>(TRANS("FX"));
    pvf->fxButton->setColour(TextButton::buttonOnColourId, Colour::fromFloatRGBA(0.2, 0.5, 0.7, 0.5));
    pvf->fxButton->addListener(this);
    pvf->fxButton->setLookAndFeel(&pvf->medLnf);
    if (!mPeerMode) {
        pvf->fxButton->setTooltip(TRANS("Edit input effects (applied before sending)"));
        pvf->fxButton->setTitle(TRANS("Input Effects"));
    } else {
        pvf->fxButton->setTooltip(TRANS("Edit effects"));
        pvf->fxButton->setTitle(TRANS("Effects"));
    }

    pvf->monfxButton = std::make_unique<TextButton>(TRANS("M.FX"));
    pvf->monfxButton->setColour(TextButton::buttonOnColourId, Colour::fromFloatRGBA(0.2, 0.5, 0.7, 0.5));
    pvf->monfxButton->addListener(this);
    pvf->monfxButton->setLookAndFeel(&pvf->medLnf);
    if (!mPeerMode) {
        pvf->monfxButton->setTooltip(TRANS("Edit input monitoring effects (applied to local monitoring only)"));
        pvf->monfxButton->setTitle(TRANS("Input Monitoring Effects"));
    } else {
        pvf->monfxButton->setTooltip(TRANS("Edit monitoring effects"));
        pvf->monfxButton->setTitle(TRANS("Monitoring Effects"));
    }

    

    // meters
    auto flags = foleys::LevelMeter::Minimal /* | foleys::LevelMeter::SingleChannel */;
    
    // Cache the static meter layers (bar/clip backgrounds below, tick marks above) as images; only the level bars are
    // vector-drawn per refresh. Rate gating: 8 Hz while the view is active and the window is showing, else 0.
    const int meterRate = (metersActive && !metersSuspended) ? 8 : 0;

    pvf->meter = std::make_unique<foleys::LevelMeter>(flags);
    pvf->meter->setLookAndFeel(&(pvf->rmeterLnf));
    pvf->meter->setUseBackgroundImage(true);
    pvf->meter->setRefreshRateHz(meterRate);
    pvf->meter->addMouseListener(this, false);

    if (!mPeerMode) {
        pvf->premeter = std::make_unique<foleys::LevelMeter>(flags);
        pvf->premeter->setLookAndFeel(&(pvf->rmeterLnf));
        pvf->premeter->setUseBackgroundImage(true);
        pvf->premeter->setRefreshRateHz(meterRate);
        pvf->premeter->addMouseListener(this, false);
        pvf->premeter2 = std::make_unique<foleys::LevelMeter>(flags);
        pvf->premeter2->setLookAndFeel(&(pvf->rmeterLnf));
        pvf->premeter2->setUseBackgroundImage(true);
        pvf->premeter2->setRefreshRateHz(meterRate);
        pvf->premeter2->addMouseListener(this, false);

        // Captions over the meters; hidden until the input layout shows them
        pvf->inCaption = std::make_unique<Label>("incap", TRANS("IN"));
        pvf->outCaption = std::make_unique<Label>("outcap", TRANS("OUT"));
        for (Label * cap : { pvf->inCaption.get(), pvf->outCaption.get() }) {
            cap->setFont(9);
            cap->setColour(Label::textColourId, dimTextColor);
            cap->setJustificationType(Justification::centredBottom);
            cap->setBorderSize(BorderSize<int>(0));
            cap->setInterceptsMouseClicks(false, false);
            cap->setAccessible(false);
        }
    }

    // Every label, button and slider in the row is buffered to an image, because the adjacent meters dirty their
    // neighbourhood 8 times a second. Not buffered: meter/premeter (animated), nameEditor (TextEditor
    // caret/selection), and the row itself (would freeze its child meters).
    for (Component* c : std::initializer_list<Component*>{
            pvf->nameLabel.get(), pvf->muteButton.get(), pvf->soloButton.get(),
            pvf->chanLabel.get(), pvf->levelSlider.get(), pvf->monitorSlider.get(),
            pvf->panLabel.get(), pvf->panSlider.get(), pvf->widthSlider.get(), pvf->linkButton.get(),
            pvf->monoButton.get(), pvf->destButton.get(), pvf->fxButton.get(),
            pvf->monfxButton.get(), pvf->inCaption.get(), pvf->outCaption.get() }) {
        if (c) c->setBufferedToImage(true);
    }

    return pvf;
}

void ChannelGroupsView::visibilityChanged()
{
    DBG("ChannelGroupsView visibility changed: " << (int) isVisible());
    setMetersActive(isVisible());
}


void ChannelGroupsView::setMetersSuspended(bool flag)
{
    if (metersSuspended == flag) return;
    metersSuspended = flag;
    setMetersActive(metersActive);
}

void ChannelGroupsView::setSendMutedAppearance(bool flag)
{
    mSendMutedAppearance = flag;
    const float alpha = flag ? 0.35f : 1.0f;
    for (auto * pvf : mChannelViews) {
        if (pvf->meter) pvf->meter->setAlpha(alpha);
        if (pvf->premeter) pvf->premeter->setAlpha(alpha);
        if (pvf->premeter2) pvf->premeter2->setAlpha(alpha);
    }
}

void ChannelGroupsView::setMetersActive(bool flag)
{
    // Visible rate is 8 Hz; suspended (window minimised or not showing) forces 0 regardless of layout visibility.
    float rate = (flag && !metersSuspended) ? 8.0f : 0.0f;
    float subrate = mPeerMode && !processor.getRemotePeerViewExpanded(mPeerIndex) ? 0.0f : rate;
    metersActive = flag;


    if (mMainChannelView) {
        mMainChannelView->meter->setRefreshRateHz(rate);
    }

    if (mMetChannelView) {
        mMetChannelView->meter->setRefreshRateHz(rate);
    }
    if (mFileChannelView) {
        mFileChannelView->meter->setRefreshRateHz(rate);
    }
    if (mSoundboardChannelView) {
        mSoundboardChannelView->meter->setRefreshRateHz(rate);
    }


    for (int i=0; i < mChannelViews.size(); ++i) {
        ChannelGroupView * pvf = mChannelViews.getUnchecked(i);
        if (pvf->meter) {
            pvf->meter->setRefreshRateHz(subrate);
        }
        if (pvf->premeter) {
            pvf->premeter->setRefreshRateHz(subrate);
            if (pvf->premeter2) pvf->premeter2->setRefreshRateHz(subrate);
        }
    }
}



void ChannelGroupsView::rebuildChannelViews(bool notify)
{
    int numchans = 0;

    if (mPeerMode) {
        numchans = jmax(1, processor.getRemotePeerRecvChannelCount(mPeerIndex));

        if (!mMainChannelView) {
            mMainChannelView.reset(createChannelGroupView(true));
            mMainChannelView->linkButton->setClickingTogglesState(true);
            //mMainChannelView->nameLabel->setColour(TextEditor::backgroundColourId, Colours::transparentBlack);
            //mMainChannelView->nameLabel->setColour(TextEditor::outlineColourId, Colours::transparentBlack);
            mMainChannelView->nameLabel->setWantsKeyboardFocus(true);

            mMainChannelView->linkButton->onClick = [this]() {
                processor.setRemotePeerViewExpanded(mPeerIndex, mMainChannelView->linkButton->getToggleState());
                updateChannelViews();
                updateLayout();
                setMetersActive(metersActive);
                resized();
            };

            mMainChannelView->soloButton->onClick = [this]() {
                // Info-only and HostedMix peers route solo through the hub-mix control channel, per-(L,S) gated. JUCE
                // has already flipped the toggle. The hub mix loop gates non-soloed feeds when a listener has any
                // solo set.
                if (processor.getRemotePeerInfoOnly(mPeerIndex) || processor.isHostedMixModeEnabled()) {
                    bool newsolo = mMainChannelView->soloButton->getToggleState();
                    String name = processor.getRemotePeerUserName(mPeerIndex);
                    processor.setHubMixSoloForPeer(name, newsolo);
                    updateChannelViews();
                    return;
                }
                if (ModifierKeys::currentModifiers.isAltDown()) {
                    // exclusive solo this one

                    bool newsolo = mMainChannelView->soloButton->getToggleState();

                    for (int j=0; j < processor.getNumberRemotePeers(); ++j) {
                        if (newsolo) {
                            processor.setRemotePeerSoloed(j, mPeerIndex == j);
                        }
                        else {
                            processor.setRemotePeerSoloed(j, false);
                        }
                    }


                    // disable solo for main monitor too
                    processor.getValueTreeState().getParameter(SonobusAudioProcessor::paramMainMonitorSolo)->setValueNotifyingHost(0.0);

                    updateChannelViews();
                } else {
                    bool newsolo = mMainChannelView->soloButton->getToggleState();

                    processor.setRemotePeerSoloed(mPeerIndex, newsolo);

                    updateChannelViews();
                }
            };

            mMainChannelView->muteButton->onClick = [this]() {

                bool newmute = mMainChannelView->muteButton->getToggleState();

                // Info-only rows route mute through the hub's mix control channel: they have no local audio plumbing,
                // so the recvActive/recvAllow path is a no-op. HostedMix peers use the same OSC path: they are real
                // RemotePeer entries, but their audio arrives via hub-allocated per-(L,S) feeds that the local recv
                // path does not mute.
                if (processor.getRemotePeerInfoOnly(mPeerIndex) || processor.isHostedMixModeEnabled()) {
                    String name = processor.getRemotePeerUserName(mPeerIndex);
                    processor.setHubMixMuteForPeer(name, newmute);
                    updateChannelViews();
                    return;
                }

                if (!newmute) {
                    // allows receiving and invites
                    processor.setRemotePeerRecvActive(mPeerIndex, true);
                } else {
                    // turns off receiving and allow
                    processor.setRemotePeerRecvAllow(mPeerIndex, false);
                }

                updateChannelViews();
            };


            mMainChannelView->fxButton->onClick = [this]() {
                if (!effectsCalloutBox) {
                    showEffects(0, true, mMainChannelView->fxButton.get());
                } else {
                    showEffects(0, false);
                }
            };

            mMainChannelView->monfxButton->onClick = [this]() {
                if (!monEffectsCalloutBox) {
                    showMonitorEffects(0, true, mMainChannelView->monfxButton.get());
                } else {
                    showMonitorEffects(0, false);
                }
            };

            mMainChannelView->destButton->onClick = [this]() {
                // when shown it will be for the first one
                showDestSelectionMenu(mMainChannelView->destButton.get(), 0);
            };

        }
    } else {
        int changroups = processor.getInputGroupCount();
        numchans = 0;
        for (int i=0; i < changroups; ++i) {
            int chst, chcnt=0;
            processor.getInputGroupChannelStartAndCount(i, chst, chcnt);
            numchans += chcnt;
        }

        if (!mMetChannelView) {
            mMetChannelView.reset(createChannelGroupView(true));
            mMetChannelView->nameLabel->setText(TRANS("Metronome"), dontSendNotification);
            mMetChannelView->nameLabel->setEditable(false);
            mMetChannelView->nameLabel->setColour(Label::backgroundColourId, Colours::transparentBlack);
            mMetChannelView->nameLabel->setColour(Label::outlineColourId, Colours::transparentBlack);
            //mMetChannelView->nameLabel->setReadOnly(true);
            //mMetChannelView->nameLabel->setColour(TextEditor::backgroundColourId, Colours::transparentBlack);
            //mMetChannelView->nameLabel->setColour(TextEditor::outlineColourId, Colours::transparentBlack);

            mMetLevelAttachment   = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (processor.getValueTreeState(), SonobusAudioProcessor::paramMetGain, *mMetChannelView->levelSlider);


            std::unique_ptr<Drawable> grpimg(Drawable::createFromImageData(BinaryData::send_group_small_svg, BinaryData::send_group_small_svgSize));
            mMetChannelView->linkButton->setButtonStyle(DrawableButton::ButtonStyle::ImageOnButtonBackground);
            mMetChannelView->linkButton->setTitle(TRANS("Send Metronome"));
            mMetChannelView->linkButton->setImages(grpimg.get());
            mMetChannelView->linkButton->setClickingTogglesState(true);
            mMetSendAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (processor.getValueTreeState(), SonobusAudioProcessor::paramSendMetAudio, *mMetChannelView->linkButton);
            mMetChannelView->linkButton->setForegroundImageRatio(1.0f);
            mMetChannelView->linkButton->setColour(TextButton::buttonOnColourId, Colour::fromFloatRGBA(0.2, 0.5, 0.7, 0.65));
            mMetChannelView->linkButton->setColour(TextButton::buttonColourId, Colours::transparentBlack);
            mMetChannelView->linkButton->setTooltip(TRANS("Send Metronome to All"));



            mMetChannelView->soloButton->onClick = [this]() {
            };

            mMetChannelView->muteButton->onClick = [this]() {
            };


            mMetChannelView->fxButton->onClick = [this]() {
                /*
                if (!effectsCalloutBox) {
                    showEffects(0, true, mMetChannelView->fxButton.get());
                } else {
                    showEffects(0, false);
                }
                 */
            };

            mMetChannelView->monfxButton->onClick = [this]() {
                if (!monEffectsCalloutBox) {
                    showMonitorEffects(-1, true, mMetChannelView->monfxButton.get());
                } else {
                    showMonitorEffects(-1, false);
                }
            };

            mMetChannelView->destButton->onClick = [this]() {
                // when shown it will be for the first one
                showDestSelectionMenu(mMetChannelView->destButton.get(), -1);
            };

            //mMetChannelView->levelSlider->onValueChange = [this]() {
            //    processor.setMetronomeGain(mMetChannelView->levelSlider->getValue());
            //};

            mMetChannelView->panSlider->onValueChange = [this]() {
                processor.setMetronomePan(mMetChannelView->panSlider->getValue());
            };

            mMetChannelView->monitorSlider->onValueChange = [this]() {
                processor.setMetronomeMonitor(mMetChannelView->monitorSlider->getValue());
            };

            setupChildren(mMetChannelView.get());

            mMetChannelView->muteButton->setVisible(false);
            mMetChannelView->soloButton->setVisible(false);
            mMetChannelView->fxButton->setVisible(false);
            mMetChannelView->premeter->setVisible(false);
            mMetChannelView->premeter->setRefreshRateHz(0);
            //mMetChannelView->premeter->setVisible(false);
            //mMetChannelView->premeter->setRefreshRateHz(0);

        }

        if (!mFileChannelView) {
            mFileChannelView.reset(createChannelGroupView(true));
            mFileChannelView->nameLabel->setEditable(false);
            mFileChannelView->nameLabel->setText(TRANS("File Playback"), dontSendNotification);
            mFileChannelView->nameLabel->setColour(Label::backgroundColourId, Colours::transparentBlack);
            mFileChannelView->nameLabel->setColour(Label::outlineColourId, Colours::transparentBlack);
            //mFileChannelView->nameLabel->setColour(TextEditor::backgroundColourId, Colours::transparentBlack);
            //mFileChannelView->nameLabel->setColour(TextEditor::outlineColourId, Colours::transparentBlack);
            //mFileChannelView->nameLabel->setReadOnly(true);

            mFileChannelView->linkButton->setClickingTogglesState(true);
            std::unique_ptr<Drawable> grpimg(Drawable::createFromImageData(BinaryData::send_group_small_svg, BinaryData::send_group_small_svgSize));
            mFileChannelView->linkButton->setTitle(TRANS("Send File Playback"));
            mFileChannelView->linkButton->setImages(grpimg.get());
            mFileChannelView->linkButton->setClickingTogglesState(true);
            mFileSendAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (processor.getValueTreeState(), SonobusAudioProcessor::paramSendFileAudio, *mFileChannelView->linkButton);
            mFileChannelView->linkButton->setButtonStyle(DrawableButton::ButtonStyle::ImageOnButtonBackground);
            mFileChannelView->linkButton->setForegroundImageRatio(1.0f);
            mFileChannelView->linkButton->setColour(TextButton::buttonOnColourId, Colour::fromFloatRGBA(0.2, 0.5, 0.7, 0.65));
            mFileChannelView->linkButton->setColour(TextButton::buttonColourId, Colours::transparentBlack);
            mFileChannelView->linkButton->setTooltip(TRANS("Send File Playback to All"));


            mFileChannelView->soloButton->onClick = [this]() {
            };

            mFileChannelView->muteButton->onClick = [this]() {
            };


            mFileChannelView->fxButton->onClick = [this]() {
                /*
                if (!effectsCalloutBox) {
                    showEffects(0, true, mFileChannelView->fxButton.get());
                } else {
                    showEffects(0, false);
                }
                 */
            };

            mFileChannelView->monfxButton->onClick = [this]() {
                if (!monEffectsCalloutBox) {
                    showMonitorEffects(-2, true, mFileChannelView->monfxButton.get());
                } else {
                    showMonitorEffects(-2, false);
                }
            };

            mFileChannelView->destButton->onClick = [this]() {
                // when shown it will be for the first one
                showDestSelectionMenu(mFileChannelView->destButton.get(), -2);
            };

            mFileChannelView->levelSlider->onValueChange = [this]() {
                processor.setFilePlaybackGain(mFileChannelView->levelSlider->getValue());
            };

            //mFileChannelView->panSlider->onValueChange = [this]() {
            //    processor.setFilePlaybackPan(mFileChannelView->panSlider->getValue());
            //};

            mFileChannelView->monitorSlider->onValueChange = [this]() {
                processor.setFilePlaybackMonitor(mFileChannelView->monitorSlider->getValue());
            };

            setupChildren(mFileChannelView.get());

            mFileChannelView->muteButton->setVisible(false);
            mFileChannelView->soloButton->setVisible(false);
            mFileChannelView->fxButton->setVisible(false);
            mFileChannelView->panSlider->setVisible(false);

            mFileChannelView->premeter->setVisible(false);
            mFileChannelView->premeter->setRefreshRateHz(0);
            //mFileChannelView->premeter->setVisible(false);
            //mFileChannelView->premeter->setRefreshRateHz(0);

        }
        if (!mSoundboardChannelView) {
            mSoundboardChannelView.reset(createChannelGroupView(true));
            mSoundboardChannelView->nameLabel->setEditable(false);
            mSoundboardChannelView->nameLabel->setText(TRANS("Soundboard"), dontSendNotification);
            mSoundboardChannelView->nameLabel->setColour(Label::backgroundColourId, Colours::transparentBlack);
            mSoundboardChannelView->nameLabel->setColour(Label::outlineColourId, Colours::transparentBlack);

            mSoundboardChannelView->linkButton->setClickingTogglesState(true);
            std::unique_ptr<Drawable> grpimg(Drawable::createFromImageData(BinaryData::send_group_small_svg, BinaryData::send_group_small_svgSize));
            mSoundboardChannelView->linkButton->setTitle(TRANS("Send Soundboard"));
            mSoundboardChannelView->linkButton->setImages(grpimg.get());
            mSoundboardChannelView->linkButton->setClickingTogglesState(true);
            mSoundboardSendAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (processor.getValueTreeState(), SonobusAudioProcessor::paramSendSoundboardAudio, *mSoundboardChannelView->linkButton);
            mSoundboardChannelView->linkButton->setButtonStyle(DrawableButton::ButtonStyle::ImageOnButtonBackground);
            mSoundboardChannelView->linkButton->setForegroundImageRatio(1.0f);
            mSoundboardChannelView->linkButton->setColour(TextButton::buttonOnColourId, Colour::fromFloatRGBA(0.2, 0.5, 0.7, 0.65));
            mSoundboardChannelView->linkButton->setColour(TextButton::buttonColourId, Colours::transparentBlack);
            mSoundboardChannelView->linkButton->setTooltip(TRANS("Send Soundboard to All"));

            mSoundboardChannelView->soloButton->onClick = []() {};
            mSoundboardChannelView->muteButton->onClick = []() {};
            mSoundboardChannelView->fxButton->onClick = []() {};

            mSoundboardChannelView->monfxButton->onClick = [this]() {
               if (!monEffectsCalloutBox) {
                   showMonitorEffects(-3, true, mSoundboardChannelView->monfxButton.get());
               }
               else {
                   showMonitorEffects(-3, false);
               }
            };

            mSoundboardChannelView->destButton->onClick = [this]() {
                // when shown it will be for the first one
                showDestSelectionMenu(mSoundboardChannelView->destButton.get(), -3);
            };

            mSoundboardChannelView->levelSlider->onValueChange = [this]() {
                processor.getSoundboardProcessor()->setGain(mSoundboardChannelView->levelSlider->getValue());
            };

            //mSoundboardChannelView->panSlider->onValueChange = [this]() {
            //    processor.setSoundboardPlaybackPan(mSoundboardChannelView->panSlider->getValue());
            //};

            mSoundboardChannelView->monitorSlider->onValueChange = [this]() {
                processor.getSoundboardProcessor()->setMonitorGain(mSoundboardChannelView->monitorSlider->getValue());
            };

            setupChildren(mSoundboardChannelView.get());

            mSoundboardChannelView->muteButton->setVisible(false);
            mSoundboardChannelView->soloButton->setVisible(false);
            mSoundboardChannelView->fxButton->setVisible(false);
            mSoundboardChannelView->panSlider->setVisible(false);

            mSoundboardChannelView->premeter->setVisible(false);
            mSoundboardChannelView->premeter->setRefreshRateHz(0);
            //mSoundboardChannelView->premeter->setVisible(false);
            //mSoundboardChannelView->premeter->setRefreshRateHz(0);
        }

        mMetFileBg->setVisible(true);
    }

    while (mChannelViews.size() < numchans) {
        mChannelViews.add(createChannelGroupView(!mPeerMode && mChannelViews.size() == 0));
    }
    while (mChannelViews.size() > numchans) {
        mChannelViews.removeLast();
    }
    if (mSendMutedAppearance) setSendMutedAppearance(true);   // rows added above start dimmed


    for (int i= (mPeerMode ? -1 : 0); i < mChannelViews.size(); ++i) {
        ChannelGroupView * pvf = i < 0 ? mMainChannelView.get() : mChannelViews.getUnchecked(i);

        setupChildren(pvf);
    }
    
    updateChannelViews();
    updateLayout(notify);
    resized();
}

void ChannelGroupsView::setupChildren(ChannelGroupView * pvf)
{
    Component* dw = this->findParentComponentOfClass<AudioProcessorEditor>();
    if (!dw) dw = this->findParentComponentOfClass<Component>();
    if (!dw) dw = this;

    pvf->addAndMakeVisible(pvf->linkButton.get());
    pvf->addChildComponent(pvf->monoButton.get());
    pvf->addAndMakeVisible(pvf->destButton.get());
    pvf->addAndMakeVisible(pvf->muteButton.get());
    pvf->addAndMakeVisible(pvf->soloButton.get());
    pvf->addAndMakeVisible(pvf->levelSlider.get());
    pvf->addChildComponent(pvf->monitorSlider.get());
    //pvf->addAndMakeVisible(pvf->levelLabel.get());
    pvf->addChildComponent(pvf->panLabel.get());   // the knobs carry their own caption
    pvf->addAndMakeVisible(pvf->nameLabel.get());
    pvf->addAndMakeVisible(pvf->chanLabel.get());
    pvf->addChildComponent(pvf->nameEditor.get());

    pvf->addAndMakeVisible(pvf->meter.get());
    if (pvf->premeter) {
        pvf->addAndMakeVisible(pvf->premeter.get());
        if (pvf->premeter2) pvf->addChildComponent(pvf->premeter2.get());
    }
    pvf->addAndMakeVisible(pvf->fxButton.get());

    pvf->addAndMakeVisible(pvf->monfxButton.get());

    pvf->addAndMakeVisible(pvf->panSlider.get());
    pvf->addChildComponent(pvf->widthSlider.get());

    if (pvf->inCaption) pvf->addChildComponent(pvf->inCaption.get());
    if (pvf->outCaption) pvf->addChildComponent(pvf->outCaption.get());

    // A player's header row shows the knob values under the knobs, so no value bubble there
    const bool headerKnobs = (mPeerMode && pvf == mMainChannelView.get()) || !mPeerMode;   // the Input Mixer's too
    pvf->panSlider->setPopupDisplayEnabled(!headerKnobs, !headerKnobs, dw);
    pvf->widthSlider->setPopupDisplayEnabled(!headerKnobs, !headerKnobs, dw);

    pvf->monitorSlider->setPopupDisplayEnabled(mPeerMode, mPeerMode, dw);

    addAndMakeVisible(pvf);
}

void ChannelGroupsView::updateLayoutForRemotePeer(bool notify)
{
    int minitemheight = 34 ;
    int mincheckheight = 32;
    int minButtonWidth = 60;
    int meterwidth = kSbMeterPxPerChannel;
    int mainmeterwidth = kSbMeterPxPerChannel;
    int destbuttwidth = 44;
    int monsliderwidth =  0 ;
    int addrowheight = minitemheight - 2;

#if JUCE_IOS || JUCE_ANDROID
    // make the button heights a bit more for touchscreen purposes
    minitemheight = 42;
    mincheckheight = 40;
#endif

    const int textheight = minitemheight / 2;

    bool servconnected = processor.isConnectedToServer();

    mLayoutChanged = true;
    channelsBox.items.clear();
    channelsBox.flexDirection = FlexBox::Direction::column;
    channelsBox.justifyContent = FlexBox::JustifyContent::flexStart;
    int peersheight = 0;
    //const int singleph =  minitemheight*3 + 12;
    const int singleph =  minitemheight;

    //channelsBox.items.add(FlexItem(8, 2).withMargin(0));
    //peersheight += 2;

    const int ph = singleph;
    int mbh = 0;


    int chi = 0;
    int changroup = 0;
    int sendcnt = 0;
    int totalchans = 0;
    int changroups = 0;

    int chstart = 0;
    int chcnt = 1;

    int deststart = 0;
    int destcnt = 1;

    int totaloutchans = 1;
    totaloutchans = processor.getTotalNumOutputChannels();

    int rowMinWidth = 0;   // Widest line at its smallest (names and output labels given way)

    changroups = processor.getRemotePeerChannelGroupCount(mPeerIndex);

    totalchans = processor.getRemotePeerRecvChannelCount(mPeerIndex);
    // Info-only peers report 0 channels (no audio plumbing on this side), which collapses the main meter to zero
    // width. Use 2 so the meter has somewhere to render.
    if (totalchans <= 0 && processor.getRemotePeerInfoOnly(mPeerIndex)) {
        totalchans = 2;
    }

    processor.getRemotePeerChannelGroupStartAndCount(mPeerIndex, changroup, chstart, chcnt);
    processor.getRemotePeerChannelGroupDestStartAndCount(mPeerIndex, changroup, deststart, destcnt);
    destcnt = jmin(totaloutchans, destcnt);

    // Every meter has the bottom meters' bars: one fixed pitch per channel, so more channels make a wider meter
    mainmeterwidth = kSbMeterPxPerChannel * jmax(1, totalchans);


    // Main connected peer views


    for (int i = -1; i < mChannelViews.size(); ++i, ++chi) {
        if (i==0) {
            chi = 0; // ensure this
        }

        if (chi >= chcnt && changroup < changroups-1) {
            changroup++;
            processor.getRemotePeerChannelGroupStartAndCount(mPeerIndex, changroup, chstart, chcnt);
            processor.getRemotePeerChannelGroupDestStartAndCount(mPeerIndex, changroup, deststart, destcnt);
            destcnt = jmin(totaloutchans, destcnt);
            chi = 0;
        }

        ChannelGroupView * pvf = i < 0 ? mMainChannelView.get() : mChannelViews.getUnchecked(i);

        //pvf->updateLayout();
        bool viewexpanded = processor.getRemotePeerViewExpanded(mPeerIndex);

        if (!viewexpanded && i >= 0) {
            // skip this one, not expanded
            continue;
        }
        if (i >= 0 && i < mChannelViews.size() && chcnt == 2 && chi == 1) continue;   // a stereo pair is one row

        if (i < 0) {
            // The player's header is laid out by hand (ChannelGroupView::layoutHeader) in the form the list chose
            auto & h = pvf->header;
            h.active = true;
            h.form = mRowForm;
            h.nameCol = mRowNameCol;
            h.destCol = mRowDestCol;
            h.knobCol = mRowKnobCol;
            h.statW = mRowStatWidth;
            h.buttonW = mRowButtonWidth;
            h.meterSlack = mRowMeterSlack;
            h.showDest = !viewexpanded;
            h.destOwn = destWidthFor(pvf, destbuttwidth);
            h.showMeter = !mHideRowMeter;
            h.meterW = mainmeterwidth;
            pvf->meter->setVisible(h.showMeter);

            // The narrowest form's need, whatever the form is now, so the list's minimum width does not depend on it
            const int headerMinWidth = jmax(ChannelGroupView::kHeaderIndent + 60 + 6 + mainmeterwidth + 2,
                                            90 + 6 + mRowKnobCol + 2,
                                            mRowStatWidth + 8 + 3 * kHeaderButtonMin + 6 + 2);
            mbh = ChannelGroupView::headerHeight(h);
            channelsBox.items.add(FlexItem(headerMinWidth, mbh, *pvf).withMargin(0).withFlex(0));
            rowMinWidth = jmax(rowMinWidth, headerMinWidth);
            peersheight += mbh;

            if (i < mChannelViews.size()-1) {
                channelsBox.items.add(FlexItem(3, 4));
                peersheight += 4;
            }
            continue;
        }

        // An instrument row has the player's columns (name, level, knobs, output) in the same form, so its level
        // slider starts and ends where the player rows' do; MUTE, SOLO and FX on its lower line. The further
        // channels of an instrument with more than two have one line: the channel number, its knob and meter.
        const bool primary = chi == 0;
        auto & h = pvf->header;
        h.active = true;
        h.form = mRowForm;
        h.nameCol = mRowNameCol;
        h.destCol = mRowDestCol;
        h.knobCol = mRowKnobCol;
        h.statW = 0;
        h.buttonW = mRowButtonWidth;
        h.meterSlack = mRowMeterSlack;
        h.showDest = primary;
        h.destOwn = destWidthFor(pvf, destbuttwidth);
        h.showMeter = !mHideRowMeter;
        h.meterW = meterwidth;
        h.meterCol = mainmeterwidth;   // an inline meter keeps the player's meter column, so the controls line up
        h.lower = primary;
        pvf->meter->setVisible(h.showMeter);

        const int subMinWidth = jmax(ChannelGroupView::kHeaderIndent + 60 + 6 + meterwidth + 2,
                                     90 + 6 + mRowKnobCol + 2,
                                     3 * kHeaderButtonMin + 6 + 2);
        mbh = ChannelGroupView::headerHeight(h);
        channelsBox.items.add(FlexItem(subMinWidth, mbh, *pvf).withMargin(0).withFlex(0));
        rowMinWidth = jmax(rowMinWidth, subMinWidth);
        peersheight += mbh;

        if (i < mChannelViews.size()-1) {
            channelsBox.items.add(FlexItem(3, 4));
            peersheight += 4;
        }

    }


    // for input mode add met and file playback rows
    if (!mPeerMode) {

    }


    if (isNarrow) {
        channelMinHeight = std::max(mbh,  peersheight);
        channelMinWidth = rowMinWidth;   // What the lines need at their smallest, so the row never lays them past its edge
    }
    else {
        if (!mPeerMode) {
            channelMinHeight = std::max(mbh + 18,  peersheight);
        } else {
            channelMinHeight = std::max(mbh,  peersheight);
        }
        channelMinWidth = rowMinWidth;
    }

    if (notify) {
        listeners.call (&ChannelGroupsView::Listener::channelLayoutChanged, this);
    }

}

// Sizes a level slider's value box to the longest text it can show, in the font it is drawn with; returns that width
static int fitLevelTextBox(Slider & slider)
{
    Label * box = nullptr;
    for (auto * c : slider.getChildren())
        if ((box = dynamic_cast<Label*>(c)) != nullptr) break;

    const double lo = slider.getMinimum(), hi = slider.getMaximum();
    int need = 0;
    for (double v : { lo, hi, jlimit(lo, hi, (double) Decibels::decibelsToGain(-99.9f)) }) {
        const String t = slider.getTextFromValue(v);
        need = jmax(need, box != nullptr ? SbTextFit::labelWidth(*box, t) : SbTextFit::textWidth(Font(12.0f), t) + 12);
    }
    if (slider.getTextBoxPosition() != Slider::NoTextBox && slider.getTextBoxWidth() != need)
        slider.setTextBoxStyle(slider.getTextBoxPosition(), ! slider.isTextBoxEditable(), need, slider.getTextBoxHeight());
    return need;
}

void ChannelGroupsView::updateLayoutForInput(bool notify)
{
    int minitemheight =  30;
    int mincheckheight = 32;
    int minButtonWidth = 60;
    int maxPannerWidth = 100;   // Narrower pan block → longer level slider, pan + routing sit next to the meter
    int compactMaxPannerWidth = 90;
    // Every bar has the bottom Out Level meter's pitch
    int meterwidth = kSbMeterPxPerChannel;
    int mainmeterwidth = kSbMeterPxPerChannel;
    int linkbuttwidth = 50;
    int linkminbuttwidth = 50;
    if (!mPeerMode) {   // Size the channel button to its longest "3 › BH Talkback" label, no wasted width
        int w = 0, wmin = 0;
        Font lf(14.0f);   // at least as wide as the face the drawable-button label is drawn with (textH caps at 14)
        const String sep = String::fromUTF8(" \xe2\x80\xba ");
        for (auto* cv : mChannelViews) if (cv && cv->linkButton) {
            const String t = cv->linkButton->getButtonText();
            w = jmax(w, lf.getStringWidth(t));
            // In a narrow window the device names may give way; the channel numbers stay (the hover has the names)
            const String head = t.contains(sep) ? t.upToFirstOccurrenceOf(sep, true, false) + String::fromUTF8("\xe2\x80\xa6") : t;
            wmin = jmax(wmin, lf.getStringWidth(head));
        }
        linkbuttwidth = jmax(50, w + 18);   // no arrow on input rows: text + padding
        linkminbuttwidth = jmin(linkbuttwidth, jmax(50, wmin + 18));
    }
    int destbuttwidth = 44;
    int monsliderwidth =  40;
    int addrowheight = minitemheight - 2;

#if JUCE_IOS || JUCE_ANDROID
    // make the button heights a bit more for touchscreen purposes
    minitemheight =  38;
    mincheckheight = 40;
#endif

    const int textheight = minitemheight / 2;

    bool servconnected = processor.isConnectedToServer();

    // What the captions and names need, measured in the fonts they are drawn with
    int buttonNeed = 0;          // MUTE, SOLO, FX, M.FX: one width for all four
    int nameNeed = 0;            // the name column: the fixed row names, and the group names up to a limit
    if (auto * mv = mMetChannelView.get()) {
        for (TextButton * b : { mv->muteButton.get(), mv->soloButton.get(), mv->fxButton.get(), mv->monfxButton.get() })
            if (b) buttonNeed = jmax(buttonNeed, SbTextFit::buttonWidth(*b, minitemheight));
    }
    for (auto * v : { mMetChannelView.get(), mFileChannelView.get(), mSoundboardChannelView.get() })
        if (v != nullptr && v->nameLabel) nameNeed = jmax(nameNeed, SbTextFit::labelWidth(*v->nameLabel));
    // Group names count up to a limit for the one-line decision; with room the column grows to the longest one
    int nameFull = 0;
    for (auto * v : mChannelViews)
        if (v != nullptr && v->nameLabel && v->chanIndex == 0) {
            const int w = SbTextFit::labelWidth(*v->nameLabel);
            nameNeed = jmax(nameNeed, jmin(160, w));
            nameFull = jmax(nameFull, jmin(320, w));
        }
    // The value over each level slider ("Pre Level: -INF dB"), its box as wide as its longest text
    int levelNeed = 0;
    for (auto * v : mChannelViews)
        if (v != nullptr && v->levelSlider && v->chanIndex == 0) levelNeed = jmax(levelNeed, fitLevelTextBox(*v->levelSlider));
    for (auto * v : { mMetChannelView.get(), mFileChannelView.get(), mSoundboardChannelView.get() })
        if (v != nullptr && v->levelSlider) levelNeed = jmax(levelNeed, fitLevelTextBox(*v->levelSlider));

    // One Output width for every row of the mixer, so the right-hand columns line up and no label is cut
    int sharedDestW = destbuttwidth;
    int sharedDestMinW = 0;
    for (auto * v : mChannelViews) if (v != nullptr && v->destButton) sharedDestW = jmax(sharedDestW, destWidthFor(v, destbuttwidth));
    for (auto * v : { mMetChannelView.get(), mFileChannelView.get(), mSoundboardChannelView.get() })
        if (v != nullptr && v->destButton) sharedDestW = jmax(sharedDestW, destWidthFor(v, destbuttwidth));
    for (auto * v : mChannelViews) if (v != nullptr && v->destButton) sharedDestMinW = jmax(sharedDestMinW, destMinWidthFor(v, 36));
    for (auto * v : { mMetChannelView.get(), mFileChannelView.get(), mSoundboardChannelView.get() })
        if (v != nullptr && v->destButton) sharedDestMinW = jmax(sharedDestMinW, destMinWidthFor(v, 36));
    sharedDestMinW = jlimit(36, sharedDestW, sharedDestMinW);

    // Meter columns: room for two input bars on every row when any group is a stereo pair, so the names line up
    int changroupsNow = processor.getInputGroupCount();
    bool anyPair = false;
    for (int g = 0; g < changroupsNow; ++g) {
        int st = 0, ct = 0;
        processor.getInputGroupChannelStartAndCount(g, st, ct);
        if (ct == 2) anyPair = true;
    }
    const int inMeterCol = anyPair ? 2 * meterwidth : meterwidth;
    const int fileMeterW = processor.getFilePlaybackMeterSource().getNumChannels() * meterwidth;
    const int sbMeterW = processor.getSoundboardProcessor()->getMeterSource().getNumChannels() * meterwidth;
    const int maxPostMeterW = jmax(anyPair ? 2 * meterwidth : meterwidth, fileMeterW, sbMeterW);

    // The header row: In Reverb, Monitor Delay and CLEAR sit together on the right, each as wide as its whole label
    auto labelWidth = [addrowheight](TextButton& b, int minW) {
        const Font f = b.getLookAndFeel().getTextButtonFont(b, addrowheight);
        return jmax(minW, (int) std::ceil(f.getStringWidthFloat(b.getButtonText())) + 20);
    };
    const int inRevW = labelWidth(*mInReverbButton, minButtonWidth);
    const int monDelayW = labelWidth(*mMonDelayButton, minButtonWidth);
    const int clearW = labelWidth(*mClearButton, isNarrow ? 42 : 52);

    // One line per row when the whole row fits with every label whole; otherwise two lines per row
    const int wideButtonW = jmax(52, buttonNeed);
    const int wideNameW = jmax(100, nameNeed);
    const int wideSliderW = jmax(100, levelNeed);
    const int wideInbox = linkbuttwidth + 3 + inMeterCol + 3 + wideNameW + 6 + wideButtonW + 3 + wideButtonW + 5 + wideButtonW + 3 + wideSliderW + 1;
    // Knob columns: PAN (and WIDTH when any input is a stereo pair), then MONITOR, the same on every row
    const int panCol = anyPair ? 2 * kMixerKnobW : kMixerKnobW;
    maxPannerWidth = panCol;
    monsliderwidth = kMixerMonW;
    const int wideMonMax = maxPannerWidth + wideButtonW + monsliderwidth + sharedDestW + 2 + 2;
    const int wideMonMin = maxPostMeterW + 4 + panCol + 3 + monsliderwidth + 2 + wideButtonW + 2 + sharedDestW + 2;
    const int oneLineRow = 3 + wideInbox + 2 + jmax(wideMonMax, wideMonMin);
    const bool narrow = isNarrow || (mEstimatedWidth > 13 && oneLineRow > mEstimatedWidth - 13 - 2);

    const int minSliderWidth = narrow ? jmax(90, levelNeed) : wideSliderW;
    const int mutebuttwidth = narrow ? jmax(42, buttonNeed) : wideButtonW;
    const int namewidth = narrow ? jmax(88, nameNeed) : wideNameW;

    channelsBox.items.clear();
    channelsBox.flexDirection = FlexBox::Direction::column;
    channelsBox.justifyContent = FlexBox::JustifyContent::flexStart;
    int peersheight = 0;
    //const int singleph =  minitemheight*3 + 12;
    const int singleph =  minitemheight;

    //channelsBox.items.add(FlexItem(8, 2).withMargin(0));
    //peersheight += 2;

    const int ph = singleph;
    int ipw = 0;//2*minButtonWidth + minSliderWidth + 15 + 32 + 60 + 20;
    int mpw = 0;// 2*minButtonWidth + minPannerWidth + 6 + 44;
    int iph = 0;
    int mph = 0;
    int mbh = 0;
    int rowsMinWidth = 0;   // the widest row at its smallest


    int chi = 0;
    int changroup = 0;
    int sendcnt = 0;
    int totalchans = 0;
    int changroups = 0;

    int chstart = 0;
    int chcnt = 1;

    int deststart = 0;
    int destcnt = 1;

    bool pannervisible = true;

    int totaloutchans = 1;

    int estwidth = mEstimatedWidth > 13 ? mEstimatedWidth - 13 : 320;


    sendcnt = (int) processor.getValueTreeState().getParameter(SonobusAudioProcessor::paramSendChannels)->convertFrom0to1( processor.getValueTreeState().getParameter(SonobusAudioProcessor::paramSendChannels)->getValue());

    if (sendcnt == 0) {
        sendcnt = processor.getTotalNumInputChannels(); // processor.getMainBusNumInputChannels();
    }

    totalchans = processor.getTotalNumInputChannels();
    changroups = processor.getInputGroupCount();

    totaloutchans = processor.getTotalNumOutputChannels();

    processor.getInputGroupChannelStartAndCount(changroup, chstart, chcnt);
    processor.getInputGroupChannelDestStartAndCount(changroup, deststart, destcnt);

    destcnt = jmin(totaloutchans, destcnt);
    if ((sendcnt != 2 && destcnt != 2) || chcnt == 0) {
        pannervisible = false;
    }


    // Main connected peer views

    addrowBox.items.clear();
    addrowBox.flexDirection = FlexBox::Direction::row;
    addrowBox.items.add(FlexItem(4, 2).withMargin(0));
    addrowBox.items.add(FlexItem(*mAddButton).withWidth(linkbuttwidth).withMinWidth(linkminbuttwidth).withHeight(addrowheight).withMinHeight(addrowheight).withMargin(0).withFlex(0));
    addrowBox.items.add(FlexItem(6, 2).withMargin(0).withFlex(1));
    addrowBox.items.add(FlexItem(inRevW, addrowheight, *mInReverbButton).withMargin(0).withFlex(0));
    addrowBox.items.add(FlexItem(4, 2).withMargin(0));
    addrowBox.items.add(FlexItem(monDelayW, addrowheight, *mMonDelayButton).withMargin(0).withFlex(0));
    addrowBox.items.add(FlexItem(4, 2).withMargin(0));
    addrowBox.items.add(FlexItem(clearW, addrowheight, *mClearButton).withMargin(0).withFlex(0));
    addrowBox.items.add(FlexItem(4, 2).withMargin(0));
    const int addrowMinWidth = 4 + linkminbuttwidth + 6 + inRevW + 4 + monDelayW + 4 + clearW + 4;

    int gaph = 6 ; //changroups > 0 ? 2 : 6;
    int bgaph = changroups > 0 ? 4 : 6;
    channelsBox.items.add(FlexItem(8, gaph).withMargin(0));
    channelsBox.items.add(FlexItem(100, addrowheight, addrowBox).withMargin(0).withFlex(0));
    channelsBox.items.add(FlexItem(8, bgaph).withMargin(0));
    peersheight += addrowheight + gaph + bgaph;

    // all the inputs, plus three extra (met and file playback and soundboard)
    for (int i =  0; i < mChannelViews.size() + 3; ++i, ++chi) {
        if (i==0) {
            chi = 0; // ensure this
        }

        if (i >= mChannelViews.size()) {
            // one of met or file playback
            chi = 0;
        }
        else if (chi >= chcnt && changroup < changroups-1) {
            changroup++;
            processor.getInputGroupChannelStartAndCount(changroup, chstart, chcnt);

            processor.getInputGroupChannelDestStartAndCount(changroup, deststart, destcnt);

            destcnt = jmin(totaloutchans, destcnt);
            if ((sendcnt != 2 && destcnt != 2) || chcnt == 0) {
                pannervisible = false;
            } else {
                pannervisible = true;
            }
            chi = 0;
        }

        bool ismetorfileorsoundboard = false;

        ChannelGroupView * pvf;
        if (i == mChannelViews.size()) {
            pvf = mMetChannelView.get();
            ismetorfileorsoundboard = true;
        } else if (i == mChannelViews.size() + 1) {
            pvf = mFileChannelView.get();
            auto numfilechan = processor.getFilePlaybackMeterSource().getNumChannels();
            mainmeterwidth = numfilechan * meterwidth;
            ismetorfileorsoundboard = true;
        }
        else if (i > mChannelViews.size()) {
            pvf = mSoundboardChannelView.get();
            auto numsoundboardchan = processor.getSoundboardProcessor()->getMeterSource().getNumChannels();
            mainmeterwidth = numsoundboardchan * meterwidth;
            ismetorfileorsoundboard = true;
        }
        else {
            pvf = mChannelViews.getUnchecked(i);
        }

        //pvf->updateLayout();
        bool viewexpanded = true;

        if (!viewexpanded && i >= 0) {
            // skip this one, not expanded
            continue;
        }
        if (i >= 0 && i < mChannelViews.size() && chcnt == 2 && chi == 1) continue;   // a stereo pair is one row


        bool destbuttvisible = true;

        //if (chi == 0 || !mPeerMode || (totalchans > 1) )
        {

            pvf->namebox.items.clear();
            pvf->namebox.flexDirection = FlexBox::Direction::row;
            //.withAlignSelf(FlexItem::AlignSelf::center));

            pvf->namebox.items.add(FlexItem(namewidth, minitemheight, *pvf->nameLabel).withMargin(0).withFlex(1));

            pvf->inbox.items.clear();
            pvf->inbox.flexDirection = FlexBox::Direction::row;
            // pvf->inbox.items.add(FlexItem(20, minitemheight, *pvf->chanLabel).withMargin(0).withFlex(0));
            // pvf->inbox.items.add(FlexItem(3, 3));

            if (narrow) {
                // The device names after the channel numbers give way first when the line is short
                pvf->inbox.items.add(FlexItem(*pvf->linkButton).withWidth(linkbuttwidth).withMinWidth(linkminbuttwidth).withHeight(minitemheight).withMinHeight(minitemheight).withMargin(0).withFlex(0));
            } else {
                pvf->inbox.items.add(FlexItem(linkbuttwidth, minitemheight, *pvf->linkButton).withMargin(0).withFlex(0));
            }

            pvf->inbox.items.add(FlexItem(3, 3));

            const bool pairRow = i < mChannelViews.size() && chcnt == 2 && chi == 0;
            const int postmeterwidth = pairRow ? 2 * mainmeterwidth : mainmeterwidth;   // two bars for a stereo pair
            pvf->inbox.items.add(FlexItem(meterwidth, minitemheight, *pvf->premeter).withMargin(0).withFlex(0));
            if (pvf->premeter2) {
                pvf->premeter2->setVisible(pairRow);
                if (pairRow) pvf->inbox.items.add(FlexItem(meterwidth, minitemheight, *pvf->premeter2).withMargin(0).withFlex(0));
            }
            // A row with one bar keeps the room of two, so the names start at the same place on every row
            if (inMeterCol > (pairRow ? 2 : 1) * meterwidth) pvf->inbox.items.add(FlexItem(inMeterCol - (pairRow ? 2 : 1) * meterwidth, 3));
            // Captions go in the gap above the meters, so only input rows that have that gap get them
            const bool captionRow = i < mChannelViews.size() && chi == 0;
            if (pvf->inCaption) pvf->inCaption->setVisible(captionRow);
            if (pvf->outCaption) pvf->outCaption->setVisible(captionRow && !mHideRowMeter);
            pvf->inbox.items.add(FlexItem(3, 3));


            pvf->inbox.items.add(FlexItem(namewidth, minitemheight, pvf->namebox).withMargin(0).withFlex(1).withMaxWidth(jmax(namewidth, nameFull)));
            if (!narrow) {
                if (!ismetorfileorsoundboard) {
                    pvf->inbox.items.add(FlexItem(6, 3));
                    pvf->inbox.items.add(FlexItem(mutebuttwidth, minitemheight, *pvf->muteButton).withMargin(0).withFlex(0));
                    pvf->inbox.items.add(FlexItem(3, 3));
                    pvf->inbox.items.add(FlexItem(mutebuttwidth, minitemheight, *pvf->soloButton).withMargin(0).withFlex(0));

                    pvf->inbox.items.add(FlexItem(5, 3));
                    pvf->inbox.items.add(FlexItem(mutebuttwidth, minitemheight, *pvf->fxButton).withMargin(0).withFlex(0));
                } else {
                    // No MUTE, SOLO or FX here: their room stays empty, so every level slider starts at the same place
                    pvf->inbox.items.add(FlexItem(6 + mutebuttwidth + 3 + mutebuttwidth + 5 + mutebuttwidth, 3));
                }
            }
            pvf->inbox.items.add(FlexItem(3, 3));
            pvf->inbox.items.add(FlexItem(minSliderWidth, minitemheight, *pvf->levelSlider).withMargin(0).withFlex(1));
            pvf->inbox.items.add(FlexItem(1, 3));


            if (narrow) {
                pvf->inbox.items.add(FlexItem(2, 3));
            }


            ipw = 0;
            iph = minitemheight;
            for (auto & item : pvf->inbox.items) {
                ipw += item.minWidth;
            }

            pvf->monbox.items.clear();
            pvf->monbox.flexDirection = FlexBox::Direction::row;
            pvf->monbox.alignItems = FlexBox::AlignItems::center;
            auto addKnobs = [&] {
                pvf->monbox.items.add(FlexItem(kMixerKnobW, kMixerKnobH, *pvf->panSlider).withMargin(0).withFlex(0));
                if (panCol > kMixerKnobW) {
                    if (pairRow) pvf->monbox.items.add(FlexItem(kMixerKnobW, kMixerKnobH, *pvf->widthSlider).withMargin(0).withFlex(0));
                    else pvf->monbox.items.add(FlexItem(kMixerKnobW, 3));
                }
            };
            if (narrow) {
                // Every row's second line ends at the right edge, so its columns line up with the other rows'
                pvf->monbox.items.add(FlexItem(3, 3).withFlex(0.25));
                if (!ismetorfileorsoundboard) {
                    pvf->monbox.items.add(FlexItem(mutebuttwidth, minitemheight, *pvf->muteButton).withMargin(0).withFlex(0));
                    pvf->monbox.items.add(FlexItem(3, 3));
                    pvf->monbox.items.add(FlexItem(mutebuttwidth, minitemheight, *pvf->soloButton).withMargin(0).withFlex(0));
                    pvf->monbox.items.add(FlexItem(3, 3));
                    pvf->monbox.items.add(FlexItem(mutebuttwidth, minitemheight, *pvf->fxButton).withMargin(0).withFlex(0));
                } else {
                    pvf->monbox.items.add(FlexItem(3 * mutebuttwidth + 6, 3));   // their room stays empty, the columns line up
                }

                pvf->monbox.items.add(FlexItem(3, 3).withFlex(0));   // No slack before the pan block

                if (!mHideRowMeter) {
                    pvf->monbox.items.add(FlexItem(postmeterwidth, minitemheight, *pvf->meter).withMargin(0).withFlex(0));   // Collapsed peer row — tall meter drawn by the peer view instead
                    // a narrower meter keeps the widest one's room, so the pan and the rest line up
                    if (maxPostMeterW > postmeterwidth) pvf->monbox.items.add(FlexItem(maxPostMeterW - postmeterwidth, 3));
                }
                pvf->meter->setVisible(!mHideRowMeter);

                pvf->monbox.items.add(FlexItem(4, 3));

                //if (pannervisible)
                pvf->monbox.items.add(FlexItem(2, 3));
                addKnobs();
                pvf->monbox.items.add(FlexItem(1, 3).withFlex(0));   // No slack after the pan

                pvf->monbox.items.add(FlexItem(monsliderwidth, kMixerKnobH, *pvf->monitorSlider).withMargin(0).withFlex(0));
                pvf->monbox.items.add(FlexItem(2, 3));

                pvf->monbox.items.add(FlexItem(mutebuttwidth, minitemheight, *pvf->monfxButton).withMargin(0).withFlex(0));
                pvf->monbox.items.add(FlexItem(2, 3));


                if (destbuttvisible) {
                    pvf->monbox.items.add(FlexItem(sharedDestMinW, minitemheight, *pvf->destButton).withMargin(0).withFlex(1).withMaxWidth(sharedDestW));
                    pvf->monbox.items.add(FlexItem(2, 3));
                }

            }
            else {
                if (!mHideRowMeter) {
                    pvf->monbox.items.add(FlexItem(postmeterwidth, minitemheight, *pvf->meter).withMargin(0).withFlex(0));
                    // a narrower meter keeps the widest one's room, so the knob columns and the level sliders line up
                    if (maxPostMeterW > postmeterwidth) pvf->monbox.items.add(FlexItem(maxPostMeterW - postmeterwidth, 3));
                }
                pvf->meter->setVisible(!mHideRowMeter);
                pvf->monbox.items.add(FlexItem(4, 3));

                addKnobs();
                pvf->monbox.items.add(FlexItem(3, 3));

                pvf->monbox.items.add(FlexItem(monsliderwidth, kMixerKnobH, *pvf->monitorSlider).withMargin(0).withFlex(0));
                pvf->monbox.items.add(FlexItem(2, 3));
                pvf->monbox.items.add(FlexItem(mutebuttwidth, minitemheight, *pvf->monfxButton).withMargin(0).withFlex(0));
                pvf->monbox.items.add(FlexItem(2, 3));

                if (destbuttvisible) {
                    pvf->monbox.items.add(FlexItem(sharedDestW, minitemheight, *pvf->destButton).withMargin(0).withFlex(0));
                    pvf->monbox.items.add(FlexItem(2, 3));
                }
            }

            mpw = 0;
            mph = kMixerKnobH;   // the knobs with their caption and value
            for (auto & item : pvf->monbox.items) {
                mpw += item.minWidth;
            }
            const int monboxW = jmax(mpw, maxPannerWidth + mutebuttwidth + monsliderwidth + (destbuttvisible ? sharedDestW + 2 : 0) + 2);

            pvf->maincontentbox.items.clear();

            pvf->mainbox.items.clear();
            pvf->mainbox.flexDirection = FlexBox::Direction::column;

            bool dotopgap = (chi == 0) || i >= mChannelViews.size();


            // A row with captions leaves them their height above the meters
            const int capGap = captionRow ? pvf->meterCaptionHeight() + 1 : 0;
            const bool outCapShown = captionRow && !mHideRowMeter;

            if (dotopgap) {
                // gap at top
                pvf->mainbox.items.add(FlexItem(3, jmax(6, narrow ? capGap - 2 : capGap)));
            }


            if (narrow) {
                pvf->maincontentbox.flexDirection = FlexBox::Direction::column;
                pvf->maincontentbox.alignItems = FlexBox::AlignItems::stretch;   // both lines the row's whole width
                pvf->maincontentbox.items.add(FlexItem(3, 2));
                if (chi == 0) {
                    pvf->maincontentbox.items.add(FlexItem(ipw, iph , pvf->inbox).withMargin(0).withFlex(0));
                    pvf->maincontentbox.items.add(FlexItem(2, jmax(2, outCapShown ? capGap : 0)));   // OUT sits over the second line's meter
                }
                pvf->maincontentbox.items.add(FlexItem(mpw, mph , pvf->monbox).withMargin(0).withFlex(0));

                int mch = 0;
                for (auto & item : pvf->maincontentbox.items) {
                    mch += item.minHeight;
                }

                pvf->mainbox.items.add(FlexItem(60, mch, pvf->maincontentbox).withMargin(0).withFlex(0));

            } else {
                pvf->maincontentbox.flexDirection = FlexBox::Direction::row;
                pvf->maincontentbox.alignItems = FlexBox::AlignItems::center;
                pvf->maincontentbox.items.add(FlexItem(3, 2));
                pvf->maincontentbox.items.add(FlexItem(ipw, iph , pvf->inbox).withMargin(0).withFlex(2));
                pvf->maincontentbox.items.add(FlexItem(2, 2));
                //if (pannervisible)
                {
                    // One width on every row (never below what the box holds), so the meters and the Output
                    // column line up; the input side takes the rest
                    pvf->maincontentbox.items.add(FlexItem(monboxW, mph , pvf->monbox).withMargin(0).withFlex(0));
                }
                //else {
                //    pvf->maincontentbox.items.add(FlexItem(mpw, mph , pvf->monbox).withMargin(0).withFlex(0)); // (1).withMaxWidth(mutebuttwidth + destbuttwidth + 4));
                //}


                pvf->mainbox.items.add(FlexItem(60, jmax(iph, mph), pvf->maincontentbox).withMargin(0).withFlex(0));
            }



            mbh = 0;
            for (auto & item : pvf->mainbox.items) {
                mbh += item.minHeight;
            }



            if (narrow) {
                const int rowMin = jmax(ipw, mpw);
                channelsBox.items.add(FlexItem(rowMin, mbh, *pvf).withMargin(0).withFlex(0));
                rowsMinWidth = jmax(rowsMinWidth, rowMin);
                //peersheight += ph*2 + 6;
                peersheight += mbh + 2;
            } else {
                channelsBox.items.add(FlexItem(ipw + monboxW + 6, mbh, *pvf).withMargin(1).withFlex(0));
                rowsMinWidth = jmax(rowsMinWidth, 3 + ipw + 2 + monboxW + 2);
                //peersheight += ph + 11;
                peersheight += mbh + 2;
            }

            if (i < mChannelViews.size()+2) {
                channelsBox.items.add(FlexItem(3, 4));
                peersheight += 4;
            }

        }

    }


    // for input mode add met and file playback rows

    // The widest row and the header row at their smallest, plus the side insets (13 px)
    channelMinWidth = jmax(rowsMinWidth, addrowMinWidth) + 13;
    if (narrow) {
        channelMinHeight = std::max(mbh,  peersheight);
    }
    else {
        if (!mPeerMode) {
            channelMinHeight = std::max(mbh + 18,  peersheight);
        } else {
            channelMinHeight = std::max(mbh,  peersheight);
        }
    }

    if (notify) {
        listeners.call (&ChannelGroupsView::Listener::channelLayoutChanged, this);
    }

}


void ChannelGroupsView::updateLayout(bool notify)
{
    if (mPeerMode) {
        updateLayoutForRemotePeer(notify);
    } else {
        updateLayoutForInput(notify);
    }
}

Rectangle<int> ChannelGroupsView::getMinimumContentBounds() const
{
    return Rectangle<int>(0,0, channelMinWidth, channelMinHeight);
}

ChannelGroupsView::PeerRowMetrics ChannelGroupsView::getPeerRowMetrics() const
{
    PeerRowMetrics m;
    if (!mPeerMode || !mMainChannelView) return m;

    auto * mv = mMainChannelView.get();
    m.expanded = processor.getRemotePeerViewExpanded(mPeerIndex);
    m.nameNeed = (int) std::ceil(mv->nameLabel->getFont().getStringWidthFloat(mv->nameLabel->getText())) + 11;
    m.destNeed = m.expanded ? 0 : destWidthFor(mv, 44);
    m.widthKnob = mv->panMode == ChannelGroupView::PanPair;

    int totalchans = processor.getRemotePeerRecvChannelCount(mPeerIndex);
    if (totalchans <= 0 && processor.getRemotePeerInfoOnly(mPeerIndex)) totalchans = 2;
    m.meterWidth = kSbMeterPxPerChannel * jmax(1, totalchans);

    if (m.expanded) {
        // The instrument rows share the player rows' columns: their names, outputs and width knobs count too
        for (auto * cv : mChannelViews) {
            if (cv->chanIndex != 0) continue;
            m.nameNeed = jmax(m.nameNeed, (int) std::ceil(cv->nameLabel->getFont().getStringWidthFloat(cv->nameLabel->getText())) + 11);
            m.destNeed = jmax(m.destNeed, destWidthFor(cv, 44));
            if (cv->panMode == ChannelGroupView::PanPair) m.widthKnob = true;
        }
    }

    return m;
}

void ChannelGroupsView::setPeerRowLayout(int form, int nameCol, int destCol, int knobCol, int statWidth, int buttonWidth, int meterSlack)
{
    mRowMeterSlack = meterSlack;
    mRowForm = form;
    mRowNameCol = nameCol;
    mRowDestCol = destCol;
    mRowKnobCol = knobCol;
    mRowStatWidth = statWidth;
    mRowButtonWidth = buttonWidth;
}

Rectangle<int> ChannelGroupsView::getStatSlotBounds() const
{
    if (!mMainChannelView) return {};
    return mMainChannelView->statSlot + mMainChannelView->getPosition();
}

Rectangle<int> ChannelGroupsView::getNoteSlotBounds() const
{
    if (!mMainChannelView) return {};
    return mMainChannelView->noteSlot + mMainChannelView->getPosition();
}

void ChannelGroupsView::applyToAllSliders(std::function<void(Slider *)> & routine)
{
    for (int i=0; i < mChannelViews.size(); ++i) {
        ChannelGroupView * pvf = mChannelViews.getUnchecked(i);
        routine(pvf->levelSlider.get());
        routine(pvf->monitorSlider.get());
        routine(pvf->panSlider.get());
    }

    if (mMainChannelView) {
        routine(mMainChannelView->levelSlider.get());
        routine(mMainChannelView->panSlider.get());
        routine(mMainChannelView->monitorSlider.get());
    }

    if (mFileChannelView) {
        routine(mFileChannelView->levelSlider.get());
        routine(mFileChannelView->panSlider.get());
        routine(mFileChannelView->monitorSlider.get());
    }

    if (mMetChannelView) {
        routine(mMetChannelView->levelSlider.get());
        routine(mMetChannelView->panSlider.get());
        routine(mMetChannelView->monitorSlider.get());
    }

    if (mSoundboardChannelView) {
        routine(mSoundboardChannelView->levelSlider.get());
        routine(mSoundboardChannelView->panSlider.get());
        routine(mSoundboardChannelView->monitorSlider.get());
    }
}

void ChannelGroupsView::configKnob(ChannelGroupView * pvf, Slider * slider, bool isWidth)
{
    slider->setSliderStyle(Slider::RotaryHorizontalVerticalDrag);
    slider->setTextBoxStyle(Slider::NoTextBox, true, 0, 0);
    slider->setRotaryParameters(MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
    slider->setMouseDragSensitivity(160);
    slider->setSliderSnapsToMousePosition(false);
    slider->setScrollWheelEnabled(false);
    slider->setWantsKeyboardFocus(true);
    slider->getProperties().set ("sbKnob", true);

    if (isWidth) {
        slider->setRange(0.0, 1.0, 0.0);
        slider->setDoubleClickReturnValue(true, 1.0);
        slider->getProperties().set ("knobCaption", TRANS("WIDTH"));
        slider->textFromValueFunction = [](double v) -> String { return String((int) std::lround(v * 100.0)) + " %"; };
        slider->valueFromTextFunction = [](const String& s) -> double { return s.getDoubleValue() * 0.01; };
        slider->setTooltip(TRANS("Stereo width. 0 % is mono, 100 % is as sent."));
        slider->setValue(1.0, dontSendNotification);
    } else {
        slider->setRange(-1.0, 1.0, 0.0);
        slider->setDoubleClickReturnValue(true, 0.0);
        slider->getProperties().set ("knobCaption", TRANS("PAN"));
        slider->textFromValueFunction = [](double v) -> String {
            const int pct = (int) std::lround(std::abs(v) * 100.0);
            if (pct == 0) return "C";
            return String(v < 0 ? "L " : "R ") + String(pct);
        };
        slider->valueFromTextFunction = [](const String& s) -> double {
            const double v = s.retainCharacters("0123456789.").getDoubleValue() * 0.01;
            return s.containsIgnoreCase("l") ? -v : v;
        };
        slider->setValue(0.0, dontSendNotification);
    }
    slider->updateText();
    ignoreUnused(pvf);
}

// A stereo pair: left = pan - width, right = pan + width, each kept inside the pan range (width 1 = as sent)
void ChannelGroupsView::pairPansFromKnobs(ChannelGroupView * pvf, float & left, float & right) const
{
    const float pan = (float) pvf->panSlider->getValue();
    const float half = (float) pvf->widthSlider->getValue() * (pvf->pairSwapped ? -1.0f : 1.0f);
    left  = jlimit(-1.0f, 1.0f, pan - half);
    right = jlimit(-1.0f, 1.0f, pan + half);
}

// Knobs follow the stored pan positions, and stay where they are while they still describe them
void ChannelGroupsView::syncPairKnobs(ChannelGroupView * pvf, float left, float right)
{
    float kl = 0.0f, kr = 0.0f;
    pairPansFromKnobs(pvf, kl, kr);
    if (std::abs(kl - left) < 1.0e-3f && std::abs(kr - right) < 1.0e-3f) return;

    pvf->pairSwapped = right < left;
    pvf->panSlider->setValue(0.5 * (left + right), dontSendNotification);
    pvf->widthSlider->setValue(0.5 * std::abs(right - left), dontSendNotification);
}

void ChannelGroupsView::applyPairKnobs(ChannelGroupView * pvf, int changroup)
{
    float left = 0.0f, right = 0.0f;
    pairPansFromKnobs(pvf, left, right);
    processor.setRemotePeerChannelPan(mPeerIndex, changroup, 0, left);
    processor.setRemotePeerChannelPan(mPeerIndex, changroup, 1, right);
}

// Input Mixer monitor level: the console knob with its dB value under it; the brighter tick is 0 dB
void ChannelGroupsView::configMonitorKnob(Slider * slider)
{
    slider->setSliderStyle(Slider::RotaryHorizontalVerticalDrag);
    slider->setTextBoxStyle(Slider::NoTextBox, true, 0, 0);
    slider->setRotaryParameters(MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
    slider->setMouseDragSensitivity(160);
    slider->getProperties().set ("sbKnob", true);
    slider->getProperties().set ("knobCaption", TRANS("MONITOR"));
    slider->getProperties().set ("knobSize", kMixerKnob);
    slider->getProperties().set ("brightTick", slider->valueToProportionOfLength(1.0));
    slider->setDoubleClickReturnValue(true, 1.0);
    slider->textFromValueFunction = [](double v) -> String {
        if (v <= 0.0) return "-INF";
        const double db = Decibels::gainToDecibels(v);
        return (std::abs(db) < 0.05 ? String("0.0") : String(db, 1)) + " dB";
    };
    slider->setTooltip(TRANS("Monitor level: what you hear of this input. Double-click for 0 dB."));
    slider->updateText();
}

// "1-2 > Main L / Main R": the same style as the input labels, with the device's own channel names.
String ChannelGroupsView::outputLabel(int deststart, int destcnt) const
{
    String text;
    text << deststart + 1;
    if (destcnt > 1) text << "-" << deststart + destcnt;
    if (JUCEApplicationBase::isStandaloneApp() && getAudioDeviceManager && getAudioDeviceManager()) {
        if (auto cad = getAudioDeviceManager()->getCurrentAudioDevice()) {
            auto actives = cad->getActiveOutputChannels();
            auto all = cad->getOutputChannelNames();
            StringArray activeNames, names;
            for (int ni = 0; ni < all.size(); ++ni) if (actives[ni]) activeNames.add(all[ni]);
            for (int k = 0; k < destcnt; ++k)
                if (deststart + k < activeNames.size() && activeNames[deststart + k].isNotEmpty()) names.add(activeNames[deststart + k]);
            if (!names.isEmpty()) text << String::fromUTF8(" \xe2\x80\xba ") << names.joinIntoString(" / ");
        }
    }
    return text;
}

void ChannelGroupsView::setDestText(ChannelGroupView * pvf, int deststart, int destcnt)
{
    setDestLabel(pvf, outputLabel(deststart, destcnt));
}

void ChannelGroupsView::setDestLabel(ChannelGroupView * pvf, const String & text)
{
    if (text == pvf->destButton->getButtonText()) return;
    pvf->destButton->setButtonText(text);
    mDestTextChanged = true;
}

// The Output label in the font the button draws it with (its text area is the width less 10 px)
static Font destTextFont(Component * button)
{
    if (auto * lnf = dynamic_cast<SonoLookAndFeel*>(&button->getLookAndFeel()))
        return lnf->getDrawableButtonTextFont(ChannelGroupView::kHeaderLineH);
    return Font(13.0f);
}

int ChannelGroupsView::destWidthFor(ChannelGroupView * pvf, int minWidth) const
{
    return jlimit(minWidth, 320, (int) std::ceil(destTextFont(pvf->destButton.get()).getStringWidthFloat(pvf->destButton->getButtonText())) + 14);
}

int ChannelGroupsView::destMinWidthFor(ChannelGroupView * pvf, int minWidth) const
{
    // The channel numbers always show; the device names after them give way first (elided by the look and feel)
    const String text = pvf->destButton->getButtonText();
    const String sep = String::fromUTF8(" \xe2\x80\xba ");
    const String head = text.contains(sep) ? text.upToFirstOccurrenceOf(sep, true, false) + String::fromUTF8("\xe2\x80\xa6") : text;
    return jmin(destWidthFor(pvf, minWidth), jmax(minWidth, (int) std::ceil(destTextFont(pvf->destButton.get()).getStringWidthFloat(head)) + 14));
}

void ChannelGroupsView::updateCutTooltips()
{
    const String destTip = mPeerMode ? TRANS("Choose destination output channels") : TRANS("Choose destination monitoring channels");
    auto refresh = [this, &destTip](ChannelGroupView * pvf) {
        if (pvf == nullptr) return;
        if (pvf->destButton && pvf->destButton->isVisible()) {
            // The label's text area is the width less 10 px
            const String route = pvf->destButton->getButtonText();
            const int need = SbTextFit::textWidth(destTextFont(pvf->destButton.get()), route) + 10;
            SbTextFit::tooltipIfCut(*pvf->destButton, route + "\n" + destTip, need, destTip);
        }
        if (!mPeerMode && pvf->nameLabel && pvf->nameLabel->isEditableOnSingleClick() && pvf->nameLabel->isVisible()) {
            const String hint = TRANS("Set name for this group that others will see");
            auto & nl = *pvf->nameLabel;
            SbTextFit::tooltipIfCut(nl, nl.getText() + "\n" + hint, SbTextFit::labelWidth(nl), hint);
        }
    };
    for (auto * pvf : mChannelViews) refresh(pvf);
    refresh(mMetChannelView.get());
    refresh(mFileChannelView.get());
    refresh(mSoundboardChannelView.get());
    refresh(mMainChannelView.get());

    if (mPeerMode && mMainChannelView) {
        auto & nl = *mMainChannelView->nameLabel;
        SbTextFit::tooltipIfCut(nl, nl.getText(), SbTextFit::labelWidth(nl));
    }
}

void ChannelGroupsView::updateChannelViews(int specific)
{
    if (mPeerMode) {
        updatePeerModeChannelViews(specific);

        mAddButton->setVisible(false);
        mClearButton->setVisible(false);
        mInReverbButton->setVisible(false);
        mMonDelayButton->setVisible(false);
    } else {
        updateInputModeChannelViews(specific);

        mAddButton->setVisible(true);
        mClearButton->setVisible(true);
        mInReverbButton->setVisible(true);
        mMonDelayButton->setVisible(true);
    }
}


void ChannelGroupsView::updateInputModeChannelViews(int specific)
{
    uint32 nowstampms = Time::getMillisecondCounter();
    bool needsUpdateLayout = false;

    int changroup = 0;
    int sendcnt = (int) processor.getValueTreeState().getParameter(SonobusAudioProcessor::paramSendChannels)->convertFrom0to1( processor.getValueTreeState().getParameter(SonobusAudioProcessor::paramSendChannels)->getValue());

    if (sendcnt == 0) {
        sendcnt = processor.getTotalNumInputChannels(); // processor.getMainBusNumInputChannels();
    }

    int totalchans = processor.getTotalNumInputChannels(); // processor.getMainBusNumInputChannels();

    int totaloutchans = processor.getTotalNumOutputChannels();
    int changroups = processor.getInputGroupCount();
    int chi = 0;


    // met and file playback and soundboard
    if (mMetChannelView) {
                int destcnt, deststart;
        processor.getMetronomeChannelDestStartAndCount(deststart, destcnt);
        setDestText(mMetChannelView.get(), deststart, destcnt);
        mMetChannelView->monitorSlider->setVisible(true);
        mMetChannelView->monitorSlider->setValue(processor.getMetronomeMonitor(), dontSendNotification);
        mMetChannelView->levelSlider->setValue(processor.getMetronomeGain(), dontSendNotification);
        mMetChannelView->panSlider->setValue(processor.getMetronomePan(), dontSendNotification);
        mMetChannelView->showDivider = true;
        mMetChannelView->nameEditor->setVisible(false);

        mMetChannelView->meter->setMeterSource (&processor.getMetronomeMeterSource());
        mMetChannelView->meter->setSelectedChannel(0);

        SonoAudio::DelayParams eparams;
        processor.getMetronomeMonitorDelayParams(eparams);
        mMetChannelView->monfxButton->setToggleState(eparams.enabled, dontSendNotification);

    }

    if (mFileChannelView) {
                int destcnt, deststart;
        processor.getFilePlaybackDestStartAndCount(deststart, destcnt);
        setDestText(mFileChannelView.get(), deststart, destcnt);
        mFileChannelView->monitorSlider->setVisible(true);
        mFileChannelView->monitorSlider->setValue(processor.getFilePlaybackMonitor(), dontSendNotification);
        mFileChannelView->levelSlider->setValue(processor.getFilePlaybackGain(), dontSendNotification);
        mFileChannelView->panSlider->setVisible(false);
        mFileChannelView->panLabel->setVisible(false);
        //mFileChannelView->panSlider->setValue(processor.getFilPlaybackPan(), dontSendNotification);
        mFileChannelView->showDivider = true;
        mFileChannelView->nameEditor->setVisible(false);

        mFileChannelView->meter->setMeterSource (&processor.getFilePlaybackMeterSource());
        mFileChannelView->meter->setSelectedChannel(0);

        SonoAudio::DelayParams eparams;
        processor.getFilePlaybackMonitorDelayParams(eparams);
        mFileChannelView->monfxButton->setToggleState(eparams.enabled, dontSendNotification);

    }

    if (mSoundboardChannelView) {
                int destcnt, deststart;
        processor.getSoundboardProcessor()->getDestStartAndCount(deststart, destcnt);
        setDestText(mSoundboardChannelView.get(), deststart, destcnt);
        mSoundboardChannelView->monitorSlider->setVisible(true);
        mSoundboardChannelView->monitorSlider->setValue(processor.getSoundboardProcessor()->getMonitorGain(), dontSendNotification);
        mSoundboardChannelView->levelSlider->setValue(processor.getSoundboardProcessor()->getGain(), dontSendNotification);
        mSoundboardChannelView->panSlider->setVisible(false);
        mSoundboardChannelView->panLabel->setVisible(false);
        //mSoundboardChannelView->panSlider->setValue(processor.getSoundboardPan(), dontSendNotification);
        mSoundboardChannelView->showDivider = true;
        mSoundboardChannelView->nameEditor->setVisible(false);

        mSoundboardChannelView->meter->setMeterSource(&processor.getSoundboardProcessor()->getMeterSource());
        mSoundboardChannelView->meter->setSelectedChannel(0);

        SonoAudio::DelayParams eparams = processor.getSoundboardProcessor()->getMonitorDelayParams();
        mSoundboardChannelView->monfxButton->setToggleState(eparams.enabled, dontSendNotification);
    }


    int chstart = 0;
    int chcnt = 1;
    processor.getInputGroupChannelStartAndCount(changroup, chstart, chcnt);

    for (int i=0; i < mChannelViews.size(); ++i, ++chi) {
        if (specific >= 0 && specific != i) continue;

        if (chi >= chcnt && changroup < changroups-1) {
            changroup++;
            processor.getInputGroupChannelStartAndCount(changroup, chstart, chcnt);
            chi = 0;
        }

        ChannelGroupView * pvf = mChannelViews.getUnchecked(i);

        pvf->group = changroup;
        pvf->groupChanCount = chcnt;
        pvf->chanIndex = chi;
        // A stereo pair is one row.
        pvf->setVisible(!(chcnt == 2 && chi == 1));
        if (chcnt == 2 && chi == 1) continue;
        const bool stereoPair = chcnt == 2;


        String name = processor.getInputGroupName(changroup);

        //DBG("Got username: '" << username << "'");

        if (!pvf->nameLabel->isBeingEdited()) pvf->nameLabel->setText(name, dontSendNotification);
        pvf->nameEditor->setText(name, dontSendNotification);

        String chantext;
        if (chstart + chi >= totalchans) {
            // out of range now
            chantext << "--";
        }
        else {
            // Show the device's name for the mapped input next to the number ("3 › Input Name") so a glance tells
            // which physical or virtual input feeds the row. Names come from the active input channel list.
            StringArray activeNames;
            if (JUCEApplicationBase::isStandaloneApp() && getAudioDeviceManager && getAudioDeviceManager()) {   // hook may be unset during construction
                if (auto cad = getAudioDeviceManager()->getCurrentAudioDevice()) {
                    auto activeIns = cad->getActiveInputChannels(); auto all = cad->getInputChannelNames();
                    for (int ni = 0; ni < all.size(); ++ni) if (activeIns[ni]) activeNames.add(all[ni]);
                }
            }
            auto nameFor = [&](int ch) { return ch < activeNames.size() ? activeNames[ch] : String(); };   // never truncate — the button grows instead
            if (stereoPair) {
                chantext << chstart + 1 << "-" << chstart + 2;
                StringArray pairNames;
                for (int ch : { chstart, chstart + 1 }) if (nameFor(ch).isNotEmpty()) pairNames.add(nameFor(ch));
                if (!pairNames.isEmpty()) chantext << String::fromUTF8(" \xe2\x80\xba ") << pairNames.joinIntoString(" / ");
            } else if (chi == 0 && chcnt > 1 && activeNames.isEmpty()) {   // no device names (plugin host): keep the stock "1-2" range
                chantext << chstart+1 << "-" << chstart+chcnt;
            } else {   // Every channel line reads "<n> › <device name>", the first of a pair included
                chantext << chstart + chi + 1;
                if (nameFor(chstart + chi).isNotEmpty()) chantext << String::fromUTF8(" \xe2\x80\xba ") << nameFor(chstart + chi);   // "3 › BH Talkback"
            }
        }
        //pvf->chanLabel->setText(String::formatted("%d", i+1), dontSendNotification);

        pvf->linkButton->setButtonText(chantext);
        {   // Hover anywhere on the strip -> the device channel name(s) feeding it, never clipped
            StringArray tipNames;
            if (JUCEApplicationBase::isStandaloneApp() && getAudioDeviceManager && getAudioDeviceManager()) {
                if (auto cad = getAudioDeviceManager()->getCurrentAudioDevice()) {
                    auto activeIns = cad->getActiveInputChannels(); auto all = cad->getInputChannelNames();
                    for (int ni = 0; ni < all.size(); ++ni) if (activeIns[ni]) tipNames.add(all[ni]);
                }
            }
            String tip;
            for (int k = 0; k < chcnt; ++k) {
                if (k) tip << "\n";
                tip << "Input " << (chstart + k + 1);
                const int ch = chstart + k;
                if (ch < tipNames.size() && tipNames[ch].isNotEmpty()) tip << String::fromUTF8("  \xe2\x80\xba  ") << tipNames[ch];
            }
            pvf->linkButton->setTooltip(tip);
            if (pvf->nameLabel) pvf->nameLabel->setTooltip(tip);
            if (pvf->levelSlider) pvf->levelSlider->setTooltip(tip);
            if (pvf->chanLabel) pvf->chanLabel->setTooltip(tip);
        }

        bool aresoloed = processor.getInputGroupSoloed(changroup);
        bool aremuted = processor.getInputGroupMuted(changroup);

        // ShowDivider is the only mutable state ChannelGroupView::paint reads (bgColor/useBgColor are fixed at
        // construction), so the whole-row repaint at the end of this loop is gated on it changing.
        const bool prevShowDivider = pvf->showDivider;
        pvf->showDivider = chi == 0 ; // || i == 0;

        pvf->muteButton->setToggleState(aremuted , dontSendNotification);
        pvf->soloButton->setToggleState(aresoloed , dontSendNotification);


        bool infxon = processor.getInputEffectsActive(changroup);
        pvf->fxButton->setToggleState(infxon, dontSendNotification);

        bool inmonfxon = processor.getInputMonitorEffectsActive(changroup);
        pvf->monfxButton->setToggleState(inmonfxon, dontSendNotification);


        if (!pvf->levelSlider->isMouseOverOrDragging()) {
            pvf->levelSlider->setValue(processor.getInputGroupGain(changroup), dontSendNotification);
        }


        if (!pvf->monitorSlider->isMouseOverOrDragging()) {
            pvf->monitorSlider->setValue(processor.getInputMonitor(changroup), dontSendNotification);
        }


        pvf->meter->setMeterSource (&processor.getPostInputMeterSource());
        pvf->meter->setSelectedChannel(i);
        pvf->meter->setFixedNumChannels(stereoPair ? 2 : 1);   // a pair shows both post-fader channels

        pvf->premeter->setMeterSource (&processor.getInputMeterSource());
        pvf->premeter->setSelectedChannel(chstart + chi);
        if (pvf->premeter2 && stereoPair) {
            pvf->premeter2->setMeterSource(&processor.getInputMeterSource());
            pvf->premeter2->setSelectedChannel(chstart + 1);
            pvf->premeter2->setFixedNumChannels(1);
        }
        pvf->premeter->setFixedNumChannels(1);


        int deststart = 0;
        int destcnt = 2;
        processor.getInputGroupChannelDestStartAndCount(changroup, deststart, destcnt);
        destcnt = jmin(totaloutchans, destcnt);

        setDestText(pvf, deststart, destcnt);

        // PAN knob, and WIDTH beside it for a stereo pair, as on the player rows
        const bool panShown = !(sendcnt != 2 && destcnt != 2);
        pvf->panMode = !panShown ? ChannelGroupView::PanNone : stereoPair ? ChannelGroupView::PanPair : ChannelGroupView::PanSingle;
        pvf->panLabel->setVisible(false);
        pvf->panSlider->setVisible(panShown);
        pvf->widthSlider->setVisible(panShown && stereoPair);
        if (chcnt == 1 || stereoPair) pvf->panSlider->setDoubleClickReturnValue(true, 0.0);
        else pvf->panSlider->setDoubleClickReturnValue(true, (chi & 2) ? 1.0f : -1.0f); // double click defaults to alternating left/right

        if (!pvf->panSlider->isMouseButtonDown() && !pvf->widthSlider->isMouseButtonDown()) {
            if (stereoPair) syncPairKnobs(pvf, processor.getInputChannelPan(changroup, 0), processor.getInputChannelPan(changroup, 1));
            else pvf->panSlider->setValue(processor.getInputChannelPan(changroup, chi), dontSendNotification);
        }

        // hide things if we are not the first channel
        bool isprimary = chi == 0;
        bool destbuttvisible = isprimary; // && chcnt < totaloutchans;

        pvf->levelSlider->setVisible(isprimary);
        pvf->soloButton->setVisible(isprimary);
        pvf->muteButton->setVisible(isprimary);
        pvf->destButton->setVisible(destbuttvisible);
        pvf->monitorSlider->setVisible(isprimary);
        pvf->monfxButton->setVisible(isprimary);
        pvf->nameEditor->setVisible(false);
        pvf->nameLabel->setVisible(isprimary);


        // effects aren't used if channel count is above 2, right now
        pvf->fxButton->setVisible(isprimary && chcnt <= 2);

        pvf->linkButton->setVisible(isprimary || !mPeerMode);   // Sub-channels of a stereo/multi group show their own "2 › BH Mac R"
        pvf->monoButton->setVisible(false);

        if (prevShowDivider != pvf->showDivider) {
            pvf->repaint();
        }
    }

    updateMonDelayButton();

    if (needsUpdateLayout || mDestTextChanged) {
        mDestTextChanged = false;
        updateLayout();
    }

    lastUpdateTimestampMs = nowstampms;
}

void ChannelGroupsView::updatePeerModeChannelViews(int specific)
{
    uint32 nowstampms = Time::getMillisecondCounter();
    bool needsUpdateLayout = false;

    if (mMainChannelView.get() == nullptr) return;

    int changroup = 0;
    int changroups = processor.getRemotePeerChannelGroupCount(mPeerIndex);
    int chi = 0;

    int totalchans = processor.getRemotePeerRecvChannelCount(mPeerIndex);
    int totaloutchans = processor.getTotalNumOutputChannels();

    // Info-only peers have no channel plumbing on this side; use 2 so the main meter has nonzero width and the row
    // does not read "0ch".
    const bool peerInfoOnly = processor.getRemotePeerInfoOnly(mPeerIndex);
    if (totalchans <= 0 && peerInfoOnly) {
        totalchans = 2;
    }

    int chstart = 0;
    int chcnt = 0;
    processor.getRemotePeerChannelGroupStartAndCount(mPeerIndex, changroup, chstart, chcnt);

    int deststart = 0;
    int destcnt = 2;
    processor.getRemotePeerChannelGroupDestStartAndCount(mPeerIndex, changroup, deststart, destcnt);
    destcnt = jmin(totaloutchans, destcnt);

    bool expanded = processor.getRemotePeerViewExpanded(mPeerIndex);

    // deal with main strip

    bool connected = processor.getRemotePeerConnected(mPeerIndex);
    String username = processor.getRemotePeerShownName(mPeerIndex);

    // On a stage the player's instrument follows the name, as in the stages list: "Name (Instrument)"
    String shownName = username;
    const String instrument = processor.getRemotePeerHubInstrument(mPeerIndex);
    if (instrument.isNotEmpty()) shownName << " (" << instrument << ")";
    const bool nameChanged = mMainChannelView->nameLabel->getText() != shownName;
    mMainChannelView->nameLabel->setText(shownName, dontSendNotification);
    // The whole name in the hover while the row shows it cut
    SbTextFit::tooltipIfCut(*mMainChannelView->nameLabel, shownName, SbTextFit::labelWidth(*mMainChannelView->nameLabel, shownName));
    if (nameChanged) {
        // The name column is measured from this text: lay the rows out again, after this update
        Component::SafePointer<ChannelGroupsView> safe(this);
        MessageManager::callAsync([safe]() { if (safe) safe->listeners.call (&ChannelGroupsView::Listener::channelLayoutChanged, safe.getComponent()); });
    }

    String chantext;
    if (peerInfoOnly) {
        chantext = TRANS("hub");
    } else {
        chantext << totalchans << TRANS("ch");
    }
    mMainChannelView->linkButton->setButtonText(chantext);

    bool safetymuted = processor.getRemotePeerSafetyMuted(mPeerIndex);
    bool recvactive = processor.getRemotePeerRecvActive(mPeerIndex);
    bool recvallow = processor.getRemotePeerRecvAllow(mPeerIndex);

    bool mainanysoloed = processor.isAnythingSoloed();
    bool mainsoloed = processor.getRemotePeerSoloed(mPeerIndex);
    bool mainmuted = !processor.getRemotePeerRecvAllow(mPeerIndex);
    // Info-only and HostedMix peers read mute + solo state from the hub-mix override, which is authoritative because
    // audio flows via hub feeds, not local recv. The click handlers above and these visual reads must use the same
    // path.
    if (processor.getRemotePeerInfoOnly(mPeerIndex) || processor.isHostedMixModeEnabled()) {
        mainmuted = processor.getHubMixMuteForPeer(
            processor.getRemotePeerUserName(mPeerIndex));
        mainsoloed = processor.getHubMixSoloForPeer(
            processor.getRemotePeerUserName(mPeerIndex));
        mainanysoloed = processor.isAnyHubMixSoloed();
    }
    if (processor.getRemotePeerInfoOnly(mPeerIndex) || processor.isHostedMixModeEnabled()) {
        // Info-only/HostedMix peers: render OFF as transparent (so the row
        // reads blank) and the JUCE buttonOnColourId paints ON state as orange.
        // mutedBySoloColor when another player's solo is silencing this row, same as the expanded channel rows.
        mMainChannelView->muteButton->setColour(TextButton::buttonColourId,
            (mainanysoloed && !mainsoloed && !mainmuted) ? mutedBySoloColor : Colours::transparentBlack);
    } else if (safetymuted || (mainanysoloed && !mainsoloed && !mainmuted)) {
        mMainChannelView->muteButton->setColour(TextButton::buttonColourId, mutedBySoloColor);
    } else {
        mMainChannelView->muteButton->removeColour(TextButton::buttonColourId);
    }

    if (chi == 0 && totalchans==1) {
        mMainChannelView->monoButton->setVisible(true);
        mMainChannelView->linkButton->setVisible(false);
    } else
    {
        mMainChannelView->linkButton->setVisible(true);
        mMainChannelView->monoButton->setVisible(false);
    }
    //pvf->linkButton->setAlpha(totalchans > 1 ? 0.6f : 0.4f);

    mMainChannelView->muteButton->setToggleState(mainmuted , dontSendNotification);
    mMainChannelView->soloButton->setToggleState(mainsoloed , dontSendNotification);

    if (!mMainChannelView->levelSlider->isMouseOverOrDragging()) {
        // Info-only peers: the slider reflects the per-listener hub-mix gain override. All others reflect their local
        // recv gain.
        const bool useHubGain = processor.getRemotePeerInfoOnly(mPeerIndex);   // Hub mode = local fader (see sliderValueChanged)
        float sliderValue = useHubGain
            ? processor.getHubMixGainForPeer(processor.getRemotePeerUserName(mPeerIndex))
            : processor.getRemotePeerLevelGain(mPeerIndex);
        mMainChannelView->levelSlider->setValue(sliderValue, dontSendNotification);
    }


    mMainChannelView->meter->setMeterSource (processor.getRemotePeerRecvMeterSource(mPeerIndex));
    mMainChannelView->meter->setSelectedChannel(0);

    // A pan knob on the player's line for a mono player (its channel) and a stereo player (its pair, with the
    // width knob). A collapsed player with several instruments has a balance knob for its whole mix; expanded,
    // the knobs are on its instrument rows only.
    {
        auto * mv = mMainChannelView.get();
        int mode = ChannelGroupView::PanNone;
        if (!expanded && changroups == 1 && destcnt == 2) {
            if (chcnt == 1) mode = ChannelGroupView::PanSingle;
            else if (chcnt == 2 && totalchans == 2) mode = ChannelGroupView::PanPair;
        }
        else if (!expanded && changroups > 1 && totaloutchans >= 2) {
            mode = ChannelGroupView::PanBalance;
        }
        if (mode != mv->panMode) {
            mv->panMode = mode;
            needsUpdateLayout = true;  // the knob column may change
        }
        mv->panLabel->setVisible(false);
        mv->panSlider->setVisible(mode != ChannelGroupView::PanNone);
        mv->widthSlider->setVisible(mode == ChannelGroupView::PanPair);

        if (!mv->panSlider->isMouseButtonDown() && !mv->widthSlider->isMouseButtonDown()) {
            if (mode == ChannelGroupView::PanPair) {
                syncPairKnobs(mv, processor.getRemotePeerChannelPan(mPeerIndex, changroup, 0),
                                  processor.getRemotePeerChannelPan(mPeerIndex, changroup, 1));
            }
            else if (mode == ChannelGroupView::PanSingle) {
                const auto pan = processor.getRemotePeerChannelPan(mPeerIndex, changroup, chi);
                if (pan != mv->panSlider->getValue()) mv->panSlider->setValue(pan, dontSendNotification);
            }
            else if (mode == ChannelGroupView::PanBalance) {
                const auto bal = processor.getRemotePeerBalance(mPeerIndex);
                if (bal != mv->panSlider->getValue()) mv->panSlider->setValue(bal, dontSendNotification);
            }
        }
        mv->panSlider->setTooltip(mode == ChannelGroupView::PanBalance ? TRANS("Balance of this player's whole mix, after the instruments' own pans. Double-click for centre.")
                                                                       : TRANS("Pan"));
    }

    bool maindestbuttvisible = !expanded;
    mMainChannelView->destButton->setVisible(maindestbuttvisible);

    if (maindestbuttvisible) {
        // A collapsed player shows its one output, or Mixed when its instruments go to different outputs.
        bool sameOutput = true;
        for (int g = 1; g < changroups && sameOutput; ++g) {
            int st = 0, cnt = 2;
            processor.getRemotePeerChannelGroupDestStartAndCount(mPeerIndex, g, st, cnt);
            sameOutput = st == deststart && jmin(totaloutchans, cnt) == destcnt;
        }
        if (sameOutput) setDestText(mMainChannelView.get(), deststart, destcnt);
        else setDestLabel(mMainChannelView.get(), TRANS("Mixed"));
    }

    mMainChannelView->monitorSlider->setVisible(false);

    float disalpha = 0.4;
    mMainChannelView->nameLabel->setAlpha(connected ? 1.0 : 0.8);
    // Info-only rows have recvactive=false and safetymuted=true, so the stock formula would dim the slider as if
    // disabled even though it controls a real per-listener gain via /sb/hxgain. Treat them as always active.
    mMainChannelView->levelSlider->setAlpha(
        (peerInfoOnly || (recvactive && !safetymuted)) ? 1.0 : disalpha);

    // effects aren't used if channel count is above 2, right now
    mMainChannelView->fxButton->setVisible(!expanded && changroups == 1 && chcnt <= 2);
    bool infxon = processor.getRemotePeerEffectsActive(mPeerIndex, changroup);
    mMainChannelView->fxButton->setToggleState(infxon, dontSendNotification);

    mMainChannelView->linkButton->setToggleState(expanded, dontSendNotification);

    mMainChannelView->monfxButton->setVisible(false);
    mMainChannelView->nameEditor->setVisible(false);

    if (!expanded) {
        // hide all the subchannel views
        for (int i=0; i < mChannelViews.size(); ++i) {
            if (specific >= 0 && specific != i) continue;
            ChannelGroupView * pvf = mChannelViews.getUnchecked(i);
            pvf->setVisible(false);
        }
    }


    for (int i=0; expanded && i < mChannelViews.size(); ++i, ++chi) {
        if (specific >= 0 && specific != i) continue;


        if (chi >= chcnt && changroup < changroups-1) {
            changroup++;
            chstart += chcnt;

            processor.getRemotePeerChannelGroupStartAndCount(mPeerIndex, changroup, chstart, chcnt);
            chi = 0;
        }


        ChannelGroupView * pvf = mChannelViews.getUnchecked(i);
        pvf->group = changroup;
        pvf->groupChanCount = chcnt;
        pvf->chanIndex = chi;
        // A stereo pair is one row.
        pvf->setVisible(!(chcnt == 2 && chi == 1));
        if (chcnt == 2 && chi == 1) continue;
        const bool stereoPair = chcnt == 2;

        String name = processor.getRemotePeerChannelGroupName (mPeerIndex, changroup);

        //DBG("Got username: '" << username << "'");
        String dispname = name ; //(i==0) ? username : "";
        //if (name.isNotEmpty()) {
        //    dispname << " | " << name;
        //}

        if (pvf->nameLabel->getText() != dispname) {
            pvf->nameLabel->setText(dispname, dontSendNotification);
            needsUpdateLayout = true;   // the name column follows the names
        }
        {   // Hover on a remote player's strip -> that channel's label (the clayinfo name), even when the strip is too narrow to print it
            String tip = name.isNotEmpty() ? name : String("(unnamed channel)");
            if (chcnt > 2) tip << "  (" << (chi + 1) << " of " << chcnt << ")";
            pvf->nameLabel->setTooltip(tip);
            if (pvf->levelSlider) pvf->levelSlider->setTooltip(tip);
            if (pvf->linkButton) pvf->linkButton->setTooltip(tip);
            if (pvf->chanLabel) pvf->chanLabel->setTooltip(tip);
        }

        //pvf->chanLabel->setText(String::formatted("%d", i+1), dontSendNotification);

        String chantext;
        if (processor.getLayoutFormatChangedForRemotePeer(mPeerIndex)) {
            chantext << "*";
        }
        chantext << chstart + chi + 1;
        if (stereoPair) chantext << "-" << chstart + 2;
        pvf->linkButton->setButtonText(chantext);


        int chstart=0, chcnt=0;
        processor.getRemotePeerChannelGroupStartAndCount(mPeerIndex, changroup, chstart, chcnt);

        bool anysoloed = processor.getRemotePeerChannelSoloed(mPeerIndex, -1);
        bool aresoloed = processor.getRemotePeerChannelSoloed(mPeerIndex, changroup);
        bool aremuted = !processor.getRemotePeerRecvAllow(mPeerIndex) || processor.getRemotePeerChannelMuted(mPeerIndex, changroup);

        // Info-only rows reflect the hub-mix mute override instead of the local recv-allow / channel-mute state,
        // which does not apply when the peer has no audio plumbing on this client. Do not extend to HostedMix: this
        // is per-channel-group state, so hub-peer mute would make every channel button mirror the main mute, which
        // mMainChannelView handles.
        if (processor.getRemotePeerInfoOnly(mPeerIndex)) {
            aremuted = processor.getHubMixMuteForPeer(processor.getRemotePeerUserName(mPeerIndex));
        }

        if (safetymuted || (anysoloed && !aresoloed && !aremuted) || (mainanysoloed && !mainsoloed && !mainmuted)) {
            pvf->muteButton->setColour(TextButton::buttonColourId, mutedBySoloColor);
        } else {
            pvf->muteButton->removeColour(TextButton::buttonColourId);
        }

        // See updateInputModeChannelViews: gate the row repaint on the only paint-affecting state changing.
        const bool prevShowDivider = pvf->showDivider;
        pvf->showDivider = chi == 0; //  && i != 0 ; //&& i < mChannelViews.size()-1; // || i == 0;


        if (chi == 0 && totalchans==1) {
            pvf->monoButton->setVisible(true);
            pvf->linkButton->setVisible(false);
        } else {
            pvf->linkButton->setVisible(true);
            pvf->monoButton->setVisible(false);

            if (i < 0) {
                pvf->linkButton->setToggleState(expanded, dontSendNotification);
            }
            else { // if (chi == 0 && chcnt > 1) {
                pvf->linkButton->setToggleState(false, dontSendNotification);
            }
        }
        //pvf->linkButton->setAlpha(totalchans > 1 ? 0.6f : 0.4f);


        pvf->muteButton->setToggleState(aremuted , dontSendNotification);
        pvf->soloButton->setToggleState(aresoloed , dontSendNotification);


        bool infxon = processor.getRemotePeerEffectsActive(mPeerIndex, changroup);
        pvf->fxButton->setToggleState(infxon, dontSendNotification);

        if (!pvf->levelSlider->isMouseOverOrDragging()) {
            pvf->levelSlider->setValue(processor.getRemotePeerChannelGain (mPeerIndex, changroup), dontSendNotification);
        }


        pvf->meter->setMeterSource (processor.getRemotePeerRecvMeterSource(mPeerIndex));
        pvf->meter->setSelectedChannel(i);
        //if (mPeerMode && totalchans == 2 && chi == 0) {
        //    pvf->meter->setFixedNumChannels(2);
        //} else
        {
            pvf->meter->setFixedNumChannels(1);
        }


        deststart = 0;
        destcnt = 2;
        processor.getRemotePeerChannelGroupDestStartAndCount(mPeerIndex, changroup, deststart, destcnt);
        destcnt = jmin(totaloutchans, destcnt);

        setDestText(pvf, deststart, destcnt);

        // A channel row has its pan knob; a stereo pair's row has pan and width
        {
            const int mode = destcnt != 2 ? ChannelGroupView::PanNone
                           : stereoPair   ? ChannelGroupView::PanPair : ChannelGroupView::PanSingle;
            if (mode != pvf->panMode) {
                pvf->panMode = mode;
                needsUpdateLayout = true;
            }
            pvf->panLabel->setVisible(false);
            pvf->panSlider->setVisible(mode != ChannelGroupView::PanNone);
            pvf->widthSlider->setVisible(mode == ChannelGroupView::PanPair);
            pvf->panSlider->setTooltip(TRANS("Pan"));

            if (!pvf->panSlider->isMouseButtonDown() && !pvf->widthSlider->isMouseButtonDown()) {
                if (mode == ChannelGroupView::PanPair) {
                    syncPairKnobs(pvf, processor.getRemotePeerChannelPan(mPeerIndex, changroup, 0),
                                       processor.getRemotePeerChannelPan(mPeerIndex, changroup, 1));
                }
                else if (mode == ChannelGroupView::PanSingle) {
                    pvf->panSlider->setValue(processor.getRemotePeerChannelPan(mPeerIndex, changroup, chi), dontSendNotification);
                }
            }
        }

        bool isprimary = chi == 0;
        bool destbuttvisible = isprimary /*&& chcnt < totaloutchans */;
        pvf->levelSlider->setVisible(isprimary);
        pvf->soloButton->setVisible(isprimary);
        pvf->muteButton->setVisible(isprimary);
        pvf->nameLabel->setVisible(isprimary);
        pvf->destButton->setVisible(destbuttvisible);
        pvf->monitorSlider->setVisible(false);
        pvf->monfxButton->setVisible(false);
        pvf->nameEditor->setVisible(false);

        const float disalpha = 0.4;
        pvf->nameLabel->setAlpha(connected ? 1.0 : 0.8);
        pvf->levelSlider->setAlpha((recvactive && !safetymuted) ? 1.0 : disalpha);

        // effects aren't used if channel count is above 2, right now
        pvf->fxButton->setVisible(isprimary && chcnt <= 2);

        if (prevShowDivider != pvf->showDivider) {
            pvf->repaint();
        }
    }

    if (needsUpdateLayout || mDestTextChanged) {
        mDestTextChanged = false;
        updateLayout();
    }

    lastUpdateTimestampMs = nowstampms;
}



void ChannelGroupsView::timerCallback(int timerId)
{

}

void ChannelGroupsView::updateMonDelayButton()
{

    DelayParams metparams;
    processor.getMetronomeMonitorDelayParams(metparams);

    DelayParams fileparams;
    processor.getFilePlaybackMonitorDelayParams(fileparams);

    DelayParams sbparams = processor.getSoundboardProcessor()->getMonitorDelayParams();

    DelayParams eparam;

    // if any are on, turn them off... otherwise turn them all on
    bool anyenabled = metparams.enabled || fileparams.enabled || sbparams.enabled;

    if (!anyenabled) {
        int numgroups = processor.getInputGroupCount();
        for (int i=0; i < numgroups; ++i) {
            processor.getInputMonitorDelayParams(i, eparam);
            if (eparam.enabled) {
                anyenabled = true;
                break;
            }
        }
    }

    mMonDelayButton->setToggleState(anyenabled, dontSendNotification);
}

void ChannelGroupsView::toggleAllMonitorDelay()
{
    int numgroups = processor.getInputGroupCount();

    DelayParams metparams;
    processor.getMetronomeMonitorDelayParams(metparams);

    DelayParams fileparams;
    processor.getFilePlaybackMonitorDelayParams(fileparams);

    DelayParams sbparams = processor.getSoundboardProcessor()->getMonitorDelayParams();

    DelayParams eparam;

    // if any are on, turn them off... otherwise turn them all on
    bool doDisable = metparams.enabled || fileparams.enabled || sbparams.enabled;

    if (!doDisable) {
        for (int i=0; i < numgroups; ++i) {
            processor.getInputMonitorDelayParams(i, eparam);
            if (eparam.enabled) {
                doDisable = true;
                break;
            }
        }
    }

    // Switching it on with no time set would do nothing: start from how late the stage hears this player.
    const double fromStage = doDisable ? -1.0 : processor.getMonitoringDelayTimeFromStage();
    auto fill = [fromStage](DelayParams& p) { if (fromStage > 0.0 && p.delayTimeMs <= 0.0f) p.delayTimeMs = (float) fromStage; };
    fill(metparams); fill(fileparams); fill(sbparams);

    // set them all
    metparams.enabled = !doDisable;
    processor.setMetronomeMonitorDelayParams(metparams);

    fileparams.enabled = !doDisable;
    processor.setFilePlaybackMonitorDelayParams(fileparams);

    sbparams.enabled = !doDisable;
    processor.getSoundboardProcessor()->setMonitorDelayParams(sbparams);


    for (int i=0; i < numgroups; ++i) {
        processor.getInputMonitorDelayParams(i, eparam);
        fill(eparam);
        eparam.enabled = !doDisable;
        processor.setInputMonitorDelayParams(i, eparam);
    }

    updateChannelViews();
}


void ChannelGroupsView::choiceButtonSelected(SonoChoiceButton *comp, int index, int ident)
{
    for (int i=0; i < mChannelViews.size(); ++i) {
        ChannelGroupView * pvf = mChannelViews.getUnchecked(i);

    }
}

void ChannelGroupsView::buttonClicked (Button* buttonThatWasClicked)
{

    if (mPeerMode) {
        int changroups = processor.getRemotePeerChannelGroupCount(mPeerIndex);

        for (int i=0; i < mChannelViews.size(); ++i) {

            ChannelGroupView * pvf = mChannelViews.getUnchecked(i);
            int changroup = pvf->group;

            if (pvf->muteButton.get() == buttonThatWasClicked) {

                bool newmute = buttonThatWasClicked->getToggleState();

                // Info-only rows (HostedMix non-hub peers) route mute through the hub's mix-control channel rather
                // than the local channel-group state, which has no audio plumbing. Do not extend this to HostedMix:
                // this is per-channel-group mute but hub-mix mute is per-peer, so it would silence all channels when
                // one was clicked. Per-channel-group mute in HostedMix would need a new protocol, so it is inert for
                // relayed peers.
                if (processor.getRemotePeerInfoOnly(mPeerIndex)) {
                    String peerName = processor.getRemotePeerUserName(mPeerIndex);
                    processor.setHubMixMuteForPeer(peerName, newmute);
                    updateChannelViews();
                    break;
                }

                processor.setRemotePeerChannelMuted (mPeerIndex, changroup, newmute);

                // see if any are muted
                bool anysubmute = false;
                bool allsubmute = true;
                for (int gi=0; gi < changroups; ++gi) {
                    if (processor.getRemotePeerChannelMuted(mPeerIndex, gi)) {
                        anysubmute = true;
                    } else {
                        allsubmute = false;
                    }
                }

                bool recvactive = processor.getRemotePeerRecvActive(mPeerIndex);

                // Muting a player's only group clears recv allow, but a hub row can still report recv active, so test
                // allow as well or the unmute never restores it.
                if (!allsubmute && (!recvactive || !processor.getRemotePeerRecvAllow(mPeerIndex))) {
                    // allows receiving and invites
                    processor.setRemotePeerRecvActive(mPeerIndex, true);
                } else if (allsubmute && recvactive) {
                    // turns off receiving and allow
                    processor.setRemotePeerRecvAllow(mPeerIndex, false);
                }

                updateChannelViews();
                break;
            }
            else if (pvf->linkButton.get() == buttonThatWasClicked) {
                //processor.setRemotePeerChannelMuted (mPeerIndex, changroup, buttonThatWasClicked->getToggleState());
                //updateChannelViews();

                showChangePeerChannelsLayout(i, buttonThatWasClicked);

                break;
            }
            else if (pvf->destButton.get() == buttonThatWasClicked) {
                //processor.setRemotePeerChannelMuted (mPeerIndex, changroup, buttonThatWasClicked->getToggleState());
                //updateChannelViews();

                showDestSelectionMenu(buttonThatWasClicked, i);

                break;
            }
            else if (pvf->soloButton.get() == buttonThatWasClicked) {
                if (ModifierKeys::currentModifiers.isAltDown()) {
                    // exclusive solo this one

                    bool newsolo = buttonThatWasClicked->getToggleState();

                    // exclusive solo this one XX

                    for (int gi=0; gi < changroups; ++gi) {
                        if (newsolo) {
                            processor.setRemotePeerChannelSoloed(mPeerIndex, gi, changroup == gi);
                        }
                        else {
                            processor.setRemotePeerChannelSoloed(mPeerIndex, gi, false);
                        }
                    }

                    /*
                    for (int j=0; j < processor.getNumberRemotePeers(); ++j) {
                        if (newsolo) {
                            processor.setRemotePeerSoloed(j, mPeerIndex == j);
                        }
                        else {
                            processor.setRemotePeerSoloed(j, false);
                        }
                    }
                     */

                    // disable solo for main monitor too
                    //processor.getValueTreeState().getParameter(SonobusAudioProcessor::paramMainMonitorSolo)->setValueNotifyingHost(0.0);

                    updateChannelViews();
                } else {
                    bool newsolo = buttonThatWasClicked->getToggleState();

                    processor.setRemotePeerChannelSoloed (mPeerIndex, changroup, newsolo);

                    bool anysolo = false;
                    for (int j=0; j < changroups; ++j) {
                        if (processor.getRemotePeerChannelSoloed(mPeerIndex, j)) {
                            anysolo = true;
                        }
                    }

                    //if (newsolo) {
                    //    processor.setRemotePeerSoloed(mPeerIndex, newsolo);
                    //} else if (!anysolo) {
                    //    processor.setRemotePeerSoloed(mPeerIndex, false);
                    //}


                    updateChannelViews();
                }
                break;
            }
            else if (pvf->fxButton.get() == buttonThatWasClicked) {

                if (!effectsCalloutBox) {
                    showEffects(changroup, true, pvf->fxButton.get());
                } else {
                    showEffects(changroup, false);
                }

                break;
            }
            else if (pvf->monfxButton.get() == buttonThatWasClicked) {

                if (!monEffectsCalloutBox) {
                    showMonitorEffects(changroup, true, pvf->monfxButton.get());
                } else {
                    showMonitorEffects(changroup, false);
                }

                break;
            }
        }

    }
    else {
        int changroups = processor.getInputGroupCount();

        for (int i=0; i < mChannelViews.size(); ++i)
        {
            ChannelGroupView * pvf = mChannelViews.getUnchecked(i);
            int changroup = pvf->group;

            if (pvf->muteButton.get() == buttonThatWasClicked) {
                processor.setInputGroupMuted(changroup, buttonThatWasClicked->getToggleState());
                updateChannelViews();
                break;
            }
            else if (pvf->linkButton.get() == buttonThatWasClicked) {
                //processor.setRemotePeerChannelMuted (mPeerIndex, changroup, buttonThatWasClicked->getToggleState());
                //updateChannelViews();

                inputButtonPressed(buttonThatWasClicked, i, buttonThatWasClicked->getToggleState());

                break;
            }
            else if (pvf->destButton.get() == buttonThatWasClicked) {
                //processor.setRemotePeerChannelMuted (mPeerIndex, changroup, buttonThatWasClicked->getToggleState());
                //updateChannelViews();

                showDestSelectionMenu(buttonThatWasClicked, i);

                break;
            }
            else if (pvf->soloButton.get() == buttonThatWasClicked) {

                if (ModifierKeys::currentModifiers.isAltDown()) {
                    // exclusive solo this one

                    bool newsolo = buttonThatWasClicked->getToggleState();

                    for (int j=0; j < changroups; ++j) {
                        if (newsolo) {
                            processor.setInputGroupSoloed(j, changroup == j);
                        }
                        else {
                            processor.setInputGroupSoloed(j, false);
                        }
                    }

                    // change solo for main monitor too
                    //processor.getValueTreeState().getParameter(SonobusAudioProcessor::paramMainMonitorSolo)->setValueNotifyingHost(newsolo ? 1.0f : 0.0);

                    updateChannelViews();
                } else {
                    bool newsolo = buttonThatWasClicked->getToggleState();
                    processor.setInputGroupSoloed (changroup, newsolo);

                    //if (newsolo) {
                       // only enable main in solo
                      //  processor.getValueTreeState().getParameter(SonobusAudioProcessor::paramMainMonitorSolo)->setValueNotifyingHost(1.0f);
                    //}

                    updateChannelViews();
                }
                break;
            }
            else if (pvf->fxButton.get() == buttonThatWasClicked) {

                if (!effectsCalloutBox) {
                    showEffects(changroup, true, pvf->fxButton.get());
                } else {
                    showEffects(changroup, false);
                }

                break;
            }
            else if (pvf->monfxButton.get() == buttonThatWasClicked) {

                if (!monEffectsCalloutBox) {
                    showMonitorEffects(changroup, true, pvf->monfxButton.get());
                } else {
                    showMonitorEffects(changroup, false);
                }

                break;
            }
        }

    }
}

void ChannelGroupsView::clearGroupsPressed()
{
    if (mPeerMode) return; // not used for it

    Array<GenericItemChooserItem> items;
    items.add(GenericItemChooserItem(TRANS("Confirm Remove of All Inputs")));

    Component* dw = mClearButton->findParentComponentOfClass<AudioProcessorEditor>();
    if (!dw) dw = mClearButton->findParentComponentOfClass<Component>();
    Rectangle<int> bounds =  dw->getLocalArea(nullptr, mClearButton->getScreenBounds());

    SafePointer<ChannelGroupsView> safeThis(this);

    auto callback = [safeThis](GenericItemChooser* chooser,int index) mutable {
        if (!safeThis) return;

        safeThis->processor.setInputGroupCount(0);
        safeThis->processor.updateRemotePeerUserFormat();

        safeThis->rebuildChannelViews(true);
    };

    GenericItemChooser::launchPopupChooser(items, bounds, dw, callback, -1, dw ? dw->getHeight()-30 : 0);
}

void ChannelGroupsView::addGroupPressed()
{
    if (mPeerMode) return; // not used for it

    Array<GenericItemChooserItem> items;

    int totalouts = processor.getTotalNumOutputChannels();
    int totalins = processor.getTotalNumInputChannels();
    totalins = jmin(totalins, MAX_CHANNELS);

    items.add(GenericItemChooserItem(TRANS("ADD INPUT GROUP:"), {}, nullptr, false));

    for (int i=0; i < totalins; ++i) {
        String name;
        if (i == 0) {
            name << TRANS("Mono");
        } else if (i == 1) {
            name << TRANS("Stereo");
        } else {
            name << i+1 << " " << TRANS("channel");
        }

        items.add(GenericItemChooserItem(name, {}, nullptr, i==0));
    }


    Component* dw = mAddButton->findParentComponentOfClass<AudioProcessorEditor>();
    if (!dw) dw = mAddButton->findParentComponentOfClass<Component>();
    Rectangle<int> bounds =  dw->getLocalArea(nullptr, mAddButton->getScreenBounds());

    SafePointer<ChannelGroupsView> safeThis(this);

    auto callback = [safeThis,totalins,totalouts](GenericItemChooser* chooser,int index) mutable {
        if (!safeThis || index == 0) return;
        int insAt = safeThis->processor.getInputGroupCount();
        int chstart = 0;
        if (insAt > 0) {
            int chcnt;
            safeThis->processor.getInputGroupChannelStartAndCount(insAt-1, chstart, chcnt);
            chstart = jmin(totalins - (index), chstart+chcnt);
        }
        // add new group to end, defaulting to after last existing
        if (safeThis->processor.insertInputChannelGroup(insAt, chstart, index)) {
            safeThis->processor.setInputGroupChannelDestStartAndCount(insAt, 0, jmin(totalouts, jmax(2, index)));
            safeThis->processor.setInputGroupCount(insAt+1);
            safeThis->processor.setInputGroupName(insAt, ""); // clear the name
            safeThis->processor.updateRemotePeerUserFormat();
            safeThis->rebuildChannelViews(true);
        }
    };

    GenericItemChooser::launchPopupChooser(items, bounds, dw, callback, -1, dw ? dw->getHeight()-30 : 0);
}

void ChannelGroupsView::showChangeGroupChannels(int changroup, Component * showfrom)
{
    if (mPeerMode) return; // not used for it

    Array<GenericItemChooserItem> items;

    int totalouts = processor.getTotalNumOutputChannels();
    int totalins = processor.getTotalNumInputChannels();

    totalins = jmin(totalins, MAX_CHANNELS);

    items.add(GenericItemChooserItem(TRANS("CHANGE CHANNEL LAYOUT:"), {}, nullptr, false, true));


    for (int i=0; i < totalins; ++i) {
        String name;
        if (i == 0) {
            name << TRANS("Mono");
        } else if (i == 1) {
            name << TRANS("Stereo");
        } else {
            name << i+1 << " " << TRANS("channel");
        }

        items.add(GenericItemChooserItem(name, {}, nullptr, i==0));
    }


    Component* dw = showfrom->findParentComponentOfClass<AudioProcessorEditor>();
    if (!dw) dw = showfrom->findParentComponentOfClass<Component>();
    Rectangle<int> bounds =  dw->getLocalArea(nullptr, showfrom->getScreenBounds());

    SafePointer<ChannelGroupsView> safeThis(this);
    SafePointer<Component> safeSource(showfrom);

    auto callback = [safeThis,changroup,totalins,totalouts,safeSource](GenericItemChooser* chooser,int index) mutable {
        if (!safeThis || index == 0) return;
        int chstart = 0;
        int chcnt;
        safeThis->processor.getInputGroupChannelStartAndCount(changroup, chstart, chcnt);
        // just set the new channel count
        safeThis->processor.setInputGroupChannelStartAndCount(changroup, chstart, index);
        safeThis->processor.updateRemotePeerUserFormat();
        safeThis->rebuildChannelViews(true);

        Timer::callAfterDelay(100, [safeSource](){
            if (safeSource) {
                safeSource->grabKeyboardFocus();
            }
        });
    };

    GenericItemChooser::launchPopupChooser(items, bounds, dw, callback, -1, dw ? dw->getHeight()-30 : 0);
}

void ChannelGroupsView::showChangePeerChannelsLayout(int chindex, Component * showfrom)
{
    if (!mPeerMode) return; // not used for it

    Array<GenericItemChooserItem> items;

    int totalins = processor.getRemotePeerRecvChannelCount(mPeerIndex);

    totalins = jmin(totalins, MAX_CHANNELS);

    int changroup = getChanGroupFromIndex(chindex);

    int chstart = 0;
    int chcnt = 0;
    processor.getRemotePeerChannelGroupStartAndCount(mPeerIndex, changroup, chstart, chcnt);

    int usableins = totalins - (chindex);
    int selindex = chindex == chstart ? chcnt : -1;

    items.add(GenericItemChooserItem(TRANS("CHANGE CHANNEL LAYOUT:"), {}, nullptr, false, true));

    for (int i=0; i < usableins; ++i) {
        String name;
        if (i == 0) {
            name << TRANS("Mono");
        } else if (i == 1) {
            name << TRANS("Stereo");
        } else {
            name << i+1 << " " << TRANS("channel");
        }

        items.add(GenericItemChooserItem(name, {}, nullptr, i==0));
    }


    if (processor.getLayoutFormatChangedForRemotePeer(mPeerIndex)) {
        auto udata = std::make_shared<CmdListItemData>(1);
        items.add(GenericItemChooserItem(TRANS("<Restore Original Layout>"), {}, udata, true));
    }


    Component* dw = showfrom->findParentComponentOfClass<AudioProcessorEditor>();
    if (!dw) dw = showfrom->findParentComponentOfClass<Component>();
    Rectangle<int> bounds =  dw->getLocalArea(nullptr, showfrom->getScreenBounds());

    SafePointer<ChannelGroupsView> safeThis(this);
    SafePointer<Component> safeSource(showfrom);

    auto callback = [safeThis,changroup,chindex,totalins,safeSource](GenericItemChooser* chooser,int index) mutable {
        if (!safeThis || index == 0) return;
        // jlcc
        auto & selitem = chooser->getItems().getReference(index);
        auto dclitem = std::dynamic_pointer_cast<CmdListItemData>(selitem.userdata);
        if (dclitem) {
            // this is restore
            safeThis->processor.restoreLayoutFormatForRemotePeer(safeThis->mPeerIndex);
            safeThis->rebuildChannelViews(true);
            return;
        }

        int newcount = index;
        int chstart = 0;
        int chcnt;
        safeThis->processor.getRemotePeerChannelGroupStartAndCount(safeThis->mPeerIndex, changroup, chstart, chcnt);
        int changroups = safeThis->processor.getRemotePeerChannelGroupCount(safeThis->mPeerIndex);

        // set the new channel count for this group
        if (chindex == chstart) {
            // this was the first in the group, just adjust the count
            chcnt = newcount;
            safeThis->processor.setRemotePeerChannelGroupStartAndCount(safeThis->mPeerIndex, changroup, chstart, chcnt);
        }
        else {
            // need to split this group, by inserting a new one
            int ncnt = jmax(1, chindex - chstart);
            safeThis->processor.setRemotePeerChannelGroupStartAndCount(safeThis->mPeerIndex, changroup, chstart, ncnt);
            chcnt = newcount;
            changroup++;
            safeThis->processor.insertRemotePeerChannelGroup(safeThis->mPeerIndex, changroup, chindex, chcnt);
            changroups++;
        }

        // now we need to possibly delete the following groups that were within the range, or insert a new one
        int endch = chindex + chcnt;

        if (changroup == changroups-1) {
            if (endch < totalins) {
                // this was the last one, possibly insert one
                int nlen = totalins - endch;
                safeThis->processor.insertRemotePeerChannelGroup(safeThis->mPeerIndex, changroup+1, endch, nlen);
                changroups++;
            }
        }
        else {
            for (int cgi=changroup+1; cgi < changroups; ++cgi) {
                int nchstart=0, nchcnt=0;
                safeThis->processor.getRemotePeerChannelGroupStartAndCount(safeThis->mPeerIndex, cgi, nchstart, nchcnt);
                if (nchstart < endch) {
                    // the new one overlaps
                    if (nchstart + nchcnt <= endch) {
                        // fully subsumed, remove it
                        safeThis->processor.removeRemotePeerChannelGroup(safeThis->mPeerIndex, cgi);
                        changroups--;
                        cgi--;
                    }
                    else {
                        // partially subsumed, adjust it and done
                        int adjstart = endch;
                        int adjlen = (nchstart + nchcnt) - adjstart;
                        safeThis->processor.setRemotePeerChannelGroupStartAndCount(safeThis->mPeerIndex, cgi, adjstart, adjlen);

                        break;
                    }
                } else if (endch < nchstart) {
                    // the new one leaves a gap, insert one to fill it
                    int nlen = nchstart - endch;
                    safeThis->processor.insertRemotePeerChannelGroup(safeThis->mPeerIndex, cgi, endch, nlen);
                    changroups++;

                    break;
                }
            }
        }

        safeThis->processor.setRemotePeerChannelGroupCount(safeThis->mPeerIndex, changroups);


        safeThis->rebuildChannelViews(true);

        Timer::callAfterDelay(100, [safeSource](){
            if (safeSource) {
                safeSource->grabKeyboardFocus();
            }
        });
    };

    GenericItemChooser::launchPopupChooser(items, bounds, dw, callback, selindex, dw ? dw->getHeight()-30 : 0);
}

int ChannelGroupsView::getChanGroupFromIndex(int index)
{
    if (index >= 0 && index < mChannelViews.size()) {
        return mChannelViews.getUnchecked(index)->group;
    }

    return 0;
}

int ChannelGroupsView::getChanGroupForPoint(Point<int> pos, bool inbetween)
{
    int i=0;
    for (; i < mChanGroupBounds.size(); ++i) {
        auto bounds = mChanGroupBounds.getUnchecked(i);

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

Rectangle<int> ChannelGroupsView::getBoundsForChanGroup(int chgroup)
{
    if (chgroup >= 0 && chgroup < mChanGroupBounds.size()) {
        return mChanGroupBounds.getUnchecked(chgroup);
    }
    // otherwise return a line after the last of them
    if (!mChanGroupBounds.isEmpty()) {
        auto lastone = mChanGroupBounds.getLast();
        return Rectangle<int>(lastone.getX(), lastone.getBottom(), lastone.getWidth(), 0);
    }
    return {};
}



void ChannelGroupsView::peerChanButtonPressed(Component * source, int index, bool newlinkstate)
{
    // jlc  show selector to change the target start channel for this group
    Array<GenericItemChooserItem> items;



}

void ChannelGroupsView::inputButtonPressed(Component * source, int index, bool newlinkstate)
{
    // jlc  show selector to change the target start channel for this group
    Array<GenericItemChooserItem> items;


    int chstart=0, chcnt=0;
    int totalins = 0;

    int changroup =  getChanGroupFromIndex(index);

    if (mPeerMode) {
        totalins = processor.getRemotePeerRecvChannelCount(mPeerIndex);
        processor.getRemotePeerChannelGroupStartAndCount(mPeerIndex, changroup, chstart, chcnt);
    }
    else {
        totalins = processor.getTotalNumInputChannels();
        processor.getInputGroupChannelStartAndCount(changroup, chstart, chcnt);
    }

    // for each number of channel counts possible (1-chcnt)
    int selindex = -1;

    items.add(GenericItemChooserItem(chcnt > 1 ? TRANS("SELECT INPUTS:") : TRANS("SELECT INPUT:"), {}, nullptr, false, true));


    StringArray inputnames;
    if (JUCEApplicationBase::isStandaloneApp() && getAudioDeviceManager()) {
        auto adm = getAudioDeviceManager();
        if (auto cad = adm->getCurrentAudioDevice()) {
            auto activeIns = cad->getActiveInputChannels();
            auto allinputnames = cad->getInputChannelNames();
            int ind = 0;
            for ( int ni=0; ni < allinputnames.size(); ++ni ) {
                if (activeIns[ni]) {
                    String name;
                    //name << ind + 1 << ": " << allinputnames[ni];
                    name << allinputnames[ni];
                    inputnames.add(name);
                    ++ind;
                }
            }
        }
    }
    else {
        for (int i=0; i < totalins; ++i) {
            int busnum = 0;
            auto bch = processor.getOffsetInBusBufferForAbsoluteChannelIndex(true, i, busnum);
            String chname;
            String name;
            if (auto bus = processor.getBus(true, busnum)) {
                chname = bus->getName();
                //name << i+1 << ": " << chname << " " << bch+1;
                name << chname << " " << bch+1;
            } else {
                name << i+1;
            }

            inputnames.add(name);
        }
    }


    int ind = 1;
    for (int cc=chcnt; cc <= jmin( chcnt, totalins); ++cc) {
        for (int i=0; i < totalins - (cc - 1); ++i) {
            String name;

            if (cc == 1) {
                if (mPeerMode) {
                    name << i+1;
                } else {
                    if (i < inputnames.size()) {
                        name << "[" << i+1 << "] " << inputnames[i];
                    }
                    else {
                        name << i+1;
                    }
                }
            }
            else if (cc > 1) {

                if (mPeerMode) {
                    name << i+1;
                } else {
                    if (i+cc-1 < inputnames.size()) {
                        name << "[" << i+1 << "-" << i+cc << "] " << inputnames[i] << " - " << inputnames[i+cc-1];
                    }
                    else {
                        name << i+1 << " - " << i+cc;
                    }
                }
            }

            auto udata = std::make_shared<DestChannelListItemData>(i, cc);
            items.add(GenericItemChooserItem(name, Image(), udata, ind==1));

            if (i == chstart && cc == chcnt) {
                selindex = ind;
            }
            ++ind;
        }
    }

    items.add(GenericItemChooserItem(TRANS("CHANGE LAYOUT..."), {}, nullptr, true));
    items.add(GenericItemChooserItem(TRANS("REMOVE")));



    Component* dw = source->findParentComponentOfClass<AudioProcessorEditor>();
    if (!dw) dw = source->findParentComponentOfClass<Component>();
    Rectangle<int> bounds =  dw->getLocalArea(nullptr, source->getScreenBounds());

    SafePointer<ChannelGroupsView> safeThis(this);
    SafePointer<Component> safeSource(source);

    auto callback = [safeThis,changroup,inputnames, safeSource](GenericItemChooser* chooser,int index) mutable {
        // first ignore
        auto & items = chooser->getItems();
        if (!safeThis) return;
        if (index == 0) return;
        if (index == items.size()-1) {
            // last is remove
            if (safeThis->mPeerMode) {
                int groupcount = safeThis->processor.getRemotePeerChannelGroupCount(safeThis->mPeerIndex);
                if (safeThis->processor.removeRemotePeerChannelGroup(safeThis->mPeerIndex, changroup)) {
                    safeThis->processor.setRemotePeerChannelGroupCount(safeThis->mPeerIndex, groupcount-1);
                }
            }
            else {
                int groupcount = safeThis->processor.getInputGroupCount();
                if (safeThis->processor.removeInputChannelGroup(changroup)) {
                    safeThis->processor.setInputGroupCount(groupcount-1);
                    safeThis->processor.updateRemotePeerUserFormat();
                }
            }

            safeThis->rebuildChannelViews(true);
            return;
        }
        else if (index == items.size()-2 && safeSource) {
            // second to last is change size
            safeThis->showChangeGroupChannels(changroup, safeSource);
            return;
        }

        auto & selitem = items.getReference(index);
        auto dclitem = std::dynamic_pointer_cast<DestChannelListItemData>(selitem.userdata);
        if (!dclitem) {
            DBG("Error getting user data");
            return;
        }

        // change src chan stuff

        if (safeThis->mPeerMode) {
            safeThis->processor.setRemotePeerChannelGroupStartAndCount(safeThis->mPeerIndex, changroup, dclitem->startIndex, dclitem->count);
        }
        else {
            safeThis->processor.setInputGroupChannelStartAndCount(changroup, dclitem->startIndex, dclitem->count);
            //if (safeThis->processor.getInputGroupName(changroup).isEmpty()) {
            //    safeThis->processor.setInputGroupName(changroup, selitem.name);
            //}
            safeThis->processor.updateRemotePeerUserFormat();
        }

        safeThis->updateChannelViews();
        safeThis->updateLayout();
        safeThis->resized();

        Timer::callAfterDelay(100, [safeSource](){
            if (safeSource) {
                safeSource->grabKeyboardFocus();
            }
        });
    };

    GenericItemChooser::launchPopupChooser(items, bounds, dw, callback, selindex, dw ? dw->getHeight()-30 : 0);

}

void ChannelGroupsView::showEffects(int index, bool flag, Component * fromView)
{
    if (flag && effectsCalloutBox == nullptr) {
        
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
        
        int defWidth = 260;
#if JUCE_IOS || JUCE_ANDROID
        int defHeight = 180;
#else
        int defHeight = 156;
#endif

        if (!mEffectsView) {
            mEffectsView = std::make_unique<ChannelGroupEffectsView>(processor, mPeerMode);
            mEffectsView->addListener(this);
        }

        auto minbounds = mEffectsView->getMinimumContentBounds();
        defWidth = minbounds.getWidth();
        defHeight = minbounds.getHeight();
        

        int extrawidth = 0;
        if (defHeight > dw->getHeight() - 24) {
            extrawidth = wrap->getScrollBarThickness() + 1;
        }

        wrap->setSize(jmin(defWidth + extrawidth, dw->getWidth() - 10), jmin(defHeight, dw->getHeight() - 24));

        
        mEffectsView->setBounds(Rectangle<int>(0,0,defWidth,defHeight));

        mEffectsView->peerMode = mPeerMode;
        mEffectsView->peerIndex = mPeerIndex;
        mEffectsView->groupIndex = index;

        mEffectsView->updateState();

        wrap->setViewedComponent(mEffectsView.get(), false);
        mEffectsView->setVisible(true);
        
        Rectangle<int> bounds =  dw->getLocalArea(nullptr, fromView->getScreenBounds());
        DBG("effect callout bounds: " << bounds.toString());
        effectsCalloutBox = & CallOutBox::launchAsynchronously (std::move(wrap), bounds , dw, false);
        if (CallOutBox * box = dynamic_cast<CallOutBox*>(effectsCalloutBox.get())) {
            box->setDismissalMouseClicksAreAlwaysConsumed(true);
        }
        mEffectsView->grabKeyboardFocus();
    }
    else {
        // dismiss it
        if (CallOutBox * box = dynamic_cast<CallOutBox*>(effectsCalloutBox.get())) {
            box->dismiss();
            effectsCalloutBox = nullptr;
        }
    }
}

void ChannelGroupsView::showMonitorEffects(int index, bool flag, Component * fromView)
{
    if (flag && monEffectsCalloutBox == nullptr) {

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

        int defWidth = 260;
#if JUCE_IOS || JUCE_ANDROID
        int defHeight = 180;
#else
        int defHeight = 156;
#endif

        if (!mMonEffectsView) {
            mMonEffectsView = std::make_unique<ChannelGroupMonitorEffectsView>(processor, mPeerMode);
            mMonEffectsView->addListener(this);
        }

        mMonEffectsView->peerMode = mPeerMode;
        mMonEffectsView->peerIndex = mPeerIndex;
        mMonEffectsView->groupIndex = index;

        auto minbounds = mMonEffectsView->getMinimumContentBounds();
        defWidth = minbounds.getWidth();
        defHeight = minbounds.getHeight();


        int extrawidth = 0;
        if (defHeight > dw->getHeight() - 24) {
            extrawidth = wrap->getScrollBarThickness() + 1;
        }

        wrap->setSize(jmin(defWidth + extrawidth, dw->getWidth() - 10), jmin(defHeight, dw->getHeight() - 24));


        mMonEffectsView->updateLayout();

        mMonEffectsView->setBounds(Rectangle<int>(0,0,defWidth,defHeight));

        mMonEffectsView->updateState();

        wrap->setViewedComponent(mMonEffectsView.get(), false);
        mMonEffectsView->setVisible(true);

        Rectangle<int> bounds =  dw->getLocalArea(nullptr, fromView->getScreenBounds());
        DBG("effect callout bounds: " << bounds.toString());
        monEffectsCalloutBox = & CallOutBox::launchAsynchronously (std::move(wrap), bounds , dw, false);
        if (CallOutBox * box = dynamic_cast<CallOutBox*>(monEffectsCalloutBox.get())) {
            box->setDismissalMouseClicksAreAlwaysConsumed(true);
        }

        mMonEffectsView->setFocusContainerType(FocusContainerType::keyboardFocusContainer);

        mMonEffectsView->grabKeyboardFocus();

    }
    else {
        // dismiss it
        if (CallOutBox * box = dynamic_cast<CallOutBox*>(monEffectsCalloutBox.get())) {
            box->dismiss();
            monEffectsCalloutBox = nullptr;
        }
    }
}

void ChannelGroupsView::showInputReverbView(bool flag, Component * fromView)
{
    if (flag && inReverbCalloutBox == nullptr) {

        if (!fromView) {
            fromView = mInReverbButton.get();
        }

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

        int defWidth = 260;
#if JUCE_IOS || JUCE_ANDROID
        int defHeight = 180;
#else
        int defHeight = 156;
#endif

        if (!mInputReverbView) {
            mInputReverbView = std::make_unique<ChannelGroupReverbEffectsView>(processor);
            //mInputReverbView->addListener(this);
        }


        auto minbounds = mInputReverbView->getMinimumContentBounds();
        defWidth = minbounds.getWidth();
        defHeight = minbounds.getHeight();


        int extrawidth = 0;
        if (defHeight > dw->getHeight() - 24) {
            extrawidth = wrap->getScrollBarThickness() + 1;
        }

        wrap->setSize(jmin(defWidth + extrawidth, dw->getWidth() - 10), jmin(defHeight, dw->getHeight() - 24));


        mInputReverbView->updateLayout();

        mInputReverbView->setBounds(Rectangle<int>(0,0,defWidth,defHeight));

        mInputReverbView->updateState();

        wrap->setViewedComponent(mInputReverbView.get(), false);
        mInputReverbView->setVisible(true);

        Rectangle<int> bounds =  dw->getLocalArea(nullptr, fromView->getScreenBounds());
        DBG("in reverb callout bounds: " << bounds.toString());
        inReverbCalloutBox = & CallOutBox::launchAsynchronously (std::move(wrap), bounds , dw, false);
        if (CallOutBox * box = dynamic_cast<CallOutBox*>(inReverbCalloutBox.get())) {
            box->setDismissalMouseClicksAreAlwaysConsumed(true);
        }
        inReverbCalloutBox->setFocusContainerType(FocusContainerType::keyboardFocusContainer);
        mInputReverbView->grabKeyboardFocus();

    }
    else {
        // dismiss it
        if (CallOutBox * box = dynamic_cast<CallOutBox*>(inReverbCalloutBox.get())) {
            box->dismiss();
            inReverbCalloutBox = nullptr;
        }
    }
}

bool ChannelGroupsView::isDraggable(Component * comp) const
{
    if (comp == this || comp == mMainChannelView.get() || comp == mMainChannelView->nameLabel.get() || comp == mMainChannelView->linkButton.get()) {
        return true;
    }
    return false;
}


void ChannelGroupsView::mouseDown (const MouseEvent& event)
{
    if (mMainChannelView) {
        if (event.eventComponent == mMainChannelView->meter.get()) {
            clearClipIndicators();
            return;
        }
    }

    if (mMetChannelView) {
        if (event.eventComponent == mMetChannelView->meter.get()) {
            clearClipIndicators();
            return;
        }
    }
    if (mFileChannelView) {
        if (event.eventComponent == mFileChannelView->meter.get()) {
            clearClipIndicators();
            return;
        }
    }
    if (mSoundboardChannelView) {
        if (event.eventComponent == mSoundboardChannelView->meter.get()) {
            clearClipIndicators();
            return;
        }
    }

    for (int i=0; i < mChannelViews.size(); ++i) {
        ChannelGroupView * pvf = mChannelViews.getUnchecked(i);

        if (event.eventComponent == pvf->linkButton.get()) {
            mDraggingSourceGroup = pvf->group;
            break;
        }
        else if (event.eventComponent == pvf->meter.get() || (event.eventComponent == pvf->premeter.get())) {
            clearClipIndicators();
            break;
        }
    }
}

void ChannelGroupsView::mouseDrag (const MouseEvent& event)
{
    for (int i=0; i < mChannelViews.size(); ++i) {
        ChannelGroupView * pvf = mChannelViews.getUnchecked(i);

        if (!mPeerMode &&
            (event.eventComponent == pvf->linkButton.get()
            || event.eventComponent == pvf->nameLabel.get()
            || event.eventComponent == pvf->nameEditor.get())) {
            auto adjpos =  getLocalPoint(event.eventComponent, event.getPosition());
            DBG("Dragging link button: " << adjpos.toString());
            if (abs(event.getDistanceFromDragStartY()) > 4 && !mDraggingActive) {
                // start drag behavior
                mDraggingSourceGroup = pvf->group;
                mDraggingActive = true;
                mDraggingGroupPos = getChanGroupForPoint(adjpos, true);
                auto groupbounds = getBoundsForChanGroup(mDraggingSourceGroup);
                mDragImage = createComponentSnapshot(groupbounds);
                mDragDrawable->setImage(mDragImage);
                mDragDrawable->setVisible(true);
                mDragDrawable->setBounds(groupbounds.getX(), adjpos.getY() - groupbounds.getHeight()/2, groupbounds.getWidth(), groupbounds.getHeight());
            }
            else if (mDraggingActive) {
                // adjust drag indicator
                int changroup = getChanGroupForPoint(adjpos, true);
                DBG("In changroup: " << changroup);

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

                if (changroup != mDraggingGroupPos) {
                    // insert point changed, update it
                    mDraggingGroupPos = changroup;

                    auto groupbounds = getBoundsForChanGroup(mDraggingGroupPos);
                    groupbounds.setHeight(0);
                    groupbounds.setWidth(getWidth() - 16);
                    groupbounds.setX(7);
                    mInsertLine->setRectangle (groupbounds.toFloat());

                    int delta = mDraggingGroupPos - mDraggingSourceGroup;
                    bool canmove = delta > 1 || delta < 0;
                    mInsertLine->setVisible(canmove);
                }
            }
            break;
        }
    }
}

void ChannelGroupsView::mouseUp (const MouseEvent& event)
{
    // A click on a player's name does nothing: the row's details follow the layout buttons for all rows,
    // and the name is the handle for dragging the row.
    if (mMainChannelView && event.eventComponent == mMainChannelView->nameLabel.get()) {
        return;
    }

    for (int i=0; i < mChannelViews.size(); ++i) {
        ChannelGroupView * pvf = mChannelViews.getUnchecked(i);

        if (event.eventComponent == pvf->linkButton.get()
            || event.eventComponent == pvf->nameLabel.get()
            || event.eventComponent == pvf->nameEditor.get()) {
            if (mDraggingActive) {
                DBG("Mouse up after drag: " << event.getPosition().toString());
                // commit it
                int delta = mDraggingGroupPos - mDraggingSourceGroup;
                bool canmove = delta > 1 || delta < 0;

                if (canmove && processor.moveInputChannelGroupTo(mDraggingSourceGroup, mDraggingGroupPos)) {
                    // moved it
                    processor.updateRemotePeerUserFormat();
                    rebuildChannelViews();
                }

                mInsertLine->setVisible(false);
                mDragDrawable->setVisible(false);
                mDraggingActive = false;
                mAutoscrolling = false;
            }

            break;
        }
    }
}


void ChannelGroupsView::clearClipIndicators()
{
    for (int i=0; i < mChannelViews.size(); ++i) {
        ChannelGroupView * pvf = mChannelViews.getUnchecked(i);
        pvf->meter->clearClipIndicator(-1);
        pvf->meter->clearMaxLevelDisplay(-1);
        if (pvf->premeter) {
            pvf->premeter->clearClipIndicator(-1);
            pvf->premeter->clearMaxLevelDisplay(-1);
        }
        pvf->meter->clearClipIndicator(-1);
        pvf->meter->clearMaxLevelDisplay(-1);
    }

    if (mMainChannelView) {
        mMainChannelView->meter->clearClipIndicator();
        mMainChannelView->meter->clearMaxLevelDisplay(-1);
    }

    if (mMetChannelView) {
        mMetChannelView->premeter->clearClipIndicator();
        mMetChannelView->premeter->clearMaxLevelDisplay(-1);
        mMetChannelView->meter->clearClipIndicator();
        mMetChannelView->meter->clearMaxLevelDisplay(-1);
    }

    if (mFileChannelView) {
        mFileChannelView->premeter->clearClipIndicator();
        mFileChannelView->premeter->clearMaxLevelDisplay(-1);
        mFileChannelView->meter->clearClipIndicator();
        mFileChannelView->meter->clearMaxLevelDisplay(-1);
    }

    if (mSoundboardChannelView) {
        mSoundboardChannelView->premeter->clearClipIndicator();
        mSoundboardChannelView->premeter->clearMaxLevelDisplay(-1);
        mSoundboardChannelView->meter->clearClipIndicator();
        mSoundboardChannelView->meter->clearMaxLevelDisplay(-1);
    }

}


void ChannelGroupsView::showDestSelectionMenu(Component * source, int index)
{
    if (index >= mChannelViews.size()) return;

    ChannelGroupView * pvf = nullptr;
    bool ismet = false, isfile = false, issoundboard = false;

    if (index == -1) {
        pvf = mMetChannelView.get();
        ismet = true;
    } else if (index == -2) {
        pvf = mFileChannelView.get();
        isfile = true;
    } else if (index == -3) {
        pvf = mSoundboardChannelView.get();
        issoundboard = true;
    } else {
        pvf = mChannelViews.getUnchecked(index);
    }

    Array<GenericItemChooserItem> items;
    items.add(GenericItemChooserItem(TRANS("SELECT MONITOR OUT:"), {}, nullptr, false, true));

    int chstart=0, chcnt=0;
    int totalouts = 0;
    int destst=0, destcnt=0;

    int changroup =  getChanGroupFromIndex(index);
    int maxchcnt = 0;
    const int peerGroupCount = mPeerMode ? processor.getRemotePeerChannelGroupCount(mPeerIndex) : 0;
    // The collapsed player's button moves all of its instruments together.
    const bool applyToAllGroups = mPeerMode && peerGroupCount > 1 && mMainChannelView && source == mMainChannelView->destButton.get();

    if (mPeerMode) {
        totalouts = processor.getTotalNumOutputChannels();
        processor.getRemotePeerChannelGroupStartAndCount(mPeerIndex, changroup, chstart, chcnt);
        processor.getRemotePeerChannelGroupDestStartAndCount(mPeerIndex, changroup, destst, destcnt);
        chcnt = jmin(chcnt, totalouts);
        maxchcnt = chcnt;
        if (applyToAllGroups) {
            chcnt = maxchcnt = jmin(2, totalouts);
            destcnt = 0;   // a Mixed player has no single current choice to highlight
            bool sameOutput = true;
            int firstst = 0, firstcnt = 0;
            processor.getRemotePeerChannelGroupDestStartAndCount(mPeerIndex, 0, firstst, firstcnt);
            for (int g = 1; g < peerGroupCount; ++g) {
                int st = 0, cnt = 0;
                processor.getRemotePeerChannelGroupDestStartAndCount(mPeerIndex, g, st, cnt);
                sameOutput = sameOutput && st == firstst && cnt == firstcnt;
            }
            if (sameOutput) { destst = firstst; destcnt = firstcnt; }
        }
    }
    else {
        // input mode
        totalouts = processor.getTotalNumOutputChannels();
        if (ismet) {
            chcnt = maxchcnt = 1;
            processor.getMetronomeChannelDestStartAndCount(destst, destcnt);
        }
        else if (isfile) {
            processor.getFilePlaybackDestStartAndCount(destst, destcnt);
            chcnt = processor.getFilePlaybackMeterSource().getNumChannels();
            maxchcnt = chcnt;
            chcnt = jmin(2, chcnt, totalouts);
        }
        else if (issoundboard) {
            processor.getSoundboardProcessor()->getDestStartAndCount(destst, destcnt);
            chcnt = processor.getSoundboardProcessor()->getMeterSource().getNumChannels();
            maxchcnt = chcnt;
            chcnt = jmin(2, chcnt, totalouts);
        }
        else {
            processor.getInputGroupChannelStartAndCount(changroup, chstart, chcnt);
            processor.getInputGroupChannelDestStartAndCount(changroup, destst, destcnt);
            maxchcnt = chcnt = jmin(chcnt, totalouts);
        }
    }


    StringArray outputnames;
    if (JUCEApplicationBase::isStandaloneApp() && getAudioDeviceManager()) {
        auto adm = getAudioDeviceManager();
        if (auto cad = adm->getCurrentAudioDevice()) {
            auto actives = cad->getActiveOutputChannels();
            auto allnames = cad->getOutputChannelNames();
            int ind = 0;
            for ( int ni=0; ni < allnames.size(); ++ni ) {
                if (actives[ni]) {
                    String name;
                    //name << "[" << ind + 1 << "] " << allnames[ni];
                    name << allnames[ni];
                    outputnames.add(name);
                    ++ind;
                }
            }
        }
    }
    else {
        for (int i=0; i < totalouts; ++i) {
            int busnum = 0;
            auto bch = processor.getOffsetInBusBufferForAbsoluteChannelIndex(false, i, busnum);
            String chname;
            String name;
            if (auto bus = processor.getBus(false, busnum)) {
                chname = bus->getName();
                //name << "[" << i+1 << "] " << chname << " " << bch+1;
                name << chname << " " << bch+1;
            } else {
                name << i+1;
            }

            outputnames.add(name);
        }
    }


    // for each number of channel counts possible (1-chcnt)
    int selindex = -1;

    int ind = 1;
    for (int cc=chcnt; cc <= jmin( jmax(2, maxchcnt), totalouts); ++cc) {
        for (int i=0; i < totalouts - (cc - 1); ++i) {
            String name;
            if (cc == 1) {
                if (i < outputnames.size()) {
                    name << "[" << i+1 << "] " << outputnames[i];
                    //name << outputnames[i];
                } else {
                    name << i+1;
                }
            } else {
                if (i+cc-1 < outputnames.size()) {
                    name << "[" << i+1 << "-" << i+cc <<  "] " << outputnames[i] << " - " << outputnames[i+cc-1];
                    //name << outputnames[i] << " - " << outputnames[i+cc-1];
                } else {
                    name << i+1 << " - " << i+cc;
                }
            }
            auto udata = std::make_shared<DestChannelListItemData>(i, cc);
            items.add(GenericItemChooserItem(name, Image(), udata, i==0));

            if (i == destst && cc == destcnt) {
                selindex = ind;
            }
            ++ind;
        }
    }

    SafePointer<ChannelGroupsView> safeThis(this);
    SafePointer<Component> safeSource(source);

    auto callback = [safeThis,safeSource,changroup,ismet,isfile,issoundboard,applyToAllGroups,peerGroupCount](GenericItemChooser* chooser,int index) mutable {
        auto & items = chooser->getItems();
        auto & selitem = items.getReference(index);
        auto dclitem = std::dynamic_pointer_cast<DestChannelListItemData>(selitem.userdata);
        if (!dclitem) {
            DBG("Error getting user data");
            return;
        }

        // change src chan stuff

        if (ismet) {
            safeThis->processor.setMetronomeChannelDestStartAndCount(dclitem->startIndex, dclitem->count);
        }
        else if (isfile) {
            safeThis->processor.setFilePlaybackDestStartAndCount(dclitem->startIndex, dclitem->count);
        }
        else if (issoundboard) {
            safeThis->processor.getSoundboardProcessor()->setDestStartAndCount(dclitem->startIndex, dclitem->count);
        }
        else if (safeThis->mPeerMode && applyToAllGroups) {
            for (int g = 0; g < peerGroupCount; ++g)
                safeThis->processor.setRemotePeerChannelGroupDestStartAndCount(safeThis->mPeerIndex, g, dclitem->startIndex, dclitem->count);
        }
        else if (safeThis->mPeerMode) {
            safeThis->processor.setRemotePeerChannelGroupDestStartAndCount(safeThis->mPeerIndex, changroup, dclitem->startIndex, dclitem->count);
        }
        else {
            safeThis->processor.setInputGroupChannelDestStartAndCount(changroup, dclitem->startIndex, dclitem->count);
        }

        safeThis->updateChannelViews();
        safeThis->updateLayout();
        safeThis->resized();

        Timer::callAfterDelay(100, [safeSource](){
            if (safeSource) {
                safeSource->grabKeyboardFocus();
            }
        });
    };


    Component* dw = source->findParentComponentOfClass<AudioProcessorEditor>();
    if (!dw) dw = source->findParentComponentOfClass<Component>();

    Rectangle<int> bounds =  dw->getLocalArea(nullptr, source->getScreenBounds());
    
    GenericItemChooser::launchPopupChooser(items, bounds, dw, callback, selindex, dw ? dw->getHeight()-30 : 0);
}

void ChannelGroupsView::genericItemChooserSelected(GenericItemChooser *comp, int index)
{

}

void ChannelGroupsView::nameLabelChanged (int changroup, const String & name) {

    if (mPeerMode) {
        processor.setRemotePeerChannelGroupName(mPeerIndex, changroup, name);
    }
    else {
        if (processor.getInputGroupName(changroup) != name) {
            processor.setInputGroupName(changroup, name);
            processor.updateRemotePeerUserFormat();
        }
    }
}


void ChannelGroupsView::effectsEnableChanged(ChannelGroupEffectsView *comp)
{
    updateChannelViews();
}

void ChannelGroupsView::monitorEffectsEnableChanged(ChannelGroupMonitorEffectsView *comp)
{
    updateChannelViews();
}



void ChannelGroupsView::sliderValueChanged (Slider* slider)
{

    if (mPeerMode) {
        int changroup = 0;
        int chi = 0;

        if (slider == mMainChannelView->levelSlider.get()) {
            // Info-only peers have no local audio plumbing, so their level goes through the hub-mix override. All
            // others apply the fader locally: what arrives is metered and the fader only changes what we hear. In hub
            // mode the hub-side gain is reset to unity once so a stale hub attenuation cannot linger.
            if (processor.getRemotePeerInfoOnly(mPeerIndex)) {
                String name = processor.getRemotePeerUserName(mPeerIndex);
                processor.setHubMixGainForPeer(name, (float)mMainChannelView->levelSlider->getValue());
            } else {
                processor.setRemotePeerLevelGain(mPeerIndex, mMainChannelView->levelSlider->getValue());
                if (processor.isHostedMixModeEnabled()) {
                    String name = processor.getRemotePeerUserName(mPeerIndex);
                    if (std::fabs(processor.getHubMixGainForPeer(name) - 1.0f) > 1e-4f) processor.setHubMixGainForPeer(name, 1.0f);
                }
            }
            return;
        }
        else if (slider == mMainChannelView->panSlider.get() || slider == mMainChannelView->widthSlider.get()) {
            const bool isPan = slider == mMainChannelView->panSlider.get();
            if (mMainChannelView->panMode == ChannelGroupView::PanPair) {
                applyPairKnobs(mMainChannelView.get(), changroup);
            }
            else if (isPan && mMainChannelView->panMode == ChannelGroupView::PanSingle) {
                processor.setRemotePeerChannelPan(mPeerIndex, changroup, chi, mMainChannelView->panSlider->getValue());
            }
            else if (isPan && mMainChannelView->panMode == ChannelGroupView::PanBalance) {
                processor.setRemotePeerBalance(mPeerIndex, (float) mMainChannelView->panSlider->getValue());
            }
            return;
        }

        for (int i=0; i < mChannelViews.size(); ++i)
        {
            ChannelGroupView * pvf = mChannelViews.getUnchecked(i);
            changroup = pvf->group;
            chi = pvf->chanIndex;

            if (pvf->levelSlider.get() == slider) {
                processor.setRemotePeerChannelGain(mPeerIndex, changroup, pvf->levelSlider->getValue());
                break;
            }
            else if (pvf->monitorSlider.get() == slider) {
                //processor.setRemotePeerChanel (mPeerIndex, changroup, pvf->levelSlider->getValue());
                break;
            }
            else if (pvf->panSlider.get() == slider || pvf->widthSlider.get() == slider) {
                if (pvf->panMode == ChannelGroupView::PanPair) {
                    applyPairKnobs(pvf, changroup);
                }
                else if (pvf->panSlider.get() == slider && pvf->panMode == ChannelGroupView::PanSingle) {
                    processor.setRemotePeerChannelPan(mPeerIndex, changroup, chi, pvf->panSlider->getValue());
                }
                break;
            }

        }
    }
    else {



        for (int i=0; i < mChannelViews.size(); ++i)
        {

            ChannelGroupView * pvf = mChannelViews.getUnchecked(i);
            int changroup = pvf->group;
            int chi = pvf->chanIndex;

            if (pvf->levelSlider.get() == slider) {
                processor.setInputGroupGain(changroup, pvf->levelSlider->getValue());
                break;
            }
            else if (pvf->monitorSlider.get() == slider) {
                processor.setInputMonitor(changroup, pvf->monitorSlider->getValue());
                break;
            }
            else if (pvf->panSlider.get() == slider || pvf->widthSlider.get() == slider) {
                if (pvf->panMode == ChannelGroupView::PanPair) {
                    float left = 0.0f, right = 0.0f;
                    pairPansFromKnobs(pvf, left, right);
                    processor.setInputChannelPan(changroup, 0, left);
                    processor.setInputChannelPan(changroup, 1, right);
                }
                else if (pvf->panSlider.get() == slider) {
                    processor.setInputChannelPan(changroup, chi, pvf->panSlider->getValue());
                }
                break;
            }

        }

    }

}
