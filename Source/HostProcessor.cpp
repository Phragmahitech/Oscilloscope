#include "HostProcessor.h"
#include "HubAccess.h"
#include "Names.h"

//==============================================================================
/** Un paramètre d'automation relayé vers le paramètre de même rang de l'instrument chargé. */
class HostProcessor::Proxy final : public juce::HostedAudioProcessorParameter
{
public:
    explicit Proxy (int i) : juce::HostedAudioProcessorParameter (1), index (i) {}

    juce::String getParameterID() const override        { return "p" + juce::String (index + 1); }

    float getValue() const override                     { return value.load(); }

    /** Appelé par l'hôte (souvent sur le fil audio) : appliqué à l'instrument au prochain bloc. */
    void setValue (float v) override
    {
        value.store (v);
        pending.store (true);
    }

    float getDefaultValue() const override              { return defaultValue.load(); }

    juce::String getName (int maximumStringLength) const override
    {
        const juce::ScopedLock sl (textLock);
        return (name.isNotEmpty() ? name : "P" + juce::String (index + 1)).substring (0, maximumStringLength);
    }

    juce::String getLabel() const override
    {
        const juce::ScopedLock sl (textLock);
        return label;
    }

    juce::String getText (float v, int maximumStringLength) const override
    {
        const juce::ScopedLock sl (textLock);
        if (target != nullptr)
            return target->getText (v, maximumStringLength);
        return juce::String (v, 2).substring (0, maximumStringLength);
    }

    float getValueForText (const juce::String& text) const override
    {
        const juce::ScopedLock sl (textLock);
        if (target != nullptr)
            return target->getValueForText (text);
        return juce::jlimit (0.0f, 1.0f, text.getFloatValue());
    }

    int getNumSteps() const override                    { return steps.load(); }

    /** Relie (ou délie, nullptr) le paramètre de l'instrument. Fil des messages, sous innerLock. */
    void bind (juce::AudioProcessorParameter* p)
    {
        const juce::ScopedLock sl (textLock);
        target = p;
        name = p != nullptr ? p->getName (64) : juce::String();
        label = p != nullptr ? p->getLabel() : juce::String();
        if (p != nullptr)
        {
            value.store (p->getValue());
            defaultValue.store (p->getDefaultValue());
            steps.store (p->getNumSteps());
        }
        else
        {
            steps.store (AudioProcessor::getDefaultNumParameterSteps());
        }
        pending.store (false);
    }

    /** Fil audio, sous innerLock : envoie la valeur reçue de l'hôte à l'instrument. */
    void apply() noexcept
    {
        if (pending.exchange (false) && target != nullptr)
            target->setValue (value.load());
    }

    const int index;
    std::atomic<float> value { 0.0f };

private:
    std::atomic<float> defaultValue { 0.0f };
    std::atomic<int> steps { AudioProcessor::getDefaultNumParameterSteps() };
    std::atomic<bool> pending { false };
    juce::CriticalSection textLock;
    juce::AudioProcessorParameter* target = nullptr;
    juce::String name, label;
};

//==============================================================================
HostProcessor::HostProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    formatManager.addFormat (std::make_unique<juce::VST3PluginFormat>());

    for (int i = 0; i < numProxies; ++i)
    {
        auto* p = new Proxy (i);
        proxies.push_back (p);
        addParameter (p);
    }

    hub = osc::getProcessHub();
    if (hub != nullptr)
        slotIndex = hub->claim (osc::Kind::sync, 2);
    tap.attach (hub, slotIndex);

    work.setSize (8, 8192);
    startTimer (300);
}

HostProcessor::~HostProcessor()
{
    stopTimer();
    unloadPlugin();
    if (hub != nullptr)
        hub->release (slotIndex);
}

double HostProcessor::getTailLengthSeconds() const
{
    return inner != nullptr ? inner->getTailLengthSeconds() : 0.0;
}

bool HostProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void HostProcessor::prepareInner (juce::AudioPluginInstance& p)
{
    p.setRateAndBufferSizeDetails (currentRate, currentBlock);
    p.setNonRealtime (isNonRealtime());
    p.prepareToPlay (currentRate, currentBlock);
}

void HostProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    currentBlock = juce::jmax (1, samplesPerBlock);
    tap.prepare (currentRate);

    const juce::SpinLock::ScopedLockType sl (innerLock);
    work.setSize (juce::jmax (8, work.getNumChannels()), juce::jmax (currentBlock, 8192), false, true, true);
    if (inner != nullptr)
        prepareInner (*inner);
    prepared = true;
}

void HostProcessor::releaseResources()
{
    const juce::SpinLock::ScopedLockType sl (innerLock);
    if (inner != nullptr)
        inner->releaseResources();
    prepared = false;
}

void HostProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    buffer.clear();

    {
        const juce::SpinLock::ScopedTryLockType sl (innerLock);
        if (sl.isLocked() && inner != nullptr && prepared && ! inner->isSuspended())
        {
            for (auto* p : proxies)
                p->apply();

            inner->setPlayHead (getPlayHead());

            const int ins = inner->getTotalNumInputChannels(), outs = inner->getTotalNumOutputChannels();
            const int ch = juce::jmax (1, ins, outs);
            if (work.getNumChannels() < ch || work.getNumSamples() < n)
                work.setSize (juce::jmax (ch, work.getNumChannels()), juce::jmax (n, work.getNumSamples()), false, false, true);

            juce::AudioBuffer<float> view (work.getArrayOfWritePointers(), ch, n);
            view.clear();
            inner->processBlock (view, midi);

            if (outs > 0)
                for (int c = 0; c < buffer.getNumChannels(); ++c)
                    buffer.copyFrom (c, 0, view, juce::jmin (c, outs - 1), 0, n);
        }
    }

    midi.clear();

    if (slotIndex >= 0)
    {
        osc::Transport t;
        if (auto* ph = getPlayHead())
            if (const auto pos = ph->getPosition())
            {
                t.playing = pos->getIsPlaying();
                if (const auto s = pos->getTimeInSamples())   { t.hasSamplePosition = true; t.samplePosition = *s; }
                if (const auto q = pos->getPpqPosition())     { t.hasPpq = true; t.ppq = *q; }
                if (const auto b = pos->getBpm())             { t.bpm = *b; }
            }

        const float* left = buffer.getNumChannels() > 0 ? buffer.getReadPointer (0) : nullptr;
        const float* right = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : left;
        if (left != nullptr)
            tap.process (left, right, n, t);
    }
}

void HostProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    buffer.clear();
    midi.clear();
}

//==============================================================================
void HostProcessor::refreshLatency()
{
    const int l = inner != nullptr ? juce::jmax (0, inner->getLatencySamples()) : 0;
    if (hub != nullptr && slotIndex >= 0)
        hub->slots[slotIndex].sourceLatency.store (l);

    if (l != innerLatency.load() || l != getLatencySamples())
    {
        innerLatency.store (l);
        setLatencySamples (l);   // Live recalcule sa compensation
        sendChangeMessage();
    }
}

void HostProcessor::attachProxies()
{
    relayed.clear();
    innerToProxy.clear();
    if (inner == nullptr)
        return;

    // Seulement les paramètres automatisables, sans le Bypass (Live a son propre bouton)
    const auto& params = inner->getParameters();
    auto* bypass = inner->getBypassParameter();
    innerToProxy.assign ((size_t) params.size(), -1);
    for (int i = 0; i < params.size() && (int) relayed.size() < numProxies; ++i)
    {
        auto* p = params[i];
        if (p == bypass || ! p->isAutomatable())
            continue;
        innerToProxy[(size_t) i] = (int) relayed.size();
        relayed.push_back (p);
    }

    for (int i = 0; i < numProxies; ++i)
    {
        auto* p = i < (int) relayed.size() ? relayed[(size_t) i] : nullptr;
        proxies[(size_t) i]->bind (p);
        if (p != nullptr)
            p->addListener (this);
    }
}

void HostProcessor::detachProxies()
{
    for (auto* p : relayed)
        p->removeListener (this);
    relayed.clear();
    innerToProxy.clear();
    for (auto* p : proxies)
        p->bind (nullptr);
}

void HostProcessor::parameterValueChanged (int parameterIndex, float newValue)
{
    if (parameterIndex < 0 || parameterIndex >= (int) innerToProxy.size() || innerToProxy[(size_t) parameterIndex] < 0)
        return;
    auto* p = proxies[(size_t) innerToProxy[(size_t) parameterIndex]];
    // Ignore l'écho d'une valeur envoyée par l'hôte (sinon Live croirait à un geste et couperait l'automation)
    if (std::abs (p->value.load() - newValue) < 1.0e-6f)
        return;
    p->value.store (newValue);
    p->sendValueChangedMessageToListeners (newValue);
}

void HostProcessor::parameterGestureChanged (int parameterIndex, bool starting)
{
    if (parameterIndex < 0 || parameterIndex >= (int) innerToProxy.size() || innerToProxy[(size_t) parameterIndex] < 0)
        return;
    auto* p = proxies[(size_t) innerToProxy[(size_t) parameterIndex]];
    if (starting) p->beginChangeGesture();
    else          p->endChangeGesture();
}

//==============================================================================
bool HostProcessor::loadPlugin (const juce::PluginDescription& description, juce::String& error, const juce::MemoryBlock* state)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (description.manufacturerName == "Phragma" && description.name.startsWith ("Oscilloscope"))
    {
        error = "Oscilloscope plugins cannot be loaded here.";
        return false;
    }

    auto instance = formatManager.createPluginInstance (description, currentRate, currentBlock, error);
    if (instance == nullptr)
    {
        if (error.isEmpty())
            error = "Could not load " + description.name + ".";
        return false;
    }

    if (state != nullptr && state->getSize() > 0)
        instance->setStateInformation (state->getData(), (int) state->getSize());

    if (prepared)
        prepareInner (*instance);

    if (openEditor != nullptr)
        openEditor->dropInnerEditor();

    std::unique_ptr<juce::AudioPluginInstance> old;
    {
        const juce::SpinLock::ScopedLockType sl (innerLock);
        detachProxies();
        old = std::move (inner);
        inner = std::move (instance);
        innerDescription = description;
        attachProxies();
    }

    if (old != nullptr)
    {
        old->releaseResources();
        old.reset();
    }

    loadError = {};
    pendingState = {};
    if (hub != nullptr && slotIndex >= 0)
        osc::Slot::writeText (hub->slots[slotIndex].sourceName, description.name.toRawUTF8());

    refreshLatency();
    updateHostDisplay (ChangeDetails().withParameterInfoChanged (true));
    if (openEditor != nullptr)
        openEditor->rebuild();
    sendChangeMessage();
    return true;
}

bool HostProcessor::loadFile (const juce::File& file, juce::String& error, const juce::String& name)
{
    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* format : formatManager.getFormats())
        if (format->fileMightContainThisPluginType (file.getFullPathName()))
            format->findAllTypesForFile (types, file.getFullPathName());

    if (types.isEmpty())
    {
        error = "No VST3 plugin found in " + file.getFileName() + ".";
        return false;
    }

    const juce::PluginDescription* chosen = nullptr;
    for (auto* t : types)
        if (name.isNotEmpty() && t->name == name)
            chosen = t;
    for (auto* t : types)
        if (chosen == nullptr && t->isInstrument)
            chosen = t;
    if (chosen == nullptr)
        chosen = types.getFirst();

    return loadPlugin (*chosen, error);
}

void HostProcessor::unloadPlugin()
{
    if (inner == nullptr)
        return;

    if (openEditor != nullptr)
        openEditor->dropInnerEditor();

    std::unique_ptr<juce::AudioPluginInstance> old;
    {
        const juce::SpinLock::ScopedLockType sl (innerLock);
        detachProxies();
        old = std::move (inner);
    }
    old->releaseResources();
    old.reset();

    innerDescription = {};
    if (hub != nullptr && slotIndex >= 0)
        osc::Slot::writeText (hub->slots[slotIndex].sourceName, "");
    refreshLatency();
    updateHostDisplay (ChangeDetails().withParameterInfoChanged (true));
    if (openEditor != nullptr)
        openEditor->rebuild();
    sendChangeMessage();
}

juce::String HostProcessor::getInnerName() const
{
    return inner != nullptr ? innerDescription.name : juce::String();
}

juce::Array<juce::File> HostProcessor::findVst3Files()
{
    juce::Array<juce::File> dirs;
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
   #if JUCE_MAC
    dirs.add (home.getChildFile ("Library/Audio/Plug-Ins/VST3"));
    dirs.add (juce::File ("/Library/Audio/Plug-Ins/VST3"));
   #elif JUCE_WINDOWS
    const auto common = juce::SystemStats::getEnvironmentVariable ("CommonProgramFiles", "C:\\Program Files\\Common Files");
    dirs.add (juce::File (common).getChildFile ("VST3"));
    dirs.add (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getParentDirectory().getChildFile ("Local\\Programs\\Common\\VST3"));
   #else
    dirs.add (home.getChildFile (".vst3"));
    dirs.add (juce::File ("/usr/lib/vst3"));
    dirs.add (juce::File ("/usr/local/lib/vst3"));
   #endif
    const auto extra = juce::SystemStats::getEnvironmentVariable ("OSC_EXTRA_VST3_DIR", {});
    if (extra.isNotEmpty())
        dirs.add (juce::File (extra));

    juce::Array<juce::File> found;
    for (auto& dir : dirs)
    {
        if (! dir.isDirectory())
            continue;
        for (const auto& entry : juce::RangedDirectoryIterator (dir, true, "*.vst3", juce::File::findFilesAndDirectories))
        {
            const auto f = entry.getFile();
            // pas l'intérieur d'un bundle, pas nos propres plugins, pas de doublon (même nom dans deux dossiers)
            if (f.getFullPathName().upToLastOccurrenceOf (f.getFileName(), false, false).containsIgnoreCase (".vst3"))
                continue;
            if (f.getFileNameWithoutExtension().startsWith ("Oscilloscope"))
                continue;
            bool duplicate = false;
            for (auto& g : found)
                duplicate = duplicate || g.getFileName() == f.getFileName();
            if (! duplicate)
                found.add (f);
        }
    }

    std::sort (found.begin(), found.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getFileName().compareIgnoreCase (b.getFileName()) < 0;
    });
    return found;
}

//==============================================================================
void HostProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree state ("OscilloscopeSyncInstrument");

    if (inner != nullptr)
    {
        juce::MemoryBlock innerState;
        inner->getStateInformation (innerState);
        if (auto xml = innerDescription.createXml())
            state.appendChild (juce::ValueTree::fromXml (*xml), nullptr);
        state.setProperty ("innerState", innerState.toBase64Encoding(), nullptr);
    }
    else if (pendingState.isValid())
    {
        state = pendingState.createCopy();
    }

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void HostProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName ("OscilloscopeSyncInstrument"))
        return;

    const auto state = juce::ValueTree::fromXml (*xml);
    auto load = [state] (HostProcessor& self)
    {
        const auto pluginTree = state.getChildWithName ("PLUGIN");
        if (! pluginTree.isValid())
        {
            self.unloadPlugin();
            return;
        }

        juce::PluginDescription description;
        std::unique_ptr<juce::XmlElement> pluginXml (pluginTree.createXml());
        if (pluginXml == nullptr || ! description.loadFromXml (*pluginXml))
            return;

        juce::MemoryBlock innerState;
        innerState.fromBase64Encoding (state["innerState"].toString());

        juce::String error;
        if (! self.loadPlugin (description, error, &innerState))
        {
            self.loadError = "CANNOT LOAD " + description.name.toUpperCase() + " (" + error + ")";
            self.pendingState = state.createCopy();   // rien n'est perdu à la prochaine sauvegarde
            self.sendChangeMessage();
        }
    };

    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        load (*this);
    }
    else
    {
        juce::WeakReference<HostProcessor> weak (this);
        juce::MessageManager::callAsync ([weak, load]
        {
            if (auto* self = weak.get())
                load (*self);
        });
    }
}

void HostProcessor::updateTrackProperties (const TrackProperties& properties)
{
    if (hub != nullptr && slotIndex >= 0)
        osc::Slot::writeText (hub->slots[slotIndex].trackName, properties.name.has_value() ? properties.name->toRawUTF8() : "");
}

juce::AudioProcessorEditor* HostProcessor::createEditor()
{
    return new HostEditor (*this);
}

//==============================================================================
HostEditor::HostEditor (HostProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    setLookAndFeel (&look);
    for (auto* b : { &loadButton, &unloadButton })
    {
        b->setMouseCursor (juce::MouseCursor::PointingHandCursor);
        addAndMakeVisible (*b);
    }
    loadButton.onClick = [this] { showLoadMenu(); };
    unloadButton.onClick = [this] { processor.unloadPlugin(); };

    processor.setOpenEditor (this);
    processor.addChangeListener (this);
    rebuild();
    timerCallback();
    startTimerHz (5);
}

HostEditor::~HostEditor()
{
    stopTimer();
    processor.removeChangeListener (this);
    dropInnerEditor();
    processor.setOpenEditor (nullptr);
    setLookAndFeel (nullptr);
}

void HostEditor::dropInnerEditor()
{
    if (innerEditor == nullptr)
        return;
    innerEditor->removeComponentListener (this);
    removeChildComponent (innerEditor.get());
    innerEditor.reset();
}

void HostEditor::rebuild()
{
    dropInnerEditor();

    if (auto* inner = processor.getInner())
    {
        juce::AudioProcessorEditor* e = inner->hasEditor() ? inner->createEditorAndMakeActive() : nullptr;
        if (e == nullptr)
            e = new juce::GenericAudioProcessorEditor (*inner);
        innerEditor.reset (e);
        addAndMakeVisible (*innerEditor);
        innerEditor->addComponentListener (this);
    }

    unloadButton.setVisible (processor.getInner() != nullptr);
    loadButton.setButtonText (processor.getInner() != nullptr ? "CHANGE" : "LOAD INSTRUMENT");
    layoutSize();
    repaint();
}

void HostEditor::layoutSize()
{
    int w = minWidth, h = headerHeight + emptyHeight;
    if (innerEditor != nullptr)
    {
        w = juce::jmax (minWidth, innerEditor->getWidth());
        h = headerHeight + innerEditor->getHeight();
    }
    if (w != getWidth() || h != getHeight())
        setSize (w, h);
    else
        resized();
}

void HostEditor::componentMovedOrResized (juce::Component& c, bool, bool wasResized)
{
    if (&c == innerEditor.get() && wasResized)
        layoutSize();
}

void HostEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Instrument changé (ou latence) : on ne reconstruit que si l'instrument n'est plus le même
    const bool haveEditor = innerEditor != nullptr;
    const bool haveInner = processor.getInner() != nullptr;
    if (haveEditor != haveInner
        || (haveEditor && innerEditor->getAudioProcessor() != processor.getInner()))
        rebuild();
    repaint();
}

juce::StringArray HostEditor::describe() const
{
    auto* hub = processor.getHub();
    if (hub == nullptr || processor.getSlotIndex() < 0)
        return { "NOT CONNECTED" };
    return osc::describeFollowers (*hub, processor.getSlotIndex());
}

void HostEditor::timerCallback()
{
    const auto next = describe();
    if (next != lines || shownLatency != processor.getInnerLatency())
    {
        lines = next;
        shownLatency = processor.getInnerLatency();
        repaint (0, 0, getWidth(), headerHeight);
    }
}

void HostEditor::showLoadMenu()
{
    const auto files = HostProcessor::findVst3Files();
    juce::PopupMenu m;
    m.addSectionHeader ("VST3 INSTRUMENTS");
    for (int i = 0; i < files.size(); ++i)
        m.addItem (i + 1, files[i].getFileNameWithoutExtension(), true, files[i].getFileNameWithoutExtension() == processor.getInnerName());
    if (files.isEmpty())
        m.addItem (-1, "(NO VST3 FOUND)", false);
    m.addSeparator();
    m.addItem (100000, "BROWSE...");

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (loadButton),
                     [safe = juce::Component::SafePointer<HostEditor> (this), files] (int r)
                     {
                         if (safe == nullptr || r == 0)
                             return;

                         auto load = [safe] (const juce::File& f)
                         {
                             if (safe == nullptr)
                                 return;
                             juce::String error;
                             if (! safe->processor.loadFile (f, error))
                                 juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Oscilloscope Sync Instrument", error);
                         };

                         if (r == 100000)
                         {
                             safe->chooser = std::make_unique<juce::FileChooser> ("Choose a VST3 instrument", juce::File(), "*.vst3");
                             safe->chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                                             | juce::FileBrowserComponent::canSelectDirectories,
                                                         [load] (const juce::FileChooser& fc)
                                                         {
                                                             if (fc.getResult() != juce::File())
                                                                 load (fc.getResult());
                                                         });
                             return;
                         }

                         if (r > 0 && r <= files.size())
                             load (files[r - 1]);
                     });
}

void HostEditor::paint (juce::Graphics& g)
{
    using osclook::drawText;
    g.fillAll (OscLook::ground());
    const float w = (float) getWidth();

    drawText (g, "OSCILLOSCOPE SYNC INSTRUMENT", look.title (11.0f, 0.18f), OscLook::ink(), { 16.0f, 10.0f, w - 32.0f, 18.0f }, juce::Justification::centredLeft);

    juce::String line;
    juce::Colour colour = OscLook::label();
    if (processor.getInner() != nullptr)
    {
        line = processor.getInnerName().toUpperCase() + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 LATENCY "))
             + osc::latencyText (processor.getInnerLatency(), processor.getSampleRate());
        colour = OscLook::ink();
    }
    else if (processor.getLoadError().isNotEmpty())
    {
        line = processor.getLoadError();
        colour = OscLook::white();
    }
    else
    {
        line = "NO INSTRUMENT LOADED";
    }
    const float textRight = (float) loadButton.getX() - 12.0f;
    drawText (g, line, look.mono (10.5f, 0.1f, true), colour, { 16.0f, 36.0f, textRight - 16.0f, 16.0f }, juce::Justification::centredLeft);

    juce::String follow;
    if (lines.isEmpty())
        follow = "NO OSCILLOSCOPE ON THIS TRACK YET";
    else
    {
        juce::StringArray parts;
        for (auto& l : lines)
            parts.add (l.upToFirstOccurrenceOf ("|", false, false).toUpperCase() + " " + l.fromFirstOccurrenceOf ("|", false, false));
        follow = "COMPENSATED: " + parts.joinIntoString (juce::String (juce::CharPointer_UTF8 ("  \xc2\xb7  ")));
    }
    drawText (g, follow, look.mono (9.0f, 0.12f), OscLook::dim(), { 16.0f, 58.0f, textRight - 16.0f, 14.0f }, juce::Justification::centredLeft);

    g.setColour (OscLook::line());
    g.fillRect (0.0f, (float) headerHeight - 1.0f, w, 1.0f);

    if (innerEditor == nullptr)
    {
        const auto area = juce::Rectangle<float> (0.0f, (float) headerHeight, w, (float) getHeight() - (float) headerHeight).reduced (24.0f);
        drawText (g, "LOAD AN INSTRUMENT (KICK3, SERUM...) INSTEAD OF PUTTING IT ON THE TRACK.", look.mono (9.5f, 0.1f), OscLook::label(),
                  area.withHeight (16.0f).withY (area.getCentreY() - 18.0f), juce::Justification::centred);
        drawText (g, "MIDI GOES TO IT. ITS LATENCY IS REPORTED TO LIVE AND TO OSCILLOSCOPE.", look.mono (9.5f, 0.1f), OscLook::label(),
                  area.withHeight (16.0f).withY (area.getCentreY() + 2.0f), juce::Justification::centred);
    }
}

void HostEditor::resized()
{
    const int w = getWidth();
    unloadButton.setBounds (w - 16 - 80, 38, 80, 26);
    const int loadWidth = unloadButton.isVisible() ? 90 : 150;
    loadButton.setBounds ((unloadButton.isVisible() ? unloadButton.getX() - 8 : w - 16) - loadWidth, 38, loadWidth, 26);
    if (innerEditor != nullptr)
        innerEditor->setTopLeftPosition (0, headerHeight);
}

//==============================================================================
#if ! OSC_TESTS
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HostProcessor();
}
#endif
