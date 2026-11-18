// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2020 Jesse Chappell


#pragma once

#include "JuceHeader.h"

#include "SonobusPluginProcessor.h"
#include "SonoLookAndFeel.h"
#include "SonoChoiceButton.h"
#include "SonoDrawableButton.h"
#include "GenericItemChooser.h"
#include "CompressorView.h"
#include "ExpanderView.h"
#include "ParametricEqView.h"
#include "MonitorDelayView.h"
#include "ReverbSendView.h"
#include "PolarityInvertView.h"
#include "ReverbView.h"

class ChannelGroupEffectsView :
public Component,
public Timer,   // Gate open/closed indicator refresh
public CompressorView::Listener,
public ExpanderView::Listener,
public ParametricEqView::Listener,
public ReverbSendView::Listener,
public PolarityInvertView::Listener,
public EffectsBaseView::HeaderListener
{
public:
    ChannelGroupEffectsView(SonobusAudioProcessor& proc, bool peermode=false);
    virtual ~ChannelGroupEffectsView();


    class Listener {
    public:
        virtual ~Listener() {}
        virtual void effectsEnableChanged(ChannelGroupEffectsView *comp) {}
    };

    void addListener(Listener * listener) { listeners.add(listener); }
    void removeListener(Listener * listener) { listeners.remove(listener); }


    juce::Rectangle<int> getMinimumContentBounds() const;


    void updateState();

    void updateStateForRemotePeer();

    void updateStateForInput();
    void timerCallback() override;   // Gate indicator refresh
    void updateLayout();

    void resized() override;


    void compressorParamsChanged(CompressorView *comp, SonoAudio::CompressorParams & params) override;

    void expanderParamsChanged(ExpanderView *comp, SonoAudio::CompressorParams & params) override;
    void parametricEqParamsChanged(ParametricEqView *comp, SonoAudio::ParametricEqParams & params) override;

    void reverbSendLevelChanged(ReverbSendView *comp, float revlevel) override;

    void polarityInvertChanged(PolarityInvertView *comp, bool polinv) override;

    void effectsHeaderClicked(EffectsBaseView *comp) override;


    int  groupIndex = 0;
    bool peerMode = false;
    int  peerIndex = 0;
    bool firstShow = true;

protected:

    SonobusAudioProcessor& processor;

    ListenerList<Listener> listeners;

    std::unique_ptr<ConcertinaPanel> effectsConcertina;

    std::unique_ptr<CompressorView> compressorView;


    std::unique_ptr<ExpanderView> expanderView;

    std::unique_ptr<ParametricEqView> eqView;

    std::unique_ptr<ReverbSendView> reverbSendView;

    std::unique_ptr<PolarityInvertView> polarityInvertView;

    FlexBox effectsBox;

    juce::Rectangle<int> minBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelGroupEffectsView)

};

class ChannelGroupMonitorEffectsView :
public Component,
public ParametricEqView::Listener,
public MonitorDelayView::Listener,
public ReverbSendView::Listener,
public EffectsBaseView::HeaderListener
{
public:
    ChannelGroupMonitorEffectsView(SonobusAudioProcessor& proc, bool peermode=false);
    virtual ~ChannelGroupMonitorEffectsView();


    class Listener {
    public:
        virtual ~Listener() {}
        virtual void monitorEffectsEnableChanged(ChannelGroupMonitorEffectsView *comp) {}
    };

    void addListener(Listener * listener) { listeners.add(listener); }
    void removeListener(Listener * listener) { listeners.remove(listener); }


    juce::Rectangle<int> getMinimumContentBounds() const;


    void updateState();

    void updateStateForRemotePeer();
    void updateStateForInput();

    void updateLayout();

    void updateLayoutForRemotePeer();
    void updateLayoutForInput();

    void resized() override;

    void monitorDelayParamsChanged(MonitorDelayView *comp, SonoAudio::DelayParams &params) override;

    void reverbSendLevelChanged(ReverbSendView *comp, float revlevel) override;

    void effectsHeaderClicked(EffectsBaseView *comp) override;


    int  groupIndex = 0;
    bool peerMode = false;
    int  peerIndex = 0;
    bool firstShow = true;

protected:

    SonobusAudioProcessor& processor;

    ListenerList<Listener> listeners;

    std::unique_ptr<ConcertinaPanel> effectsConcertina;

    std::unique_ptr<MonitorDelayView> delayView;

    std::unique_ptr<ReverbSendView> reverbSendView;


    FlexBox effectsBox;

    juce::Rectangle<int> minBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelGroupMonitorEffectsView)

};

class ChannelGroupReverbEffectsView :
public Component,
public ReverbView::Listener,
public EffectsBaseView::HeaderListener
{
public:
    ChannelGroupReverbEffectsView(SonobusAudioProcessor& proc);
    virtual ~ChannelGroupReverbEffectsView();


    class Listener {
    public:
        virtual ~Listener() {}
        virtual void reverbEffectsEnableChanged(ChannelGroupReverbEffectsView *comp) {}
    };

    void addListener(Listener * listener) { listeners.add(listener); }
    void removeListener(Listener * listener) { listeners.remove(listener); }


    juce::Rectangle<int> getMinimumContentBounds() const;


    void updateState();

    void updateLayout();

    void resized() override;

    //void reverbSendLevelChanged(ReverbSendView *comp, float revlevel) override;

    void effectsHeaderClicked(EffectsBaseView *comp) override;

    bool firstShow = true;

protected:

    SonobusAudioProcessor& processor;

    ListenerList<Listener> listeners;

    std::unique_ptr<ConcertinaPanel> effectsConcertina;

    std::unique_ptr<ReverbView> reverbView;


    FlexBox effectsBox;

    juce::Rectangle<int> minBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelGroupReverbEffectsView)

};


class ChannelGroupView : public Component {
public:
    ChannelGroupView();
    virtual ~ChannelGroupView();
    
    void paint(Graphics& g) override;
    void resized() override;
    
    //void updateLayout();

    int group = 0;
    int chanIndex = 0;
    int groupChanCount = 0;

    bool showDivider = false;

    SonoBigTextLookAndFeel smallLnf;
    SonoBigTextLookAndFeel medLnf;
    SonoBigTextLookAndFeel sonoSliderLNF;
    SonoPanSliderLookAndFeel panSliderLNF;

    SonoLookAndFeel rmeterLnf;
    SonoLookAndFeel smeterLnf;
    
    std::unique_ptr<Label> nameLabel;
    std::unique_ptr<TextEditor> nameEditor;
    std::unique_ptr<TextButton> muteButton;
    std::unique_ptr<TextButton> soloButton;
    std::unique_ptr<TextButton> fxButton;
    std::unique_ptr<TextButton> monfxButton;
    std::unique_ptr<Label>  chanLabel;
    std::unique_ptr<Label>  levelLabel;
    std::unique_ptr<Slider> levelSlider;
    std::unique_ptr<Slider> monitorSlider;
    std::unique_ptr<Label>  panLabel;
    std::unique_ptr<Slider> panSlider;
    std::unique_ptr<Slider> widthSlider;   // player rows: stereo width of a pair, beside its pan knob
    std::unique_ptr<SonoDrawableButton> linkButton;
    std::unique_ptr<SonoDrawableButton> monoButton;
    std::unique_ptr<SonoDrawableButton> destButton;


    bool singlePanner = true;

    // Player rows: what the pan knob moves
    enum PanMode { PanNone = 0, PanSingle, PanPair, PanBalance };   // PanBalance: the whole mix of a player with several instruments
    int panMode = PanNone;
    bool pairSwapped = false;   // a pair stored right-of-left keeps that order

    // A player's header row, and each instrument row of an expanded player, is laid out by hand in one of these
    // forms, with the same columns, so every level slider starts and ends at the same place:
    //   wide       name, level, knobs, output on one line; status and buttons under it
    //   medium     name and meter; level, output, knobs; status and buttons
    //   narrow     as medium, the output on a line of its own at the bottom
    //   narrowTall as narrow, the knobs beside the level only (the status line needs the whole width)
    enum HeaderForm { HeaderWide = 0, HeaderMedium, HeaderNarrow, HeaderNarrowTall };
    struct HeaderLayout {
        bool active = false;
        int form = HeaderMedium;
        int nameCol = 110;      // wide: name column, the same on every row
        int destCol = 0;        // wide and medium: output column, the same on every row (0 = none)
        int destOwn = 44;       // narrow: this row's output button at its own width
        int knobCol = 48;       // knob column, the same on every row
        int statW = 0;          // status text slot at the start of the lower line (0 = none)
        int buttonW = 52;
        int meterW = 0;
        int meterCol = 0;       // wide: room kept for the meter when it is wider than this row's own (0 = its own)
        int meterSlack = 0;     // wide: how much narrower this row's meter beside it is than the widest (the row is wider)
        bool showMeter = false;
        bool showDest = true;
        bool lower = true;      // the lower line (MUTE, SOLO, FX); off for a channel row inside an instrument
    };
    HeaderLayout header;
    juce::Rectangle<int> statSlot;   // kept free for the owner's status text
    juce::Rectangle<int> noteSlot;   // what is left of the lower line after the buttons
    void layoutHeader();
    static int headerHeight (const HeaderLayout &);

    static constexpr int kHeaderTop = 4, kHeaderLineH = 34, kHeaderLowerH = 26, kHeaderButtonH = 24, kHeaderGap = 2;
    static constexpr int kHeaderKnobH = kHeaderLineH + kHeaderGap + kHeaderLowerH + 1;
    static constexpr int kHeaderIndent = 53;   // link button and its gap: where the name and the level start

    std::unique_ptr<foleys::LevelMeter> meter;
    std::unique_ptr<foleys::LevelMeter> premeter;
    std::unique_ptr<foleys::LevelMeter> premeter2;   // second bar of a stereo pair's single row
    std::unique_ptr<Label> inCaption;    // "IN" / "OUT" over the pre- and post-fader meters of an input row
    std::unique_ptr<Label> outCaption;

    
    FlexBox mainbox;
    FlexBox maincontentbox;
    FlexBox inbox;
    FlexBox monbox;
    FlexBox namebox;
    FlexBox levelbox;
    FlexBox pannerbox;

    FlexBox linkedchannelsbox;

    
    bool  useBgColor = false;
    Colour bgColor;
    Colour borderColor;
    Colour itemColor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelGroupView)
};



class ChannelGroupsView : public Component,
public Button::Listener,
public Slider::Listener,
public SonoChoiceButton::Listener,
public GenericItemChooser::Listener,
public ChannelGroupEffectsView::Listener,
public ChannelGroupMonitorEffectsView::Listener,
public MultiTimer
{
public:
    ChannelGroupsView(SonobusAudioProcessor&, bool peerMode, int peerIndex=0);
    virtual ~ChannelGroupsView();


    class Listener {
    public:
        virtual ~Listener() {}
        virtual void channelLayoutChanged(ChannelGroupsView *comp) {}
        virtual void nameLabelClicked(ChannelGroupsView *comp) {}
    };

    void addListener(Listener * listener) { listeners.add(listener); }
    void removeListener(Listener * listener) { listeners.remove(listener); }



    void paint(Graphics & g) override;
    
    void resized() override;

    void setPeerMode(bool peermode, int index=0);
    bool getPeerMode() const { return mPeerMode; }
    // Relayed rows draw a full-height meter outside this view; hide the inline one
    void setRowMeterHidden(bool f) { mHideRowMeter = f; }
    const Array< juce::Rectangle<int> >& getGroupBounds() const { return mChanGroupBounds; }   // Per-group row bounds for the peer row's group meters
    bool getRowMeterHidden() const { return mHideRowMeter; }
    int getPeerIndex() const { return mPeerIndex; }

    void buttonClicked (Button* buttonThatWasClicked) override;        
    void sliderValueChanged (Slider* slider) override;
    void choiceButtonSelected(SonoChoiceButton *comp, int index, int ident) override;

    void effectsEnableChanged(ChannelGroupEffectsView *comp) override;

    void monitorEffectsEnableChanged(ChannelGroupMonitorEffectsView *comp) override;


    void mouseUp (const MouseEvent& event)  override;
    void mouseDown (const MouseEvent& event)  override;
    void mouseDrag (const MouseEvent& event)  override;


    void timerCallback(int timerId) override;

    void visibilityChanged() override;

    void setMetersActive(bool flag);

    // While the top-level window is minimised or the view is not showing on screen, meter timers run at 0 Hz.
    // Independent of metersActive (visibility within the layout): either gate alone stops them, and clearing both
    // restores the 8 Hz visible rate.
    void setMetersSuspended(bool flag);
    bool getMetersSuspended() const { return metersSuspended; }

    // Dims the input row meters while nothing is being sent.
    void setSendMutedAppearance(bool flag);

    int getGroupViewsCount() const { return mChannelViews.size(); }

    void rebuildChannelViews(bool notify=false);
    void updateChannelViews(int specific=-1);
    
    void setNarrowMode(bool flag, bool update=false) { if (isNarrow != flag) { isNarrow = flag; if (update) { updateLayout(); resized(); }} }
    bool setNarrowMode() const { return isNarrow; }
    
    void showDestSelectionMenu(Component * source, int index);

    void genericItemChooserSelected(GenericItemChooser *comp, int index) override;

    bool isDraggable(Component * comp) const;

    void clearClipIndicators();

    void toggleAllMonitorDelay();

    void setEstimatedWidth(int estwidth) { mEstimatedWidth = estwidth; }
    int getEstimatedWidth() const { return mEstimatedWidth;}

    // Player rows: the list measures every row, then gives all of them the same form and columns
    struct PeerRowMetrics {
        int nameNeed = 0;         // the name in full (expanded: also each instrument's name)
        int destNeed = 0;         // the output label in full (0 = no output button; expanded: the instruments')
        int meterWidth = 0;       // the collapsed row's meter
        bool widthKnob = false;
        bool expanded = false;
    };
    PeerRowMetrics getPeerRowMetrics() const;
    void setPeerRowLayout(int form, int nameCol, int destCol, int knobCol, int statWidth, int buttonWidth, int meterSlack = 0);
    juce::Rectangle<int> getStatSlotBounds() const;
    juce::Rectangle<int> getNoteSlotBounds() const;
    bool takeLayoutChanged() { const bool f = mLayoutChanged; mLayoutChanged = false; return f; }

    static constexpr int kKnobColSingle = 48, kKnobColPair = 90;
    static constexpr int kHeaderButtonMin = 40, kHeaderButtonMax = 52;

    juce::Rectangle<int> getMinimumContentBounds() const;

    void applyToAllSliders(std::function<void(Slider *)> & routine);

    void updateLayout(bool notify=true);


    std::function<AudioDeviceManager*()> getAudioDeviceManager; // = []() { return 0; };


protected:

    void updateLayoutForRemotePeer(bool notify=true);
    void updateLayoutForInput(bool notify=true);

    void updateMonDelayButton();

    void configLevelSlider(Slider * slider, bool monmode=false);    
    void configLabel(Label *label, int ltype);
    void configKnobSlider(Slider * slider);    

    void updateInputModeChannelViews(int specific=-1);
    void updatePeerModeChannelViews(int specific=-1);

    void nameLabelChanged(int changroup, const String & name);

    void inputButtonPressed(Component *src, int index, bool newlinkstate);
    void peerChanButtonPressed(Component *src, int index, bool newlinkstate);

    void addGroupPressed();
    void clearGroupsPressed();
    void showChangeGroupChannels(int changroup, Component * showfrom);
    void showChangePeerChannelsLayout(int changroup, Component * showfrom);


    ChannelGroupView * createChannelGroupView(bool first=false);
    void setupChildren(ChannelGroupView * pvf);

    void showPopTip(const String & message, int timeoutMs, Component * target, int maxwidth);
    void showEffects(int index, bool flag, Component * fromView=nullptr);
    void showMonitorEffects(int index, bool flag, Component * fromView=nullptr);
    void showInputReverbView(bool flag, Component * fromView=nullptr);

    int getChanGroupFromIndex(int index);
    juce::Rectangle<int> getBoundsForChanGroup(int chgroup);
    void setPairPanner(ChannelGroupView * pvf, bool stereoPair);
    // Player rows: pan and width knobs on top of the per-channel pan positions
    void configKnob(ChannelGroupView * pvf, Slider * slider, bool isWidth);
    void pairPansFromKnobs(ChannelGroupView * pvf, float & left, float & right) const;
    void syncPairKnobs(ChannelGroupView * pvf, float left, float right);
    void applyPairKnobs(ChannelGroupView * pvf, int changroup);
    String outputLabel(int deststart, int destcnt) const;
    void setDestText(ChannelGroupView * pvf, int deststart, int destcnt);
    void setDestLabel(ChannelGroupView * pvf, const String & text);
    int destWidthFor(ChannelGroupView * pvf, int minWidth) const;
    int destMinWidthFor(ChannelGroupView * pvf, int minWidth) const;   // channel part + "…" only
    bool mDestTextChanged = false;
    int getChanGroupForPoint(Point<int> pos, bool inbetween);

    SonoBigTextLookAndFeel addLnf;

    SonobusAudioProcessor& processor;

    ListenerList<Listener> listeners;

    OwnedArray<ChannelGroupView> mChannelViews;
    std::unique_ptr<ChannelGroupView> mMainChannelView; // used for peers

    std::unique_ptr<ChannelGroupView> mFileChannelView; // used for input
    std::unique_ptr<ChannelGroupView> mMetChannelView; // used for input
    std::unique_ptr<ChannelGroupView> mSoundboardChannelView; // used for init


    std::unique_ptr<ChannelGroupEffectsView> mEffectsView;
    std::unique_ptr<ChannelGroupMonitorEffectsView> mMonEffectsView;
    std::unique_ptr<ChannelGroupReverbEffectsView> mInputReverbView;



    std::unique_ptr<Slider> mInGainSlider;
    std::unique_ptr<TextButton> mAddButton;
    std::unique_ptr<TextButton> mClearButton;
    std::unique_ptr<TextButton> mInReverbButton;
    std::unique_ptr<TextButton> mMonDelayButton;

    std::unique_ptr<DrawableRectangle> mInsertLine;
    std::unique_ptr<DrawableImage> mDragDrawable;
    std::unique_ptr<DrawableRectangle> mMetFileBg;


    std::unique_ptr<BubbleMessageComponent> popTip;

    std::unique_ptr<AudioProcessorValueTreeState::ButtonAttachment> mMetSendAttachment;
    std::unique_ptr<AudioProcessorValueTreeState::ButtonAttachment> mFileSendAttachment;
    std::unique_ptr<AudioProcessorValueTreeState::ButtonAttachment> mSoundboardSendAttachment;
    std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment> mMetLevelAttachment;


    WeakReference<Component> effectsCalloutBox;
    WeakReference<Component> monEffectsCalloutBox;
    WeakReference<Component> inReverbCalloutBox;

    FlexBox channelsBox;
    FlexBox addrowBox;
    int channelMinHeight = 60;
    int channelMinWidth = 400;
    int mEstimatedWidth = 0;

    bool isNarrow = false;
    bool metersActive = false;
    bool metersSuspended = false; // Window minimised / not showing
    bool mSendMutedAppearance = false;

    bool mPeerMode = false;
    bool mHideRowMeter = false;   
    int mPeerIndex = 0;

    // Player row header, as set by the list
    int mRowForm = ChannelGroupView::HeaderMedium;
    int mRowNameCol = 110;
    int mRowDestCol = 0;
    int mRowKnobCol = kKnobColSingle;
    int mRowStatWidth = 0;
    int mRowButtonWidth = kHeaderButtonMax;
    int mRowMeterSlack = 0;
    bool mLayoutChanged = false;

    // dragging state
    bool mDraggingActive = false;
    int mDraggingSourceGroup = -1;
    int mDraggingGroupPos = -1;
    Array< juce::Rectangle<int> > mChanGroupBounds;
    Image  mDragImage;
    bool mAutoscrolling = false;

    uint32 lastUpdateTimestampMs = 0;
    
    Colour mutedBySoloColor;
    Colour mutedTextColor;
    Colour mutedColor;
    Colour soloColor;
    Colour regularTextColor;
    Colour droppedTextColor;
    Colour dimTextColor;
    Colour outlineColor;
    Colour bgColor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelGroupsView)

};
