/* Structure-aware mutation fuzzer for the libFLAC stream decoder.
 * Fork-isolated: each iteration mutates a seed and decodes it in a child
 * (with random seeks). Crashes/timeouts are saved to fuzz/crashes/.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include "FLAC/stream_decoder.h"

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int alloc_check_threshold = INT32_MAX, alloc_check_counter = 0, alloc_check_keep_failing = 0;
#endif

#define MAX_FILE (1<<20)
#define MAX_SEEDS 64

static uint8_t seeds[MAX_SEEDS][MAX_FILE];
static size_t seed_len[MAX_SEEDS];
static int nseeds = 0;
static uint64_t rng_state;

static uint64_t rng(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return rng_state;
}

typedef struct { const uint8_t *data; size_t len, pos; } memsrc;

static FLAC__StreamDecoderReadStatus read_cb(const FLAC__StreamDecoder *d, FLAC__byte buf[], size_t *bytes, void *client) {
    memsrc *m = client; (void)d;
    if (*bytes == 0) return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    if (m->pos >= m->len) { *bytes = 0; return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM; }
    size_t n = m->len - m->pos; if (n > *bytes) n = *bytes;
    memcpy(buf, m->data + m->pos, n); m->pos += n; *bytes = n;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}
static FLAC__StreamDecoderSeekStatus seek_cb(const FLAC__StreamDecoder *d, FLAC__uint64 off, void *client) {
    memsrc *m = client; (void)d;
    if (off > m->len) return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    m->pos = (size_t)off; return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}
static FLAC__StreamDecoderTellStatus tell_cb(const FLAC__StreamDecoder *d, FLAC__uint64 *off, void *client) {
    memsrc *m = client; (void)d; *off = m->pos; return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}
static FLAC__StreamDecoderLengthStatus len_cb(const FLAC__StreamDecoder *d, FLAC__uint64 *len, void *client) {
    memsrc *m = client; (void)d; *len = m->len; return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}
static FLAC__bool eof_cb(const FLAC__StreamDecoder *d, void *client) {
    memsrc *m = client; (void)d; return m->pos >= m->len;
}
static FLAC__StreamDecoderWriteStatus write_cb(const FLAC__StreamDecoder *d, const FLAC__Frame *f, const FLAC__int32 * const buf[], void *client) {
    (void)d; (void)f; (void)buf; (void)client;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void meta_cb(const FLAC__StreamDecoder *d, const FLAC__StreamMetadata *m, void *client) { (void)d; (void)m; (void)client; }
static void err_cb(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus s, void *client) { (void)d; (void)s; (void)client; }

static void decode_once(const uint8_t *data, size_t len, unsigned seed) {
    memsrc m = { data, len, 0 };
    FLAC__StreamDecoder *dec = FLAC__stream_decoder_new();
    if (!dec) _exit(65);
    FLAC__stream_decoder_set_md5_checking(dec, true);
    if (FLAC__stream_decoder_init_stream(dec, read_cb, seek_cb, tell_cb, len_cb, eof_cb,
                                         write_cb, meta_cb, err_cb, &m) != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        FLAC__stream_decoder_delete(dec); _exit(3);
    }
    /* random decode pattern: full stream, or single steps, possibly with seeks */
    unsigned mode = seed % 4;
    if (mode == 0) {
        FLAC__stream_decoder_process_until_end_of_stream(dec);
    } else if (mode == 1) {
        for (int i = 0; i < 64 && FLAC__stream_decoder_process_single(dec); i++) ;
    } else if (mode == 2) {
        FLAC__stream_decoder_process_until_end_of_metadata(dec);
        for (int i = 0; i < 3; i++) {
            FLAC__uint64 target = rng() % 200000;
            FLAC__stream_decoder_seek_absolute(dec, target);
            for (int j = 0; j < 8 && FLAC__stream_decoder_process_single(dec); j++) ;
        }
    } else {
        FLAC__stream_decoder_process_until_end_of_metadata(dec);
        FLAC__stream_decoder_process_until_end_of_stream(dec);
    }
    FLAC__stream_decoder_finish(dec);
    FLAC__stream_decoder_delete(dec);
    _exit(0);
}

static const uint8_t interesting[] = {0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF, 0xF8, 0x3E};

static size_t mutate(uint8_t *out, const uint8_t *in, size_t n) {
    memcpy(out, in, n);
    unsigned nmut = 1 + rng() % 6;
    for (unsigned k = 0; k < nmut; k++) {
        unsigned strat = rng() % 10;
        if (strat < 4 && n > 8) {               /* byte flips */
            unsigned flips = 1 + rng() % 8;
            for (unsigned j = 0; j < flips; j++)
                out[4 + rng() % (n - 4)] ^= 1 << (rng() % 8);
        } else if (strat < 5 && n > 8) {        /* interesting byte */
            out[4 + rng() % (n - 4)] = interesting[rng() % 8];
        } else if (strat < 6 && n > 8) {        /* truncate */
            n = 4 + rng() % (n - 4);
        } else if (strat < 7 && n > 8) {        /* metadata block header corruption */
            size_t off = 4;
            while (off + 4 <= n) {
                size_t blen = ((out[off+1] << 16) | (out[off+2] << 8) | out[off+3]);
                if (rng() % 3 == 0) {
                    if (rng() % 2) out[off] = (out[off] & 0x80) | (rng() % 128);
                    else { uint32_t v = rng() % 0x1000000; out[off+1] = v >> 16; out[off+2] = v >> 8; out[off+3] = v; }
                    break;
                }
                off += 4 + blen;
                if (out[off - blen - 4] & 0x80 || off >= n) break;
            }
        } else if (strat < 8 && n > 64) {       /* delete chunk */
            size_t pos = 4 + rng() % (n - 4);
            size_t dlen = 1 + rng() % 256; if (pos + dlen > n) dlen = n - pos;
            memmove(out + pos, out + pos + dlen, n - pos - dlen);
            n -= dlen;
        } else if (strat < 9 && n > 64 && n < MAX_FILE - 256) { /* duplicate chunk */
            size_t pos = 4 + rng() % (n - 4);
            size_t dlen = 1 + rng() % 256; if (pos + dlen > n) dlen = n - pos;
            memmove(out + pos + dlen, out + pos, n - pos);
            memcpy(out + pos, out + pos + dlen, dlen);
            n += dlen;
        } else if (n > 16) {                    /* multi-byte stomp */
            size_t pos = 4 + rng() % (n - 8);
            uint32_t v = rng();
            memcpy(out + pos, &v, 4);
        }
    }
    return n;
}

int main(int argc, char **argv) {
    uint64_t iters = argc > 1 ? strtoull(argv[1], 0, 0) : 100000;
    rng_state = argc > 2 ? strtoull(argv[2], 0, 0) : 0x9e3779b97f4a7c15ULL;
    const char *seeddir = argc > 3 ? argv[3] : "fuzz/corpus";
    mkdir("fuzz/crashes", 0755);

    /* load seeds */
    char cmd[512];
    snprintf(cmd, sizeof cmd, "ls %s/*.flac 2>/dev/null", seeddir);
    FILE *p = popen(cmd, "r");
    char line[512];
    while (nseeds < MAX_SEEDS && fgets(line, sizeof line, p)) {
        line[strcspn(line, "\n")] = 0;
        FILE *f = fopen(line, "rb"); if (!f) continue;
        seed_len[nseeds] = fread(seeds[nseeds], 1, MAX_FILE, f);
        fclose(f);
        if (seed_len[nseeds] > 8) nseeds++;
    }
    pclose(p);
    if (!nseeds) { fprintf(stderr, "no seeds\n"); return 1; }

    static uint8_t buf[MAX_FILE];
    uint64_t crashes = 0, timeouts = 0;
    for (uint64_t it = 0; it < iters; it++) {
        int s = rng() % nseeds;
        size_t n = mutate(buf, seeds[s], seed_len[s]);
        unsigned mode_seed = rng();
        pid_t pid = fork();
        if (pid == 0) {
            alarm(10);
            /* silence child stderr */
            freopen("/dev/null", "w", stderr);
            decode_once(buf, n, mode_seed);
        }
        int status;
        waitpid(pid, &status, 0);
        int bad = 0, to = 0;
        if (WIFSIGNALED(status)) {
            bad = 1;
            if (WTERMSIG(status) == SIGALRM) to = 1;
        } else if (WIFEXITED(status) && WEXITSTATUS(status) >= 90) {
            bad = 1; /* sanitizer abort */
        }
        if (bad) {
            crashes++; timeouts += to;
            char path[256];
            snprintf(path, sizeof path, "fuzz/crashes/crash_%06llu_%d.flac", (unsigned long long)it, to);
            FILE *f = fopen(path, "wb"); fwrite(buf, 1, n, f); fclose(f);
            fprintf(stderr, "CRASH it=%llu sig/asan saved=%s\n", (unsigned long long)it, path);
        }
        if ((it + 1) % 5000 == 0)
            printf("iter %llu/%llu crashes=%llu (timeouts=%llu)\n",
                   (unsigned long long)(it+1), (unsigned long long)iters,
                   (unsigned long long)crashes, (unsigned long long)timeouts);
    }
    printf("DONE iters=%llu crashes=%llu timeouts=%llu\n",
           (unsigned long long)iters, (unsigned long long)crashes, (unsigned long long)timeouts);
    return crashes ? 1 : 0;
}
