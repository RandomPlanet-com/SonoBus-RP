#pragma once
// A foleys::LevelMeter whose tooltip names only the bar the mouse is over. The bar is found with the same
// look-and-feel geometry the meter paints with (getMeterInnerBounds / getMeterBounds), so the name follows the mouse
// across bars. Used for the top input meter, the output meter and each player row's receive meter. Control path only;
// nothing on the audio thread.
#include "JuceHeader.h"
#include <functional>

struct SbChannelTipMeter : public foleys::LevelMeter, public ::juce::SettableTooltipClient
{
    explicit SbChannelTipMeter (foleys::LevelMeter::MeterFlags flags) : foleys::LevelMeter (flags), mFlags (flags) {}

    // Returns the tooltip for bar `channel` of `numChannels` ("" = no tip). Called on mouse moves only.
    std::function<::juce::String (int channel, int numChannels)> nameForChannel;

    // Tiny channel numbers drawn at the foot of each bar (default ON); skipped when
    // a bar is narrower than ~6 px. Drawn over the bottom of the bar with a dark backing so they stay legible.
    bool showChannelNumbers = true;
    // A meter that shows a SLICE of its source (one channel group): bars = tipNumChannels starting at
    // tipFirstChannel (what our LnF draws for setSelectedChannel + setFixedNumChannels). -1 = whole source.
    int tipFirstChannel = -1, tipNumChannels = -1;
    int drawnChannels () const { return tipNumChannels > 0 ? tipNumChannels : (mSrc != nullptr ? mSrc->getNumChannels() : 0); }
    int firstChannel () const  { return tipFirstChannel >= 0 ? tipFirstChannel : 0; }

    void paint (::juce::Graphics& g) override
    {
        foleys::LevelMeter::paint (g);
        if (!showChannelNumbers || mSrc == nullptr) return;
        const int n = drawnChannels();
        auto* lnf = dynamic_cast<foleys::LevelMeter::LookAndFeelMethods*> (&getLookAndFeel());
        if (n <= 0 || lnf == nullptr) return;
        const auto inner = lnf->getMeterInnerBounds (getLocalBounds().toFloat(), mFlags);
        const bool horizontal = (mFlags & foleys::LevelMeter::Horizontal) != 0;
        const float fontH = 7.5f;
        g.setFont (::juce::Font (fontH));
        for (int ch = 0; ch < n; ++ch) {
            auto r = lnf->getMeterBounds (inner, mFlags, n, ch);
            const float across = horizontal ? r.getHeight() : r.getWidth();
            if (across < 6.0f) continue;
            ::juce::Rectangle<float> label = horizontal ? r.removeFromLeft (::juce::jmin (r.getWidth(), 12.0f))
                                                        : r.removeFromBottom (::juce::jmin (r.getHeight(), fontH + 2.0f));
            g.setColour (::juce::Colours::black.withAlpha (0.55f));
            g.fillRect (label);
            g.setColour (::juce::Colours::white.withAlpha (0.55f));
            g.drawText (::juce::String (firstChannel() + ch + 1), label, ::juce::Justification::centred, false);
        }
    }


    void bindSource (foleys::LevelMeterSource* src) { mSrc = src; setMeterSource (src); }
    foleys::LevelMeterSource* boundSource() const { return mSrc; }

    // (x, y in the meter's own coordinates; no juce::Point here: a `Point` macro from the Mac headers collides in
    // some TUs)
    int channelAt (int px, int py) const
    {
        const int n = drawnChannels();
        if (n <= 0) return -1;
        const auto bounds = getLocalBounds().toFloat();
        if (auto* lnf = dynamic_cast<foleys::LevelMeter::LookAndFeelMethods*> (&getLookAndFeel())) {
            const auto inner = lnf->getMeterInnerBounds (bounds, mFlags);
            for (int ch = 0; ch < n; ++ch)
                if (lnf->getMeterBounds (inner, mFlags, n, ch).contains ((float) px, (float) py)) return firstChannel() + ch;
        }
        // Outside every bar rectangle (gaps/margins): nearest bar along the meter's axis.
        const bool horizontal = (mFlags & foleys::LevelMeter::Horizontal) != 0;
        const float frac = horizontal ? (float) py / (float) ::juce::jmax (1, getHeight())
                                      : (float) px / (float) ::juce::jmax (1, getWidth());
        return firstChannel() + ::juce::jlimit (0, n - 1, (int) (frac * (float) n));
    }

    void mouseEnter (const ::juce::MouseEvent& e) override { refreshTip (e); foleys::LevelMeter::mouseEnter (e); }
    void mouseMove  (const ::juce::MouseEvent& e) override { refreshTip (e); foleys::LevelMeter::mouseMove (e); }
    void mouseExit  (const ::juce::MouseEvent& e) override { mTip = {}; setTooltip ({}); foleys::LevelMeter::mouseExit (e); }

private:
    void refreshTip (const ::juce::MouseEvent& e)
    {
        const int ch = channelAt (e.x, e.y);
        ::juce::String tip;
        if (ch >= 0 && nameForChannel) tip = nameForChannel (ch, mSrc != nullptr ? mSrc->getNumChannels() : 0);
        if (tip != mTip) { mTip = tip; setTooltip (tip); }
    }
    foleys::LevelMeter::MeterFlags mFlags;
    foleys::LevelMeterSource* mSrc = nullptr;
    ::juce::String mTip;
};
