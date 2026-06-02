# 🎛️ M8Tape

**Enregistreur multicanal de la [Dirtywave M8](https://dirtywave.com/) pour la TrimUI Brick** (NextUI / MinUI).

Branche ta M8 sur la console, ouvre M8Tape depuis le menu Tools, appuie sur
*Démarrer* : les **24 canaux audio** de la M8 sont capturés d'un bloc, puis
découpés automatiquement en **12 pistes stéréo** prêtes à mixer. Une console de
poche devient un enregistreur multipiste.

```
┌──────────────┐   USB-C audio    ┌──────────────┐   arecord    ┌─────────────────┐
│  Dirtywave   │  24 ch / 44.1k   │  TrimUI      │  S24_3LE     │  rec_AAAAMMJJ/  │
│  M8          │ ───────────────▶ │  Brick       │ ───────────▶ │  01.wav … 12.wav│
└──────────────┘                  │  (M8Tape)    │   m8split    └─────────────────┘
                                   └──────────────┘  24ch→12×2
```

---

## ✨ Ce que ça fait

- **Capture native 24 canaux** de la M8 (la sortie USB audio multicanal de
  l'appareil), figée par diagnostic sur le format qui marche : **24 canaux,
  S24_3LE (24 bits packés), 44 100 Hz**.
- **Découpe automatique** du flux 24 voies en **12 fichiers stéréo**
  (`01.wav` … `12.wav`), une paire par sortie de la M8 : la paire *N* regroupe
  les canaux *2N-1* et *2N*.
- **Un dossier par prise**, horodaté : `rec_AAAAMMJJ_HHMMSS/`.
- **Menu natif NextUI** via `minui-list` : Démarrer / Arrêter, temps écoulé,
  dossier courant. Pas de menu détecté ? Repli en mode bascule (un lancement
  démarre, le suivant arrête).
- **Pensé pour la fiabilité de la capture temps réel** : réglages anti-glitch du
  contrôleur USB, écriture carte en cache actif, priorité process élevée, et un
  mode RAM optionnel pour les cartes lentes.

À la fin d'une prise réussie, le WAV 24 canaux brut est supprimé : il ne reste
que les douze stéréo.

---

## 🧰 Matériel requis

- Une **TrimUI Brick** sous **NextUI** (ou MinUI compatible `tg5040`).
- Une **Dirtywave M8** (Model:01 ou M8 Tracker) avec la **sortie USB audio
  activée**.
- Un **câble USB-C data** (pas un câble charge seule) entre la M8 et le **port
  USB-C du haut** de la Brick.
- Une **carte microSD rapide** (V30 ou mieux) recommandée pour l'enregistrement
  direct. Une carte lente reste utilisable via le mode RAM (voir plus bas).

---

## 📦 Installation

1. **Copier le pak** : place le dossier `M8Tape.pak` dans `Tools/tg5040/` de ta
   carte SD.

2. **Déposer les deux binaires** `tg5040` dans `M8Tape.pak/bin/tg5040/`
   (détails dans [`bin/tg5040/PLACE_BINARIES_HERE.txt`](bin/tg5040/PLACE_BINARIES_HERE.txt)) :

   | Binaire | Rôle | Où le trouver |
   |---|---|---|
   | `minui-list` | menu natif | [releases josegonzalez/minui-list](https://github.com/josegonzalez/minui-list/releases/latest) → asset `tg5040` |
   | `m8split` | découpe 24ch → 12 stéréo | à compiler (source `m8split-rs`, voir son `BUILD.md`) |

   > Ces binaires ne sont **pas** inclus dans le dépôt (binaire tiers / à
   > compiler). `minui-list` accepte le nom `minui-list` ou `minui-list-tg5040`.

3. **Rendre exécutables** : `chmod +x` sur `launch.sh` et sur les deux binaires.

C'est tout : M8Tape apparaît dans **Tools > M8Tape**.

---

## 🎬 Utilisation

1. Branche la M8 (port USB-C du haut, câble data, sortie USB audio active).
2. Va dans **Tools > M8Tape**.
3. **Démarrer** lance la prise. Pendant l'enregistrement, le menu affiche
   *Arrêter*, l'heure de début et le nom du dossier courant.
4. **Arrêter** stoppe proprement : `arecord` finalise l'en-tête WAV, puis la
   découpe produit les douze fichiers stéréo.

Tu peux quitter le menu en laissant tourner l'enregistrement (*Quitter (laisse
tourner)*) et revenir l'arrêter plus tard.

### Où sont les fichiers
```
M8Tape.pak/rec_AAAAMMJJ_HHMMSS/
├── 01.wav   ← canaux 1+2
├── 02.wav   ← canaux 3+4
├── …
└── 12.wav   ← canaux 23+24
```
À rapatrier en SSH. L'emplacement est modifiable via `OUTDIR` en haut de
[`launch.sh`](launch.sh).

---

## 🛠️ Comment ça marche (et pourquoi)

L'enregistrement audio temps réel sur un appareil bridé comme la Brick, c'est
surtout une lutte contre les *xruns* (sauts/craquements). M8Tape applique
plusieurs réglages issus du diagnostic terrain :

- **Anti-glitch USB (contrôleur Allwinner / `sunxi-ehci`)** :
  `snd_usb_audio.nrpacks=1` rend les transferts isochrones plus petits et plus
  fréquents (timing plus lisse), et l'autosuspend USB est désactivé pour que le
  bus ne se mette pas en veille pendant la capture.
- **Carte en cache actif** : NextUI monte la carte en `sync`, ce qui effondre le
  débit d'écriture (mesuré ~1,3 Mo/s en `sync` contre ~24 Mo/s en `async`).
  M8Tape remonte la carte en `async` le temps de l'enregistrement, puis restaure
  `sync` à l'arrêt (avec un `sync` explicite pour ne rien perdre).
- **Priorité process élevée** (`nice -n -19`) et **gros tampons** demandés à
  `arecord` (`--buffer-time` / `--period-time`) pour absorber les à-coups.
- **Journal verbeux** (`state/arecord.log`) pour tracer les xruns éventuels.

### Découpe 24 → 12
À l'arrêt, le WAV 24 canaux est découpé en 12 stéréo :
- via **`m8split`** (binaire dédié, rapide) si présent ;
- sinon via **`sox`** s'il est installé sur l'appareil (`remix` paire par paire) ;
- sinon le **WAV 24 canaux brut est conservé** intact, non découpé.

### Modes d'écriture
- **Direct carte (défaut)** : écrit directement sur la SD, sans limite de durée,
  arrêt immédiat puis découpe. Conçu pour une carte rapide (V30+).
- **Mode RAM (filet pour carte lente)** : crée un fichier vide nommé `use_ram`
  dans `M8Tape.pak/` → la prise est enregistrée en mémoire vive puis recopiée sur
  la carte à l'arrêt. Supprime les craquements d'une carte lente, **mais** limite
  la durée à la RAM dispo (~3 min sur la Brick) et l'arrêt prend le temps de la
  copie. Supprime `use_ram` pour revenir au mode direct.

---

## 🩺 Replis & dépannage

| Situation | Comportement |
|---|---|
| Pas de `minui-list` | Mode bascule (1 lancement = démarre, le suivant = arrête), sans menu. |
| Pas de `m8split` | `sox` prend le relais s'il est là ; sinon le WAV 24ch brut est conservé. |
| `arecord` introuvable | Statut écrit dans `state/status.txt`, rien n'est enregistré. |
| Souci pendant la prise | `state/status.txt` (état courant) et `state/arecord.log` (erreurs / xruns). |

---

## ⚠️ Limites

- **Carte FAT32** : la prise brute est plafonnée à **4 Go (~21 min)** avant
  découpe. En **exFAT**, pas de limite.
- **Mode RAM** : durée bornée par la RAM libre (~3 min sur la Brick).

---

## 🔎 Note d'honnêteté

Tout l'enchaînement (dossier par prise, capture, découpe en douze stéréo,
suppression du brut) a été **simulé et vérifié hors appareil**, et la logique de
`m8split` testée sur de vrais WAV 24 canaux. **Restent à confirmer sur la Brick**
la capture réelle par `arecord` et le rendu de `minui-list`.

---

## 📁 Structure du dépôt

```
M8Tape.pak/
├── launch.sh        # tout le pak : capture, anti-glitch, découpe, replis
├── pak.json         # métadonnée NextUI (label)
├── README.md        # ce fichier
└── bin/tg5040/      # déposer ici minui-list et m8split (non fournis)
```

---

*Construit pour faire d'une TrimUI Brick un enregistreur multipiste de poche pour
la Dirtywave M8. Bricolage assumé — PRs et retours terrain bienvenus.*
