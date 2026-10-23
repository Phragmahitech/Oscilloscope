#pragma once

// Mesure de la latence entre un Oscilloscope Sync (en tête de piste) et un Oscilloscope (plus loin sur la piste).
// Les deux écrivent ce qu'ils reçoivent dans le hub, à la même position de l'hôte. Si les plugins placés entre eux
// retardent le son de L échantillons, l'Oscilloscope reçoit à la position p ce que le Sync a reçu à p - L.
// On retrouve L par corrélation croisée à poids de phase (GCC-PHAT), calculée par FFT. Sans JUCE.

#include "Hub.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace osc
{
    /** FFT complexe radix 2, taille puissance de deux. */
    class Fft
    {
    public:
        void setSize (size_t n)
        {
            if (n == size)
                return;
            size = n;
            int bits = 0;
            while ((size_t (1) << bits) < n) ++bits;

            reversed.resize (n);
            for (size_t i = 0; i < n; ++i)
            {
                size_t r = 0;
                for (int b = 0; b < bits; ++b)
                    if (i & (size_t (1) << b))
                        r |= size_t (1) << (bits - 1 - b);
                reversed[i] = (uint32_t) r;
            }

            twiddles.resize (n / 2);
            for (size_t i = 0; i < n / 2; ++i)
            {
                const double a = -2.0 * 3.14159265358979323846 * (double) i / (double) n;
                twiddles[i] = { (float) std::cos (a), (float) std::sin (a) };
            }
        }

        size_t getSize() const noexcept { return size; }

        void transform (std::complex<float>* data, bool inverse) const
        {
            const size_t n = size;
            for (size_t i = 0; i < n; ++i)
                if (i < reversed[i])
                    std::swap (data[i], data[reversed[i]]);

            for (size_t len = 2; len <= n; len <<= 1)
            {
                const size_t half = len / 2, step = n / len;
                for (size_t start = 0; start < n; start += len)
                {
                    for (size_t k = 0; k < half; ++k)
                    {
                        auto w = twiddles[k * step];
                        if (inverse) w = std::conj (w);
                        const auto a = data[start + k];
                        const auto b = data[start + k + half] * w;
                        data[start + k] = a + b;
                        data[start + k + half] = a - b;
                    }
                }
            }
        }

    private:
        size_t size = 0;
        std::vector<uint32_t> reversed;
        std::vector<std::complex<float>> twiddles;
    };

    //==========================================================================
    struct Estimate
    {
        bool valid = false;
        bool silent = true;
        int lag = 0;            // retard de y sur x, en échantillons
        float peak = 0.0f;      // hauteur du pic (0 à 1 environ)
        float ratio = 0.0f;     // pic / meilleur autre pic (hors voisinage)
    };

    /** Retard de y sur x, cherché dans [0, maxLag].
        x : signal du Sync, longueur ny + maxLag (x[0] correspond à la position p0 - maxLag) ;
        y : signal de l'Oscilloscope, longueur ny, à partir de p0. */
    class LagEstimator
    {
    public:
        float minPeak = 0.12f;    // mesuré : bonnes réponses >= 0,147 (saturation) ; fausses (filtres raides) <= 0,107
        float minRatio = 2.0f;   // deux musiques sans rapport : au plus 1,4 ; un sinus pur : 1,0
        float silence = 1.0e-6f;    // RMS sous laquelle on ne mesure rien (-120 dBFS)
        float regularisation = 1.0e-7f;  // presque blanchi : les aigus très atténués d'un son filtré comptent autant que le reste
        bool envelope = true;       // pic de l'enveloppe (insensible aux rotations de phase constantes : filtres, inversion)
        float taper = 0.25f;        // fondu des bords des deux extraits (fraction de la longueur de y) : moins de fuite spectrale

        Estimate estimate (const float* x, const float* y, int ny, int maxLag, double sampleRate)
        {
            Estimate e;
            if (ny <= 16 || maxLag < 0)
                return e;

            const int nx = ny + maxLag;
            if (rms (x, nx) < silence || rms (y, ny) < silence)
                return e;
            e.silent = false;

            size_t m = 1;
            while (m < (size_t) (nx + ny)) m <<= 1;
            fft.setSize (m);
            buffer.assign (m, { 0.0f, 0.0f });

            // Deux signaux réels dans une seule FFT complexe : z = x + i·y. Bords adoucis (demi-Hann) : sans ça, la coupure
            // nette d'une basse forte fuit dans les aigus et noie les aigus atténués d'un son filtré.
            const int fade = std::max (1, (int) (taper * (float) ny));
            auto edge = [fade] (int i, int n) -> float
            {
                const int d = std::min (i, n - 1 - i);
                return d >= fade ? 1.0f : (float) (0.5 - 0.5 * std::cos (3.14159265358979323846 * (d + 0.5) / fade));
            };
            for (int i = 0; i < nx; ++i) buffer[(size_t) i].real (x[i] * edge (i, nx));
            for (int i = 0; i < ny; ++i) buffer[(size_t) i].imag (y[i] * edge (i, ny));
            fft.transform (buffer.data(), false);

            // Spectre croisé conj(Y)·X, blanchi (poids de phase) avec une petite régularisation
            cross.resize (m);
            double meanMagnitude = 0.0;
            for (size_t k = 0; k < m; ++k)
            {
                const auto zk = buffer[k];
                const auto zm = std::conj (buffer[(m - k) & (m - 1)]);
                const auto X = 0.5f * (zk + zm);
                const auto Y = std::complex<float> (0.0f, -0.5f) * (zk - zm);
                cross[k] = std::conj (Y) * X;
                meanMagnitude += std::abs (cross[k]);
            }
            meanMagnitude /= (double) m;
            if (meanMagnitude <= 0.0)
                return e;

            const float eps = (float) (regularisation * meanMagnitude);
            for (size_t k = 0; k < m; ++k)
                cross[k] /= (std::abs (cross[k]) + eps);

            if (envelope)
            {
                // Signal analytique : on ne garde que les fréquences positives
                for (size_t k = 1; k < m / 2; ++k) cross[k] *= 2.0f;
                for (size_t k = m / 2 + 1; k < m; ++k) cross[k] = 0.0f;
            }

            fft.transform (cross.data(), true);

            auto valueAt = [&] (int k) { return envelope ? std::abs (cross[(size_t) k]) : cross[(size_t) k].real(); };

            // d[k] = Σ y[i] x[i + k] ; le retard vaut maxLag - k
            int best = -1;
            float bestValue = -1.0e30f;
            for (int k = 0; k <= maxLag; ++k)
            {
                const float v = valueAt (k);
                if (v > bestValue) { bestValue = v; best = k; }
            }

            const int guard = std::max (4, (int) std::lround (sampleRate * 0.0003));   // 0,3 ms autour du pic
            float second = 0.0f;
            for (int k = 0; k <= maxLag; ++k)
                if (std::abs (k - best) > guard)
                    second = std::max (second, valueAt (k));

            const float norm = (float) m;
            e.peak = bestValue / norm;
            e.ratio = second > 0.0f ? bestValue / second : 1000.0f;
            e.lag = maxLag - best;
            e.valid = e.peak >= minPeak && e.ratio >= minRatio;
            return e;
        }

    private:
        static float rms (const float* d, int n)
        {
            double s = 0.0;
            for (int i = 0; i < n; ++i) s += (double) d[i] * d[i];
            return (float) std::sqrt (s / std::max (1, n));
        }

        Fft fft;
        std::vector<std::complex<float>> buffer, cross;
    };

    //==========================================================================
    /** Ne retient une valeur qu'après deux mesures identiques de suite (une mesure isolée peut se tromper). */
    struct LagTracker
    {
        int current = 0;
        bool known = false;
        int candidate = 0;
        int count = 0;

        /** Renvoie true si la valeur retenue vient de changer. */
        bool push (const Estimate& e)
        {
            if (! e.valid)
                return false;

            if (known && e.lag == current)
            {
                count = 0;
                return false;
            }

            if (count > 0 && e.lag == candidate)
                ++count;
            else
            {
                candidate = e.lag;
                count = 1;
            }

            if (count >= 2)
            {
                const bool changed = ! known || current != candidate;
                current = candidate;
                known = true;
                count = 0;
                return changed;
            }

            return false;
        }

        void clear()    { *this = LagTracker(); }
    };

    //==========================================================================
    /** La mesure faite pour un Oscilloscope : choix du Sync, lecture des anneaux, suivi. Appelé hors du fil audio. */
    class LatencyMeasurer
    {
    public:
        double maxLagSeconds = 1.0;
        double windowSeconds = 0.35;
        float acquireRatio = 3.0f;

        /** Une mesure. Écrit le résultat dans la case de l'Oscilloscope. Renvoie le délai conseillé avant la
            suivante (ms) : rapide tant que rien n'est confirmé, plus lent ensuite pour ménager le processeur. */
        int run (Hub& hub, int scopeIndex)
        {
            runOnce (hub, scopeIndex);
            const auto state = (LatencyState) hub.slots[scopeIndex].latencyState.load (std::memory_order_relaxed);
            switch (state)
            {
                case LatencyState::measuring:   return 200;
                case LatencyState::locked:      return 500;
                case LatencyState::searching:   return 1000;
                case LatencyState::off:
                case LatencyState::noSignal:
                default:                        return 400;
            }
        }

    private:
        void runOnce (Hub& hub, int scopeIndex)
        {
            auto& scope = hub.slots[scopeIndex];
            const int64_t now = nowMs();

            if (scope.hasFlag (flags::latencyOff))
            {
                scope.latency.store (0, std::memory_order_relaxed);
                scope.latencyState.store ((int32_t) LatencyState::off, std::memory_order_relaxed);
                tracker.clear();
                pairedIndex = -1;
                held = 0;
                return;
            }

            if (! scope.isActive (now))
                return;

            const double sr = scope.sampleRate.load (std::memory_order_relaxed);
            if (sr <= 0.0)
                return;

            // Le Sync suivi a disparu ?
            if (pairedIndex >= 0)
            {
                const auto& s = hub.slots[pairedIndex];
                if (! s.isAlive() || s.generation.load() != pairedGeneration || s.getKind() != Kind::sync)
                    forget (scope);
            }

            // Candidats : le Sync suivi ; sinon ceux de la même piste (si l'hôte donne les noms) ; sinon tous.
            std::vector<int> candidates;
            // (tous les 12 passages, on regarde quand même les autres : un Sync ajouté ensuite sur la bonne piste)
            if (pairedIndex >= 0 && misses < 6 && (++runCount % 12) != 0)
                candidates.push_back (pairedIndex);
            else
                candidates = findCandidates (hub, scopeIndex, sr, now);

            int bestIndex = -1;
            Estimate best, pairedEstimate;
            bool anySignal = false;

            for (int c : candidates)
            {
                Estimate e;
                if (! measure (hub.slots[c], scope, sr, e))
                    continue;
                anySignal = anySignal || ! e.silent;
                // Un Sync qu'on ne suit pas encore doit répondre très nettement : deux pistes différentes peuvent
                // partager un son (le même kick), ce qui donne parfois un pic moyen à un faux retard.
                if (c != pairedIndex && e.ratio < acquireRatio)
                    e.valid = false;
                if (c == pairedIndex)
                    pairedEstimate = e;
                if (e.valid && (bestIndex < 0 || e.peak > best.peak))
                {
                    best = e;
                    bestIndex = c;
                }
            }

            // On ne quitte le Sync suivi que pour un autre nettement meilleur (deux pistes identiques ne doivent pas faire basculer)
            if (bestIndex >= 0 && bestIndex != pairedIndex && pairedEstimate.valid && best.peak < 1.3f * pairedEstimate.peak)
            {
                best = pairedEstimate;
                bestIndex = pairedIndex;
            }

            if (bestIndex >= 0)
            {
                misses = 0;
                if (bestIndex != pairedIndex)
                {
                    if (tracker.known)
                        held = tracker.current;
                    pairedIndex = bestIndex;
                    pairedGeneration = hub.slots[bestIndex].generation.load();
                    tracker.clear();
                }
                tracker.push (best);
                lastEstimate = best;
            }
            else if (anySignal && pairedIndex >= 0)
            {
                ++misses;   // du son, mais rien de net : après plusieurs échecs, on cherche à nouveau parmi tous les Sync
            }

            scope.pairedSlot.store (pairedIndex, std::memory_order_relaxed);
            scope.pairedGeneration.store (pairedGeneration, std::memory_order_relaxed);
            scope.latency.store (tracker.known ? tracker.current : held, std::memory_order_relaxed);

            LatencyState state;
            if (pairedIndex < 0)            state = LatencyState::searching;
            else if (! tracker.known)       state = LatencyState::measuring;
            else if (! anySignal)           state = LatencyState::noSignal;
            else                            state = LatencyState::locked;
            scope.latencyState.store ((int32_t) state, std::memory_order_relaxed);
        }

    public:
        const Estimate& getLastEstimate() const noexcept    { return lastEstimate; }
        LagEstimator& getEstimator() noexcept               { return estimator; }

    private:
        /** Le Sync suivi a disparu : on garde la dernière latence (les plugins sont sans doute toujours là) jusqu'à une nouvelle mesure. */
        void forget (Slot&)
        {
            if (tracker.known)
                held = tracker.current;
            pairedIndex = -1;
            pairedGeneration = 0;
            misses = 0;
            tracker.clear();
        }

        static std::vector<int> findCandidates (const Hub& hub, int scopeIndex, double sr, int64_t now)
        {
            char own[nameWords * 8];
            Slot::readText (hub.slots[scopeIndex].trackName, own);

            std::vector<int> all, sameTrack;
            for (int i = 0; i < maxSlots; ++i)
            {
                const auto& s = hub.slots[i];
                if (i == scopeIndex || s.getKind() != Kind::sync || ! s.isActive (now)
                    || std::abs (s.sampleRate.load (std::memory_order_relaxed) - sr) > 0.5)
                    continue;
                all.push_back (i);

                char name[nameWords * 8];
                Slot::readText (s.trackName, name);
                if (own[0] != 0 && std::strcmp (own, name) == 0)
                    sameTrack.push_back (i);
            }

            return sameTrack.size() == 1 ? sameTrack : all;
        }

        /** Copie les deux signaux depuis les anneaux et estime le retard. false si pas assez de données continues. */
        bool measure (const Slot& sync, const Slot& scope, double sr, Estimate& out)
        {
            const int64_t headSync = sync.head.load (std::memory_order_acquire);
            const int64_t headScope = scope.head.load (std::memory_order_acquire);
            const int64_t end = std::min (headSync, headScope);

            const int ny = (int) std::lround (windowSeconds * sr);
            const int64_t start = end - ny;
            const int64_t syncValid = sync.validStart (headSync);
            const int64_t scopeValid = scope.validStart (headScope);

            // Les deux suivent les mêmes sauts de position (même piste) : on reste dans la partie continue des deux.
            int64_t maxLag = std::min<int64_t> ((int64_t) std::lround (maxLagSeconds * sr), start - syncValid);
            maxLag = std::min (maxLag, start - scopeValid);
            if (maxLag < (int64_t) std::lround (0.002 * sr))
                return false;

            const int nx = ny + (int) maxLag;
            xs.resize ((size_t) nx);
            ys.resize ((size_t) ny);
            const int64_t xStart = start - maxLag;
            for (int i = 0; i < nx; ++i) xs[(size_t) i] = sync.readMid (xStart + i);
            for (int i = 0; i < ny; ++i) ys[(size_t) i] = scope.readMid (start + i);

            // Données écrasées entre-temps (saut de position pendant la copie) : on ignore cette mesure
            if (sync.epochStart.load (std::memory_order_acquire) > xStart || scope.epochStart.load (std::memory_order_acquire) > start)
                return false;

            out = estimator.estimate (xs.data(), ys.data(), ny, (int) maxLag, sr);
            return true;
        }

        LagEstimator estimator;
        LagTracker tracker;
        Estimate lastEstimate;
        std::vector<float> xs, ys;
        int pairedIndex = -1;
        uint32_t pairedGeneration = 0;
        int misses = 0;
        int held = 0;
        uint32_t runCount = 0;
    };
}
