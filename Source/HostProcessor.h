#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Hub.h"
#include "Tap.h"
#include "Look.h"

/**
    Oscilloscope Sync Instrument : un instrument VST3 qui en charge un autre (Kick3, Serum...).
    Il lui passe le MIDI, sort son audio, déclare sa latence à l'hôte (la compensation de Live reste juste) et l'écrit
    dans le hub : l'Oscilloscope de la piste ajoute cette latence à celle qu'il mesure après l'instrument.
    Les paramètres de l'instrument sont relayés par 128 paramètres d'automation (P1..P128, renommés d'après lui).
*/
class HostProcessor final : public juce::AudioProcessor,
                            public juce::ChangeBroadcaster,
                            private juce::AudioProcessorParameter::Listener,
                            private juce::Timer
{
public:
    static constexpr int numProxies = 128;

    HostProcessor();
    ~HostProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                         { return true; }

    const juce::String getName() const override             { return "Oscilloscope Sync Instrument"; }
    bool acceptsMidi() const override                       { return true; }
    bool producesMidi() const override                      { return false; }
    bool isMidiEffect() const override                      { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override                           { return 1; }
    int getCurrentProgram() override                        { return 0; }
    void setCurrentProgram (int) override                   {}
    const juce::String getProgramName (int) override        { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    void updateTrackProperties (const TrackProperties&) override;

    //==========================================================================
    /** Charge l'instrument (fil des messages). Remplace le précédent. false + message si impossible. */
    bool loadPlugin (const juce::PluginDescription&, juce::String& error, const juce::MemoryBlock* state = nullptr);
    /** Trouve les instruments d'un fichier .vst3 et charge le premier (ou celui nommé `name`). */
    bool loadFile (const juce::File&, juce::String& error, const juce::String& name = {});
    void unloadPlugin();

    juce::AudioPluginInstance* getInner() const noexcept    { return inner.get(); }
    juce::String getInnerName() const;
    int getInnerLatency() const noexcept                    { return innerLatency.load(); }
    /** Message affiché quand l'instrument d'un projet n'a pas pu être rechargé. */
    juce::String getLoadError() const                       { return loadError; }

    /** Les .vst3 trouvés dans les dossiers habituels (sans les plugins Oscilloscope). */
    static juce::Array<juce::File> findVst3Files();

    osc::Hub* getHub() const noexcept                       { return hub; }
    int getSlotIndex() const noexcept                       { return slotIndex; }

    /** Pour les tests : relit la latence de l'instrument tout de suite. */
    void refreshLatencyNow()                                { refreshLatency(); }

    juce::AudioPluginFormatManager& getFormatManager()      { return formatManager; }

    /** La fenêtre ouverte (une seule), tenue par la fenêtre elle-même : on ferme celle de l'instrument avant de le détruire. */
    void setOpenEditor (class HostEditor* e) noexcept      { openEditor = e; }

private:
    class Proxy;

    void refreshLatency();
    void prepareInner (juce::AudioPluginInstance&);
    void attachProxies();
    void detachProxies();
    void timerCallback() override                           { refreshLatency(); }

    void parameterValueChanged (int parameterIndex, float newValue) override;
    void parameterGestureChanged (int parameterIndex, bool gestureIsStarting) override;

    osc::Hub* hub = nullptr;
    int slotIndex = -1;
    osc::Tap tap;

    juce::AudioPluginFormatManager formatManager;
    std::unique_ptr<juce::AudioPluginInstance> inner;
    juce::PluginDescription innerDescription;
    juce::SpinLock innerLock;                 // le fil audio n'attend jamais : s'il ne l'obtient pas, il rend du silence
    std::atomic<int> innerLatency { 0 };

    juce::AudioBuffer<float> work;
    double currentRate = 44100.0;
    int currentBlock = 512;
    bool prepared = false;

    std::vector<Proxy*> proxies;
    std::vector<int> innerToProxy;           // rang dans l'instrument -> rang du paramètre relayé (-1 : pas relayé)
    std::vector<juce::AudioProcessorParameter*> relayed;
    class HostEditor* openEditor = nullptr;
    juce::String loadError;
    juce::ValueTree pendingState;            // état d'un instrument introuvable : gardé tel quel à la sauvegarde

    JUCE_DECLARE_WEAK_REFERENCEABLE (HostProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HostProcessor)
};

//==============================================================================
/** En-tête (choix de l'instrument, latence) + la fenêtre de l'instrument dessous. */
class HostEditor final : public juce::AudioProcessorEditor,
                         private juce::ChangeListener,
                         private juce::ComponentListener,
                         private juce::Timer
{
public:
    explicit HostEditor (HostProcessor&);
    ~HostEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int headerHeight = 84, minWidth = 640, emptyHeight = 240;

    /** À appeler avant de changer d'instrument : ferme sa fenêtre. */
    void dropInnerEditor();
    /** Ouvre la fenêtre de l'instrument actuel. */
    void rebuild();
    juce::Component* getInnerEditor() const noexcept    { return innerEditor.get(); }
    juce::StringArray describe() const;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void componentMovedOrResized (juce::Component&, bool wasMoved, bool wasResized) override;
    void timerCallback() override;
    void layoutSize();
    void showLoadMenu();

    HostProcessor& processor;
    OscLook look;
    juce::TextButton loadButton { "LOAD INSTRUMENT" }, unloadButton { "REMOVE" };
    std::unique_ptr<juce::AudioProcessorEditor> innerEditor;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::StringArray lines;
    int shownLatency = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HostEditor)
};
