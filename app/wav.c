// wav.c - interleaved-PCM WAV splitter (ported from m8split.c), single pass.
#include "wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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
