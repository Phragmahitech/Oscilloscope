#include "Look.h"
#include "BinaryData.h"

OscLook::OscLook()
{
    michroma    = juce::Typeface::createSystemTypefaceFor (BinaryData::MichromaRegular_ttf, BinaryData::MichromaRegular_ttfSize);
    plexRegular = juce::Typeface::createSystemTypefaceFor (BinaryData::IBMPlexMonoRegular_ttf, BinaryData::IBMPlexMonoRegular_ttfSize);
    plexMedium  = juce::Typeface::createSystemTypefaceFor (BinaryData::IBMPlexMonoMedium_ttf, BinaryData::IBMPlexMonoMedium_ttfSize);

    setColour (juce::ResizableWindow::backgroundColourId, ground());
    setColour (juce::TextButton::buttonColourId, ground());
    setColour (juce::TextButton::textColourOffId, ink());
    setColour (juce::TextEditor::textColourId, ink());
    setColour (juce::TextEditor::highlightColourId, juce::Colour (0xff4a4a4a));
    setColour (juce::TextEditor::highlightedTextColourId, juce::Colours::white);
    setColour (juce::CaretComponent::caretColourId, juce::Colours::white);
    setColour (juce::ColourSelector::backgroundColourId, panel());
}

juce::Font OscLook::title (float height, float tracking) const
{
    return juce::Font (juce::FontOptions (michroma).withPointHeight (height)).withExtraKerningFactor (tracking);
}

juce::Font OscLook::mono (float height, float tracking, bool medium) const
{
    return juce::Font (juce::FontOptions (medium ? plexMedium : plexRegular).withPointHeight (height)).withExtraKerningFactor (tracking);
}

void OscLook::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h,
                                float sliderPos, float, float, juce::Slider::SliderStyle, juce::Slider& slider)
{
    const float cy = (float) y + (float) h * 0.5f;
    g.setColour (outline());
    g.fillRect ((float) x, cy - 0.5f, (float) w, 1.0f);

    // Repère du zéro (valeur par défaut) quand il est dans la course
    if (slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0)
    {
        const float zx = (float) slider.getPositionOfValue (0.0);
        g.setColour (faint());
        g.fillRect (zx - 0.5f, cy - 4.0f, 1.0f, 8.0f);
    }

    const bool active = slider.isMouseOverOrDragging();
    g.setColour (active ? juce::Colours::white : white());
    g.fillRect (juce::Rectangle<float> (7.0f, 15.0f).withCentre ({ sliderPos, cy }));
}

void OscLook::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                    bool highlighted, bool)
{
    const auto area = button.getLocalBounds().toFloat();
    const bool on = button.getToggleState();

    if (button.getComponentID() == "bare")
        return;

    if (on)
    {
        g.setColour (white());
        g.fillRect (area);
    }

    g.setColour (on || highlighted ? white() : outline());
    g.drawRect (area, 1.0f);
}

void OscLook::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool highlighted, bool)
{
    const bool on = button.getToggleState();
    g.setColour (on ? ground() : (highlighted ? juce::Colours::white : ink()));
    g.setFont (mono (10.0f, 0.18f));
    g.drawText (button.getButtonText(), button.getLocalBounds(), juce::Justification::centred, false);
}

juce::Font OscLook::getPopupMenuFont()
{
    return mono (12.0f, 0.06f);
}

void OscLook::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    g.fillAll (juce::Colour (0xff0b0b0b));
    g.setColour (outline());
    g.drawRect (0, 0, w, h, 1);
}

void OscLook::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                                 bool isHighlighted, bool isTicked, bool, const juce::String& text,
                                 const juce::String&, const juce::Drawable*, const juce::Colour* textColour)
{
    if (isSeparator)
    {
        g.setColour (line().brighter (0.3f));
        g.fillRect (area.reduced (10, 0).withHeight (1).withY (area.getCentreY()));
        return;
    }

    auto r = area.reduced (1, 0);
    if (isHighlighted && isActive)
    {
        g.setColour (white());
        g.fillRect (r);
    }

    const auto colour = ! isActive ? juce::Colour (0xff4a4a4a) : isHighlighted ? ground() : ink();

    if (isTicked)
    {
        g.setColour (colour);
        g.fillRect (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ (float) r.getX() + 13.0f, (float) r.getCentreY() }));
    }

    // Pastille de couleur (menu des couleurs)
    if (textColour != nullptr)
    {
        g.setColour (*textColour);
        g.fillRect (juce::Rectangle<float> (10.0f, 10.0f).withCentre ({ (float) r.getRight() - 18.0f, (float) r.getCentreY() }));
    }

    g.setColour (colour);
    g.setFont (getPopupMenuFont());
    g.drawText (text, r.withTrimmedLeft (26).withTrimmedRight (30), juce::Justification::centredLeft, true);
}

void OscLook::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName)
{
    g.setColour (dim());
    g.setFont (mono (9.0f, 0.2f));
    g.drawText (sectionName, area.withTrimmedLeft (12).withTrimmedTop (4), juce::Justification::centredLeft, false);
}

void OscLook::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int, int& idealWidth, int& idealHeight)
{
    if (isSeparator)
    {
        idealWidth = 60;
        idealHeight = 9;
        return;
    }

    idealHeight = 24;
    idealWidth = juce::jmax (180, (int) std::ceil (osclook::textWidth (getPopupMenuFont(), text)) + 66);
}

void OscLook::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor&)
{
    g.setColour (juce::Colour (0xff0d0d0d));
    g.fillRect (0, 0, w, h);
}

void OscLook::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& editor)
{
    g.setColour (editor.hasKeyboardFocus (true) ? white() : outline());
    g.drawRect (0, 0, w, h, 1);
}

void OscLook::drawCallOutBoxBackground (juce::CallOutBox&, juce::Graphics& g, const juce::Path& path, juce::Image&)
{
    g.setColour (juce::Colour (0xff0b0b0b));
    g.fillPath (path);
    g.setColour (outline());
    g.strokePath (path, juce::PathStrokeType (1.0f));
}

void OscLook::drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
{
    g.setColour (isMouseOver || isMouseDragging ? white() : faint());
    for (int i = 1; i <= 3; ++i)
    {
        const float d = (float) i * 4.0f;
        g.drawLine ((float) w - d, (float) h - 1.0f, (float) w - 1.0f, (float) h - d, 1.0f);
    }
}

namespace osclook
{
    void drawText (juce::Graphics& g, const juce::String& text, const juce::Font& font, juce::Colour colour,
                   juce::Rectangle<float> area, juce::Justification just)
    {
        g.setColour (colour);
        g.setFont (font);
        g.drawText (text, area, just, true);
    }

    float textWidth (const juce::Font& font, const juce::String& text)
    {
        return juce::GlyphArrangement::getStringWidth (font, text);
    }
}
