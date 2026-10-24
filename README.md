# Oscilloscope by Phragma

Oscilloscope VST3 multi-pistes, calé sur le tempo, qui compense automatiquement la latence des plugins de la piste.

Trois plugins :

- **Oscilloscope** : à mettre où on veut regarder (en général en fin de chaîne). Chaque fenêtre montre toutes les instances du projet. Chaque nouvelle instance reçoit une couleur différente.
- **Oscilloscope Sync** : à mettre **en premier** sur la piste. Il ne touche pas au son. L'Oscilloscope de la même piste compare ce que le Sync reçoit avec ce qu'il reçoit lui-même et en déduit, à l'échantillon près, le retard des plugins placés entre les deux. La mesure tourne en continu : ajouter ou retirer un plugin à latence est pris en compte en une à deux secondes. Sans Sync sur la piste, la trace n'est pas compensée.
- **Oscilloscope Sync Instrument** : pour une piste d'instrument. Un effet ne peut pas se placer avant l'instrument, donc la latence de l'instrument lui-même (Kick3 : 110 échantillons) échappe au Sync. On met Oscilloscope Sync Instrument à la place de l'instrument et on charge l'instrument dedans (LOAD INSTRUMENT : liste des VST3 installés, ou BROWSE). Il lui passe le MIDI, montre sa fenêtre, lit sa latence, la déclare à Live (la compensation de Live reste juste) et l'Oscilloscope l'ajoute à ce qu'il mesure après. Les paramètres automatisables de l'instrument (128 au plus, sans Bypass) sont relayés pour l'automation de Live (P1… renommés d'après l'instrument). Le réglage et l'état de l'instrument sont enregistrés avec le projet ; si l'instrument est introuvable à l'ouverture, un message s'affiche et son état est conservé.

Réglages de la fenêtre :

- **TIME** : 1/32 à 4/1, **1/4 par défaut**, calé sur la grille de l'hôte (balayage).
- **VIEW** : OVERLAY (traces superposées, pour aligner les phases), SPLIT (une ligne par piste), SUM (somme des traces visibles, compensées).
- **CHANNEL** : MID, L, R, SIDE. **GAIN** : ±24 dB (double-clic : 0). **FREEZE** : fige l'image.
- Liste : clic sur la pastille = couleur ; œil = afficher / masquer ; double-clic sur le nom = renommer ; clic sur la ligne de latence = compensation AUTO ou OFF.

Limites connues : un passe-bas très raide (24 dB/oct sous 300 Hz environ) entre le Sync et l'Oscilloscope peut empêcher la mesure (la dernière valeur est gardée) ; un son parfaitement périodique (sinus pur) ne permet pas de mesurer (retard ambigu) ; les instances doivent tourner dans le même processus (normal dans Live).

JUCE 8, C++17, CMake. Le moteur (`Source/Hub.h`, `Tap.h`, `LatencyFinder.h`, `Trace.h`) n'utilise pas JUCE. Les polices (Michroma, IBM Plex Mono, licence OFL : voir `Assets/`) sont téléchargées à la configuration depuis `google/fonts` à un commit fixe et vérifiées par SHA-256.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target Oscilloscope_VST3 OscilloscopeSync_VST3
```

Tests : `-DOSC_BUILD_TESTS=ON`, puis `ctest --test-dir build -C Release`.
