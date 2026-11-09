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

#include "ChannelGroupsView.h"
#include "SbChannelTipMeter.h"

#include <map>

class JitterBufferMeter;

class PeerViewInfo : public Component{
public:
    PeerViewInfo();
    virtual ~PeerViewInfo();
    
    void paint(Graphics& g) override;
    void resized() override;
    
    //void updateLayout();

    SonoBigTextLookAndFeel smallLnf;
    SonoBigTextLookAndFeel medLnf;
    SonoBigTextLookAndFeel sonoSliderLNF;
    SonoPanSliderLookAndFeel panSliderLNF;

    SonoLookAndFeel rmeterLnf;
    SonoLookAndFeel smeterLnf;

    std::unique_ptr<TextEditor> addrLabel;
    std::unique_ptr<Label> staticAddrLabel;
    std::unique_ptr<ToggleButton> sendMutedButton;
    std::unique_ptr<TextButton> recvMutedButton;
    std::unique_ptr<TextButton> recvSoloButton;
    std::unique_ptr<SonoDrawableButton> latActiveButton;
    std::unique_ptr<SonoDrawableButton> sendOptionsButton;
    std::unique_ptr<SonoDrawableButton> recvOptionsButton;
    std::unique_ptr<Label>  statusLabel;
    std::unique_ptr<Label>  bufferTimeLabel;
    std::unique_ptr<Slider> bufferTimeSlider;        
    std::unique_ptr<SonoChoiceButton> autosizeButton;
    std::unique_ptr<SonoChoiceButton> formatChoiceButton;
    std::unique_ptr<ToggleButton> changeAllFormatButton;
    std::unique_ptr<TextButton> optionsResetDropButton;
    std::unique_ptr<TextButton> optionsRemoveButton;
    std::unique_ptr<TextButton> optionsBlockButton;
    std::unique_ptr<SonoChoiceButton> remoteSendFormatChoiceButton;
    std::unique_ptr<ToggleButton> changeAllRecvFormatButton;
    std::unique_ptr<SonoDrawableButton> bufferMinButton;
    std::unique_ptr<SonoDrawableButton> bufferMinFrontButton;
    std::unique_ptr<Drawable> recvButtonImage;
    std::unique_ptr<Drawable> sendButtonImage;


    
    std::unique_ptr<Component> sendOptionsContainer;
    std::unique_ptr<Component> recvOptionsContainer;

    std::unique_ptr<Label>  staticLatencyLabel;
    std::unique_ptr<Label>  latencyUpLabel;
    std::unique_ptr<Label>  latencyDownLabel;
    std::unique_ptr<Label>  staticPingLabel;
    std::unique_ptr<Label>  pingLabel;

    std::unique_ptr<Label>  staticSendQualLabel;
    std::unique_ptr<Label>  sendQualityLabel;
    std::unique_ptr<Label>  staticBufferLabel;
    std::unique_ptr<Label>  bufferLabel;

    
    std::unique_ptr<Label>  staticFormatChoiceLabel;
    std::unique_ptr<Label>  staticRemoteSendFormatChoiceLabel;

    std::unique_ptr<Label>  sendActualBitrateLabel;
    std::unique_ptr<Label>  recvActualBitrateLabel;

    // HostedMix-only per-row dropped-packet readout. Hidden and not laid out outside hosted mode.
    std::unique_ptr<Label>  hostedDropsLabel;
    std::unique_ptr<Label>  hubLatLabel;   // Relayed-row popup — to/from-hub latency (replaces address + reset-drops)
    // Drops-per-minute is a true sliding window: 12 samples at >=5-s spacing (t, count); "recent" = count now minus
    // the oldest sample inside the last 60 s.
    uint32  dropsSampleT[12] = {0};
    int64_t dropsSampleC[12] = {0};
    int     dropsSampleN = 0;
    foleys::LevelMeterSource * recvMeterSrc = nullptr;   // Rebind when the row's peer changes

    std::unique_ptr<Drawable>  sendUpArrow;
    std::unique_ptr<Drawable>  recvDownArrow;

    std::unique_ptr<Drawable>  latUpArrow;
    std::unique_ptr<Drawable>  latDownArrow;


    double fillRatio = 0.0;
    std::unique_ptr<JitterBufferMeter> jitterBufferMeter;

    std::unique_ptr<ChannelGroupsView> channelGroups;


    std::unique_ptr<DrawableRectangle> sendStatsBg;
    std::unique_ptr<DrawableRectangle> recvStatsBg;
    std::unique_ptr<DrawableRectangle> pingBg;


    std::unique_ptr<SbChannelTipMeter> recvMeter;   // Hover names the bar under the mouse
    // EXPANDED relayed rows show one meter per channel group (stereo = 2 bars side by side) in the same
    // right-hand column as the tall meter, each spanning its group's rows; the last one runs to the row's bottom.
    std::unique_ptr<Component> groupMeterColumn;     // invisible flex placeholder = the column's bounds
    OwnedArray<SbChannelTipMeter> groupMeters;
    Array<int> groupMeterChans;
    int laidOutMeterChans = -1;   // Channel count the row meter was last sized for
    std::unique_ptr<foleys::LevelMeter> sendMeter;
    
    
    FlexBox mainbox;
    FlexBox mainnarrowbox;
    FlexBox mainsendbox;
    FlexBox mainrecvbox;
    FlexBox sendbox;
    FlexBox sendmeterbox;
    FlexBox recvbox;
    FlexBox recvnetbox;
    FlexBox netstatbox;
    FlexBox pingbox;
    FlexBox latencybox;
    FlexBox recvlevelbox;
    FlexBox pannerbox;
    FlexBox latencylabbox;

    FlexBox recvOptionsBox;
    FlexBox optionsNetbufBox;
    FlexBox optionsstatbox;
    FlexBox optionsbuttbox;
    FlexBox optionsaddrbox;
    FlexBox optionsRemoteQualityBox;

    FlexBox sendOptionsBox;
    FlexBox optionsSendQualBox;
    FlexBox optionsChangeAllQualBox;
    FlexBox optionsSendMutedBox;
    
    
    FlexBox effectsBox;

    FlexBox squalbox;
    FlexBox netbufbox;
    FlexBox recvstatbox;

    FlexBox sendlabbox;
    FlexBox recvlabbox;
    // Hosted-mode drops readout line (mirrors recvlabbox).
    FlexBox dropslabbox;

    int64_t lastBytesRecv = 0;
    int64_t lastBytesSent = 0;

    int64_t lastDropped = 0;
    uint32 lastDroppedChangedTimestampMs = 0;

    uint32 stopLatencyTestTimestampMs = 0;
    bool wasRecvActiveAtLatencyTest = false;
    bool wasSendActiveAtLatencyTest = false;
    
    bool singlePanner = true;
    bool isNarrow = false;
    bool fullMode = true;
    bool infoOnly = false;
    bool addrClicked = false;
    // HostedMix: per-row receive-buffer widgets are hidden. resized() re-asserts bufferMinFrontButton visibility and
    // anchors the recv-options overlay to the buffer labels every layout pass, so it must be gated by this flag (set
    // from PeersContainerView::updateLayout, which knows the topology mode). The latency/ping cluster (netstatbox) is
    // also not laid out in hosted mode, so resized() must not anchor overlays to latActiveButton / staticBufferLabel
    // when this flag is set.
    bool hideBufferWidgets = false;
    // Hosted mode: relayed peer rows (audio reaches them via the hub, there is no per-peer upload) drop the P2P send
    // cluster: "Send Quality:" static label, send-options button (format chooser / send-mute / remove / block popup)
    // and its overlay chrome. sendQualityLabel stays and shows the channel count we receive from that peer.
    // staticSendQualLabel is not laid out on those rows, so resized() must anchor the overlays to sendQualityLabel
    // instead.
    bool hideSendCluster = false;
    
    Colour bgColor;
    Colour borderColor;
    Colour itemColor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PeerViewInfo)
};

class PendingPeerViewInfo : public Component {
public:
    PendingPeerViewInfo();
    virtual ~PendingPeerViewInfo();
    
    void paint(Graphics& g) override;
    void resized() override;
    
    std::unique_ptr<Label>  nameLabel;
    std::unique_ptr<Label>  messageLabel;
    std::unique_ptr<TextButton>  removeButton;
    std::unique_ptr<TextButton>  unblockButton;

    FlexBox mainbox;

    Colour bgColor;
    Colour borderColor;
};

// A Label that can paint per-run colours (AttributedString): the hub status line and the row counters, where the
// drops number carries a colour but the throughput next to it must not.
class SbRichLabel : public Label {
public:
    using Label::Label;
    void setRich(const AttributedString& a) { rich = a; hasRich = true; repaint(); }
    void clearRich() { if (hasRich) { hasRich = false; repaint(); } }
    void paint(Graphics& g) override {
        if (!hasRich) { Label::paint(g); return; }
        g.fillAll(findColour(Label::backgroundColourId));
        auto r = getLocalBounds().toFloat();
        r.removeFromLeft((float) getBorderSize().getLeft());
        rich.draw(g, r);
    }
private:
    AttributedString rich;
    bool hasRich = false;
};

class PeersContainerView : public Component,
public Button::Listener,
public Slider::Listener,
public SonoChoiceButton::Listener,
public GenericItemChooser::Listener,
public ChannelGroupsView::Listener,
public MultiTimer
{
public:
    PeersContainerView(SonobusAudioProcessor&);

    class Listener {
    public:
        virtual ~Listener() {}
        virtual void internalSizesChanged(PeersContainerView *comp) {}
    };
    void addListener(Listener * listener) { listeners.add(listener); }
    void removeListener(Listener * listener) { listeners.remove(listener); }

    void paint(Graphics & g) override;
    
    void resized() override;
    
    void buttonClicked (Button* buttonThatWasClicked) override;        
    void sliderValueChanged (Slider* slider) override;
    void choiceButtonSelected(SonoChoiceButton *comp, int index, int ident) override;
    
    void mouseUp (const MouseEvent& event)  override;

    void mouseDown (const MouseEvent& event)  override;
    void mouseDrag (const MouseEvent& event)  override;


    void timerCallback(int timerId) override;
    
    
    int getPeerViewCount() const { return mPeerViews.size(); }
    
    void resetPendingUsers();
    void peerPendingJoin(String & group, String & user);
    void peerFailedJoin(String & group, String & user);
    void peerBlockedJoin(String & group, String & user, String & address, int port);
    void peerLeftGroup(String & group, String & user);
    int getPendingPeerCount() const { return (int)mPendingUsers.size(); }
    
    void rebuildPeerViews();
    void updatePeerViews(int specific=-1);

    // Propagate "window minimised / not
    // showing" to every peer row's ChannelGroupsView so their meter timers
    // drop to 0 Hz; new rows inherit the current state.
    void setMetersSuspended(bool flag);

    void setNarrowMode(bool flag) { isNarrow = flag; updateLayout(); }
    bool setNarrowMode() const { return isNarrow; }
    
    void showPopupMenu(Component * source, int index);

    void genericItemChooserSelected(GenericItemChooser *comp, int index) override;
    

    // channelgroupsview
    void channelLayoutChanged(ChannelGroupsView *comp) override;
    void nameLabelClicked(ChannelGroupsView *comp) override;

    void clearClipIndicators();
    
    juce::Rectangle<int> getMinimumContentBounds() const;

    void applyToAllSliders(std::function<void(Slider *)> & routine);

    void updateLayout();

    std::function<AudioDeviceManager*()> getAudioDeviceManager; // = []() { return 0; };

    void setPeerDisplayMode(SonobusAudioProcessor::PeerDisplayMode mode);


protected:
    
    void startLatencyTest(int i);
    void stopLatencyTest(int i);
    
    void configLevelSlider(Slider * slider);    
    void configLabel(Label *label, int ltype);
    void configKnobSlider(Slider * slider);    
    
    String generateLatencyMessage(const SonobusAudioProcessor::LatencyInfo &latinfo);

    
    PeerViewInfo * createPeerViewInfo();

    PendingPeerViewInfo * createPendingPeerViewInfo();

    bool metersSuspended = false; 
    
    void showPopTip(const String & message, int timeoutMs, Component * target, int maxwidth);
    void showSendOptions(int index, bool flag, Component * fromView=nullptr);
    void showRecvOptions(int index, bool flag, Component * fromView=nullptr);
    // Popup for the unified receive-buffer controls, launched from the compact hub-header chip (see the mUnifiedBuf*
    // member comment below).
    void showUnifiedBufferOptions(bool flag, Component * fromView=nullptr);

    void updatePeerOrdering();
    int getPeerFromIndex(int index);
    juce::Rectangle<int> getBoundsForPeer(int chgroup);
    int getPeerForPoint(Point<int> pos, bool inbetween);

    ListenerList<Listener> listeners;

    OwnedArray<PeerViewInfo> mPeerViews;
    SonobusAudioProcessor& processor;

    // HostedMix: small status header shown at the top of the peer list in place of the SonoBus-Hub peer row,
    // surfacing hub connection + ping. Per-peer meters live on the rows themselves; an aggregate hub-mix meter would
    // duplicate that signal.
    std::unique_ptr<Label> mHubStatusLabel;


    // HostedMix unified receive-buffer controls. These drive the processor's unified policy across all per-sender
    // sinks: every feed rides the same hub->us network path, so one jitter-buffer value + auto mode covers every
    // player, and per-peer-row controls are hidden in this mode. One hosted header row holds the hub status text on
    // the left and a compact clickable value chip on the right. The slider + auto-mode combo live in a CallOutBox
    // popup the chip opens (same pattern as showRecvOptions). The chip shows the live median "N ms (Auto)" in auto
    // modes and the policy value in manual.
    std::unique_ptr<Label> mUnifiedBufStaticLabel;   // popup title
    std::unique_ptr<Label> mUnifiedBufLabel;         // popup live value readout
    std::unique_ptr<Slider> mUnifiedBufferTimeSlider;
    std::unique_ptr<SonoChoiceButton> mUnifiedAutosizeButton;
    std::unique_ptr<TextButton> mUnifiedBufChipButton;
    // One-click reset of the hub-wide aggregate drop count (resets every peer's packet stats, same call as the
    // per-peer Reset Dropped button). Lives in the chip popup.
    std::unique_ptr<TextButton> mUnifiedResetDropsButton;
    std::unique_ptr<Component> mUnifiedOptionsContainer;
    FlexBox hubHeaderBox;
    FlexBox unifiedOptionsBox;
    FlexBox unifiedOptionsTitleBox;
    FlexBox unifiedOptionsNetbufBox;

    // key is username, value is priority (lower is first)
    std::map<String, int> mPeerPriorityOrdering;
    
    std::vector<int> mPeerUpdateOrdering;

    std::unique_ptr<BubbleMessageComponent> popTip;

    struct PendingUserInfo {
        PendingUserInfo(const String & group_="", const String & user_="") : group(group_), user(user_) {}
        String group;
        String user;
        bool failed = false;
        bool blocked = false;
        String address;
        int    port = 0;
    };
    
    std::map<String,PendingUserInfo> mPendingUsers;

    OwnedArray<PendingPeerViewInfo> mPendingPeerViews;

    std::unique_ptr<ChannelGroupEffectsView> mEffectsView;

    std::unique_ptr<DrawableImage> mDragDrawable;
    std::unique_ptr<DrawableRectangle> mInsertLine;

    
    WeakReference<Component> pannerCalloutBox;
    WeakReference<Component> recvOptionsCalloutBox;
    WeakReference<Component> sendOptionsCalloutBox;
    WeakReference<Component> effectsCalloutBox;
    // Unified receive-buffer popup (hub-header chip)
    WeakReference<Component> unifiedBufCalloutBox;

    FlexBox peersBox;
    int peersMinHeight = 120;
    int peersMinWidth = 400;

    int mLastWidth = 0;
    bool isNarrow = false;
    bool peerModeFull = true; // default

    uint32 lastUpdateTimestampMs = 0;
    // Hub line Mb/s (this player's whole link to the hub)
    int64_t mHubLastBytesSent = 0, mHubLastBytesRecv = 0;
    float mHubRateSent = -1.0f, mHubRateRecv = -1.0f;
    int64_t mHubBytesSentLast = -1, mHubBytesRecvLast = -1; uint32 mHubRateStampMs = 0;   // Our own link's rates were never computed
    uint32 mHubDropsT[12] = {}; int64_t mHubDropsC[12] = {}; int mHubDropsN = 0;   // Drops-from-hub last-minute window
    
    Colour mutedBySoloColor;
    Colour mutedTextColor;
    Colour mutedColor;
    Colour soloColor;
    Colour regularTextColor;
    Colour droppedTextColor;
    Colour latGoodColor, latWarnColor, latBadColor;   // Latency / drop-rate colouring
    Colour dimTextColor;
    Colour outlineColor;
    Colour bgColor;

    // dragging state
    bool mDraggingActive = false;
    int mDraggingSourcePeer = -1;
    int mDraggingGroupPos = -1;
    bool mIgnoreNameClick = false;
    Array< juce::Rectangle<int> > mPeerViewBounds;
    Image  mDragImage;
    bool mAutoscrolling = false;


    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PeersContainerView)

};
