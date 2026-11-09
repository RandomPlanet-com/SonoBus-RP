/*
 ==============================================================================
 Copyright (c) 2017 - 2020 Foleys Finest Audio Ltd. - Daniel Walz
 All rights reserved.

 Redistribution and use in source and binary forms, with or without modification,
 are permitted provided that the following conditions are met:
 1. Redistributions of source code must retain the above copyright notice, this
    list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.
 3. Neither the name of the copyright holder nor the names of its contributors
    may be used to endorse or promote products derived from this software without
    specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 OF THE POSSIBILITY OF SUCH DAMAGE.

 ==============================================================================

    LevelMeter.cpp
    Created: 5 Apr 2016 9:49:54am
    Author:  Daniel Walz

 ==============================================================================
*/

namespace foleys
{

LevelMeter::LevelMeter (MeterFlags type)
  : meterType       (type)
{
    lookAndFeelChanged();

    onMaxLevelClicked = [](FFAU::LevelMeter& meter, [[maybe_unused]] int channel, [[maybe_unused]] juce::ModifierKeys mods)
    {
        // default clear all indicators. Overwrite this lambda to change the behaviour
        meter.clearMaxLevelDisplay();
        meter.clearClipIndicator();
    };

    onClipLightClicked = [](FFAU::LevelMeter& meter, [[maybe_unused]] int channel, [[maybe_unused]] juce::ModifierKeys mods)
    {
        // default clear all indicators. Overwrite this lambda to change the behaviour
        meter.clearMaxLevelDisplay();
        meter.clearClipIndicator();
    };

    startTimerHz (refreshRate);
}

LevelMeter::~LevelMeter()
{
    stopTimer();
}

void LevelMeter::setMeterFlags (MeterFlags type)
{
    if (meterType != type)
    {
        meterType = type;
        backgroundNeedsRepaint = true;
    }
}

void LevelMeter::setMeterSource (LevelMeterSource* src)
{
    // The periodic view refresh re-points every meter at the same
    // source each tick; only repaint when it actually changes.
    if (source.get() == src)
        return;

    source = src;
    repaint();
}

void LevelMeter::setSelectedChannel (int c)
{
    selectedChannel = c;
}

void LevelMeter::setFixedNumChannels (int numChannels)
{
    fixedNumChannels = numChannels;
}

void LevelMeter::setRefreshRateHz (int newRefreshRate)
{
    refreshRate = newRefreshRate;
    startTimerHz (refreshRate);
}

void LevelMeter::setUseBackgroundImage (bool shouldUseImage)
{
    if (useBackgroundImage != shouldUseImage)
    {
        useBackgroundImage = shouldUseImage;
        backgroundNeedsRepaint = true;

        if (! useBackgroundImage)
        {
            backgroundImage = juce::Image();
            overlayImage = juce::Image();
        }
    }
}

void LevelMeter::rebuildCachedImages (juce::Graphics& g, juce::Rectangle<float> bounds, int numChannels, float pixelScale)
{
    juce::ignoreUnused (g);

    // Render at the physical pixel scale so Retina output is pixel-identical to the
    // direct vector path (1px outlines / tick lines stay crisp).
    const int w = juce::jmax (1, juce::roundToInt (bounds.getWidth()  * pixelScale));
    const int h = juce::jmax (1, juce::roundToInt (bounds.getHeight() * pixelScale));
    const auto scaleXform = juce::AffineTransform::scale (pixelScale);

    backgroundImage = juce::Image (juce::Image::ARGB, w, h, true);
    {
        juce::Graphics bg (backgroundImage);
        bg.addTransform (scaleXform);
        lmLookAndFeel->drawBackground (bg, meterType, bounds);
        lmLookAndFeel->drawMeterBarsBackground (bg, meterType, bounds, numChannels, fixedNumChannels);
    }

    overlayImage = juce::Image (juce::Image::ARGB, w, h, true);
    {
        juce::Graphics ov (overlayImage);
        ov.addTransform (scaleXform);
        lmLookAndFeel->drawMeterBarsOverlay (ov, meterType, bounds, numChannels, fixedNumChannels, selectedChannel);
    }

    cachedNumChannels      = numChannels;
    cachedFixedNumChannels = fixedNumChannels;
    cachedSelectedChannel  = selectedChannel;
    cachedHasSource        = (source != nullptr);
    cachedPixelScale       = pixelScale;
    cachedMeterType        = meterType;
    backgroundNeedsRepaint = false;
}

void LevelMeter::paint (juce::Graphics& g)
{
    juce::Graphics::ScopedSaveState saved (g);

    const juce::Rectangle<float> bounds = getLocalBounds().toFloat();
    int numChannels = source ? source->getNumChannels() : 1;
    if (useBackgroundImage)
    {
        // The static layers (backgrounds below the bars,
        // tick-mark overlay above them) are cached as device-resolution images; per frame we
        // blit background -> vector bars -> blit overlay. Any key that changes what the static
        // layers look like invalidates the cache.
        const float pixelScale = juce::jmax (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());

        if (backgroundNeedsRepaint
            || ! backgroundImage.isValid()
            || cachedNumChannels != numChannels
            || cachedFixedNumChannels != fixedNumChannels
            || cachedSelectedChannel != selectedChannel
            || cachedHasSource != (source != nullptr)
            || cachedMeterType != meterType
            || ! juce::approximatelyEqual (cachedPixelScale, pixelScale))
        {
            rebuildCachedImages (g, bounds, numChannels, pixelScale);
        }

        const auto blitXform = juce::AffineTransform::scale (1.0f / pixelScale);

        g.drawImageTransformed (backgroundImage, blitXform);
        lmLookAndFeel->drawMeterBars (g, meterType, bounds, source, fixedNumChannels, selectedChannel);
        if (source)
            g.drawImageTransformed (overlayImage, blitXform);
    }
    else
    {
        lmLookAndFeel->drawBackground (g, meterType, bounds);
        lmLookAndFeel->drawMeterBarsBackground (g, meterType, bounds, numChannels, fixedNumChannels);
        lmLookAndFeel->drawMeterBars (g, meterType, bounds, source, fixedNumChannels, selectedChannel);
        if (source)
            lmLookAndFeel->drawMeterBarsOverlay (g, meterType, bounds, numChannels, fixedNumChannels, selectedChannel);
    }

    if (source)
    {
        source->decayIfNeeded();
    }
}

void LevelMeter::resized ()
{
    lmLookAndFeel->updateMeterGradients();
    backgroundNeedsRepaint = true;
}

void LevelMeter::visibilityChanged ()
{
    backgroundNeedsRepaint = true;
}

void LevelMeter::timerCallback ()
{
    // The shared newDataFlag was consumed by the FIRST
    // meter whose timer ran after each measurement, so the other meters on the same source never repainted. Each meter
    // now compares the source's last-measurement time against its own copy — no shared state, same idle behaviour.
    const juce::int64 t = source ? source->getLastMeasurementTime() : 0;
    if (backgroundNeedsRepaint || (source && t != lastSeenMeasurement))
    {
        lastSeenMeasurement = t;
        if (source) source->resetNewDataFlag();
        repaint();
    }
}

void LevelMeter::clearClipIndicator (int channel)
{
    if (source == nullptr)
        return;

    if (channel < 0)
        source->clearAllClipFlags();
    else
        source->clearClipFlag (channel);
}

void LevelMeter::clearMaxLevelDisplay (int channel)
{
    if (source == nullptr)
        return;

    if (channel < 0)
        source->clearAllMaxNums();
    else
        source->clearMaxNum (channel);
}

void LevelMeter::mouseDown (const juce::MouseEvent &event)
{
    if (source == nullptr)
        return;

    const juce::Rectangle<float> innerBounds = lmLookAndFeel->getMeterInnerBounds (getLocalBounds().toFloat(),
                                                                                   meterType);
    if (event.mods.isLeftButtonDown())
    {
        auto channel = lmLookAndFeel->hitTestClipIndicator (event.getPosition(),
                                                            meterType,
                                                            innerBounds,
                                                            source);
        if (channel >= 0)
        {
            listeners.call (&LevelMeter::Listener::clipLightClicked, this, channel, event.mods);
            if (onClipLightClicked)
                onClipLightClicked (*this, channel, event.mods);
        }

        channel = lmLookAndFeel->hitTestMaxNumber (event.getPosition(),
                                                   meterType,
                                                   innerBounds,
                                                   source);
        if (channel >= 0)
        {
            listeners.call (&LevelMeter::Listener::maxLevelClicked, this, channel, event.mods);
            if (onMaxLevelClicked)
                onMaxLevelClicked (*this, channel, event.mods);
        }
    }
}

void LevelMeter::addListener (LevelMeter::Listener* listener)
{
    listeners.add (listener);
}

void LevelMeter::removeListener (LevelMeter::Listener* listener)
{
    listeners.remove (listener);
}

void LevelMeter::parentHierarchyChanged()
{
    lookAndFeelChanged();
}

void LevelMeter::lookAndFeelChanged()
{
    // colours / metrics may have changed: drop the cached static layers
    backgroundNeedsRepaint = true;

    if (auto* lnf = dynamic_cast<LookAndFeelMethods*> (&getLookAndFeel()))
    {
        lmLookAndFeel = lnf;
        fallbackLookAndFeel.reset();
    }
    else
    {
        if (fallbackLookAndFeel.get() == nullptr)
            fallbackLookAndFeel = std::make_unique<LevelMeterLookAndFeel>();

        lmLookAndFeel = fallbackLookAndFeel.get();
    }
}

} // namespace foleys
