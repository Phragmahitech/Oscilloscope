// Petit instrument VST3 pour les tests : une note MIDI déclenche un « kick » (sinus qui descend + bruit), le tout
// retardé de LATENCY échantillons, et cette latence est déclarée à l'hôte comme le ferait Kick3.
// Paramètres : gain, latency (pour vérifier qu'un changement de latence est suivi).

#include <juce_audio_processors/juce_audio_processors.h>

class TestSynth final : public juce::AudioProcessor
{
public:
    TestSynth()
        : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "TestSynth", layout())
    {
        setLatencySamples (110);
    }

    static juce::AudioProcessorValueTreeState::ParameterLayout layout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Gain", 0.0f, 1.0f, 0.8f));
        l.add (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { "latency", 1 }, "Latency", 0, 4000, 110));
        return l;
    }

    void prepareToPlay (double sr, int) override
    {
        sampleRate = sr;
        line.assign (8192, 0.0f);
        pos = 0;
        phase = 0.0;
        age = 1 << 30;
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        const int latency = (int) apvts.getRawParameterValue ("latency")->load();
        if (latency != getLatencySamples())
            setLatencySamples (latency);
        const float gain = apvts.getRawParameterValue ("gain")->load();

        std::vector<int> onsets;
        for (const auto m : midi)
            if (m.getMessage().isNoteOn())
                onsets.push_back (m.samplePosition);

        auto* l = buffer.getWritePointer (0);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            for (int o : onsets)
                if (o == i) { age = 0; phase = 0.0; }

            float v = 0.0f;
            if (age < (int) (sampleRate * 0.3))
            {
                const double t = age / sampleRate;
                phase += (45.0 + 300.0 * std::exp (-t / 0.015)) / sampleRate;
                rng = rng * 1664525u + 1013904223u;
                const float noise = ((float) (rng >> 9) / 4194304.0f - 1.0f) * (float) std::exp (-t / 0.004);
                v = gain * (float) (std::sin (2.0 * juce::MathConstants<double>::pi * phase) * std::exp (-t / 0.09)) + 0.3f * noise;
            }
            ++age;

            line[(size_t) (pos & 8191)] = v;
            l[i] = line[(size_t) ((pos - latency) & 8191)];
            ++pos;
        }
        for (int c = 1; c < buffer.getNumChannels(); ++c)
            buffer.copyFrom (c, 0, buffer, 0, 0, buffer.getNumSamples());
    }

    juce::AudioProcessorEditor* createEditor() override     { return new juce::GenericAudioProcessorEditor (*this); }
    bool hasEditor() const override                         { return true; }
    const juce::String getName() const override             { return "OscTestSynth"; }
    bool acceptsMidi() const override                       { return true; }
    bool producesMidi() const override                      { return false; }
    double getTailLengthSeconds() const override            { return 0.3; }
    int getNumPrograms() override                           { return 1; }
    int getCurrentProgram() override                        { return 0; }
    void setCurrentProgram (int) override                   {}
    const juce::String getProgramName (int) override        { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& d) override
    {
        if (auto xml = apvts.copyState().createXml())
            copyXmlToBinary (*xml, d);
    }
    void setStateInformation (const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary (data, size))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
        setLatencySamples ((int) apvts.getRawParameterValue ("latency")->load());
    }

    juce::AudioProcessorValueTreeState apvts;

private:
    double sampleRate = 48000.0, phase = 0.0;
    std::vector<float> line;
    int64_t pos = 0;
    int age = 1 << 30;
    uint32_t rng = 12345;
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TestSynth();
}
