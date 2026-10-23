#pragma once

// Mémoire partagée entre toutes les instances d'Oscilloscope et d'Oscilloscope Sync d'un même processus
// (les deux plugins sont deux bibliothèques séparées : elles se retrouvent par une variable d'environnement
// du processus, voir HubAccess.h). Ce fichier ne dépend pas de JUCE.
//
// Chaque instance occupe une case (Slot). Elle y écrit ce qu'elle reçoit dans un anneau, rangé par position
// de l'hôte en échantillons. Les autres instances relisent ces anneaux sans verrou.
//
// Règle : la structure Hub ne contient ni pointeur de fonction ni objet à table virtuelle, et sa mémoire
// n'est jamais libérée (une instance peut lire la case d'une autre bibliothèque déchargée entre-temps).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <new>

namespace osc
{
    constexpr uint64_t hubMagic = 0x5048524f53434831ull;   // "PHROSCH1"
    constexpr uint32_t hubLayoutVersion = 1;
    constexpr int maxSlots = 64;
    constexpr int ringBits = 21;                            // 2^21 échantillons : 43 s à 48 kHz, 10,9 s à 192 kHz
    constexpr int64_t ringSize = int64_t (1) << ringBits;
    constexpr int64_t ringMask = ringSize - 1;
    constexpr int nameWords = 8;                            // 64 octets de nom (63 caractères + 0)

    enum class Kind : uint32_t { none = 0, scope = 1, sync = 2 };

    namespace flags
    {
        constexpr uint32_t hidden       = 1u << 0;  // trace masquée dans toutes les fenêtres
        constexpr uint32_t manualColour = 1u << 1;  // couleur choisie à la main (sinon attribuée automatiquement)
        constexpr uint32_t latencyOff   = 1u << 2;  // compensation désactivée pour cette trace
    }

    enum class LatencyState : int32_t
    {
        off = 0,        // compensation coupée à la main
        searching = 1,  // pas encore de Sync trouvé
        measuring = 2,  // un Sync répond, mesure pas encore confirmée
        locked = 3,     // mesure confirmée
        noSignal = 4    // Sync trouvé, mais rien à mesurer (silence) : on garde la dernière valeur
    };

    inline int64_t nowMs() noexcept
    {
        using namespace std::chrono;
        return duration_cast<milliseconds> (steady_clock::now().time_since_epoch()).count();
    }

    inline int64_t nowNs() noexcept
    {
        using namespace std::chrono;
        return duration_cast<nanoseconds> (steady_clock::now().time_since_epoch()).count();
    }

    // Fonctions du système (HubPlatform.cpp : le seul fichier qui inclut les en-têtes de Windows)
    /** Mémoire du système (pas du tas d'une bibliothèque), remise à zéro, jamais rendue. */
    void* osAllocate (size_t bytes) noexcept;
    unsigned long long currentProcessId() noexcept;
    /** Variable d'environnement du processus (partagée par toutes les bibliothèques chargées). */
    bool readProcessVariable (const char* name, char* out, size_t size) noexcept;
    void writeProcessVariable (const char* name, const char* value) noexcept;

    //==========================================================================
    struct Slot
    {
        std::atomic<uint32_t> state { 0 };          // 0 libre, 1 occupée
        std::atomic<uint32_t> generation { 0 };     // +1 à chaque nouvelle occupation
        std::atomic<uint32_t> kind { 0 };
        std::atomic<uint32_t> order { 0 };          // ordre d'arrivée (affichage)
        std::atomic<uint32_t> colour { 0 };         // ARGB
        std::atomic<uint32_t> flags { 0 };
        std::atomic<int32_t>  channels { 2 };

        // Écrit par le fil audio du propriétaire
        std::atomic<int64_t> head { 0 };            // fin (exclue) de ce qui est écrit, en position de l'hôte
        std::atomic<int64_t> epochStart { 0 };      // début de la dernière suite continue de positions
        std::atomic<int64_t> lastActiveMs { 0 };
        std::atomic<int64_t> lastWriteNs { 0 };      // horloge fine : sert à caler une instance qui démarre à l'arrêt
        std::atomic<int32_t> lastBlockLength { 0 };
        std::atomic<double>  sampleRate { 0.0 };
        std::atomic<double>  bpm { 120.0 };
        std::atomic<int64_t> anchorPos { 0 };       // position (échantillons) où la position musicale vaut anchorPpq
        std::atomic<double>  anchorPpq { 0.0 };
        std::atomic<uint32_t> playing { 0 };

        // Écrit par la mesure de latence du propriétaire (Oscilloscope seulement)
        std::atomic<int32_t> latency { 0 };         // échantillons retranchés à l'affichage
        std::atomic<int32_t> latencyState { 0 };
        std::atomic<int32_t> pairedSlot { -1 };
        std::atomic<uint32_t> pairedGeneration { 0 };

        std::atomic<uint64_t> name[nameWords] {};       // nom donné dans l'interface (vide = automatique)
        std::atomic<uint64_t> trackName[nameWords] {};  // nom de la piste donné par l'hôte (vide si inconnu)

        std::atomic<float>* ringL = nullptr;        // alloués une fois pour la case, réutilisés ensuite
        std::atomic<float>* ringR = nullptr;

        bool isAlive() const noexcept           { return state.load (std::memory_order_acquire) == 1; }
        Kind getKind() const noexcept           { return (Kind) kind.load (std::memory_order_relaxed); }
        bool hasFlag (uint32_t f) const noexcept { return (flags.load (std::memory_order_relaxed) & f) != 0; }

        void setFlag (uint32_t f, bool on) noexcept
        {
            if (on) flags.fetch_or (f, std::memory_order_relaxed);
            else    flags.fetch_and (~f, std::memory_order_relaxed);
        }

        /** Actif = a traité de l'audio il y a moins de `ms` millisecondes. */
        bool isActive (int64_t now, int64_t ms = 400) const noexcept
        {
            return isAlive() && now - lastActiveMs.load (std::memory_order_relaxed) < ms;
        }

        static void writeText (std::atomic<uint64_t>* words, const char* text) noexcept
        {
            char buffer[nameWords * 8] = {};
            if (text != nullptr)
            {
                // On coupe à 63 octets sans casser un caractère UTF-8
                size_t n = 0, i = 0;
                const auto* u = reinterpret_cast<const unsigned char*> (text);
                while (u[i] != 0)
                {
                    const size_t len = u[i] < 0x80 ? 1 : (u[i] >> 5) == 6 ? 2 : (u[i] >> 4) == 14 ? 3 : (u[i] >> 3) == 30 ? 4 : 1;
                    size_t k = 1;
                    while (k < len && u[i + k] != 0) ++k;
                    if (k < len || i + len > sizeof (buffer) - 1)
                        break;
                    i += len;
                    n = i;
                }
                std::memcpy (buffer, text, n);
            }
            for (int i = 0; i < nameWords; ++i)
            {
                uint64_t w;
                std::memcpy (&w, buffer + i * 8, 8);
                words[i].store (w, std::memory_order_relaxed);
            }
        }

        /** Copie le texte (63 caractères au plus) dans out[64]. */
        static void readText (const std::atomic<uint64_t>* words, char* out) noexcept
        {
            for (int i = 0; i < nameWords; ++i)
            {
                const uint64_t w = words[i].load (std::memory_order_relaxed);
                std::memcpy (out + i * 8, &w, 8);
            }
            out[nameWords * 8 - 1] = 0;
        }

        float readMid (int64_t pos) const noexcept
        {
            const auto i = (size_t) ((uint64_t) pos & (uint64_t) ringMask);
            return 0.5f * (ringL[i].load (std::memory_order_relaxed) + ringR[i].load (std::memory_order_relaxed));
        }

        float readL (int64_t pos) const noexcept  { return ringL[(size_t) ((uint64_t) pos & (uint64_t) ringMask)].load (std::memory_order_relaxed); }
        float readR (int64_t pos) const noexcept  { return ringR[(size_t) ((uint64_t) pos & (uint64_t) ringMask)].load (std::memory_order_relaxed); }

        /** Début des données utilisables : dernière discontinuité, ou ce que l'anneau n'a pas encore écrasé. */
        int64_t validStart (int64_t headNow) const noexcept
        {
            const int64_t e = epochStart.load (std::memory_order_acquire);
            const int64_t oldest = headNow - ringSize + 8192;   // marge : l'écriture continue pendant la lecture
            return e > oldest ? e : oldest;
        }
    };

    //==========================================================================
    /** Couleurs attribuées automatiquement, dans cet ordre (la première libre). */
    inline const uint32_t* palette (int& count) noexcept
    {
        static const uint32_t colours[] = {
            0xff39ff88,  // vert
            0xffff4040,  // rouge
            0xff3da5ff,  // bleu
            0xffffd23d,  // jaune
            0xffc86bff,  // violet
            0xffff8a1f,  // orange
            0xff2ef0f0,  // cyan
            0xffff5cc8,  // rose
            0xffa6ff3d,  // vert pomme
            0xff8a9bff,  // pervenche
            0xffff9e9e,  // saumon
            0xffe8e8e8   // blanc
        };
        count = (int) (sizeof (colours) / sizeof (colours[0]));
        return colours;
    }

    //==========================================================================
    struct Hub
    {
        uint64_t magic = hubMagic;
        uint32_t layoutVersion = hubLayoutVersion;
        std::atomic<uint32_t> lock { 0 };
        std::atomic<uint32_t> orderCounter { 0 };
        std::atomic<uint64_t> uidCounter { 1 };
        Slot slots[maxSlots];

        struct ScopedLock
        {
            explicit ScopedLock (std::atomic<uint32_t>& l) noexcept : lockRef (l)
            {
                uint32_t expected = 0;
                while (! lockRef.compare_exchange_weak (expected, 1, std::memory_order_acquire))
                    expected = 0;
            }
            ~ScopedLock()   { lockRef.store (0, std::memory_order_release); }
            std::atomic<uint32_t>& lockRef;
        };

        /** Première couleur de la palette qu'aucune autre trace vivante n'utilise (sinon, par rotation). */
        uint32_t pickColourLocked (int exceptSlot) const noexcept
        {
            int count = 0;
            const uint32_t* colours = palette (count);
            for (int c = 0; c < count; ++c)
            {
                bool used = false;
                for (int i = 0; i < maxSlots && ! used; ++i)
                    if (i != exceptSlot && slots[i].isAlive() && slots[i].getKind() == Kind::scope
                        && slots[i].colour.load (std::memory_order_relaxed) == colours[c])
                        used = true;
                if (! used)
                    return colours[c];
            }
            return colours[orderCounter.load (std::memory_order_relaxed) % (uint32_t) count];
        }

        /** Occupe une case libre. Renvoie -1 s'il n'y en a plus (ou faute de mémoire). Les couleurs des Oscilloscope sont attribuées ici. */
        int claim (Kind k, int channels) noexcept
        {
            ScopedLock sl (lock);

            for (int i = 0; i < maxSlots; ++i)
            {
                auto& s = slots[i];
                if (s.state.load (std::memory_order_relaxed) != 0)
                    continue;

                if (s.ringL == nullptr)
                {
                    void* l = osAllocate ((size_t) ringSize * sizeof (std::atomic<float>));
                    void* r = osAllocate ((size_t) ringSize * sizeof (std::atomic<float>));
                    if (l == nullptr || r == nullptr)
                        return -1;      // (ce qui a pu être alloué reste perdu : rare, et sans danger)
                    s.ringL = new (l) std::atomic<float>[(size_t) ringSize];
                    s.ringR = new (r) std::atomic<float>[(size_t) ringSize];
                }
                else
                {
                    for (int64_t j = 0; j < ringSize; ++j)
                    {
                        s.ringL[j].store (0.0f, std::memory_order_relaxed);
                        s.ringR[j].store (0.0f, std::memory_order_relaxed);
                    }
                }

                s.kind.store ((uint32_t) k, std::memory_order_relaxed);
                s.order.store (orderCounter.fetch_add (1, std::memory_order_relaxed), std::memory_order_relaxed);
                s.flags.store (0, std::memory_order_relaxed);
                s.channels.store (channels, std::memory_order_relaxed);
                s.head.store (0, std::memory_order_relaxed);
                s.epochStart.store (0, std::memory_order_relaxed);
                s.lastActiveMs.store (0, std::memory_order_relaxed);
                s.lastWriteNs.store (0, std::memory_order_relaxed);
                s.lastBlockLength.store (0, std::memory_order_relaxed);
                s.sampleRate.store (0.0, std::memory_order_relaxed);
                s.bpm.store (120.0, std::memory_order_relaxed);
                s.anchorPos.store (0, std::memory_order_relaxed);
                s.anchorPpq.store (0.0, std::memory_order_relaxed);
                s.playing.store (0, std::memory_order_relaxed);
                s.latency.store (0, std::memory_order_relaxed);
                s.latencyState.store ((int32_t) LatencyState::searching, std::memory_order_relaxed);
                s.pairedSlot.store (-1, std::memory_order_relaxed);
                s.pairedGeneration.store (0, std::memory_order_relaxed);
                Slot::writeText (s.name, "");
                Slot::writeText (s.trackName, "");
                s.colour.store (k == Kind::scope ? pickColourLocked (i) : 0xffffffffu, std::memory_order_relaxed);
                s.generation.fetch_add (1, std::memory_order_relaxed);
                s.state.store (1, std::memory_order_release);
                return i;
            }

            return -1;
        }

        void release (int index) noexcept
        {
            if (index < 0 || index >= maxSlots)
                return;
            ScopedLock sl (lock);
            slots[index].state.store (0, std::memory_order_release);
        }

        /** Couleur enregistrée dans un projet. Si elle est déjà prise par une autre trace et qu'elle avait été
            attribuée automatiquement (piste dupliquée, par exemple), on en donne une autre. */
        void restoreColour (int index, uint32_t colour, bool manual) noexcept
        {
            if (index < 0 || index >= maxSlots)
                return;
            ScopedLock sl (lock);
            auto& s = slots[index];
            s.setFlag (flags::manualColour, manual);

            bool taken = false;
            for (int i = 0; i < maxSlots; ++i)
                if (i != index && slots[i].isAlive() && slots[i].getKind() == Kind::scope
                    && slots[i].colour.load (std::memory_order_relaxed) == colour)
                    taken = true;

            if (taken && ! manual)
            {
                // On regarde d'abord si la couleur actuelle de la case est libre
                const uint32_t current = s.colour.load (std::memory_order_relaxed);
                bool currentTaken = false;
                for (int i = 0; i < maxSlots; ++i)
                    if (i != index && slots[i].isAlive() && slots[i].getKind() == Kind::scope
                        && slots[i].colour.load (std::memory_order_relaxed) == current)
                        currentTaken = true;
                s.colour.store (currentTaken ? pickColourLocked (index) : current, std::memory_order_relaxed);
                return;
            }

            s.colour.store (colour, std::memory_order_relaxed);
        }
    };

    /** Nouveau hub dans la mémoire du système (jamais rendu). */
    inline Hub* createHub() noexcept
    {
        void* memory = osAllocate (sizeof (Hub));
        return memory != nullptr ? new (memory) Hub() : nullptr;
    }
}
