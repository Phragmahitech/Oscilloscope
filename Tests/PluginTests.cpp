// Tests des deux plugins dans un même programme (comme dans un DAW) : passage du son, hub partagé, mesure de la
// latence entre un Oscilloscope Sync et un Oscilloscope, couleurs, état, interface.
// Si la variable OSC_SNAPSHOT_DIR est définie, des captures de l'interface y sont écrites.

#include "../Source/ScopeProcessor.h"
#include "../Source/ScopeEditor.h"
#include "../Source/SyncProcessor.h"
#include "../Source/Names.h"
#include "../Source/HostProcessor.h"

#include <cstdio>
#include <random>

static int failures = 0, checks = 0;

#define CHECK(cond, ...) do { ++checks; if (! (cond)) { ++failures; std::printf ("  ECHEC (%s:%d) ", __FILE__, __LINE__); std::printf (__VA_ARGS__); std::printf ("\n"); } } while (0)

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 256;
    constexpr double bpm = 145.0;

    struct FakePlayHead final : public juce::AudioPlayHead
    {
        bool playing = true;
        int64_t position = 0;

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setIsPlaying (playing);
            info.setTimeInSamples (position);
            info.setPpqPosition ((double) position / (sampleRate * 60.0 / bpm));
            info.setBpm (bpm);
            return info;
        }
    };

    /** Son de test : kick, basse, bruit (pseudo-aléatoire mais reproductible). */
    float source (int64_t i, uint32_t seed)
    {
        const double t = (double) i / sampleRate;
        const double beat = 60.0 / bpm;
        const double tb = std::fmod (t, beat);
        double s = 0.8 * std::sin (2.0 * juce::MathConstants<double>::pi * (50.0 * tb + 2.2 * (1.0 - std::exp (-tb / 0.02)))) * std::exp (-tb / 0.12);
        const int step = (int) (t / (beat / 4.0));
        const double f = 40.0 + 7.0 * (double) (((uint32_t) step * 7919u + seed * 104729u) % 9u);
        if (step % 4 != 0)
            s += 0.4 * std::sin (2.0 * juce::MathConstants<double>::pi * f * t) * std::exp (-std::fmod (t, beat / 4.0) / 0.05);
        uint32_t h = (uint32_t) i * 2654435761u ^ seed * 40503u;
        h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
        s += 0.08 * ((double) (h & 0xffff) / 32768.0 - 1.0) * (step % 2 == 1 ? 1.0 : 0.2);
        return (float) s;
    }

    /** Une piste : [Sync] -> plugin qui retarde de `lag` -> [Oscilloscope]. */
    struct Track
    {
        std::unique_ptr<SyncProcessor> sync;
        std::unique_ptr<ScopeProcessor> scope;
        uint32_t seed = 1;
        int lag = 0;
        std::vector<float> line;
        size_t linePos = 0;
        int channels = 2;

        void setLag (int l) { lag = l; line.assign ((size_t) std::max (1, l) * 2, 0.0f); linePos = 0; }

        void prepare (bool withSync)
        {
            if (withSync)
            {
                sync = std::make_unique<SyncProcessor>();
                sync->setRateAndBufferSizeDetails (sampleRate, blockSize);
                sync->prepareToPlay (sampleRate, blockSize);
            }
            scope = std::make_unique<ScopeProcessor>();
            scope->setRateAndBufferSizeDetails (sampleRate, blockSize);
            scope->prepareToPlay (sampleRate, blockSize);
        }
    };

    struct Session
    {
        FakePlayHead head;
        std::vector<Track*> tracks;
        double sinceMeasure = 0.0;
        juce::AudioBuffer<float> lastOutput;

        void run (double seconds, bool measure = true)
        {
            const int blocks = (int) (seconds * sampleRate / blockSize);
            for (int b = 0; b < blocks; ++b)
            {
                for (auto* t : tracks)
                {
                    juce::AudioBuffer<float> buffer (t->channels, blockSize);
                    juce::MidiBuffer midi;
                    for (int i = 0; i < blockSize; ++i)
                    {
                        const float v = source (head.position + i, t->seed);
                        for (int ch = 0; ch < t->channels; ++ch)
                            buffer.setSample (ch, i, ch == 0 ? v : 0.8f * v);
                    }

                    if (t->sync != nullptr)
                    {
                        t->sync->setPlayHead (&head);
                        t->sync->processBlock (buffer, midi);
                    }

                    if (t->lag > 0)
                        for (int i = 0; i < blockSize; ++i)
                            for (int ch = 0; ch < t->channels; ++ch)
                            {
                                const size_t idx = (t->linePos + (size_t) i) % (size_t) t->lag;
                                float& cell = t->line[idx * 2 + (size_t) ch];
                                const float out = cell;
                                cell = buffer.getSample (ch, i);
                                buffer.setSample (ch, i, out);
                            }
                    t->linePos = (t->linePos + blockSize) % (size_t) std::max (1, t->lag);

                    juce::AudioBuffer<float> before;
                    before.makeCopyOf (buffer);
                    t->scope->setPlayHead (&head);
                    t->scope->processBlock (buffer, midi);

                    // L'Oscilloscope ne touche pas au son
                    float diff = 0.0f;
                    for (int ch = 0; ch < t->channels; ++ch)
                        for (int i = 0; i < blockSize; ++i)
                            diff = std::max (diff, std::abs (buffer.getSample (ch, i) - before.getSample (ch, i)));
                    if (diff > 0.0f)
                    {
                        ++failures;
                        std::printf ("  ECHEC : l'Oscilloscope modifie le son (%g)\n", diff);
                    }
                }

                head.position += blockSize;
                sinceMeasure += (double) blockSize / sampleRate;
                if (measure && sinceMeasure >= 0.2)
                {
                    sinceMeasure = 0.0;
                    for (auto* t : tracks)
                        t->scope->measureNow();
                }
            }
        }
    };

    int latencyOf (Track& t)    { return t.scope->getSlot()->latency.load(); }

    void setParam (juce::AudioProcessorValueTreeState& apvts, const char* id, float value)
    {
        auto* p = apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (value));
    }

    juce::File snapshotDir()
    {
        const auto dir = juce::SystemStats::getEnvironmentVariable ("OSC_SNAPSHOT_DIR", {});
        return dir.isEmpty() ? juce::File() : juce::File (dir);
    }

    void snapshot (juce::Component& c, const juce::String& name)
    {
        const auto dir = snapshotDir();
        if (dir == juce::File())
            return;
        dir.createDirectory();
        const auto image = c.createComponentSnapshot (c.getLocalBounds(), true, 2.0f);
        juce::FileOutputStream out (dir.getChildFile (name + ".png"));
        if (out.openedOk())
        {
            out.setPosition (0);
            out.truncate();
            juce::PNGImageFormat().writeImageToStream (image, out);
        }
    }

    //==========================================================================
    void testParametersAndDefaults()
    {
        std::printf ("Paramètres et valeurs par défaut\n");
        ScopeProcessor p;
        CHECK (p.getTimeIndex() == ScopeProcessor::defaultTimeIndex && ScopeProcessor::timeNames()[p.getTimeIndex()] == "1/4", "1/4 par défaut");
        CHECK (p.getView() == ScopeProcessor::overlay, "superposé par défaut");
        CHECK (p.getChannel() == osc::Channel::mid, "mid par défaut");
        CHECK (std::abs (p.getGainLinear() - 1.0f) < 1e-6f, "gain 0 dB");
        CHECK (! p.isFrozen(), "pas figé");
        CHECK (p.getLatencySamples() == 0, "aucune latence ajoutée");
        CHECK (p.getSlot() != nullptr, "case dans le hub");

        SyncProcessor s;
        CHECK (s.getHub() == p.getHub(), "même hub pour les deux plugins");
        CHECK (s.getLatencySamples() == 0, "Sync : aucune latence ajoutée");

        // Mono
        juce::AudioProcessor::BusesLayout mono;
        mono.inputBuses.add (juce::AudioChannelSet::mono());
        mono.outputBuses.add (juce::AudioChannelSet::mono());
        CHECK (p.checkBusesLayoutSupported (mono) && s.checkBusesLayoutSupported (mono), "mono accepté");
    }

    void testLatency()
    {
        std::printf ("Latence mesurée entre Sync et Oscilloscope\n");
        Track bass, kick, pad;
        bass.seed = 1; kick.seed = 2; pad.seed = 3;
        bass.prepare (true); kick.prepare (true); pad.prepare (false);
        bass.setLag (2048);
        kick.setLag (0);
        pad.setLag (700);

        Session session;
        session.tracks = { &bass, &kick, &pad };
        session.run (2.5);

        CHECK (latencyOf (bass) == 2048, "basse : %d", latencyOf (bass));
        CHECK (latencyOf (kick) == 0 && (osc::LatencyState) kick.scope->getSlot()->latencyState.load() == osc::LatencyState::locked,
               "kick : %d", latencyOf (kick));
        CHECK ((osc::LatencyState) pad.scope->getSlot()->latencyState.load() == osc::LatencyState::searching, "pad sans Sync : en recherche");

        // On ajoute un plugin à latence sur le kick : la mesure suit
        kick.setLag (4410);
        session.run (2.0);
        CHECK (latencyOf (kick) == 4410, "kick après ajout d'un plugin : %d", latencyOf (kick));

        // Le Sync du kick est retiré : la dernière latence est gardée
        kick.sync.reset();
        session.run (1.5);
        CHECK (latencyOf (kick) == 4410, "Sync retiré : %d", latencyOf (kick));

        // Le fil de fond mesure aussi tout seul (sans measureNow)
        Track lead;
        lead.seed = 9;
        lead.prepare (true);
        lead.setLag (333);
        Session background;
        background.tracks = { &lead };
        background.run (0.2, false);
        for (int i = 0; i < 60 && latencyOf (lead) != 333; ++i)
        {
            background.run (0.1, false);
            juce::Thread::sleep (50);
        }
        CHECK (latencyOf (lead) == 333, "mesure par le fil de fond : %d", latencyOf (lead));

        // La fenêtre du Sync voit l'Oscilloscope qui le suit
        SyncEditor editor (*lead.sync);
        const auto lines = editor.describe();
        CHECK (lines.size() == 1 && lines[0].contains ("333 smp"), "fenêtre du Sync : %s", lines.joinIntoString (" / ").toRawUTF8());
        snapshot (editor, "sync");
    }

    void testColoursAndState()
    {
        std::printf ("Couleurs et état\n");
        int count = 0;
        const uint32_t* palette = osc::palette (count);

        auto a = std::make_unique<ScopeProcessor>();
        auto b = std::make_unique<ScopeProcessor>();
        const uint32_t ca = a->getSlot()->colour.load(), cb = b->getSlot()->colour.load();
        CHECK (ca != cb, "deux instances : deux couleurs");

        // Réglages + état
        setParam (b->apvts, ScopeProcessor::timeId, 5.0f);
        setParam (b->apvts, ScopeProcessor::viewId, 2.0f);
        setParam (b->apvts, ScopeProcessor::gainId, 6.0f);
        b->getSlot()->colour.store (0xff123456);
        b->getSlot()->setFlag (osc::flags::manualColour, true);
        b->getSlot()->setFlag (osc::flags::hidden, true);
        osc::Slot::writeText (b->getSlot()->name, "Bass Sub");

        juce::MemoryBlock state;
        b->getStateInformation (state);
        b.reset();

        auto c = std::make_unique<ScopeProcessor>();
        c->setStateInformation (state.getData(), (int) state.getSize());
        CHECK (c->getTimeIndex() == 5 && c->getView() == ScopeProcessor::sum, "réglages relus");
        CHECK (std::abs (c->apvts.getRawParameterValue (ScopeProcessor::gainId)->load() - 6.0f) < 0.01f, "gain relu");
        CHECK (c->getSlot()->colour.load() == 0xff123456, "couleur choisie relue");
        CHECK (c->getSlot()->hasFlag (osc::flags::hidden), "masquée relue");
        CHECK (osc::displayName (*c->getHub(), c->getSlotIndex()) == "Bass Sub", "nom relu");

        // Piste dupliquée : l'état d'une instance à couleur automatique, chargé dans une nouvelle
        juce::MemoryBlock stateA;
        a->getStateInformation (stateA);
        auto d = std::make_unique<ScopeProcessor>();
        d->setStateInformation (stateA.getData(), (int) stateA.getSize());
        CHECK (d->getSlot()->colour.load() != a->getSlot()->colour.load(), "copie : couleur différente");

        // États abîmés
        ScopeProcessor e;
        e.setStateInformation ("garbage", 7);
        juce::MemoryBlock empty;
        e.setStateInformation (empty.getData(), 0);
        CHECK (e.getTimeIndex() == ScopeProcessor::defaultTimeIndex, "état abîmé ignoré");

        // Nom de piste donné par l'hôte
        juce::AudioProcessor::TrackProperties props;
        props.name = juce::String ("3-PHRGM-BASSLINESERUM");
        e.updateTrackProperties (props);
        CHECK (osc::displayName (*e.getHub(), e.getSlotIndex()) == "3-PHRGM-BASSLINESERUM", "nom de la piste");

        // Nom trop long, en UTF-8 : coupé proprement
        osc::Slot::writeText (e.getSlot()->name, juce::String::repeatedString (juce::String (juce::CharPointer_UTF8 ("\xc3\xa9")), 40).toRawUTF8());
        const auto n = osc::displayName (*e.getHub(), e.getSlotIndex());
        CHECK (n.length() == 31 && n.toUTF8().sizeInBytes() == 63, "nom UTF-8 coupé à %d caractères", n.length());
        (void) palette;
    }

    void testEditor()
    {
        std::printf ("Interface\n");
        Track bass, lead;
        bass.seed = 4; lead.seed = 4;   // même son : la compensation doit les superposer exactement
        bass.prepare (true); lead.prepare (true);
        bass.setLag (0);
        lead.setLag (1500);

        juce::AudioProcessor::TrackProperties props;
        props.name = juce::String ("1-BASS");
        bass.scope->updateTrackProperties (props);
        bass.sync->updateTrackProperties (props);
        props.name = juce::String ("2-BASS LIMITED");
        lead.scope->updateTrackProperties (props);
        lead.sync->updateTrackProperties (props);

        Session session;
        session.tracks = { &bass, &lead };
        session.run (2.5);
        CHECK (latencyOf (lead) == 1500, "latence %d", latencyOf (lead));

        std::unique_ptr<juce::AudioProcessorEditor> base (bass.scope->createEditor());
        auto* editor = dynamic_cast<ScopeEditor*> (base.get());
        CHECK (editor != nullptr, "éditeur");
        if (editor == nullptr)
            return;

        // Les traces n'apparaissent que si leur instance a traité du son il y a moins de 400 ms : on fait tourner
        // un peu de son avant chaque image (les captures d'écran prennent du temps)
        auto fresh = [&] (ScopeEditor* e) { session.run (0.02, false); e->tick(); };

        CHECK (editor->getWidth() == ScopeEditor::defaultWidth && editor->getHeight() == ScopeEditor::defaultHeight, "taille par défaut");
        fresh (editor);
        auto& view = editor->getScopeView();
        CHECK (view.getTraces().size() == 2, "deux traces : %d", (int) view.getTraces().size());
        CHECK (editor->getTrackList().getTracks().size() == 2, "deux lignes dans la liste");

        // Superposées après compensation : mêmes colonnes
        if (view.getTraces().size() == 2)
        {
            const auto& t0 = view.getTraces()[0].columns;
            const auto& t1 = view.getTraces()[1].columns;
            float diff = 0.0f;
            int valid = 0;
            for (size_t i = 0; i < t0.size(); ++i)
                if (t0[i].valid && t1[i].valid)
                {
                    ++valid;
                    diff = std::max ({ diff, std::abs (t0[i].lo - t1[i].lo), std::abs (t0[i].hi - t1[i].hi) });
                }
            CHECK (valid > (int) t0.size() * 9 / 10 && diff < 1e-5f, "traces compensées identiques (%d colonnes, écart %g)", valid, diff);
            CHECK (view.getTraces().back().own, "la trace de la fenêtre est dessinée en dernier");
        }

        const auto& win = view.getWindow();
        CHECK (std::abs ((double) win.length - sampleRate * 60.0 / bpm) < 1.0, "fenêtre d'un temps (1/4) : %lld", (long long) win.length);
        snapshot (*editor, "scope-overlay");

        // Somme
        setParam (bass.scope->apvts, ScopeProcessor::viewId, 2.0f);
        fresh (editor);
        CHECK (view.getTraces().size() == 1 && view.getTraces()[0].name == "SUM", "somme : une trace");
        snapshot (*editor, "scope-sum");

        // Séparé + 1/16
        setParam (bass.scope->apvts, ScopeProcessor::viewId, 1.0f);
        setParam (bass.scope->apvts, ScopeProcessor::timeId, 1.0f);
        fresh (editor);
        CHECK (view.getTraces().size() == 2, "séparé : deux traces");
        CHECK (std::abs ((double) view.getWindow().length - sampleRate * 60.0 / bpm / 4.0) < 1.0, "1/16");
        snapshot (*editor, "scope-split");

        // Masquer la deuxième trace
        editor->toggleHidden (1);
        fresh (editor);
        CHECK (view.getTraces().size() == 1, "trace masquée (%d traces, %d lignes, masquée %d)", (int) view.getTraces().size(),
               (int) editor->getTrackList().getTracks().size(), (int) lead.scope->getSlot()->hasFlag (osc::flags::hidden));
        editor->toggleHidden (1);

        // Renommer
        const auto& rows = editor->getTrackList().getTracks();
        editor->rename (rows[0].index, rows[0].generation, "Sub");
        fresh (editor);
        CHECK (osc::displayName (*bass.scope->getHub(), bass.scope->getSlotIndex()) == "Sub", "renommé");

        // Figer : l'image ne bouge plus pendant que le son avance
        setParam (bass.scope->apvts, ScopeProcessor::viewId, 0.0f);
        setParam (bass.scope->apvts, ScopeProcessor::timeId, 3.0f);
        fresh (editor);
        setParam (bass.scope->apvts, ScopeProcessor::freezeId, 1.0f);
        fresh (editor);
        const auto frozenWindow = view.getWindow().now;
        session.run (0.5);
        editor->tick();
        CHECK (view.getWindow().now == frozenWindow, "figé (%lld -> %lld, param %d)", (long long) frozenWindow, (long long) view.getWindow().now, (int) bass.scope->isFrozen());
        setParam (bass.scope->apvts, ScopeProcessor::freezeId, 0.0f);
        fresh (editor);
        CHECK (view.getWindow().now > frozenWindow, "reprise");

        // Redimensionnement gardé dans l'état
        editor->setSize (1400, 700);
        juce::MemoryBlock state;
        bass.scope->getStateInformation (state);
        ScopeProcessor reloaded;
        reloaded.setStateInformation (state.getData(), (int) state.getSize());
        CHECK ((int) reloaded.apvts.state.getProperty ("editorWidth") == 1400 && (int) reloaded.apvts.state.getProperty ("editorHeight") == 700, "taille gardée");

        // Une fenêtre ouverte sur la deuxième instance montre aussi les deux
        std::unique_ptr<juce::AudioProcessorEditor> other (lead.scope->createEditor());
        if (auto* e2 = dynamic_cast<ScopeEditor*> (other.get()))
        {
            fresh (e2);
            CHECK (e2->getScopeView().getTraces().size() == 2, "deuxième fenêtre : deux traces");
            snapshot (*e2, "scope-second-window");
        }

        // Transport arrêté : l'affichage continue
        session.head.playing = false;
        session.run (0.3);
        fresh (editor);
        CHECK (view.getTraces().size() == 2 && view.hasWindow(), "à l'arrêt : toujours affiché");

        // Valeurs non finies à l'entrée : rien ne casse
        bass.seed = 1;
        juce::AudioBuffer<float> bad (2, blockSize);
        for (int i = 0; i < blockSize; ++i) { bad.setSample (0, i, std::numeric_limits<float>::infinity()); bad.setSample (1, i, std::nanf ("")); }
        juce::MidiBuffer midi;
        bass.scope->processBlock (bad, midi);
        fresh (editor);
        CHECK (true, "valeurs non finies");
    }
    //==========================================================================
    void testInstrumentHost()
    {
        std::printf ("Oscilloscope Sync Instrument\n");
        const auto synthFile = juce::File (OSC_TEST_SYNTH_DIR).getChildFile ("OscTestSynth.vst3");
        CHECK (synthFile.exists(), "instrument de test introuvable : %s", synthFile.getFullPathName().toRawUTF8());

        auto host = std::make_unique<HostProcessor>();
        host->setRateAndBufferSizeDetails (sampleRate, blockSize);
        host->prepareToPlay (sampleRate, blockSize);
        CHECK (host->getLatencySamples() == 0 && host->getInner() == nullptr, "vide au départ");

        juce::String error;
        CHECK (host->loadFile (synthFile, error), "chargement : %s", error.toRawUTF8());
        CHECK (host->getInnerName() == "OscTestSynth", "nom %s", host->getInnerName().toRawUTF8());
        CHECK (host->getInnerLatency() == 110 && host->getLatencySamples() == 110, "latence déclarée %d / %d", host->getInnerLatency(), host->getLatencySamples());
        CHECK (host->getHub()->slots[host->getSlotIndex()].sourceLatency.load() == 110, "latence dans le hub");
        CHECK (host->getParameters()[0]->getName (64) == "Gain" && host->getParameters()[1]->getName (64) == "Latency"
               && host->getParameters()[2]->getName (64) == "P3", "noms des paramètres relayés : %s, %s, %s (%d dans l instrument)", host->getParameters()[0]->getName (64).toRawUTF8(), host->getParameters()[1]->getName (64).toRawUTF8(), host->getParameters()[2]->getName (64).toRawUTF8(), host->getInner()->getParameters().size());

        // On ne peut pas se charger soi-même
        {
            juce::PluginDescription self;
            self.name = "Oscilloscope Sync Instrument";
            self.manufacturerName = "Phragma";
            juce::String e2;
            CHECK (! host->loadPlugin (self, e2) && host->getInnerName() == "OscTestSynth", "refus de se charger soi-même");
        }

        // Piste : [Sync Instrument (instrument de test)] -> effet qui retarde de 300 -> [Oscilloscope]
        auto scope = std::make_unique<ScopeProcessor>();
        scope->setRateAndBufferSizeDetails (sampleRate, blockSize);
        scope->prepareToPlay (sampleRate, blockSize);
        FakePlayHead head;
        host->setPlayHead (&head);
        scope->setPlayHead (&head);

        std::vector<float> line (2048, 0.0f);
        size_t linePos = 0;
        int fxLag = 300;
        const double sixteenth = sampleRate * 60.0 / bpm / 4.0;
        int noteCounter = 0;

        auto run = [&] (double seconds)
        {
            double since = 0.0;
            for (int b = 0; b < (int) (seconds * sampleRate / blockSize); ++b)
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                juce::MidiBuffer midi;
                for (int i = 0; i < blockSize; ++i)
                {
                    const int64_t at = head.position + i;
                    // un kick à chaque temps, des notes en plus sur certaines doubles-croches
                    const int64_t step = (int64_t) std::floor ((double) at / sixteenth);
                    if ((int64_t) std::floor ((double) (at - 1) / sixteenth) != step && (step % 4 == 0 || (step * 7 + noteCounter) % 5 == 0))
                    {
                        midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), i);
                        ++noteCounter;
                    }
                }
                host->processBlock (buffer, midi);
                for (int i = 0; i < blockSize; ++i)
                    for (int c = 0; c < 2; ++c)
                    {
                        float& cell = line[((linePos + (size_t) i) % (size_t) fxLag) * 1];
                        if (c == 0)
                        {
                            const float out = cell;
                            cell = buffer.getSample (0, i);
                            buffer.setSample (0, i, out);
                        }
                        else
                            buffer.setSample (1, i, buffer.getSample (0, i));
                    }
                linePos = (linePos + blockSize) % (size_t) fxLag;
                scope->processBlock (buffer, midi);
                head.position += blockSize;
                since += (double) blockSize / sampleRate;
                if (since >= 0.2)
                {
                    since = 0.0;
                    host->refreshLatencyNow();
                    scope->measureNow();
                }
            }
        };

        run (3.0);
        CHECK (scope->getSlot()->latency.load() == 410, "kick 110 + effet 300 : %d (état %d)", scope->getSlot()->latency.load(), scope->getSlot()->latencyState.load());
        CHECK (scope->getSlot()->pairedSourceLatency.load() == 110, "part de l'instrument");

        // L'instrument change de latence (réglage interne) : relayé par le paramètre P2
        auto* latencyProxy = host->getParameters()[1];
        auto* innerLatencyParam = host->getInner()->getParameters()[1];
        latencyProxy->setValueNotifyingHost (innerLatencyParam->getValueForText ("500"));
        run (0.6);
        CHECK (host->getLatencySamples() == 500, "nouvelle latence déclarée : %d", host->getLatencySamples());
        CHECK (scope->getSlot()->latency.load() == 800, "500 + 300 : %d", scope->getSlot()->latency.load());

        // Un réglage tourné dans la fenêtre de l'instrument remonte vers l'hôte (automation)
        {
            struct Watch final : juce::AudioProcessorParameter::Listener
            {
                float last = -1.0f;
                void parameterValueChanged (int, float v) override   { last = v; }
                void parameterGestureChanged (int, bool) override   {}
            } watch;
            auto* gainProxy = host->getParameters()[0];
            gainProxy->addListener (&watch);
            host->getInner()->getParameters()[0]->setValueNotifyingHost (0.25f);
            CHECK (std::abs (gainProxy->getValue() - 0.25f) < 1e-4f && std::abs (watch.last - 0.25f) < 1e-4f, "geste relayé (%g, %g)", gainProxy->getValue(), watch.last);
            // l'écho de l'hôte ne repart pas vers lui
            watch.last = -1.0f;
            gainProxy->setValue (0.25f);
            run (0.05);
            CHECK (watch.last < 0.0f, "pas d'écho");
            gainProxy->removeListener (&watch);
        }

        // Fenêtre : l'instrument est dedans
        {
            std::unique_ptr<juce::AudioProcessorEditor> base (host->createEditor());
            auto* editor = dynamic_cast<HostEditor*> (base.get());
            CHECK (editor != nullptr && editor->getInnerEditor() != nullptr, "fenêtre de l'instrument intégrée");
            if (editor != nullptr && editor->getInnerEditor() != nullptr)
            {
                CHECK (editor->getHeight() == HostEditor::headerHeight + editor->getInnerEditor()->getHeight(), "hauteur");
                const auto lines = editor->describe();
                CHECK (lines.size() == 1 && lines[0].contains ("800 smp"), "suivi : %s", lines.joinIntoString (" / ").toRawUTF8());
                snapshot (*editor, "sync-instrument");

                // Changer d'instrument avec la fenêtre ouverte
                juce::String e3;
                CHECK (host->loadFile (synthFile, e3), "rechargement fenêtre ouverte");
                CHECK (editor->getInnerEditor() != nullptr && dynamic_cast<juce::AudioProcessorEditor*> (editor->getInnerEditor())->getAudioProcessor() == host->getInner(), "fenêtre reconstruite");
            }
        }

        // État : instrument + ses réglages
        host->getInner()->getParameters()[1]->setValueNotifyingHost (innerLatencyParam == nullptr ? 0.0f : host->getInner()->getParameters()[1]->getValueForText ("777"));
        run (0.1);
        juce::MemoryBlock state;
        host->getStateInformation (state);

        HostProcessor reloaded;
        reloaded.setRateAndBufferSizeDetails (sampleRate, blockSize);
        reloaded.prepareToPlay (sampleRate, blockSize);
        reloaded.setStateInformation (state.getData(), (int) state.getSize());
        CHECK (reloaded.getInnerName() == "OscTestSynth" && reloaded.getLatencySamples() == 777, "état relu : %s, %d",
               reloaded.getInnerName().toRawUTF8(), reloaded.getLatencySamples());

        // Instrument introuvable : message, et l'état est gardé tel quel
        {
            auto text = juce::String::fromUTF8 ((const char*) state.getData() + 8, (int) state.getSize() - 8);
            auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
            CHECK (xml != nullptr, "xml");
            if (xml != nullptr)
            {
                if (auto* plugin = xml->getChildByName ("PLUGIN"))
                    plugin->setAttribute ("file", "/nowhere/Missing.vst3");
                juce::MemoryBlock broken;
                juce::AudioProcessor::copyXmlToBinary (*xml, broken);
                HostProcessor lost;
                lost.setStateInformation (broken.getData(), (int) broken.getSize());
                CHECK (lost.getInner() == nullptr && lost.getLoadError().isNotEmpty(), "message : %s", lost.getLoadError().toRawUTF8());
                juce::MemoryBlock again;
                lost.getStateInformation (again);
                auto xml2 = juce::AudioProcessor::getXmlFromBinary (again.getData(), (int) again.getSize());
                CHECK (xml2 != nullptr && xml2->getChildByName ("PLUGIN") != nullptr
                       && xml2->getChildByName ("PLUGIN")->getStringAttribute ("file") == "/nowhere/Missing.vst3", "état gardé");
            }
            (void) text;
        }

        // Retirer l'instrument
        host->unloadPlugin();
        CHECK (host->getInner() == nullptr && host->getLatencySamples() == 0 && host->getParameters()[0]->getName (64) == "P1", "retiré");
        run (0.1);   // silence, rien ne casse

        // Liste des .vst3 (dossier supplémentaire des tests)
        const auto files = HostProcessor::findVst3Files();
        bool listed = false;
        for (auto& f : files)
            listed = listed || f.getFileName() == "OscTestSynth.vst3";
        CHECK (listed, "instrument de test dans la liste (%d fichiers)", files.size());
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    setenv ("OSC_EXTRA_VST3_DIR", OSC_TEST_SYNTH_DIR, 1);

    testParametersAndDefaults();
    testLatency();
    testColoursAndState();
    testEditor();
    testInstrumentHost();

    std::printf ("\n%d vérifications, %d échec(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
