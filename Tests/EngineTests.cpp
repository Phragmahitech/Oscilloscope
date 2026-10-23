// Tests du moteur (sans JUCE) : hub, écriture, mesure de latence, lecture des traces.

#include "Hub.h"
#include "Tap.h"
#include "LatencyFinder.h"
#include "Trace.h"

#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <chrono>
#include <limits>

using namespace osc;

static int failures = 0, checks = 0;

#define CHECK(cond, ...) do { ++checks; if (! (cond)) { ++failures; std::printf ("  ECHEC (%s:%d) ", __FILE__, __LINE__); std::printf (__VA_ARGS__); std::printf ("\n"); } } while (0)

namespace
{
    constexpr double pi = 3.14159265358979323846;

    /** Boucle façon hi-tech : kick, basse roulante (notes au hasard), charley de bruit, zaps. */
    std::vector<float> music (double sr, double seconds, uint32_t seed, double bpm = 145.0)
    {
        std::mt19937 rng (seed);
        std::uniform_real_distribution<float> uni (-1.0f, 1.0f);
        const int n = (int) (sr * seconds);
        std::vector<float> out ((size_t) n, 0.0f);
        const double beat = 60.0 / bpm * sr, sixteenth = beat / 4.0;
        const double notes[] = { 41.2, 43.65, 46.25, 49.0, 55.0, 61.7 };

        double bassPhase = 0.0, bassFreq = 41.2, lp = 0.0;
        double zapPhase = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double inBeat = std::fmod ((double) i, beat);
            const int sixteenthIndex = (int) ((double) i / sixteenth);
            const double inSixteenth = std::fmod ((double) i, sixteenth);

            // kick
            const double tk = inBeat / sr;
            const double kickPhase = 2.0 * pi * (50.0 * tk + (160.0 - 50.0) * 0.02 * (1.0 - std::exp (-tk / 0.02)));
            double s = 0.9 * std::sin (kickPhase) * std::exp (-tk / 0.12);

            // basse : 3 doubles-croches sur 4 (pas sur le temps), note tirée au hasard
            if (inSixteenth < 1.0)
                bassFreq = notes[(std::hash<int>() (sixteenthIndex * 7919 + (int) seed)) % 6];
            if (sixteenthIndex % 4 != 0)
            {
                bassPhase += bassFreq / sr;
                bassPhase -= std::floor (bassPhase);
                const double saw = 2.0 * bassPhase - 1.0;
                lp += 0.08 * (saw - lp);
                s += 0.5 * lp * std::exp (-(inSixteenth / sr) / 0.06);
            }

            // charley
            if (sixteenthIndex % 2 == 1)
                s += 0.15 * uni (rng) * std::exp (-(inSixteenth / sr) / 0.015);

            // zap toutes les 7 doubles-croches
            if (sixteenthIndex % 7 == 3)
            {
                const double tz = inSixteenth / sr;
                zapPhase += (3000.0 * std::exp (-tz / 0.01) + 200.0) / sr;
                s += 0.2 * std::sin (2.0 * pi * zapPhase) * std::exp (-tz / 0.04);
            }

            out[(size_t) i] = (float) s;
        }
        return out;
    }

    std::vector<float> delayed (const std::vector<float>& x, int lag)
    {
        std::vector<float> y (x.size(), 0.0f);
        for (size_t i = (size_t) lag; i < x.size(); ++i)
            y[i] = x[i - (size_t) lag];
        return y;
    }

    /** Biquad (RBJ). type : 0 passe-bas, 1 passe-haut, 2 cloche. */
    std::vector<float> biquad (const std::vector<float>& x, double sr, int type, double f, double q, double gainDb = 0.0)
    {
        const double w = 2.0 * pi * f / sr, c = std::cos (w), al = std::sin (w) / (2.0 * q);
        const double A = std::pow (10.0, gainDb / 40.0);
        double b0, b1, b2, a0, a1, a2;
        if (type == 0)      { b0 = (1 - c) / 2; b1 = 1 - c; b2 = (1 - c) / 2; a0 = 1 + al; a1 = -2 * c; a2 = 1 - al; }
        else if (type == 1) { b0 = (1 + c) / 2; b1 = -(1 + c); b2 = (1 + c) / 2; a0 = 1 + al; a1 = -2 * c; a2 = 1 - al; }
        else                { b0 = 1 + al * A; b1 = -2 * c; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * c; a2 = 1 - al / A; }
        std::vector<float> y (x.size());
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double v = (b0 * x[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
            x2 = x1; x1 = x[i]; y2 = y1; y1 = v;
            y[i] = (float) v;
        }
        return y;
    }

    std::vector<float> distort (const std::vector<float>& x, float drive)
    {
        std::vector<float> y (x.size());
        for (size_t i = 0; i < x.size(); ++i) y[i] = std::tanh (drive * x[i]) / std::tanh (drive);
        return y;
    }

    /** Petite réverbération (quatre peignes en parallèle) mélangée à 35 %. */
    std::vector<float> reverb (const std::vector<float>& x, double sr)
    {
        const int lens[] = { (int) (0.0297 * sr), (int) (0.0371 * sr), (int) (0.0411 * sr), (int) (0.0437 * sr) };
        std::vector<float> wet (x.size(), 0.0f);
        for (int L : lens)
        {
            std::vector<float> buf ((size_t) L, 0.0f);
            size_t p = 0;
            for (size_t i = 0; i < x.size(); ++i)
            {
                const float out = buf[p];
                buf[p] = x[i] + 0.78f * out;
                p = (p + 1) % (size_t) L;
                wet[i] += 0.25f * out;
            }
        }
        std::vector<float> y (x.size());
        for (size_t i = 0; i < x.size(); ++i) y[i] = 0.65f * x[i] + 0.35f * wet[i];
        return y;
    }

    Estimate estimateOn (const std::vector<float>& x, const std::vector<float>& y, double sr, size_t at, int maxLag)
    {
        LagEstimator est;
        const int ny = (int) (0.35 * sr);
        return est.estimate (x.data() + at - (size_t) maxLag, y.data() + at, ny, maxLag, sr);
    }

    //==========================================================================
    void testEstimator()
    {
        std::printf ("Estimation du retard (signal type)\n");
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            const auto x = music (sr, 4.0, 7);
            const int maxLag = (int) sr;
            const size_t at = (size_t) (2.5 * sr);

            for (int lag : { 0, 1, 7, 64, 512, 1023, 2048, 4800, (int) (0.3 * sr), maxLag - 1, maxLag })
            {
                const auto e = estimateOn (x, delayed (x, lag), sr, at, maxLag);
                CHECK (e.valid && e.lag == lag, "sr %.0f, retard %d : trouvé %d (valide %d, pic %.3f, rapport %.2f)", sr, lag, e.lag, (int) e.valid, e.peak, e.ratio);
            }
        }

        std::printf ("Estimation à travers des traitements (48 kHz)\n");
        const double sr = 48000.0;
        const auto x = music (sr, 4.0, 11);
        const size_t at = (size_t) (2.5 * sr);
        const int maxLag = 48000;

        struct Case { const char* name; std::function<std::vector<float> (const std::vector<float>&)> f; int tolerance; };
        const std::vector<Case> cases = {
            { "gain -12 dB", [] (const std::vector<float>& v) { auto y = v; for (auto& s : y) s *= 0.25f; return y; }, 0 },
            { "inversion de phase", [] (const std::vector<float>& v) { auto y = v; for (auto& s : y) s = -s; return y; }, 0 },
            { "passe-bas 800 Hz", [sr] (const std::vector<float>& v) { return biquad (v, sr, 0, 800.0, 0.707); }, 0 },
            { "passe-bas 150 Hz", [sr] (const std::vector<float>& v) { return biquad (v, sr, 0, 150.0, 0.707); }, 0 },
            { "passe-haut 120 Hz", [sr] (const std::vector<float>& v) { return biquad (v, sr, 1, 120.0, 0.707); }, 0 },
            { "passe-haut 40 Hz", [sr] (const std::vector<float>& v) { return biquad (v, sr, 1, 40.0, 0.707); }, 0 },
            { "cloche +9 dB 3 kHz", [sr] (const std::vector<float>& v) { return biquad (v, sr, 2, 3000.0, 1.0, 9.0); }, 0 },
            { "cloche +9 dB 100 Hz", [sr] (const std::vector<float>& v) { return biquad (v, sr, 2, 100.0, 2.0, 9.0); }, 0 },
            { "saturation", [] (const std::vector<float>& v) { return distort (v, 6.0f); }, 0 },
            { "réverbération", [sr] (const std::vector<float>& v) { return reverb (v, sr); }, 0 },
            { "EQ + saturation + réverb", [sr] (const std::vector<float>& v) { return reverb (distort (biquad (v, sr, 1, 60.0, 0.707), 3.0f), sr); }, 0 },
            // 24 dB/oct à 150 Hz : on accepte de ne rien trouver (la dernière valeur est gardée), pas de se tromper de plus de 2
            { "passe-bas 150 Hz 24 dB/oct", [sr] (const std::vector<float>& v) { return biquad (biquad (v, sr, 0, 150.0, 0.707), sr, 0, 150.0, 0.707); }, -2 },
        };

        for (auto& c : cases)
        {
            for (int lag : { 0, 333, 2048, 12000 })
            {
                const auto y = c.f (delayed (x, lag));
                const auto e = estimateOn (x, y, sr, at, maxLag);
                if (c.tolerance < 0)   // cas difficile : pas de résultat, ou un résultat juste à |tolérance| près
                {
                    CHECK (! e.valid || std::abs (e.lag - lag) <= -c.tolerance, "%s, retard %d : faux résultat %d", c.name, lag, e.lag);
                    continue;
                }
                CHECK (e.valid && std::abs (e.lag - lag) <= c.tolerance, "%s, retard %d : trouvé %d (valide %d, pic %.3f, rapport %.2f)",
                       c.name, lag, e.lag, (int) e.valid, e.peak, e.ratio);
                if (e.valid && e.lag != lag)
                    std::printf ("  (info) %s, retard %d : %d\n", c.name, lag, e.lag);
            }
        }

        std::printf ("Signaux sans réponse sûre\n");
        {
            // Sinus pur : retard ambigu (une période près) -> pas de résultat
            std::vector<float> sine ((size_t) (4 * sr));
            for (size_t i = 0; i < sine.size(); ++i) sine[i] = (float) std::sin (2.0 * pi * 110.0 * (double) i / sr);
            const auto e = estimateOn (sine, delayed (sine, 1000), sr, at, maxLag);
            CHECK (! e.valid || e.lag == 1000, "sinus pur : faux résultat %d (pic %.3f, rapport %.2f)", e.lag, e.peak, e.ratio);

            // Deux musiques sans rapport
            const auto other = music (sr, 4.0, 99, 138.0);
            int falsePositives = 0;
            for (int k = 0; k < 6; ++k)
            {
                const auto e2 = estimateOn (x, other, sr, at - (size_t) (k * 4000), maxLag);
                if (e2.valid) ++falsePositives;
            }
            CHECK (falsePositives == 0, "musiques sans rapport : %d résultats inventés sur 6", falsePositives);

            // Bruit blanc indépendant
            std::mt19937 rng (3);
            std::normal_distribution<float> g (0.0f, 0.2f);
            std::vector<float> n1 (x.size()), n2 (x.size());
            for (size_t i = 0; i < x.size(); ++i) { n1[i] = g (rng); n2[i] = g (rng); }
            const auto e3 = estimateOn (n1, n2, sr, at, maxLag);
            CHECK (! e3.valid, "bruits indépendants : résultat inventé %d", e3.lag);

            // Bruit retardé : très net
            const auto e4 = estimateOn (n1, delayed (n1, 777), sr, at, maxLag);
            CHECK (e4.valid && e4.lag == 777, "bruit retardé : %d", e4.lag);

            // Silence
            std::vector<float> silence (x.size(), 0.0f);
            const auto e5 = estimateOn (silence, silence, sr, at, maxLag);
            CHECK (! e5.valid && e5.silent, "silence");

            // Signal très faible (-80 dBFS) mais retardé : on le mesure quand même
            auto quiet = x;
            for (auto& s : quiet) s *= 1.0e-4f;
            const auto e6 = estimateOn (quiet, delayed (quiet, 300), sr, at, maxLag);
            CHECK (e6.valid && e6.lag == 300, "signal à -80 dBFS : %d (valide %d)", e6.lag, (int) e6.valid);
        }
    }

    //==========================================================================
    /** Une piste simulée : un Sync, des plugins (retard + traitement), un Oscilloscope. */
    struct Track
    {
        int syncIndex = -1, scopeIndex = -1;
        Tap syncTap, scopeTap;
        std::vector<float> source;
        int lag = 0;
        std::vector<float> line;      // ligne à retard des « plugins »
        size_t linePos = 0;
        LatencyMeasurer measurer;

        void setLag (int newLag)
        {
            lag = newLag;
            line.assign ((size_t) std::max (1, lag), 0.0f);
            linePos = 0;
        }
    };

    struct World
    {
        Hub* hub = createHub();   // mémoire du système, jamais rendue
        double sr = 48000.0;
        int block = 256;
        int64_t pos = 0;
        bool playing = true;
        int64_t clockNs = nowNs();

        void addTrack (Track& t, std::vector<float> source, int lag, bool withSync = true)
        {
            if (withSync)
            {
                t.syncIndex = hub->claim (Kind::sync, 2);
                t.syncTap.attach (hub, t.syncIndex);
                t.syncTap.prepare (sr);
            }
            t.scopeIndex = hub->claim (Kind::scope, 2);
            t.scopeTap.attach (hub, t.scopeIndex);
            t.scopeTap.prepare (sr);
            t.source = std::move (source);
            t.setLag (lag);
        }

        void run (std::vector<Track*> tracks, double seconds, bool measureEachBlock = false)
        {
            const int blocks = (int) (seconds * sr / block);
            std::vector<float> in ((size_t) block), out ((size_t) block);
            int sinceMeasure = 0;

            for (int b = 0; b < blocks; ++b)
            {
                Transport t;
                t.playing = playing;
                t.hasSamplePosition = playing;
                t.samplePosition = pos;
                t.hasPpq = playing;
                t.ppq = (double) pos / (sr * 60.0 / 145.0);
                t.bpm = 145.0;

                for (auto* tr : tracks)
                {
                    for (int i = 0; i < block; ++i)
                    {
                        const auto srcIndex = (size_t) ((tr->source.size() + (size_t) ((pos + i) % (int64_t) tr->source.size())) % tr->source.size());
                        in[(size_t) i] = tr->source[srcIndex];
                    }
                    if (tr->syncIndex >= 0)
                        tr->syncTap.process (in.data(), in.data(), block, t, clockNs);
                    for (int i = 0; i < block; ++i)
                    {
                        if (tr->lag == 0) { out[(size_t) i] = in[(size_t) i]; continue; }
                        out[(size_t) i] = tr->line[tr->linePos];
                        tr->line[tr->linePos] = in[(size_t) i];
                        tr->linePos = (tr->linePos + 1) % tr->line.size();
                    }
                    tr->scopeTap.process (out.data(), out.data(), block, t, clockNs + 1000);
                }

                pos += block;
                clockNs += (int64_t) (1.0e9 * block / sr);

                sinceMeasure += block;
                if (measureEachBlock || sinceMeasure >= (int) (0.2 * sr))
                {
                    sinceMeasure = 0;
                    for (auto* tr : tracks)
                        tr->measurer.run (*hub, tr->scopeIndex);
                }
            }
        }
    };

    int latencyOf (World& w, const Track& t)      { return w.hub->slots[t.scopeIndex].latency.load(); }
    LatencyState stateOf (World& w, const Track& t) { return (LatencyState) w.hub->slots[t.scopeIndex].latencyState.load(); }

    void testEndToEnd()
    {
        std::printf ("Mesure de bout en bout (hub, anneaux, suivi)\n");
        World w;
        Track a;
        w.addTrack (a, music (w.sr, 6.0, 21), 2048);
        w.run ({ &a }, 2.0);
        CHECK (latencyOf (w, a) == 2048 && stateOf (w, a) == LatencyState::locked, "latence %d, état %d", latencyOf (w, a), (int) stateOf (w, a));
        CHECK (w.hub->slots[a.scopeIndex].pairedSlot.load() == a.syncIndex, "Sync associé");

        // Un plugin est retiré : la latence tombe à 64
        a.setLag (64);
        w.run ({ &a }, 1.5);
        CHECK (latencyOf (w, a) == 64, "après changement : %d", latencyOf (w, a));

        // Plus de plugin à latence
        a.setLag (0);
        w.run ({ &a }, 1.5);
        CHECK (latencyOf (w, a) == 0 && stateOf (w, a) == LatencyState::locked, "sans latence : %d", latencyOf (w, a));

        // Ajout d'un gros plugin (0,5 s)
        a.setLag (24000);
        w.run ({ &a }, 2.5);
        CHECK (latencyOf (w, a) == 24000, "0,5 s : %d", latencyOf (w, a));

        // Boucle courte de l'hôte (1 mesure à 145 BPM = 1,66 s) : la mesure tient entre les sauts
        std::printf ("Lecture en boucle\n");
        {
            World w2;
            Track b;
            w2.addTrack (b, music (w2.sr, 6.0, 5), 1500);
            const int64_t loopLength = (int64_t) (4 * 60.0 / 145.0 * w2.sr);
            for (int k = 0; k < 6; ++k)
            {
                w2.pos = 0;
                w2.run ({ &b }, (double) loopLength / w2.sr);
            }
            CHECK (latencyOf (w2, b) == 1500, "en boucle : %d (état %d)", latencyOf (w2, b), (int) stateOf (w2, b));
        }

        // Silence : on garde la dernière valeur
        std::printf ("Silence\n");
        {
            std::vector<float> silent ((size_t) w.sr * 3, 0.0f);
            a.source = silent;
            w.run ({ &a }, 1.5);
            CHECK (latencyOf (w, a) == 24000 && stateOf (w, a) == LatencyState::noSignal, "silence : %d, état %d", latencyOf (w, a), (int) stateOf (w, a));
        }

        // Compensation coupée à la main
        w.hub->slots[a.scopeIndex].setFlag (flags::latencyOff, true);
        w.run ({ &a }, 0.5);
        CHECK (latencyOf (w, a) == 0 && stateOf (w, a) == LatencyState::off, "OFF");
    }

    void testPairing()
    {
        std::printf ("Plusieurs pistes : chaque Oscilloscope trouve son Sync\n");
        World w;
        Track bass, kick, lead, noSync;
        w.addTrack (bass, music (w.sr, 6.0, 1), 512);
        w.addTrack (kick, music (w.sr, 6.0, 2, 150.0), 4096);
        w.addTrack (lead, music (w.sr, 6.0, 3, 132.0), 0);
        w.addTrack (noSync, music (w.sr, 6.0, 4, 140.0), 900, false);
        w.run ({ &bass, &kick, &lead, &noSync }, 3.0);

        CHECK (latencyOf (w, bass) == 512, "basse : %d", latencyOf (w, bass));
        CHECK (latencyOf (w, kick) == 4096, "kick : %d", latencyOf (w, kick));
        CHECK (latencyOf (w, lead) == 0, "lead : %d", latencyOf (w, lead));
        CHECK (w.hub->slots[bass.scopeIndex].pairedSlot.load() == bass.syncIndex, "basse -> son Sync");
        CHECK (w.hub->slots[kick.scopeIndex].pairedSlot.load() == kick.syncIndex, "kick -> son Sync");
        CHECK (w.hub->slots[lead.scopeIndex].pairedSlot.load() == lead.syncIndex, "lead -> son Sync");
        CHECK (stateOf (w, noSync) == LatencyState::searching && latencyOf (w, noSync) == 0,
               "piste sans Sync : état %d, latence %d", (int) stateOf (w, noSync), latencyOf (w, noSync));

        std::printf ("Noms de pistes donnés par l'hôte\n");
        {
            // Deux pistes au contenu identique : le nom départage
            World w2;
            Track one, two;
            const auto same = music (w2.sr, 6.0, 8);
            w2.addTrack (one, same, 100);
            w2.addTrack (two, same, 3000);
            Slot::writeText (w2.hub->slots[one.syncIndex].trackName, "1-BASS");
            Slot::writeText (w2.hub->slots[one.scopeIndex].trackName, "1-BASS");
            Slot::writeText (w2.hub->slots[two.syncIndex].trackName, "2-BASS COPY");
            Slot::writeText (w2.hub->slots[two.scopeIndex].trackName, "2-BASS COPY");
            w2.run ({ &one, &two }, 2.0);
            CHECK (latencyOf (w2, one) == 100 && latencyOf (w2, two) == 3000, "pistes identiques : %d / %d", latencyOf (w2, one), latencyOf (w2, two));
        }

        std::printf ("Sync supprimé, puis remis\n");
        {
            const int lat = latencyOf (w, kick);
            w.hub->release (kick.syncIndex);
            kick.syncIndex = -1;
            w.run ({ &bass, &kick }, 1.0);
            CHECK (latencyOf (w, kick) == lat && stateOf (w, kick) == LatencyState::searching, "sans Sync, on garde %d : %d (état %d)", lat, latencyOf (w, kick), (int) stateOf (w, kick));

            kick.syncIndex = w.hub->claim (Kind::sync, 2);
            kick.syncTap.attach (w.hub, kick.syncIndex);
            kick.syncTap.prepare (w.sr);
            kick.setLag (256);
            w.run ({ &bass, &kick }, 2.0);
            CHECK (latencyOf (w, kick) == 256, "Sync remis : %d", latencyOf (w, kick));
        }
    }

    //==========================================================================
    void testTapAndClock()
    {
        std::printf ("Écriture : positions, sauts, arrêt\n");
        World w;
        Track a, b;
        std::vector<float> ramp ((size_t) w.sr * 2);
        for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = (float) i / (float) ramp.size();
        w.addTrack (a, ramp, 0, false);
        w.run ({ &a }, 0.5);
        auto& sa = w.hub->slots[a.scopeIndex];
        CHECK (sa.head.load() == w.pos, "tête %lld / %lld", (long long) sa.head.load(), (long long) w.pos);
        CHECK (std::abs (sa.readL (w.pos - 1) - ramp[(size_t) (w.pos - 1)]) < 1e-6f, "contenu");
        CHECK (sa.epochStart.load() == 0, "début");

        // Saut de l'hôte
        w.pos = 100000;
        w.run ({ &a }, 0.1);
        CHECK (sa.epochStart.load() == 100000, "saut : %lld", (long long) sa.epochStart.load());

        // Arrêt : la position continue, sans saut
        const int64_t before = w.pos;
        w.playing = false;
        w.run ({ &a }, 0.2);
        CHECK (sa.epochStart.load() == 100000 && sa.head.load() == before + (int64_t) (0.2 * w.sr / w.block) * w.block,
               "arrêt : début %lld, tête %lld", (long long) sa.epochStart.load(), (long long) sa.head.load());

        // Une instance créée à l'arrêt se cale sur celle qui tourne déjà
        w.addTrack (b, ramp, 0, false);
        w.run ({ &a, &b }, 0.2);
        auto& sb = w.hub->slots[b.scopeIndex];
        CHECK (sb.head.load() == sa.head.load(), "nouvelle instance à l'arrêt : %lld / %lld", (long long) sb.head.load(), (long long) sa.head.load());

        // Valeurs non finies
        Track c;
        std::vector<float> bad ((size_t) w.sr, std::numeric_limits<float>::quiet_NaN());
        w.addTrack (c, bad, 0, false);
        w.run ({ &c }, 0.05);
        CHECK (w.hub->slots[c.scopeIndex].readL (w.hub->slots[c.scopeIndex].head.load() - 1) == 0.0f, "NaN remplacé par 0");
    }

    void testColours()
    {
        std::printf ("Couleurs\n");
        auto* hub = createHub();
        int count = 0;
        const uint32_t* colours = palette (count);

        const int a = hub->claim (Kind::scope, 2);
        const int s = hub->claim (Kind::sync, 2);
        const int b = hub->claim (Kind::scope, 2);
        const int c = hub->claim (Kind::scope, 1);
        CHECK (hub->slots[a].colour.load() == colours[0], "première : vert");
        CHECK (hub->slots[b].colour.load() == colours[1], "deuxième : rouge (le Sync ne prend pas de couleur)");
        CHECK (hub->slots[c].colour.load() == colours[2], "troisième : bleu");

        // La deuxième disparaît : la suivante reprend sa couleur
        hub->release (b);
        const int d = hub->claim (Kind::scope, 2);
        CHECK (hub->slots[d].colour.load() == colours[1], "couleur libérée reprise");

        // Piste dupliquée : même couleur automatique dans l'état -> on en change
        const int e = hub->claim (Kind::scope, 2);
        hub->restoreColour (e, colours[0], false);
        CHECK (hub->slots[e].colour.load() != colours[0], "doublon automatique changé");
        // Couleur choisie à la main : gardée même en double
        hub->restoreColour (e, colours[0], true);
        CHECK (hub->slots[e].colour.load() == colours[0], "doublon choisi à la main gardé");

        // Plus de cases que de couleurs : on tourne dans la palette sans planter
        std::vector<int> many;
        for (int i = 0; i < 30; ++i) many.push_back (hub->claim (Kind::scope, 2));
        CHECK (many.back() >= 0, "beaucoup d'instances");
        for (int i : many) hub->release (i);

        // Toutes les cases prises
        std::vector<int> all;
        for (int i = 0; i < maxSlots + 3; ++i) all.push_back (hub->claim (Kind::scope, 2));
        CHECK (all.back() == -1, "plus de case libre : -1");
        (void) s;
    }

    //==========================================================================
    void testTrace()
    {
        std::printf ("Lecture des traces (compensation, fenêtre, somme)\n");
        World w;
        Track a, b;
        // Même son sur deux pistes, l'une retardée de 1000 échantillons par un plugin
        const auto src = music (w.sr, 6.0, 31);
        w.addTrack (a, src, 0);
        w.addTrack (b, src, 1000);
        w.run ({ &a, &b }, 3.0);
        CHECK (latencyOf (w, b) == 1000, "latence b %d", latencyOf (w, b));

        const auto& sa = w.hub->slots[a.scopeIndex];
        const auto& sb = w.hub->slots[b.scopeIndex];
        const int64_t t0 = w.pos - 30000;

        TraceSource ta { &sa, sa.latency.load(), Channel::mid };
        TraceSource tb { &sb, sb.latency.load(), Channel::mid };
        float maxDiff = 0.0f;
        for (int64_t t = t0; t < t0 + 20000; ++t)
        {
            float va = 0, vb = 0;
            const bool oka = ta.sample (t, va), okb = tb.sample (t, vb);
            CHECK (oka && okb, "échantillon disponible %lld", (long long) t);
            maxDiff = std::max (maxDiff, std::abs (va - vb));
            if (! oka || ! okb) break;
        }
        CHECK (maxDiff < 1e-6f, "après compensation, les deux traces se superposent (écart %g)", maxDiff);

        // Sans compensation, elles sont décalées
        TraceSource raw { &sb, 0, Channel::mid };
        float diffRaw = 0.0f;
        for (int64_t t = t0; t < t0 + 20000; ++t) { float va = 0, vb = 0; ta.sample (t, va); raw.sample (t, vb); diffRaw = std::max (diffRaw, std::abs (va - vb)); }
        CHECK (diffRaw > 0.1f, "sans compensation : décalées (%g)", diffRaw);

        // Le présent d'une trace compensée est en retard de sa latence
        CHECK (tb.latestTime() == sb.head.load() - 1000, "présent compensé");

        // Fenêtre calée sur la grille : 1/4 = 1 temps
        const Window win = Window::compute (sa, 1.0, std::min (ta.latestTime(), tb.latestTime()));
        const double spb = w.sr * 60.0 / 145.0;
        CHECK (std::abs ((double) win.length - spb) < 1.0, "longueur %lld", (long long) win.length);
        CHECK (win.start <= win.now && win.now < win.start + win.length, "présent dans la fenêtre");
        const double startBeats = (double) win.start / spb;
        CHECK (std::abs (startBeats - std::round (startBeats)) < 1e-3, "début sur un temps (%f)", startBeats);

        // Colonnes : min/max, partie récente et partie précédente
        std::vector<Column> cols (500);
        renderColumns ({ ta }, win, cols, 1.0f);
        int valid = 0;
        for (auto& c : cols) if (c.valid) ++valid;
        CHECK (valid == 500, "toutes les colonnes ont des données (%d)", valid);

        // Somme : a + b compensés = 2a
        std::vector<Column> sum (500), single (500);
        renderColumns ({ ta, tb }, win, sum, 1.0f);
        renderColumns ({ ta }, win, single, 2.0f);
        float sumErr = 0.0f;
        for (size_t i = 0; i < sum.size(); ++i)
            sumErr = std::max ({ sumErr, std::abs (sum[i].lo - single[i].lo), std::abs (sum[i].hi - single[i].hi) });
        CHECK (sumErr < 1e-4f, "somme = 2 x même signal (écart %g)", sumErr);

        // Zoom : moins d'un échantillon par colonne -> une valeur par colonne, interpolée
        const Window tiny { win.now - 200, 200, win.now };
        std::vector<Column> fine (800);
        renderColumns ({ ta }, tiny, fine, 1.0f);
        bool smooth = true;
        for (auto& c : fine) if (! c.valid || c.lo != c.hi) smooth = false;
        CHECK (smooth, "zoom : valeurs interpolées");

        // Après un saut de l'hôte, rien d'avant le saut n'est montré
        w.pos = 500000;
        w.run ({ &a, &b }, 0.1);
        TraceSource ta2 { &sa, sa.latency.load(), Channel::mid };
        float v = 0;
        CHECK (! ta2.sample (499999, v), "avant le saut : vide");
        CHECK (ta2.sample (500000, v), "après le saut : disponible");
        TraceSource tb2 { &sb, sb.latency.load(), Channel::mid };
        CHECK (! tb2.sample (500000 - 1, v) && tb2.sample (500000, v), "trace compensée : le saut est au même endroit");

        // Canaux
        std::printf ("Canaux\n");
        {
            Hub* hub = createHub();
            const int i = hub->claim (Kind::scope, 2);
            Tap tap;
            tap.attach (hub, i);
            tap.prepare (48000.0);
            const float L[4] = { 1.0f, 0.5f, 0.0f, -1.0f }, R[4] = { 0.0f, 0.5f, 1.0f, -1.0f };
            Transport t; t.playing = true; t.hasSamplePosition = true; t.samplePosition = 10;
            tap.process (L, R, 4, t);
            const auto& s = hub->slots[i];
            float m = 0, l = 0, r = 0, side = 0;
            TraceSource { &s, 0, Channel::mid }.sample (10, m);
            TraceSource { &s, 0, Channel::left }.sample (10, l);
            TraceSource { &s, 0, Channel::right }.sample (10, r);
            TraceSource { &s, 0, Channel::side }.sample (10, side);
            CHECK (m == 0.5f && l == 1.0f && r == 0.0f && side == 0.5f, "mid %g left %g right %g side %g", m, l, r, side);
        }
    }

    void testPerformance()
    {
        std::printf ("Charge de la mesure\n");
        World w;
        w.sr = 96000.0;
        Track a;
        w.addTrack (a, music (w.sr, 6.0, 41), 3000);
        w.run ({ &a }, 3.0);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 10; ++i) a.measurer.run (*w.hub, a.scopeIndex);
        const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count() / 10.0;
        std::printf ("  une mesure à 96 kHz : %.1f ms (toutes les 200 ms)\n", ms);
        CHECK (ms < 120.0, "mesure trop lente : %.1f ms", ms);
    }
}

int main()
{
    testEstimator();
    testEndToEnd();
    testPairing();
    testTapAndClock();
    testColours();
    testTrace();
    testPerformance();

    std::printf ("\n%d vérifications, %d échec(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
