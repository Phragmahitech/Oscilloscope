#pragma once

#include "ScopeProcessor.h"
#include "Look.h"
#include "Trace.h"

class ScopeEditor;

//==============================================================================
/** Ce que l'interface sait d'une instance, relu dans le hub à chaque image. */
struct TrackInfo
{
    int index = -1;
    uint32_t generation = 0;
    juce::String name;
    juce::Colour colour;
    bool own = false;
    bool active = false;
    bool hidden = false;
    int latency = 0;
    int sourceLatency = 0;       // part venant de l'instrument (Oscilloscope Sync Instrument)
    osc::LatencyState latencyState = osc::LatencyState::searching;
    juce::String pairedName;     // nom du Sync suivi, s'il est sur une autre piste (bus, groupe)
    double sampleRate = 0.0;
};

/** Liste des Oscilloscope du projet : couleur, nom, visible ou non, latence compensée. */
class TrackList final : public juce::Component,
                        public juce::SettableTooltipClient
{
public:
    explicit TrackList (ScopeEditor&);

    void setTracks (const std::vector<TrackInfo>&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    static constexpr int rowHeight = 50;

    /** Zones cliquables d'une ligne (coordonnées de la liste). */
    juce::Rectangle<int> rowBounds (int row) const      { return { 0, 8 + row * rowHeight, getWidth(), rowHeight }; }
    juce::Rectangle<int> swatchBounds (int row) const   { return { 14, rowBounds (row).getY() + 9, 16, 16 }; }
    juce::Rectangle<int> eyeBounds (int row) const      { return { getWidth() - 40, rowBounds (row).getY() + 6, 28, 22 }; }
    juce::Rectangle<int> nameBounds (int row) const     { return { 40, rowBounds (row).getY() + 6, getWidth() - 86, 22 }; }
    juce::Rectangle<int> statusBounds (int row) const   { return { 40, rowBounds (row).getY() + 27, getWidth() - 54, 16 }; }

    int rowAt (juce::Point<int> p) const;
    const std::vector<TrackInfo>& getTracks() const     { return tracks; }

    /** Ouvre le champ pour renommer la ligne. */
    void startRename (int row);

private:
    juce::String statusText (const TrackInfo&) const;

    ScopeEditor& editor;
    std::vector<TrackInfo> tracks;
    int hoverRow = -1;
    juce::TextEditor renameField;
    int renamingIndex = -1;
    uint32_t renamingGeneration = 0;
};

//==============================================================================
/** L'écran de l'oscilloscope. */
class ScopeView final : public juce::Component
{
public:
    explicit ScopeView (ScopeEditor&);

    /** Relit les anneaux et recalcule les colonnes (sauf si figé). */
    void refresh (const std::vector<TrackInfo>&);
    void paint (juce::Graphics&) override;

    struct Trace
    {
        juce::String name;
        juce::Colour colour;
        std::vector<osc::Column> columns;
        bool own = false;
    };

    const std::vector<Trace>& getTraces() const     { return traces; }
    bool hasWindow() const noexcept                 { return windowValid; }
    const osc::Window& getWindow() const noexcept   { return window; }

private:
    void drawGrid (juce::Graphics&, juce::Rectangle<float> area, bool centreOnly) const;
    void drawTrace (juce::Graphics&, const Trace&, juce::Rectangle<float> lane) const;

    ScopeEditor& editor;
    std::vector<Trace> traces;
    osc::Window window;
    bool windowValid = false;
    bool frozenShown = false;
    double beats = 1.0;
    ScopeProcessor::View view = ScopeProcessor::overlay;
};

//==============================================================================
class ScopeEditor final : public juce::AudioProcessorEditor,
                          private juce::Timer
{
public:
    explicit ScopeEditor (ScopeProcessor&);
    ~ScopeEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    ScopeProcessor& getProcessor() noexcept     { return processor; }
    OscLook& getLook() noexcept                 { return look; }

    /** Une image : relit le hub, met à jour la liste et l'écran. Appelé par le minuteur (et par les tests). */
    void tick();

    // Actions de la liste
    void chooseColour (int row);
    void toggleHidden (int row);
    void showLatencyMenu (int row);
    void rename (int index, uint32_t generation, const juce::String& name);

    static constexpr int headerHeight = 48, footerHeight = 60, listWidth = 248;
    static constexpr int defaultWidth = 1100, defaultHeight = 560;
    static constexpr int minWidth = 900, minHeight = 400, maxWidth = 3000, maxHeight = 1800;

    TrackList& getTrackList() noexcept          { return trackList; }
    ScopeView& getScopeView() noexcept          { return scopeView; }
    std::vector<TrackInfo> readTracks() const;

private:
    void timerCallback() override               { tick(); }
    void setChoice (const char* id, int index);
    void refreshControls();
    osc::Slot* slotFor (int row, uint32_t* generation = nullptr) const;

    ScopeProcessor& processor;
    OscLook look;

    TrackList trackList;
    ScopeView scopeView;

    juce::TextButton timePrev, timeValue, timeNext;
    juce::TextButton viewButtons[3];
    juce::TextButton channelButtons[4];
    juce::TextButton freezeButton { "FREEZE" };
    juce::Slider gainSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gainAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> freezeAttachment;

    std::unique_ptr<juce::TooltipWindow> tooltips;
    bool sized = false;
    int frameCount = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScopeEditor)
};
