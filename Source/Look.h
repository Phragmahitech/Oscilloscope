#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/**
    Look noir et blanc des plugins Phragma (comme HyperHats) : fond presque noir, traits fins gris, texte blanc.
    La couleur est réservée aux ondes. Polices embarquées : Michroma (titres) et IBM Plex Mono, licence OFL.
*/
class OscLook final : public juce::LookAndFeel_V4
{
public:
    OscLook();

    static juce::Colour ground()      { return juce::Colour (0xff050505); }
    static juce::Colour panel()       { return juce::Colour (0xff0a0a0a); }
    static juce::Colour ink()         { return juce::Colour (0xffefefef); }
    static juce::Colour white()       { return juce::Colour (0xfff2f2f2); }
    static juce::Colour label()       { return juce::Colour (0xff8c8c8c); }
    static juce::Colour dim()         { return juce::Colour (0xff6f6f6f); }
    static juce::Colour faint()       { return juce::Colour (0xff4f4f4f); }
    static juce::Colour line()        { return juce::Colour (0xff1f1f1f); }
    static juce::Colour outline()     { return juce::Colour (0xff3a3a3a); }

    /** Titres (Michroma). tracking = espacement des lettres en fraction de la hauteur. */
    juce::Font title (float height, float tracking) const;
    /** Étiquettes et valeurs (IBM Plex Mono). */
    juce::Font mono (float height, float tracking, bool medium = false) const;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&,
                         bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    juce::Font getPopupMenuFont() override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText, const juce::Drawable* icon, const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>& area, const juce::String& sectionName) override;
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                    int& idealWidth, int& idealHeight) override;

    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;

    void drawCallOutBoxBackground (juce::CallOutBox&, juce::Graphics&, const juce::Path&, juce::Image&) override;
    void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;

private:
    juce::Typeface::Ptr michroma, plexRegular, plexMedium;
};

namespace osclook
{
    void drawText (juce::Graphics&, const juce::String& text, const juce::Font&, juce::Colour,
                   juce::Rectangle<float> area, juce::Justification);
    float textWidth (const juce::Font&, const juce::String&);
}
