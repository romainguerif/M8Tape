# Eventide H3000 — Recherche complète et plan de reproduction sur TrimUI Brick

> Objectif : comprendre le fonctionnement du H3000 Ultra-Harmonizer, identifier
> quels algorithmes sont open source ou reproductibles en code, et spécifier le
> module de traitement d'un **éditeur de samples sur TrimUI Brick** (Allwinner
> A133P, Linux).
>
> **Décision de design (validée).** L'éditeur n'offre **qu'une seule option de
> traitement : le H3000, reproduit dans son mode de fonctionnement d'origine,
> rien de plus, rien de moins.** Le moteur est mono-programme : **une passe =
> un seul algorithme**, rendu de façon destructive sur le sample. Pour empiler
> des effets, l'utilisateur relance une passe — exactement comme sur le matériel
> d'origine. Il n'y a **pas** de rack libre dans le moteur ; le chaînage est le
> fait de l'utilisateur, passe après passe.

---

## 1. Fonctionnement du matériel d'origine

### 1.1 Architecture

Le H3000 est une machine à deux étages strictement séparés :

- **Hôte / contrôle** : un Motorola **6809** (8 bits) gère l'écran LCD, les
  encodeurs, le MIDI et les paramètres. Il ne traite jamais l'audio lui-même ;
  il pousse des instructions vers l'étage DSP.
- **Audio / DSP** : **trois Texas Instruments TMS32010** en virgule fixe
  exécutent les algorithmes audio. Les algorithmes vivent dans les ROMs
  (SROM2 / SROM3, désignées « ALGORITHMS » dans le manuel de service).

Conséquence directe pour la reproduction : le code audio réel est de l'assembleur
TMS32010 16 bits virgule fixe, **jamais désassemblé ni publié**. Le seul travail
public de rétro-ingénierie (Hübner, mod OLED) porte sur le firmware 6809
(affichage), pas sur le DSP. Il n'existe donc **aucun portage copier-coller** ;
on reconstruit les algorithmes à partir de leur description fonctionnelle.

### 1.2 Modèle d'exécution : mono-programme

Le H3000 n'est pas un pedalboard. On charge **un seul algorithme (programme) à la
fois**. La touche ORIGIN affiche l'algorithme sur lequel repose le preset courant.

- ~20 à 24 algorithmes selon les modèles (S / SE / B / B+ / D-SX / D-SE / H3500).
- Un **preset** = un algorithme + un jeu de valeurs de paramètres mémorisées.
- Les 1000 presets se répartissent donc sur une vingtaine d'algorithmes seulement.

### 1.3 « Empilement » d'effets : à l'intérieur d'un algorithme

L'empilement se fait *dans* l'algorithme, qui contient plusieurs modules figés.
Exemples :

| Algorithme   | Modules internes                                                        |
|--------------|------------------------------------------------------------------------|
| Multi-Shift  | 2 pitch shifters 6 octaves + 2 delays + panning + feedback patchable    |
| Band Delay   | 1 ligne multitap + 8 filtres passe-bande résonants                      |
| Dual Shift   | 2 pitch shifters indépendants (recrée 2× H910)                          |
| Patch Factory| kit modulaire : delays, filtres, pitch shifting, routing patchable      |
| Mod Factory  | kit modulaire dynamique : ducking, BPM delays, sweeps, compression, etc.|

Les seuls algorithmes réellement « libres » sont les **kits de construction**
Patch Factory et Mod Factory. Le plugin moderne **H3000 Factory** (descendant de
Patch Factory) en donne le plafond : jusqu'à **18 blocs** patchables (paires de
delays, pitch shifters, filtres, mixeurs, VCA/AM, 2 LFO, enveloppes, Function
Generator à 19 formes d'onde). Le matériel d'origine était plus restreint
(1 pitch shifter, LFO limité au seul Function Generator).

**Limite réelle du nombre de modules** : le budget de calcul des 3 TMS32010 en
virgule fixe. On ajoute des modules jusqu'à épuisement des MIPS. En pratique, de
l'ordre d'une douzaine de modules dans les algos construction-kit.

---

## 2. Le pitch-shifting : le cœur, et la seule vraie subtilité

C'est la signature du H3000 et le module le plus délicat à reproduire fidèlement.

### 2.1 Principe (méthode temporelle)

- Une **delay line** lue par deux taps qui se déplacent à une vitesse différente
  de l'écriture (ratio de transposition).
- **Crossfade** entre les deux taps pour masquer la discontinuité quand un tap
  « rattrape » l'autre.
- Un **de-glitcher par autocorrélation** choisit les points de splice : il aligne
  les segments en phase pour minimiser les annulations pendant le crossfade.

Compromis fondamental : crossfade long = coloration métallique (comb filtering
entre les deux taps) ; crossfade court = glitch audible (chute de volume à la
jonction). Le de-glitcher cherche le meilleur point de coupe par similarité de
phase (vraie autocorrélation, pas seulement les passages par zéro).

### 2.2 Lignée et « grain »

- **H910** (1975) : 2-tap, crossfade triangulaire simple, **pas** de de-glitch.
  Son horloge maître était un oscillateur LC (pas un quartz) : tout le système
  dérivait lentement et de façon imprévisible. Cette instabilité fait partie du
  son.
- **H949** (1977) : premier pitch shifter « deglitché » par autocorrélation
  (carte ALG-3, brevet Anthony Agnello).
- **H3000** : pitch shifting diatonique, stéréo, multi-octaves, hérité de cette
  méthode.

Le « son H3000 » tient autant à ces imperfections (convertisseurs ~16 bits
d'époque, filtres, drift d'horloge, timing de splice) qu'à l'algorithme nu.
Reproduire l'algo est facile ; reproduire le grain demande de modéliser les
défauts.

### 2.3 Bibliothèques open source prêtes à l'emploi

| Lib                 | Langage | Méthode                 | Notes |
|---------------------|---------|-------------------------|-------|
| Signalsmith Stretch | C++11   | hybride spectral/temps  | Wrapper Rust dispo (Colin Marc), WASM, NPM, Python |
| stftPitchShift      | C++/Py  | phase vocoder + formants| Pitch poly, préservation de formants |
| TarsosDSP           | Java    | WSOLA + STFT shifter    | Pédagogique, deux implémentations |
| Faust (`pitch_shifter.dsp`) | Faust | delay-line 2-tap | Le plus proche de la méthode H910/H949, compile en C/Rust/WASM |

Pour coller au caractère temporel original, l'approche Faust 2-tap (ou une
réimplémentation Rust de la delay-line + crossfade + recherche de splice par
autocorrélation) est plus fidèle qu'un phase vocoder spectral.

---

## 3. Reproductibilité, algorithme par algorithme

| Famille / algo | Brique DSP | Open source dispo | Difficulté |
|----------------|-----------|-------------------|------------|
| MicroPitch / Micro-shift | 2 voix détunées (±cents) + delays courts pannés | trivial à coder | ★ |
| Dual / Stereo / Layered Shift | pitch shifter ×N + mix | Signalsmith, Faust | ★★ |
| Diatonic Shift | pitch shifter + table d'intervalles diatoniques | logique simple | ★★ |
| Reverse / Crystal Echoes | buffers inversés + pitch + feedback | (cf. Soundtoys Crystallizer pour la référence sonore) | ★★★ |
| Reverbs (Reverb Factory, Dense Room, Swept Reverb, Swept Combs) | FDN / Schroeder / Dattorro | Faust `reverbs.lib`, CloudSeed | ★★ |
| Ultra-Tap | delay multitap à enveloppes | trivial | ★ |
| Delays (Long / Dual Digiplex) | delay + feedback + filtrage | trivial | ★ |
| Band Delay | multitap + banc 8× bandpass résonants | filtres standard | ★★ |
| Phaser | all-pass modulés en série | standard | ★★ |
| Vocoder | banc d'analyse + porteuse + banc de synthèse | standard | ★★★ |
| String Modeler | Karplus-Strong / waveguide | bien documenté | ★★★ |
| Stutter | capture/replay de buffer rythmé | facile (logique) | ★★ |
| Timesqueeze | time-stretch temporel | WSOLA (TarsosDSP) | ★★★ |
| Mod / Patch Factory | graphe de modules patchables | architecture à construire | ★★ (concept) |
| **Le grain exact** | convertisseurs, drift, timing de splice | non documenté | ★★★★★ |

### Recommandations de priorité

1. **MicroPitch** d'abord : ±cents, delays ~15–25 ms, panning dur. Réglage type
   connu : un côté −9 cents / 15 ms, l'autre +11 cents / 25 ms. Effet iconique,
   1 h de dev.
2. **Delays / Ultra-Tap / reverbs FDN** : rendement maximal, coût minimal.
3. **Pitch shifter générique** (Signalsmith ou delay-line maison) : débloque
   Dual/Stereo/Layered/Diatonic Shift d'un coup.
4. **Reverse / Crystal** : une fois le pitch shifter en place.
5. **Architecture Patch Factory** : un mini-graphe de modules en série/parallèle
   = l'esprit de la machine, et le plus utile pour un processeur de samples.

---

## 4. Module de traitement dans l'éditeur de samples (TrimUI Brick)

### 4.1 Modèle de fonctionnement : la passe = un algorithme

Le traitement reproduit le **mode mono-programme** du H3000. Le déroulé exact,
identique au matériel :

1. l'utilisateur sélectionne un sample ;
2. il choisit **un** algorithme H3000 ;
3. il règle **les paramètres exacts de cet algorithme** (noms, plages, unités du
   manuel H3500) ;
4. il prévisualise, puis **rend** la passe de façon destructive sur le sample ;
5. pour empiler un autre effet, il **relance une passe** sur le résultat.

Aucun rack interne, aucun routing libre, aucun ajout par rapport au hardware. Le
chaînage d'effets est le fait de l'utilisateur, passe après passe — exactement
comme on enchaînait les programmes sur un H3000 réel. Conserver l'historique des
passes (non destructif côté projet) est un confort moderne acceptable tant qu'il
ne change pas le rendu.

### 4.2 Contraintes matérielles (vérifiées)

- **SoC** : Allwinner A133P (A133 Plus), 4× ARM Cortex-A53 @ 1.8 GHz, ARMv8-A
  64 bits → **NEON SIMD disponible** (idéal pour le DSP virgule flottante).
- **RAM** : 1 Go (LPDDR3/LPDDR4x selon lot) — surveiller l'empreinte mémoire.
- **OS** : Linux (firmwares communautaires : NextUI, etc.).
- **Audio** : sortie 3.5 mm stéréo ; **pas de vraie entrée ligne**.

Le rendu de passe est **hors-ligne** (fichier → fichier sur la carte SD), donc
les contraintes de latence temps réel ne s'appliquent pas au rendu ; seule la
prévisualisation casque demande un flux ALSA.

### 4.3 Fidélité : reproduire le mode d'origine, pas seulement l'algorithme

Pour « exactement l'original », au-delà de la logique de l'algorithme :

- **Paramètres** : strictement ceux du hardware pour chaque algorithme. Pas de
  second pitch shifter façon plugin, pas de LFO supplémentaire, pas d'extension
  de plage. La référence est le manuel H3500 (schémas-blocs + paramètres).
- **Caractère du splice** (pitch-shifting) : proposer les modes de splice
  d'époque — `H910` (sans de-glitch, avec drift d'horloge) et `H949`
  (de-glitch par autocorrélation). C'est le facteur sonore dominant.
- **Grain** : modéliser la bande passante / le comportement des convertisseurs
  d'époque. Optionnel mais c'est ce qui sépare « techniquement juste » de
  « sonne comme un H3000 ».
- **Fréquence d'échantillonnage** : le caractère du pitch dépend du taux ;
  décider si l'on traite au taux du sample ou si l'on reproduit le comportement
  interne de la machine (à arbitrer lors de l'implémentation du cœur pitch).

### 4.4 Architecture logicielle (rendu de passe)

```
+-------------------------------------------------------------+
|  Éditeur de samples (UI gamepad-native, 1024x768, 4:3)      |
|   - sélection sample / algorithme H3000 / preset            |
|   - édition des paramètres natifs de l'algorithme           |
|   - aperçu A/B, RENDU de la passe, export                   |
+----------------------------+--------------------------------+
                             |  (un seul algorithme par passe)
+----------------------------v--------------------------------+
|  Moteur DSP H3000 (virgule flottante f32, NEON)             |
|   - 1 algorithme = 1 structure de modules FIGÉE (manuel)    |
|   - cœur pitch-shifting (splice H910/H949) partagé          |
|   - modules : delay, filtres, AM, LFO/Function Generator... |
+----------------------------+--------------------------------+
                             |
+----------------------------v--------------------------------+
|  I/O : décodage/encodage WAV, carte SD, ALSA (aperçu)       |
+-------------------------------------------------------------+
```

Recommandations techniques :

- Moteur DSP **isolé du reste de l'éditeur** derrière une interface simple
  (`process(sample, algo_id, params) -> sample`), pour que le langage de l'UI et
  celui du DSP puissent diverger si besoin.
- Cible **`aarch64-unknown-linux-gnu`** (ou `-musl` pour un binaire statique
  portable entre firmwares).
- **Distribution via PortMaster** (déjà utilisé pour m8c / LGPT).
- Chaque algorithme = une **structure de modules figée** fidèle au schéma-bloc
  du manuel, et **non** un rack reconfigurable.

### 4.5 Jalons

1. **Cœur pitch-shifting fidèle** : delay-line 2-tap + crossfade + de-glitch par
   autocorrélation, avec modes de splice `H910` / `H949`. C'est la fondation de
   ~la moitié des algorithmes.
2. **Premier algorithme complet** (selon décision en cours) câblé sur ce cœur,
   avec ses paramètres natifs.
3. **Rendu de passe** destructif (WAV → WAV) + aperçu A/B.
4. **Catalogue d'algorithmes** étendu un à un, chacun spécifié depuis le manuel.
5. **Prévisualisation ALSA** + packaging PortMaster.

---

## 5. Liens et références

### Manuels officiels (descriptions des algorithmes, schémas-blocs)

- Manuel d'instruction H3000 Series (tous modèles combinés, 184 p.) — ManualsLib :
  https://www.manualslib.com/manual/3652672/Eventide-Ultra-Harmonizer-H3000-Series.html
- Même manuel (téléchargement PDF, 182 p.) — manualzz :
  https://manualzz.com/doc/6799538/eventide-h3000-series-user-manual
- Manuel d'instruction H3500 (algos supplémentaires) — Scribd :
  https://www.scribd.com/document/624755664/Eventide-H3500-instruction-manual
- **Manuel technique / service** (architecture, block diagrams, signal flow) —
  Internet Archive :
  https://archive.org/details/eventide-h3000-series-technical-service-reference-manual
- Texte intégral du Service Manual (config EPROM, block diagram) :
  https://archive.org/stream/H3000SvcMan/H3000%20Svc%20Man_djvu.txt

### Compréhension des algorithmes de pitch-shifting

- ValhallaDSP — H949 et le « de-glitching » (autocorrélation, splice) :
  https://valhalladsp.com/2010/05/07/pitch-shifting-the-h949-and-de-glitching/
- ValhallaDSP — H910, le premier Harmonizer :
  https://valhalladsp.com/2010/05/07/early-pitch-shifting-the-eventide-h910-harmonizer/
- Manuel Eventide H90, page Pitch (Eventide y décrit les « Splice Types »
  H910 / H949 émulés) :
  https://cdn.eventideaudio.com/manuals/h90/1.1.4/content/algorithms/pitch.html
- Méthodes temporelles de pitch/time-stretch (PSOLA, WSOLA) — katjaas :
  https://www.katjaas.nl/pitchshift/pitchshift.html
- Brevet Eventide time/pitch scaling (splice, cross-fade, read pointers) :
  https://patents.justia.com/patent/6049766

### Aperçus techniques et tables de référence

- Reverb News — « The tech behind the H3000 » (6809 + 3× TMS32010) :
  https://reverb.com/news/tech-behind-eventide-h3000-ultra-harmonizer
- Table complète algorithmes / presets par révision — donsolaris :
  https://www.donsolaris.com/?p=3695
- Rétro-ingénierie firmware 6809 (mod OLED, désassemblage hôte) — Hübner :
  https://huebnerie.de/2019/09/eventide-h3000-h3500-oled-update/

### Bibliothèques open source (briques de reconstruction)

- Signalsmith Stretch (pitch/time, C++ ; wrappers Rust/WASM/Python) :
  https://github.com/Signalsmith-Audio/signalsmith-stretch
- stftPitchShift (phase vocoder + formants, C++/Python) :
  https://github.com/jurihock/stftPitchShift
- TarsosDSP (WSOLA, STFT shifter, Java — pédagogique) :
  https://github.com/JorenSix/TarsosDSP
- Faust `reverbs.lib` (Freeverb, FDN, Zita, Dattorro) :
  https://faustlibraries.grame.fr/libs/reverbs/
- CloudSeed (réverbe algorithmique de référence) :
  https://github.com/ValdemarOrn/CloudSeed
- CloudReverb (portage JUCE de CloudSeed, Linux/macOS) :
  https://github.com/xunil-cloud/CloudReverb
- Liste OpenAudio (catalogue de plugins/libs open source) :
  https://github.com/webprofusion/OpenAudio

### Spécifications TrimUI Brick

- Specs détaillées (A133 Plus, 4× Cortex-A53, PowerVR GE8300) — Retro Catalog :
  https://retrocatalog.com/retro-handhelds/trimui-smart-brick

---

## 6. Synthèse en une phrase

Aucun code source officiel des algorithmes du H3000 n'est disponible, mais ils
reposent tous sur des primitives DSP classiques et bien documentées. Le module
de traitement de l'éditeur reproduit fidèlement le **mode mono-programme** de la
machine : une passe = un algorithme, rendu destructif, empilement à la main par
passes successives. Le vrai défi n'est pas l'algorithme mais la restitution du
caractère d'origine (modes de splice H910/H949, grain des convertisseurs).
