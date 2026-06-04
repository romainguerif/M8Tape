// fx_diatonic.c - Diatonic Shift: the H3000 "Intelligent / Diatonic" harmonizer.
// The H3000 was the first box to do pitch shifting that STAYS IN KEY: you set a
// KEY + SCALE and ask for a diatonic INTERVAL (a "3rd", "5th"...), and instead of
// transposing by a fixed number of semitones it counts that many *scale steps*
// above the note you played. So in C major a "3rd" above A is C (a minor 3rd, 3
// semitones) but a "3rd" above C is E (a major 3rd, 4 semitones) - the shift
// amount changes with where the note sits in the scale. (See the Eventide manual
// "Diatonic Shift" and the harmony-note patent US 8,362,348: pick the scale,
// find the reference scale-note nearest the input, count `degree` scale-notes up.)
//
// To do that on raw audio we must KNOW the note being played, so this module
// carries its own monophonic pitch tracker (autocorrelation / McLeod-style
// normalized clarity, computed locally - see pd_* below) feeding the shared
// time-domain pitch core (pitchcore) for the actual shifting.
//
// FAITHFUL here: the diatonic scale-step interval logic and per-note variable
// semitone shift (that's the whole point of "Intelligent" mode), plus the
// shared H910/H949 splice character of the pitch core.
// MODELED / not claimed bit-exact: the H3000's exact pitch detector and tracking
// speed are undocumented; this uses a standard autocorrelation tracker tuned for
// monophonic sources (voice, bass, lead). Polyphonic input is out of scope -
// like the original, this is a monophonic harmonizer.
#include "h3000.h"
#include "pitchcore.h"
#include "dsputil.h"
#include <stdlib.h>
#include <math.h>

// Parameter indices - MUST match the params[] order in the def below.
enum { P_KEY, P_SCALE, P_INTERVAL, P_MIX, P_SPLICE };

// ---- UI choice lists --------------------------------------------------------
static const char *const KEY_CHOICES[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B", 0
};
static const char *const SCALE_CHOICES[] = {
    "MAJOR", "MINOR", "DORIAN", "MIXOLYDIAN", 0
};
// Diatonic degrees as the H3000 names them: counted in scale steps above the
// detected note. "3RD" = move up 2 scale steps, "5TH" = 4 steps, "OCT" = 7.
static const char *const INTERVAL_CHOICES[] = {
    "3RD", "4TH", "5TH", "6TH", "OCT", 0
};

// Scale patterns: semitone offset of each of the 7 degrees from the tonic.
// (Dorian / Mixolydian are common Eventide "modes"; cheap to add and useful.)
static const int SCALE_TBL[4][7] = {
    { 0, 2, 4, 5, 7, 9, 11 },  // MAJOR (ionian)
    { 0, 2, 3, 5, 7, 8, 10 },  // MINOR (natural / aeolian)
    { 0, 2, 3, 5, 7, 9, 10 },  // DORIAN
    { 0, 2, 4, 5, 7, 9, 10 },  // MIXOLYDIAN
};
// INTERVAL choice -> number of scale steps to climb (3RD=2, 4TH=3, 5TH=4, 6TH=5,
// OCT=7). One octave up = 7 scale steps in a 7-note scale.
static const int INTERVAL_STEPS[5] = { 2, 3, 4, 5, 7 };

// ---- pitch tracker geometry -------------------------------------------------
// Window must hold at least two periods of the lowest note we track. ~46 ms at
// 48 k (2048 samples) reaches down to ~43 Hz (low for bass/voice) with margin.
#define PD_WIN     2048   // analysis frame (samples)
#define PD_HOP      512   // re-estimate every this many samples (~11 ms @48k)
#define PD_FMIN    50.0f  // lowest f0 we will report (Hz)
#define PD_FMAX  1200.0f  // highest f0 we will report (Hz)
#define PD_CLARITY 0.80f  // normalized-autocorr peak >= this => "voiced"

typedef struct {
    PitchVoice *v;          // shared pitch-core shifter (does the transposition)

    // pitch-detector ring buffer (one full window of recent dry input)
    float ring[PD_WIN];
    int   rw;               // ring write index
    int   filled;           // samples seen so far (until >= PD_WIN)
    int   hop;              // samples until next pitch estimate
    float work[PD_WIN];     // scratch copy for autocorrelation (snapshot)
    float clar[PD_WIN / 2]; // scratch clarity-by-lag for the peak picker

    // detector state, persisted between frames
    float f0;               // last estimated fundamental (Hz), 0 = unvoiced now
    int   midi;             // last voiced MIDI note (rounded), -1 = none yet
    int   haveNote;         // 1 once we've ever locked a note

    // smoothing of the semitone shift -> no zipper noise on note changes
    float shift;            // current (smoothed) shift in semitones
    float shiftTarget;      // where we're gliding to
    float glideA;           // one-pole coeff for the glide
    int   lastCentsI;       // last cents pushed to the voice (avoid redundant set)

    int   rate;
} DiatState;

// ---------------------------------------------------------------------------
// Monophonic pitch detector.
// Method: short-time autocorrelation with McLeod-style normalization. For each
// lag tau we form r(tau)=sum x[i]x[i+tau] and m(tau)=sum(x[i]^2+x[i+tau]^2);
// the normalized clarity n(tau)=2 r(tau)/m(tau) lies in [-1,1] and peaks at the
// period. We take the FIRST clear maximum (after the autocorr has dipped below
// zero past the tau=0 lobe) to avoid octave errors, refine it with parabolic
// interpolation, and gate on energy + clarity for voiced/unvoiced. This is the
// well-trodden ACF/MPM approach; everything here is computed locally.
// Returns f0 in Hz, or 0.0 if the frame is unvoiced / too weak / unreliable.
// ---------------------------------------------------------------------------
static float pd_estimate(const float *x, int rate, float *clar /*PD_WIN/2*/) {
    int   tauMin = (int)((float)rate / PD_FMAX);
    int   tauMax = (int)((float)rate / PD_FMIN);
    if (tauMin < 2) tauMin = 2;
    if (tauMax > PD_WIN / 2 - 1) tauMax = PD_WIN / 2 - 1;
    if (tauMax <= tauMin + 2) return 0.0f;

    // Energy gate: ignore silence / near-silence (RMS in normalized float).
    double energy = 0.0;
    for (int i = 0; i < PD_WIN; i++) energy += (double)x[i] * (double)x[i];
    float rms = (float)sqrt(energy / PD_WIN);
    if (rms < 1.0e-4f) return 0.0f;        // unvoiced: below noise floor

    // Normalized clarity n(tau) = 2*sum(x[i]x[i+tau]) / sum(x[i]^2 + x[i+tau]^2),
    // in [-1,1], peaking at the period. (McLeod-Wyvill NSDF form.) Compute it for
    // every lag in range. (~PD_WIN*tauMax MACs/frame, a few frames/sec - trivial
    // on the A53; render is offline anyway.)
    float globalMax = 0.0f;
    for (int tau = tauMin; tau <= tauMax; tau++) {
        double r = 0.0, m = 0.0;
        int lim = PD_WIN - tau;
        for (int i = 0; i < lim; i++) {
            double a = (double)x[i], b = (double)x[i + tau];
            r += a * b;
            m += a * a + b * b;
        }
        float n = (m > 1.0e-12) ? (float)(2.0 * r / m) : 0.0f;
        clar[tau] = n;
        if (n > globalMax) globalMax = n;
    }
    if (globalMax < PD_CLARITY) return 0.0f;   // no periodicity -> unvoiced

    // Pick the fundamental, not a harmonic: among local maxima that clear a
    // fraction of the global peak, take the one at the SMALLEST lag (highest
    // pitch). This is the standard MPM cure for sub-octave errors - a true f0
    // peak and its octave-down both score high, so we trust the shorter period.
    float thresh = 0.85f * globalMax;
    int bestTau = -1;
    for (int tau = tauMin + 1; tau < tauMax; tau++) {
        if (clar[tau] > thresh &&
            clar[tau] >= clar[tau - 1] && clar[tau] >= clar[tau + 1]) {
            bestTau = tau;
            break;                          // first (shortest-period) qualifier
        }
    }
    if (bestTau < 0) return 0.0f;

    // Parabolic interpolation around the clarity peak for sub-sample period:
    // tau* = tau + 0.5 (nm1 - np1) / (nm1 - 2 nbest + np1).
    float nm1 = clar[bestTau - 1], n0 = clar[bestTau], np1 = clar[bestTau + 1];
    float denom = nm1 - 2.0f * n0 + np1;
    float delta = 0.0f;
    if (fabsf(denom) > 1.0e-9f) {
        delta = 0.5f * (nm1 - np1) / denom;
        if (delta > 1.0f)  delta = 1.0f;     // guard pathological neighbours
        if (delta < -1.0f) delta = -1.0f;
    }
    float period = (float)bestTau + delta;
    if (period < 1.0f) return 0.0f;

    float f0 = (float)rate / period;
    if (!(f0 > PD_FMIN * 0.5f) || !(f0 < PD_FMAX * 1.5f)) return 0.0f;
    return f0;
}

// f0(Hz) -> floating MIDI note number. A4 = 69 = 440 Hz.
static float hz_to_midi(float hz) {
    return 69.0f + 12.0f * log2f(hz / 440.0f);
}

// Snap an absolute MIDI note to the nearest in-scale note, return its (pitch
// class) scale-degree index 0..6 and the octave, so we can climb scale steps.
// `tonic` is the KEY's pitch class (0=C..11=B), `scale` indexes SCALE_TBL.
// Out: *degOut = degree index of the snapped note, *octOut = its octave anchor.
static int snap_to_scale(int midi, int tonic, int scale, int *degOut, int *octOut) {
    // Find which scale degree (and octave) is closest in semitones to `midi`.
    int bestMidi = midi, bestDeg = 0, bestOct = 0, bestDist = 1 << 30;
    // search a couple of octaves around the note to be safe
    int baseOct = (midi - tonic) / 12 - 1;
    for (int oct = baseOct; oct <= baseOct + 2; oct++) {
        for (int d = 0; d < 7; d++) {
            int cand = tonic + 12 * oct + SCALE_TBL[scale][d];
            int dist = cand - midi; if (dist < 0) dist = -dist;
            if (dist < bestDist) {
                bestDist = dist; bestMidi = cand; bestDeg = d; bestOct = oct;
            }
        }
    }
    *degOut = bestDeg;
    *octOut = bestOct;
    return bestMidi;   // the in-key note nearest the detected pitch
}

// Given a snapped degree/octave, climb `steps` scale-degrees and return the
// absolute MIDI note of the harmony. Wrapping past degree 6 adds an octave.
static int climb_degrees(int tonic, int scale, int deg, int oct, int steps) {
    int total = deg + steps;
    int addOct = total / 7;
    int newDeg = total % 7;
    return tonic + 12 * (oct + addOct) + SCALE_TBL[scale][newDeg];
}

static void *diat_create(int rate) {
    DiatState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rate = rate > 0 ? rate : 48000;
    s->v = pv_create(s->rate);
    if (!s->v) { free(s); return NULL; }
    s->midi = -1;
    s->haveNote = 0;
    s->shift = 0.0f;
    s->shiftTarget = 0.0f;
    s->lastCentsI = -1000000;   // impossible cents value -> forces first push
    // Glide ~30 ms: fast enough to track a phrase, slow enough to kill zippers.
    float glideMs = 30.0f;
    float tc = (glideMs * 0.001f) * (float)s->rate;
    s->glideA = (tc > 1.0f) ? (1.0f - expf(-1.0f / tc)) : 1.0f;
    s->hop = PD_HOP;
    return s;
}

// Recompute the target semitone shift from the latest detected pitch + params.
// Holds the previous shift when the frame is unvoiced (so sustained notes and
// gaps between words don't snap the harmony to zero). Falls back to a fixed
// chromatic interval only if we have NEVER locked a note yet.
static void diat_update_target(DiatState *s, float f0,
                               int tonic, int scale, int ivalIdx) {
    int steps = INTERVAL_STEPS[ivalIdx];
    if (f0 > 0.0f) {
        float fmidi = hz_to_midi(f0);
        int   midi  = (int)lrintf(fmidi);
        int deg, oct;
        int snapped = snap_to_scale(midi, tonic, scale, &deg, &oct);
        int target  = climb_degrees(tonic, scale, deg, oct, steps);
        // The diatonic interval is the DISTANCE from the input's nearest scale
        // note to the harmony note, in semitones - variable per note (a "3rd" is
        // 3 or 4 semitones depending on the degree). We shift by that integer
        // amount, so the harmony rides the singer's own vibrato/detune a perfect
        // diatonic third (etc.) above - the classic harmonizer behaviour. (Snap
        // the OUTPUT to even temperament instead, by using target-fmidi here, to
        // emulate the H3000's optional Quantize; we keep expression by default.)
        s->shiftTarget = (float)(target - snapped);
        s->midi = midi;
        s->haveNote = 1;
    } else if (!s->haveNote) {
        // Never locked a note: fall back to a fixed chromatic interval so the
        // effect still does something sensible on un-pitched/transient input.
        // Use the diatonic interval measured from the tonic as a stand-in.
        int target = climb_degrees(tonic, scale, 0, 0, steps);
        s->shiftTarget = (float)(target - tonic);
    }
    // voiced-but-unsure or unvoiced with a prior note: hold s->shiftTarget.
}

static void diat_block(void *st, const float *dry, int n, const float *p, float *outLR) {
    DiatState *s = (DiatState *)st;

    int   key   = (int)(p[P_KEY]      + 0.5f);
    int   scale = (int)(p[P_SCALE]    + 0.5f);
    int   ival  = (int)(p[P_INTERVAL] + 0.5f);
    float mix   = p[P_MIX];
    int   sp    = (int)(p[P_SPLICE]   + 0.5f);
    if (key   < 0) key   = 0; if (key   > 11) key   = 11;
    if (scale < 0) scale = 0; if (scale > 3)  scale = 3;
    if (ival  < 0) ival  = 0; if (ival  > 4)  ival  = 4;
    if (mix   < 0) mix   = 0; if (mix   > 1)  mix   = 1;
    if (sp    < 0) sp    = 0; if (sp >= SPLICE_COUNT) sp = SPLICE_COUNT - 1;
    pv_set_splice(s->v, sp);

    for (int i = 0; i < n; i++) {
        float in = dry[i];
        if (!(in == in)) in = 0.0f;          // scrub NaN from the input

        // feed the pitch-tracker ring buffer
        s->ring[s->rw] = in;
        if (++s->rw >= PD_WIN) s->rw = 0;
        if (s->filled < PD_WIN) s->filled++;

        // periodically re-estimate the fundamental and the target shift
        if (--s->hop <= 0) {
            s->hop = PD_HOP;
            if (s->filled >= PD_WIN) {
                // snapshot ring -> work[] in chronological order
                int idx = s->rw;             // oldest sample (write head)
                for (int k = 0; k < PD_WIN; k++) {
                    s->work[k] = s->ring[idx];
                    idx++; if (idx >= PD_WIN) idx = 0;
                }
                float f0 = pd_estimate(s->work, s->rate, s->clar);
                s->f0 = f0;
                diat_update_target(s, f0, key, scale, ival);
            }
        }

        // glide the shift toward its target (per-sample, click-free)
        s->shift += s->glideA * (s->shiftTarget - s->shift);
        // clamp to the pitch core's safe range (+-2 octaves)
        if (s->shift >  24.0f) s->shift =  24.0f;
        if (s->shift < -24.0f) s->shift = -24.0f;

        // push cents to the voice only when it actually changed (cheap + clean)
        int centsI = (int)lrintf(s->shift * 100.0f);
        if (centsI != s->lastCentsI) {
            pv_set_cents(s->v, (float)centsI);
            s->lastCentsI = centsI;
        }

        float wet = pv_process(s->v, in);
        if (!(wet == wet)) wet = 0.0f;       // denormal/NaN guard on output

        float out = mix * wet + (1.0f - mix) * in;
        outLR[i * 2 + 0] = out;
        outLR[i * 2 + 1] = out;              // mono harmony, centred
    }
}

static void diat_destroy(void *st) {
    DiatState *s = (DiatState *)st;
    if (!s) return;
    pv_destroy(s->v);
    free(s);
}

const H3kAlgoDef diatonic_def = {
    .name = "DIATONIC",
    .nparams = 5,
    .params = {
        { "KEY",      0, 11, 1, 0,    PK_CHOICE,  KEY_CHOICES      },
        { "SCALE",    0,  3, 1, 0,    PK_CHOICE,  SCALE_CHOICES    },
        { "INTERVAL", 0,  4, 1, 0,    PK_CHOICE,  INTERVAL_CHOICES },
        { "MIX",      0,  1, 0.05f, 0.5f, PK_PERCENT, 0            },
        { "SPLICE",   0,  3, 1, 1,    PK_CHOICE,  PITCH_SPLICE_CHOICES },
    },
    .create = diat_create,
    .block = diat_block,
    .destroy = diat_destroy,
};
