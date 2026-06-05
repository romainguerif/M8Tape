// fx_declick.c - Declicker: impulsive-noise (click/tick/crackle) removal by
// AR-residual detection + least-squares autoregressive (LSAR) interpolation.
// A STUDIO restoration tool, not an H3000 voice: mono in -> repaired mono,
// copied to both output channels. Designed to KILL clicks without dulling clean
// material — the detector whitens the signal first, so steep but legitimate
// transients (drum hits, plucks) are not mistaken for defects.
//
// THEORY (Godsill & Rayner, "Digital Audio Restoration", 1998; Vaseghi;
// reproduced in IPOL 2018/23 "Interpolation of Missing Samples..."). The
// reference algorithm is:
//   1. Model a short stationary frame as an order-p autoregressive (AR) process,
//      x[n] = sum_{k=1..p} a[k] x[n-k] + e[n], and estimate a[] by least squares
//      (here via autocorrelation + Levinson-Durbin -> O(p^2), no matrices).
//   2. The prediction error / residual  e[n] = x[n] - sum a[k] x[n-k]  is the
//      WHITENED signal. A click is an outlier in e[] even when x[] itself is a
//      smooth-looking glide, because the AR model has already "explained" the
//      predictable part. This is why an AR-residual detector beats a raw
//      derivative: clean transients are predictable and stay small in e[].
//   3. Robust threshold: sigma_e = 1.4826 * median(|e|)  (MAD, the median
//      absolute deviation; the 1.4826 makes it a consistent estimate of the
//      Gaussian std, and the median ignores the clicks themselves — the very
//      outliers we are hunting). Flag |e[n]| > k * sigma_e.
//   4. Flagged samples within < p of each other are merged into one gap (each
//      side of a gap needs >= p clean neighbours for the model to bridge it).
//   5. Repair the gap by LEAST-SQUARES AR INTERPOLATION (LSAR): choose the
//      unknown samples that minimise the total squared AR residual over the
//      gap given the known left/right context. With the AR matrix A built from
//      a[], this is the normal-equation solve  x_gap = -(Ai^T Ai)^-1 Ai^T Ak xk
//      (Godsill-Rayner eq. for the unknown subset). It is the optimal
//      interpolant under the AR model and reconstructs the local spectrum
//      rather than just drawing a line.
//
// WHAT IS FAITHFUL: the AR(p) least-squares model, the whitened-residual MAD
// detector with the 1.4826 factor and the k*sigma rule, the < p burst merging,
// and the LSAR normal-equation gap solve are exactly the Godsill-Rayner method.
// WHAT IS MODELED / ADAPTED for a real-time-capable embedded effect: it runs as
// a single causal sliding window (a persistent ring) instead of 75%-overlap OLA
// frames, so block size is irrelevant and there is a fixed look-ahead latency;
// the AR coefficients are refreshed on a sliding analysis window rather than per
// OLA frame; a guarded cubic-Hermite interpolant is the fallback for very short
// gaps or when the small LSAR system is ill-conditioned; and a fast normalised
// second-difference test gates the (heavier) residual test so silence/clean
// passages cost almost nothing. SENSITIVITY maps to k (higher sensitivity ->
// lower k -> catch smaller clicks), STRENGTH crossfades repair vs. original, and
// MAX SIZE caps the longest defect repaired (longer runs are passed through
// untouched rather than smeared).
#include "h3000.h"
#include <stdlib.h>
#include <math.h>

// Parameter indices -- MUST match the params[] order in the def below.
enum { P_SENS, P_MAXMS, P_STRENGTH, P_LOOKAHEAD };

#define DC_AR_ORDER     16     // AR model order p (Godsill-Rayner use ~20; 16 is
                               // ample for clicks and keeps the LSAR solve tiny)
#define DC_ANA          1024   // sliding analysis window (samples) for the AR fit
#define DC_ANA_HOP      256    // re-fit the AR model every this many samples
#define DC_MAX_GAP      256    // hard cap on repairable gap length (samples)
#define DC_LSAR_MAX     64     // gaps up to this use the (O(L^3)) LSAR solve;
                               // longer gaps fall back to cubic Hermite. The AR
                               // advantage is greatest on short defects, and
                               // this keeps the matrix small (64x64) and the
                               // per-repair cost bounded on the embedded target.
#define DC_GUARD        (DC_AR_ORDER + 2)  // clean samples required each side
#define DC_DENORM       1.0e-20f           // flush threshold (denormals/NaN)
#define DC_EPS          1.0e-12f

// Ring big enough to hold: look-ahead + the longest gap + the guard context on
// both sides + the analysis window, with headroom. Power-of-two for cheap wrap.
#define DC_RING_BITS    13                 // 8192 samples (~170 ms @ 48k)
#define DC_RING         (1 << DC_RING_BITS)
#define DC_RING_MASK    (DC_RING - 1)

typedef struct {
    int   rate;

    // Persistent input ring (the as-received samples) and a parallel "output"
    // ring holding the possibly-repaired values. We emit from a point D samples
    // behind the write head so a defect always has right-hand context.
    float in[DC_RING];      // raw input history
    float out[DC_RING];     // repaired history (== in[] where nothing was fixed)
    long  wr;               // absolute write index (monotone); ring pos = wr&MASK
    long  emitted;          // absolute index of the next sample to output
    long  resolved;         // absolute index up to which repair has been decided

    // AR model state.
    float a[DC_AR_ORDER];   // prediction coefficients a[1..p] (a[k] -> a[k-1])
    int   haveAR;           // model is valid yet?
    long  lastFit;          // absolute index of the last AR re-fit
    float sigma;            // robust residual scale (1.4826 * MAD) from last fit

    long  clicks;           // cumulative count of repaired bursts (for the live meter)
} DcState;

// --- denormal / NaN scrub ----------------------------------------------------
static inline float dc_clean(float x) {
    if (!isfinite(x)) return 0.0f;
    if (x > -DC_DENORM && x < DC_DENORM) return 0.0f;
    return x;
}

static void *dc_create(int rate) {
    DcState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rate = rate > 0 ? rate : 48000;
    return s;   // rings are zeroed by calloc -> implicit silent pre-roll
}

static void dc_destroy(void *st) {
    free((DcState *)st);
}

// ---------------------------------------------------------------------------
// AR model fit over a window of `out[]` (already-repaired history, so earlier
// fixes feed forward). Autocorrelation method + Levinson-Durbin recursion.
// Fills s->a[] and s->sigma (robust residual scale). end = absolute index one
// past the last sample of the analysis window.
// ---------------------------------------------------------------------------
static void dc_fit_ar(DcState *s, long end) {
    int N = DC_ANA;
    if (end < N) N = (int)end;            // not enough history yet
    if (N < DC_AR_ORDER * 3) { s->haveAR = 0; return; }
    long start = end - N;

    // Windowed autocorrelation r[0..p]. A light Hann taper reduces edge bias of
    // the autocorrelation estimate without needing the full frame energy.
    double r[DC_AR_ORDER + 1];
    for (int k = 0; k <= DC_AR_ORDER; k++) r[k] = 0.0;

    // Compute mean to remove DC (the AR model assumes a zero-mean process).
    double mean = 0.0;
    for (int i = 0; i < N; i++) mean += s->out[(start + i) & DC_RING_MASK];
    mean /= (double)N;

    for (int lag = 0; lag <= DC_AR_ORDER; lag++) {
        double acc = 0.0;
        for (int i = lag; i < N; i++) {
            double a0 = s->out[(start + i)       & DC_RING_MASK] - mean;
            double a1 = s->out[(start + i - lag) & DC_RING_MASK] - mean;
            acc += a0 * a1;
        }
        r[lag] = acc;
    }

    if (r[0] < DC_EPS) { s->haveAR = 0; return; }   // (near-)silent window
    // White-noise floor / ridge: stabilises Levinson on highly tonal frames.
    r[0] *= 1.0 + 1.0e-4;

    // Levinson-Durbin: solve the Yule-Walker normal equations in O(p^2).
    double a[DC_AR_ORDER + 1];   // a[0]=1 convention internally
    double err = r[0];
    a[0] = 1.0;
    for (int k = 1; k <= DC_AR_ORDER; k++) a[k] = 0.0;

    for (int m = 1; m <= DC_AR_ORDER; m++) {
        double acc = r[m];
        for (int j = 1; j < m; j++) acc += a[j] * r[m - j];
        double kref = (err > DC_EPS) ? -acc / err : 0.0;
        if (kref > 0.9999) kref = 0.9999;     // keep reflection coeff in (-1,1)
        if (kref < -0.9999) kref = -0.9999;   // -> guaranteed-stable predictor
        // Update coefficients symmetrically.
        double tmp[DC_AR_ORDER + 1];
        for (int j = 0; j <= m; j++) tmp[j] = a[j];
        for (int j = 1; j < m; j++) a[j] = tmp[j] + kref * tmp[m - j];
        a[m] = kref;
        err *= (1.0 - kref * kref);
        if (err < DC_EPS) err = DC_EPS;
    }

    // Our prediction form is x[n] ~= sum_{k=1..p} pa[k] x[n-k], where the
    // Levinson polynomial is A(z)=1+a[1]z^-1+...; so pa[k] = -a[k].
    for (int k = 1; k <= DC_AR_ORDER; k++) {
        float v = (float)(-a[k]);
        s->a[k - 1] = isfinite(v) ? v : 0.0f;
    }
    s->haveAR = 1;

    // Robust residual scale over the analysis window: sigma = 1.4826*median(|e|).
    // (Computed only at fit time; reused for detection until the next fit.)
    static float mag[DC_ANA];   // function-local scratch (single-threaded engine)
    int cnt = 0;
    for (int i = DC_AR_ORDER; i < N; i++) {
        long idx = start + i;
        double pred = 0.0;
        for (int k = 0; k < DC_AR_ORDER; k++)
            pred += (double)s->a[k] * (s->out[(idx - 1 - k) & DC_RING_MASK] - mean);
        double xi = s->out[idx & DC_RING_MASK] - mean;
        float e = (float)fabs(xi - pred);
        mag[cnt++] = isfinite(e) ? e : 0.0f;
    }
    // Median of |e|: insertion sort the magnitudes (cnt ~1k, only at re-fit
    // time so the cost is amortised), then take the middle element.
    for (int i = 1; i < cnt; i++) {
        float v = mag[i];
        int j = i - 1;
        while (j >= 0 && mag[j] > v) { mag[j + 1] = mag[j]; j--; }
        mag[j + 1] = v;
    }
    float med = (cnt > 0) ? mag[cnt / 2] : 0.0f;
    float sg = 1.4826f * med;
    if (!(sg > 0.0f) || !isfinite(sg)) sg = 1.0e-6f;   // floor for silent input
    s->sigma = sg;
    s->lastFit = end;
}

// ---------------------------------------------------------------------------
// LSAR gap repair. Solve for the L unknown samples g[0..L-1] starting at
// absolute index gapStart that minimise the squared AR residual over the gap
// using DC_GUARD known samples on each side from out[]. We form the normal
// equations  (Ai^T Ai) g = -Ai^T Ak xk  for the unknowns only and solve the
// small LxL symmetric positive-definite system by Cholesky. Returns 1 on a
// clean solve, 0 if the gap is too big / system ill-conditioned (-> caller
// falls back to cubic-Hermite). Writes the solution into outGap[0..L-1].
// ---------------------------------------------------------------------------
static int dc_lsar(DcState *s, long gapStart, int L, float *outGap) {
    if (L < 1 || L > DC_LSAR_MAX) return 0;   // long gaps -> caller uses cubic
    const int p = DC_AR_ORDER;

    // Predictor polynomial coefficients h[0..p] with h[0]=1, h[k]=-a[k].
    // Residual at output index n (n>=p) is sum_{k=0..p} h[k] * x[n-k].
    // Rows of the residual run for n in [gapStart, gapStart+L-1+p): each row
    // touches samples n-p..n. We accumulate the normal matrix over exactly the
    // rows that involve at least one unknown.
    double h[DC_AR_ORDER + 1];
    h[0] = 1.0;
    for (int k = 1; k <= p; k++) h[k] = -(double)s->a[k - 1];

    // M = Ai^T Ai  (LxL),  rhs = -Ai^T Ak xk  (L).  Index the unknowns 0..L-1 at
    // absolute positions gapStart..gapStart+L-1.
    static double M[DC_LSAR_MAX * DC_LSAR_MAX];
    static double rhs[DC_LSAR_MAX];
    for (int i = 0; i < L * L; i++) M[i] = 0.0;
    for (int i = 0; i < L; i++) rhs[i] = 0.0;

    long rowFirst = gapStart;             // first residual row touching the gap
    long rowLast  = gapStart + L - 1 + p; // exclusive upper handled by < below
    for (long n = rowFirst; n <= rowLast; n++) {
        // Build this row over the unknown set; tally known contribution k_n.
        // sample at n-k has unknown-index (n-k - gapStart) if in [0,L).
        double known = 0.0;
        // local list of (unknownIndex, coeff) — at most p+1 entries.
        int    ui[DC_AR_ORDER + 1];
        double uc[DC_AR_ORDER + 1];
        int    un = 0;
        for (int k = 0; k <= p; k++) {
            long pos = n - k;
            if (pos < 0) { known += 0.0; continue; }
            long rel = pos - gapStart;
            double coeff = h[k];
            if (rel >= 0 && rel < L) {        // unknown sample
                ui[un] = (int)rel;
                uc[un] = coeff;
                un++;
            } else {                          // known sample from repaired ring
                known += coeff * (double)s->out[pos & DC_RING_MASK];
            }
        }
        if (un == 0) continue;                // row has no unknowns -> skip
        // Accumulate into normal equations.
        for (int a1 = 0; a1 < un; a1++) {
            rhs[ui[a1]] -= uc[a1] * known;     // -Ai^T Ak xk
            for (int b = 0; b < un; b++)
                M[ui[a1] * L + ui[b]] += uc[a1] * uc[b];
        }
    }

    // Tiny Tikhonov ridge for numerical safety (keeps M positive-definite).
    double diagAvg = 0.0;
    for (int i = 0; i < L; i++) diagAvg += M[i * L + i];
    diagAvg = (L > 0) ? diagAvg / L : 1.0;
    double ridge = (diagAvg > 0.0 ? diagAvg : 1.0) * 1.0e-7;
    for (int i = 0; i < L; i++) M[i * L + i] += ridge;

    // Cholesky factorisation M = Rm * Rm^T with Rm lower-triangular, then solve
    // M g = rhs as two triangular substitutions. M is SPD by construction
    // (Ai^T Ai + ridge), so this is stable; a non-PD pivot bails to the fallback.
    static double Rm[DC_LSAR_MAX * DC_LSAR_MAX];
    for (int i = 0; i < L * L; i++) Rm[i] = 0.0;
    for (int i = 0; i < L; i++) {
        for (int j = 0; j <= i; j++) {
            double sum = M[i * L + j];
            for (int kk = 0; kk < j; kk++) sum -= Rm[i * L + kk] * Rm[j * L + kk];
            if (i == j) {
                if (sum <= DC_EPS) return 0;          // not PD -> bail to cubic
                Rm[i * L + j] = sqrt(sum);
            } else {
                Rm[i * L + j] = sum / Rm[j * L + j];
            }
        }
    }
    // Solve Rm y = rhs (forward substitution), then Rm^T g = y (back).
    static double y[DC_LSAR_MAX];
    for (int i = 0; i < L; i++) {
        double sum = rhs[i];
        for (int k = 0; k < i; k++) sum -= Rm[i * L + k] * y[k];
        y[i] = sum / Rm[i * L + i];
    }
    for (int i = L - 1; i >= 0; i--) {
        double sum = y[i];
        for (int k = i + 1; k < L; k++) sum -= Rm[k * L + i] * outGap[k];
        double g = sum / Rm[i * L + i];
        if (!isfinite(g)) return 0;
        outGap[i] = (float)g;
    }
    return 1;
}

// Cubic-Hermite fallback across a gap of L samples between the two known
// endpoints just outside it, using one further neighbour on each side to set
// the endpoint tangents (Catmull-Rom-style). Robust and always finite. Fills
// outGap[0..L-1] for the unknown positions at gapStart..gapStart+L-1.
static void dc_cubic(DcState *s, long gapStart, int L, float *outGap) {
    // p0 = last known left, m0 = its slope; p1 = first known right, m1 its slope.
    long li = gapStart - 1;          // last good left sample
    long ri = gapStart + L;          // first good right sample
    float p0 = s->out[li & DC_RING_MASK];
    float p1 = s->out[ri & DC_RING_MASK];
    float pl = s->out[(li - 1) & DC_RING_MASK];        // one further left
    float pr = s->out[(ri + 1) & DC_RING_MASK];        // one further right
    // Endpoint tangents from one-sided differences at the gap edges. The gap is
    // parameterised t in [0,1] across (L+1) sample steps, so express the
    // per-sample slope in per-unit-t by scaling by (L+1).
    float span = (float)(L + 1);
    float m0 = (p0 - pl) * span;                       // slope entering the gap
    float m1 = (pr - p1) * span;                       // slope leaving the gap
    for (int i = 0; i < L; i++) {
        float t = (float)(i + 1) / span;     // gap samples sit strictly inside
        float t2 = t * t, t3 = t2 * t;
        float h00 = 2*t3 - 3*t2 + 1;
        float h10 = t3 - 2*t2 + t;
        float h01 = -2*t3 + 3*t2;
        float h11 = t3 - t2;
        float v = h00*p0 + h10*m0 + h01*p1 + h11*m1;
        outGap[i] = isfinite(v) ? v : 0.5f * (p0 + p1);
    }
}

// ---------------------------------------------------------------------------
// Resolve (detect + repair) all samples that have now gained full right-hand
// context: i.e. every absolute index < (wr - lookahead). Operates on out[],
// in place, marching `s->resolved` forward.
// ---------------------------------------------------------------------------
static void dc_resolve(DcState *s, float k_thresh, int maxGap, float strength,
                       int lookahead) {
    long safeEnd = s->wr - lookahead;     // indices strictly below are resolvable
    if (safeEnd < 0) return;

    while (s->resolved < safeEnd) {
        long n = s->resolved;

        // Need at least p samples of history and the model present.
        if (n < DC_GUARD || !s->haveAR) { s->resolved++; continue; }

        // Refresh the AR model on a sliding window when due (uses repaired
        // history up to n so prior fixes inform the next prediction).
        if (n - s->lastFit >= DC_ANA_HOP) dc_fit_ar(s, n);
        if (!s->haveAR) { s->resolved++; continue; }

        // --- fast pre-gate: normalised second difference -------------------
        // Cheap rejection so clean/quiet passages skip the residual math.
        float x0 = s->out[n & DC_RING_MASK];
        float x1 = s->out[(n - 1) & DC_RING_MASK];
        float x2 = s->out[(n - 2) & DC_RING_MASK];
        float d2 = x0 - 2.0f * x1 + x2;
        if (fabsf(d2) < 2.0f * s->sigma) { s->resolved++; continue; }

        // --- AR residual at n ----------------------------------------------
        double pred = 0.0;
        for (int j = 0; j < DC_AR_ORDER; j++)
            pred += (double)s->a[j] * (double)s->out[(n - 1 - j) & DC_RING_MASK];
        float e = (float)((double)x0 - pred);
        if (!(fabsf(e) > k_thresh * s->sigma)) { s->resolved++; continue; }

        // --- grow the burst: extend right while residual stays hot, merging
        // any near (< GUARD) re-trigger; cap at maxGap+ (we still scan a little
        // past the cap to decide whether the run is genuinely too long). -------
        long gapStart = n;
        long gapEnd   = n;                 // inclusive last flagged sample
        long look     = n + 1;
        long hardStop = n + maxGap + DC_GUARD;
        int  clean    = 0;                 // consecutive clean samples seen
        while (look < safeEnd && look <= hardStop) {
            float lx0 = s->out[look & DC_RING_MASK];
            float lx1 = s->out[(look - 1) & DC_RING_MASK];
            float lx2 = s->out[(look - 2) & DC_RING_MASK];
            double lp = 0.0;
            for (int j = 0; j < DC_AR_ORDER; j++)
                lp += (double)s->a[j] * (double)s->out[(look - 1 - j) & DC_RING_MASK];
            float le = (float)((double)lx0 - lp);
            float ld2 = lx0 - 2.0f * lx1 + lx2;
            int hot = (fabsf(le) > k_thresh * s->sigma) ||
                      (fabsf(ld2) > 3.0f * s->sigma);
            if (hot) { gapEnd = look; clean = 0; }
            else if (++clean >= 2) break;   // two clean samples -> burst over
            look++;
        }

        int L = (int)(gapEnd - gapStart + 1);

        // Reject over-long runs: not a click — leave the audio untouched so we
        // never smear sustained content. Advance past it.
        if (L > maxGap) { s->resolved = gapEnd + 1; continue; }

        // Need GUARD clean right-context resolved-safe; if not yet available,
        // wait for more input (do NOT advance) so the repair always has both
        // sides. safeEnd guarantees lookahead; require GUARD beyond the gap.
        if (gapEnd + DC_GUARD >= safeEnd) return;

        // --- repair: LSAR, cubic fallback ----------------------------------
        float rep[DC_MAX_GAP];
        int ok = dc_lsar(s, gapStart, L, rep);
        if (!ok) dc_cubic(s, gapStart, L, rep);

        // Crossfade repair against the original by STRENGTH and write back into
        // the repaired ring so later predictions see the corrected samples.
        for (int i = 0; i < L; i++) {
            long pos = gapStart + i;
            float orig = s->in[pos & DC_RING_MASK];
            float fixed = dc_clean(rep[i]);
            float v = strength * fixed + (1.0f - strength) * orig;
            s->out[pos & DC_RING_MASK] = dc_clean(v);
        }
        s->clicks++;                       // one more defect repaired (live meter)
        s->resolved = gapEnd + 1;          // jump past the repaired burst
    }
}

// live meter: cumulative number of clicks repaired since this instance started.
static int dc_meter(void *st) {
    DcState *s = (DcState *)st;
    long c = s->clicks;
    return (c > 2000000000L) ? 2000000000 : (int)c;   // saturate (won't realistically hit)
}

static void dc_block(void *st, const float *dry, int n, const float *p, float *outLR) {
    DcState *s = (DcState *)st;

    // --- read & clamp params --------------------------------------------------
    // SENSITIVITY 0..1 -> detection multiplier k. Godsill-Rayner use k=4 as a
    // good default; we sweep k from ~6 (conservative) down to ~2.2 (aggressive)
    // so higher SENSITIVITY catches smaller clicks. 0.5 lands near the classic 4.
    float sens = p[P_SENS]; if (sens < 0) sens = 0; if (sens > 1) sens = 1;
    float k_thresh = 6.0f - 3.8f * sens;          // 6.0 .. 2.2

    // MAX SIZE in ms -> samples (longest defect we will repair).
    float maxms = p[P_MAXMS]; if (maxms < 0.05f) maxms = 0.05f;
    int maxGap = (int)(maxms * 0.001f * (float)s->rate + 0.5f);
    if (maxGap < 1) maxGap = 1;
    if (maxGap > DC_MAX_GAP) maxGap = DC_MAX_GAP;

    // STRENGTH (repair mix) 0..1.
    float strength = p[P_STRENGTH]; if (strength < 0) strength = 0; if (strength > 1) strength = 1;

    // LOOK-AHEAD in ms -> samples (latency budget for right-hand context). Must
    // exceed maxGap + guard so a defect at the look-ahead edge can still be
    // bridged; clamp into the ring with headroom.
    float lams = p[P_LOOKAHEAD]; if (lams < 0) lams = 0;
    int lookahead = (int)(lams * 0.001f * (float)s->rate + 0.5f);
    int minLook = maxGap + DC_GUARD + 4;
    if (lookahead < minLook) lookahead = minLook;
    int maxLook = DC_RING - DC_ANA - DC_GUARD - 8;   // leave room for analysis
    if (lookahead > maxLook) lookahead = maxLook;

    for (int i = 0; i < n; i++) {
        // Push one input sample into both rings (out defaults to a clean copy).
        float x = dc_clean(dry[i]);
        s->in[s->wr & DC_RING_MASK]  = x;
        s->out[s->wr & DC_RING_MASK] = x;
        s->wr++;

        // Decide repairs for everything that now has full right context.
        dc_resolve(s, k_thresh, maxGap, strength, lookahead);

        // Emit the sample that is `lookahead` behind the write head. Until the
        // pipeline has filled, emit the zeroed pre-roll (implicit silence).
        long want = s->wr - 1 - lookahead;
        float y;
        if (want < 0) {
            y = 0.0f;                       // priming latency -> silence
        } else {
            // If this index is still inside the unresolved zone (a pending long
            // wait for right-context), fall back to the raw input so we never
            // output a stale/garbage value; resolved samples read repaired.
            if (want < s->resolved) y = s->out[want & DC_RING_MASK];
            else                    y = s->in[want & DC_RING_MASK];
        }
        y = dc_clean(y);
        outLR[i * 2 + 0] = y;               // mono -> both channels
        outLR[i * 2 + 1] = y;
    }
}

const H3kAlgoDef declick_def = {
    .name = "DECLICKER",
    .category = "STUDIO",
    .nparams = 4,
    .params = {
        // label          min   max   step   def    kind        choices
        { "SENSITIVITY",   0,    1,   0.05f, 0.5f,  PK_PERCENT, 0 },
        { "MAX SIZE",      0.1f, 3,   0.1f,  1.5f,  PK_MS,      0 },  // ms of defect
        { "STRENGTH",      0,    1,   0.05f, 1.0f,  PK_PERCENT, 0 },
        { "LOOK-AHEAD",    0,    8,   0.5f,  3.0f,  PK_MS,      0 },  // latency budget
    },
    .create  = dc_create,
    .block   = dc_block,
    .destroy = dc_destroy,
    .meter   = dc_meter,
};
