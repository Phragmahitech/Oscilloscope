#pragma once

#include <juce_core/juce_core.h>

#include "Hub.h"

namespace osc
{
    /** Nom affiché d'une instance : celui donné dans l'interface, sinon celui de la piste (si l'hôte le donne),
        sinon « SCOPE n » (n = rang d'arrivée parmi les Oscilloscope encore ouverts). */
    inline juce::String displayName (const Hub& hub, int index)
    {
        const auto& s = hub.slots[index];
        char text[nameWords * 8];

        Slot::readText (s.name, text);
        if (text[0] != 0)
            return juce::String::fromUTF8 (text);

        Slot::readText (s.trackName, text);
        if (text[0] != 0)
            return juce::String::fromUTF8 (text);

        if (s.getKind() == Kind::sync)
            return "SYNC";

        const uint32_t order = s.order.load (std::memory_order_relaxed);
        int rank = 1;
        for (int i = 0; i < maxSlots; ++i)
        {
            const auto& o = hub.slots[i];
            if (i != index && o.isAlive() && o.getKind() == Kind::scope && o.order.load (std::memory_order_relaxed) < order)
                ++rank;
        }
        return "SCOPE " + juce::String (rank);
    }

    /** « 2048 smp · 42.7 ms ». */
    inline juce::String latencyText (int samples, double sampleRate)
    {
        juce::String t = juce::String (samples) + " smp";
        if (sampleRate > 0.0)
            t << juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) << juce::String (1000.0 * samples / sampleRate, samples * 1000.0 / sampleRate < 10.0 ? 2 : 1) << " ms";
        return t;
    }

    /** Les Oscilloscope qui suivent ce Sync : « nom|latence compensée » (fenêtres des deux Sync). */
    inline juce::StringArray describeFollowers (const Hub& hub, int syncIndex)
    {
        juce::StringArray out;
        if (syncIndex < 0)
            return out;

        const uint32_t generation = hub.slots[syncIndex].generation.load();
        for (int i = 0; i < maxSlots; ++i)
        {
            const auto& s = hub.slots[i];
            if (! s.isAlive() || s.getKind() != Kind::scope)
                continue;
            if (s.pairedSlot.load() != syncIndex || s.pairedGeneration.load() != generation)
                continue;

            const auto state = (LatencyState) s.latencyState.load();
            const juce::String status = state == LatencyState::measuring ? juce::String ("MEASURING...")
                                                                         : latencyText (s.latency.load(), s.sampleRate.load());
            out.add (displayName (hub, i) + "|" + status);
        }
        return out;
    }
}
