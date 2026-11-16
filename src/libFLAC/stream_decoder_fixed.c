/* libFLAC - Free Lossless Audio Codec library
 * Copyright (C) 2025  Xiph.Org Foundation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * - Neither the name of the Xiph.org Foundation nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "FLAC/stream_decoder_fixed.h"
#include "protected/stream_decoder.h"
#include "share/alloc.h"

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
/* GCC/Clang atomic builtins are available in both C99 and C11 modes. */
#else
#define FLAC__FIXED_NO_ATOMICS
#endif

struct FLAC__StreamDecoderFixed {
	FLAC__StreamDecoder *decoder;
	FLAC__StreamDecoderReadCallback read_callback;
	void *read_client_data;
	FLAC__int32 *pcm_ring;
	uint32_t pcm_ring_bytes;
	uint32_t pcm_ring_capacity_samples;
	uint32_t pcm_ring_mask;
	uint32_t max_frame_samples;
	uint32_t channels;
	uint32_t sample_rate;
	uint32_t bits_per_sample;
	uint32_t stream_max_blocksize;
	FLAC__uint64 total_samples;
	FLAC__uint64 produced_frames;
	size_t core_bytes;
	size_t budget_bytes;
	volatile uint32_t write_pos;
	volatile uint32_t read_pos;
	volatile uint32_t ended;
	volatile uint32_t error_status;
};

FLAC_API const char * const FLAC__StreamDecoderFixedStatusString[] = {
	"FLAC__STREAM_DECODER_FIXED_STATUS_OK",
	"FLAC__STREAM_DECODER_FIXED_STATUS_UNDERRUN",
	"FLAC__STREAM_DECODER_FIXED_STATUS_OUTPUT_FULL",
	"FLAC__STREAM_DECODER_FIXED_STATUS_END_OF_STREAM",
	"FLAC__STREAM_DECODER_FIXED_STATUS_FRAME_TOO_LARGE",
	"FLAC__STREAM_DECODER_FIXED_STATUS_DECODE_ERROR",
	"FLAC__STREAM_DECODER_FIXED_STATUS_INVALID_ARGUMENT",
	"FLAC__STREAM_DECODER_FIXED_STATUS_OUT_OF_MEMORY"
};

static uint32_t load_acquire_(const volatile uint32_t *p)
{
#if defined(_MSC_VER)
	return (uint32_t)_InterlockedCompareExchange((volatile long *)p, 0, 0);
#elif defined(__GNUC__) || defined(__clang__)
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
#else
	return *p;
#endif
}

static void store_release_(volatile uint32_t *p, uint32_t value)
{
#if defined(_MSC_VER)
	_InterlockedExchange((volatile long *)p, (long)value);
#elif defined(__GNUC__) || defined(__clang__)
	__atomic_store_n(p, value, __ATOMIC_RELEASE);
#else
	*p = value;
#endif
}

static uint32_t ring_available_samples_(const FLAC__StreamDecoderFixed *decoder)
{
	return load_acquire_(&decoder->write_pos) - load_acquire_(&decoder->read_pos);
}

static void set_error_(FLAC__StreamDecoderFixed *decoder, FLAC__StreamDecoderFixedStatus status)
{
	uint32_t expected = FLAC__STREAM_DECODER_FIXED_STATUS_OK;
	uint32_t value = (uint32_t)status;

#if defined(_MSC_VER)
	_InterlockedCompareExchange((volatile long *)&decoder->error_status, (long)value, (long)expected);
#elif defined(__GNUC__) || defined(__clang__)
	__atomic_compare_exchange_n(&decoder->error_status, &expected, value, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
#else
	if(decoder->error_status == expected)
		decoder->error_status = value;
#endif
}

static FLAC__StreamDecoderFixedStatus get_error_(const FLAC__StreamDecoderFixed *decoder)
{
	return (FLAC__StreamDecoderFixedStatus)load_acquire_(&decoder->error_status);
}

static FLAC__bool pcm_ring_write_(FLAC__StreamDecoderFixed *decoder, const FLAC__int32 * const buffer[], uint32_t frames)
{
	uint32_t channel;
	uint32_t sample;
	uint32_t samples = frames * decoder->channels;
	uint32_t write_pos = decoder->write_pos;
	uint32_t write_index = write_pos & decoder->pcm_ring_mask;

	if(samples > decoder->pcm_ring_capacity_samples - ring_available_samples_(decoder))
		return false;

	for(sample = 0; sample < frames; sample++) {
		for(channel = 0; channel < decoder->channels; channel++) {
			uint32_t index = (write_index + sample * decoder->channels + channel) & decoder->pcm_ring_mask;
			decoder->pcm_ring[index] = buffer[channel][sample];
		}
	}

	write_pos += samples;
	store_release_(&decoder->write_pos, write_pos);
	decoder->produced_frames += frames;

	return true;
}

static FLAC__StreamDecoderWriteStatus write_callback_(const FLAC__StreamDecoder *flac_decoder, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *client_data)
{
	FLAC__StreamDecoderFixed *decoder = (FLAC__StreamDecoderFixed *)client_data;

	(void)flac_decoder;

	if(frame->header.channels != decoder->channels ||
	   frame->header.blocksize > decoder->max_frame_samples / decoder->channels) {
		set_error_(decoder, FLAC__STREAM_DECODER_FIXED_STATUS_FRAME_TOO_LARGE);
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	}

	if(!pcm_ring_write_(decoder, buffer, frame->header.blocksize)) {
		set_error_(decoder, FLAC__STREAM_DECODER_FIXED_STATUS_DECODE_ERROR);
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	}

	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void metadata_callback_(const FLAC__StreamDecoder *flac_decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
	FLAC__StreamDecoderFixed *decoder = (FLAC__StreamDecoderFixed *)client_data;

	(void)flac_decoder;

	if(metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
		decoder->channels = metadata->data.stream_info.channels;
		decoder->sample_rate = metadata->data.stream_info.sample_rate;
		decoder->bits_per_sample = metadata->data.stream_info.bits_per_sample;
		decoder->stream_max_blocksize = metadata->data.stream_info.max_blocksize;
		decoder->total_samples = metadata->data.stream_info.total_samples;
	}
}

static void error_callback_(const FLAC__StreamDecoder *flac_decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
	FLAC__StreamDecoderFixed *decoder = (FLAC__StreamDecoderFixed *)client_data;

	(void)flac_decoder;

	if(status == FLAC__STREAM_DECODER_ERROR_STATUS_OUT_OF_BOUNDS)
		set_error_(decoder, FLAC__STREAM_DECODER_FIXED_STATUS_FRAME_TOO_LARGE);
	else
		set_error_(decoder, FLAC__STREAM_DECODER_FIXED_STATUS_DECODE_ERROR);
}

static FLAC__StreamDecoderReadStatus read_callback_(const FLAC__StreamDecoder *flac_decoder, FLAC__byte buffer[], size_t *bytes, void *client_data)
{
	FLAC__StreamDecoderFixed *decoder = (FLAC__StreamDecoderFixed *)client_data;
	const size_t requested = *bytes;
	FLAC__StreamDecoderReadStatus status;

	status = decoder->read_callback(flac_decoder, buffer, bytes, decoder->read_client_data);

	if(*bytes > requested ||
	   (status == FLAC__STREAM_DECODER_READ_STATUS_CONTINUE && *bytes == 0) ||
	   ((status == FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM || status == FLAC__STREAM_DECODER_READ_STATUS_END_OF_LINK) && *bytes != 0)) {
		set_error_(decoder, FLAC__STREAM_DECODER_FIXED_STATUS_DECODE_ERROR);
		return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
	}
	if(status == FLAC__STREAM_DECODER_READ_STATUS_ABORT)
		set_error_(decoder, FLAC__STREAM_DECODER_FIXED_STATUS_DECODE_ERROR);

	return status;
}

static FLAC__bool config_valid_(const FLAC__StreamDecoderFixedConfig *config)
{
	uint64_t required_ring;

	if(config == NULL)
		return false;
	if(config->max_blocksize < FLAC__MIN_BLOCK_SIZE || config->max_blocksize > FLAC__MAX_BLOCK_SIZE)
		return false;
	if(config->max_channels < 1 || config->max_channels > FLAC__MAX_CHANNELS)
		return false;
	if(config->max_bits_per_sample < FLAC__MIN_BITS_PER_SAMPLE || config->max_bits_per_sample > FLAC__MAX_BITS_PER_SAMPLE)
		return false;
	if(config->pcm_ring_bytes == 0 || config->pcm_ring_bytes % sizeof(FLAC__int32) != 0)
		return false;
	{
		uint32_t ring_samples = config->pcm_ring_bytes / sizeof(FLAC__int32);
		if((ring_samples & (ring_samples - 1u)) != 0)
			return false;
	}
	if(config->memory_budget_bytes > FLAC__STREAM_DECODER_FIXED_MAX_MEMORY_BUDGET)
		return false;

	required_ring = (uint64_t)config->max_blocksize * config->max_channels;
	if(required_ring > UINT32_MAX / sizeof(FLAC__int32))
		return false;
	if(config->pcm_ring_bytes / sizeof(FLAC__int32) < required_ring)
		return false;

	return true;
}

FLAC_API FLAC__StreamDecoderFixed *FLAC__stream_decoder_fixed_new(
	const FLAC__StreamDecoderFixedConfig *config,
	FLAC__StreamDecoderReadCallback read_callback,
	void *client_data,
	FLAC__StreamDecoderFixedStatus *status)
{
	FLAC__StreamDecoderFixed *decoder = NULL;
	FLAC__StreamDecoderInitStatus init_status;
	size_t budget;
	size_t core_bytes;
	FLAC__StreamDecoderFixedStatus result = FLAC__STREAM_DECODER_FIXED_STATUS_OK;

	if(status != NULL)
		*status = FLAC__STREAM_DECODER_FIXED_STATUS_INVALID_ARGUMENT;
	if(!config_valid_(config) || read_callback == NULL)
		return NULL;

	budget = config->memory_budget_bytes != 0 ? config->memory_budget_bytes : FLAC__STREAM_DECODER_FIXED_DEFAULT_MEMORY_BUDGET;
	core_bytes = FLAC__stream_decoder_get_fixed_memory_requirement(config->max_blocksize, config->max_channels, config->max_bits_per_sample);
	if(core_bytes == 0 || core_bytes > budget ||
	   sizeof(*decoder) > budget - core_bytes ||
	   config->pcm_ring_bytes > budget - core_bytes - sizeof(*decoder)) {
		if(status != NULL)
			*status = FLAC__STREAM_DECODER_FIXED_STATUS_OUT_OF_MEMORY;
		return NULL;
	}

	decoder = safe_calloc_(1, sizeof(*decoder));
	if(decoder == NULL) {
		if(status != NULL)
			*status = FLAC__STREAM_DECODER_FIXED_STATUS_OUT_OF_MEMORY;
		return NULL;
	}

	decoder->pcm_ring = safe_malloc_(config->pcm_ring_bytes);
	if(decoder->pcm_ring == NULL) {
		result = FLAC__STREAM_DECODER_FIXED_STATUS_OUT_OF_MEMORY;
		goto error;
	}

	decoder->read_callback = read_callback;
	decoder->read_client_data = client_data;
	decoder->pcm_ring_bytes = config->pcm_ring_bytes;
	decoder->pcm_ring_capacity_samples = config->pcm_ring_bytes / sizeof(FLAC__int32);
	decoder->pcm_ring_mask = decoder->pcm_ring_capacity_samples - 1u;
	decoder->max_frame_samples = config->max_blocksize * config->max_channels;
	decoder->core_bytes = core_bytes;
	decoder->budget_bytes = budget;
	decoder->write_pos = 0;
	decoder->read_pos = 0;
	decoder->ended = 0;
	decoder->error_status = FLAC__STREAM_DECODER_FIXED_STATUS_OK;

	decoder->decoder = FLAC__stream_decoder_new();
	if(decoder->decoder == NULL) {
		result = FLAC__STREAM_DECODER_FIXED_STATUS_OUT_OF_MEMORY;
		goto error;
	}

	if(!FLAC__stream_decoder_set_metadata_ignore_all(decoder->decoder) ||
	   !FLAC__stream_decoder_set_metadata_respond(decoder->decoder, FLAC__METADATA_TYPE_STREAMINFO)) {
		result = FLAC__STREAM_DECODER_FIXED_STATUS_INVALID_ARGUMENT;
		goto error;
	}

	FLAC__stream_decoder_set_fixed_memory_mode(decoder->decoder);

	init_status = FLAC__stream_decoder_init_stream(
		decoder->decoder,
		read_callback_,
		/*seek_callback=*/NULL,
		/*tell_callback=*/NULL,
		/*length_callback=*/NULL,
		/*eof_callback=*/NULL,
		write_callback_,
		metadata_callback_,
		error_callback_,
		decoder);

	if(init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		result = FLAC__STREAM_DECODER_FIXED_STATUS_DECODE_ERROR;
		goto error;
	}

	if(!FLAC__stream_decoder_process_until_end_of_metadata(decoder->decoder)) {
		result = FLAC__STREAM_DECODER_FIXED_STATUS_DECODE_ERROR;
		goto error;
	}
	result = get_error_(decoder);
	if(result != FLAC__STREAM_DECODER_FIXED_STATUS_OK)
		goto error;

	if(decoder->channels == 0 || decoder->sample_rate == 0 ||
	   decoder->channels > config->max_channels ||
	   decoder->bits_per_sample > config->max_bits_per_sample ||
	   decoder->stream_max_blocksize > config->max_blocksize) {
		result = FLAC__STREAM_DECODER_FIXED_STATUS_FRAME_TOO_LARGE;
		goto error;
	}

	if(!FLAC__stream_decoder_reserve_fixed_memory(
		   decoder->decoder,
		   config->max_blocksize,
		   config->max_channels,
		   config->max_bits_per_sample)) {
		result = FLAC__STREAM_DECODER_FIXED_STATUS_OUT_OF_MEMORY;
		goto error;
	}

	if(status != NULL)
		*status = FLAC__STREAM_DECODER_FIXED_STATUS_OK;
	return decoder;

error:
	if(decoder != NULL) {
		if(decoder->decoder != NULL)
			FLAC__stream_decoder_delete(decoder->decoder);
		free(decoder->pcm_ring);
		free(decoder);
	}
	if(status != NULL)
		*status = result;
	return NULL;
}

FLAC_API void FLAC__stream_decoder_fixed_delete(FLAC__StreamDecoderFixed *decoder)
{
	if(decoder == NULL)
		return;

	FLAC__stream_decoder_delete(decoder->decoder);
	free(decoder->pcm_ring);
	free(decoder);
}

FLAC_API FLAC__StreamDecoderFixedStatus FLAC__stream_decoder_fixed_pump(
	FLAC__StreamDecoderFixed *decoder,
	uint32_t max_frames,
	uint32_t *frames_decoded)
{
	uint32_t produced = 0;
	FLAC__StreamDecoderFixedStatus error;

	if(frames_decoded != NULL)
		*frames_decoded = 0;
	if(decoder == NULL)
		return FLAC__STREAM_DECODER_FIXED_STATUS_INVALID_ARGUMENT;

	error = get_error_(decoder);
	if(error != FLAC__STREAM_DECODER_FIXED_STATUS_OK)
		return error;
	if(load_acquire_(&decoder->ended))
		return FLAC__STREAM_DECODER_FIXED_STATUS_END_OF_STREAM;

	while(produced < max_frames) {
		FLAC__uint64 before = decoder->produced_frames;
		uint32_t newly_produced;
		FLAC__StreamDecoderState state;
		FLAC__bool ok;

		if(decoder->pcm_ring_capacity_samples - ring_available_samples_(decoder) < decoder->max_frame_samples) {
			if(frames_decoded != NULL)
				*frames_decoded = produced;
			return FLAC__STREAM_DECODER_FIXED_STATUS_OUTPUT_FULL;
		}

		ok = FLAC__stream_decoder_process_single(decoder->decoder);
		state = FLAC__stream_decoder_get_state(decoder->decoder);
		newly_produced = (uint32_t)(decoder->produced_frames - before);
		produced += newly_produced;

		error = get_error_(decoder);
		if(error != FLAC__STREAM_DECODER_FIXED_STATUS_OK) {
			if(frames_decoded != NULL)
				*frames_decoded = produced;
			return error;
		}

		if(!ok || state == FLAC__STREAM_DECODER_ABORTED || state == FLAC__STREAM_DECODER_MEMORY_ALLOCATION_ERROR) {
			set_error_(decoder, FLAC__STREAM_DECODER_FIXED_STATUS_DECODE_ERROR);
			if(frames_decoded != NULL)
				*frames_decoded = produced;
			return FLAC__STREAM_DECODER_FIXED_STATUS_DECODE_ERROR;
		}

		if(state == FLAC__STREAM_DECODER_END_OF_STREAM) {
			store_release_(&decoder->ended, 1);
			if(frames_decoded != NULL)
				*frames_decoded = produced;
			return produced != 0 ? FLAC__STREAM_DECODER_FIXED_STATUS_OK : FLAC__STREAM_DECODER_FIXED_STATUS_END_OF_STREAM;
		}
	}

	if(frames_decoded != NULL)
		*frames_decoded = produced;
	return FLAC__STREAM_DECODER_FIXED_STATUS_OK;
}

FLAC_API uint32_t FLAC__stream_decoder_fixed_read(
	FLAC__StreamDecoderFixed *decoder,
	FLAC__int32 *output,
	uint32_t frames,
	FLAC__StreamDecoderFixedStatus *status)
{
	uint32_t available_frames;
	uint32_t frames_to_copy;
	uint32_t samples_to_copy;
	uint32_t read_pos;
	uint32_t read_index;
	uint32_t first;
	FLAC__StreamDecoderFixedStatus error;

	if(status != NULL)
		*status = FLAC__STREAM_DECODER_FIXED_STATUS_OK;
	if(decoder == NULL || (output == NULL && frames != 0)) {
		if(status != NULL)
			*status = FLAC__STREAM_DECODER_FIXED_STATUS_INVALID_ARGUMENT;
		return 0;
	}
	if(frames == 0)
		return 0;

	available_frames = ring_available_samples_(decoder) / decoder->channels;
	if(available_frames == 0) {
		error = get_error_(decoder);
		if(error != FLAC__STREAM_DECODER_FIXED_STATUS_OK) {
			if(status != NULL)
				*status = error;
		}
		else if(load_acquire_(&decoder->ended)) {
			if(status != NULL)
				*status = FLAC__STREAM_DECODER_FIXED_STATUS_END_OF_STREAM;
		}
		else {
			if(status != NULL)
				*status = FLAC__STREAM_DECODER_FIXED_STATUS_UNDERRUN;
		}
		return 0;
	}

	frames_to_copy = frames < available_frames ? frames : available_frames;
	samples_to_copy = frames_to_copy * decoder->channels;
	read_pos = decoder->read_pos;
	read_index = read_pos & decoder->pcm_ring_mask;
	first = decoder->pcm_ring_capacity_samples - read_index;
	if(first > samples_to_copy)
		first = samples_to_copy;

	memcpy(output, decoder->pcm_ring + read_index, (size_t)first * sizeof(FLAC__int32));
	if(samples_to_copy > first)
		memcpy(output + first, decoder->pcm_ring, (size_t)(samples_to_copy - first) * sizeof(FLAC__int32));

	read_pos += samples_to_copy;
	store_release_(&decoder->read_pos, read_pos);

	return frames_to_copy;
}

FLAC_API uint32_t FLAC__stream_decoder_fixed_available_frames(const FLAC__StreamDecoderFixed *decoder)
{
	if(decoder == NULL || decoder->channels == 0)
		return 0;
	return ring_available_samples_(decoder) / decoder->channels;
}

FLAC_API FLAC__bool FLAC__stream_decoder_fixed_get_info(
	const FLAC__StreamDecoderFixed *decoder,
	FLAC__StreamDecoderFixedInfo *info)
{
	if(decoder == NULL || info == NULL)
		return false;

	info->sample_rate = decoder->sample_rate;
	info->channels = decoder->channels;
	info->bits_per_sample = decoder->bits_per_sample;
	info->total_samples = decoder->total_samples;
	return true;
}

FLAC_API FLAC__bool FLAC__stream_decoder_fixed_get_memory(
	const FLAC__StreamDecoderFixed *decoder,
	FLAC__StreamDecoderFixedMemory *memory)
{
	if(decoder == NULL || memory == NULL)
		return false;

	memory->budget_bytes = decoder->budget_bytes;
	memory->core_bytes = decoder->core_bytes;
	memory->ring_bytes = decoder->pcm_ring_bytes;
	memory->object_bytes = sizeof(*decoder);
	memory->total_bytes = decoder->core_bytes + decoder->pcm_ring_bytes + sizeof(*decoder);
	return true;
}
