// wav.c - interleaved-PCM WAV splitter (ported from m8split.c), single pass.
#include "wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static uint32_t rd_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd_u16(const unsigned char *p) { return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8); }
static void wr_u32(FILE *f, uint32_t v) {
    unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
    fwrite(b, 1, 4, f);
}
static void wr_u16(FILE *f, uint16_t v) {
    unsigned char b[2] = {(unsigned char)v, (unsigned char)(v >> 8)};
    fwrite(b, 1, 2, f);
}
static void write_header(FILE *f, uint16_t ch, uint32_t rate, uint16_t bits, uint32_t data_size) {
    uint16_t ba = (uint16_t)(ch * (bits / 8));
    uint32_t br = rate * ba;
    fwrite("RIFF", 1, 4, f); wr_u32(f, 36 + data_size); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); wr_u32(f, 16);
    wr_u16(f, 1); wr_u16(f, ch); wr_u32(f, rate); wr_u32(f, br); wr_u16(f, ba); wr_u16(f, bits);
    fwrite("data", 1, 4, f); wr_u32(f, data_size);
}

int wav_split(const char *in_path, const char *out_dir, const char *prefix) {
    FILE *in = fopen(in_path, "rb");
    if (!in) return -1;

    unsigned char hdr[12];
    if (fread(hdr, 1, 12, in) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fclose(in); return -1;
    }
    uint16_t channels = 0, bits = 0, fmt_tag = 0;
    uint32_t rate = 0, data_size = 0;
    int have_fmt = 0, have_data = 0;
    for (;;) {
        unsigned char ck[8];
        if (fread(ck, 1, 8, in) != 8) break;
        uint32_t sz = rd_u32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            unsigned char fb[40];
            uint32_t n = sz > sizeof(fb) ? (uint32_t)sizeof(fb) : sz;
            if (fread(fb, 1, n, in) != n) { fclose(in); return -1; }
            fmt_tag = rd_u16(fb); channels = rd_u16(fb + 2); rate = rd_u32(fb + 4); bits = rd_u16(fb + 14);
            have_fmt = 1;
            if (sz > n) fseek(in, (long)(sz - n), SEEK_CUR);
            if (sz & 1) fseek(in, 1, SEEK_CUR);
        } else if (!memcmp(ck, "data", 4)) {
            data_size = sz; have_data = 1; break;
        } else {
            fseek(in, (long)(sz + (sz & 1)), SEEK_CUR);
        }
    }
    if (!have_fmt || !have_data || fmt_tag != 1 || channels < 2 || (channels & 1) ||
        bits == 0 || (bits & 7)) { fclose(in); return -1; }

    uint32_t bps = bits / 8, frame = channels * bps, pairs = channels / 2, pair_bytes = 2 * bps;

    FILE **out = calloc(pairs, sizeof(FILE *));
    if (!out) { fclose(in); return -1; }
    for (uint32_t i = 0; i < pairs; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s_%02u.wav", out_dir, prefix, i + 1);
        out[i] = fopen(path, "wb");
        if (!out[i]) { for (uint32_t j = 0; j < i; j++) fclose(out[j]); free(out); fclose(in); return -1; }
        write_header(out[i], 2, rate, bits, 0);
    }

    enum { BLK = 4096 };
    unsigned char *buf = malloc((size_t)frame * BLK);
    if (!buf) { for (uint32_t i = 0; i < pairs; i++) fclose(out[i]); free(out); fclose(in); return -1; }

    int bounded = (data_size != 0 && data_size != 0xFFFFFFFFu);
    uint64_t remaining = bounded ? data_size : (uint64_t)-1, frames_written = 0;
    int io_err = 0;
    for (;;) {
        size_t want = BLK;
        if (bounded) {
            uint64_t rf = remaining / frame;
            if (rf == 0) break;
            if (rf < want) want = (size_t)rf;
        }
        size_t got = fread(buf, frame, want, in);
        if (got == 0) break;
        for (uint32_t i = 0; i < pairs && !io_err; i++) {
            unsigned char *src = buf + (size_t)(2 * i) * bps;
            for (size_t f = 0; f < got; f++)
                if (fwrite(src + (size_t)f * frame, 1, pair_bytes, out[i]) != pair_bytes) { io_err = 1; break; }
        }
        if (io_err) break;
        frames_written += got;
        if (bounded) remaining -= (uint64_t)got * frame;
    }
    free(buf);

    uint32_t out_data = (uint32_t)(frames_written * pair_bytes);
    for (uint32_t i = 0; i < pairs; i++) {
        if (fseek(out[i], 0, SEEK_SET) == 0) write_header(out[i], 2, rate, bits, out_data);
        if (fclose(out[i]) != 0) io_err = 1;
    }
    free(out);
    fclose(in);
    return io_err ? -1 : (int)pairs;
}

// --- in-memory audio (editor) -----------------------------------------------
static float s8(unsigned char b)  { return ((int)b - 128) / 128.0f; }
static float s16(const unsigned char *p) { return (int16_t)(p[0] | p[1] << 8) / 32768.0f; }
static float s24(const unsigned char *p) {
    int v = p[0] | p[1] << 8 | p[2] << 16; if (v & 0x800000) v |= ~0xFFFFFF;
    return v / 8388608.0f;
}
static float s32(const unsigned char *p) { return (int32_t)rd_u32(p) / 2147483648.0f; }

static float clampf(float f) { return f < -1.0f ? -1.0f : f > 1.0f ? 1.0f : f; }

int wav_load(const char *path, Audio *a) {
    memset(a, 0, sizeof(*a));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) { fclose(f); return -1; }
    int ch = 0, bits = 0; uint32_t rate = 0, data_size = 0; int hf = 0, hd = 0;
    for (;;) {
        unsigned char ck[8];
        if (fread(ck, 1, 8, f) != 8) break;
        uint32_t sz = rd_u32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            unsigned char fb[40]; uint32_t n = sz > sizeof(fb) ? (uint32_t)sizeof(fb) : sz;
            if (fread(fb, 1, n, f) != n) { fclose(f); return -1; }
            ch = rd_u16(fb + 2); rate = rd_u32(fb + 4); bits = rd_u16(fb + 14); hf = 1;
            if (sz > n) fseek(f, (long)(sz - n), SEEK_CUR);
            if (sz & 1) fseek(f, 1, SEEK_CUR);
        } else if (!memcmp(ck, "data", 4)) { data_size = sz; hd = 1; break; }
        else fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
    }
    if (!hf || !hd || ch < 1 || bits < 8 || (bits & 7)) { fclose(f); return -1; }
    int bps = bits / 8;
    long frames = data_size / (ch * bps);
    float *data = malloc((size_t)frames * ch * sizeof(float));
    if (!data) { fclose(f); return -1; }
    unsigned char *raw = malloc((size_t)frames * ch * bps);
    if (!raw) { free(data); fclose(f); return -1; }
    if (fread(raw, 1, (size_t)frames * ch * bps, f) != (size_t)frames * ch * bps) { /* short ok */ }
    fclose(f);
    for (long i = 0; i < frames * ch; i++) {
        const unsigned char *p = raw + (size_t)i * bps;
        data[i] = bps == 2 ? s16(p) : bps == 3 ? s24(p) : bps == 4 ? s32(p) : s8(p[0]);
    }
    free(raw);
    a->ch = ch; a->rate = (int)rate; a->bits = bits; a->frames = frames; a->data = data;
    return 0;
}

static void put_sample(unsigned char *p, int bps, float f) {
    f = clampf(f);
    if (bps == 2) { int v = (int)(f * 32767); p[0] = v; p[1] = v >> 8; }
    else if (bps == 3) { int v = (int)(f * 8388607); p[0] = v; p[1] = v >> 8; p[2] = v >> 16; }
    else if (bps == 4) { long v = (long)(f * 2147483647.0); p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
    else { int v = (int)(f * 127) + 128; p[0] = (unsigned char)v; }
}

static int write_range(const char *path, const Audio *a, long s, long e) {
    if (s < 0) s = 0; if (e > a->frames) e = a->frames; if (e < s) e = s;
    int bps = a->bits / 8; long frames = e - s;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    write_header(f, (uint16_t)a->ch, (uint32_t)a->rate, (uint16_t)a->bits, (uint32_t)(frames * a->ch * bps));
    unsigned char buf[4];
    for (long i = s * a->ch; i < e * a->ch; i++) { put_sample(buf, bps, a->data[i]); fwrite(buf, 1, bps, f); }
    fclose(f);
    return 0;
}
int wav_save(const char *path, const Audio *a) { return write_range(path, a, 0, a->frames); }
int wav_save_range(const char *path, const Audio *a, long s, long e) { return write_range(path, a, s, e); }

void audio_free(Audio *a) { free(a->data); a->data = NULL; a->frames = 0; }

void au_normalize(Audio *a) {
    float peak = 0;
    for (long i = 0; i < a->frames * a->ch; i++) { float v = fabsf(a->data[i]); if (v > peak) peak = v; }
    if (peak <= 0.0001f || peak >= 0.999f) return;
    float g = 0.99f / peak;
    for (long i = 0; i < a->frames * a->ch; i++) a->data[i] *= g;
}

void au_reverse(Audio *a, long s, long e) {
    if (s < 0) s = 0; if (e > a->frames) e = a->frames;
    long i = s, j = e - 1;
    while (i < j) {
        for (int c = 0; c < a->ch; c++) {
            float t = a->data[i * a->ch + c];
            a->data[i * a->ch + c] = a->data[j * a->ch + c];
            a->data[j * a->ch + c] = t;
        }
        i++; j--;
    }
}

// equal-power (sine-law) fades — smoother, constant perceived energy
void au_fade_in(Audio *a, long s, long e) {
    if (s < 0) s = 0; if (e > a->frames) e = a->frames;
    long n = e - s; if (n < 1) return;
    for (long i = 0; i < n; i++) { float g = sinf((float)i / n * (float)M_PI / 2.0f); for (int c = 0; c < a->ch; c++) a->data[(s + i) * a->ch + c] *= g; }
}
void au_fade_out(Audio *a, long s, long e) {
    if (s < 0) s = 0; if (e > a->frames) e = a->frames;
    long n = e - s; if (n < 1) return;
    for (long i = 0; i < n; i++) { float g = cosf((float)i / n * (float)M_PI / 2.0f); for (int c = 0; c < a->ch; c++) a->data[(s + i) * a->ch + c] *= g; }
}

void au_gain_db(Audio *a, long s, long e, float db) {
    if (s < 0) s = 0; if (e > a->frames) e = a->frames;
    float g = powf(10.0f, db / 20.0f);
    for (long i = s; i < e; i++) for (int c = 0; c < a->ch; c++) a->data[i * a->ch + c] *= g;
}

// one-pole high-pass per channel over the whole file (kills DC/rumble)
void au_highpass(Audio *a, float fc) {
    if (a->rate <= 0 || a->frames < 2) return;
    float dt = 1.0f / a->rate, rc = 1.0f / (2.0f * (float)M_PI * fc), alpha = rc / (rc + dt);
    for (int c = 0; c < a->ch; c++) {
        float px = a->data[c], py = a->data[c];
        for (long i = 1; i < a->frames; i++) {
            float x = a->data[i * a->ch + c];
            float y = alpha * (py + x - px);
            a->data[i * a->ch + c] = y; px = x; py = y;
        }
    }
}

// crossfade the loop boundary so [s,e) loops seamlessly (blend tail into head)
void au_loop_xfade(Audio *a, long s, long e) {
    if (s < 0) s = 0; if (e > a->frames) e = a->frames;
    long n = e - s; if (n < 8) return;
    long L = n / 4, maxL = (long)(a->rate * 0.05); // <=50ms
    if (L > maxL) L = maxL; if (L < 1) return;
    for (long i = 0; i < L; i++) {
        float t = (i + 0.5f) / L, wo = cosf(t * (float)M_PI / 2.0f), wi = sinf(t * (float)M_PI / 2.0f);
        for (int c = 0; c < a->ch; c++) {
            float endv = a->data[(e - L + i) * a->ch + c];
            float startv = a->data[(s + i) * a->ch + c];
            a->data[(e - L + i) * a->ch + c] = endv * wo + startv * wi;
        }
    }
}

void au_dither16(Audio *a) {
    static int seeded = 0;
    if (!seeded) { srand(1234567u + (unsigned)a->frames); seeded = 1; }
    float lsb = 1.0f / 32768.0f;
    for (long i = 0; i < a->frames * a->ch; i++) {
        float rm = (float)RAND_MAX;
        float r = ((float)rand() / rm - 0.5f) + ((float)rand() / rm - 0.5f); // TPDF
        a->data[i] += r * lsb;
    }
    a->bits = 16;
}

// nearest zero crossing to pos (channel 0), within +-win frames. The caller
// bounds `win` to the visible zoom so the snapped marker stays at the cursor
// (a fixed window could snap the marker off-screen when zoomed in).
long au_snap_zero(const Audio *a, long pos, long win) {
    if (pos < 1) pos = 1; if (pos >= a->frames) pos = a->frames - 1;
    if (win < 1) win = 1;
    for (long d = 0; d < win; d++) {
        long i = pos + d;
        if (i > 0 && i < a->frames && a->data[(i - 1) * a->ch] * a->data[i * a->ch] <= 0) return i;
        long j = pos - d;
        if (j > 0 && j < a->frames && a->data[(j - 1) * a->ch] * a->data[j * a->ch] <= 0) return j;
    }
    return pos;
}

int au_crop(Audio *a, long s, long e) {
    if (s < 0) s = 0; if (e > a->frames) e = a->frames; if (e <= s) return -1;
    long n = e - s;
    memmove(a->data, a->data + s * a->ch, (size_t)n * a->ch * sizeof(float));
    a->frames = n;
    return 0;
}

int au_to_mono(Audio *a) {
    if (a->ch <= 1) return 0;
    float *nd = malloc((size_t)a->frames * sizeof(float));
    if (!nd) return -1;
    for (long i = 0; i < a->frames; i++) {
        float sum = 0; for (int c = 0; c < a->ch; c++) sum += a->data[i * a->ch + c];
        nd[i] = sum / a->ch;
    }
    free(a->data); a->data = nd; a->ch = 1;
    return 0;
}

int au_halve_rate(Audio *a) {
    long nf = a->frames / 2;
    if (nf < 1) return -1;
    float *nd = malloc((size_t)nf * a->ch * sizeof(float));
    if (!nd) return -1;
    for (long i = 0; i < nf; i++)
        for (int c = 0; c < a->ch; c++)
            nd[i * a->ch + c] = 0.5f * (a->data[(2 * i) * a->ch + c] + a->data[(2 * i + 1) * a->ch + c]);
    free(a->data); a->data = nd; a->frames = nf; a->rate /= 2;
    return 0;
}

void au_silence_bounds(const Audio *a, float thresh, long *s, long *e) {
    long first = 0, last = a->frames;
    for (long i = 0; i < a->frames; i++) {
        float m = 0; for (int c = 0; c < a->ch; c++) { float v = fabsf(a->data[i * a->ch + c]); if (v > m) m = v; }
        if (m > thresh) { first = i; break; }
    }
    for (long i = a->frames - 1; i >= 0; i--) {
        float m = 0; for (int c = 0; c < a->ch; c++) { float v = fabsf(a->data[i * a->ch + c]); if (v > m) m = v; }
        if (m > thresh) { last = i + 1; break; }
    }
    if (last <= first) { first = 0; last = a->frames; }
    *s = first; *e = last;
}

void au_peaks(const Audio *a, long s, long e, int cols, float *mn, float *mx) {
    if (s < 0) s = 0; if (e > a->frames) e = a->frames; if (e < s) e = s;
    long span = e - s;
    for (int c = 0; c < cols; c++) {
        long f0 = s + (long)((double)span * c / cols);
        long f1 = s + (long)((double)span * (c + 1) / cols);
        if (f1 <= f0) f1 = f0 + 1; if (f1 > e) f1 = e;
        float lo = 0, hi = 0;
        for (long i = f0; i < f1; i++)
            for (int ch = 0; ch < a->ch; ch++) {
                float v = a->data[i * a->ch + ch];
                if (v < lo) lo = v; if (v > hi) hi = v;
            }
        mn[c] = lo; mx[c] = hi;
    }
}

// --- streaming silence-trim (file -> file) ----------------------------------
static float dec_sample(const unsigned char *p, int bps) {
    if (bps == 2) return (int16_t)(p[0] | p[1] << 8) / 32768.0f;
    if (bps == 3) { int v = p[0] | p[1] << 8 | p[2] << 16; if (v & 0x800000) v |= ~0xFFFFFF; return v / 8388608.0f; }
    if (bps == 4) return (int32_t)rd_u32(p) / 2147483648.0f;
    return ((int)p[0] - 128) / 128.0f;
}

int wav_trim_silence_file(const char *in_path, const char *out_path, float thresh) {
    FILE *in = fopen(in_path, "rb");
    if (!in) return -1;
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, in) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) { fclose(in); return -1; }
    int ch = 0, bits = 0; uint32_t rate = 0, data_size = 0; long data_off = 0; int hf = 0, hd = 0;
    for (;;) {
        unsigned char ck[8];
        if (fread(ck, 1, 8, in) != 8) break;
        uint32_t sz = rd_u32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            unsigned char fb[40]; uint32_t n = sz > sizeof(fb) ? (uint32_t)sizeof(fb) : sz;
            if (fread(fb, 1, n, in) != n) { fclose(in); return -1; }
            ch = rd_u16(fb + 2); rate = rd_u32(fb + 4); bits = rd_u16(fb + 14); hf = 1;
            if (sz > n) fseek(in, (long)(sz - n), SEEK_CUR);
            if (sz & 1) fseek(in, 1, SEEK_CUR);
        } else if (!memcmp(ck, "data", 4)) { data_size = sz; data_off = ftell(in); hd = 1; break; }
        else fseek(in, (long)(sz + (sz & 1)), SEEK_CUR);
    }
    if (!hf || !hd || ch < 1 || bits < 8 || (bits & 7)) { fclose(in); return -1; }
    int bps = bits / 8; long frame = ch * bps;
    long frames = data_size / frame;
    if (frames < 1) { fclose(in); return -1; }

    // pass 1: scan for first/last frame above threshold
    enum { BLK = 8192 };
    unsigned char *buf = malloc((size_t)frame * BLK);
    if (!buf) { fclose(in); return -1; }
    long first = -1, last = -1, idx = 0;
    fseek(in, data_off, SEEK_SET);
    for (;;) {
        size_t got = fread(buf, frame, BLK, in);
        if (got == 0) break;
        for (size_t f = 0; f < got; f++, idx++) {
            const unsigned char *fp = buf + f * frame;
            float m = 0;
            for (int c = 0; c < ch; c++) { float v = fabsf(dec_sample(fp + c * bps, bps)); if (v > m) m = v; }
            if (m > thresh) { if (first < 0) first = idx; last = idx; }
        }
    }
    if (first < 0) { first = 0; last = frames - 1; }  // all silence: keep as-is

    // pass 2: copy [first, last] to out
    long nframes = last - first + 1;
    FILE *out = fopen(out_path, "wb");
    if (!out) { free(buf); fclose(in); return -1; }
    write_header(out, (uint16_t)ch, rate, (uint16_t)bits, (uint32_t)(nframes * frame));
    fseek(in, data_off + first * frame, SEEK_SET);
    long left = nframes;
    while (left > 0) {
        size_t want = left < BLK ? (size_t)left : BLK;
        size_t got = fread(buf, frame, want, in);
        if (got == 0) break;
        fwrite(buf, frame, got, out);
        left -= got;
    }
    fclose(out); free(buf); fclose(in);
    return 0;
}
