/* test_libFLAC - Unit tester for libFLAC
 * Copyright (C) 2025  Xiph.Org Foundation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fixed_decoder.h"
#include "FLAC/stream_decoder_fixed.h"
#include "FLAC/stream_encoder.h"
#include "share/compat.h"

#define TEST_FILENAME "fixed_decoder.flac"
#define TEST_BLOCKSIZE 1024u
#define TEST_CHANNELS 2u
#define TEST_BITS_PER_SAMPLE 16u
#define TEST_FRAMES (TEST_BLOCKSIZE * 128u)
#define TEST_RING_SAMPLES (TEST_BLOCKSIZE * TEST_CHANNELS * 2u)

typedef struct {
	FILE *file;
} ReadClient;

static FLAC__StreamDecoderReadStatus read_callback_(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data)
{
	ReadClient *client = (ReadClient *)client_data;

	(void)decoder;

	if(*bytes == 0)
		return FLAC__STREAM_DECODER_READ_STATUS_ABORT;

	*bytes = fread(buffer, 1, *bytes, client->file);
	if(ferror(client->file))
		return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
	if(*bytes == 0)
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderReadStatus abort_read_callback_(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data)
{
	(void)decoder, (void)buffer, (void)client_data;
	*bytes = 0;
	return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
}

static FLAC__StreamDecoderReadStatus empty_read_callback_(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data)
{
	(void)decoder, (void)buffer, (void)client_data;
	*bytes = 0;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamEncoderWriteStatus encoder_write_callback_(const FLAC__StreamEncoder *encoder, const FLAC__byte buffer[], size_t bytes, uint32_t samples, uint32_t current_frame, void *client_data)
{
	FILE *file = (FILE *)client_data;

	(void)encoder, (void)samples, (void)current_frame;
	return fwrite(buffer, 1, bytes, file) == bytes ? FLAC__STREAM_ENCODER_WRITE_STATUS_OK : FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
}

static FLAC__bool generate_test_file_(void)
{
	FLAC__StreamEncoder *encoder = NULL;
	FLAC__int32 samples[TEST_BLOCKSIZE * TEST_CHANNELS];
	FILE *file = NULL;
	uint32_t frame;
	uint32_t sample;
	FLAC__bool ok = false;

	file = flac_fopen(TEST_FILENAME, "wb");
	if(file == NULL)
		return false;

	encoder = FLAC__stream_encoder_new();
	if(encoder == NULL)
		goto cleanup;

	FLAC__stream_encoder_set_verify(encoder, false);
	FLAC__stream_encoder_set_streamable_subset(encoder, true);
	FLAC__stream_encoder_set_do_mid_side_stereo(encoder, false);
	FLAC__stream_encoder_set_channels(encoder, TEST_CHANNELS);
	FLAC__stream_encoder_set_bits_per_sample(encoder, TEST_BITS_PER_SAMPLE);
	FLAC__stream_encoder_set_sample_rate(encoder, 44100);
	FLAC__stream_encoder_set_blocksize(encoder, TEST_BLOCKSIZE);
	FLAC__stream_encoder_set_total_samples_estimate(encoder, TEST_FRAMES);
	FLAC__stream_encoder_set_max_lpc_order(encoder, 0);
	FLAC__stream_encoder_set_max_residual_partition_order(encoder, 0);

	if(FLAC__stream_encoder_init_stream(encoder, encoder_write_callback_, NULL, NULL, NULL, file) != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
		goto cleanup;

	for(frame = 0; frame < TEST_FRAMES; frame += TEST_BLOCKSIZE) {
		for(sample = 0; sample < TEST_BLOCKSIZE * TEST_CHANNELS; sample++) {
			uint32_t global = frame * TEST_CHANNELS + sample;
			samples[sample] = (FLAC__int32)((global % TEST_BLOCKSIZE) & 7u);
		}
		if(!FLAC__stream_encoder_process_interleaved(encoder, samples, TEST_BLOCKSIZE))
			goto cleanup;
	}

	ok = FLAC__stream_encoder_finish(encoder);

cleanup:
	if(encoder != NULL)
		FLAC__stream_encoder_delete(encoder);
	fclose(file);
	return ok;
}

static FLAC__bool open_input_(ReadClient *client)
{
	client->file = flac_fopen(TEST_FILENAME, "rb");
	return client->file != NULL;
}

static void close_input_(ReadClient *client)
{
	if(client->file != NULL) {
		fclose(client->file);
		client->file = NULL;
	}
}

static FLAC__bool drain_all_(FLAC__StreamDecoderFixed *decoder, FLAC__int32 output[333 * TEST_CHANNELS], FLAC__uint64 *samples_read)
{
	FLAC__StreamDecoderFixedStatus status;

	while(FLAC__stream_decoder_fixed_available_frames(decoder) != 0) {
		uint32_t i;
		uint32_t got = FLAC__stream_decoder_fixed_read(decoder, output, 333, &status);

		if(got == 0 || status != FLAC__STREAM_DECODER_FIXED_STATUS_OK)
			return false;
		for(i = 0; i < got * TEST_CHANNELS; i++) {
			FLAC__int32 expected = (FLAC__int32)(((*samples_read + i) % TEST_BLOCKSIZE) & 7u);
			if(output[i] != expected)
				return false;
		}
		*samples_read += (FLAC__uint64)got * TEST_CHANNELS;
	}

	return true;
}

static FLAC__bool check_invalid_configs_(void)
{
	FLAC__StreamDecoderFixedConfig config;
	FLAC__StreamDecoderFixedStatus status = FLAC__STREAM_DECODER_FIXED_STATUS_OK;

	memset(&config, 0, sizeof(config));
	config.max_blocksize = TEST_BLOCKSIZE;
	config.max_channels = TEST_CHANNELS;
	config.max_bits_per_sample = TEST_BITS_PER_SAMPLE;
	config.pcm_ring_bytes = 4;

	printf("testing fixed decoder invalid ring size... ");
	if(FLAC__stream_decoder_fixed_new(&config, abort_read_callback_, NULL, &status) != NULL ||
	   status != FLAC__STREAM_DECODER_FIXED_STATUS_INVALID_ARGUMENT) {
		printf("FAILED\n");
		return false;
	}
	printf("OK\n");

	config.pcm_ring_bytes = TEST_RING_SAMPLES * sizeof(FLAC__int32);
	config.memory_budget_bytes = FLAC__STREAM_DECODER_FIXED_MAX_MEMORY_BUDGET + 1u;
	printf("testing fixed decoder over-budget config... ");
	if(FLAC__stream_decoder_fixed_new(&config, abort_read_callback_, NULL, &status) != NULL ||
	   status != FLAC__STREAM_DECODER_FIXED_STATUS_INVALID_ARGUMENT) {
		printf("FAILED\n");
		return false;
	}
	printf("OK\n");

	printf("testing fixed decoder aborting input... ");
	config.memory_budget_bytes = 0;
	if(FLAC__stream_decoder_fixed_new(&config, abort_read_callback_, NULL, &status) != NULL ||
	   status != FLAC__STREAM_DECODER_FIXED_STATUS_DECODE_ERROR) {
		printf("FAILED\n");
		return false;
	}
	printf("OK\n");

	printf("testing fixed decoder empty read... ");
	if(FLAC__stream_decoder_fixed_new(&config, empty_read_callback_, NULL, &status) != NULL ||
	   status != FLAC__STREAM_DECODER_FIXED_STATUS_DECODE_ERROR) {
		printf("FAILED\n");
		return false;
	}
	printf("OK\n");

	config.memory_budget_bytes = 1024;
	printf("testing fixed decoder memory budget... ");
	if(FLAC__stream_decoder_fixed_new(&config, abort_read_callback_, NULL, &status) != NULL ||
	   status != FLAC__STREAM_DECODER_FIXED_STATUS_OUT_OF_MEMORY) {
		printf("FAILED\n");
		return false;
	}
	printf("OK\n");

	memset(&config, 0, sizeof(config));
	config.max_blocksize = FLAC__MAX_BLOCK_SIZE;
	config.max_channels = 4;
	config.max_bits_per_sample = 24;
	config.pcm_ring_bytes = 1024u * 1024u;
	printf("testing fixed decoder 4 MiB hard limit... ");
	if(FLAC__stream_decoder_fixed_new(&config, abort_read_callback_, NULL, &status) != NULL ||
	   status != FLAC__STREAM_DECODER_FIXED_STATUS_OUT_OF_MEMORY) {
		printf("FAILED\n");
		return false;
	}
	printf("OK\n");

	return true;
}

static FLAC__bool test_wrap_and_underrun_(void)
{
	FLAC__StreamDecoderFixedConfig config;
	FLAC__StreamDecoderFixedInfo info;
	FLAC__StreamDecoderFixedMemory memory;
	FLAC__StreamDecoderFixed *decoder;
	FLAC__StreamDecoderFixedStatus status;
	ReadClient client = { NULL };
	FLAC__int32 output[333 * TEST_CHANNELS];
	FLAC__uint64 samples_read = 0;
	FLAC__bool saw_output_full = false;
	FLAC__bool saw_eos = false;
	uint32_t frames;
	uint32_t guard = 0;

	if(!open_input_(&client))
		return false;

	memset(&config, 0, sizeof(config));
	config.max_blocksize = TEST_BLOCKSIZE;
	config.max_channels = TEST_CHANNELS;
	config.max_bits_per_sample = TEST_BITS_PER_SAMPLE;
	config.pcm_ring_bytes = TEST_RING_SAMPLES * sizeof(FLAC__int32);

	decoder = FLAC__stream_decoder_fixed_new(&config, read_callback_, &client, &status);
	if(decoder == NULL) {
		printf("FAILED, constructor status %s\n", FLAC__StreamDecoderFixedStatusString[status]);
		close_input_(&client);
		return false;
	}

	printf("testing fixed decoder info and memory... ");
	if(!FLAC__stream_decoder_fixed_get_info(decoder, &info) ||
	   info.sample_rate != 44100 || info.channels != TEST_CHANNELS ||
	   info.bits_per_sample != TEST_BITS_PER_SAMPLE || info.total_samples != TEST_FRAMES ||
	   !FLAC__stream_decoder_fixed_get_memory(decoder, &memory) ||
	   memory.total_bytes > memory.budget_bytes ||
	   memory.total_bytes > FLAC__STREAM_DECODER_FIXED_MAX_MEMORY_BUDGET ||
	   memory.total_bytes != memory.core_bytes + memory.ring_bytes + memory.object_bytes) {
		printf("FAILED\n");
		FLAC__stream_decoder_fixed_delete(decoder);
		close_input_(&client);
		return false;
	}
	printf("OK (%zu bytes)\n", memory.total_bytes);

	printf("testing fixed decoder initial underrun... ");
	frames = FLAC__stream_decoder_fixed_read(decoder, output, 1, &status);
	if(frames != 0 || status != FLAC__STREAM_DECODER_FIXED_STATUS_UNDERRUN) {
		printf("FAILED\n");
		FLAC__stream_decoder_fixed_delete(decoder);
		close_input_(&client);
		return false;
	}
	printf("OK\n");

	while(guard++ < TEST_FRAMES / TEST_BLOCKSIZE * 8u) {
		FLAC__StreamDecoderFixedStatus pump_status = FLAC__stream_decoder_fixed_pump(decoder, 1, &frames);

		if(pump_status == FLAC__STREAM_DECODER_FIXED_STATUS_OUTPUT_FULL) {
			saw_output_full = true;
			if(!drain_all_(decoder, output, &samples_read)) {
				printf("FAILED, PCM mismatch or read error\n");
				FLAC__stream_decoder_fixed_delete(decoder);
				close_input_(&client);
				return false;
			}
		}
		else if(pump_status != FLAC__STREAM_DECODER_FIXED_STATUS_OK &&
		        pump_status != FLAC__STREAM_DECODER_FIXED_STATUS_END_OF_STREAM) {
			printf("FAILED, pump status %s\n", FLAC__StreamDecoderFixedStatusString[pump_status]);
			FLAC__stream_decoder_fixed_delete(decoder);
			close_input_(&client);
			return false;
		}

		if(pump_status == FLAC__STREAM_DECODER_FIXED_STATUS_END_OF_STREAM) {
			saw_eos = true;
			break;
		}
	}

	if(saw_eos && !drain_all_(decoder, output, &samples_read)) {
		printf("FAILED, final PCM mismatch or read error\n");
		FLAC__stream_decoder_fixed_delete(decoder);
		close_input_(&client);
		return false;
	}

	printf("testing fixed decoder ring wrap and backpressure... ");
	if(!saw_output_full || !saw_eos || samples_read != (FLAC__uint64)TEST_FRAMES * TEST_CHANNELS) {
		printf("FAILED, output_full=%d eos=%d samples=%" PRIu64 "\n", (int)saw_output_full, (int)saw_eos, samples_read);
		FLAC__stream_decoder_fixed_delete(decoder);
		close_input_(&client);
		return false;
	}
	printf("OK\n");

	printf("testing fixed decoder final underrun/EOS... ");
	frames = FLAC__stream_decoder_fixed_read(decoder, output, 1, &status);
	if(frames != 0 || status != FLAC__STREAM_DECODER_FIXED_STATUS_END_OF_STREAM) {
		printf("FAILED\n");
		FLAC__stream_decoder_fixed_delete(decoder);
		close_input_(&client);
		return false;
	}
	printf("OK\n");

	printf("testing fixed decoder invalid read arguments... ");
	if(FLAC__stream_decoder_fixed_read(decoder, NULL, 1, &status) != 0 ||
	   status != FLAC__STREAM_DECODER_FIXED_STATUS_INVALID_ARGUMENT) {
		printf("FAILED\n");
		FLAC__stream_decoder_fixed_delete(decoder);
		close_input_(&client);
		return false;
	}
	printf("OK\n");

	FLAC__stream_decoder_fixed_delete(decoder);
	close_input_(&client);
	return true;
}

static FLAC__bool test_frame_too_large_(void)
{
	FLAC__StreamDecoderFixedConfig config;
	FLAC__StreamDecoderFixedStatus status;
	ReadClient client = { NULL };

	if(!open_input_(&client))
		return false;

	memset(&config, 0, sizeof(config));
	config.max_blocksize = TEST_BLOCKSIZE / 2;
	config.max_channels = TEST_CHANNELS;
	config.max_bits_per_sample = TEST_BITS_PER_SAMPLE;
	config.pcm_ring_bytes = TEST_BLOCKSIZE * TEST_CHANNELS * sizeof(FLAC__int32);

	printf("testing fixed decoder configured frame limit... ");
	if(FLAC__stream_decoder_fixed_new(&config, read_callback_, &client, &status) != NULL ||
	   status != FLAC__STREAM_DECODER_FIXED_STATUS_FRAME_TOO_LARGE) {
		printf("FAILED\n");
		close_input_(&client);
		return false;
	}
	printf("OK\n");

	close_input_(&client);
	return true;
}

FLAC__bool test_fixed_decoder(void)
{
	FLAC__bool ok = true;

	printf("\nTesting fixed-memory stream decoder...\n");

	if(!generate_test_file_()) {
		printf("ERROR: generating fixed decoder test file\n");
		return false;
	}

	ok = check_invalid_configs_() && ok;
	ok = test_frame_too_large_() && ok;
	ok = test_wrap_and_underrun_() && ok;

	(void)remove(TEST_FILENAME);

	return ok;
}
