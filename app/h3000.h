// h3000.h - Eventide H3000-style DSP engine for M8Tape Studio.
// Faithful to the hardware's mono-program model: one pass = ONE algorithm,
// rendered destructively. This header is the GENERIC ENGINE: every algorithm is
// an H3kAlgoDef (a name + a parameter spec + create/block/destroy) registered in
// a table. The UI, the real-time preview child and the whole-sample render all
// drive any algorithm through this one interface — add an effect by adding a
// module, never by touching the plumbing.
#ifndef H3000_H
#define H3000_H

#include "wav.h"   // Audio

#define H3K_MAX_PARAMS 28   /* room for the 8-band EQ (8 x freq/gain/Q) */

// How a parameter's float value is interpreted / displayed / stepped.
typedef enum {
    PK_FLOAT,    // raw "%.2f"
    PK_INT,      // "%d"
    PK_CENTS,    // "%+d CENTS"
    PK_SEMI,     // "%+d ST"  (semitones)
    PK_MS,       // "%d MS"
    PK_PERCENT,  // value 0..1 shown "%d%%"
    PK_HZ,       // "%d HZ"
    PK_DB,       // "%+d DB"
    PK_CHOICE    // discrete: value = index into `choices`
} ParamKind;

typedef struct {
    const char *label;            // UI row label, e.g. "PITCH A (L)"
    float min, max, step, def;    // range, step per left/right press, default
    ParamKind kind;
    const char *const *choices;   // PK_CHOICE only: NULL-terminated name list
} ParamSpec;

// One algorithm. `block` reads `nparams` floats from `p` (same order as
// `params[]`), consumes `n` mono `dry` samples and writes `n` interleaved-stereo
// frames to `outLR`. State is whatever `create` returns. Params are read live so
// the preview hears edits immediately.
typedef struct {
    const char *name;
    const char *category;   // picker grouping; NULL is treated as "H3000"
    int nparams;
    ParamSpec params[H3K_MAX_PARAMS];
    void *(*create)(int rate);
    void  (*block)(void *st, const float *dry, int n, const float *p, float *outLR);
    void  (*destroy)(void *st);
    // OPTIONAL viz: fill outDb[n] with the effect's magnitude response in dB across
    // a log sweep (20 Hz .. min(20k, rate*0.45)) computed from the params ALONE
    // (stateless — runs in the UI process). Return 1 if filled, 0/NULL = no viz
    // (the FX screen then just shows the parameter rows). Used by the EQ curve.
    int   (*response)(const float *p, int rate, float *outDb, int n);
    // OPTIONAL live meter: return a scalar reading from the RUNNING state (read by
    // the preview child each block for an on-screen readout). DECLICKER returns the
    // cumulative count of clicks repaired so far. NULL hook / negative return = none.
    int   (*meter)(void *st);
} H3kAlgoDef;

// Category of algo `i` ("H3000" if the def left it NULL).
const char *h3k_category(int algo);

// --- algorithm registry (defined in h3000.c) --------------------------------
extern const H3kAlgoDef *const h3k_algos[];
extern const int h3k_algo_count;

// --- generic streaming engine (for the real-time preview child) -------------
typedef struct H3kEngine H3kEngine;
H3kEngine *h3k_create(int algo, int rate);
void       h3k_block(H3kEngine *e, const float *dry, int n, const float *params, float *outLR);
int        h3k_meter(H3kEngine *e);   // live meter from the running algo, or -1 if none
void       h3k_destroy(H3kEngine *e);

// --- whole-sample destructive render (file -> file). Result is stereo --------
// (a->ch becomes 2; mono input is upmixed by the effect). Returns 0 on success.
int h3k_render(Audio *a, int algo, const float *params);

// --- UI helpers (keep the FX screen dumb & generic) -------------------------
void  h3k_defaults(int algo, float *params);              // fill with spec defaults
float h3k_adjust(const ParamSpec *ps, float v, int dir);  // step/clamp/choice-wrap
void  h3k_format(const ParamSpec *ps, float v, char *out, int outsz);

#endif
