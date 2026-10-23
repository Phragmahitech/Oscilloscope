# Oscilloscope by Phragma

Oscilloscope VST3 multi-pistes, calé sur le tempo, qui compense automatiquement la latence des plugins de la piste.

Deux plugins :

- **Oscilloscope** : à mettre où on veut regarder (en général en fin de chaîne). Chaque fenêtre montre toutes les instances du projet. Chaque nouvelle instance reçoit une couleur différente.
- **Oscilloscope Sync** : à mettre **en premier** sur la piste. Il ne touche pas au son. L'Oscilloscope de la même piste compare ce que le Sync reçoit avec ce qu'il reçoit lui-même et en déduit, à l'échantillon près, le retard des plugins placés entre les deux. La mesure tourne en continu : ajouter ou retirer un plugin à latence est pris en compte en une à deux secondes. Sans Sync sur la piste, la trace n'est pas compensée.

Réglages de la fenêtre :

- **TIME** : 1/32 à 4/1, **1/4 par défaut**, calé sur la grille de l'hôte (balayage).
- **VIEW** : OVERLAY (traces superposées, pour aligner les phases), SPLIT (une ligne par piste), SUM (somme des traces visibles, compensées).
- **CHANNEL** : MID, L, R, SIDE. **GAIN** : ±24 dB (double-clic : 0). **FREEZE** : fige l'image.
- Liste : clic sur la pastille = couleur ; œil = afficher / masquer ; double-clic sur le nom = renommer ; clic sur la ligne de latence = compensation AUTO ou OFF.

Limites connues : un passe-bas très raide et très bas (24 dB/oct vers 150 Hz) entre le Sync et l'Oscilloscope peut empêcher la mesure (la dernière valeur est gardée) ; un son parfaitement périodique (sinus pur) ne permet pas de mesurer (retard ambigu) ; les instances doivent tourner dans le même processus (normal dans Live).

JUCE 8, C++17, CMake. Le moteur (`Source/Hub.h`, `Tap.h`, `LatencyFinder.h`, `Trace.h`) n'utilise pas JUCE. Les polices (Michroma, IBM Plex Mono, licence OFL : voir `Assets/`) sont téléchargées à la configuration depuis `google/fonts` à un commit fixe et vérifiées par SHA-256.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target Oscilloscope_VST3 OscilloscopeSync_VST3
```

Tests : `-DOSC_BUILD_TESTS=ON`, puis `ctest --test-dir build -C Release`.
