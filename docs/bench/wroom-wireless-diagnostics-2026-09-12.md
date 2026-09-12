# WROOM wireless diagnostics — 2026-09-12

The updated I2S image was built, flashed and hash-verified on MAC
`70:4b:ca:6d:c2:2c`. SHA-256:
`330ff42e09a43ebb9a1e4942c03c60c8f59aae32558e0c015d2614686ca2f406`.
Artifacts and raw serial capture are local, ignored files under
`build-wroom-wireless/`. Firmware contains private Wi-Fi credentials.

The first association attempt timed out. The second connected at about 35 seconds
and acquired `192.168.8.176` at about 37 seconds. The read-only browser page and
`/logs` returned HTTP 200 over Wi-Fi. An incomplete HTTP request was closed after
approximately 0.31 seconds; subsequent log requests continued to work. The browser
script's independent Node regression verifies aborting a stalled fetch, retrying,
and rendering returned log text without interpreting HTML.

The board reported `LMS connect failed rc=-3 server=192.168.11.132:3483`.
The PC independently could not reach either server port 3483 or 9000. Thus this
session does **not** validate sustained audio playback or resolve the reported
30-second dropout. The page stayed reachable while LMS was unavailable. Server
availability/address must be restored before repeating the standalone audio test.

Earlier diagnostics saw a 272-byte stream ending immediately, then a restarted
stream whose submitted source time advanced much slower than wall time. The
original small RX pool implied a roughly 1365-byte default TCP window. The new
configuration uses an explicit 8192-byte receive window, a 16 KiB RX fragment pool,
more connection slots and enabled send/receive socket timeouts. These changes are
candidate starvation fixes, not a demonstrated cure for the user's dropout.

Both real ESP32 I2S and null-output variants link without PSRAM. The app limits
null-output diagnostic buffers to 128 frames consistently across translation
units. Native ASan/UBSan suite: 22 tests passed. Concurrent log history and output
lifecycle tests also passed TSan. Clang checks: 41 native translation units and
19 Zephyr native-simulator translation units, zero failed checks. Optional SBC
hardware/decoder validation was outside this change.

Still pending: continuous standalone music beyond several track lengths with
browser polling, record received bytes/queued bytes/underruns, AP and LMS recovery,
heap/stack headroom, stereo channel verification, and timing under radio load.
Synchronized playback is unsupported: timestamped pause/resume commands currently
return EAF_UNSUPPORTED and end the LMS session. The fixed −18 dB bench attenuation
remains enabled even at LMS volume 100%; it has not been raised during this update.
