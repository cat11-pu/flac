# Fixed-memory streaming decoder

`FLAC/stream_decoder_fixed.h` provides a pull-style decoder for real-time
audio producers. It has two sides:

- The decode side calls `FLAC__stream_decoder_fixed_pump()`.
- The audio side calls `FLAC__stream_decoder_fixed_read()`.

The decode side may block in the caller's compressed-input callback. The audio
side never blocks and, after construction, never allocates. The two sides may
run on different threads; each side must have only one caller at a time.

## Contract

- All heap allocation, metadata parsing and decoder scratch reservation happens
  in `FLAC__stream_decoder_fixed_new()`.
- `pump()` and `read()` perform no heap allocation.
- PCM is interleaved native-endian `FLAC__int32`.
- The PCM ring contains a power-of-two number of samples and must hold at least
  one configured maximum-size frame.
- `read()` returns `UNDERRUN` when no PCM is available and the stream has not
  ended. This is a normal real-time condition, not an error.
- `pump()` returns `OUTPUT_FULL` when the ring cannot hold another
  configured maximum-size frame. The caller drains and retries.
- Streams exceeding the configured block size, channel count or bit depth fail
  with `FRAME_TOO_LARGE`; the decoder never grows a buffer.
- Native FLAC only. Seeking, Ogg FLAC and chained streams are not exposed.

## Basic integration

```c
FLAC__StreamDecoderFixedConfig config = {
    .max_blocksize = 4608,
    .max_channels = 2,
    .max_bits_per_sample = 24,
    .pcm_ring_bytes = 512u * 1024u,
    .memory_budget_bytes = 4u * 1024u * 1024u
};

FLAC__StreamDecoderFixedStatus status;
FLAC__StreamDecoderFixed *decoder =
    FLAC__stream_decoder_fixed_new(&config, read_callback, client, &status);

/* Decode thread. */
while (running) {
    uint32_t frames;

    status = FLAC__stream_decoder_fixed_pump(decoder, 32, &frames);
    if (status == FLAC__STREAM_DECODER_FIXED_STATUS_OUTPUT_FULL)
        wait_for_consumer();
    else if (status == FLAC__STREAM_DECODER_FIXED_STATUS_END_OF_STREAM)
        break;
    else if (status != FLAC__STREAM_DECODER_FIXED_STATUS_OK)
        handle_error(status);
}

/* Audio callback. */
uint32_t frames = FLAC__stream_decoder_fixed_read(
    decoder, pcm_output, requested_frames, &status);
if (frames == 0 && status == FLAC__STREAM_DECODER_FIXED_STATUS_UNDERRUN)
    render_silence_or_repeat();
```

`read_callback` may block while compressed data is fetched. It must not return
`FLAC__STREAM_DECODER_READ_STATUS_CONTINUE` with `*bytes == 0`; the wrapper
converts that contract violation into `DECODE_ERROR` instead of spinning.

## Boundary cases

| Case | Result |
| --- | --- |
| Read before the first pump | `UNDERRUN` |
| Ring has one maximum-size frame and pump is called again | `OUTPUT_FULL` |
| Read after all PCM is drained and input ended | `END_OF_STREAM` |
| Zero-frame read | returns 0, status `OK` |
| Read with NULL output and non-zero frame count | `INVALID_ARGUMENT` |
| Ring too small or not a power-of-two sample count | constructor rejects |
| Requested memory above 4 MiB | constructor rejects |
| STREAMINFO/frame exceeds configured limits | `FRAME_TOO_LARGE` |
| Read callback aborts or returns zero bytes with `CONTINUE` | `DECODE_ERROR` |
| Corrupt frame/header/CRC | `DECODE_ERROR` |
| 32-bit samples | dedicated `int64` side buffer is reserved up front |

## Memory accounting

The 4 MiB limit covers the decoder wrapper, native libFLAC state, bitreader,
frame/residual/side scratch, entropy tables and the PCM ring. It does not cover
the caller's destination PCM buffer or buffers owned by the compressed-input
callback.

Reported sizes below are for x86-64, `FLAC__int32 == 4`, and include the
internal conservative allocator margin. Use
`FLAC__stream_decoder_fixed_get_memory()` for the exact build/configuration.

| Profile | Core | PCM ring | Total | Headroom to 4 MiB |
| --- | ---: | ---: | ---: | ---: |
| 4,096 block, 2 ch, 16-bit | 148,960 B | 1 MiB | 1,197,648 B | 2,996,656 B |
| 4,608 block, 2 ch, 24-bit | 157,152 B | 512 KiB | 681,552 B | 3,512,752 B |
| 4,608 block, 2 ch, 24-bit | 157,152 B | 1 MiB | 1,205,840 B | 2,988,464 B |
| 8,192 block, 2 ch, 24-bit | 280,032 B | 1 MiB | 1,328,720 B | 2,865,584 B |
| 16,384 block, 2 ch, 24-bit | 542,176 B | 1 MiB | 1,590,864 B | 2,603,440 B |
| 16,384 block, 8 ch, 24-bit | 2,115,136 B | 1 MiB | 3,163,824 B | 1,030,480 B |
| 16,384 block, 8 ch, 32-bit | 2,246,208 B | 1 MiB | 3,294,896 B | 899,408 B |
| 65,535 block, 4 ch, 24-bit | 3,163,616 B | 1 MiB | 4,212,304 B | rejected |

At 48 kHz/2 ch, a 512 KiB int32 ring is 65,536 samples or about 1.37 seconds
of PCM. A 1 MiB ring is about 2.73 seconds.

## Verification performed

- Full libFLAC unit suite passes.
- Allocation-wrapped hot path: 0 calls to `malloc`, `calloc`, `realloc` and
  `free` during a complete 5-second stereo decode.
- One decoder decoded a continuous one-hour stream (317,520,000 interleaved
  samples) in 38,761 pump calls with 0 allocation/free calls.
- 720 create/decode/delete cycles of the same 5-second stream:
  317,520,000 interleaved samples, exactly 3,600.000 seconds. ASan reported no
  leaks and the heap delta remained 0 bytes over all cycles.
- Additional zero-allocation decode runs passed for 2 ch/32-bit and
  8 ch/24-bit streams, exercising the side-subframe and 8-channel paths.
