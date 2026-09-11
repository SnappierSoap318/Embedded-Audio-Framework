# Native playback

`eaf_play file.wav` connects a Linux regular-file reader to a portable WAV parser,
a background source task, the Q1.31 reservoir, master volume, and the timed null
sink. The executable is a headless playback/integration harness; sound-card
output is the next integration. Static wiring lives in
[`play_file.c`](../platform/native_linux/play_file.c).

The parser supports classic little-endian RIFF/WAVE PCM with 16, 24 or 32 bits
and one or two channels. Mono uses FRONT_CENTER and stereo uses FRONT_LEFT |
FRONT_RIGHT. It checks RIFF/chunk boundaries, block alignment, byte rate and
odd-byte padding, skips unknown chunks, and rejects duplicate fmt/data chunks.
Float, extensible WAVE, compressed WAVE, RF64 and RIFX return UNSUPPORTED.
The layout follows Microsoft's [RIFF description](https://learn.microsoft.com/en-us/windows/win32/xaudio2/resource-interchange-file-format--riff-)
and [original multimedia specification](https://www.mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/Docs/riffmci.pdf).
Strict padding checks may reject malformed files produced by permissive tools.

## Interfaces and lifetime

1. Zero-initialize contexts. Open a regular file with `hal_file_open`, then bind
   its reader using `eaf_wav_open`. Alternatively supply any exact `read_at`
   callback and size to the parser. Readers report short/truncated reads as I/O
   errors. POSIX file calls are confined to `hal/linux/hal_file_linux.c`.
2. Initialize and configure the graph using the source format. Initialize
   `eaf_player_t` with that graph, source and an optional graph-owned volume
   context. The player requires sole ownership of the reservoir's producer and
   wake callback. Startup watermark and block size must not exceed the producer
   high watermark, and `0 < backpressure_low < backpressure_high <= capacity`.
3. Call `eaf_player_start` on the owner thread. It creates a producer before
   START, gates it until the reservoir reset completes, and wakes it afterward.
4. Call `eaf_player_step` on that same owner thread for each output block. A
   successful final commit returns `EAF_EOF` and stops/joins the player. A source
   error stops playback and returns the source error. No silence loop conceals
   a failed reader.
5. Call `eaf_player_deinit`, then graph deinit and reader close. Stop any control
   producer before destroying or reinitializing the player. Graph/source/format
   metadata must remain stable throughout the player's active lifetime.

For another format, stop and deinitialize the player, open the new source,
reinitialize the reservoir with adequately sized backing storage, configure the
stopped graph, and bind a new player. Gapless format changes are not implemented.

## Control ownership

One control thread may submit `eaf_command_t` through `eaf_player_command`:

- STOP: stop output, signal shutdown, wake and join the producer. Step returns
  EAF_EOF for this intentional termination as well as natural EOF.
- SEEK: absolute source-frame index, including the end position. The owner
  stops output, joins the producer, seeks, resets queued audio, and restarts if
  previously running. A stopped seek changes position without starting playback.
- GAIN: nonnegative Q1.31 master gain for all channels of the bound volume node.
  INT32_MAX means unity. The owner applies it at a block boundary. There is no
  gain ramp yet; abrupt changes can click.

Queue capacity is 16 commands. Full/invalid submissions return false; callers
must retry or report rejection. Commands are FIFO, with at most one applied per
step, so STOP may follow earlier queued commands. The queue retains pending
commands across stop/start. Direct owner stop/seek APIs are available when a
queue is unnecessary. Concurrent lifecycle calls are prohibited.

Stop discards queued PCM; starting afterward reads from the decoder's current
(potentially prefetched) position. Explicitly seek to select a restart position.
Seek and stop wait for an in-flight regular-file read to finish after output is
stopped. They do not promise bounded disk-I/O cancellation latency. Producer
backpressure waits are signaled and also time out; they cannot prevent shutdown.

## EOF and memory bounds

Only the producer calls `eaf_reservoir_finish`, after all its writes succeed.
No further writes are accepted until reset. The consumer acquires EOF before
reading the write cursor, preserving visibility of the final samples. EOF
bypasses prebuffering, including for short and empty tracks. The last output
block stays fixed-size with zeros after its source frames, and carries EOS.
Further graph pulls return EAF_EOF without acquiring or committing another block.
`reservoir.frames_read` counts source frames, excluding padding and silence;
read this consumer-owned counter after stopping or on the owner thread.

This drains source PCM, not arbitrary IIR tails or a physical sink's hardware
queue. The null sink synchronously accounts for each committed block before
returning. A future ALSA sink needs an explicit output-drain contract before
STOP; truncating DSP tails and gapless final blocks also remain future work.

The WAV decoder uses a fixed 2,048-byte input scratch buffer. The player uses a
fixed 4,096-byte Q1.31 decode buffer (256 frames, up to four channels), plus its
16-command queue and caller-provided reservoir. Linux thread/semaphore resources
are allocated before START and released after STOP. Decode/read/pull and queue
operations contain no heap allocation. Storage reads occur only on the producer;
the audio owner never reads files during regular block processing.
