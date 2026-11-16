#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FLAC/format.h"
#include "FLAC/metadata.h"
#include "FLAC/stream_decoder.h"

__attribute__((weak)) void __sanitizer_set_death_callback(void (*callback)(void));

typedef struct {
	const char *path;
	FLAC__byte *data;
	size_t size;
	size_t pos;
	size_t last_read_start;
	size_t last_read_end;
	uint64_t read_calls;
	uint64_t decode_position;

	FLAC__bool expect_valid;
	FLAC__bool decode_ok;
	FLAC__bool finish_ok;
	FLAC__StreamDecoderState end_state;

	FLAC__bool got_streaminfo;
	uint32_t streaminfo_channels;
	uint32_t streaminfo_bps;
	uint32_t streaminfo_rate;
	uint64_t streaminfo_total_samples;

	uint64_t frames;
	uint64_t samples;
	uint64_t metadata_count;
	uint64_t semantic_illegal;
	uint64_t errors;
	uint64_t first_error_offset;
	char first_error[96];

	FLAC__bool chain_ok;
	FLAC__Metadata_ChainStatus chain_status;
	uint64_t chain_blocks;

	FLAC__bool invariant;
	uint64_t invariant_offset;
	char invariant_message[192];

	FILE *dump;
	uint64_t dump_bytes;
} PropertyContext;

static volatile uint64_t death_offset;
static const char *volatile death_path;

static void sanitizer_death_callback(void)
{
	fprintf(stderr,
		"\nFLAC_PROPERTY_SANITIZER_DEATH\tpath=%s\toffset=%" PRIu64 "\n",
		death_path != NULL ? death_path : "(unknown)", death_offset);
	fflush(stderr);
}

static uint64_t decoder_position(PropertyContext *ctx, const FLAC__StreamDecoder *decoder)
{
	FLAC__uint64 position = 0;

	if(decoder != NULL && FLAC__stream_decoder_get_decode_position(decoder, &position))
		return position;
	return ctx->last_read_start;
}

static void mark_invariant(PropertyContext *ctx, const FLAC__StreamDecoder *decoder, const char *message)
{
	if(ctx->invariant)
		return;
	ctx->invariant = true;
	ctx->invariant_offset = decoder_position(ctx, decoder);
	death_offset = ctx->invariant_offset;
	snprintf(ctx->invariant_message, sizeof(ctx->invariant_message), "%s", message);
}

static FLAC__StreamDecoderReadStatus read_callback(
	const FLAC__StreamDecoder *decoder,
	FLAC__byte buffer[],
	size_t *bytes,
	void *client_data)
{
	PropertyContext *ctx = (PropertyContext *)client_data;
	static const size_t chunk_sizes[] = {
		1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 512, 1024, 4096, 8192
	};
	size_t available;
	size_t wanted;
	size_t chunk;

	(void)decoder;
	if(bytes == NULL || buffer == NULL)
		return FLAC__STREAM_DECODER_READ_STATUS_ABORT;

	ctx->last_read_start = ctx->pos;
	death_offset = ctx->last_read_start;
	if(ctx->pos >= ctx->size) {
		*bytes = 0;
		ctx->last_read_end = ctx->pos;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}
	if(*bytes == 0)
		return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;

	available = ctx->size - ctx->pos;
	wanted = *bytes;
	chunk = chunk_sizes[ctx->read_calls++ % (sizeof(chunk_sizes) / sizeof(chunk_sizes[0]))];
	if(chunk > wanted)
		chunk = wanted;
	if(chunk > available)
		chunk = available;
	memcpy(buffer, ctx->data + ctx->pos, chunk);
	ctx->pos += chunk;
	ctx->last_read_end = ctx->pos;
	death_offset = ctx->pos;
	*bytes = chunk;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderSeekStatus seek_callback(
	const FLAC__StreamDecoder *decoder,
	FLAC__uint64 absolute_byte_offset,
	void *client_data)
{
	PropertyContext *ctx = (PropertyContext *)client_data;

	(void)decoder;
	if(absolute_byte_offset > ctx->size)
		return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
	ctx->pos = (size_t)absolute_byte_offset;
	ctx->last_read_start = ctx->pos;
	ctx->last_read_end = ctx->pos;
	death_offset = ctx->pos;
	return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

static FLAC__StreamDecoderTellStatus tell_callback(
	const FLAC__StreamDecoder *decoder,
	FLAC__uint64 *absolute_byte_offset,
	void *client_data)
{
	PropertyContext *ctx = (PropertyContext *)client_data;

	(void)decoder;
	*absolute_byte_offset = ctx->pos;
	return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static FLAC__StreamDecoderLengthStatus length_callback(
	const FLAC__StreamDecoder *decoder,
	FLAC__uint64 *stream_length,
	void *client_data)
{
	PropertyContext *ctx = (PropertyContext *)client_data;

	(void)decoder;
	*stream_length = ctx->size;
	return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

static FLAC__bool eof_callback(const FLAC__StreamDecoder *decoder, void *client_data)
{
	PropertyContext *ctx = (PropertyContext *)client_data;

	(void)decoder;
	return ctx->pos >= ctx->size;
}

static FLAC__bool seektable_references_are_forward(const FLAC__StreamMetadata_SeekTable *table)
{
	uint32_t index;
	FLAC__bool have_previous = false;
	FLAC__uint64 previous_sample = 0;
	FLAC__uint64 previous_offset = 0;

	for(index = 0; index < table->num_points; index++) {
		const FLAC__StreamMetadata_SeekPoint *point = &table->points[index];
		if(point->sample_number == FLAC__STREAM_METADATA_SEEKPOINT_PLACEHOLDER)
			continue;
		if(point->frame_samples == 0)
			return false;
		if(have_previous &&
		   (point->sample_number <= previous_sample ||
		    point->stream_offset <= previous_offset))
			return false;
		have_previous = true;
		previous_sample = point->sample_number;
		previous_offset = point->stream_offset;
	}
	return true;
}

static FLAC__bool cuesheet_references_are_forward(const FLAC__StreamMetadata_CueSheet *cuesheet)
{
	uint32_t index;

	if(cuesheet->num_tracks == 0)
		return false;
	for(index = 0; index < cuesheet->num_tracks; index++) {
		const FLAC__StreamMetadata_CueSheet_Track *track = &cuesheet->tracks[index];
		if(index > 0) {
			const FLAC__StreamMetadata_CueSheet_Track *previous = &cuesheet->tracks[index - 1];
			if(track->offset <= previous->offset || track->number <= previous->number)
				return false;
		}
	}
	return cuesheet->tracks[cuesheet->num_tracks - 1].number == 170;
}

static void check_metadata_semantics(PropertyContext *ctx, const FLAC__StreamMetadata *metadata)
{
	const char *violation = NULL;
	FLAC__StreamMetadata *clone;

	if(metadata->type > 126u || metadata->length > 0xffffffu) {
		ctx->semantic_illegal++;
		return;
	}

	if(metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
		const FLAC__StreamMetadata_StreamInfo *si = &metadata->data.stream_info;
		if(metadata->length != FLAC__STREAM_METADATA_STREAMINFO_LENGTH ||
		   si->min_blocksize == 0 || si->min_blocksize > si->max_blocksize ||
		   si->max_blocksize > FLAC__MAX_BLOCK_SIZE || si->sample_rate == 0 ||
		   si->sample_rate > FLAC__MAX_SAMPLE_RATE ||
		   si->channels == 0 || si->channels > FLAC__MAX_CHANNELS ||
		   si->bits_per_sample < FLAC__MIN_BITS_PER_SAMPLE ||
		   si->bits_per_sample > FLAC__MAX_BITS_PER_SAMPLE)
			ctx->semantic_illegal++;
	}
	else if(metadata->type == FLAC__METADATA_TYPE_APPLICATION) {
		if(metadata->length < 4)
			ctx->semantic_illegal++;
	}
	else if(metadata->type == FLAC__METADATA_TYPE_SEEKTABLE) {
		if(metadata->length % FLAC__STREAM_METADATA_SEEKPOINT_LENGTH != 0 ||
		   !FLAC__metadata_object_seektable_is_legal(metadata) ||
		   !seektable_references_are_forward(&metadata->data.seek_table))
			ctx->semantic_illegal++;
	}
	else if(metadata->type == FLAC__METADATA_TYPE_CUESHEET) {
		if(!FLAC__metadata_object_cuesheet_is_legal(metadata, false, &violation) ||
		   !cuesheet_references_are_forward(&metadata->data.cue_sheet))
			ctx->semantic_illegal++;
	}

	clone = FLAC__metadata_object_clone(metadata);
	if(clone != NULL)
		FLAC__metadata_object_delete(clone);
}

static void metadata_callback(
	const FLAC__StreamDecoder *decoder,
	const FLAC__StreamMetadata *metadata,
	void *client_data)
{
	PropertyContext *ctx = (PropertyContext *)client_data;

	ctx->metadata_count++;
	if(metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
		if(ctx->got_streaminfo)
			ctx->semantic_illegal++;
		else {
			ctx->got_streaminfo = true;
			ctx->streaminfo_channels = metadata->data.stream_info.channels;
			ctx->streaminfo_bps = metadata->data.stream_info.bits_per_sample;
			ctx->streaminfo_rate = metadata->data.stream_info.sample_rate;
			ctx->streaminfo_total_samples = metadata->data.stream_info.total_samples;
		}
	}
	check_metadata_semantics(ctx, metadata);
	ctx->decode_position = decoder_position(ctx, decoder);
}

static void error_callback(
	const FLAC__StreamDecoder *decoder,
	FLAC__StreamDecoderErrorStatus status,
	void *client_data)
{
	PropertyContext *ctx = (PropertyContext *)client_data;
	uint64_t offset = decoder_position(ctx, decoder);

	if(ctx->errors == 0) {
		ctx->first_error_offset = offset;
		snprintf(ctx->first_error, sizeof(ctx->first_error), "%s",
			FLAC__StreamDecoderErrorStatusString[status]);
	}
	ctx->errors++;
	ctx->decode_position = offset;
}

static void write_s32le(FILE *file, uint32_t value, PropertyContext *ctx)
{
	FLAC__byte bytes[4];

	bytes[0] = (FLAC__byte)(value & 0xffu);
	bytes[1] = (FLAC__byte)((value >> 8) & 0xffu);
	bytes[2] = (FLAC__byte)((value >> 16) & 0xffu);
	bytes[3] = (FLAC__byte)((value >> 24) & 0xffu);
	if(fwrite(bytes, 1, sizeof(bytes), file) != sizeof(bytes)) {
		mark_invariant(ctx, NULL, "PCM dump write failed");
		return;
	}
	ctx->dump_bytes += sizeof(bytes);
}

static FLAC__StreamDecoderWriteStatus write_callback(
	const FLAC__StreamDecoder *decoder,
	const FLAC__Frame *frame,
	const FLAC__int32 *const buffer[],
	void *client_data)
{
	PropertyContext *ctx = (PropertyContext *)client_data;
	uint32_t channel;
	uint32_t sample;
	uint32_t bits_per_sample;
	uint32_t shift;
	int64_t min_sample;
	int64_t max_sample;
	char message[192];

	if(frame == NULL || buffer == NULL) {
		mark_invariant(ctx, decoder, "NULL frame or channel buffer");
		return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
	}
	if(frame->header.blocksize == 0 || frame->header.blocksize > FLAC__MAX_BLOCK_SIZE) {
		snprintf(message, sizeof(message), "invalid blocksize %" PRIu32, frame->header.blocksize);
		mark_invariant(ctx, decoder, message);
		return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
	}
	if(frame->header.channels == 0 || frame->header.channels > FLAC__MAX_CHANNELS) {
		snprintf(message, sizeof(message), "invalid channel count %" PRIu32, frame->header.channels);
		mark_invariant(ctx, decoder, message);
		return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
	}
	if((uint32_t)frame->header.channel_assignment > (uint32_t)FLAC__CHANNEL_ASSIGNMENT_MID_SIDE) {
		mark_invariant(ctx, decoder, "invalid channel assignment");
		return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
	}
	if(frame->header.bits_per_sample < FLAC__MIN_BITS_PER_SAMPLE ||
	   frame->header.bits_per_sample > FLAC__MAX_BITS_PER_SAMPLE) {
		snprintf(message, sizeof(message), "invalid bits_per_sample %" PRIu32,
			frame->header.bits_per_sample);
		mark_invariant(ctx, decoder, message);
		return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
	}
	if(ctx->expect_valid && ctx->got_streaminfo &&
	   (frame->header.channels != ctx->streaminfo_channels ||
	    frame->header.bits_per_sample != ctx->streaminfo_bps ||
	    frame->header.sample_rate != ctx->streaminfo_rate)) {
		mark_invariant(ctx, decoder, "frame format differs from STREAMINFO");
		return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
	}

	bits_per_sample = frame->header.bits_per_sample;
	shift = 32u - bits_per_sample;
	min_sample = -((int64_t)1 << (bits_per_sample - 1u));
	max_sample = ((int64_t)1 << (bits_per_sample - 1u)) - 1;

	for(channel = 0; channel < frame->header.channels; channel++) {
		if(buffer[channel] == NULL) {
			snprintf(message, sizeof(message), "NULL sample buffer for channel %" PRIu32, channel);
			mark_invariant(ctx, decoder, message);
			return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
		}
	}
	for(sample = 0; sample < frame->header.blocksize; sample++) {
		for(channel = 0; channel < frame->header.channels; channel++) {
			int64_t value = buffer[channel][sample];
			if(value < min_sample || value > max_sample) {
				snprintf(message, sizeof(message),
					"sample out of %" PRIu32 "-bit range: channel=%" PRIu32
					" sample=%" PRIu32 " value=%" PRId64,
					bits_per_sample, channel, sample, value);
				mark_invariant(ctx, decoder, message);
				return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
			}
			if(ctx->dump != NULL) {
				uint32_t raw = (uint32_t)buffer[channel][sample];
				uint32_t scaled = shift == 0 ? raw : raw << shift;
				write_s32le(ctx->dump, scaled, ctx);
			}
		}
	}

	if(UINT64_MAX - ctx->samples < frame->header.blocksize) {
		mark_invariant(ctx, decoder, "sample counter overflow");
		return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
	}
	ctx->samples += frame->header.blocksize;
	ctx->frames++;
	ctx->decode_position = decoder_position(ctx, decoder);
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void exercise_metadata_chain(PropertyContext *ctx)
{
	FLAC__Metadata_Chain *chain = FLAC__metadata_chain_new();
	FLAC__Metadata_Iterator *iterator = NULL;
	FLAC__StreamMetadata *block;

	if(chain == NULL) {
		mark_invariant(ctx, NULL, "metadata chain allocation failed");
		return;
	}
	if(!FLAC__metadata_chain_read(chain, ctx->path)) {
		ctx->chain_status = FLAC__metadata_chain_status(chain);
		FLAC__metadata_chain_delete(chain);
		return;
	}
	ctx->chain_ok = true;
	ctx->chain_status = FLAC__metadata_chain_status(chain);
	iterator = FLAC__metadata_iterator_new();
	if(iterator == NULL) {
		mark_invariant(ctx, NULL, "metadata iterator allocation failed");
		FLAC__metadata_chain_delete(chain);
		return;
	}
	FLAC__metadata_iterator_init(iterator, chain);
	do {
		block = FLAC__metadata_iterator_get_block(iterator);
		if(block == NULL) {
			mark_invariant(ctx, NULL, "metadata iterator returned NULL block");
			break;
		}
		ctx->chain_blocks++;
		check_metadata_semantics(ctx, block);
	} while(FLAC__metadata_iterator_next(iterator));
	FLAC__metadata_iterator_delete(iterator);
	FLAC__metadata_chain_delete(chain);
}

static int read_file(PropertyContext *ctx)
{
	FILE *file = fopen(ctx->path, "rb");
	long length;

	if(file == NULL) {
		fprintf(stderr, "cannot open %s: %s\n", ctx->path, strerror(errno));
		return 1;
	}
	if(fseek(file, 0, SEEK_END) != 0) {
		fprintf(stderr, "cannot seek %s: %s\n", ctx->path, strerror(errno));
		fclose(file);
		return 1;
	}
	length = ftell(file);
	if(length < 0) {
		fprintf(stderr, "cannot tell %s: %s\n", ctx->path, strerror(errno));
		fclose(file);
		return 1;
	}
	if(fseek(file, 0, SEEK_SET) != 0) {
		fprintf(stderr, "cannot rewind %s: %s\n", ctx->path, strerror(errno));
		fclose(file);
		return 1;
	}
	ctx->size = (size_t)length;
	ctx->data = (FLAC__byte *)malloc(ctx->size == 0 ? 1 : ctx->size);
	if(ctx->data == NULL) {
		fprintf(stderr, "cannot allocate %zu bytes for %s\n", ctx->size, ctx->path);
		fclose(file);
		return 1;
	}
	if(ctx->size > 0 && fread(ctx->data, 1, ctx->size, file) != ctx->size) {
		fprintf(stderr, "short read from %s: %s\n", ctx->path, strerror(errno));
		fclose(file);
		return 1;
	}
	fclose(file);
	return 0;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s [--expect valid|malformed] [--dump FILE.s32le] FILE.flac\n",
		program);
}

int main(int argc, char **argv)
{
	PropertyContext ctx;
	FLAC__StreamDecoder *decoder = NULL;
	FLAC__StreamDecoderInitStatus init_status;
	const char *dump_path = NULL;
	uint64_t reported_offset;
	int argument;
	int io_error;
	int valid_contract_failure = 0;
	int exit_code = 0;

	memset(&ctx, 0, sizeof(ctx));
	ctx.expect_valid = false;
	ctx.end_state = FLAC__STREAM_DECODER_UNINITIALIZED;
	for(argument = 1; argument < argc; argument++) {
		if(strcmp(argv[argument], "--expect") == 0 && argument + 1 < argc) {
			argument++;
			if(strcmp(argv[argument], "valid") == 0)
				ctx.expect_valid = true;
			else if(strcmp(argv[argument], "malformed") == 0)
				ctx.expect_valid = false;
			else {
				usage(argv[0]);
				return 2;
			}
		}
		else if(strcmp(argv[argument], "--dump") == 0 && argument + 1 < argc) {
			dump_path = argv[++argument];
		}
		else if(argv[argument][0] == '-') {
			usage(argv[0]);
			return 2;
		}
		else {
			if(ctx.path != NULL) {
				usage(argv[0]);
				return 2;
			}
			ctx.path = argv[argument];
		}
	}
	if(ctx.path == NULL) {
		usage(argv[0]);
		return 2;
	}

	death_path = ctx.path;
	if(__sanitizer_set_death_callback != NULL)
		__sanitizer_set_death_callback(sanitizer_death_callback);

	io_error = read_file(&ctx);
	if(io_error) {
		free(ctx.data);
		return 2;
	}
	if(dump_path != NULL) {
		ctx.dump = fopen(dump_path, "wb");
		if(ctx.dump == NULL) {
			fprintf(stderr, "cannot open PCM dump %s: %s\n", dump_path, strerror(errno));
			free(ctx.data);
			return 2;
		}
	}

	exercise_metadata_chain(&ctx);

	decoder = FLAC__stream_decoder_new();
	if(decoder == NULL) {
		fprintf(stderr, "cannot allocate stream decoder\n");
		exit_code = 5;
		goto cleanup;
	}
	(void)FLAC__stream_decoder_set_md5_checking(decoder, true);
	(void)FLAC__stream_decoder_set_metadata_respond_all(decoder);
	init_status = FLAC__stream_decoder_init_stream(
		decoder,
		read_callback,
		seek_callback,
		tell_callback,
		length_callback,
		eof_callback,
		write_callback,
		metadata_callback,
		error_callback,
		&ctx);
	if(init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		fprintf(stderr, "decoder init failed: %s\n",
			FLAC__StreamDecoderInitStatusString[init_status]);
		exit_code = 5;
		goto cleanup;
	}

	ctx.decode_ok = FLAC__stream_decoder_process_until_end_of_stream(decoder);
	ctx.end_state = FLAC__stream_decoder_get_state(decoder);
	ctx.decode_position = decoder_position(&ctx, decoder);
	ctx.finish_ok = FLAC__stream_decoder_finish(decoder);

	if(ctx.expect_valid) {
		if(!ctx.decode_ok || !ctx.finish_ok || ctx.errors != 0 ||
		   !ctx.got_streaminfo || ctx.frames == 0 || !ctx.chain_ok ||
		   ctx.semantic_illegal != 0 ||
		   (ctx.streaminfo_total_samples != 0 &&
		    ctx.samples != ctx.streaminfo_total_samples))
			valid_contract_failure = 1;
	}

cleanup:
	if(ctx.dump != NULL) {
		if(fclose(ctx.dump) != 0)
			mark_invariant(&ctx, NULL, "PCM dump close failed");
		ctx.dump = NULL;
	}
	if(decoder != NULL)
		FLAC__stream_decoder_delete(decoder);

	reported_offset = ctx.size;
	if(ctx.invariant)
		reported_offset = ctx.invariant_offset;
	else if(ctx.errors != 0)
		reported_offset = ctx.first_error_offset;
	else if(!ctx.decode_ok)
		reported_offset = ctx.decode_position;

	printf("FLAC_PROPERTY\tpath=%s\texpect=%s\tdecode=%s\tstate=%d\tstate_name=%s"
	       "\toffset=%" PRIu64 "\tframes=%" PRIu64 "\tsamples=%" PRIu64
	       "\tchannels=%" PRIu32 "\tbps=%" PRIu32 "\trate=%" PRIu32
	       "\terrors=%" PRIu64 "\tfirst_error=%s\tmetadata=%" PRIu64
	       "\tchain=%s\tchain_status=%d\tchain_blocks=%" PRIu64
	       "\tsemantic_illegal=%" PRIu64 "\tinvariant=%s\tinvariant_offset=%" PRIu64
	       "\tbytes=%zu\tdump_bytes=%" PRIu64 "\n",
		ctx.path,
		ctx.expect_valid ? "valid" : "malformed",
		ctx.decode_ok ? "true" : "false",
		(int)ctx.end_state,
		FLAC__StreamDecoderStateString[ctx.end_state],
		reported_offset,
		ctx.frames,
		ctx.samples,
		ctx.streaminfo_channels,
		ctx.streaminfo_bps,
		ctx.streaminfo_rate,
		ctx.errors,
		ctx.first_error[0] != '\0' ? ctx.first_error : "-",
		ctx.metadata_count,
		ctx.chain_ok ? "true" : "false",
		(int)ctx.chain_status,
		ctx.chain_blocks,
		ctx.semantic_illegal,
		ctx.invariant ? ctx.invariant_message : "-",
		ctx.invariant_offset,
		ctx.size,
		ctx.dump_bytes);

	if(ctx.invariant)
		exit_code = 5;
	else if(valid_contract_failure && exit_code == 0)
		exit_code = 4;

	free(ctx.data);
	return exit_code;
}
