# SBC fixture

`tone.sbc` contains four 64-byte SBC frames (512 stereo PCM frames at 48 kHz).
`tone_sbc.inc` is the same bytes for the Zephyr smoke test. This is a generated
440 Hz sine wave, not a recording or third-party media.

Reproduce with ffmpeg, then retain the first four frames:

```sh
ffmpeg -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=0.1' \
  -ac 2 -c:a sbc -b:a 192k -f sbc /tmp/tone-full.sbc
head -c 256 /tmp/tone-full.sbc > tone.sbc
```

Verify frame size before truncating when using a different encoder version.
Tests check structure and decoded behavior, not a bit-identical encoder output.
