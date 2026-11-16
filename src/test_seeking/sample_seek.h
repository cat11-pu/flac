/* sample_seek - Narrow sample-exact seek interface on top of libFLAC
 *
 * This interface exposes exactly one positioning primitive: seek to an
 * absolute sample index, then read.  The first frame returned by a read
 * after a successful seek is guaranteed to be the requested global
 * sample, bit-identical to the sample a full from-scratch decode would
 * produce at the same index.  No decoder state may leak across seeks.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef FLAC__TEST_SEEKING__SAMPLE_SEEK_H
#define FLAC__TEST_SEEKING__SAMPLE_SEEK_H

#include "FLAC/ordinals.h"
#include "FLAC/stream_decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FLACSampleSeeker FLACSampleSeeker;

/* Open a FLAC file.  Returns NULL on error. */
FLACSampleSeeker *fss_open(const char *path);

/* Stream properties, valid after a successful fss_open(). */
FLAC__uint64 fss_total_samples(const FLACSampleSeeker *s);
uint32_t     fss_channels(const FLACSampleSeeker *s);
uint32_t     fss_bits_per_sample(const FLACSampleSeeker *s);
uint32_t     fss_sample_rate(const FLACSampleSeeker *s);

/* Absolute index of the sample the next fss_read() would return first. */
FLAC__uint64 fss_tell(const FLACSampleSeeker *s);

/* Position the stream so the next sample returned by fss_read() is
 * exactly the global sample `sample`.
 *
 * Returns false (and performs no positioning) if `sample` is out of
 * range, i.e. sample >= fss_total_samples().  After a failed seek the
 * stream is left at a well-defined state: the previous position is
 * discarded and the next operation must be a successful fss_seek()
 * before reading again.
 */
FLAC__bool fss_seek(FLACSampleSeeker *s, FLAC__uint64 sample);

/* Decode up to `nframes` frames (one frame = one sample per channel)
 * into `out` as interleaved 32-bit samples (sign-extended from the
 * stream bit depth).  Returns the number of frames actually decoded;
 * 0 means end of stream.
 */
FLAC__uint64 fss_read(FLACSampleSeeker *s, FLAC__int32 *out, FLAC__uint64 nframes);

void fss_close(FLACSampleSeeker *s);

#ifdef __cplusplus
}
#endif

#endif
