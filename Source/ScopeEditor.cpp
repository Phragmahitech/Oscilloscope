#include "ScopeEditor.h"
#include "Names.h"

#include <algorithm>

namespace
{
    using osclook::drawText;

    juce::String middleDot()    { return juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")); }

    /** Petit œil (barré si la trace est masquée). */
    void drawEye (juce::Graphics& g, juce::Rectangle<float> r, bool open, juce::Colour c)
    {
        const auto centre = r.getCentre();
        juce::Path eye;
        eye.startNewSubPath (centre.x - 8.0f, centre.y);
        eye.quadraticTo (centre.x, centre.y - 6.5f, centre.x + 8.0f, centre.y);
        eye.quadraticTo (centre.x, centre.y + 6.5f, centre.x - 8.0f, centre.y);
        eye.closeSubPath();
        g.setColour (c);
        g.strokePath (eye, juce::PathStrokeType (1.1f));
        if (open)
            g.fillEllipse (juce::Rectangle<float> (4.6f, 4.6f).withCentre (centre));
        else
            g.drawLine (centre.x - 7.0f, centre.y + 6.0f, centre.x + 7.0f, centre.y - 6.0f, 1.1f);
    }

    //==========================================================================
    /** Choix de la couleur : la palette, puis un sélecteur libre. */
    class ColourPanel final : public juce::Component,
                              private juce::ChangeListener
    {
    public:
        ColourPanel (OscLook& l, osc::Slot& s, uint32_t gen)
            : look (l), slot (s), generation (gen),
              selector (juce::ColourSelector::showColourspace)
        {
            selector.setCurrentColour (juce::Colour (slot.colour.load()), juce::dontSendNotification);
            selector.addChangeListener (this);
            selector.setColour (juce::ColourSelector::backgroundColourId, juce::Colour (0xff0b0b0b));
            addAndMakeVisible (selector);
            setSize (6 * 34 + 20, 2 * 34 + 20 + 150);
        }

        ~ColourPanel() override    { selector.removeChangeListener (this); }

        void paint (juce::Graphics& g) override
        {
            int count = 0;
            const uint32_t* colours = osc::palette (count);
            const uint32_t current = slot.colour.load();
            for (int i = 0; i < count; ++i)
            {
                const auto r = swatch (i).toFloat();
                g.setColour (juce::Colour (colours[i]));
                g.fillRect (r.reduced (4.0f));
                if (colours[i] == current)
                {
                    g.setColour (OscLook::white());
                    g.drawRect (r.reduced (1.0f), 1.0f);
                }
            }
        }

        void resized() override
        {
            selector.setBounds (10, 2 * 34 + 14, getWidth() - 20, 146);
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            int count = 0;
            const uint32_t* colours = osc::palette (count);
            for (int i = 0; i < count; ++i)
                if (swatch (i).contains (e.getPosition()))
                {
                    apply (colours[i]);
                    selector.setCurrentColour (juce::Colour (colours[i]), juce::dontSendNotification);
                    repaint();
                }
        }

    private:
        juce::Rectangle<int> swatch (int i) const   { return { 10 + (i % 6) * 34, 10 + (i / 6) * 34, 34, 34 }; }

        void apply (uint32_t argb)
        {
            if (! slot.isAlive() || slot.generation.load() != generation)
                return;
            slot.colour.store (argb | 0xff000000u);
            slot.setFlag (osc::flags::manualColour, true);
        }

        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            apply (selector.getCurrentColour().withAlpha (1.0f).getARGB());
            repaint();
        }

        OscLook& look;
        osc::Slot& slot;
        uint32_t generation;
        juce::ColourSelector selector;
    };
}

//==============================================================================
TrackList::TrackList (ScopeEditor& e) : editor (e)
{
    renameField.setFont (editor.getLook().mono (11.0f, 0.08f, true));
    renameField.setIndents (6, 0);
    renameField.setJustification (juce::Justification::centredLeft);
    renameField.setInputRestrictions (40);
    renameField.setSelectAllWhenFocused (true);
    renameField.onReturnKey = [this]
    {
        editor.rename (renamingIndex, renamingGeneration, renameField.getText());
        renameField.setVisible (false);
        renamingIndex = -1;
        giveAwayKeyboardFocus();
    };
    renameField.onEscapeKey = [this]
    {
        renameField.setVisible (false);
        renamingIndex = -1;
        giveAwayKeyboardFocus();
    };
    renameField.onFocusLost = [this]
    {
        if (renamingIndex >= 0)
            renameField.onReturnKey();
    };
    addChildComponent (renameField);
}

void TrackList::setTracks (const std::vector<TrackInfo>& t)
{
    bool changed = t.size() != tracks.size();
    for (size_t i = 0; ! changed && i < t.size(); ++i)
    {
        const auto& a = t[i];
        const auto& b = tracks[i];
        changed = a.index != b.index || a.generation != b.generation || a.name != b.name || a.colour != b.colour
               || a.active != b.active || a.hidden != b.hidden || a.latency != b.latency
               || a.latencyState != b.latencyState || a.pairedName != b.pairedName;
    }

    if (changed)
    {
        tracks = t;
        repaint();
    }
}

int TrackList::rowAt (juce::Point<int> p) const
{
    for (int r = 0; r < (int) tracks.size(); ++r)
        if (rowBounds (r).contains (p))
            return r;
    return -1;
}

juce::String TrackList::statusText (const TrackInfo& t) const
{
    if (! t.active)
        return "INACTIVE";

    const auto lat = osc::latencyText (t.latency, t.sampleRate);
    switch (t.latencyState)
    {
        case osc::LatencyState::off:        return "COMPENSATION OFF";
        case osc::LatencyState::measuring:  return "SYNC FOUND" + middleDot() + "MEASURING";
        case osc::LatencyState::searching:  return t.latency > 0 ? "NO SYNC" + middleDot() + "HOLD " + lat : "NO SYNC" + middleDot() + "0 smp";
        case osc::LatencyState::noSignal:
        case osc::LatencyState::locked:
        default:                            break;
    }

    auto s = t.latency == 0 ? juce::String ("SYNC" + middleDot() + "NO LATENCY") : "COMP " + lat;
    if (t.pairedName.isNotEmpty())
        s << middleDot() << "VIA " << t.pairedName.toUpperCase();
    return s;
}

void TrackList::paint (juce::Graphics& g)
{
    auto& look = editor.getLook();
    g.fillAll (OscLook::panel());

    for (int r = 0; r < (int) tracks.size(); ++r)
    {
        const auto& t = tracks[(size_t) r];
        const auto row = rowBounds (r).toFloat();
        if (row.getY() > (float) getHeight())
            break;

        if (t.own)
        {
            g.setColour (juce::Colour (0xff121212));
            g.fillRect (row.reduced (0.0f, 1.0f));
            g.setColour (OscLook::white());
            g.fillRect (row.getX(), row.getY() + 6.0f, 2.0f, row.getHeight() - 12.0f);
        }
        else if (r == hoverRow)
        {
            g.setColour (juce::Colour (0xff0e0e0e));
            g.fillRect (row.reduced (0.0f, 1.0f));
        }

        // Pastille de couleur
        const auto sw = swatchBounds (r).toFloat();
        g.setColour (t.colour.withMultipliedAlpha (t.hidden ? 0.3f : 1.0f));
        g.fillRect (sw);
        g.setColour (OscLook::outline());
        g.drawRect (sw.expanded (2.0f), 1.0f);

        // Nom
        const auto nameColour = ! t.active || t.hidden ? OscLook::dim() : OscLook::ink();
        auto nameArea = nameBounds (r).toFloat();
        if (t.own)
        {
            const auto tag = look.mono (8.0f, 0.2f);
            const float tw = osclook::textWidth (tag, "THIS") + 10.0f;
            const auto tagArea = nameArea.removeFromRight (tw).withSizeKeepingCentre (tw, 13.0f);
            g.setColour (OscLook::outline());
            g.drawRect (tagArea, 1.0f);
            drawText (g, "THIS", tag, OscLook::label(), tagArea, juce::Justification::centred);
            nameArea.removeFromRight (6.0f);
        }
        drawText (g, t.name.toUpperCase(), look.mono (11.0f, 0.08f, true), nameColour, nameArea, juce::Justification::centredLeft);

        // Latence
        const bool warn = t.active && t.latencyState == osc::LatencyState::searching;
        drawText (g, statusText (t), look.mono (8.5f, 0.1f), warn ? OscLook::dim() : OscLook::label(),
                  statusBounds (r).toFloat(), juce::Justification::centredLeft);

        // Œil
        drawEye (g, eyeBounds (r).toFloat(), ! t.hidden, t.hidden ? OscLook::faint() : OscLook::label());

        g.setColour (OscLook::line());
        g.fillRect (row.getX() + 12.0f, row.getBottom() - 0.5f, row.getWidth() - 24.0f, 1.0f);
    }

    // Aide en bas : une trace sans Sync
    bool missing = false;
    for (auto& t : tracks)
        missing = missing || (t.active && t.latencyState == osc::LatencyState::searching);

    const float w = (float) getWidth();
    const auto small = look.mono (8.5f, 0.1f);
    if (missing)
    {
        drawText (g, "AUTO LATENCY: PUT OSCILLOSCOPE SYNC", small, OscLook::dim(), { 14.0f, (float) getHeight() - 44.0f, w - 28.0f, 14.0f }, juce::Justification::centredLeft);
        drawText (g, "FIRST ON THE TRACK.", small, OscLook::dim(), { 14.0f, (float) getHeight() - 30.0f, w - 28.0f, 14.0f }, juce::Justification::centredLeft);
    }
    else if (tracks.size() == 1)
    {
        drawText (g, "ADD OSCILLOSCOPE ON OTHER TRACKS", small, OscLook::faint(), { 14.0f, (float) getHeight() - 44.0f, w - 28.0f, 14.0f }, juce::Justification::centredLeft);
        drawText (g, "TO SEE THEM HERE.", small, OscLook::faint(), { 14.0f, (float) getHeight() - 30.0f, w - 28.0f, 14.0f }, juce::Justification::centredLeft);
    }
}

void TrackList::resized() {}

void TrackList::mouseMove (const juce::MouseEvent& e)
{
    const int r = rowAt (e.getPosition());
    if (r != hoverRow)
    {
        hoverRow = r;
        repaint();
    }

    juce::String tip;
    if (r >= 0)
    {
        if (swatchBounds (r).expanded (4).contains (e.getPosition()))       tip = "Colour";
        else if (eyeBounds (r).contains (e.getPosition()))                  tip = "Show / hide this trace";
        else if (statusBounds (r).contains (e.getPosition()))               tip = "Latency compensation (click for options)";
        else if (nameBounds (r).contains (e.getPosition()))                 tip = "Double-click to rename";
    }
    setTooltip (tip);
    setMouseCursor (r >= 0 && tip.isNotEmpty() ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void TrackList::mouseExit (const juce::MouseEvent&)
{
    hoverRow = -1;
    repaint();
}

void TrackList::mouseUp (const juce::MouseEvent& e)
{
    const int r = rowAt (e.getPosition());
    if (r < 0 || e.mouseWasDraggedSinceMouseDown())
        return;

    if (swatchBounds (r).expanded (4).contains (e.getPosition()))
        editor.chooseColour (r);
    else if (eyeBounds (r).contains (e.getPosition()))
        editor.toggleHidden (r);
    else if (statusBounds (r).contains (e.getPosition()))
        editor.showLatencyMenu (r);
}

void TrackList::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int r = rowAt (e.getPosition());
    if (r >= 0 && nameBounds (r).contains (e.getPosition()))
        startRename (r);
}

void TrackList::startRename (int row)
{
    if (row < 0 || row >= (int) tracks.size())
        return;
    renamingIndex = tracks[(size_t) row].index;
    renamingGeneration = tracks[(size_t) row].generation;
    renameField.setBounds (nameBounds (row).withTrimmedRight (-30));
    renameField.setText (tracks[(size_t) row].name, juce::dontSendNotification);
    renameField.setVisible (true);
    if (renameField.isShowing())
        renameField.grabKeyboardFocus();
    renameField.selectAll();
}

//==============================================================================
ScopeView::ScopeView (ScopeEditor& e) : editor (e)
{
    setOpaque (true);
}

void ScopeView::refresh (const std::vector<TrackInfo>& tracks)
{
    auto& p = editor.getProcessor();
    const bool frozen = p.isFrozen();

    if (frozen && windowValid)
    {
        if (! frozenShown)
        {
            frozenShown = true;
            repaint();
        }
        return;
    }
    frozenShown = false;

    auto* own = p.getSlot();
    auto* hub = p.getHub();
    if (own == nullptr || hub == nullptr)
    {
        traces.clear();
        windowValid = false;
        repaint();
        return;
    }

    beats = ScopeProcessor::timeInBeats (p.getTimeIndex());
    view = p.getView();
    const auto channel = p.getChannel();
    const float gain = p.getGainLinear();

    std::vector<std::pair<const TrackInfo*, osc::TraceSource>> sources;
    for (auto& t : tracks)
        if (t.active && ! t.hidden)
            sources.push_back ({ &t, osc::TraceSource { &hub->slots[t.index], t.latency, channel } });

    if (sources.empty())
    {
        traces.clear();
        windowValid = false;
        repaint();
        return;
    }

    int64_t now = sources.front().second.latestTime();
    for (auto& s : sources)
        now = std::min (now, s.second.latestTime());

    window = osc::Window::compute (*own, beats, now);
    if (window.length > osc::ringSize / 2)   // très long à très haute fréquence : on garde ce que l'anneau contient
    {
        window.length = osc::ringSize / 2;
        window.start = now - window.length / 2;
    }
    windowValid = true;

    const int columns = juce::jmax (1, getWidth());
    std::vector<Trace> next;

    if (view == ScopeProcessor::sum)
    {
        Trace t;
        t.name = "SUM";
        t.own = true;
        t.colour = juce::Colour (own->colour.load());
        t.columns.resize ((size_t) columns);
        std::vector<osc::TraceSource> all;
        for (auto& s : sources)
            all.push_back (s.second);
        osc::renderColumns (all, window, t.columns, gain);
        next.push_back (std::move (t));
    }
    else
    {
        for (auto& s : sources)
        {
            Trace t;
            t.name = s.first->name;
            t.colour = s.first->colour;
            t.own = s.first->own;
            t.columns.resize ((size_t) columns);
            osc::renderColumns ({ s.second }, window, t.columns, gain);
            next.push_back (std::move (t));
        }

        // Superposé : la trace de cette fenêtre par-dessus les autres (séparé : ordre de la liste)
        if (view == ScopeProcessor::overlay)
            std::stable_partition (next.begin(), next.end(), [] (const Trace& t) { return ! t.own; });
    }

    traces = std::move (next);
    repaint();
}

void ScopeView::drawGrid (juce::Graphics& g, juce::Rectangle<float> area, bool centreOnly) const
{
    const float cy = area.getCentreY(), h = area.getHeight() * 0.5f;

    g.setColour (juce::Colour (0xff1c1c1c));
    g.fillRect (area.getX(), cy - 0.5f, area.getWidth(), 1.0f);

    if (centreOnly)
        return;

    g.setColour (juce::Colour (0xff121212));
    for (float level : { 0.5f, -0.5f })
        g.fillRect (area.getX(), cy - level * h - 0.5f, area.getWidth(), 1.0f);
}

void ScopeView::drawTrace (juce::Graphics& g, const Trace& t, juce::Rectangle<float> lane) const
{
    const int n = (int) t.columns.size();
    if (n == 0)
        return;

    const float x0 = lane.getX(), cy = lane.getCentreY(), half = lane.getHeight() * 0.5f * 0.92f;
    const float colWidth = lane.getWidth() / (float) n;
    auto toY = [&] (float v) { return cy - juce::jlimit (-1.08f, 1.08f, v) * half; };

    // Ombre sous la courbe
    g.setColour (t.colour.withAlpha (0.10f));
    for (int i = 0; i < n; ++i)
    {
        const auto& c = t.columns[(size_t) i];
        if (! c.valid)
            continue;
        const float y = toY (0.5f * (c.lo + c.hi));
        g.fillRect (x0 + (float) i * colWidth, juce::jmin (y, cy), colWidth, std::abs (y - cy));
    }

    // La courbe : chaque colonne relie aussi la précédente pour rester continue
    g.setColour (t.colour);
    bool havePrev = false;
    float prevLo = 0.0f, prevHi = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const auto& c = t.columns[(size_t) i];
        if (! c.valid)
        {
            havePrev = false;
            continue;
        }

        float lo = c.lo, hi = c.hi;
        if (havePrev)
        {
            lo = juce::jmin (lo, prevHi);
            hi = juce::jmax (hi, prevLo);
        }
        const float yTop = toY (hi) - 0.7f, yBottom = toY (lo) + 0.7f;
        g.fillRect (x0 + (float) i * colWidth, yTop, juce::jmax (1.0f, colWidth), juce::jmax (1.4f, yBottom - yTop));

        prevLo = c.lo;
        prevHi = c.hi;
        havePrev = true;
    }
}

void ScopeView::paint (juce::Graphics& g)
{
    auto& look = editor.getLook();
    const auto area = getLocalBounds().toFloat();
    g.fillAll (OscLook::ground());

    // Grille du temps : 4 grandes divisions, 16 petites
    g.setColour (juce::Colour (0xff0f0f0f));
    for (int i = 1; i < 16; ++i)
        if (i % 4 != 0)
            g.fillRect (area.getX() + area.getWidth() * (float) i / 16.0f - 0.5f, area.getY(), 1.0f, area.getHeight());
    g.setColour (juce::Colour (0xff1a1a1a));
    for (int i = 1; i < 4; ++i)
        g.fillRect (area.getX() + area.getWidth() * (float) i / 4.0f - 0.5f, area.getY(), 1.0f, area.getHeight());

    if (! windowValid || traces.empty())
    {
        drawGrid (g, area, false);
        drawText (g, "WAITING FOR AUDIO", look.mono (10.0f, 0.24f), OscLook::faint(), area, juce::Justification::centred);
        return;
    }

    if (view == ScopeProcessor::split && traces.size() > 1)
    {
        const float laneHeight = area.getHeight() / (float) traces.size();
        for (size_t i = 0; i < traces.size(); ++i)
        {
            const auto lane = juce::Rectangle<float> (area.getX(), area.getY() + laneHeight * (float) i, area.getWidth(), laneHeight);
            drawGrid (g, lane, true);
            if (i > 0)
            {
                g.setColour (juce::Colour (0xff1f1f1f));
                g.fillRect (lane.getX(), lane.getY() - 0.5f, lane.getWidth(), 1.0f);
            }
            drawTrace (g, traces[i], lane);
            drawText (g, traces[i].name.toUpperCase(), look.mono (8.5f, 0.14f), traces[i].colour.withAlpha (0.85f),
                      lane.reduced (10.0f, 6.0f).removeFromTop (12.0f), juce::Justification::centredLeft);
        }
    }
    else
    {
        drawGrid (g, area, false);
        for (auto& t : traces)
            drawTrace (g, t, area);
    }

    // Balayage : le présent, et un fondu juste après (ce qui suit est le passage précédent)
    if (window.now > window.start && window.now < window.start + window.length)
    {
        const float x = area.getX() + area.getWidth() * (float) (window.now - window.start) / (float) window.length;
        const float fadeWidth = juce::jmin (70.0f, area.getRight() - x);
        if (fadeWidth > 1.0f)
        {
            juce::ColourGradient fade (OscLook::ground().withAlpha (0.85f), x, 0.0f, OscLook::ground().withAlpha (0.0f), x + fadeWidth, 0.0f, false);
            g.setGradientFill (fade);
            g.fillRect (x, area.getY(), fadeWidth, area.getHeight());
        }
        g.setColour (juce::Colour (0xff6a6a6a));
        g.fillRect (x - 0.5f, area.getY(), 1.0f, area.getHeight());
    }

    if (editor.getProcessor().isFrozen())
        drawText (g, "FROZEN", look.mono (9.0f, 0.24f), OscLook::white(), area.reduced (12.0f, 8.0f), juce::Justification::topRight);
}

//==============================================================================
ScopeEditor::ScopeEditor (ScopeProcessor& p)
    : AudioProcessorEditor (&p), processor (p), trackList (*this), scopeView (*this)
{
    setLookAndFeel (&look);
    addAndMakeVisible (trackList);
    addAndMakeVisible (scopeView);

    timePrev.setButtonText ("<");
    timeNext.setButtonText (">");
    timePrev.onClick = [this] { setChoice (ScopeProcessor::timeId, processor.getTimeIndex() - 1); };
    timeNext.onClick = [this] { setChoice (ScopeProcessor::timeId, processor.getTimeIndex() + 1); };
    timeValue.onClick = [this]
    {
        juce::Component::SafePointer<ScopeEditor> safePointer (this);   // (hors de la lambda interne : MSVC)
        juce::PopupMenu m;
        const auto& names = ScopeProcessor::timeNames();
        for (int i = 0; i < names.size(); ++i)
            m.addItem (i + 1, names[i], true, i == processor.getTimeIndex());
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (timeValue),
                         [safe = safePointer] (int r)
                         {
                             if (safe != nullptr && r > 0)
                                 safe->setChoice (ScopeProcessor::timeId, r - 1);
                         });
    };

    const char* viewNames[] = { "OVERLAY", "SPLIT", "SUM" };
    for (int i = 0; i < 3; ++i)
    {
        viewButtons[i].setButtonText (viewNames[i]);
        viewButtons[i].onClick = [this, i] { setChoice (ScopeProcessor::viewId, i); };
    }

    const char* channelNames[] = { "MID", "L", "R", "SIDE" };
    for (int i = 0; i < 4; ++i)
    {
        channelButtons[i].setButtonText (channelNames[i]);
        channelButtons[i].onClick = [this, i] { setChoice (ScopeProcessor::channelId, i); };
    }

    freezeButton.setClickingTogglesState (true);
    freezeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (processor.apvts, ScopeProcessor::freezeId, freezeButton);

    gainSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    gainSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    gainSlider.setDoubleClickReturnValue (true, 0.0);
    gainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.apvts, ScopeProcessor::gainId, gainSlider);
    gainSlider.onValueChange = [this] { repaint (0, getHeight() - footerHeight, getWidth(), footerHeight); };

    for (auto* b : { &timePrev, &timeValue, &timeNext, &freezeButton })
        addAndMakeVisible (*b);
    for (auto& b : viewButtons)     addAndMakeVisible (b);
    for (auto& b : channelButtons)  addAndMakeVisible (b);
    addAndMakeVisible (gainSlider);

    for (juce::Component* c : { (juce::Component*) &timePrev, (juce::Component*) &timeValue, (juce::Component*) &timeNext,
                                (juce::Component*) &freezeButton, (juce::Component*) &gainSlider })
        c->setMouseCursor (juce::MouseCursor::PointingHandCursor);
    for (auto& b : viewButtons)     b.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    for (auto& b : channelButtons)  b.setMouseCursor (juce::MouseCursor::PointingHandCursor);

    tooltips = std::make_unique<juce::TooltipWindow> (this, 600);
    tooltips->setLookAndFeel (&look);

    setResizable (true, true);
    setResizeLimits (minWidth, minHeight, maxWidth, maxHeight);
    const int w = juce::jlimit (minWidth, maxWidth, (int) processor.apvts.state.getProperty ("editorWidth", defaultWidth));
    const int h = juce::jlimit (minHeight, maxHeight, (int) processor.apvts.state.getProperty ("editorHeight", defaultHeight));
    setSize (w, h);
    sized = true;

    refreshControls();
    tick();
    startTimerHz (60);
}

ScopeEditor::~ScopeEditor()
{
    stopTimer();
    if (tooltips != nullptr)
        tooltips->setLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

void ScopeEditor::setChoice (const char* id, int index)
{
    if (auto* param = dynamic_cast<juce::AudioParameterChoice*> (processor.apvts.getParameter (id)))
    {
        index = juce::jlimit (0, param->choices.size() - 1, index);
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 ((float) index));
        param->endChangeGesture();
    }
    refreshControls();
}

void ScopeEditor::refreshControls()
{
    timeValue.setButtonText (ScopeProcessor::timeNames()[processor.getTimeIndex()]);
    timePrev.setEnabled (processor.getTimeIndex() > 0);
    timeNext.setEnabled (processor.getTimeIndex() < ScopeProcessor::timeNames().size() - 1);
    for (int i = 0; i < 3; ++i)
        viewButtons[i].setToggleState (processor.getView() == i, juce::dontSendNotification);
    for (int i = 0; i < 4; ++i)
        channelButtons[i].setToggleState ((int) processor.getChannel() == i, juce::dontSendNotification);
}

std::vector<TrackInfo> ScopeEditor::readTracks() const
{
    std::vector<TrackInfo> out;
    auto* hub = processor.getHub();
    if (hub == nullptr)
        return out;

    const int64_t now = osc::nowMs();
    for (int i = 0; i < osc::maxSlots; ++i)
    {
        const auto& s = hub->slots[i];
        if (! s.isAlive() || s.getKind() != osc::Kind::scope)
            continue;

        TrackInfo t;
        t.index = i;
        t.generation = s.generation.load();
        t.name = osc::displayName (*hub, i);
        t.colour = juce::Colour (s.colour.load());
        t.own = i == processor.getSlotIndex();
        t.active = s.isActive (now) && s.sampleRate.load() > 0.0;
        t.hidden = s.hasFlag (osc::flags::hidden);
        t.latency = s.latency.load();
        t.latencyState = (osc::LatencyState) s.latencyState.load();
        t.sampleRate = s.sampleRate.load();

        const int paired = s.pairedSlot.load();
        if (paired >= 0 && paired < osc::maxSlots)
        {
            const auto& sync = hub->slots[paired];
            char syncTrack[osc::nameWords * 8], ownTrack[osc::nameWords * 8];
            osc::Slot::readText (sync.trackName, syncTrack);
            osc::Slot::readText (s.trackName, ownTrack);
            if (sync.isAlive() && sync.generation.load() == s.pairedGeneration.load()
                && syncTrack[0] != 0 && std::strcmp (syncTrack, ownTrack) != 0)
                t.pairedName = juce::String::fromUTF8 (syncTrack);
        }
        out.push_back (t);
    }

    std::sort (out.begin(), out.end(), [hub] (const TrackInfo& a, const TrackInfo& b)
    {
        return hub->slots[a.index].order.load() < hub->slots[b.index].order.load();
    });
    return out;
}

void ScopeEditor::tick()
{
    const auto tracks = readTracks();
    scopeView.refresh (tracks);

    refreshControls();   // (une automation de l'hôte peut changer les réglages)
    if (++frameCount % 6 == 0 || frameCount == 1)
        trackList.setTracks (tracks);
}

osc::Slot* ScopeEditor::slotFor (int row, uint32_t* generation) const
{
    const auto& tracks = trackList.getTracks();
    auto* hub = processor.getHub();
    if (hub == nullptr || row < 0 || row >= (int) tracks.size())
        return nullptr;
    auto& t = tracks[(size_t) row];
    auto& s = hub->slots[t.index];
    if (! s.isAlive() || s.generation.load() != t.generation)
        return nullptr;
    if (generation != nullptr)
        *generation = t.generation;
    return &s;
}

void ScopeEditor::chooseColour (int row)
{
    uint32_t generation = 0;
    if (auto* s = slotFor (row, &generation))
    {
        auto panel = std::make_unique<ColourPanel> (look, *s, generation);
        auto& box = juce::CallOutBox::launchAsynchronously (std::move (panel),
                                                            trackList.swatchBounds (row).translated (trackList.getX(), trackList.getY()),
                                                            this);
        box.setLookAndFeel (&look);
    }
}

void ScopeEditor::toggleHidden (int row)
{
    if (auto* s = slotFor (row))
        s->setFlag (osc::flags::hidden, ! s->hasFlag (osc::flags::hidden));
    tick();
    trackList.setTracks (readTracks());
}

void ScopeEditor::showLatencyMenu (int row)
{
    uint32_t generation = 0;
    auto* s = slotFor (row, &generation);
    if (s == nullptr)
        return;

    const bool off = s->hasFlag (osc::flags::latencyOff);
    juce::PopupMenu m;
    m.addSectionHeader ("LATENCY COMPENSATION");
    m.addItem (1, "AUTO (OSCILLOSCOPE SYNC)", true, ! off);
    m.addItem (2, "OFF", true, off);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (trackList).withTargetScreenArea (
                         trackList.localAreaToGlobal (trackList.statusBounds (row))),
                     [s, generation] (int r)
                     {
                         if (r > 0 && s->isAlive() && s->generation.load() == generation)
                             s->setFlag (osc::flags::latencyOff, r == 2);
                     });
}

void ScopeEditor::rename (int index, uint32_t generation, const juce::String& name)
{
    auto* hub = processor.getHub();
    if (hub == nullptr || index < 0 || index >= osc::maxSlots)
        return;
    auto& s = hub->slots[index];
    if (! s.isAlive() || s.generation.load() != generation)
        return;
    osc::Slot::writeText (s.name, name.trim().substring (0, 40).toRawUTF8());
    trackList.setTracks (readTracks());
}

//==============================================================================
void ScopeEditor::paint (juce::Graphics& g)
{
    g.fillAll (OscLook::ground());
    const float w = (float) getWidth(), h = (float) getHeight();

    // En-tête
    drawText (g, "OSCILLOSCOPE", look.title (14.0f, 0.22f), OscLook::ink(), { 20.0f, 0.0f, 300.0f, (float) headerHeight }, juce::Justification::centredLeft);
    const float titleWidth = osclook::textWidth (look.title (14.0f, 0.22f), "OSCILLOSCOPE");
    drawText (g, "BY PHRAGMA", look.mono (9.0f, 0.24f), OscLook::dim(), { 20.0f + titleWidth + 18.0f, 0.0f, 200.0f, (float) headerHeight }, juce::Justification::centredLeft);
    drawText (g, juce::String ("V") + OSC_VERSION, look.mono (9.0f, 0.2f), OscLook::faint(), { w - 200.0f, 0.0f, 80.0f, (float) headerHeight }, juce::Justification::centredRight);

    g.setColour (OscLook::line());
    g.fillRect (0.0f, (float) headerHeight - 1.0f, w, 1.0f);
    g.fillRect (0.0f, h - (float) footerHeight, w, 1.0f);
    g.fillRect ((float) listWidth, (float) headerHeight, 1.0f, h - (float) headerHeight - (float) footerHeight);

    // Pied
    const auto label = look.mono (8.5f, 0.24f);
    const float fy = h - (float) footerHeight;
    drawText (g, "TIME", label, OscLook::label(), { (float) timePrev.getX(), fy + 6.0f, 100.0f, 12.0f }, juce::Justification::centredLeft);
    drawText (g, "VIEW", label, OscLook::label(), { (float) viewButtons[0].getX(), fy + 6.0f, 100.0f, 12.0f }, juce::Justification::centredLeft);
    drawText (g, "CHANNEL", label, OscLook::label(), { (float) channelButtons[0].getX(), fy + 6.0f, 100.0f, 12.0f }, juce::Justification::centredLeft);
    drawText (g, "GAIN", label, OscLook::label(), { (float) gainSlider.getX(), fy + 6.0f, 100.0f, 12.0f }, juce::Justification::centredLeft);

    const float gainDb = (float) gainSlider.getValue();
    drawText (g, (gainDb > 0.05f ? "+" : "") + juce::String (gainDb, 1) + " dB", look.mono (10.0f, 0.08f), OscLook::ink(),
              { (float) gainSlider.getRight() + 8.0f, (float) gainSlider.getY(), 70.0f, (float) gainSlider.getHeight() }, juce::Justification::centredLeft);
}

void ScopeEditor::resized()
{
    const int w = getWidth(), h = getHeight();
    trackList.setBounds (0, headerHeight, listWidth, h - headerHeight - footerHeight);
    scopeView.setBounds (listWidth + 1, headerHeight, w - listWidth - 1, h - headerHeight - footerHeight);

    freezeButton.setBounds (w - 104, (headerHeight - 26) / 2, 84, 26);

    const int y = h - footerHeight + 22, bh = 26;
    int x = 20;
    timePrev.setBounds (x, y, 26, bh);
    timeValue.setBounds (x + 25, y, 62, bh);
    timeNext.setBounds (x + 86, y, 26, bh);
    x += 112 + 34;

    for (int i = 0; i < 3; ++i)
        viewButtons[i].setBounds (x + i * 75, y, 76, bh);
    x += 3 * 75 + 1 + 34;

    const int channelWidths[] = { 48, 34, 34, 52 };
    int cx = x;
    for (int i = 0; i < 4; ++i)
    {
        channelButtons[i].setBounds (cx, y, channelWidths[i] + 1, bh);
        cx += channelWidths[i];
    }
    x = cx + 1 + 34;

    gainSlider.setBounds (x, y, juce::jmin (220, juce::jmax (100, w - x - 100)), bh);

    if (sized)
    {
        processor.apvts.state.setProperty ("editorWidth", w, nullptr);
        processor.apvts.state.setProperty ("editorHeight", h, nullptr);
    }
}
