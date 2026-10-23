#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Hub.h"
#include "Tap.h"
#include "LatencyFinder.h"
#include "Trace.h"

/**
    Oscilloscope : laisse passer le son sans y toucher, l'écrit dans le hub partagé et affiche toutes les instances
    du projet. La latence des plugins placés entre un Oscilloscope Sync (en tête de piste) et lui est mesurée en
    continu, et retranchée à l'affichage.
*/
class ScopeProcessor final : public juce::AudioProcessor,
                             private juce::TimeSliceClient
{
public:
    // Identifiants des paramètres (ne pas les renommer : les projets DAW sauvegardés en dépendent)
    static constexpr const char* timeId    = "time";
    static constexpr const char* viewId    = "view";
    static constexpr const char* channelId = "channel";
    static constexpr const char* gainId    = "gain";
    static constexpr const char* freezeId  = "freeze";

    /** Durées affichées, en noires. L'index 3 (1/4) est la valeur par défaut. */
    static const juce::StringArray& timeNames();
    static double timeInBeats (int index);
    static constexpr int defaultTimeIndex = 3;

    enum View { overlay = 0, split = 1, sum = 2 };

    ScopeProcessor();
    ~ScopeProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                         { return true; }

    const juce::String getName() const override             { return "Oscilloscope"; }
    bool acceptsMidi() const override                       { return false; }
    bool producesMidi() const override                      { return false; }
    bool isMidiEffect() const override                      { return false; }
    double getTailLengthSeconds() const override            { return 0.0; }

    int getNumPrograms() override                           { return 1; }
    int getCurrentProgram() override                        { return 0; }
    void setCurrentProgram (int) override                   {}
    const juce::String getProgramName (int) override        { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    void updateTrackProperties (const TrackProperties&) override;

    juce::AudioProcessorValueTreeState apvts;

    //==========================================================================
    osc::Hub* getHub() const noexcept               { return hub; }
    int getSlotIndex() const noexcept               { return slotIndex; }
    osc::Slot* getSlot() const noexcept             { return slotIndex >= 0 ? &hub->slots[slotIndex] : nullptr; }

    int getTimeIndex() const noexcept               { return juce::jlimit (0, timeNames().size() - 1, (int) timeParam->load()); }
    View getView() const noexcept                   { return (View) juce::jlimit (0, 2, (int) viewParam->load()); }
    osc::Channel getChannel() const noexcept        { return (osc::Channel) juce::jlimit (0, 3, (int) channelParam->load()); }
    float getGainLinear() const noexcept            { return juce::Decibels::decibelsToGain (gainParam->load()); }
    bool isFrozen() const noexcept                  { return freezeParam->load() > 0.5f; }

    /** Pour les tests : lance une mesure de latence tout de suite, sur le fil appelant. */
    int measureNow()                                { return useTimeSlice(); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void write (const juce::AudioBuffer<float>&);
    int useTimeSlice() override;

    osc::Hub* hub = nullptr;
    int slotIndex = -1;
    osc::Tap tap;
    osc::LatencyMeasurer measurer;
    juce::CriticalSection measureLock;   // la mesure (fil de fond) contre measureNow (tests)

    struct MeasureThread;
    juce::SharedResourcePointer<MeasureThread>* measureThread = nullptr;

    std::atomic<float>* timeParam = nullptr;
    std::atomic<float>* viewParam = nullptr;
    std::atomic<float>* channelParam = nullptr;
    std::atomic<float>* gainParam = nullptr;
    std::atomic<float>* freezeParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScopeProcessor)
};
