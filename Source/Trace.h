#pragma once

// Lecture des traces pour l'affichage : compensation de latence, fenêtre calée sur la grille, colonnes min/max.
// Sans JUCE.
//
// « Temps » = position de l'hôte telle qu'elle serait sans les plugins à latence de la piste : la trace d'une
// instance retardée de L échantillons se lit à la position t + L de son anneau.

#include "Hub.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace osc
{
    enum class Channel : int { mid = 0, left = 1, right = 2, side = 3 };

    struct TraceSource
    {
        const Slot* slot = nullptr;
        int latency = 0;
        Channel channel = Channel::mid;

        /** Fin (exclue) des données disponibles, en temps compensé. */
        int64_t latestTime() const noexcept
        {
            return slot->head.load (std::memory_order_acquire) - latency;
        }

        bool sample (int64_t t, float& out) const noexcept
        {
            const int64_t pos = t + latency;
            const int64_t head = slot->head.load (std::memory_order_acquire);
            if (pos >= head)
                return false;
            // Juste après un saut de l'hôte, les L premiers échantillons sont la fin retardée d'avant le saut : on les écarte
            const int64_t start = std::max (slot->epochStart.load (std::memory_order_acquire) + latency, head - ringSize + 8192);
            if (pos < start)
                return false;

            switch (channel)
            {
                case Channel::left:  out = slot->readL (pos); break;
                case Channel::right: out = slot->readR (pos); break;
                case Channel::side:  out = 0.5f * (slot->readL (pos) - slot->readR (pos)); break;
                case Channel::mid:
                default:             out = slot->readMid (pos); break;
            }
            return true;
        }
    };

    //==========================================================================
    /** La fenêtre affichée : [start, start + length), le présent `now` (exclu) quelque part dedans.
        Avant `now` : le passage en cours ; après : le passage précédent (t - length), comme un balayage. */
    struct Window
    {
        int64_t start = 0;
        int64_t length = 1;
        int64_t now = 0;

        /** Fenêtre de `beats` noires calée sur la grille de l'hôte, d'après le transport de `clock`. */
        static Window compute (const Slot& clock, double beats, int64_t now) noexcept
        {
            const double sr = std::max (1.0, clock.sampleRate.load (std::memory_order_relaxed));
            const double bpm = std::max (1.0, clock.bpm.load (std::memory_order_relaxed));
            const int64_t anchorPos = clock.anchorPos.load (std::memory_order_relaxed);
            const double anchorPpq = clock.anchorPpq.load (std::memory_order_relaxed);
            const double spb = sr * 60.0 / bpm;

            Window w;
            w.now = now;
            w.length = std::max<int64_t> (2, (int64_t) std::llround (beats * spb));

            const double ppqNow = anchorPpq + (double) (now - 1 - anchorPos) / spb;
            const double index = std::floor (ppqNow / beats + 1.0e-9);
            w.start = anchorPos + (int64_t) std::llround ((index * beats - anchorPpq) * spb);

            // Arrondis : le présent doit tomber dans la fenêtre
            if (now - 1 < w.start)              w.start -= w.length;
            if (now - 1 >= w.start + w.length)  w.start += w.length;
            return w;
        }

        /** Le temps à lire pour une position de la fenêtre (balayage). */
        int64_t timeAt (int64_t t) const noexcept    { return t < now ? t : t - length; }
    };

    struct Column
    {
        float lo = 0.0f, hi = 0.0f;
        bool valid = false;
    };

    /** Somme des sources (une seule pour une trace normale) × gain, rangée en colonnes min/max. */
    inline void renderColumns (const std::vector<TraceSource>& sources, const Window& w, std::vector<Column>& cols, float gain)
    {
        const int n = (int) cols.size();
        if (n == 0)
            return;

        auto value = [&] (int64_t t, float& out) -> bool
        {
            const int64_t tt = w.timeAt (t);
            float sum = 0.0f;
            bool any = false;
            for (auto& s : sources)
            {
                float v;
                if (s.sample (tt, v)) { sum += v; any = true; }
            }
            out = sum * gain;
            return any;
        };

        const double spc = (double) w.length / (double) n;   // échantillons par colonne

        if (spc >= 1.0)
        {
            for (int c = 0; c < n; ++c)
            {
                const int64_t a = w.start + (int64_t) std::floor (c * spc);
                const int64_t b = std::max (a + 1, w.start + (int64_t) std::floor ((c + 1) * spc));
                Column col;
                for (int64_t t = a; t < b; ++t)
                {
                    float v;
                    if (! value (t, v))
                        continue;
                    if (! col.valid) { col.lo = col.hi = v; col.valid = true; }
                    else { col.lo = std::min (col.lo, v); col.hi = std::max (col.hi, v); }
                }
                cols[(size_t) c] = col;
            }
        }
        else
        {
            for (int c = 0; c < n; ++c)
            {
                const double x = (double) w.start + (c + 0.5) * spc - 0.5;
                const int64_t i0 = (int64_t) std::floor (x);
                const float f = (float) (x - (double) i0);
                float v0, v1;
                const bool ok0 = value (i0, v0), ok1 = value (i0 + 1, v1);
                Column col;
                if (ok0 && ok1)      { col.lo = col.hi = v0 + f * (v1 - v0); col.valid = true; }
                else if (ok0)        { col.lo = col.hi = v0; col.valid = true; }
                else if (ok1)        { col.lo = col.hi = v1; col.valid = true; }
                cols[(size_t) c] = col;
            }
        }
    }
}
