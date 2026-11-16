/* sample_seek - Narrow sample-exact seek interface on top of libFLAC
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include "sample_seek.h"

/* Positioning is made exact by trusting only the frame header's absolute
 * sample number delivered in each write callback: any samples preceding
 * the seek target inside the first decoded frame are discarded, so the
 * caller-visible stream starts exactly at the requested sample no matter
 * where the codec-level frame boundary fell.
 */

struct FLACSampleSeeker {
	FLAC__StreamDecoder *decoder;
	FLAC__uint64 total_samples;
	uint32_t channels;
	uint32_t bits_per_sample;
	uint32_t sample_rate;

	FLAC__uint64 position;   /* absolute index of next sample to hand out */
	FLAC__uint64 target;     /* seek target; samples before it are dropped */
	FLAC__bool   need_seek;  /* set after open/failed seek: read requires seek first */
	FLAC__bool   eof;
	FLAC__bool   fatal;

	/* FIFO of decoded-but-not-yet-consumed interleaved samples */
	FLAC__int32 *fifo;
	size_t fifo_cap;         /* in samples (not frames) */
	size_t fifo_len;
	size_t fifo_head;
};

static void fifo_reset_(FLACSampleSeeker *s)
{
	s->fifo_len = 0;
	s->fifo_head = 0;
}

static FLAC__bool fifo_reserve_(FLACSampleSeeker *s, size_t extra)
{
	if(s->fifo_head + s->fifo_len + extra <= s->fifo_cap)
		return true;
	if(s->fifo_len + extra <= s->fifo_cap) {
		if(s->fifo_len > 0)
			memmove(s->fifo, s->fifo + s->fifo_head, s->fifo_len * sizeof(*s->fifo));
		s->fifo_head = 0;
		return true;
	}
	{
		size_t newcap = s->fifo_cap ? s->fifo_cap : 4096;
		FLAC__int32 *p;
		while(newcap < s->fifo_len + extra)
			newcap *= 2;
		p = malloc(newcap * sizeof(*p));
		if(!p)
			return false;
		if(s->fifo_len > 0)
			memcpy(p, s->fifo + s->fifo_head, s->fifo_len * sizeof(*p));
		free(s->fifo);
		s->fifo = p;
		s->fifo_cap = newcap;
		s->fifo_head = 0;
		return true;
	}
}

static FLAC__StreamDecoderWriteStatus write_cb_(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *client_data)
{
	FLACSampleSeeker *s = client_data;
	FLAC__uint64 first = frame->header.number.sample_number;
	uint32_t blocksize = frame->header.blocksize;
	uint32_t channels = frame->header.channels;
	uint32_t skip = 0, i, c;

	(void)decoder;

	if(s->fatal)
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;

	/* Drop leading samples that precede the seek target. */
	if(first < s->target) {
		FLAC__uint64 d = s->target - first;
		if(d >= blocksize) {
			/* Whole frame precedes the target: decoder was still
			 * finishing a frame started before the seek.  Discard it
			 * completely so no stale sample leaks through. */
			return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
		}
		skip = (uint32_t)d;
	}

	if(!fifo_reserve_(s, (size_t)(blocksize - skip) * channels)) {
		s->fatal = true;
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	}

	for(i = skip; i < blocksize; i++)
		for(c = 0; c < channels; c++)
			s->fifo[s->fifo_head + s->fifo_len++] = buffer[c][i];

	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void metadata_cb_(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
	FLACSampleSeeker *s = client_data;
	(void)decoder;
	if(metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
		s->total_samples = metadata->data.stream_info.total_samples;
		s->channels = metadata->data.stream_info.channels;
		s->bits_per_sample = metadata->data.stream_info.bits_per_sample;
		s->sample_rate = metadata->data.stream_info.sample_rate;
	}
}

static void error_cb_(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
	FLACSampleSeeker *s = client_data;
	(void)decoder; (void)status;
	s->fatal = true;
}

FLACSampleSeeker *fss_open(const char *path)
{
	FLACSampleSeeker *s = calloc(1, sizeof(*s));
	FLAC__StreamDecoderInitStatus st;

	if(!s)
		return NULL;
	s->decoder = FLAC__stream_decoder_new();
	if(!s->decoder) {
		free(s);
		return NULL;
	}
	FLAC__stream_decoder_set_md5_checking(s->decoder, true);
	st = FLAC__stream_decoder_init_file(s->decoder, path, write_cb_, metadata_cb_, error_cb_, s);
	if(st != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		fss_close(s);
		return NULL;
	}
	if(!FLAC__stream_decoder_process_until_end_of_metadata(s->decoder)) {
		fss_close(s);
		return NULL;
	}
	s->need_seek = false; /* fresh decoder is positioned at sample 0 */
	return s;
}

FLAC__uint64 fss_total_samples(const FLACSampleSeeker *s) { return s->total_samples; }
uint32_t     fss_channels(const FLACSampleSeeker *s) { return s->channels; }
uint32_t     fss_bits_per_sample(const FLACSampleSeeker *s) { return s->bits_per_sample; }
uint32_t     fss_sample_rate(const FLACSampleSeeker *s) { return s->sample_rate; }
FLAC__uint64 fss_tell(const FLACSampleSeeker *s) { return s->position; }

FLAC__bool fss_seek(FLACSampleSeeker *s, FLAC__uint64 sample)
{
	if(sample >= s->total_samples) {
		/* Out of range: refuse, and force the caller to seek to a valid
		 * position before reading again. */
		FLAC__stream_decoder_flush(s->decoder);
		fifo_reset_(s);
		s->need_seek = true;
		s->eof = false;
		return false;
	}
	/* Drop stale pre-seek samples up front.  Note that the write
	 * callback fires synchronously from inside
	 * FLAC__stream_decoder_seek_absolute() with the tail of the frame
	 * containing the target; those samples belong to the post-seek
	 * stream and must be kept. */
	fifo_reset_(s);
	s->target = sample;
	s->position = sample;
	if(!FLAC__stream_decoder_seek_absolute(s->decoder, sample)) {
		FLAC__stream_decoder_flush(s->decoder);
		fifo_reset_(s);
		s->need_seek = true;
		s->eof = false;
		return false;
	}
	s->need_seek = false;
	s->eof = false;
	return true;
}

FLAC__uint64 fss_read(FLACSampleSeeker *s, FLAC__int32 *out, FLAC__uint64 nframes)
{
	FLAC__uint64 produced = 0;

	if(s->need_seek || s->fatal || nframes == 0)
		return 0;

	while(produced < nframes) {
		/* Serve from the FIFO first. */
		while(produced < nframes && s->fifo_len >= s->channels) {
			memcpy(out + produced * s->channels,
			       s->fifo + s->fifo_head,
			       s->channels * sizeof(*out));
			s->fifo_head += s->channels;
			s->fifo_len -= s->channels;
			if(s->fifo_len == 0)
				s->fifo_head = 0;
			produced++;
			s->position++;
		}
		if(produced >= nframes || s->eof)
			break;

		/* Decode one more frame into the FIFO. */
		if(!FLAC__stream_decoder_process_single(s->decoder) ||
		   FLAC__stream_decoder_get_state(s->decoder) == FLAC__STREAM_DECODER_END_OF_STREAM) {
			s->eof = true;
		}
		if(s->fatal) {
			s->eof = true;
			break;
		}
	}
	return produced;
}

void fss_close(FLACSampleSeeker *s)
{
	if(!s)
		return;
	if(s->decoder) {
		FLAC__stream_decoder_finish(s->decoder);
		FLAC__stream_decoder_delete(s->decoder);
	}
	free(s->fifo);
	free(s);
}
