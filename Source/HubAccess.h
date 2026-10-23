#pragma once

// Retrouver le hub du processus. Oscilloscope et Oscilloscope Sync sont deux bibliothèques chargées séparément
// par l'hôte : leurs variables globales ne sont pas partagées. Le premier qui arrive crée le hub dans la mémoire
// du système et en écrit l'adresse dans une variable d'environnement du processus ; les suivants la relisent.
// La valeur contient le numéro du processus : un processus enfant (scanner de plugins) qui hérite de
// l'environnement ne prend pas une adresse qui n'existe pas chez lui.

#include "Hub.h"

#include <juce_core/juce_core.h>

#include <cstdio>

namespace osc
{
    inline Hub* getProcessHub()
    {
        static Hub* cached = nullptr;   // par bibliothèque (créé sur le fil des messages)
        if (cached != nullptr)
            return cached;

        const char* variable = "PHRAGMA_OSCILLOSCOPE_HUB_1";
        const unsigned long long pid = currentProcessId();

        juce::InterProcessLock lock ("PhragmaOscilloscopeHub_" + juce::String ((juce::int64) pid));
        const bool locked = lock.enter (2000);

        char value[128] = {};
        unsigned long long storedPid = 0, address = 0;
        if (readProcessVariable (variable, value, sizeof (value))
            && std::sscanf (value, "%llu:%llx", &storedPid, &address) == 2 && storedPid == pid && address != 0)
        {
            auto* h = reinterpret_cast<Hub*> ((uintptr_t) address);
            if (h->magic == hubMagic && h->layoutVersion == hubLayoutVersion)
                cached = h;
        }

        if (cached == nullptr)
        {
            cached = createHub();
            if (cached != nullptr)
            {
                std::snprintf (value, sizeof (value), "%llu:%llx", pid, (unsigned long long) (uintptr_t) cached);
                writeProcessVariable (variable, value);
            }
        }

        if (locked)
            lock.exit();

        return cached;
    }
}
