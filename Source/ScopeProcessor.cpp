#include "ScopeProcessor.h"
#include "ScopeEditor.h"
#include "HubAccess.h"

//==============================================================================
/** Un seul fil de fond par bibliothèque pour toutes les mesures de latence (une instance après l'autre). */
struct ScopeProcessor::MeasureThread : public juce::TimeSliceThread
{
    MeasureThread() : juce::TimeSliceThread ("Oscilloscope latency")
    {
        startThread (juce::Thread::Priority::low);
    }

    ~MeasureThread() override
    {
        stopThread (2000);
    }
};

//==============================================================================
const juce::StringArray& ScopeProcessor::timeNames()
{
    static const juce::StringArray names { "1/32", "1/16", "1/8", "1/4", "1/2", "1/1", "2/1", "4/1" };
    return names;
}

double ScopeProcessor::timeInBeats (int index)
{
    static const double beats[] = { 0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0 };
    return beats[juce::jlimit (0, 7, index)];
}

juce::AudioProcessorValueTreeState::ParameterLayout ScopeProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { timeId, 1 }, "Time", timeNames(), defaultTimeIndex));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { viewId, 1 }, "View", StringArray { "Overlay", "Split", "Sum" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { channelId, 1 }, "Channel", StringArray { "Mid", "Left", "Right", "Side" }, 0));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { gainId, 1 }, "Gain", NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { freezeId, 1 }, "Freeze", false));
    return layout;
}

ScopeProcessor::ScopeProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Oscilloscope", createParameterLayout())
{
    timeParam    = apvts.getRawParameterValue (timeId);
    viewParam    = apvts.getRawParameterValue (viewId);
    channelParam = apvts.getRawParameterValue (channelId);
    gainParam    = apvts.getRawParameterValue (gainId);
    freezeParam  = apvts.getRawParameterValue (freezeId);

    hub = osc::getProcessHub();
    if (hub != nullptr)
        slotIndex = hub->claim (osc::Kind::scope, 2);
    tap.attach (hub, slotIndex);

    if (slotIndex >= 0)
    {
        measureThread = new juce::SharedResourcePointer<MeasureThread>();
        (*measureThread)->addTimeSliceClient (this, 300);
    }
}

ScopeProcessor::~ScopeProcessor()
{
    if (measureThread != nullptr)
    {
        (*measureThread)->removeTimeSliceClient (this);
        delete measureThread;
    }

    if (hub != nullptr)
        hub->release (slotIndex);
}

void ScopeProcessor::prepareToPlay (double sampleRate, int)
{
    tap.prepare (sampleRate);
}

bool ScopeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void ScopeProcessor::write (const juce::AudioBuffer<float>& buffer)
{
    if (slotIndex < 0 || buffer.getNumChannels() == 0)
        return;

    osc::Transport t;
    if (auto* ph = getPlayHead())
    {
        if (const auto pos = ph->getPosition())
        {
            t.playing = pos->getIsPlaying();
            if (const auto s = pos->getTimeInSamples())   { t.hasSamplePosition = true; t.samplePosition = *s; }
            if (const auto q = pos->getPpqPosition())     { t.hasPpq = true; t.ppq = *q; }
            if (const auto b = pos->getBpm())             { t.bpm = *b; }
        }
    }

    const float* left = buffer.getReadPointer (0);
    const float* right = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : left;
    hub->slots[slotIndex].channels.store (buffer.getNumChannels() > 1 ? 2 : 1, std::memory_order_relaxed);
    tap.process (left, right, buffer.getNumSamples(), t);
}

void ScopeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());
    write (buffer);   // le son n'est pas modifié
}

void ScopeProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Désactivé dans l'hôte : on continue d'afficher (le son passe de toute façon tel quel)
    write (buffer);
}

int ScopeProcessor::useTimeSlice()
{
    if (hub == nullptr || slotIndex < 0)
        return 1000;
    const juce::ScopedLock sl (measureLock);
    return measurer.run (*hub, slotIndex);
}

juce::AudioProcessorEditor* ScopeProcessor::createEditor()
{
    return new ScopeEditor (*this);
}

void ScopeProcessor::updateTrackProperties (const TrackProperties& properties)
{
    if (auto* s = getSlot())
        osc::Slot::writeText (s->trackName, properties.name.has_value() ? properties.name->toRawUTF8() : "");
}

//==============================================================================
void ScopeProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    if (auto* s = getSlot())
    {
        state.setProperty ("colour", juce::String::toHexString ((int) s->colour.load()), nullptr);
        state.setProperty ("manualColour", s->hasFlag (osc::flags::manualColour), nullptr);
        state.setProperty ("hidden", s->hasFlag (osc::flags::hidden), nullptr);
        state.setProperty ("latencyOff", s->hasFlag (osc::flags::latencyOff), nullptr);

        char name[osc::nameWords * 8];
        osc::Slot::readText (s->name, name);
        state.setProperty ("name", juce::String::fromUTF8 (name), nullptr);
    }

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void ScopeProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    const auto state = juce::ValueTree::fromXml (*xml);
    apvts.replaceState (state);

    if (auto* s = getSlot())
    {
        if (state.hasProperty ("colour"))
        {
            const auto colour = (uint32_t) state["colour"].toString().getHexValue64();
            if ((colour >> 24) != 0)
                hub->restoreColour (slotIndex, colour, (bool) state.getProperty ("manualColour", false));
        }

        s->setFlag (osc::flags::hidden, (bool) state.getProperty ("hidden", false));
        s->setFlag (osc::flags::latencyOff, (bool) state.getProperty ("latencyOff", false));
        osc::Slot::writeText (s->name, state.getProperty ("name", "").toString().substring (0, 40).toRawUTF8());
    }
}

//==============================================================================
#if ! OSC_TESTS
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ScopeProcessor();
}
#endif
