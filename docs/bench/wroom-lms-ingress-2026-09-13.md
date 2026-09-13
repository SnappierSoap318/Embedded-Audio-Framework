# WROOM LMS intake: RX window and ingress ring — 2026-09-13

Result: **real-time 44.1 kHz/16-bit stereo intake on the physical WROOM**, with a
large drop in output underruns. Audible correctness was not independently
verified in this session; the speaker setup was remote. The evidence below is
telemetry only and does not claim sample-accurate or amplifier-quality output.

## Baseline

Before this change the WROOM received HTTP PCM at ~80–140 KB/s (needed
176,400 B/s), `min_frames` frequently reached 0, and one track accumulated
198 underruns. The 8 KiB TCP receive window held only ~46 ms of 44.1 kHz/16-bit
stereo, and the client stopped reading the socket whenever the output reservoir
was full, closing the server's TCP window. See
[streaming audit](../streaming-audit-2026-09-13.md) S01/S05.

## Change

1. `boards/esp32_devkitc_esp32_procpu.conf`: raise
   `CONFIG_NET_TCP_MAX_RECV_WINDOW_SIZE` to 16384 with a matching RX pbuf pool
   (`NET_BUF_RX_COUNT=96`, `NET_PKT_RX_COUNT=24`).
2. Portable `apps/lms/lms_client.c`: a raw source-PCM ingress ring
   (`EAF_LMS_INGRESS_BYTES`, 16 KiB on this board) between the socket and the
   output callback. The transport keeps draining the socket into the ring while
   the output reservoir is backpressured; decode sources from the ring.

## Telemetry (physical board, 44.1 kHz stereo, browser polling)

```text
[23747 ms] Output request: 44100 Hz channels=2
[35350 ms] RX=176332 B/s total=1972694 again=1059 backpressure=5924 budget=1069
[35351 ms] Output queued=32768 min_frames=2688 played_ms=10988 underruns=0 failed=0
[50372 ms] RX=177174 B/s total=4622806 again=1813 backpressure=13974 budget=2452
[50373 ms] Output queued=32768 min_frames=2688 played_ms=26009 underruns=0 failed=0
[65374 ms] RX=176443 B/s total=7268870 again=2800 backpressure=21970 budget=3822
[65374 ms] Output queued=32768 min_frames=696 played_ms=41012 underruns=0 failed=0
[75375 ms] RX=174308 B/s total=9022478 again=3680 backpressure=27284 budget=4742
[75375 ms] Output queued=31744 min_frames=0 played_ms=50973 underruns=1 failed=0
[85380 ms] RX=175276 B/s total=10785814 again=4227 backpressure=32663 budget=5664
[85380 ms] Output queued=28672 min_frames=0 played_ms=60978 underruns=1 failed=0
```

- `RX` sustained 174–177 KB/s, i.e. real-time for the required 176,400 B/s.
- `played_ms` advanced exactly +5000 ms per 5 s wall interval.
- `underruns` stayed 0 for ~50 s, reached 1 at ~75 s and then held at 1.
- `failed=0`; the I2S path reported no driver error.
- Rising `backpressure` is expected: the reservoir is full and the transport now
  keeps reading into the ring rather than stalling.

## Caveats

- One underrun occurred mid-track; the reservoir still has limited depth and the
  underrun-tail policy remains destructive (R4).
- The fixed −18 dB bench attenuation is still active, so this does not validate
  maximum level or amplifier headroom.
- Sustained multiple-track playback, controlled network gaps, AP/server restart
  and long-run memory stability remain to be tested.
- Clang/ASan results validate software behavior, not speaker presentation.
