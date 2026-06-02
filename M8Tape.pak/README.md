# M8Tape - enregistreur multicanal pour TrimUI (NextUI/MinUI)

Enregistre les 24 canaux de la Dirtywave M8 et les decoupe en douze fichiers
stereo, ranges dans un dossier par prise. Interface : menu natif NextUI via
minui-list. Capture : arecord. Decoupage : m8split (ou sox). Format fige par le
diagnostic : 24 canaux, S24_3LE (24 bits packes), 44100 Hz.

## Ce que tu obtiens
Pour chaque prise, un dossier dans le tool :
  M8Tape.pak/rec_AAAAMMJJ_HHMMSS/01.wav ... 12.wav
Douze fichiers stereo (paire N = canaux 2N-1 et 2N). Le WAV 24 canaux brut est
supprime apres un decoupage reussi.

## Installation
1. Copier le dossier M8Tape.pak dans Tools/tg5040/.
2. Deposer les deux binaires tg5040 dans M8Tape.pak/bin/tg5040/ (voir
   bin/tg5040/PLACE_BINARIES_HERE.txt) :
   - minui-list (releases GitHub josegonzalez/minui-list, asset tg5040),
   - m8split (compiler la source m8split-rs, voir son BUILD.md).
   chmod +x sur les deux.
3. chmod +x sur launch.sh.

## Utilisation
Brancher la M8 (port USB-C du haut, cable data, sortie USB audio active), aller
dans Tools > M8Tape. Demarrer lance la prise ; pendant l'enregistrement le menu
affiche Arreter, le temps ecoule et le nom du dossier. Arreter stoppe, arecord
ferme le WAV, puis le decoupage produit les douze stereo.

## Ou sont les fichiers
M8Tape.pak/rec_*/, a rapatrier en SSH. (Emplacement modifiable via OUTDIR en haut
de launch.sh.)

## Replis
- Pas de minui-list : mode bascule (un lancement demarre, le suivant arrete),
  sans menu.
- Pas de m8split : si sox est present sur l'appareil il decoupe a sa place ;
  sinon le WAV 24 canaux brut est conserve, non decoupe.
- En cas de souci, state/status.txt et state/arecord.log donnent l'etat et les
  erreurs.

## Limites
- Carte FAT32 : la prise brute est plafonnee a 4 Go (~21 min) avant decoupage ;
  en exFAT, pas de limite.

## Note d'honnetete
Tout l'enchainement (dossier par prise, capture, decoupage en douze stereo,
suppression du brut) a ete simule et verifie hors appareil, et la logique de
m8split testee sur de vrais WAV 24 canaux. Restent a confirmer sur la Brick la
capture reelle par arecord et le rendu de minui-list.

## Mode d'ecriture
Par defaut : enregistrement DIRECT sur la carte, sans limite de duree, arret
immediat puis decoupage. Concu pour une carte rapide (V30 ou mieux).

Filet pour carte lente : si tu crees un fichier vide nomme `use_ram` dans ce
dossier (M8Tape.pak/use_ram), M8Tape enregistre en memoire vive puis recopie sur
la carte a l'arret. Ca supprime les craquements d'une carte lente, mais limite
la prise a la RAM disponible (~3 min sur la Brick) et l'arret prend le temps de
la copie. Supprime le fichier use_ram pour revenir au mode direct.
