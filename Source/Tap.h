#pragma once

// Écriture de ce qu'une instance reçoit dans sa case du hub, rangé par position de l'hôte.
// Appelé par le fil audio, sans allocation ni verrou. Sans JUCE.

#include "Hub.h"

#include <cmath>

namespace osc
{
    /** Ce que l'hôte dit du transport au début d'un bloc. */
    struct Transport
    {
        bool playing = false;
        bool hasSamplePosition = false;
        int64_t samplePosition = 0;
        bool hasPpq = false;
        double ppq = 0.0;
        double bpm = 120.0;
    };

    class Tap
    {
    public:
        void attach (Hub* h, int index) noexcept
        {
            hub = h;
            slotIndex = index;
            slot = (h != nullptr && index >= 0) ? &h->slots[index] : nullptr;
            started = false;
            expected = 0;
        }

        Slot* getSlot() const noexcept          { return slot; }

        void prepare (double sr) noexcept
        {
            sampleRate = sr > 0.0 ? sr : 44100.0;
            started = false;
            if (slot != nullptr)
                slot->sampleRate.store (sampleRate, std::memory_order_relaxed);
        }

        void process (const float* left, const float* right, int numSamples, const Transport& t,
                      int64_t nowNanoseconds = nowNs()) noexcept
        {
            if (slot == nullptr || numSamples <= 0)
                return;

            const double bpm = (t.bpm > 1.0 && t.bpm < 2000.0) ? t.bpm : 120.0;
            const double samplesPerBeat = sampleRate * 60.0 / bpm;

            const int64_t previousWrite = slot->lastWriteNs.load (std::memory_order_relaxed);
            const bool resumed = started && nowNanoseconds - previousWrite > 300000000;   // pas appelé depuis 300 ms

            int64_t pos;
            bool anchored = false;
            double ppq = 0.0;

            if (t.playing && (t.hasSamplePosition || t.hasPpq))
            {
                // En lecture : la position de l'hôte. Sans position en échantillons, on la déduit de la position musicale.
                pos = t.hasSamplePosition ? t.samplePosition : (int64_t) std::llround (t.ppq * samplesPerBeat);
                ppq = t.hasPpq ? t.ppq : (double) pos / samplesPerBeat;
                anchored = true;
            }
            else if (! started || resumed || wasPlaying)
            {
                // À l'arrêt, on avance comme si le temps passait. Une instance qui démarre (ou reprend) se cale sur
                // les autres pour que toutes restent alignées entre elles.
                int64_t joined = 0;
                pos = joinRunningClock (numSamples, nowNanoseconds, joined) ? joined : (started ? expected : 0);
            }
            else
            {
                pos = expected;
            }

            if (! started || pos != expected)
                slot->epochStart.store (pos, std::memory_order_release);

            if (anchored)
            {
                slot->anchorPos.store (pos, std::memory_order_relaxed);
                slot->anchorPpq.store (ppq, std::memory_order_relaxed);
            }
            else if (! started)
            {
                slot->anchorPos.store (pos, std::memory_order_relaxed);
                slot->anchorPpq.store ((double) pos / samplesPerBeat, std::memory_order_relaxed);
            }

            slot->bpm.store (bpm, std::memory_order_relaxed);
            slot->playing.store (anchored ? 1u : 0u, std::memory_order_relaxed);

            if (right == nullptr)
                right = left;

            auto* l = slot->ringL;
            auto* r = slot->ringR;
            for (int i = 0; i < numSamples; ++i)
            {
                const auto idx = (size_t) ((uint64_t) (pos + i) & (uint64_t) ringMask);
                l[idx].store (sanitise (left[i]), std::memory_order_relaxed);
                r[idx].store (sanitise (right[i]), std::memory_order_relaxed);
            }

            expected = pos + numSamples;
            started = true;
            wasPlaying = anchored;
            slot->lastBlockLength.store (numSamples, std::memory_order_relaxed);
            slot->head.store (expected, std::memory_order_release);
            slot->lastWriteNs.store (nowNanoseconds, std::memory_order_relaxed);
            slot->lastActiveMs.store (nanosecondsToMs (nowNanoseconds), std::memory_order_relaxed);
        }

    private:
        static int64_t nanosecondsToMs (int64_t ns) noexcept   { return ns / 1000000; }

        static float sanitise (float x) noexcept
        {
            return std::isfinite (x) ? (x > 64.0f ? 64.0f : (x < -64.0f ? -64.0f : x)) : 0.0f;
        }

        /** Position d'une autre instance qui tourne à l'arrêt. Si elle a déjà traité ce bloc (écrite il y a moins
            d'une demi-durée de bloc), on recule d'un bloc pour tomber sur le même départ qu'elle. */
        bool joinRunningClock (int numSamples, int64_t now, int64_t& out) const noexcept
        {
            if (hub == nullptr)
                return false;

            const Slot* best = nullptr;
            int64_t bestNs = 0;
            for (int i = 0; i < maxSlots; ++i)
            {
                const auto& s = hub->slots[i];
                if (i == slotIndex || ! s.isAlive() || s.playing.load (std::memory_order_relaxed) != 0)
                    continue;
                const int64_t w = s.lastWriteNs.load (std::memory_order_relaxed);
                if (now - w < 300000000 && w > bestNs)
                {
                    best = &s;
                    bestNs = w;
                }
            }

            if (best == nullptr)
                return false;

            const double blockNs = 1.0e9 * numSamples / sampleRate;
            const int64_t head = best->head.load (std::memory_order_acquire);
            out = (double) (now - bestNs) < 0.5 * blockNs ? head - best->lastBlockLength.load (std::memory_order_relaxed) : head;
            return true;
        }

        Hub* hub = nullptr;
        Slot* slot = nullptr;
        int slotIndex = -1;
        double sampleRate = 44100.0;
        int64_t expected = 0;
        bool started = false;
        bool wasPlaying = false;
    };
}
