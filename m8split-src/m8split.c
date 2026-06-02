/*
 * m8split - decoupe un WAV PCM multicanal entrelace en fichiers stereo.
 *
 * Concu pour M8Tape : un WAV 24 canaux (S24_3LE, 44100 Hz) capture depuis la
 * Dirtywave M8 est decoupe en 12 fichiers stereo 01.wav..12.wav, une paire par
 * fichier (paire N = canaux 2N-1 et 2N). Une seule passe de lecture.
 *
 * Generique : fonctionne pour tout nombre PAIR de canaux et toute profondeur
 * (8/16/24/32 bits PCM entier). Les canaux d'une paire etant adjacents dans la
 * trame entrelacee, leur copie est un simple bloc contigu.
 *
 * Usage : m8split entree_Nch.wav dossier_sortie
 * Codes retour : 0 ok, 1 usage, 2 E/S, 3 WAV invalide / format non gere.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t rd_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd_u16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}
static void wr_u32(FILE *f, uint32_t v) {
    unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                           (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    fwrite(b, 1, 4, f);
}
static void wr_u16(FILE *f, uint16_t v) {
    unsigned char b[2] = { (unsigned char)v, (unsigned char)(v >> 8) };
    fwrite(b, 1, 2, f);
}

/* En-tete WAV PCM canonique (44 octets). data_size en octets. */
static void write_wav_header(FILE *f, uint16_t ch, uint32_t rate,
                             uint16_t bits, uint32_t data_size) {
    uint16_t block_align = (uint16_t)(ch * (bits / 8));
    uint32_t byte_rate = rate * block_align;
    fwrite("RIFF", 1, 4, f); wr_u32(f, 36 + data_size); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); wr_u32(f, 16);
    wr_u16(f, 1);            /* PCM */
    wr_u16(f, ch);
    wr_u32(f, rate);
    wr_u32(f, byte_rate);
    wr_u16(f, block_align);
    wr_u16(f, bits);
    fwrite("data", 1, 4, f); wr_u32(f, data_size);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s entree_Nch.wav dossier_sortie\n", argv[0]);
        return 1;
    }
    const char *in_path = argv[1];
    const char *out_dir = argv[2];

    FILE *in = fopen(in_path, "rb");
    if (!in) { fprintf(stderr, "m8split: ouverture %s impossible\n", in_path); return 2; }

    unsigned char hdr[12];
    if (fread(hdr, 1, 12, in) != 12 ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "m8split: pas un WAV RIFF/WAVE\n"); fclose(in); return 3;
    }

    uint16_t channels = 0, bits = 0, fmt_tag = 0;
    uint32_t rate = 0;
    uint32_t data_size = 0;     /* taille annoncee du chunk data, si fiable */
    int have_fmt = 0, have_data = 0;

    /* Parcours des chunks jusqu'a 'data'. */
    for (;;) {
        unsigned char ch[8];
        if (fread(ch, 1, 8, in) != 8) break;
        uint32_t id_is_fmt  = (memcmp(ch, "fmt ", 4) == 0);
        uint32_t id_is_data = (memcmp(ch, "data", 4) == 0);
        uint32_t sz = rd_u32(ch + 4);

        if (id_is_fmt) {
            unsigned char fb[40];
            uint32_t n = sz > sizeof(fb) ? (uint32_t)sizeof(fb) : sz;
            if (fread(fb, 1, n, in) != n) { fprintf(stderr, "m8split: fmt tronque\n"); fclose(in); return 3; }
            fmt_tag  = rd_u16(fb);
            channels = rd_u16(fb + 2);
            rate     = rd_u32(fb + 4);
            bits     = rd_u16(fb + 14);
            have_fmt = 1;
            if (sz > n) fseek(in, (long)(sz - n), SEEK_CUR);   /* solde du chunk */
            if (sz & 1) fseek(in, 1, SEEK_CUR);                /* padding pair */
        } else if (id_is_data) {
            data_size = sz;
            have_data = 1;
            break;   /* les echantillons commencent ici */
        } else {
            fseek(in, (long)(sz + (sz & 1)), SEEK_CUR);        /* chunk ignore + padding */
        }
    }

    if (!have_fmt || !have_data) { fprintf(stderr, "m8split: fmt/data manquant\n"); fclose(in); return 3; }
    if (fmt_tag != 1) { fprintf(stderr, "m8split: format non PCM (tag %u)\n", fmt_tag); fclose(in); return 3; }
    if (channels < 2 || (channels & 1)) { fprintf(stderr, "m8split: %u canaux (pair >=2 attendu)\n", channels); fclose(in); return 3; }
    if (bits == 0 || (bits & 7)) { fprintf(stderr, "m8split: %u bits non gere\n", bits); fclose(in); return 3; }

    uint32_t bps = bits / 8;                 /* octets / echantillon */
    uint32_t frame = channels * bps;         /* octets / trame entrelacee */
    uint32_t pairs = channels / 2;           /* nombre de fichiers stereo */
    uint32_t pair_bytes = 2 * bps;           /* octets d'une paire dans la trame */

    /* Ouverture des sorties + en-tete provisoire (data_size patché a la fin). */
    FILE **out = calloc(pairs, sizeof(FILE *));
    if (!out) { fclose(in); return 2; }
    for (uint32_t i = 0; i < pairs; i++) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/%02u.wav", out_dir, i + 1);
        out[i] = fopen(path, "wb");
        if (!out[i]) {
            fprintf(stderr, "m8split: creation %s impossible\n", path);
            for (uint32_t j = 0; j < i; j++) fclose(out[j]);
            free(out); fclose(in); return 2;
        }
        write_wav_header(out[i], 2, rate, bits, 0);
    }

    /* Lecture par blocs de trames, distribution paire par paire. */
    enum { BLK = 4096 };                     /* trames par bloc */
    unsigned char *buf = malloc((size_t)frame * BLK);
    if (!buf) {
        for (uint32_t i = 0; i < pairs; i++) fclose(out[i]);
        free(out); fclose(in); return 2;
    }

    /* data_size fiable ? sinon (0 ou 0xFFFFFFFF) on lit jusqu'a EOF. */
    int bounded = (data_size != 0 && data_size != 0xFFFFFFFFu);
    uint64_t remaining = bounded ? data_size : (uint64_t)-1;
    uint64_t frames_written = 0;
    int io_err = 0;

    for (;;) {
        size_t want = BLK;
        if (bounded) {
            uint64_t rem_frames = remaining / frame;
            if (rem_frames == 0) break;
            if (rem_frames < want) want = (size_t)rem_frames;
        }
        size_t got = fread(buf, frame, want, in);
        if (got == 0) break;

        for (uint32_t i = 0; i < pairs; i++) {
            unsigned char *src = buf + (size_t)(2 * i) * bps;   /* canaux 2i, 2i+1 */
            for (size_t f = 0; f < got; f++) {
                if (fwrite(src + (size_t)f * frame, 1, pair_bytes, out[i]) != pair_bytes) { io_err = 1; break; }
            }
            if (io_err) break;
        }
        if (io_err) break;
        frames_written += got;
        if (bounded) remaining -= (uint64_t)got * frame;
    }

    free(buf);

    /* Patch des en-tetes avec la taille reelle. */
    uint32_t out_data = (uint32_t)(frames_written * pair_bytes);
    for (uint32_t i = 0; i < pairs; i++) {
        if (fseek(out[i], 0, SEEK_SET) == 0)
            write_wav_header(out[i], 2, rate, bits, out_data);
        if (fclose(out[i]) != 0) io_err = 1;
    }
    free(out);
    fclose(in);

    if (io_err) { fprintf(stderr, "m8split: erreur d'ecriture\n"); return 2; }
    fprintf(stdout, "m8split: %u fichiers stereo (%llu trames) -> %s\n",
            pairs, (unsigned long long)frames_written, out_dir);
    return 0;
}
