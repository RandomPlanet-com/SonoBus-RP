// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2020 Jesse Chappell


#include "SonoLookAndFeel.h"
#include "SonoDrawableButton.h"
//#include "ConfigurationRowView.h"
#include "SonoTextButton.h"

#include <JuceHeader.h>

#define OLDFONTSTUFF 1

#if OLDFONTSTUFF

float SonoLookAndFeel::fontScale = 1.0f;

#else

 #if JUCE_ANDROID
 float SonoLookAndFeel::fontScale = 1.0f;
 #elif JUCE_WINDOWS
 float SonoLookAndFeel::fontScale = 1.35f;
 #else
 float SonoLookAndFeel::fontScale = 1.25f;
 #endif

#endif

//==============================================================================

// Tooltips are one dark card. A tip that starts with sbTipMark is a rich card: one element per line,
// fields split by sbTipSep:
//   T|total|colour|caption        big figure and a small caption on the right
//   B                             bar, built from the rows that carry a weight
//   R|label|value|colour|weight   row: label left, value right; a colour draws a swatch, a weight joins the bar
//   S|text                        dim line under the row above
//   D                             divider
//   F|colour|text                 dim line, led by a dot when a colour is given
//   P|text                        paragraph
// Colours are hex rgb. Anything without the mark is plain text.
namespace
{
    const float sbTipFontSize = 14.0f;
    const int sbTipShadow = 5;          // room around the card for its shadow
    const int sbTipPlainMaxText = 314;  // 340 px card less its padding
    const int sbTipPadX = 13, sbTipPadY = 9;
    const int sbTipRichPadX = 18, sbTipRichPadY = 14;
    const int sbTipRichMinW = 360, sbTipRichMaxW = 420;

    const Colour sbTipEdge (0xff2e3638), sbTipText (0xffdfe5e6), sbTipDim (0xff8a9496), sbTipFoot (0xffaab3b5);

    bool sbTipIsRich (const String& text)   { return text.isNotEmpty() && text[0] == SonoLookAndFeel::sbTipMark; }

    Colour sbTipColour (const String& hex, Colour fallback)
    {
        return hex.isEmpty() ? fallback : Colour ((uint32) hex.getHexValue32() | 0xff000000u);
    }

    TextLayout sbTipWrapped (const String& text, const Font& font, Colour colour, float maxWidth)
    {
        AttributedString s;
        s.setJustification (Justification::topLeft);
        s.setWordWrap (AttributedString::byWord);
        s.append (text, font, colour);
        TextLayout tl;
        tl.createLayout (s, jmax (20.0f, maxWidth));
        return tl;
    }

    // Lays a rich tip out; draws it when g is given. innerW <= 0: the width is found from the content.
    // Returns the size of the content (without padding).
    ::juce::Rectangle<float> sbTipRichLayout (const String& text, Graphics* g, float innerW, float x0, float y0)
    {
        StringArray lines;
        lines.addLines (text.substring (1));

        const Font rowFont (sbTipFontSize), totalFont (26.0f, Font::bold), footFont (13.0f), subFont (12.5f);
        const Font capFont = Font (11.5f).withExtraKerningFactor (0.06f);
        const float rowH = 23.0f, swatch = 8.0f, indentSw = 17.0f, gap = 24.0f, dotW = 15.0f;
        const String sep = String::charToString (SonoLookAndFeel::sbTipSep);

        struct Seg { Colour c; float w; };
        Array<Seg> segs;
        Array<StringArray> rows;
        bool anySwatch = false;
        float natural = 0.0f, segSum = 0.0f;
        for (auto& l : lines)
        {
            StringArray f;
            f.addTokens (l, sep, "");
            while (f.size() < 5) f.add ({});
            rows.add (f);
            if (f[0] == "R" && f[3].isNotEmpty()) anySwatch = true;
        }
        for (auto& f : rows)
        {
            const String& k = f[0];
            if (k == "T")       natural = jmax (natural, totalFont.getStringWidthFloat (f[1]) + gap + jmin (200.0f, capFont.getStringWidthFloat (f[3])));
            else if (k == "R")
            {
                natural = jmax (natural, (f[3].isNotEmpty() ? indentSw : 0.0f) + rowFont.getStringWidthFloat (f[1]) + gap + rowFont.getStringWidthFloat (f[2]));
                const float w = f[4].getFloatValue();
                if (w > 0.0f) { segs.add ({ sbTipColour (f[3], sbTipDim), w }); segSum += w; }
            }
            else if (k == "S")  natural = jmax (natural, (anySwatch ? indentSw : 0.0f) + subFont.getStringWidthFloat (f[1]));
            else if (k == "F")  natural = jmax (natural, (f[1].isNotEmpty() ? dotW : 0.0f) + footFont.getStringWidthFloat (f[2]));
            else if (k == "P")  natural = jmax (natural, rowFont.getStringWidthFloat (f[1]));
        }
        const float W = innerW > 0.0f ? innerW
                                      : jlimit ((float) (sbTipRichMinW - 2 * sbTipRichPadX), (float) (sbTipRichMaxW - 2 * sbTipRichPadX), std::ceil (natural));

        float y = y0;
        for (auto& f : rows)
        {
            const String& k = f[0];
            if (k == "T")
            {
                const float h = 32.0f;
                if (g != nullptr)
                {
                    const float tw = totalFont.getStringWidthFloat (f[1]);
                    g->setFont (totalFont);
                    g->setColour (sbTipColour (f[2], sbTipText));
                    g->drawText (f[1], ::juce::Rectangle<float> (x0, y, W, h), Justification::bottomLeft, true);
                    g->setFont (capFont);
                    g->setColour (sbTipDim);
                    g->drawText (f[3], ::juce::Rectangle<float> (x0 + tw + 12.0f, y, jmax (0.0f, W - tw - 12.0f), h - 4.0f), Justification::bottomRight, true);
                }
                y += h + 8.0f;
            }
            else if (k == "B")
            {
                if (segs.size() < 2 || segSum <= 0.0f) continue;
                const float h = 9.0f;
                if (g != nullptr)
                {
                    const ::juce::Rectangle<float> bar (x0, y, W, h);
                    Path clip;
                    clip.addRoundedRectangle (bar, h * 0.5f);
                    Graphics::ScopedSaveState save (*g);
                    g->reduceClipRegion (clip);
                    float x = x0;
                    for (auto& sgm : segs)
                    {
                        const float w = W * sgm.w / segSum;
                        g->setColour (sgm.c);
                        g->fillRect (::juce::Rectangle<float> (x, y, w + 0.5f, h));
                        x += w;
                    }
                }
                y += h + 10.0f;
            }
            else if (k == "R")
            {
                if (g != nullptr)
                {
                    const bool sw = f[3].isNotEmpty();
                    if (sw)
                    {
                        g->setColour (sbTipColour (f[3], sbTipDim));
                        g->fillRoundedRectangle (x0, y + (rowH - swatch) * 0.5f, swatch, swatch, 2.0f);
                    }
                    const float lx = x0 + (sw ? indentSw : 0.0f);
                    const float vw = rowFont.getStringWidthFloat (f[2]) + 1.0f;
                    g->setFont (rowFont);
                    g->setColour (sbTipText);
                    g->drawText (f[2], ::juce::Rectangle<float> (x0 + W - vw, y, vw, rowH), Justification::centredRight, false);
                    g->drawText (f[1], ::juce::Rectangle<float> (lx, y, jmax (0.0f, x0 + W - vw - 8.0f - lx), rowH), Justification::centredLeft, true);
                }
                y += rowH;
            }
            else if (k == "S")
            {
                const float ind = anySwatch ? indentSw : 0.0f;
                const TextLayout tl (sbTipWrapped (f[1], subFont, sbTipDim, W - ind));
                if (g != nullptr) tl.draw (*g, ::juce::Rectangle<float> (x0 + ind, y - 2.0f, W - ind, tl.getHeight()));
                y += tl.getHeight() + 2.0f;
            }
            else if (k == "D")
            {
                y += 7.0f;
                if (g != nullptr) { g->setColour (sbTipEdge); g->fillRect (::juce::Rectangle<float> (x0, y, W, 1.0f)); }
                y += 8.0f;
            }
            else if (k == "F")
            {
                const bool dot = f[1].isNotEmpty();
                const float ind = dot ? dotW : 0.0f;
                const TextLayout tl (sbTipWrapped (f[2], footFont, sbTipFoot, W - ind));
                if (g != nullptr)
                {
                    if (dot) { g->setColour (sbTipColour (f[1], sbTipDim)); g->fillEllipse (x0, y + 5.0f, 8.0f, 8.0f); }
                    tl.draw (*g, ::juce::Rectangle<float> (x0 + ind, y, W - ind, tl.getHeight()));
                }
                y += tl.getHeight() + 3.0f;
            }
            else if (k == "P")
            {
                const TextLayout tl (sbTipWrapped (f[1], rowFont, sbTipText, W));
                if (g != nullptr) tl.draw (*g, ::juce::Rectangle<float> (x0, y, W, tl.getHeight()));
                y += tl.getHeight() + 3.0f;
            }
        }
        return { x0, y0, W, jmax (0.0f, y - y0 - 3.0f) };
    }
}

String SonoLookAndFeel::tipPlainText (const String& tip)
{
    if (! sbTipIsRich (tip)) return tip;
    StringArray lines, out;
    lines.addLines (tip.substring (1));
    const String sep = String::charToString (sbTipSep);
    for (auto& l : lines)
    {
        StringArray f;
        f.addTokens (l, sep, "");
        while (f.size() < 4) f.add ({});
        if (f[0] == "T")       out.add ((f[1] + "  " + f[3]).trim());
        else if (f[0] == "R")  out.add ((f[1] + " " + f[2]).trim());
        else if (f[0] == "S" || f[0] == "P") out.add (f[1]);
        else if (f[0] == "F")  out.add (f[2]);
    }
    return out.joinIntoString ("\n");
}

static String sbElideEnd (const Font& f, const String& in, float w)
{
    const String s = in.trim();
    if (f.getStringWidthFloat (s) <= w)
        return s;
    const String ell = String::fromUTF8 ("\xe2\x80\xa6");
    int lo = 0, hi = s.length();
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (f.getStringWidthFloat (s.substring (0, mid).trimEnd() + ell) <= w) lo = mid;
        else hi = mid - 1;
    }
    return s.substring (0, lo).trimEnd() + ell;
}

// Splits one paragraph into at most `budget` lines, breaking only at spaces; the last line keeps the rest.
static void sbWrapWords (const Font& f, const String& para, float w, int budget, StringArray& out)
{
    String rest = para.trim();
    while (rest.isNotEmpty() && budget > 0)
    {
        if (budget == 1 || f.getStringWidthFloat (rest) <= w) {
            out.add (rest);
            return;
        }
        // "Group - Name": break after the dash so the second part keeps a line of its own
        const int dash = rest.indexOf (" - ");
        if (dash > 0) {
            const String head = rest.substring (0, dash + 2);
            if (! rest.substring (0, dash).containsChar (' ') || f.getStringWidthFloat (head) <= w) {
                out.add (head);
                rest = rest.substring (dash + 3).trimStart();
                --budget;
                continue;
            }
        }
        // As many whole words as fit; a single word wider than the line takes the line alone
        int breakAt = -1;
        for (int i = rest.indexOfChar (' '); i > 0; i = rest.indexOfChar (i + 1, ' ')) {
            if (f.getStringWidthFloat (rest.substring (0, i).trimEnd()) <= w) breakAt = i;
            else break;
        }
        if (breakAt < 0)
            breakAt = rest.indexOfChar (' ');
        if (breakAt < 0) {
            out.add (rest);
            return;
        }
        out.add (rest.substring (0, breakAt).trimEnd());
        rest = rest.substring (breakAt).trimStart();
        --budget;
    }
}

void SonoLookAndFeel::drawTextNoSqueeze (Graphics& g, const String& text, juce::Rectangle<int> area,
                                         Justification just, int maxLines)
{
    if (area.getWidth() <= 0 || area.getHeight() <= 0 || text.isEmpty())
        return;

    const Font font = g.getCurrentFont();
    const float w = (float) area.getWidth();
    const float lineH = font.getHeight();
    const bool hasBreaks = text.containsAnyOf ("\r\n");

    if (! hasBreaks && (maxLines <= 1 || font.getStringWidthFloat (text.trim()) <= w)) {
        g.drawText (sbElideEnd (font, text, w), area, just, true);
        return;
    }

    StringArray paras;
    if (hasBreaks) paras.addLines (text);
    else           paras.add (text);

    // Explicit line breaks are always kept; extra wrapped lines only where the height allows
    const int fitLines = jmax (1, (int) ((float) area.getHeight() / lineH));
    const int budget = jmax (jmax (1, jmin (maxLines, fitLines)), paras.size());

    StringArray lines;
    for (int i = 0; i < paras.size(); ++i) {
        const int laterParas = paras.size() - i - 1;
        sbWrapWords (font, paras[i], w, jmax (1, budget - lines.size() - laterParas), lines);
    }

    if (lines.size() == 1) {
        g.drawText (sbElideEnd (font, lines[0], w), area, just, true);
        return;
    }

    const float total = lineH * (float) lines.size();
    float y = (float) area.getY();
    if (just.testFlags (Justification::bottom))                 y = (float) area.getBottom() - total;
    else if (! just.testFlags (Justification::top))             y += ((float) area.getHeight() - total) * 0.5f;

    auto horiz = just.getOnlyHorizontalFlags();
    if (horiz == 0) horiz = Justification::left;
    const Justification lineJust (horiz | Justification::verticallyCentred);

    for (const auto& line : lines) {
        g.drawText (sbElideEnd (font, line, w), juce::Rectangle<float> ((float) area.getX(), y, w, lineH), lineJust, true);
        y += lineH;
    }
}

SonoLookAndFeel::SonoLookAndFeel(bool useUniversalFont) : mUseUniversalFont(useUniversalFont)
{
    // setColour (mainBackgroundColourId, Colour::greyLevel (0.8f));
    //DBG("Sonolook and feel");

    // Any fitted text that is not handled here elides instead of compressing the glyphs
    Font::setDefaultMinimumHorizontalScaleFactor (1.0f);

    setUsingNativeAlertWindows(true);

    //fontScale = 1.2; // 1.125;

    setColourScheme(getDarkColourScheme());

    getCurrentColourScheme().setUIColour(ColourScheme::UIColour::windowBackground, Colour::fromFloatRGBA(0.0, 0.0, 0.0, 1.0));
    getCurrentColourScheme().setUIColour(ColourScheme::UIColour::widgetBackground, Colour::fromFloatRGBA(0.1, 0.1, 0.1, 1.0));
    getCurrentColourScheme().setUIColour(ColourScheme::UIColour::outline, Colour::fromFloatRGBA(0.3, 0.3, 0.3, 0.5));

    setColour (Label::textColourId, Colour (0xffcccccc));
    setColour (Label::textWhenEditingColourId, Colour (0xffe9e9e9));
    
    setColour(ResizableWindow::backgroundColourId, Colour(0xff111111));
    
    //setColour (TextButton::buttonColourId, Colour (0xff363636));
    setColour (TextButton::buttonColourId, Colour::fromFloatRGBA(0.15, 0.15, 0.15, 0.7)); // old one
    //setColour (TextButton::buttonColourId, Colour::fromFloatRGBA(0.15, 0.15, 0.15, 0.0));
    //setColour (TextButton::buttonOnColourId, Colour (0xff3d70c8));
    setColour (TextButton::buttonOnColourId, Colour::fromFloatRGBA(0.18, 0.76, 0.70, 0.8));   // Icon teal
    setColour (TextButton::textColourOnId, Colour (0xddcccccc));
    setColour (TextButton::textColourOffId, Colour (0xdde9e9e9));

    setColour (ToggleButton::textColourId, Colour (0xddcccccc));

    
    setColour (SonoTextButton::outlineColourId, Colour::fromFloatRGBA(0.3, 0.3, 0.3, 0.5));

    setColour (ScrollBar::ColourIds::thumbColourId, Colour::fromFloatRGBA(0.4, 0.4, 0.4, 0.6));
    
    //setColour (ComboBox::backgroundColourId, Colour (0xff161616));
    setColour (ComboBox::backgroundColourId, Colour::fromFloatRGBA(0.15, 0.15, 0.15, 0.7));
    setColour (ComboBox::textColourId, Colour (0xffe9e9e9));
    setColour (ComboBox::outlineColourId, Colour::fromFloatRGBA(0.3, 0.3, 0.3, 0.5));

    setColour (TextEditor::backgroundColourId, Colour (0xff050505));
    setColour (TextEditor::textColourId, Colour (0xffe9e9e9));
    setColour (TextEditor::highlightColourId, Colour (0xff2ec2b3));   // Icon teal
    setColour (TextEditor::outlineColourId, Colour::fromFloatRGBA(0.3, 0.3, 0.3, 0.5));
    setColour (TextEditor::focusedOutlineColourId, Colour::fromFloatRGBA(0.5, 0.5, 0.5, 0.7));

    setColour (Slider::backgroundColourId, Colour::fromFloatRGBA(0.2, 0.2, 0.2, 1.0));
    setColour (Slider::rotarySliderOutlineColourId, Colour::fromFloatRGBA(0.2, 0.2, 0.2, 1.0));
    setColour (Slider::textBoxTextColourId, Colour(0xddcccccc));
    setColour (Slider::textBoxBackgroundColourId, Colour::fromFloatRGBA(0.05, 0.05, 0.05, 1.0));
    setColour (Slider::textBoxHighlightColourId, Colour (0xaa555555));
    setColour (Slider::textBoxOutlineColourId, Colour::fromFloatRGBA(0.3, 0.3, 0.3, 0.5));
    
    setColour (Slider::trackColourId, Colour::fromFloatRGBA(0.1, 0.4, 0.6, 0.8));
    setColour (Slider::thumbColourId, Colour::fromFloatRGBA(0.18, 0.76, 0.70, 0.9));
    //setColour (Slider::thumbColourId, Colour::fromFloatRGBA(0.2, 0.5, 0.7, 1.0));
    setColour (Slider::rotarySliderFillColourId, Colour::fromFloatRGBA(0.18, 0.76, 0.70, 0.9));
    
    setColour (TabbedButtonBar::tabOutlineColourId, Colour::fromFloatRGBA(0.3, 0.3, 0.3, 0.5));


    
    setColour (ListBox::backgroundColourId, Colour::fromFloatRGBA(0.15, 0.15, 0.15, 0.7));
    setColour (ListBox::outlineColourId, Colour::fromFloatRGBA(0.3, 0.3, 0.3, 0.5));

    setColour (BubbleComponent::backgroundColourId, Colour::fromFloatRGBA(0.25, 0.25, 0.25, 1.0));
    setColour (BubbleComponent::outlineColourId, Colour::fromFloatRGBA(0.4, 0.4, 0.4, 0.5));
    //setColour (TooltipWindow::textColourId, Colour(0xeecccccc));
    setColour (TooltipWindow::textColourId, Colour(0xffdfe5e6));
    setColour (TooltipWindow::backgroundColourId, Colour(0xff1d2122));
    setColour (TooltipWindow::outlineColourId, Colour(0xff2e3638));

    setColour (PopupMenu::backgroundColourId, Colour::fromFloatRGBA(0.2, 0.2, 0.2, 1.0));
    setColour (PopupMenu::highlightedBackgroundColourId, Colour::fromFloatRGBA(0.35, 0.35, 0.4, 1.0));

    setColour (SidePanel::backgroundColour, Colour::fromFloatRGBA(0.17, 0.17, 0.17, 1.0));


    
    //setColour (SonoDrawableButton::overOverlayColourId, Colour::fromFloatRGBA(0.8, 0.8, 0.8, 0.08));
    //setColour (SonoDrawableButton::downOverlayColourId, Colour::fromFloatRGBA(0.8, 0.8, 0.8, 0.3));
    setColour (SonoDrawableButton::overOverlayColourId, Colour::fromFloatRGBA(0.8, 0.8, 0.8, 0.08));
    setColour (SonoDrawableButton::downOverlayColourId, Colour::fromFloatRGBA(0.8, 0.8, 0.8, 0.3));
    
    setColour (DrawableButton::textColourId, Colour (0xffb9b9b9));
    setColour (DrawableButton::textColourOnId, Colour (0xffe9e9e9));

    //setColour (DrawableButton::backgroundColourId, Colour (0xffb9b9b9));
    setColour (DrawableButton::backgroundOnColourId, Colour::fromFloatRGBA(0.18, 0.76, 0.70, 0.8));

    //setColour (ConfigurationRowView::backgroundColourId, Colour::fromFloatRGBA(0.05, 0.05, 0.05, 1.0));
    //setColour (ConfigurationRowView::selectedBackgroundColourId, Colour::fromFloatRGBA(0.15, 0.15, 0.15, 1.0));

    setColour(ToggleButton::tickColourId, Colour::fromFloatRGBA(0.4, 0.8, 1.0, 1.0));
    
    setColour (DirectoryContentsDisplayComponent::highlightColourId, Colour::fromFloatRGBA(0.1, 0.4, 0.6, 0.9));
    setColour (DirectoryContentsDisplayComponent::textColourId, Colour (0xffe9e9e9));
    // setColour (Label::textColourId, Colour (0xffe9e9e9));

    //myFont = Typeface::createSystemTypefaceFor (BinaryData::DejaVuSans_ttf, BinaryData::DejaVuSans_ttfSize);
    //setDefaultSansSerifTypefaceName("Gill Sans");
    //setDefaultSansSerifTypefaceName("Arial Unicode MS");
    //setDefaultSansSerifTypefaceName(myFont.getTypefaceName());

    //myFont = Typeface::createSystemTypefaceFor (BinaryData::GillSans_ttc, BinaryData::GillSans_ttcSize);
    myFont = Font(16 * fontScale);
    
    setupDefaultMeterColours();

    if (auto * deflnf = dynamic_cast<SonoLookAndFeel*>(&LookAndFeel::getDefaultLookAndFeel())) {
        setLanguageCode(deflnf->languageCode, mUseUniversalFont);
    }

    //DBG("Myfont name " << myFont.getTypefaceName());
}

void SonoLookAndFeel::drawTableHeaderColumn (Graphics& g, TableHeaderComponent& header, const String& columnName, int /*columnId*/,
                                             int width, int height, bool isMouseOver, bool isMouseDown, int columnFlags)
{
    auto highlightColour = header.findColour (TableHeaderComponent::highlightColourId);
    if (isMouseDown)       g.fillAll (highlightColour);
    else if (isMouseOver)  g.fillAll (highlightColour.withMultipliedAlpha (0.625f));

    juce::Rectangle<int> area (width, height);
    area.reduce (4, 0);
    if ((columnFlags & (TableHeaderComponent::sortedForwards | TableHeaderComponent::sortedBackwards)) != 0)
    {
        Path sortArrow;
        sortArrow.addTriangle (0.0f, 0.0f, 0.5f, (columnFlags & TableHeaderComponent::sortedForwards) != 0 ? -0.8f : 0.8f, 1.0f, 0.0f);
        g.setColour (Colours::white.withAlpha (0.85f));
        g.fillPath (sortArrow, sortArrow.getTransformToScaleToFit (area.removeFromRight (height / 2).reduced (2).toFloat(), true));
    }
    g.setColour (header.findColour (TableHeaderComponent::textColourId));
    g.setFont (Font ((float) height * 0.5f, Font::bold));
    g.drawFittedText (columnName, area, Justification::centredLeft, 1);
}

void SonoLookAndFeel::setLanguageCode(const String & lang, bool useUniversalFont)
{
    languageCode = lang;
    mUseUniversalFont = useUniversalFont;

    /*
    if (lang.startsWith("zh")) {
        fontScale = 1.0f;
    }
    else if (lang.startsWith("ko")) {
        fontScale = 1.15f;
    }
     */
}



Typeface::Ptr SonoLookAndFeel::getTypefaceForFont (const Font& font)
{
    DBG("get typeface for font " << font.getTypefaceName() << " with defaultsansser: " << Font::getDefaultSansSerifFontName());
    if (font.getTypefaceName() == Font::getDefaultSansSerifFontName())
    {
        // if on android and language is japanese/chinese/korean, use DroidSansFallback
        String lang = SystemStats::getUserLanguage();
        if (languageCode.isNotEmpty()) {
            lang = languageCode;
        }
        
        String slang = lang.initialSectionNotContaining("_").toLowerCase();

        if (!mUseUniversalFont)
        {
            if (slang.startsWith("ja")) {
                DBG("Using japanese");
                Font jfont(font);
#if (JUCE_MAC || JUCE_IOS)
                jfont.setTypefaceName("Hiragino Sans W3");
#elif JUCE_ANDROID
                //jfont.setTypefaceName("Droid Sans Fallback");
                return Typeface::createSystemTypefaceFor (BinaryData::DejaVuSans_ttf, BinaryData::DejaVuSans_ttfSize);
#elif JUCE_WINDOWS
                jfont.setTypefaceName("Arial Unicode MS");
#endif
                return Typeface::createSystemTypefaceFor (jfont);
            }
            else if (slang.startsWith("ko")) {
                DBG("Using korean");
                Font jfont(font);
#if (JUCE_MAC || JUCE_IOS)
                jfont.setTypefaceName("Apple SD Gothic Neo");
#elif JUCE_ANDROID
                //jfont.setTypefaceName("Droid Sans Fallback");
                return Typeface::createSystemTypefaceFor (BinaryData::DejaVuSans_ttf, BinaryData::DejaVuSans_ttfSize);
#elif JUCE_WINDOWS
                jfont.setTypefaceName("Malgun Gothic");
                //jfont.setTypefaceName("Arial Unicode MS");
#endif
                return Typeface::createSystemTypefaceFor (jfont);
            }
            else if (slang.startsWith("zh")) {
                DBG("Using chinese");
                Font jfont(font);
#if (JUCE_MAC || JUCE_IOS)
                jfont.setTypefaceName("PingFang SC");
#elif JUCE_WINDOWS
                jfont.setTypefaceName("Arial Unicode MS");
#elif JUCE_ANDROID
                jfont.setTypefaceName("DroidSansFallback");
                return Typeface::createSystemTypefaceFor (BinaryData::DejaVuSans_ttf, BinaryData::DejaVuSans_ttfSize);
#endif
                return Typeface::createSystemTypefaceFor (jfont);
            }
            else
            {
                DBG("Creating custom typeface!!");

                // The face that matches the style, so bold and italic text (chat markup, headings) show as such
                if (font.isBold() && font.isItalic())
                    return Typeface::createSystemTypefaceFor (BinaryData::DejaVuSansBoldOblique_ttf, BinaryData::DejaVuSansBoldOblique_ttfSize);
                if (font.isBold())
                    return Typeface::createSystemTypefaceFor (BinaryData::DejaVuSansBold_ttf, BinaryData::DejaVuSansBold_ttfSize);
                if (font.isItalic())
                    return Typeface::createSystemTypefaceFor (BinaryData::DejaVuSansOblique_ttf, BinaryData::DejaVuSansOblique_ttfSize);
                return Typeface::createSystemTypefaceFor (BinaryData::DejaVuSans_ttf, BinaryData::DejaVuSans_ttfSize);
                //return Typeface::createSystemTypefaceFor (BinaryData::InterUnicode_ttf, BinaryData::InterUnicode_ttfSize);
                //return Typeface::createSystemTypefaceFor (BinaryData::GoNotoKurrentRegular_ttf, BinaryData::GoNotoKurrentRegular_ttfSize);
            }
        }
        else {
            // universal font stuff
            {
                DBG("Creating custom typeface!!");

                //return Typeface::createSystemTypefaceFor (BinaryData::DejaVuSans_ttf, BinaryData::DejaVuSans_ttfSize);
                //return Typeface::createSystemTypefaceFor (BinaryData::InterUnicode_ttf, BinaryData::InterUnicode_ttfSize);
                return Typeface::createSystemTypefaceFor (BinaryData::GoNotoKurrentRegular_ttf, BinaryData::GoNotoKurrentRegular_ttfSize);
            }
        }
    }
    return LookAndFeel_V4::getTypefaceForFont(font);
}

#if 1


//==============================================================================
::juce::Rectangle<int> SonoLookAndFeel::getTooltipBounds (const ::juce::String& tipText, SbTipPoint screenPos, ::juce::Rectangle<int> parentArea)
{   // The card plus the room for its shadow
    int w = 0, h = 0;
    if (sbTipIsRich (tipText))
    {
        const auto r = sbTipRichLayout (tipText, nullptr, 0.0f, 0.0f, 0.0f);
        w = (int) std::ceil (r.getWidth()) + 2 * sbTipRichPadX;
        h = (int) std::ceil (r.getHeight()) + 2 * sbTipRichPadY;
    }
    else
    {
        const TextLayout tl (sbTipWrapped (tipText, Font (sbTipFontSize), sbTipText, (float) sbTipPlainMaxText));
        w = (int) std::ceil (tl.getWidth()) + 2 * sbTipPadX;
        h = (int) std::ceil (tl.getHeight()) + 2 * sbTipPadY;
    }
    w += 2 * sbTipShadow; h += 2 * sbTipShadow;
    return ::juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                           screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6)  : screenPos.y + 6, w, h).constrainedWithin (parentArea);
}

void SonoLookAndFeel::drawTooltip (Graphics& g, const String& text, int width, int height)
{
    const auto card = ::juce::Rectangle<int> (width, height).reduced (sbTipShadow).toFloat();
    const float corner = 10.0f;

    for (int i = sbTipShadow; i >= 1; --i)   // soft shadow, a little below the card
    {
        g.setColour (Colours::black.withAlpha (0.07f));
        g.fillRoundedRectangle (card.expanded ((float) i - 1.0f).translated (0.0f, 1.5f), corner + (float) i - 1.0f);
    }

    g.setColour (findColour (TooltipWindow::backgroundColourId));
    g.fillRoundedRectangle (card, corner);
    g.setColour (findColour (TooltipWindow::outlineColourId));
    g.drawRoundedRectangle (card.reduced (0.5f), corner, 1.0f);

    if (sbTipIsRich (text))
    {
        const auto in = card.reduced ((float) sbTipRichPadX, (float) sbTipRichPadY);
        sbTipRichLayout (text, &g, in.getWidth(), in.getX(), in.getY());
    }
    else
    {
        const auto in = card.reduced ((float) sbTipPadX, (float) sbTipPadY);
        sbTipWrapped (text, Font (sbTipFontSize), findColour (TooltipWindow::textColourId), in.getWidth() + 1.0f).draw (g, in.withWidth (in.getWidth() + 1.0f));
    }
}

Font SonoLookAndFeel::getMenuBarFont (MenuBarComponent& menuBar, int /*itemIndex*/, const String& /*itemText*/)
{
    return Font (menuBar.getHeight() * 0.7f * fontScale);
}

//==============================================================================
void SonoLookAndFeel::drawCallOutBoxBackground (CallOutBox& box, Graphics& g,
                                               const Path& path, Image& cachedImage)
{
    if (cachedImage.isNull())
    {
        cachedImage = Image (Image::ARGB, box.getWidth(), box.getHeight(), true);
        Graphics g2 (cachedImage);
        
        DropShadow (Colours::black.withAlpha (0.7f), 8, Point<int> (0, 2)).drawForPath (g2, path);
    }
    
    g.setColour (Colours::black);
    g.drawImageAt (cachedImage, 0, 0);
    
    //g.setColour (getCurrentColourScheme().getUIColour (ColourScheme::UIColour::widgetBackground).withAlpha (0.8f));
    g.setColour (getCurrentColourScheme().getUIColour (ColourScheme::UIColour::widgetBackground));
    g.fillPath (path);
    
    g.setColour (getCurrentColourScheme().getUIColour (ColourScheme::UIColour::outline).withAlpha (0.8f));
    g.strokePath (path, PathStrokeType (1.0f));
}



int SonoLookAndFeel::getTabButtonBestWidth (TabBarButton& button, int depth)
{
    // A bar marked "sbFillTabs" shares its whole width evenly between its tabs (the Connect window)
    auto& bar = button.getTabbedButtonBar();
    if ((bool) bar.getProperties().getWithDefault ("sbFillTabs", false) && bar.getNumTabs() > 0 && bar.getWidth() > 0)
        return bar.getWidth() / bar.getNumTabs();
    return 250; // 120;
}

int SonoLookAndFeel::getTabButtonSpaceAroundImage() {
    return 0;
}

static Colour getTabBackgroundColour (TabBarButton& button)
{
    
    const Colour bkg (button.findColour (TabbedComponent::backgroundColourId).contrasting (0.15f));

    if (button.isFrontTab())
        return bkg.overlaidWith (Colours::yellow.withAlpha (0.8f));

    return bkg;
}

Rectangle<int> SonoLookAndFeel::getTabButtonExtraComponentBounds (const TabBarButton& button, Rectangle<int>& textArea, Component& comp)
{
    Rectangle<int> extraComp;
    
    auto orientation = button.getTabbedButtonBar().getOrientation();
    
    if (button.getExtraComponentPlacement() == TabBarButton::beforeText)
    {
        switch (orientation)
        {
            case TabbedButtonBar::TabsAtBottom:
            case TabbedButtonBar::TabsAtTop:     extraComp = textArea.removeFromLeft   (comp.getWidth()); break;
            case TabbedButtonBar::TabsAtLeft:    extraComp = textArea.removeFromBottom (comp.getHeight()); break;
            case TabbedButtonBar::TabsAtRight:   extraComp = textArea.removeFromTop    (comp.getHeight()); break;
            default:                             jassertfalse; break;
        }
    }
    else if (button.getExtraComponentPlacement() == TabBarButton::afterText)
    {
        switch (orientation)
        {
            case TabbedButtonBar::TabsAtBottom:
            case TabbedButtonBar::TabsAtTop:     extraComp = textArea.removeFromRight  (comp.getWidth()); break;
            case TabbedButtonBar::TabsAtLeft:    extraComp = textArea.removeFromTop    (comp.getHeight()); break;
            case TabbedButtonBar::TabsAtRight:   extraComp = textArea.removeFromBottom (comp.getHeight()); break;
            default:                             jassertfalse; break;
        }
    }
    else if (button.getExtraComponentPlacement() == TabBarButton::aboveText)
    {
        switch (orientation)
        {
            case TabbedButtonBar::TabsAtBottom:
            case TabbedButtonBar::TabsAtTop:     extraComp = textArea.removeFromTop  (comp.getHeight()); break;
            case TabbedButtonBar::TabsAtLeft:    extraComp = textArea.removeFromTop    (comp.getHeight()); break;
            case TabbedButtonBar::TabsAtRight:   extraComp = textArea.removeFromTop (comp.getHeight()); break;
            default:                             jassertfalse; break;
        }

        // DBG("Extra comp bounds: " << extraComp.toString())
        extraComp.translate(0, 3);
        //DBG("After Extra comp bounds: " << extraComp.toString())

    }
    else if (button.getExtraComponentPlacement() == TabBarButton::belowText)
    {
        switch (orientation)
        {
            case TabbedButtonBar::TabsAtBottom:
            case TabbedButtonBar::TabsAtTop:     extraComp = textArea.removeFromBottom  (comp.getHeight()); break;
            case TabbedButtonBar::TabsAtLeft:    extraComp = textArea.removeFromBottom    (comp.getHeight()); break;
            case TabbedButtonBar::TabsAtRight:   extraComp = textArea.removeFromBottom (comp.getHeight()); break;
            default:                             jassertfalse; break;
        }
    }
    
    return extraComp;
}

void SonoLookAndFeel::createTabTextLayout (const TabBarButton& button, float length, float depth,
                                          Colour colour, TextLayout& textLayout)
{
    float fontsize = button.getExtraComponent() != nullptr ? jmin(depth, 32.0f) * 0.5f : jmin(depth, 32.0f) * 0.5f;
    Font font = myFont.withHeight(fontsize * fontScale);
    font.setUnderline (button.hasKeyboardFocus (false));

    AttributedString s;
    s.setWordWrap(AttributedString::byWord);
    s.setJustification (Justification::centred);
    s.append (button.getButtonText().trim(), font, colour);
    
    textLayout.createLayout (s, length);
}

void SonoLookAndFeel::drawTabButton (TabBarButton& button, Graphics& g, bool isMouseOver, bool isMouseDown)
{
    const Rectangle<int> activeArea (button.getActiveArea());
    
    const TabbedButtonBar::Orientation o = button.getTabbedButtonBar().getOrientation();
    
    const Colour bkg (button.getTabBackgroundColour());
    const Colour selcol = Colour::fromFloatRGBA(0.0f, 0.2f, 0.4f, 1.0f);

    // DBG("Sono draw tab button");
    
    
    if (button.getToggleState() && bkg != Colours::black)
    {
        //g.setColour (bkg);
        g.setColour (selcol);
    }
    else
    {
        
        Point<int> p1, p2;
        
        switch (o)
        {
            case TabbedButtonBar::TabsAtBottom:   p1 = activeArea.getBottomLeft(); p2 = activeArea.getTopLeft();    break;
            case TabbedButtonBar::TabsAtTop:      p1 = activeArea.getTopLeft();    p2 = activeArea.getBottomLeft(); break;
            case TabbedButtonBar::TabsAtRight:    p1 = activeArea.getTopRight();   p2 = activeArea.getTopLeft();    break;
            case TabbedButtonBar::TabsAtLeft:     p1 = activeArea.getTopLeft();    p2 = activeArea.getTopRight();   break;
            default:                              jassertfalse; break;
        }

        g.setColour(isMouseDown ? bkg.brighter(0.1) : bkg);

        //g.setGradientFill (ColourGradient (bkg.darker (0.1f), (float) p1.x, (float) p1.y,
        //                                   bkg.darker (0.5f),   (float) p2.x, (float) p2.y, false));
    }

    Rectangle<int> p (activeArea.reduced(1));

    g.fillRect (p);

    //g.fillRect (activeArea);
   
#if 0
    g.setColour (button.findColour (TabbedButtonBar::tabOutlineColourId));
    
    Rectangle<int> r (activeArea);
    
    if (o != TabbedButtonBar::TabsAtBottom)   g.fillRect (r.removeFromTop (1));
    if (o != TabbedButtonBar::TabsAtTop)      g.fillRect (r.removeFromBottom (1));
    if (o != TabbedButtonBar::TabsAtRight)    g.fillRect (r.removeFromLeft (1));
    if (o != TabbedButtonBar::TabsAtLeft)     g.fillRect (r.removeFromRight (1));
#endif


    const float alpha = button.isEnabled() ? ((isMouseOver || isMouseDown) ? 1.0f : 0.8f) : 0.3f;
    
    Colour col (bkg.contrasting().withMultipliedAlpha (alpha));
    
    if (TabbedButtonBar* bar = button.findParentComponentOfClass<TabbedButtonBar>())
    {
        TabbedButtonBar::ColourIds colID = button.isFrontTab() ? TabbedButtonBar::frontTextColourId
        : TabbedButtonBar::tabTextColourId;
        
        if (button.isColourSpecified (colID))
            col = button.findColour (colID);
        else if (bar->isColourSpecified (colID))
            col = bar->findColour (colID);
        else if (isColourSpecified (colID))
            col = findColour (colID);
    }
    
    const Rectangle<float> area (button.getTextArea().reduced(1).toFloat());
    
    float length = area.getWidth();
    float depth  = area.getHeight();
    
    if (button.getTabbedButtonBar().isVertical())
        std::swap (length, depth);
    
    AffineTransform t;
    
    switch (o)
    {
        case TabbedButtonBar::TabsAtLeft:   t = t.rotated (MathConstants<float>::pi * -0.5f).translated (area.getX(), area.getBottom()); break;
        case TabbedButtonBar::TabsAtRight:  t = t.rotated (MathConstants<float>::pi *  0.5f).translated (area.getRight(), area.getY()); break;
        case TabbedButtonBar::TabsAtTop:
        case TabbedButtonBar::TabsAtBottom: t = t.translated (area.getX(), area.getY()); break;
        default:                            jassertfalse; break;
    }
    
    // Wraps at spaces where the depth has room for two lines, else ends in an ellipsis
    Font font = myFont.withHeight (jmin (depth, 32.0f) * 0.5f * fontScale);
    font.setUnderline (button.hasKeyboardFocus (false));
    g.setColour (col);
    g.setFont (font);
    g.addTransform (t);
    drawTextNoSqueeze (g, button.getButtonText().trim(), { 0, 0, (int) length, (int) depth },
                       Justification::centred, jmax (1, (int) (depth / font.getHeight())));
}

/*
void SonoLookAndFeel::drawTabButton (TabBarButton& button, Graphics& g, bool isMouseOver, bool isMouseDown)
{
    const Rectangle<int> activeArea (button.getActiveArea());

    const Colour bkg (getTabBackgroundColour (button));

    g.setGradientFill (ColourGradient (bkg.brighter (0.1f), 0, (float) activeArea.getY(),
                                       bkg.darker (0.1f), 0, (float) activeArea.getBottom(), false));
    g.fillRect (activeArea);

    g.setColour (button.findColour (TabbedComponent::backgroundColourId).darker (0.3f));
    g.drawRect (activeArea);

    const float alpha = button.isEnabled() ? ((isMouseOver || isMouseDown) ? 1.0f : 0.8f) : 0.3f;
    const Colour col (bkg.contrasting().withMultipliedAlpha (alpha));

    TextLayout textLayout;
    LookAndFeel_V3::createTabTextLayout (button, (float) activeArea.getWidth(), (float) activeArea.getHeight(), col, textLayout);

    textLayout.draw (g, button.getTextArea().toFloat());
}
*/

void SonoLookAndFeel::drawTabbedButtonBarBackground (TabbedButtonBar&, Graphics&) {}

void SonoLookAndFeel::drawTabAreaBehindFrontButton (TabbedButtonBar& bar, Graphics& g, const int w, const int h)
{
    const float shadowSize = 0.15f;
    
    Rectangle<int> shadowRect, line;
    ColourGradient gradient (Colours::black.withAlpha (bar.isEnabled() ? 0.08f : 0.04f), 0, 0,
                             Colours::transparentBlack, 0, 0, false);
    
    switch (bar.getOrientation())
    {
        case TabbedButtonBar::TabsAtLeft:
            gradient.point1.x = (float) w;
            gradient.point2.x = w * (1.0f - shadowSize);
            shadowRect.setBounds ((int) gradient.point2.x, 0, w - (int) gradient.point2.x, h);
            line.setBounds (w - 1, 0, 1, h);
            break;
            
        case TabbedButtonBar::TabsAtRight:
            gradient.point2.x = w * shadowSize;
            shadowRect.setBounds (0, 0, (int) gradient.point2.x, h);
            line.setBounds (0, 0, 1, h);
            break;
            
        case TabbedButtonBar::TabsAtTop:
            gradient.point1.y = (float) h;
            gradient.point2.y = h * (1.0f - shadowSize);
            shadowRect.setBounds (0, (int) gradient.point2.y, w, h - (int) gradient.point2.y);
            line.setBounds (0, h - 1, w, 1);
            break;
            
        case TabbedButtonBar::TabsAtBottom:
            gradient.point2.y = h * shadowSize;
            shadowRect.setBounds (0, 0, w, (int) gradient.point2.y);
            line.setBounds (0, 0, w, 1);
            break;
            
        default: break;
    }
    
    g.setGradientFill (gradient);
    g.fillRect (shadowRect.expanded (2, 2));
    
    g.setColour (bar.findColour (TabbedButtonBar::tabOutlineColourId));
    g.fillRect (line);
}

void SonoLookAndFeel::drawTabButtonText (TabBarButton& button, Graphics& g, bool isMouseOver, bool isMouseDown)
{
    const Rectangle<float> area (button.getTextArea().toFloat());
    
    //DBG("Sono look and feel drawtabbutton text: " << button.getButtonText());
    
    float length = area.getWidth();
    float depth  = area.getHeight();
    
    if (button.getTabbedButtonBar().isVertical())
        std::swap (length, depth);
    
    Font font  = myFont.withHeight(jmin(depth,30.0f) * 0.6f * fontScale);
    font.setUnderline (button.hasKeyboardFocus (false));
    
    AffineTransform t;
    
    switch (button.getTabbedButtonBar().getOrientation())
    {
        case TabbedButtonBar::TabsAtLeft:   t = t.rotated (MathConstants<float>::pi * -0.5f).translated (area.getX(), area.getBottom()); break;
        case TabbedButtonBar::TabsAtRight:  t = t.rotated (MathConstants<float>::pi *  0.5f).translated (area.getRight(), area.getY()); break;
        case TabbedButtonBar::TabsAtTop:
        case TabbedButtonBar::TabsAtBottom: t = t.translated (area.getX(), area.getY()); break;
        default:                            jassertfalse; break;
    }
    
    Colour col;
    
    if (button.isFrontTab() && (button.isColourSpecified (TabbedButtonBar::frontTextColourId)
                                || isColourSpecified (TabbedButtonBar::frontTextColourId)))
        col = findColour (TabbedButtonBar::frontTextColourId);
    else if (button.isColourSpecified (TabbedButtonBar::tabTextColourId)
             || isColourSpecified (TabbedButtonBar::tabTextColourId))
        col = findColour (TabbedButtonBar::tabTextColourId);
    else
        col = button.getTabBackgroundColour().contrasting();
    
    const float alpha = button.isEnabled() ? ((isMouseOver || isMouseDown) ? 1.0f : 0.8f) : 0.3f;
    
    g.setColour (col.withMultipliedAlpha (alpha));
    g.setFont (font);
    g.addTransform (t);
    
    drawTextNoSqueeze (g, button.getButtonText().trim(), { 0, 0, (int) length, (int) depth },
                       Justification::centred, 1);
}

static Range<float> getBrightnessRange (const Image& im)
{
    float minB = 1.0f, maxB = 0;
    const int w = im.getWidth();
    const int h = im.getHeight();

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const float b = im.getPixelAt (x, y).getBrightness();
            minB = jmin (minB, b);
            maxB = jmax (maxB, b);
        }
    }

    return Range<float> (minB, maxB);
}

Font SonoLookAndFeel::getLabelFont (Label& label)
{
    if (fontScale == 1.0f) {
        return label.getFont();
    }
    else {
        return label.getFont().withHeight(label.getFont().getHeight() * fontScale);        
    }
}

void SonoLookAndFeel::drawLabel (Graphics& g, Label& label)
{
    Colour olcolor = label.findColour (Label::backgroundColourId);

    g.setColour(olcolor);
    if (!olcolor.isTransparent()) {
        if (labelCornerRadius > 0.0f) {
            g.fillRoundedRectangle(label.getLocalBounds().reduced(1).toFloat(), labelCornerRadius);
        } else {
            g.fillAll (olcolor);
        }
    }
    
    
    if (! label.isBeingEdited())
    {
        auto alpha = label.isEnabled() ? 1.0f : 0.5f;
        const Font font (getLabelFont (label));
        
        g.setColour (label.findColour (Label::textColourId).withMultipliedAlpha (alpha));
        g.setFont (font);
        
        auto textArea = getLabelBorderSize (label).subtractedFrom (label.getLocalBounds());
        
        // The label's minimum horizontal scale is ignored on purpose: text elides or wraps, never compresses
        drawTextNoSqueeze (g, label.getText(), textArea, label.getJustificationType(),
                           jmax (1, (int) (textArea.getHeight() / font.getHeight())));
        
        olcolor = label.findColour (Label::outlineColourId).withMultipliedAlpha (alpha);
    }
    else if (label.isEnabled())
    {
        olcolor = label.findColour (Label::outlineColourId);
    }
    
    if (!olcolor.isTransparent()) {
        g.setColour (olcolor);
        if (labelCornerRadius > 0.0f) {
            g.drawRoundedRectangle(label.getLocalBounds().reduced(1).toFloat(), labelCornerRadius, 1.0f);
        } else {
            g.drawRect (label.getLocalBounds());        
        }
    }
}

Font SonoLookAndFeel::getTextButtonFont (TextButton& button, int buttonHeight)
{
    // DBG("GetTextButton font with height: " << buttonHeight);
    float textRatio = 0.5f;
    if (SonoTextButton* const textbutt = dynamic_cast<SonoTextButton*> (&button)) {
        textRatio = textbutt->getTextHeightRatio();
    }
    
    return myFont.withHeight(jmin (16.0f, buttonHeight * textRatio) * fontScale);
}

Button* SonoLookAndFeel::createSliderButton (Slider&, const bool isIncrement)
{
    TextButton * butt = new TextButton (isIncrement ? "+" : "-", {});
    return butt;
}

Label* SonoLookAndFeel::createSliderTextBox (Slider& slider)
{
    Label * lab = LookAndFeel_V4::createSliderTextBox(slider);
    lab->setKeyboardType(TextInputTarget::decimalKeyboard);
    lab->setFont(myFont.withHeight(14.0));
    lab->setMinimumHorizontalScale(1.0f);
    lab->setJustificationType(Justification::centredRight);
    return lab;
}

Font SonoLookAndFeel::getSliderPopupFont (Slider&)
{
    return Font (18.0f, Font::bold);
}

int SonoLookAndFeel::getSliderPopupPlacement (Slider&)
{
    return BubbleComponent::above
    //| BubbleComponent::below
    | BubbleComponent::left
    | BubbleComponent::right
    ;
}

void SonoLookAndFeel::drawButtonTextWithAlignment (Graphics& g, TextButton& button, bool /*isMouseOverButton*/, bool /*isButtonDown*/, Justification textjust)
{
    Font font (getTextButtonFont (button, button.getHeight()));
    g.setFont (font);
    g.setColour (button.findColour (button.getToggleState() ? TextButton::textColourOnId
                                    : TextButton::textColourOffId)
                 .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
    
    float textRatio = 0.7f;
    if (SonoTextButton* const textbutt = dynamic_cast<SonoTextButton*> (&button)) {
        textRatio = textbutt->getTextHeightRatio();
    }
    
    const int yIndent = jmin (2, button.proportionOfHeight ((1.0 - textRatio) * 0.5));
    const int cornerSize = jmin (button.getHeight(), button.getWidth()) / 2;
    
    const int fontHeight = roundToInt (font.getHeight() * 0.3);
    const int leftIndent  = jmin (fontHeight, 2 + cornerSize / (button.isConnectedOnLeft() ? 4 : 2));
    const int rightIndent = jmin (fontHeight, 2 + cornerSize / (button.isConnectedOnRight() ? 4 : 2));
    
    drawTextNoSqueeze (g, button.getButtonText(),
                       { leftIndent, yIndent, button.getWidth() - leftIndent - rightIndent, button.getHeight() - yIndent * 2 },
                       textjust, 2);
}


void SonoLookAndFeel::drawButtonText (Graphics& g, TextButton& button, bool isMouseOverButton, bool isButtonDown)
{
    drawButtonTextWithAlignment(g, button, isMouseOverButton, isButtonDown);
}


void SonoLookAndFeel::drawFileBrowserRow (Graphics& g, int width, int height,
                                         const File& file, const String& filename, Image* icon,
                                         const String& fileSizeDescription,
                                         const String& fileTimeDescription,
                                         bool isDirectory, bool isItemSelected,
                                         int itemIndex, DirectoryContentsDisplayComponent& dcc)
{
    Component* const fileListComp = dynamic_cast<Component*> (&dcc);

    if (isItemSelected)
        g.fillAll (fileListComp != nullptr ? fileListComp->findColour (DirectoryContentsDisplayComponent::highlightColourId)
                   : findColour (DirectoryContentsDisplayComponent::highlightColourId));

    int x = 32;
    g.setColour (Colours::black);

    if (isDirectory) {
        if (icon != nullptr && icon->isValid())
        {
            g.drawImageWithin (*icon, 2, 2, x - 4, height - 4,
                               RectanglePlacement::centred | RectanglePlacement::onlyReduceInSize,
                               false);
        }
        else
        {
            if (const Drawable* d = isDirectory ? getDefaultFolderImage()
                : getDefaultDocumentFileImage())
                d->drawWithin (g, Rectangle<float> (2.0f, 2.0f, x - 4.0f, height - 4.0f),
                               RectanglePlacement::centred | RectanglePlacement::onlyReduceInSize, 1.0f);
        }
    }
    else {
        x = 4;
    }

    g.setColour (fileListComp != nullptr ? fileListComp->findColour (DirectoryContentsDisplayComponent::textColourId)
                 : findColour (DirectoryContentsDisplayComponent::textColourId));
    g.setFont (myFont.withHeight(height * 0.5f));

    if (width > 450 && ! isDirectory)
    {
        const int sizeX = roundToInt (width * 0.7f);
        const int dateX = roundToInt (width * 0.8f);

        g.drawFittedText (filename,
                          x, 0, sizeX - x, height,
                          Justification::centredLeft, 1, 1.0f);

        g.setFont (myFont.withHeight(height * 0.5f));
        g.setColour (Colours::darkgrey);

        if (! isDirectory)
        {
            g.drawFittedText (fileSizeDescription,
                              sizeX, 0, dateX - sizeX - 8, height,
                              Justification::centredRight, 1, 1.0f);

            g.drawFittedText (fileTimeDescription,
                              dateX, 0, width - 8 - dateX, height,
                              Justification::centredRight, 1, 1.0f);
        }
    }
    else
    {
        g.drawFittedText (filename,
                          x, 0, width - x, height,
                          Justification::centredLeft, 1, 1.0f);

    }
}

Button* SonoLookAndFeel::createFileBrowserGoUpButton()
{
    DrawableButton* goUpButton = new DrawableButton ("up", DrawableButton::ImageOnButtonBackground);

    Path arrowPath;
    arrowPath.addArrow (Line<float> (50.0f, 100.0f, 50.0f, 0.0f), 40.0f, 100.0f, 50.0f);

    DrawablePath arrowImage;
    arrowImage.setFill (Colours::white.withAlpha (0.4f));
    arrowImage.setPath (arrowPath);

    goUpButton->setImages (&arrowImage);

    return goUpButton;
}

void SonoLookAndFeel::layoutFileBrowserComponent (FileBrowserComponent& browserComp,
                                                 DirectoryContentsDisplayComponent* fileListComponent,
                                                 FilePreviewComponent* previewComp,
                                                 ComboBox* currentPathBox,
                                                 TextEditor* filenameBox,
                                                 Button* goUpButton)
{
    const int x = 8;
    int w = browserComp.getWidth() - x - x;

    if (previewComp != nullptr)
    {
        const int previewWidth = w / 3;
        previewComp->setBounds (x + w - previewWidth, 0, previewWidth, browserComp.getHeight());

        w -= previewWidth + 4;
    }

    int y = 4;

    const int controlsHeight = 22;
    const int bottomSectionHeight = controlsHeight + 8;
    const int upButtonWidth = 50;

    currentPathBox->setBounds (x, y, w - upButtonWidth - 6, controlsHeight);
    goUpButton->setBounds (x + w - upButtonWidth, y, upButtonWidth, controlsHeight);

    y += controlsHeight + 4;

    if (Component* const listAsComp = dynamic_cast <Component*> (fileListComponent))
    {
        listAsComp->setBounds (x, y, w, browserComp.getHeight() - y - bottomSectionHeight);
        y = listAsComp->getBottom() + 4;
    }

    filenameBox->setBounds (x + 50, y, w - 50, controlsHeight);
}

Font SonoLookAndFeel::getComboBoxFont (ComboBox& box)
{
    return { jmin (16.0f, (float) box.getHeight() * 0.9f) };
}

// Captioned combo box: caption line, value line under it, small arrow at the right of the value line.
// Caption type and colour match the captioned buttons (drawDrawableButton, "leadText").
static constexpr int kLeadCaptionH = 11;
static constexpr int kLeadArrowW = 14;

static juce::Rectangle<int> leadComboValueArea (int width, int height)
{
    const int valueH = jmin (22, jmax (12, height - kLeadCaptionH - 2));
    const int top = jmax (1, (height - kLeadCaptionH - valueH) / 2);
    return { 1, top + kLeadCaptionH, jmax (0, width - 1 - kLeadArrowW), valueH };
}

void SonoLookAndFeel::drawComboBox (Graphics& g, int width, int height, bool isButtonDown,
                                    int buttonX, int buttonY, int buttonW, int buttonH, ComboBox& box)
{
    if (! box.getProperties().contains ("leadText")) {
        LookAndFeel_V4::drawComboBox (g, width, height, isButtonDown, buttonX, buttonY, buttonW, buttonH, box);
        return;
    }

    const auto bounds = juce::Rectangle<int> (0, 0, width, height).toFloat();
    g.setColour (box.findColour (ComboBox::backgroundColourId));
    g.fillRoundedRectangle (bounds, 3.0f);
    g.setColour (box.findColour (ComboBox::outlineColourId));
    g.drawRoundedRectangle (bounds.reduced (0.5f, 0.5f), 3.0f, 1.0f);

    const auto value = leadComboValueArea (width, height);
    const auto textColour = box.findColour (ComboBox::textColourId).withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.4f);
    g.setFont (Font (10.0f));
    g.setColour (textColour.withMultipliedAlpha (0.6f));
    g.drawText (box.getProperties()["leadText"].toString(), 6, value.getY() - kLeadCaptionH, jmax (0, width - 10), kLeadCaptionH,
                Justification::centredLeft, false);

    const float cx = (float) (width - 2 - kLeadArrowW / 2), cy = (float) value.getCentreY();
    Path arrow;
    arrow.startNewSubPath (cx - 4.0f, cy - 2.0f);
    arrow.lineTo (cx, cy + 2.0f);
    arrow.lineTo (cx + 4.0f, cy - 2.0f);
    g.setColour (box.findColour (ComboBox::arrowColourId).withAlpha (box.isEnabled() ? 0.9f : 0.2f));
    g.strokePath (arrow, PathStrokeType (1.5f));
}

void SonoLookAndFeel::positionComboBoxText (ComboBox& box, Label& label, Drawable* image)
{
    if (! box.getProperties().contains ("leadText")) {
        LookAndFeel_V4::positionComboBoxText (box, label, image);
        return;
    }
    label.setBounds (leadComboValueArea (box.getWidth(), box.getHeight()));
    label.setBorderSize ({ 0, 5, 0, 2 });
    label.setFont (Font (15.0f));
}

void SonoLookAndFeel::drawComboBoxTextWhenNothingSelected (Graphics& g, ComboBox& box, Label& label)
{
    if (! box.getProperties().contains ("leadText")) {
        LookAndFeel_V4::drawComboBoxTextWhenNothingSelected (g, box, label);
        return;
    }
    // Drawn where the value goes (the stock version assumes the label sits at the box origin)
    g.setColour (findColour (ComboBox::textColourId).withMultipliedAlpha (0.5f));
    const auto font = getLabelFont (label);
    g.setFont (font);
    const auto area = label.getBorderSize().subtractedFrom (label.getBounds());
    drawTextNoSqueeze (g, box.getTextWhenNothingSelected(), area, label.getJustificationType(),
                       jmax (1, (int) ((float) area.getHeight() / font.getHeight())));
}

Font SonoLookAndFeel::getPopupMenuFont()
{
    return Font (17.0f * fontScale);
}

void SonoLookAndFeel::drawPopupMenuBackground (Graphics& g, [[maybe_unused]] int width, [[maybe_unused]] int height)
{
    g.fillAll (findColour (PopupMenu::backgroundColourId));

   #if ! JUCE_MAC
    g.setColour (findColour (PopupMenu::textColourId).brighter(0.1f).withAlpha(0.1f));
    g.drawRect (0, 0, width, height);
   #endif
}

PopupMenu::Options SonoLookAndFeel::getOptionsForComboBoxPopupMenu (ComboBox& box, Label& label)
{
    auto options = PopupMenu::Options().withTargetComponent (&box)
                               .withItemThatMustBeVisible (box.getSelectedId())
                               .withMinimumWidth (box.getWidth())
                               .withMaximumNumColumns (1)
                               .withStandardItemHeight (label.getHeight());

#if JUCE_IOS || JUCE_ANDROID
    auto * dw = box.findParentComponentOfClass<AudioProcessorEditor>();
    if (dw) {
        options = options.withParentComponent(dw);
    }
#endif

    return options;
}

void SonoLookAndFeel::drawTreeviewPlusMinusBox (Graphics& g, const Rectangle<float>& area,
                                               Colour backgroundColour, bool isOpen, bool isMouseOver)
{
    Path p;
    p.addTriangle (0.0f, 0.0f, 1.0f, isOpen ? 0.0f : 0.5f, isOpen ? 0.5f : 0.0f, 1.0f);

    DBG("draw plus minus ours");

    //g.setColour (backgroundColour.contrasting().withAlpha (isMouseOver ? 0.5f : 0.3f));
    g.setColour (Colours::white.withAlpha (isMouseOver ? 0.5f : 0.3f));
    g.fillPath (p, p.getTransformToScaleToFit (area.reduced (2, area.getHeight() / 4), true));
}


void SonoLookAndFeel::drawToggleButton (Graphics& g, ToggleButton& button,
                                       bool isMouseOverButton, bool isButtonDown)
{
    /*
    if (button.hasKeyboardFocus (true))
    {
        g.setColour (button.findColour (TextEditor::focusedOutlineColourId));
        g.drawRect (0, 0, button.getWidth(), button.getHeight());
    }
     */

    float fontSize = jmin (15.0f, button.getHeight() * 0.75f) * fontScale;
    const float tickWidth = fontSize * 1.1f;

    drawTickBox (g, button, 4.0f, (button.getHeight() - tickWidth) * 0.5f,
                 tickWidth, tickWidth,
                 button.getToggleState(),
                 button.isEnabled(),
                 isMouseOverButton,
                 isButtonDown);

    g.setColour (button.findColour (ToggleButton::textColourId));
    g.setFont (myFont.withHeight(fontSize));

    if (! button.isEnabled())
        g.setOpacity (0.5f);

    const int textX = (int) tickWidth + 10;

    drawTextNoSqueeze (g, button.getButtonText(),
                       { textX, 0, button.getWidth() - textX - 2, button.getHeight() },
                       Justification::centredLeft, 10);
}

void SonoLookAndFeel::drawTickBox (Graphics& g, Component& component,
                                  float x, float y, float w, float h,
                                  const bool ticked,
                                  const bool isEnabled,
                                  const bool isMouseOverButton,
                                  const bool isButtonDown)
{
    const float boxSize = w * 1.0f;


    g.setColour (component.findColour (TextEditor::focusedOutlineColourId));
    g.drawRect (x, y + (h - boxSize) * 0.5f, boxSize, boxSize);

    if (ticked)
    {
        Path tick;
        tick.startNewSubPath (1.5f, 3.0f);
        tick.lineTo (3.0f, 6.0f);
        tick.lineTo (6.0f, 0.0f);

        
        g.setColour (isEnabled ? component.findColour(ToggleButton::tickColourId) : Colours::grey);

        const AffineTransform trans (AffineTransform::scale (w / 9.0f, h / 9.0f)
                                     .translated (x+2, y+1));

        g.strokePath (tick, PathStrokeType (2.5f), trans);
    }
}

void SonoLookAndFeel::drawRotarySlider (Graphics& g, int x, int y, int width, int height, float sliderPos,
                                       const float rotaryStartAngle, const float rotaryEndAngle, Slider& slider)
{
    const auto outline = findColour (Slider::rotarySliderOutlineColourId);
    const auto fill    = findColour (Slider::rotarySliderFillColourId);
    
    const auto bounds = Rectangle<int> (x, y, width, height).toFloat().reduced (3);
    
    
    auto radius = jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f;
    const auto toAngle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
    auto lineW = jmin (8.0f, radius * 0.3f);
    auto arcRadius = radius - lineW * 0.5f;
    
    Path backgroundArc;
    backgroundArc.addCentredArc (bounds.getCentreX(),
                                 bounds.getCentreY(),
                                 arcRadius,
                                 arcRadius,
                                 0.0f,
                                 rotaryStartAngle,
                                 rotaryEndAngle,
                                 true);
    
    g.setColour (outline);
    g.strokePath (backgroundArc, PathStrokeType (lineW, PathStrokeType::curved, PathStrokeType::rounded));
    
    auto rotStartAngle = rotaryStartAngle;
    
    if (slider.getProperties().contains ("fromCentre"))
    {
        rotStartAngle = (rotStartAngle + rotaryEndAngle) / 2;
    }

    
    if (slider.isEnabled())
    {
        Path valueArc;
        valueArc.addCentredArc (bounds.getCentreX(),
                                bounds.getCentreY(),
                                arcRadius,
                                arcRadius,
                                0.0f,
                                rotStartAngle,
                                toAngle,
                                true);
        
        g.setColour (fill);
        g.strokePath (valueArc, PathStrokeType (lineW, PathStrokeType::curved, PathStrokeType::rounded));
    }
    
    const auto thumbWidth = lineW ; // * 1.5f;
    const Point<float> thumbPoint (bounds.getCentreX() + arcRadius * std::cos (toAngle - MathConstants<float>::pi * 0.5f),
                                   bounds.getCentreY() + arcRadius * std::sin (toAngle - MathConstants<float>::pi * 0.5f));
    
    g.setColour (findColour (Slider::thumbColourId));
    g.fillEllipse (Rectangle<float> (thumbWidth, thumbWidth).withCentre (thumbPoint));
}

//==============================================================================
int SonoLookAndFeel::getSliderThumbRadius (Slider& slider)
{
    if (slider.isTwoValue() || slider.isThreeValue()) {
        return jmin (14, slider.isHorizontal() ? static_cast<int> (slider.getHeight() * 0.25f)
                                               : static_cast<int> (slider.getWidth()  * 0.5f));        
    }
    return jmin (16, slider.isHorizontal() ? static_cast<int> (slider.getHeight() * 0.5f)
                                           : static_cast<int> (slider.getWidth()  * 0.5f));
}


Slider::SliderLayout SonoLookAndFeel::getSliderLayout (Slider& slider)
{
    // 1. compute the actually visible textBox size from the slider textBox size and some additional constraints

    int minXSpace = 0;
    int minYSpace = 0;

    auto textBoxPos = slider.getTextBoxPosition();

    if (textBoxPos == Slider::TextBoxLeft || textBoxPos == Slider::TextBoxRight)
        minXSpace = 30;
    else
        minYSpace = 15;

    auto localBounds = slider.getLocalBounds();

    auto textBoxWidth  = jmax (0, jmin (slider.getTextBoxWidth(),  localBounds.getWidth() - minXSpace));
    auto textBoxHeight = jmax (0, jmin (slider.getTextBoxHeight(), localBounds.getHeight() - minYSpace));

    Slider::SliderLayout layout;

    // 2. set the textBox bounds

    if (textBoxPos != Slider::NoTextBox)
    {
        if (slider.isBar())
        {
            layout.textBoxBounds = localBounds;
        }
        else
        {
            layout.textBoxBounds.setWidth (textBoxWidth);
            layout.textBoxBounds.setHeight (textBoxHeight);

            const int thumbIndent = getSliderThumbRadius (slider);

            if (textBoxPos == Slider::TextBoxLeft)           layout.textBoxBounds.setX (0);
            else if (textBoxPos == Slider::TextBoxRight)     layout.textBoxBounds.setX (localBounds.getWidth() - textBoxWidth);
            else if (sliderTextJustification.testFlags(Justification::right))/* above or below -> right */ layout.textBoxBounds.setX ((localBounds.getWidth() - textBoxWidth - 1));
            else if (sliderTextJustification.testFlags(Justification::left))/* above or below -> left */ layout.textBoxBounds.setX (1);
            else /* above or below -> centre horizontally */ layout.textBoxBounds.setX ((localBounds.getWidth() - textBoxWidth) / 2);

            if (textBoxPos == Slider::TextBoxAbove)          layout.textBoxBounds.setY (0);
            else if (textBoxPos == Slider::TextBoxBelow)     layout.textBoxBounds.setY (localBounds.getHeight() - textBoxHeight);
            else if (sliderTextJustification.testFlags(Justification::top))/* left or right -> top */ layout.textBoxBounds.setY (0);
            else if (sliderTextJustification.testFlags(Justification::bottom))/* left or right -> bottom */ layout.textBoxBounds.setY (localBounds.getHeight() - textBoxHeight);
            else /* left or right -> centre vertically */    layout.textBoxBounds.setY ((localBounds.getHeight() - textBoxHeight) / 2);
        }
    }

    // 3. set the slider bounds

    layout.sliderBounds = localBounds;

    if (slider.isBar())
    {
        layout.sliderBounds.reduce (1, 1);   // bar border
    }
    else
    {
        if (textBoxPos == Slider::TextBoxLeft)       layout.sliderBounds.removeFromLeft (textBoxWidth);
        else if (textBoxPos == Slider::TextBoxRight) layout.sliderBounds.removeFromRight (textBoxWidth);
        else if (textBoxPos == Slider::TextBoxAbove) layout.sliderBounds.removeFromTop (textBoxHeight);
        else if (textBoxPos == Slider::TextBoxBelow) layout.sliderBounds.removeFromBottom (textBoxHeight);

        const int thumbIndent = getSliderThumbRadius (slider);

        if (slider.isHorizontal())    layout.sliderBounds.reduce (thumbIndent, 0);
        else if (slider.isVertical()) layout.sliderBounds.reduce (0, thumbIndent);
    }

    return layout;
}


void SonoLookAndFeel::drawLinearSlider (Graphics& g, int x, int y, int width, int height,
                                       float sliderPos,
                                       float minSliderPos,
                                       float maxSliderPos,
                                       const Slider::SliderStyle style, Slider& slider)
{
    if (slider.isBar())
    {
        if (slider.getProperties().contains ("fromCentre")) {
            auto centrex = x + width*0.5f;
            auto centrey = y + height*0.5f;
            
            if (!slider.getProperties().contains ("noFill")) {
                g.setColour (slider.findColour (Slider::trackColourId));

                g.fillRect (slider.isHorizontal() ? Rectangle<float> (sliderPos > centrex ? centrex : sliderPos, y + 0.5f, sliderPos > centrex ? sliderPos - centrex : centrex - sliderPos, height - 1.0f)
                            : Rectangle<float> (x + 0.5f, sliderPos < centrey ? sliderPos : centrey, width - 1.0f, sliderPos < centrey ?  centrey - sliderPos : sliderPos - centrey));
            }
            
            // draw line
            g.setColour (slider.findColour (Slider::thumbColourId));

            g.fillRect (slider.isHorizontal() ? Rectangle<float> (sliderPos - 1, y + 0.5f, 2, height - 1.0f)
                        : Rectangle<float> (x + 0.5f, sliderPos - 1, width - 1.0f, 2));
        }
        else {
            
            if (!slider.getProperties().contains ("noFill")) {

                g.setColour (slider.findColour (Slider::trackColourId));

                g.fillRect (slider.isHorizontal() ? Rectangle<float> (static_cast<float> (x), y + 0.5f, sliderPos - x, height - 1.0f)
                            : Rectangle<float> (x + 0.5f, sliderPos, width - 1.0f, y + (height - sliderPos)));
            }
            //else

            g.setColour (slider.findColour (Slider::thumbColourId));

            {
                // draw line
                g.fillRect (slider.isHorizontal() ? Rectangle<float> (sliderPos - 1, y + 0.5f, 3, height - 1.0f)
                            : Rectangle<float> (x + 0.5f, sliderPos - 1, width - 1.0f, 3));
            }
        }
    }
    else
    {
        auto isTwoVal   = (style == Slider::SliderStyle::TwoValueVertical   || style == Slider::SliderStyle::TwoValueHorizontal);
        auto isThreeVal = (style == Slider::SliderStyle::ThreeValueVertical || style == Slider::SliderStyle::ThreeValueHorizontal);
        
        auto trackWidth = jmin (10.0f, slider.isHorizontal() ? height * 0.25f : width * 0.25f);
        
        Point<float> startPoint (slider.isHorizontal() ? x : x + width * 0.5f,
                                 slider.isHorizontal() ? y + height * 0.5f : height + y);
        
        Point<float> endPoint (slider.isHorizontal() ? width + x : startPoint.x,
                               slider.isHorizontal() ? startPoint.y : y);
        
        Path backgroundTrack;
        backgroundTrack.startNewSubPath (startPoint);
        backgroundTrack.lineTo (endPoint);
        g.setColour (slider.findColour (Slider::backgroundColourId));
        g.strokePath (backgroundTrack, { trackWidth, PathStrokeType::curved, PathStrokeType::rounded });
        
        Path valueTrack;
        Point<float> minPoint, maxPoint, thumbPoint;
        
        if (isTwoVal || isThreeVal)
        {
            minPoint = { slider.isHorizontal() ? minSliderPos : width * 0.5f,
                slider.isHorizontal() ? height * 0.5f : minSliderPos };
            
            if (isThreeVal)
                thumbPoint = { slider.isHorizontal() ? sliderPos : width * 0.5f,
                    slider.isHorizontal() ? height * 0.5f : sliderPos };
            
            maxPoint = { slider.isHorizontal() ? maxSliderPos : width * 0.5f,
                slider.isHorizontal() ? height * 0.5f : maxSliderPos };
        }
        else
        {
            auto kx = slider.isHorizontal() ? sliderPos : (x + width * 0.5f);
            auto ky = slider.isHorizontal() ? (y + height * 0.5f) : sliderPos;
            
            minPoint = startPoint;
            maxPoint = { kx, ky };
        }
        
        auto thumbWidth = getSliderThumbRadius (slider);
        
        valueTrack.startNewSubPath (minPoint);
        valueTrack.lineTo (isThreeVal ? thumbPoint : maxPoint);
        g.setColour (slider.findColour (Slider::trackColourId));
        g.strokePath (valueTrack, { trackWidth, PathStrokeType::curved, PathStrokeType::rounded });
        
        if (! isTwoVal)
        {
            g.setColour (slider.findColour (Slider::thumbColourId));
            g.fillEllipse (Rectangle<float> (static_cast<float> (thumbWidth), static_cast<float> (thumbWidth)).withCentre (isThreeVal ? thumbPoint : maxPoint));
        }
        
        if (isTwoVal || isThreeVal)
        {
            auto sr = jmin (trackWidth, (slider.isHorizontal() ? height : width) * 0.4f);
            auto pointerColour = slider.findColour (Slider::thumbColourId);
            
            auto wscale = 1.5f;
            
            if (slider.isHorizontal())
            {
                /*
                drawPointer (g, minSliderPos - sr,
                             height * 0.5f - trackWidth*wscale*0.5, //jmax (0.0f, y + height * 0.5f - trackWidth * 0.5f),
                             trackWidth * wscale, pointerColour, 1); // 2
                
                drawPointer (g, maxSliderPos - trackWidth,
                             height * 0.5f - trackWidth*wscale*0.5, //jmin (y + height - trackWidth * 2.0f, (float)y + height * 0.5f),
                             trackWidth * wscale, pointerColour, 3); // 4
                */
                
                drawPointer (g, minSliderPos - sr,
                             jmax (0.0f, y + height * 0.5f - trackWidth * wscale),
                             trackWidth * wscale, pointerColour, 2); // 2
                
                drawPointer (g, maxSliderPos - trackWidth*0.5*wscale,
                             jmin (y + height - trackWidth * wscale, (float)y + height * 0.5f),
                             trackWidth * wscale, pointerColour, 4); // 4
            }
            else
            {
                drawPointer (g, jmax (0.0f, x + width * 0.5f - trackWidth * 2.0f),
                             minSliderPos - trackWidth,
                             trackWidth * wscale, pointerColour, 1);
                
                drawPointer (g, jmin (x + width - trackWidth * 2.0f, x + width * 0.5f), maxSliderPos - sr,
                             trackWidth * wscale, pointerColour, 3);
            }
        }
    }
}


void SonoLookAndFeel::drawDrawableButton (Graphics& g, DrawableButton& button,
                                         bool isMouseOverButton, bool isButtonDown)
{
    const auto cornerSize = 6.0f;
    bool toggleState = button.getToggleState();
    
    ;

    //Rectangle<float> bounds = g.getClipBounds().toFloat();
    Rectangle<float> bounds = button.getLocalBounds().toFloat();

    g.setColour(button.findColour (toggleState ? DrawableButton::backgroundOnColourId
                                   : DrawableButton::backgroundColourId));
    //g.fillAll();
    g.fillRoundedRectangle(bounds, cornerSize);
    
    if (isButtonDown) {
        g.setColour(findColour(SonoDrawableButton::downOverlayColourId));
        //g.fillAll();
        g.fillRoundedRectangle(bounds, cornerSize);
    }
    else if (isMouseOverButton) {
        g.setColour(findColour(SonoDrawableButton::overOverlayColourId));
        //g.fillAll();
        g.fillRoundedRectangle(bounds, cornerSize);
    }
    
    //g.fillAll (button.findColour (toggleState ? DrawableButton::backgroundOnColourId
    //                              : DrawableButton::backgroundColourId));
    
    int textH = 0;
    int textW = 0;
    float imageratio = 0.75f;

    if (SonoDrawableButton* const sonobutt = dynamic_cast<SonoDrawableButton*> (&button)) {
        imageratio = sonobutt->getForegroundImageRatio();
    }

    if (button.getStyle() == DrawableButton::ImageAboveTextLabel || button.getStyle() == DrawableButton::ImageBelowTextLabel) {
        textH = jmin (14, button.proportionOfHeight (0.2f));
    } else if (button.getStyle() == DrawableButton::ImageLeftOfTextLabel || button.getStyle() == DrawableButton::ImageRightOfTextLabel) {
        textH = jmin (14, button.proportionOfHeight (0.8f));
        textW = jmax (20, button.proportionOfWidth (1.0f - imageratio));
    }


    if (textH > 0)
    {
        g.setFont (myFont.withHeight((float) textH * fontScale));
        
        g.setColour (button.findColour (toggleState ? DrawableButton::textColourOnId
                                        : DrawableButton::textColourId)
                     .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.4f));

        if (button.getStyle() == DrawableButton::ImageAboveTextLabel) {

            drawTextNoSqueeze (g, button.getButtonText(),
                               { 2, button.getHeight() - textH - 1, button.getWidth() - 4, textH },
                               Justification::centred, 1);
        }
        else if (button.getStyle() == DrawableButton::ImageBelowTextLabel) {
            drawTextNoSqueeze (g, button.getButtonText(),
                               { 2, 1, button.getWidth() - 4, textH },
                               Justification::centred, 1);
        }
        else if (button.getStyle() == DrawableButton::ImageRightOfTextLabel) {
            const bool leftAligned = drawableTextJustification.testFlags(Justification::left);
            const int lpad = leftAligned ? 6 : 2;   
            drawTextNoSqueeze (g, button.getButtonText(),
                               { lpad, 1, textW - (lpad - 2), button.getHeight() - 2 },
                               drawableTextJustification, leftAligned ? 1 : 2);
        }
        else if (button.getStyle() == DrawableButton::ImageLeftOfTextLabel && button.getProperties().contains ("leadText")) {
            // A small caption at the top right, the text under it, with no image.
            const String lead = button.getProperties()["leadText"].toString();
            const auto textFont = g.getCurrentFont();
            const auto textColour = button.findColour (toggleState ? DrawableButton::textColourOnId : DrawableButton::textColourId)
                                        .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.4f);
            const int captionH = 11;
            const int top = jmax (1, (button.getHeight() - captionH - textH) / 2);
            g.setFont (Font (10.0f));
            g.setColour (textColour.withMultipliedAlpha (0.6f));
            g.drawText (lead, 4, top, button.getWidth() - 10, captionH, Justification::centredRight, false);
            g.setFont (textFont);
            g.setColour (textColour);
            // Too long: keep the part up to " › " (the channel numbers) and elide the names with "…"
            String text = button.getButtonText();
            const float tw = (float) (button.getWidth() - 10);
            const String sep = String::fromUTF8 (" \xe2\x80\xba ");
            if (textFont.getStringWidthFloat (text) > tw && text.contains (sep)) {
                const String ell = String::fromUTF8 ("\xe2\x80\xa6");
                const String head = text.upToFirstOccurrenceOf (sep, true, false);
                String tail = text.fromFirstOccurrenceOf (sep, false, false);
                while (tail.isNotEmpty() && textFont.getStringWidthFloat (head + tail + ell) > tw)
                    tail = tail.dropLastCharacters (1);
                text = head + tail.trimEnd() + ell;
            }
            drawTextNoSqueeze (g, text, { 4, top + captionH, button.getWidth() - 10, textH },
                               Justification::centredRight, 1);
        }
        else if (button.getStyle() == DrawableButton::ImageLeftOfTextLabel) {
            drawTextNoSqueeze (g, button.getButtonText(),
                               { button.getWidth() - textW - 4, 1, textW, button.getHeight() - 2 },
                               Justification::centred, 2);
        }
    }
}


void SonoLookAndFeel::drawBubble (Graphics& g, BubbleComponent& comp,
                                 const Point<float>& tip, const Rectangle<float>& body)
{
    Path p;
    p.addBubble (body.reduced (0.5f), body.getUnion (Rectangle<float> (tip.x, tip.y, 1.0f, 1.0f)),
                 tip, 5.0f, jmin (10.0f, body.getWidth() * 0.2f, body.getHeight() * 0.2f));
    
    g.setColour (comp.findColour (BubbleComponent::backgroundColourId));
    g.fillPath (p);
    
    g.setColour (comp.findColour (BubbleComponent::outlineColourId));
    g.strokePath (p, PathStrokeType (1.0f));
}


SonoBigTextLookAndFeel::SonoBigTextLookAndFeel(float maxTextSize)
: maxSize(maxTextSize)
{
    
}

Font SonoBigTextLookAndFeel::getTextButtonFont (TextButton& button, int buttonHeight)
{
    // DBG("GetTextButton font with height: " << buttonHeight << " maxsize: " << maxSize);
    float textRatio = 0.8f;
    if (SonoTextButton* const textbutt = dynamic_cast<SonoTextButton*> (&button)) {
        textRatio = textbutt->getTextHeightRatio();
    }
    
    return myFont.withHeight(jmin (maxSize, buttonHeight * textRatio) * fontScale);
}

Label* SonoBigTextLookAndFeel::createSliderTextBox (Slider& slider)
{
    Label * lab = LookAndFeel_V4::createSliderTextBox(slider);
    lab->setKeyboardType(TextInputTarget::decimalKeyboard);
    lab->setFont(myFont.withHeight(maxSize));
    lab->setJustificationType(textJustification);
    lab->setMinimumHorizontalScale(1.0f);

    return lab;
}

Button* SonoBigTextLookAndFeel::createSliderButton (Slider&, const bool isIncrement)
{
    TextButton * butt = new TextButton (isIncrement ? "+" : "-", {});
    butt->setLookAndFeel(this);
    return butt;
}


void SonoBigTextLookAndFeel::drawToggleButton (Graphics& g, ToggleButton& button,
                                       bool isMouseOverButton, bool isButtonDown)
{
    /*
    if (button.hasKeyboardFocus (true))
    {
        g.setColour (button.findColour (TextEditor::focusedOutlineColourId));
        g.drawRect (0, 0, button.getWidth(), button.getHeight());
    }
     */

    float fontSize = jmin (maxSize, button.getHeight() * 0.75f) * fontScale;
    const float tickWidth = fontSize * 1.1f;

    drawTickBox (g, button, 4.0f, (button.getHeight() - tickWidth) * 0.5f,
                 tickWidth, tickWidth,
                 button.getToggleState(),
                 button.isEnabled(),
                 isMouseOverButton,
                 isButtonDown);

    g.setColour (button.findColour (ToggleButton::textColourId));
    g.setFont (myFont.withHeight(fontSize));

    if (! button.isEnabled())
        g.setOpacity (0.5f);

    const int textX = (int) tickWidth + 10;

    drawTextNoSqueeze (g, button.getButtonText(),
                       { textX, 0, button.getWidth() - textX - 2, button.getHeight() },
                       Justification::centredLeft, 10);
}


// pan slider look and feel

SonoPanSliderLookAndFeel::SonoPanSliderLookAndFeel(float maxTextSize)
: maxSize(maxTextSize)
{
    setColour (TooltipWindow::textColourId, Colour(0xeecccccc));
   // setColour (TooltipWindow::backgroundColourId, Colour(0xeeffff99));
    //setColour (Slider::textBoxBackgroundColourId, Colour::fromFloatRGBA(0.05, 0.05, 0.05, 1.0));
    setColour (Slider::textBoxBackgroundColourId, Colours::transparentBlack);
    setColour (Slider::textBoxHighlightColourId, Colour (0xaa555555));
    setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);

}

Label* SonoPanSliderLookAndFeel::createSliderTextBox (Slider& slider)
{
    Label * lab = LookAndFeel_V4::createSliderTextBox(slider);
    lab->setKeyboardType(TextInputTarget::decimalKeyboard);
    lab->setFont(myFont.withHeight(maxSize));
    lab->setJustificationType(textJustification);
    lab->setMinimumHorizontalScale(1.0f);

    return lab;
}

Button* SonoPanSliderLookAndFeel::createSliderButton (Slider&, const bool isIncrement)
{
    TextButton * butt = new TextButton (isIncrement ? "+" : "-", {});
    butt->setLookAndFeel(this);
    return butt;
}

int SonoPanSliderLookAndFeel::getSliderThumbRadius (Slider& slider)
{
    //if (slider.isTwoValue() || slider.isThreeValue()) {
        return jmin (14, slider.isHorizontal() ? static_cast<int> (slider.getHeight() * 0.25f)
                                               : static_cast<int> (slider.getWidth()  * 0.5f));        
    //}
    //return jmin (16, slider.isHorizontal() ? static_cast<int> (slider.getHeight() * 0.5f)
    //                                       : static_cast<int> (slider.getWidth()  * 0.5f));
}

Font SonoPanSliderLookAndFeel::getSliderPopupFont (Slider&)
{
    return Font (maxSize * fontScale, Font::bold);
}

int SonoPanSliderLookAndFeel::getSliderPopupPlacement (Slider&)
{
    return BubbleComponent::above
    //| BubbleComponent::below
    | BubbleComponent::left
    | BubbleComponent::right
    ;
}


void SonoPanSliderLookAndFeel::drawRotarySlider (Graphics& g, int x, int y, int width, int height, float sliderPos,
                                                 const float rotaryStartAngle, const float rotaryEndAngle, Slider& slider)
{
    if (! slider.getProperties().contains ("sbKnob")) {
        SonoLookAndFeel::drawRotarySlider (g, x, y, width, height, sliderPos, rotaryStartAngle, rotaryEndAngle, slider);
        return;
    }

    // Caption, knob and value are stacked from the top, so knobs of different sizes line their captions up
    const float captionH = 10.0f, valueH = 11.0f;
    const bool labelled = height >= 50;
    const float wanted = (float) (int) slider.getProperties().getWithDefault ("knobSize", 40);
    auto area = juce::Rectangle<int> (x, y, width, height).toFloat();
    const float d = labelled ? jmin (wanted, area.getWidth(), area.getHeight() - captionH - valueH)
                             : jmin (wanted, area.getWidth(), area.getHeight());
    if (d < 8.0f) return;

    const float cx = area.getCentreX();
    const float cy = labelled ? area.getY() + captionH + d * 0.5f : area.getCentreY();
    const float k = d / 44.0f;
    const float alpha = slider.isEnabled() ? 1.0f : 0.4f;
    const float toAngle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
    const bool fromCentre = slider.getProperties().contains ("fromCentre");
    const float midAngle = (rotaryStartAngle + rotaryEndAngle) * 0.5f;
    const auto at = [cx, cy] (float radius, float angle) {
        return Point<float> (cx + radius * std::sin (angle), cy - radius * std::cos (angle));
    };

    // Tick ring; the centre tick of a centred knob is longer and brighter
    // A "brightTick" (a position from 0 to 1) marks a reference value, e.g. 0 dB on a level knob
    const int brightTick = slider.getProperties().contains ("brightTick")
                         ? roundToInt ((double) slider.getProperties()["brightTick"] * 10.0) : -1;
    for (int i = 0; i <= 10; ++i) {
        const float a = rotaryStartAngle + (float) i * 0.1f * (rotaryEndAngle - rotaryStartAngle);
        const bool mid = (fromCentre && i == 5) || i == brightTick;
        g.setColour (Colour (mid ? 0xff9a9a9a : 0xff5c5c5c).withMultipliedAlpha (alpha));
        g.drawLine (Line<float> (at ((mid ? 15.6f : 18.2f) * k, a), at (21.2f * k, a)), mid ? 1.4f : 1.1f);
    }

    const float arcR = 15.4f * k;
    const PathStrokeType arcStroke (jmax (1.5f, 2.6f * k), PathStrokeType::curved, PathStrokeType::rounded);
    Path track;
    track.addCentredArc (cx, cy, arcR, arcR, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (Colour (0xff333333));
    g.strokePath (track, arcStroke);

    const float fromAngle = fromCentre ? midAngle : rotaryStartAngle;
    if (std::abs (toAngle - fromAngle) > 0.01f) {
        Path value;
        value.addCentredArc (cx, cy, arcR, arcR, 0.0f, jmin (fromAngle, toAngle), jmax (fromAngle, toAngle), true);
        g.setColour (slider.findColour (Slider::thumbColourId).withMultipliedAlpha (alpha));
        g.strokePath (value, arcStroke);
    }

    const float bodyR = 11.2f * k;
    g.setColour (Colours::black.withAlpha (0.55f));
    g.fillEllipse (cx - bodyR, cy - bodyR + 0.8f, bodyR * 2.0f, bodyR * 2.0f);
    g.setGradientFill (ColourGradient (Colour (0xff626262), cx - bodyR * 0.2f, cy - bodyR * 0.36f,
                                       Colour (0xff232323), cx + bodyR, cy + bodyR, true));
    g.fillEllipse (cx - bodyR, cy - bodyR, bodyR * 2.0f, bodyR * 2.0f);
    g.setColour (Colour (0xff0d0d0d));
    g.drawEllipse (cx - bodyR, cy - bodyR, bodyR * 2.0f, bodyR * 2.0f, 1.0f);

    const float dotR = jmax (1.4f, 1.9f * k);
    const auto dot = at (7.4f * k, toAngle);
    g.setColour (Colour (0xffeafffd).withMultipliedAlpha (alpha));
    g.fillEllipse (dot.x - dotR, dot.y - dotR, dotR * 2.0f, dotR * 2.0f);

    if (labelled) {
        g.setFont (Font (9.5f * fontScale));
        g.setColour (Colour (0xff9aa5a5).withMultipliedAlpha (alpha));
        g.drawText (slider.getProperties()["knobCaption"].toString(),
                    juce::Rectangle<float> (area.getX(), area.getY(), area.getWidth(), captionH), Justification::centred, false);
        g.setFont (Font (10.0f * fontScale));
        g.setColour (Colour (0xff8f9a9a).withMultipliedAlpha (alpha));
        g.drawText (slider.getTextFromValue (slider.getValue()),
                    juce::Rectangle<float> (area.getX(), cy + d * 0.5f, area.getWidth(), valueH), Justification::centred, false);
    }
}

void SonoPanSliderLookAndFeel::drawLinearSlider (Graphics& g, int x, int y, int width, int height,
                                       float sliderPos,
                                       float minSliderPos,
                                       float maxSliderPos,
                                       const Slider::SliderStyle style, Slider& slider)
{
    if (slider.isBar())
    {
        g.setColour (slider.findColour (Slider::trackColourId));
        if (slider.getProperties().contains ("fromCentre")) {
            auto centrex = x + width*0.5f;
            auto centrey = y + height*0.5f;
            
            if (!slider.getProperties().contains ("noFill")) {
                g.fillRect (slider.isHorizontal() ? Rectangle<float> (sliderPos > centrex ? centrex : sliderPos, y + 0.5f, sliderPos > centrex ? sliderPos - centrex : centrex - sliderPos, height - 1.0f)
                            : Rectangle<float> (x + 0.5f, sliderPos < centrey ? sliderPos : centrey, width - 1.0f, sliderPos < centrey ?  centrey - sliderPos : sliderPos - centrey));
            }
            
            // draw line
            g.fillRect (slider.isHorizontal() ? Rectangle<float> (sliderPos - 1, y + 0.5f, 2, height - 1.0f)
                        : Rectangle<float> (x + 0.5f, sliderPos - 1, width - 1.0f, 2));
        }
        else {
            
            if (!slider.getProperties().contains ("noFill")) {
                g.fillRect (slider.isHorizontal() ? Rectangle<float> (static_cast<float> (x), y + 0.5f, sliderPos - x, height - 1.0f)
                            : Rectangle<float> (x + 0.5f, sliderPos, width - 1.0f, y + (height - sliderPos)));
            }
            else {
                // draw line
                g.fillRect (slider.isHorizontal() ? Rectangle<float> (sliderPos - 1, y + 0.5f, 3, height - 1.0f)
                            : Rectangle<float> (x + 0.5f, sliderPos - 1, width - 1.0f, 3));
            }
        }
    }
    else
    {
        auto isTwoVal   = (style == Slider::SliderStyle::TwoValueVertical   || style == Slider::SliderStyle::TwoValueHorizontal);
        auto isThreeVal = (style == Slider::SliderStyle::ThreeValueVertical || style == Slider::SliderStyle::ThreeValueHorizontal);
        
        auto trackWidth = jmin (7.0f, slider.isHorizontal() ? height * 0.25f : width * 0.25f);
        auto voffset = (isTwoVal || isThreeVal) ? trackWidth*0.75f : trackWidth*0.5f;
        
        Point<float> startPoint (slider.isHorizontal() ? x : x + width * 0.5f,
                                 slider.isHorizontal() ? y + height * 0.5f + voffset: height + y);
        
        Point<float> endPoint (slider.isHorizontal() ? width + x : startPoint.x,
                               slider.isHorizontal() ? startPoint.y : y);
        
        Path backgroundTrack;
        backgroundTrack.startNewSubPath (startPoint);
        backgroundTrack.lineTo (endPoint);
        g.setColour (slider.findColour (Slider::backgroundColourId));
        g.strokePath (backgroundTrack, { trackWidth, PathStrokeType::curved, PathStrokeType::rounded });
        
        Path valueTrack;
        Point<float> minPoint, maxPoint, thumbPoint;
        
        if (isTwoVal || isThreeVal)
        {
            minPoint = { slider.isHorizontal() ? minSliderPos : width * 0.5f,
                slider.isHorizontal() ? height * 0.5f + voffset : minSliderPos };
            
            if (isThreeVal)
                thumbPoint = { slider.isHorizontal() ? sliderPos : width * 0.5f,
                    slider.isHorizontal() ? height * 0.5f + voffset: sliderPos };
            
            maxPoint = { slider.isHorizontal() ? maxSliderPos : width * 0.5f,
                slider.isHorizontal() ? height * 0.5f + voffset : maxSliderPos };
        }
        else
        {
            auto kx = slider.isHorizontal() ? sliderPos : (x + width * 0.5f);
            auto ky = slider.isHorizontal() ? (y + height * 0.5f + voffset) : sliderPos;
         
            if (slider.getProperties().contains ("fromCentre")) {
                if (kx > startPoint.x + width/2) {
                    minPoint = startPoint.translated(width/2, 0.0);
                    maxPoint = { kx, ky };                    
                } else {
                    minPoint = { kx, ky };
                    maxPoint = startPoint.translated(width/2, 0.0);
                }
                
                if (abs(minPoint.x - maxPoint.x) < 0.5) {
                    // make sure they are tiny bit different
                    maxPoint.x += 0.1;
                }
            }
            else {
                minPoint = startPoint;
                maxPoint = { kx, ky };
            }
        }
        
        auto thumbWidth = getSliderThumbRadius (slider);
        
        valueTrack.startNewSubPath (minPoint);
        valueTrack.lineTo (isThreeVal ? thumbPoint : maxPoint);
        g.setColour (slider.findColour (Slider::trackColourId));
        g.strokePath (valueTrack, { trackWidth, PathStrokeType::curved, PathStrokeType::rounded });
        
        if (! isTwoVal)
        {
           // g.setColour (slider.findColour (Slider::thumbColourId));
           // g.fillEllipse (Rectangle<float> (static_cast<float> (thumbWidth), static_cast<float> (thumbWidth)).withCentre (isThreeVal ? thumbPoint : maxPoint));

            auto sr = jmin (trackWidth, (slider.isHorizontal() ? height : width) * 0.4f);
            auto pointerColour = slider.findColour (Slider::thumbColourId);
            
            auto wscale = 1.5f;

            if (slider.isHorizontal())
            {
                /*
                drawPointer (g, minSliderPos - sr,
                             height * 0.5f - trackWidth*wscale*0.5, //jmax (0.0f, y + height * 0.5f - trackWidth * 0.5f),
                             trackWidth * wscale, pointerColour, 1); // 2
                
                drawPointer (g, maxSliderPos - trackWidth,
                             height * 0.5f - trackWidth*wscale*0.5, //jmin (y + height - trackWidth * 2.0f, (float)y + height * 0.5f),
                             trackWidth * wscale, pointerColour, 3); // 4
                */
                
                //drawPointer (g, minSliderPos - sr,
                //             jmax (0.0f, y + height * 0.5f - trackWidth * wscale),
                //             trackWidth * wscale, pointerColour, 2); // 2
                
                drawPointer (g, sliderPos - trackWidth*0.5*wscale,
                             jmin (y + height - trackWidth * wscale, (float)y + height * 0.5f + voffset),
                             trackWidth * wscale, pointerColour, 4); // 4
            }
            else
            {
                //drawPointer (g, jmax (0.0f, x + width * 0.5f - trackWidth * 2.0f),
                //             minSliderPos - trackWidth,
                //             trackWidth * wscale, pointerColour, 1);
                
                drawPointer (g, jmin (x + width - trackWidth * 2.0f, x + width * 0.5f), sliderPos - sr,
                             trackWidth * wscale, pointerColour, 3);
            }
        }
        
        if (isTwoVal || isThreeVal)
        {
            auto sr = jmin (trackWidth, (slider.isHorizontal() ? height : width) * 0.4f);
            auto pointerColour = slider.findColour (Slider::thumbColourId);
            
            auto wscale = 1.5f;
            
            if (slider.isHorizontal())
            {
                /*
                drawPointer (g, minSliderPos - sr,
                             height * 0.5f - trackWidth*wscale*0.5, //jmax (0.0f, y + height * 0.5f - trackWidth * 0.5f),
                             trackWidth * wscale, pointerColour, 1); // 2
                
                drawPointer (g, maxSliderPos - trackWidth,
                             height * 0.5f - trackWidth*wscale*0.5, //jmin (y + height - trackWidth * 2.0f, (float)y + height * 0.5f),
                             trackWidth * wscale, pointerColour, 3); // 4
                */
                
                drawPointer (g, minSliderPos - sr,
                             jmax (0.0f, y + height * 0.5f  + voffset - trackWidth * wscale),
                             trackWidth * wscale, pointerColour, 2); // 2
                
                drawPointer (g, maxSliderPos - trackWidth*0.5*wscale,
                             jmin (y + height - trackWidth * wscale, (float)y + height * 0.5f + voffset),
                             trackWidth * wscale, pointerColour, 4); // 4
            }
            else
            {
                drawPointer (g, jmax (0.0f, x + width * 0.5f - trackWidth * 2.0f),
                             minSliderPos - trackWidth,
                             trackWidth * wscale, pointerColour, 1);
                
                drawPointer (g, jmin (x + width - trackWidth * 2.0f, x + width * 0.5f), maxSliderPos - sr,
                             trackWidth * wscale, pointerColour, 3);
            }
        }
    }
}


#endif

void SonoDashedBorderButtonLookAndFeel::drawButtonBackground(Graphics& g, Button& button, const Colour& backgroundColour,
                                                             bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    auto cornerSize = 6.0f;
    auto bounds = button.getLocalBounds().toFloat().reduced (0.5f, 0.5f);

    auto baseColour = backgroundColour.withMultipliedSaturation (button.hasKeyboardFocus (true) ? 1.3f : 0.9f)
        .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f);

    if (shouldDrawButtonAsDown || shouldDrawButtonAsHighlighted)
        baseColour = baseColour.contrasting (shouldDrawButtonAsDown ? 0.2f : 0.05f);

    g.setColour (baseColour);

    auto flatOnLeft   = button.isConnectedOnLeft();
    auto flatOnRight  = button.isConnectedOnRight();
    auto flatOnTop    = button.isConnectedOnTop();
    auto flatOnBottom = button.isConnectedOnBottom();

    // Compute stroke
    float strokeThickness = 1.5;
    PathStrokeType stroke(strokeThickness, PathStrokeType::JointStyle::curved);
    float dash[2] = {10.0f, 10.0f};

    Path strokePath;
    if (flatOnLeft || flatOnRight || flatOnTop || flatOnBottom)
    {
        Path path;
        path.addRoundedRectangle (bounds.getX(), bounds.getY(),
            bounds.getWidth(), bounds.getHeight(),
            cornerSize, cornerSize,
            ! (flatOnLeft  || flatOnTop),
            ! (flatOnRight || flatOnTop),
            ! (flatOnLeft  || flatOnBottom),
            ! (flatOnRight || flatOnBottom));

        g.fillPath (path);

        auto reducedBounds = bounds.reduced(strokeThickness);
        strokePath.addRoundedRectangle (reducedBounds.getX(), reducedBounds.getY(),
            reducedBounds.getWidth(), reducedBounds.getHeight(),
            cornerSize, cornerSize,
            ! (flatOnLeft  || flatOnTop),
            ! (flatOnRight || flatOnTop),
            ! (flatOnLeft  || flatOnBottom),
            ! (flatOnRight || flatOnBottom));
    }
    else
    {
        g.fillRoundedRectangle (bounds, cornerSize);

        strokePath.addRoundedRectangle(bounds.reduced(strokeThickness), cornerSize);
    }

    // Draw border
    stroke.createDashedStroke(strokePath, strokePath, dash, 2);

    g.setColour(button.findColour(ComboBox::outlineColourId));
    g.strokePath(strokePath, stroke);
}

// Setup Audio input meter — unlit = dim grey, lit = thumb colour, top block red (see header)
void SonoLookAndFeel::drawLevelMeter (Graphics& g, int width, int height, float level)
{
    const float corner = 3.0f, border = 2.0f;
    const int totalBlocks = 7;
    g.setColour (findColour (ResizableWindow::backgroundColourId));
    g.fillRoundedRectangle (0.0f, 0.0f, (float) width, (float) height, corner);
    const int numBlocks = std::isfinite (level) ? jlimit (0, totalBlocks, roundToInt ((float) totalBlocks * level)) : 0;
    const float blockW = ((float) width - 2.0f * border) / (float) totalBlocks;
    const float blockH = (float) height - 2.0f * border;
    const float rectW = 0.94f * blockW, spacing = 0.03f * blockW;
    const Colour lit = findColour (Slider::thumbColourId);
    const Colour off = Colour::fromFloatRGBA (0.28f, 0.28f, 0.28f, 1.0f);
    for (int i = 0; i < totalBlocks; ++i)
    {
        g.setColour (i >= numBlocks ? off : (i < totalBlocks - 1 ? lit : Colours::red));
        g.fillRoundedRectangle (border + (float) i * blockW + spacing, border, rectW, blockH, 0.1f * blockW);
    }
}
