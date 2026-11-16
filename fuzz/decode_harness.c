/* Decode a FLAC file with libFLAC; report errors with offsets.
 * Exit 0: clean decode. Exit 2: decode errors reported (no crash).
 * Exit 3: decoder init/state failure. Anything else (signal/asan) = bug.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FLAC/stream_decoder.h"

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#include <stdint.h>
int alloc_check_threshold = INT32_MAX, alloc_check_counter = 0, alloc_check_keep_failing = 0;
#endif

static int errors = 0;
static FLAC__uint64 total_samples = 0;

static FLAC__StreamDecoderWriteStatus write_cb(const FLAC__StreamDecoder *d, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *client) {
    (void)d; (void)buffer; (void)client;
    total_samples += frame->header.blocksize;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void meta_cb(const FLAC__StreamDecoder *d, const FLAC__StreamMetadata *m, void *client) { (void)d;(void)m;(void)client; }
static void err_cb(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus s, void *client) {
    FLAC__uint64 pos = 0;
    (void)client;
    FLAC__stream_decoder_get_decode_position(d, &pos);
    fprintf(stderr, "ERROR[%d] %s @ offset %llu\n", (int)s, FLAC__StreamDecoderErrorStatusString[s], (unsigned long long)pos);
    errors++;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file\n", argv[0]); return 64; }
    FLAC__StreamDecoder *dec = FLAC__stream_decoder_new();
    if (!dec) return 65;
    FLAC__stream_decoder_set_md5_checking(dec, true);
    FLAC__StreamDecoderInitStatus is = FLAC__stream_decoder_init_file(dec, argv[1], write_cb, meta_cb, err_cb, NULL);
    if (is != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        fprintf(stderr, "INIT_FAIL %s\n", FLAC__StreamDecoderInitStatusString[is]);
        FLAC__stream_decoder_delete(dec);
        return 3;
    }
    FLAC__bool ok = FLAC__stream_decoder_process_until_end_of_stream(dec);
    FLAC__StreamDecoderState st = FLAC__stream_decoder_get_state(dec);
    if (!ok)
        fprintf(stderr, "STATE %s\n", FLAC__StreamDecoderStateString[st]);
    FLAC__stream_decoder_finish(dec);
    FLAC__stream_decoder_delete(dec);
    fprintf(stderr, "SAMPLES %llu\n", (unsigned long long)total_samples);
    return errors ? 2 : (ok ? 0 : 3);
}
