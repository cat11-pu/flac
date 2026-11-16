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

#ifndef FLAC__STREAM_DECODER_FIXED_H
#define FLAC__STREAM_DECODER_FIXED_H

#include "export.h"
#include "format.h"
#include "stream_decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/** \file include/FLAC/stream_decoder_fixed.h
 *
 *  \brief
 *  Fixed-memory, pull-style FLAC decoder.
 *
 *  This interface is intended for real-time audio producers.  All heap
 *  allocation and metadata parsing happens in
 *  FLAC__stream_decoder_fixed_new().  After that function returns
 *  successfully, FLAC__stream_decoder_fixed_pump() and
 *  FLAC__stream_decoder_fixed_read() perform no heap allocation.
 *
 *  Decoded PCM is stored in a preallocated SPSC ring.  One thread may call
 *  FLAC__stream_decoder_fixed_pump(), while one other thread may call
 *  FLAC__stream_decoder_fixed_read().  Calls on the same side of the ring
 *  must be serialized by the caller.
 */

/** \defgroup flac_fixed_stream_decoder FLAC/stream_decoder_fixed.h: fixed-memory stream decoder
 *  \ingroup flac_decoder
 *
 *  \{
 */

/** Default and hard upper memory budget for one fixed decoder, in bytes. */
#define FLAC__STREAM_DECODER_FIXED_DEFAULT_MEMORY_BUDGET (4u * 1024u * 1024u)
#define FLAC__STREAM_DECODER_FIXED_MAX_MEMORY_BUDGET (4u * 1024u * 1024u)

/** Return codes for the fixed-memory decoder operations.
 */
typedef enum {
	FLAC__STREAM_DECODER_FIXED_STATUS_OK = 0,
	/**< Operation completed and produced or returned PCM. */

	FLAC__STREAM_DECODER_FIXED_STATUS_UNDERRUN,
	/**< The PCM ring is empty.  Feed the decoder or retry later.  This is
	 *   not an error and is the normal real-time underrun indication. */

	FLAC__STREAM_DECODER_FIXED_STATUS_OUTPUT_FULL,
	/**< The PCM ring cannot hold another configured maximum-size frame.
	 *   Drain PCM with FLAC__stream_decoder_fixed_read() and call
	 *   FLAC__stream_decoder_fixed_pump() again. */

	FLAC__STREAM_DECODER_FIXED_STATUS_END_OF_STREAM,
	/**< The input reached end-of-stream and all decoded PCM was drained. */

	FLAC__STREAM_DECODER_FIXED_STATUS_FRAME_TOO_LARGE,
	/**< A frame exceeded the configured block size, channel count or bit
	 *   depth.  The decoder refuses to grow and enters a sticky error
	 *   state. */

	FLAC__STREAM_DECODER_FIXED_STATUS_DECODE_ERROR,
	/**< The input stream is corrupt or the read callback aborted.  This is
	 *   a sticky error state. */

	FLAC__STREAM_DECODER_FIXED_STATUS_INVALID_ARGUMENT,
	/**< A parameter or configuration is invalid. */

	FLAC__STREAM_DECODER_FIXED_STATUS_OUT_OF_MEMORY
	/**< The requested configuration exceeds the memory budget, or an
	 *   allocation failed while constructing the decoder. */

} FLAC__StreamDecoderFixedStatus;

/** Maps a FLAC__StreamDecoderFixedStatus to a C string.
 *
 *  Using a FLAC__StreamDecoderFixedStatus as the index to this array
 *  will give the string equivalent.  The contents should not be modified.
 */
extern FLAC_API const char * const FLAC__StreamDecoderFixedStatusString[];

/** Configuration for a fixed-memory decoder.
 *
 *  The values are worst-case limits, not hints.  A stream whose STREAMINFO
 *  or frame headers exceed these limits is rejected instead of causing a
 *  reallocation.
 */
typedef struct {
	uint32_t max_blocksize;
	/**< Maximum FLAC frame size in samples per channel.  Must be between
	 *   FLAC__MIN_BLOCK_SIZE and FLAC__MAX_BLOCK_SIZE. */

	uint32_t max_channels;
	/**< Maximum channel count.  Must be between 1 and
	 *   FLAC__MAX_CHANNELS. */

	uint32_t max_bits_per_sample;
	/**< Maximum bit depth.  Must be between FLAC__MIN_BITS_PER_SAMPLE and
	 *   FLAC__MAX_BITS_PER_SAMPLE. */

	uint32_t pcm_ring_bytes;
	/**< Preallocated interleaved PCM ring size in bytes.  Must be a
	 *   multiple of sizeof(FLAC__int32), contain a power-of-two number of
	 *   samples, and be large enough for at least one frame of
	 *   max_blocksize * max_channels samples. */

	size_t memory_budget_bytes;
	/**< Hard ceiling for this decoder including the PCM ring, wrapper and
	 *   libFLAC decode buffers.  Zero selects
	 *   FLAC__STREAM_DECODER_FIXED_DEFAULT_MEMORY_BUDGET.  Values above
	 *   FLAC__STREAM_DECODER_FIXED_MAX_MEMORY_BUDGET are rejected. */

} FLAC__StreamDecoderFixedConfig;

/** Stream properties discovered during construction. */
typedef struct {
	uint32_t sample_rate;
	/**< Sample rate in Hz. */

	uint32_t channels;
	/**< Channel count from STREAMINFO. */

	uint32_t bits_per_sample;
	/**< Bit depth from STREAMINFO. */

	FLAC__uint64 total_samples;
	/**< Total samples per channel, or zero if unknown. */

} FLAC__StreamDecoderFixedInfo;

/** Memory accounting for a constructed decoder.
 *
 *  These values are conservative requested allocation sizes.  Allocator bookkeeping
 *  overhead is deliberately covered by an internal safety margin and is
 *  not included in FLAC__StreamDecoderFixedMemory::total_bytes.
 */
typedef struct {
	size_t budget_bytes;
	/**< Configured hard ceiling. */

	size_t core_bytes;
	/**< libFLAC decoder, bitreader, frame and entropy scratch buffers. */

	size_t ring_bytes;
	/**< Preallocated PCM ring. */

	size_t object_bytes;
	/**< Fixed decoder wrapper object. */

	size_t total_bytes;
	/**< core_bytes + ring_bytes + object_bytes. */

} FLAC__StreamDecoderFixedMemory;

/** Opaque fixed-memory decoder type. */
typedef struct FLAC__StreamDecoderFixed FLAC__StreamDecoderFixed;

/** Create and fully prepare a fixed-memory decoder.
 *
 *  This function calls \a read_callback while parsing metadata.  The
 *  callback follows FLAC__StreamDecoderReadCallback semantics.  It may
 *  block while waiting for more compressed input, but it must not return
 *  FLAC__STREAM_DECODER_READ_STATUS_CONTINUE with \c *bytes == 0; that
 *  would otherwise deadlock the underlying bitreader and is converted to
 *  a decode error.
 *
 *  On success, all allocation, metadata parsing and frame-buffer
 *  reservation are complete.  No later call allocates memory.
 *
 *  \param config       Configuration; copied into the decoder.
 *  \param read_callback Callback supplying native FLAC bytes.
 *  \param client_data  Passed to \a read_callback.
 *  \param status       Optional detailed status.  On failure this receives
 *                      a status other than FLAC__STREAM_DECODER_FIXED_STATUS_OK.
 *  \return A decoder, or NULL on failure.
 */
FLAC_API FLAC__StreamDecoderFixed *FLAC__stream_decoder_fixed_new(
	const FLAC__StreamDecoderFixedConfig *config,
	FLAC__StreamDecoderReadCallback read_callback,
	void *client_data,
	FLAC__StreamDecoderFixedStatus *status);

/** Destroy a fixed-memory decoder and release all allocations. */
FLAC_API void FLAC__stream_decoder_fixed_delete(FLAC__StreamDecoderFixed *decoder);

/** Decode up to \a max_frames frames into the PCM ring.
 *
 *  This is the producer-side operation.  It may call the read callback and
 *  may therefore block according to the callback's behavior.  It performs
 *  no heap allocation.
 *
 *  \param decoder       Initialized decoder.
 *  \param max_frames    Maximum frames to decode in this call.  Use
 *                       UINT32_MAX to decode until the ring is full or the
 *                       stream ends.
 *  \param frames_decoded Optional number of frames actually produced.
 *  \return Status of the operation.
 */
FLAC_API FLAC__StreamDecoderFixedStatus FLAC__stream_decoder_fixed_pump(
	FLAC__StreamDecoderFixed *decoder,
	uint32_t max_frames,
	uint32_t *frames_decoded);

/** Copy up to \a frames frames from the PCM ring to \a output.
 *
 *  This is the consumer-side operation and is suitable for a high-priority
 *  audio callback.  It performs no heap allocation and never blocks.
 *  Samples are interleaved native-endian signed 32-bit values.  For lower
 *  bit depths the values are sign-extended and contain no scaling.
 *
 *  \param decoder Initialized decoder.
 *  \param output  Destination buffer, or NULL when \a frames is zero.
 *  \param frames  Maximum frames to copy.
 *  \param status  Optional status.  If zero frames are available this is
 *                 either UNDERRUN or END_OF_STREAM, never OK.
 *  \return Number of frames copied.
 */
FLAC_API uint32_t FLAC__stream_decoder_fixed_read(
	FLAC__StreamDecoderFixed *decoder,
	FLAC__int32 *output,
	uint32_t frames,
	FLAC__StreamDecoderFixedStatus *status);

/** Return the number of complete frames currently available to read. */
FLAC_API uint32_t FLAC__stream_decoder_fixed_available_frames(
	const FLAC__StreamDecoderFixed *decoder);

/** Retrieve stream properties discovered during construction.
 *
 *  \return \c false for a NULL decoder or info pointer.
 */
FLAC_API FLAC__bool FLAC__stream_decoder_fixed_get_info(
	const FLAC__StreamDecoderFixed *decoder,
	FLAC__StreamDecoderFixedInfo *info);

/** Retrieve fixed memory accounting.
 *
 *  \return \c false for a NULL decoder or memory pointer.
 */
FLAC_API FLAC__bool FLAC__stream_decoder_fixed_get_memory(
	const FLAC__StreamDecoderFixed *decoder,
	FLAC__StreamDecoderFixedMemory *memory);

/** \} */

#ifdef __cplusplus
}
#endif

#endif
