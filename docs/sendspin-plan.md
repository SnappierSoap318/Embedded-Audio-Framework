# EAF Sendspin player — implementation plan

Status: proposed. Target: Music Assistant (MA) Sendspin server, ESP32 WROOM first
(proof), ESP32-WROVER/PSRAM later (sync). This document is self-contained so a new
session can execute it.

## Goal

Add a portable Sendspin `player@v1` client to EAF: connect to MA's Sendspin
server, negotiate PCM, decode binary audio frames to the existing Q1.31
reservoir/graph/I2S, and schedule playback against the server clock. Encryption,
extra roles, and mDNS follow.

Non-goals for the first milestone: Noise encryption, FLAC, controller/metadata/
artwork/visualizer roles, mDNS, multi-protocol source arbitration.

## Locked context (verified)

- MA 2.10.3 ships Sendspin built-in and always on. Hardware endpoint:
  `ws://<ma-ip>:8927/sendspin` (plain `ws://`; the path `/sendspin` on port 8095 is
  reserved for MA's web player and rejects other clients).
- MA setting "Allow legacy clients" is on by default and accepts devices that
  connect without encryption. Use this to stage the audio path before Noise.
- Spec: github.com/Sendspin/spec (Open Home Foundation). SDKs: aiosendspin,
  sendspin-cpp ("suitable for embedded", Apache-2.0), sendspin-rs, sendspin-dotnet,
  SendspinKit. ESP32 prior art: RealDeco/SendspinZero (ESP32-S3, 2 MB PSRAM, MIT).
- Wire protocol (high level):
  - WebSocket transport. Cleartext JSON: `client/init` -> `server/init` ->
    (`noise/handshake`) -> transport mode. Then encrypted JSON: `server/hello` ->
    `client/hello` -> `server/activate`.
  - Continuous `client/time` -> `server/time` for clock sync.
  - Stream lifecycle: `stream/start`, binary role frames, `stream/clear`,
    `stream/end`. Player role uses binary message IDs 4-7.
  - `client/state` carries `available` and role objects. A player MUST NOT report
    `available: true` until its time filter has converged.
  - Audio chunks carry a server timestamp and a `send_ahead` interval; the client
    maps them to its local clock via the time-filter (a 2-D Kalman filter tracking
    offset and drift) and schedules playback.
  - Fragmentation: Noise plaintext max 65518 bytes; larger messages split using
    binary type 1 with first/last flag bits.
  - Audio is 16-bit to Sendspin players; codecs include PCM and FLAC. PCM is the
    first target; MA transcodes.
- Zephyr already links mbedTLS in the EAF build: X25519, ChaCha20-Poly1305,
  AES-GCM, SHA-256, HKDF are available for the later Noise phase.
- Licensing: spec/SDKs Apache-2.0, SendspinZero MIT. Clean-room C or an
  Apache-2.0 port avoids GPL (unlike Snapcast/Squeezelite).

## Phase 0 — Reference capture and spec lock (host only, no EAF changes)

Goal: capture a real MA session and turn it into Phase 1 fixtures. Do not guess
the handshake or frame layouts.

1. On MA: confirm the Sendspin provider is enabled (built-in), "Allow legacy
   clients" = on, and note the MA IP. Ensure the board and MA share the LAN.
2. Install the reference client on the host: `uv tool install sendspin`
   (or `pip install sendspin`).
3. Connect to MA, force PCM, enable DEBUG logging:
   `sendspin --url ws://<ma-ip>:8927/sendspin --audio-format pcm:44100:16:2 --log-level DEBUG`
4. Capture the wire concurrently (legacy/unencrypted client => cleartext):
   `sudo tcpdump -i <iface> -s0 -w /tmp/sendspin.pcap host <ma-ip> and port 8927`
   (or use `websocat`/tshark to pretty-print frames).
5. Extract and annotate, with exact byte layouts:
   - WebSocket Upgrade request/response headers (Sec-WebSocket-Key/Accept, etc.).
   - `client/init`: `client_id` form, `version`, `suite`; whether `noise/handshake`
     is present or skipped in the unencrypted path.
   - `server/hello`, `client/hello` (`supported_roles`, and the complete
     `player@v1_support` object: formats, buffer capacity, and any required fields),
     `server/activate` (`activities`, `active_roles`).
   - `client/time` cadence and the exact `server/time` fields.
   - `stream/start` payload (codec, sample_rate, bit_depth, channels, group info).
   - Binary type-4 header layout (server timestamp, `send_ahead`, payload framing),
     chunk sizes, and cadence; note the 16-bit PCM byte order.
   - `client/state` player object, `group/update`, `stream/end`, `client/goodbye`.
6. Save `docs/bench/sendspin-capture-<date>.md` with the annotated trace (raw pcap
   stays ignored). Pin the spec commit and MA version in the doc.

Deliverable: annotated capture + frame layouts = Phase 1 test fixtures.

## Phase 1 — Transport and core protocol (portable C, host-testable)

- `apps/sendspin/sendspin_ws.c`: WebSocket client over `hal_tcp` — HTTP Upgrade
  handshake, frame encode/decode (FIN/opcode/mask/payload lengths), Ping/Pong,
  Close, fragmentation reassembly.
- `apps/sendspin/sendspin_protocol.c`: bounded JSON encode/decode for the core
  messages (hand-rolled scanner, matching the SlimProto header-key style);
  `client/init`, `server/init`, `server/hello`, `client/hello` (player@v1 +
  support object), `server/activate`, `client/state`.
- `apps/sendspin/sendspin_client.c`: state machine, connect/reconnect, time-sync
  loop, `stream/start` intake, binary frame dispatch.
- `apps/sendspin/sendspin_sync.c`: portable C port of the Sendspin time-filter
  (Kalman offset+drift) and `compute_client_time`.
- Legacy-unencrypted handshake only in this phase.
- Tests: independent wire vectors from Phase 0; fragmented frames; malformed
  length/JSON guards; simulated WebSocket server.

Gate: host tests pass; a native harness connects to real MA and reaches
`server/activate` with `player@v1` active and the clock converging.

## Phase 2 — Player audio on WROOM (proof)

- `stream/start` -> PCM16 -> Q1.31 -> existing reservoir/graph/I2S.
- Report a small buffer capacity; schedule frames using `compute_client_time`;
  implement hard sync (drop/refill) only.
- WROOM constraint: ~19 KB DRAM free. Expect a ~50-100 ms buffer. This proves
  transport/framing/format, not sync quality.
- Reuse the I2S sink and reservoir; keep the audio owner model (worker created
  before START, gated).

Gate: audible correct PCM from MA on WROOM; telemetry shows real-time intake and
no format errors.

## Phase 3 — WROVER/PSRAM and sync quality

- PSRAM jitter buffer (~1 s); report real buffer capacity.
- Soft sync via drift correction (frame drop/duplicate) using the time filter.
- Measure inter-device phase vs MA web player and the Python client; tune.
- Record phase error, underruns, and long-play stability.

Gate: no audible echo across two players over a multi-minute run; measured phase
error documented.

## Phase 4 — Encryption and pairing

- Noise `KKpsk2` using mbedTLS primitives; persistent Curve25519 identity keypair
  (CSPRNG); Sentinel PSK fallback; `psk_id`/`psk_category` handling.
- Optional pairing (pairing PSK / static or dynamic code) if required.
- Legacy-unencrypted remains bench-only.

Gate: connects to MA as a paired or unpaired encrypted client; legacy mode still
works.

## Phase 5 — Roles and discovery

- `controller`, `metadata`, `artwork`, `visualizer` roles as needed.
- mDNS discovery (`_sendspin-server._tcp` for client-initiated, or advertise
  `_sendspin._tcp` for server-initiated).
- FLAC codec via a bounded decoder.
- Coexist with SlimProto/BT through the T08 source coordinator.

## Architecture and repo touch points

- New portable sources under `apps/sendspin/`; public headers
  `include/eaf/eaf_sendspin.h`, `eaf_sendspin_client.h`.
- HAL: WebSocket layer over `hal_tcp`; add `TCP_NODELAY`.
- New app `platform/esp32_sendspin/` (CMakeLists, prj.conf, boards/*, src/main.c).
- Output reuse: the board output owner currently lives in `platform/esp32_lms` and
  exposes `eaf_lms_callbacks_t`. Decision needed: extract a protocol-agnostic
  `platform/esp32_output/` (reservoir, graph, sink, volume, pause, worker,
  snapshot) with a generic producer interface, or stand up
  `platform/esp32_sendspin` with a copied output and refactor later.
- Build wiring: `zephyr/Kconfig` + `zephyr/CMakeLists.txt`; app Kconfig for the
  Sendspin server URL/port (make this runtime-configurable to avoid reflashes).
- Tests: `tests/test_sendspin_protocol.c`, `test_sendspin_ws.c`,
  `test_sendspin_sync.c`, and a simulated server, following existing patterns.

## Verification strategy

- Host: ASan/UBSan suite, architecture guard, independent vectors, simulated
  server, then native harness against real MA (null then ALSA).
- Board: WROOM connect/frame/format proof; WROVER sync and long-play.
- Record MA provider logs, EAF UART and log history (`RX B/s`, underruns, queue
  min, worker flags, socket diagnostics), and logic-analyzer I2S where useful.

## Risks

- Sendspin is a Technical Preview: pin MA 2.10.3 and the spec commit; expect churn.
- WebSocket, a new clock model, and later Noise are all new to EAF. mbedTLS lowers
  the crypto risk.
- WROOM cannot host a meaningful jitter buffer; keep sync work on the WROVER.
- MA "Allow legacy clients" is a temporary compatibility option; do not depend on
  it for production.

## Open decisions (resolve before execution)

1. Clean-room C implementation vs porting Apache-2.0 `sendspin-cpp`.
2. WROOM scope: connection/frame/PCM proof only, then WROVER for sync (recommended).
3. Legacy-unencrypted first, then Noise via mbedTLS (recommended).
4. Outbound WebSocket client to `ws://<ma-ip>:8927/sendspin` first, mDNS later
   (recommended).
5. PCM-only first, FLAC later (recommended).
6. Extract `platform/esp32_output/` now, or copy the output into the Sendspin app
   and refactor later.
7. Make the Sendspin server URL/port runtime-configurable from the start
   (recommended).
8. Confirm the MA IP; keep LMS disabled while testing Sendspin.

## Immediate next actions (new session)

1. Execute Phase 0 and commit the capture doc (`docs/bench/sendspin-capture-<date>.md`).
2. Implement Phase 1 against the captured bytes; run host tests.
3. Connect the native harness to MA; confirm `player@v1` activation and clock
   convergence.
