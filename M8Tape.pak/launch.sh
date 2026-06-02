#!/bin/sh
# M8Tape - enregistreur multicanal de la Dirtywave M8 pour TrimUI (NextUI/MinUI).
# Interface : menu natif via minui-list. Capture : arecord. Aucun fichier audio
# n'est traite, juste ecrit. Arret propre (arecord finalise l'en-tete WAV).

PAK="$(dirname "$0")"
PLATFORM="${PLATFORM:-tg5040}"

OUTDIR="$PAK"                       # les prises sont ecrites dans le dossier du tool
STATE="$PAK/state"
PIDFILE="$STATE/arecord.pid"
STARTFILE="$STATE/start.txt"
CURFILE="$STATE/current.txt"
CURDIR="$STATE/curdir.txt"
STAGEFILE="$STATE/stage.txt"
LOG="$STATE/arecord.log"
STATUS="$STATE/status.txt"
mkdir -p "$OUTDIR" "$STATE" 2>/dev/null

# Parametres de capture, figes par le diagnostic : 24 canaux, 24 bits packes, 44.1k.
DEV="hw:M8"
CH=24
FMT=S24_3LE
RATE=44100

# --- localisation des outils ---
LIST=""
for c in "$PAK/bin/$PLATFORM/minui-list" "$PAK/bin/$PLATFORM/minui-list-$PLATFORM" "$PAK/minui-list" minui-list; do
    if command -v "$c" >/dev/null 2>&1; then LIST="$(command -v "$c")"; break; fi
done
ARECORD="$(command -v arecord 2>/dev/null)"

# Priorite haute pour la capture, afin de limiter les preemptions (cause de sauts).
PRIO=""
command -v nice >/dev/null 2>&1 && PRIO="nice -n -19"

# La carte est montee en 'sync' par NextUI, ce qui effondre le debit d'ecriture
# (mesure : 1,3 Mo/s en sync contre 24 en async). On bascule en cache actif le
# temps de l'enregistrement, puis on restaure 'sync' a l'arret.
MP="$(awk '$2 ~ /SDCARD/{print $2; exit}' /proc/mounts 2>/dev/null)"
[ -z "$MP" ] && MP="/mnt/SDCARD"
remount_async() { mount -o remount,rw,async "$MP" 2>/dev/null; }
remount_sync()  { sync; mount -o remount,rw,sync "$MP" 2>/dev/null; }

# Outil de decoupage 24ch -> 12 stereo : binaire m8split bundle, sinon sox.
SPLIT=""
for c in "$PAK/bin/$PLATFORM/m8split" "$PAK/m8split" m8split; do
    if command -v "$c" >/dev/null 2>&1; then SPLIT="$(command -v "$c")"; break; fi
done

# --- helpers etat ---
is_recording() {
    [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null
}

# Cherche un repertoire en RAM (tmpfs) pour eviter les a-coups de la carte.
find_ramdir() {
    for d in /dev/shm /run/shm /tmp /run; do
        [ -d "$d" ] && [ -w "$d" ] || continue
        awk -v m="$d" '$2==m && $3=="tmpfs"{f=1} END{exit !f}' /proc/mounts 2>/dev/null \
            && { echo "$d"; return 0; }
    done
    return 1
}

start_rec() {
    [ -z "$ARECORD" ] && { echo "arecord introuvable" > "$STATUS"; return 1; }
    TS="$(date '+%Y%m%d_%H%M%S' 2>/dev/null)"
    REC_DIR="$OUTDIR/rec_$TS"
    mkdir -p "$REC_DIR" 2>/dev/null
    RAW="$REC_DIR/_raw_24ch.wav"
    echo "$REC_DIR" > "$CURDIR"
    echo "$RAW" > "$CURFILE"
    date '+%s' > "$STARTFILE" 2>/dev/null
    date '+%H:%M:%S' > "$STATE/starthuman.txt" 2>/dev/null

    # Par defaut : enregistrement direct sur la carte (carte rapide, sans limite).
    # Mode RAM (filet pour carte lente) active seulement si le fichier "use_ram"
    # existe dans le dossier du pak.
    RAMDIR=""
    [ -f "$PAK/use_ram" ] && RAMDIR="$(find_ramdir)"
    if [ -n "$RAMDIR" ]; then
        STAGE="$RAMDIR/m8tape_$$.wav"
        echo "$STAGE" > "$STAGEFILE"
        # Garde-fou : duree max calculee sur la RAM libre (70% / 3,2 Mo/s).
        avail_mb="$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo 2>/dev/null)"
        [ -z "$avail_mb" ] && avail_mb=256
        MAXSEC=$((avail_mb * 7 / 10 * 10 / 32))
        [ "$MAXSEC" -lt 10 ] && MAXSEC=10
        TARGET="$STAGE"; DUR="-d $MAXSEC"
        echo "enregistrement en RAM (max ${MAXSEC}s) -> copie carte a l'arret" > "$STATUS"
    else
        : > "$STAGEFILE"
        TARGET="$RAW"; DUR=""
        echo "enregistrement direct carte -> $REC_DIR" > "$STATUS"
    fi

    # --- Reglages anti-glitch pour le controleur USB Allwinner (sunxi-ehci) ---
    # nrpacks=1 : transferts isochrones plus petits/frequents, lisse le timing.
    # autosuspend off : evite la mise en veille du bus pendant la capture.
    echo 1  > /sys/module/snd_usb_audio/parameters/nrpacks 2>/dev/null
    echo -1 > /sys/module/usbcore/parameters/autosuspend 2>/dev/null
    for pc in /sys/class/sound/card*/device/../power/control; do
        [ -w "$pc" ] && echo on > "$pc" 2>/dev/null
    done
    {
        echo "reglages USB :"
        echo "  nrpacks = $(cat /sys/module/snd_usb_audio/parameters/nrpacks 2>/dev/null || echo '?')"
        echo "  usbcore.autosuspend = $(cat /sys/module/usbcore/parameters/autosuspend 2>/dev/null || echo '?')"
    } > "$STATE/usb.txt" 2>/dev/null

    # Cache actif sur la carte pour absorber les a-coups (cf. ecart sync/async).
    remount_async

    # Priorite haute + tampon large demande + journal verbeux pour tracer les xruns.
    $PRIO "$ARECORD" -v -D "$DEV" -c "$CH" -f "$FMT" -r "$RATE" $DUR \
        --buffer-time=2000000 --period-time=500000 \
        -t wav "$TARGET" >"$LOG" 2>&1 &
    echo $! > "$PIDFILE"
}

# Decoupe le WAV 24ch en 12 fichiers stereo 01.wav..12.wav dans DEST.
split_take() {
    RAW="$1"; DEST="$2"
    [ -f "$RAW" ] || { echo "pas de fichier brut a decouper" >> "$STATUS"; return 1; }

    if [ -n "$SPLIT" ]; then
        if "$SPLIT" "$RAW" "$DEST" >>"$LOG" 2>&1; then
            rm -f "$RAW"; echo "decoupe (m8split) -> $DEST" >> "$STATUS"; return 0
        fi
        echo "m8split a echoue, voir arecord.log ; WAV 24ch conserve" >> "$STATUS"; return 1
    fi

    if command -v sox >/dev/null 2>&1; then
        i=1; p=1; ok=1
        while [ "$p" -le 24 ]; do
            out="$(printf '%s/%02d.wav' "$DEST" "$i")"
            sox "$RAW" "$out" remix "$p" "$((p + 1))" >>"$LOG" 2>&1 || ok=0
            p=$((p + 2)); i=$((i + 1))
        done
        if [ "$ok" -eq 1 ]; then
            rm -f "$RAW"; echo "decoupe (sox) -> $DEST" >> "$STATUS"; return 0
        fi
        echo "sox a echoue, WAV 24ch conserve" >> "$STATUS"; return 1
    fi

    {
        echo "Aucun splitter trouve. WAV 24ch conserve : $RAW"
        echo "Compile m8split (voir README) et depose-le dans bin/$PLATFORM/, ou installe sox."
    } >> "$STATUS"
    return 1
}

stop_rec() {
    PID="$(cat "$PIDFILE" 2>/dev/null)"
    [ -n "$PID" ] && kill -TERM "$PID" 2>/dev/null   # arecord ferme proprement le WAV
    i=0
    while [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null && [ "$i" -lt 80 ]; do
        sleep 1; i=$((i + 1))
    done
    rm -f "$PIDFILE"
    RAW="$(cat "$CURFILE" 2>/dev/null)"
    DEST="$(cat "$CURDIR" 2>/dev/null)"
    STAGE="$(cat "$STAGEFILE" 2>/dev/null)"

    # Si enregistre en RAM, recopier vers la carte (non temps reel, les a-coups
    # de la carte ne posent plus de probleme ici).
    if [ -n "$STAGE" ] && [ -f "$STAGE" ]; then
        echo "arret, copie RAM -> carte..." > "$STATUS"
        cp "$STAGE" "$RAW" && rm -f "$STAGE"
    fi

    echo "arret, decoupage en cours..." > "$STATUS"
    split_take "$RAW" "$DEST"

    # Tout est ecrit : on vide le cache sur la carte et on restaure le montage sync.
    remount_sync
    echo "termine : $DEST" > "$STATUS"
}

elapsed() {
    if [ -f "$STARTFILE" ]; then
        now="$(date '+%s' 2>/dev/null)"; st="$(cat "$STARTFILE" 2>/dev/null)"
        s=$((now - st))
        printf '%02d:%02d:%02d' $((s / 3600)) $(((s % 3600) / 60)) $((s % 60))
    else
        echo "00:00:00"
    fi
}

# --- repli si minui-list absent : bascule simple, et on explique quoi faire ---
if [ -z "$LIST" ]; then
    if is_recording; then stop_rec; A="arrete"; else start_rec; A="demarre"; fi
    {
        echo "minui-list introuvable : mode bascule (1 lancement = $A)."
        echo "Pour le menu, depose le binaire tg5040 dans :"
        echo "  $PAK/bin/$PLATFORM/minui-list"
    } > "$STATUS"
    exit 0
fi

# --- boucle de menu ---
while true; do
    if is_recording; then
        ST="$(cat "$STATE/starthuman.txt" 2>/dev/null)"
        CUR="$(basename "$(cat "$CURDIR" 2>/dev/null)")"
        ITEMS="$(printf 'Arreter\nEnregistrement en cours\nDemarre a : %s\nDossier : %s\nQuitter (laisse tourner)' "$ST" "$CUR")"
    else
        ITEMS="$(printf 'Demarrer\nQuitter')"
    fi

    SEL="$(printf '%s\n' "$ITEMS" | "$LIST" --format text --file -)"
    RC=$?

    case "$SEL" in
        Demarrer) start_rec ;;
        Arreter)  stop_rec ;;
        Quitter*) break ;;
        "")       [ "$RC" -ne 0 ] && break ;;   # bouton B / retour : on sort
        *)        : ;;                          # lignes d'info : on redessine
    esac
done

exit 0
