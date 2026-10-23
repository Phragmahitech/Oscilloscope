#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Hub.h"
#include "Tap.h"
#include "Look.h"

/**
    Oscilloscope Sync : à placer en premier sur la piste. Il ne touche pas au son ; il écrit ce qu'il reçoit dans le
    hub, et les Oscilloscope placés plus loin sur la piste s'en servent pour mesurer la latence des plugins entre eux.
*/
class SyncProcessor final : public juce::AudioProcessor
{
public:
    SyncProcessor();
    ~SyncProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                         { return true; }

    const juce::String getName() const override             { return "Oscilloscope Sync"; }
    bool acceptsMidi() const override                       { return false; }
    bool producesMidi() const override                      { return false; }
    bool isMidiEffect() const override                      { return false; }
    double getTailLengthSeconds() const override            { return 0.0; }

    int getNumPrograms() override                           { return 1; }
    int getCurrentProgram() override                        { return 0; }
    void setCurrentProgram (int) override                   {}
    const juce::String getProgramName (int) override        { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override  {}
    void setStateInformation (const void*, int) override    {}

    void updateTrackProperties (const TrackProperties&) override;

    osc::Hub* getHub() const noexcept               { return hub; }
    int getSlotIndex() const noexcept               { return slotIndex; }

private:
    void write (const juce::AudioBuffer<float>&);

    osc::Hub* hub = nullptr;
    int slotIndex = -1;
    osc::Tap tap;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SyncProcessor)
};

//==============================================================================
/** Petite fenêtre : ce que le Sync a trouvé (les Oscilloscope qui le suivent et leur latence). */
class SyncEditor final : public juce::AudioProcessorEditor,
                         private juce::Timer
{
public:
    explicit SyncEditor (SyncProcessor&);
    ~SyncEditor() override;

    void paint (juce::Graphics&) override;

    /** Les lignes affichées (pour les tests). */
    juce::StringArray describe() const;

private:
    void timerCallback() override;

    SyncProcessor& processor;
    OscLook look;
    juce::StringArray lines;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SyncEditor)
};
