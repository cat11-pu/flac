/* sample_seek_test - Consistency harness for the sample_seek interface
 *
 * Verifies that seek+read through the narrow interface produces output
 * bit-identical to an independent from-scratch decode of the whole
 * stream, sample by sample:
 *
 *   Phase 1  independent full decode (direct libFLAC) as reference
 *   Phase 2  sequential read through the interface == reference
 *   Phase 3  boundary cases (first/last/past-end sample, EOS reuse, ...)
 *   Phase 4  N pseudo-random seeks, each followed by a random-length
 *            read, every sample compared against the reference
 *
 * Exit status is 0 only if every comparison matched exactly.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FLAC/stream_decoder.h"
#include "sample_seek.h"

typedef struct {
	FLAC__int32 *pcm;        /* interleaved */
	FLAC__uint64 frames;     /* frames decoded so far */
	FLAC__uint64 cap;        /* capacity in frames */
	uint32_t channels;
	uint32_t bits_per_sample;
	uint32_t sample_rate;
	FLAC__uint64 total_samples;
	FLAC__bool error;
} Ref;

static FLAC__StreamDecoderWriteStatus ref_write_(const FLAC__StreamDecoder *d, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *cd)
{
	Ref *r = cd;
	uint32_t i, c, bs = frame->header.blocksize, ch = frame->header.channels;
	(void)d;
	if(r->frames + bs > r->cap) {
		FLAC__uint64 ncap = r->cap ? r->cap : 65536;
		FLAC__int32 *p;
		while(ncap < r->frames + bs)
			ncap *= 2;
		p = realloc(r->pcm, (size_t)ncap * ch * sizeof(*p));
		if(!p) { r->error = true; return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT; }
		r->pcm = p;
		r->cap = ncap;
	}
	for(i = 0; i < bs; i++)
		for(c = 0; c < ch; c++)
			r->pcm[(r->frames + i) * ch + c] = buffer[c][i];
	r->frames += bs;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void ref_meta_(const FLAC__StreamDecoder *d, const FLAC__StreamMetadata *m, void *cd)
{
	Ref *r = cd;
	(void)d;
	if(m->type == FLAC__METADATA_TYPE_STREAMINFO) {
		r->total_samples = m->data.stream_info.total_samples;
		r->channels = m->data.stream_info.channels;
		r->bits_per_sample = m->data.stream_info.bits_per_sample;
		r->sample_rate = m->data.stream_info.sample_rate;
	}
}

static void ref_err_(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus st, void *cd)
{
	Ref *r = cd;
	(void)d; (void)st;
	r->error = true;
}

static FLAC__bool decode_reference_(const char *path, Ref *r)
{
	FLAC__StreamDecoder *d = FLAC__stream_decoder_new();
	FLAC__bool ok = false;
	if(!d)
		return false;
	if(FLAC__stream_decoder_init_file(d, path, ref_write_, ref_meta_, ref_err_, r) != FLAC__STREAM_DECODER_INIT_STATUS_OK)
		goto done;
	if(!FLAC__stream_decoder_process_until_end_of_stream(d))
		goto done;
	ok = !r->error && FLAC__stream_decoder_finish(d);
done:
	FLAC__stream_decoder_delete(d);
	return ok;
}

/* ---- comparison helpers ---- */

static FLAC__uint64 g_mismatches = 0;
static FLAC__uint64 g_compared = 0;
static FLAC__int64  g_max_abs_err = 0;
static FLAC__uint64 g_first_bad = ~(FLAC__uint64)0;

static void compare_(const Ref *r, FLAC__uint64 at, const FLAC__int32 *got, FLAC__uint64 nframes)
{
	FLAC__uint64 i;
	uint32_t c;
	for(i = 0; i < nframes; i++) {
		for(c = 0; c < r->channels; c++) {
			FLAC__int32 want = r->pcm[(at + i) * r->channels + c];
			FLAC__int32 g = got[i * r->channels + c];
			FLAC__int64 err = (FLAC__int64)g - want;
			if(err < 0) err = -err;
			if(err > g_max_abs_err) g_max_abs_err = err;
			if(g != want) {
				g_mismatches++;
				if(at + i < g_first_bad)
					g_first_bad = at + i;
			}
		}
	}
	g_compared += nframes;
}

static FLAC__uint64 rng_state_ = 0x9E3779B97F4A7C15ULL;
static FLAC__uint64 rng_(void)
{
	FLAC__uint64 x = rng_state_;
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	rng_state_ = x;
	return x;
}

/* seek to `pos`, read `nframes`, compare; returns false on hard failure */
static FLAC__bool check_seek_(FLACSampleSeeker *s, const Ref *r, FLAC__uint64 pos, FLAC__uint64 nframes, const char *tag)
{
	FLAC__int32 *buf;
	FLAC__uint64 got;

	if(!fss_seek(s, pos)) {
		printf("FAIL [%s]: seek(%" PRIu64 ") rejected but target is in range\n", tag, pos);
		return false;
	}
	if(fss_tell(s) != pos) {
		printf("FAIL [%s]: tell()=%" PRIu64 " after seek(%" PRIu64 ")\n", tag, fss_tell(s), pos);
		return false;
	}
	buf = malloc((size_t)nframes * r->channels * sizeof(*buf));
	if(!buf) {
		printf("FAIL [%s]: out of memory\n", tag);
		return false;
	}
	got = fss_read(s, buf, nframes);
	{
		FLAC__uint64 want = r->total_samples - pos;
		if(want > nframes) want = nframes;
		if(got != want) {
			printf("FAIL [%s]: seek(%" PRIu64 ") read %" PRIu64 " frames, expected %" PRIu64 "\n",
			       tag, pos, got, want);
			free(buf);
			return false;
		}
	}
	compare_(r, pos, buf, got);
	if(fss_tell(s) != pos + got) {
		printf("FAIL [%s]: tell()=%" PRIu64 " after reading to %" PRIu64 "\n", tag, fss_tell(s), pos + got);
		free(buf);
		return false;
	}
	free(buf);
	return true;
}

int main(int argc, char *argv[])
{
	const char *path;
	FLAC__uint64 seek_count = 1000;
	Ref r;
	FLACSampleSeeker *s;
	FLAC__uint64 i, total;
	FLAC__bool ok = true;
	FLAC__int32 *buf;

	if(argc < 2 || argc > 4) {
		fprintf(stderr, "usage: %s file.flac [seek_count=1000] [seed]\n", argv[0]);
		return 2;
	}
	path = argv[1];
	if(argc > 2) seek_count = strtoull(argv[2], 0, 0);
	if(argc > 3) rng_state_ = strtoull(argv[3], 0, 0) | 1;

	memset(&r, 0, sizeof(r));
	if(!decode_reference_(path, &r)) {
		printf("FAIL: reference decode of %s\n", path);
		return 1;
	}
	total = r.total_samples;
	if(total == 0 || r.frames != total) {
		printf("FAIL: reference frames %" PRIu64 " != STREAMINFO total %" PRIu64 "\n", r.frames, total);
		return 1;
	}

	s = fss_open(path);
	if(!s) {
		printf("FAIL: fss_open(%s)\n", path);
		return 1;
	}
	if(fss_total_samples(s) != total || fss_channels(s) != r.channels ||
	   fss_bits_per_sample(s) != r.bits_per_sample) {
		printf("FAIL: interface stream properties disagree with reference\n");
		return 1;
	}

	/* Phase 2: sequential from-scratch read through the interface */
	{
		FLAC__uint64 off = 0, n;
		const FLAC__uint64 chunk = 4096;
		buf = malloc((size_t)chunk * r.channels * sizeof(*buf));
		while((n = fss_read(s, buf, chunk)) > 0) {
			compare_(&r, off, buf, n);
			off += n;
		}
		free(buf);
		if(off != total) {
			printf("FAIL [sequential]: decoded %" PRIu64 " frames, expected %" PRIu64 "\n", off, total);
			ok = false;
		}
	}

	/* Phase 3: boundary cases */
	ok &= check_seek_(s, &r, 0, 1, "seek-first-sample");
	if(total > 1)
		ok &= check_seek_(s, &r, 1, 1, "seek-second-sample");
	ok &= check_seek_(s, &r, total - 1, 1, "seek-last-sample");
	ok &= check_seek_(s, &r, total - 1, 16, "read-past-eos-clamped");
	ok &= check_seek_(s, &r, total / 2, 1, "seek-mid-stream");

	/* out-of-range seeks must be rejected, and the stream must recover */
	if(fss_seek(s, total)) {
		printf("FAIL [seek-past-end]: seek(total_samples) accepted\n");
		ok = false;
	}
	if(fss_seek(s, total + 1000000)) {
		printf("FAIL [seek-past-end]: seek(total+1e6) accepted\n");
		ok = false;
	}
	ok &= check_seek_(s, &r, 0, 8, "recover-after-failed-seek");

	/* repeated seeks without intervening reads */
	ok &= check_seek_(s, &r, total / 3, 4, "seek-a");
	ok &= check_seek_(s, &r, total / 3, 4, "seek-same-position");
	if(!fss_seek(s, total - 1) || !fss_seek(s, 0) || !fss_seek(s, total / 2)) {
		printf("FAIL [seek-chain]: chained seeks without reads\n");
		ok = false;
	}
	ok &= check_seek_(s, &r, total / 2, 4, "read-after-seek-chain");

	/* read to EOS, then reuse the stream from 0 */
	{
		FLAC__int32 tmp[512];
		while(fss_read(s, tmp, sizeof(tmp) / sizeof(tmp[0]) / r.channels) > 0)
			;
		if(fss_read(s, tmp, 1) != 0) {
			printf("FAIL [eos]: read past end of stream returned data\n");
			ok = false;
		}
	}
	ok &= check_seek_(s, &r, 0, 32, "reuse-after-eos");

	/* one-sample reads across frame boundaries */
	{
		FLAC__uint64 base = total / 4, k;
		FLAC__int32 one[8];
		if(!fss_seek(s, base)) {
			printf("FAIL [single-sample]: seek(%" PRIu64 ")\n", base);
			ok = false;
		}
		for(k = 0; k < 8192 && base + k < total; k++) {
			if(fss_read(s, one, 1) != 1) {
				printf("FAIL [single-sample]: short read at %" PRIu64 "\n", base + k);
				ok = false;
				break;
			}
			compare_(&r, base + k, one, 1);
		}
	}

	/* Phase 4: random seek barrage */
	for(i = 0; i < seek_count; i++) {
		FLAC__uint64 pos = rng_() % total;
		FLAC__uint64 len = 1 + rng_() % 16384;
		if(!check_seek_(s, &r, pos, len, "random"))
			ok = false;
	}

	printf("file=%s total_samples=%" PRIu64 " channels=%u bps=%u rate=%u\n",
	       path, total, r.channels, r.bits_per_sample, r.sample_rate);
	printf("seeks=%" PRIu64 " samples_compared=%" PRIu64 " mismatches=%" PRIu64
	       " max_abs_error=%" PRId64 " first_mismatch=",
	       seek_count, g_compared, g_mismatches, g_max_abs_err);
	if(g_first_bad == ~(FLAC__uint64)0)
		printf("none\n");
	else
		printf("%" PRIu64 "\n", g_first_bad);
	printf("%s\n", ok && g_mismatches == 0 ? "PASSED" : "FAILED");

	fss_close(s);
	free(r.pcm);
	return (ok && g_mismatches == 0) ? 0 : 1;
}
