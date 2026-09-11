# Linux ALSA output and LMS volume

Install ALSA development files (`alsa-lib-devel` on Fedora/Nobara), then configure:

```sh
cmake -S . -B build -DEAF_ENABLE_ALSA=ON
cmake --build build
./build/eaf_play /path/to/music.wav default
./build/eaf_lms_play 192.168.11.132 300 default
```

Select player `02:ea:f0:00:00:01` in the LMS UI on port 9000. It may appear as
SqueezePlay. The executable connects to SlimProto port 3483. Only one instance
may use this test ID. Stop with Ctrl-C or let the duration expire.

The final argument is an ALSA PCM name: `default`, a name from `aplay -L`, or
`plughw:CARD=...,DEV=0`. Omitting it preserves the timed null sink. ALSA's `null`
PCM is also inaudible but exercises the ALSA adapter. The build detects ALSA
through pkg-config; without development files it builds null output only.
Use `-DEAF_ENABLE_ALSA=OFF` to disable the optional dependency explicitly.

The adapter uses native-endian signed 32-bit interleaved mono/stereo PCM,
nonblocking writes with partial-write handling, and a 50 ms requested ALSA buffer
latency. Write retries are bounded to 250 ms. Underruns reprepare the device and
increment an xrun counter; disconnected/suspended-device errors require restart.
Plugin buffering and resampling may alter actual latency. Setup allocates within
ALSA; this backend does not establish a whole-process heap or hard-real-time bound.

EOF drains inside the final sink commit, before the player's automatic STOP.
Drain has a one-second deadline. Explicit STOP drops queued audio. Pause uses
ALSA's hardware/plugin pause when supported; otherwise it finishes the short
queued tail before acknowledging pause, then prepares on resume. All device
operations stay with the output owner while active. LMS elapsed reports subtract
reported ALSA delay, but plugin delay, underruns and server seek offsets still
limit timing accuracy; no multi-room precision is claimed.

LMS `audg` carries unsigned 16.16 left/right gains. The client converts them to
Q1.31, preserves mute, honors the adjust-disabled unity setting and caps gains
above unity. The Linux audio thread takes one bounded coherent snapshot and
applies the existing volume DSP node at each block boundary. Mono uses the left
gain. The latest gain persists across tracks. There is no hardware mixer change,
ReplayGain support or smoothing ramp yet. Volume acts on newly processed frames;
already queued ALSA audio retains its prior gain.

Validation: ALSA null/file plugin tests verify exact interleaved post-volume
bytes, pause/resume, restart and EOF drain. A 100 ms silent WAV also opened and
drained the system default device successfully. Listening quality and audible
routing were not assessed. The simulated LMS peer checks volume/mute conversion.

API reference: [ALSA PCM documentation](https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html).

The native LMS worker is created before pipeline START and released only after
START succeeds. Failed startup cancels and joins the gated worker. Join/stop
failures retain active state and reject replacement startup instead of resetting
resources whose ownership is unresolved. The CLI reports cleanup failure; generic
partial hardware START/deinit failure recovery remains part of T03.
