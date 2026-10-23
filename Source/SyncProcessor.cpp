#include "SyncProcessor.h"
#include "HubAccess.h"
#include "Names.h"

SyncProcessor::SyncProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    hub = osc::getProcessHub();
    if (hub != nullptr)
        slotIndex = hub->claim (osc::Kind::sync, 2);
    tap.attach (hub, slotIndex);
}

SyncProcessor::~SyncProcessor()
{
    if (hub != nullptr)
        hub->release (slotIndex);
}

void SyncProcessor::prepareToPlay (double sampleRate, int)
{
    tap.prepare (sampleRate);
}

bool SyncProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void SyncProcessor::write (const juce::AudioBuffer<float>& buffer)
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
    tap.process (left, right, buffer.getNumSamples(), t);
}

void SyncProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());
    write (buffer);
}

void SyncProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    write (buffer);
}

void SyncProcessor::updateTrackProperties (const TrackProperties& properties)
{
    if (hub != nullptr && slotIndex >= 0)
        osc::Slot::writeText (hub->slots[slotIndex].trackName, properties.name.has_value() ? properties.name->toRawUTF8() : "");
}

juce::AudioProcessorEditor* SyncProcessor::createEditor()
{
    return new SyncEditor (*this);
}

//==============================================================================
SyncEditor::SyncEditor (SyncProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    setSize (440, 190);
    timerCallback();
    startTimerHz (5);
}

SyncEditor::~SyncEditor() = default;

juce::StringArray SyncEditor::describe() const
{
    juce::StringArray out;
    auto* hub = processor.getHub();
    const int me = processor.getSlotIndex();
    if (hub == nullptr || me < 0)
    {
        out.add ("NOT CONNECTED");
        return out;
    }

    const uint32_t generation = hub->slots[me].generation.load();
    for (int i = 0; i < osc::maxSlots; ++i)
    {
        const auto& s = hub->slots[i];
        if (! s.isAlive() || s.getKind() != osc::Kind::scope)
            continue;
        if (s.pairedSlot.load() != me || s.pairedGeneration.load() != generation)
            continue;

        const auto state = (osc::LatencyState) s.latencyState.load();
        juce::String status;
        if (state == osc::LatencyState::measuring)
            status = "MEASURING...";
        else
            status = osc::latencyText (s.latency.load(), s.sampleRate.load());
        out.add (osc::displayName (*hub, i) + "|" + status);
    }

    return out;
}

void SyncEditor::timerCallback()
{
    const auto next = describe();
    if (next != lines)
    {
        lines = next;
        repaint();
    }
}

void SyncEditor::paint (juce::Graphics& g)
{
    using namespace osclook;
    g.fillAll (OscLook::ground());

    const float w = (float) getWidth();
    drawText (g, "OSCILLOSCOPE SYNC", look.title (13.0f, 0.18f), OscLook::ink(), { 20.0f, 16.0f, w - 40.0f, 20.0f }, juce::Justification::centredLeft);
    drawText (g, "BY PHRAGMA  " + juce::String ("V") + OSC_VERSION, look.mono (9.0f, 0.2f), OscLook::dim(), { 20.0f, 16.0f, w - 40.0f, 20.0f }, juce::Justification::centredRight);

    g.setColour (OscLook::line());
    g.fillRect (20.0f, 46.0f, w - 40.0f, 1.0f);

    drawText (g, "PLACE FIRST ON THE TRACK. AUDIO PASSES UNCHANGED.", look.mono (9.5f, 0.12f), OscLook::label(),
              { 20.0f, 56.0f, w - 40.0f, 16.0f }, juce::Justification::centredLeft);

    float y = 88.0f;
    if (lines.isEmpty())
    {
        drawText (g, "WAITING FOR AN OSCILLOSCOPE", look.mono (11.0f, 0.12f, true), OscLook::ink(), { 20.0f, y, w - 40.0f, 18.0f }, juce::Justification::centredLeft);
        drawText (g, "LATER ON THIS TRACK, AND FOR SOUND TO PLAY.", look.mono (11.0f, 0.12f, true), OscLook::ink(), { 20.0f, y + 20.0f, w - 40.0f, 18.0f }, juce::Justification::centredLeft);
        return;
    }

    drawText (g, "LATENCY MEASURED FOR", look.mono (9.0f, 0.2f), OscLook::dim(), { 20.0f, y - 8.0f, w - 40.0f, 14.0f }, juce::Justification::centredLeft);
    y += 10.0f;
    for (auto& l : lines)
    {
        if (y > (float) getHeight() - 22.0f)
            break;
        const auto name = l.upToFirstOccurrenceOf ("|", false, false);
        const auto status = l.fromFirstOccurrenceOf ("|", false, false);
        drawText (g, name.toUpperCase(), look.mono (11.0f, 0.1f, true), OscLook::ink(), { 20.0f, y, w * 0.5f - 20.0f, 18.0f }, juce::Justification::centredLeft);
        drawText (g, status, look.mono (11.0f, 0.06f), OscLook::white(), { w * 0.5f, y, w * 0.5f - 20.0f, 18.0f }, juce::Justification::centredRight);
        y += 22.0f;
    }
}

//==============================================================================
#if ! OSC_TESTS
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SyncProcessor();
}
#endif
