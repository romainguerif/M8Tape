#!/bin/sh
# split_m8.sh - decoupe un WAV 24 canaux en 12 fichiers stereo, sur Mac/PC.
# Aucune compilation : utilise ffmpeg ou sox (au choix, selon ce qui est installe).
#
# Usage : ./split_m8.sh prise_24ch.wav [dossier_sortie]
#   defaut dossier_sortie : a cote du fichier, suffixe _stems

IN="$1"
[ -z "$IN" ] && { echo "usage: $0 fichier_24ch.wav [dossier_sortie]"; exit 1; }
[ -f "$IN" ] || { echo "introuvable : $IN"; exit 1; }

base="$(basename "$IN")"; base="${base%.*}"
OUT="${2:-$(dirname "$IN")/${base}_stems}"
mkdir -p "$OUT"

if command -v ffmpeg >/dev/null 2>&1; then
    i=1; c=0
    while [ "$c" -lt 24 ]; do
        out="$(printf '%s/%02d.wav' "$OUT" "$i")"
        ffmpeg -y -loglevel error -i "$IN" \
            -af "pan=stereo|c0=c${c}|c1=c$((c + 1))" \
            -c:a pcm_s24le "$out" </dev/null
        c=$((c + 2)); i=$((i + 1))
    done
elif command -v sox >/dev/null 2>&1; then
    i=1; p=1
    while [ "$p" -le 24 ]; do
        out="$(printf '%s/%02d.wav' "$OUT" "$i")"
        sox "$IN" "$out" remix "$p" "$((p + 1))"
        p=$((p + 2)); i=$((i + 1))
    done
else
    echo "Ni ffmpeg ni sox. Installe l'un des deux, par ex. : brew install ffmpeg"
    exit 1
fi

echo "12 fichiers stereo ecrits dans : $OUT"
