# EAF Sendspin player — implementation plan

Status: Phase 0 complete. Target: Music Assistant (MA) Sendspin server, ESP32
WROOM first (proof), ESP32-WROVER/PSRAM later (sync). This document is
self-contained so a new session can execute it.

Captured result (2026-09-13): **MA 2.10.3 speaks an older cleartext Sendspin
revision** — plain `ws://`, `client/hello` → `server/hello`, no `client/init`,
no `server/init`, no Noise, no `server/activate`, binary header `>Bq`. See
[the Phase 0 capture](bench/sendspin-capture-2026-09-13.md). The current upstream
spec mandates Noise and an init exchange; treat that as a later (future MA)
revision, not what this server does today.

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
- MA 2.10.3 has **no "Allow legacy clients" setting** in its UI or API. It does
  not need one: it implements the pre-Noise revision and accepts cleartext
  clients unconditionally (captured 2026-09-13).
- Spec: github.com/Sendspin/spec (Open Home Foundation). SDKs: aiosendspin,
  sendspin-cpp ("suitable for embedded", Apache-2.0), sendspin-rs, sendspin-dotnet,
  SendspinKit. ESP32 prior art: RealDeco/SendspinZero (ESP32-S3, 2 MB PSRAM, MIT).
- Wire protocol — **captured MA 2.10.3 revision** (implement this first):
  - WebSocket transport, cleartext throughout. `client/hello` -> `server/hello`.
    No `client/init`, no `server/init`, no Noise, no `server/activate`.
  - Continuous `client/time` -> `server/time` for clock sync (200 ms burst, then
    3 s once synchronized).
  - Stream lifecycle: `stream/start`, binary role frames, `stream/clear`,
    `stream/end`. Player role uses binary message IDs 4-7.
  - Binary Type-4 header is `>Bq`: type byte `0x04` + 8-byte **big-endian**
    signed microseconds timestamp; body is interleaved PCM16 little-endian.
    Chunks are ~25 ms; this revision has **no `send_ahead` field**.
  - `client/state` carries the `player` object (state/volume/mute/static delay,
    lead time, min buffer). Activation is conveyed by `server/hello.active_roles`,
    not by `server/activate`.
  - Audio is 16-bit to players; codecs include PCM and FLAC. PCM is the first
    target; MA transcodes.
- Wire protocol — **current upstream spec** (future MA, do later / Phase 4):
  - `client/init` -> `server/init` -> `noise/handshake` -> encrypted transport,
    then encrypted `server/hello` -> `client/hello` -> `server/activate`.
  - Audio chunks add a `send_ahead` interval; fragmentation uses binary type 1
    with first/last flag bits (Noise plaintext max 65518 bytes).
  - mDNS `_sendspin-server._tcp` (client-initiated) or `_sendspin._tcp`
    (server-initiated).
- Zephyr already links mbedTLS in the EAF build: X25519, ChaCha20-Poly1305,
  AES-GCM, SHA-256, HKDF are available for the later Noise phase.
- Licensing: spec/SDKs Apache-2.0, SendspinZero MIT. Clean-room C or an
  Apache-2.0 port avoids GPL (unlike Snapcast/Squeezelite).

## Phase 0 — Reference capture and spec lock (host only, no EAF changes)

Status: **complete** (2026-09-13). Deliverable:
[docs/bench/sendspin-capture-2026-09-13.md](bench/sendspin-capture-2026-09-13.md).

Executed method (for future re-captures):

1. Confirm MA IP and reachability; MA's Sendspin control API
   (`ws://<ma-ip>:8095/ws`) requires authentication, so playback must be started
   manually in the MA UI. Discovery: `sendspin servers list` or
   `avahi-browse -rt _sendspin-server._tcp`.
2. Install the reference client: `uv tool install sendspin` (v7.5.0; ships
   `aiosendspin` 6.0.5 and provides the `sendspin` CLI with `player`/`daemon`
   subcommands, not the flat flags formerly assumed).
3. Capture the wire with Docker `netshoot` (`--net=host --cap-add=NET_ADMIN
   --cap-add=NET_RAW`) running `tcpdump ... host <ma-ip> and port 8927`; dissect
   in the same container with `tshark`. This avoids host sudo/`tshark` installs.
4. Run the instrumented client `tools/sendspin_probe.py` (wraps `aiosendspin` and
   logs every cleartext JSON message and binary frame header), then start playback
   to the advertised `EAF Phase0 Probe` player.
5. Annotate exact byte layouts and pin MA version + reference-library version.
   (The upstream spec `main` now diverges from MA 2.10.3; pin both.)

Captured: Upgrade headers; `client/hello` (407 B) / `server/hello` (208 B);
`client/state` (164 B); `client/time` (67 B) / `server/time` (130 B);
`group/update` (112 B); `stream/start` (141 B); `stream/end` (85 B);
`client/goodbye` (61 B); binary Type-4 (4417 B = 9 B header + 4408 B PCM16).

Deliverable: annotated capture + frame layouts = Phase 1 test fixtures.

Gaps to capture later: `stream/clear` (seek), `server/command` (volume/mute),
non-player roles, FLAC/`codec_header`, and a spec-current (Noise) server.

## Phase 1 — Transport and core protocol (portable C, host-testable)

Status: **complete** (2026-09-13). Implemented as `apps/sendspin/sendspin_sync.c`,
`sendspin_protocol.c`, `sendspin_ws.c`, `sendspin_client.c` with headers
`include/eaf/eaf_sendspin.h` and `eaf_sendspin_client.h`; host tests
`test_sendspin_{sync,protocol,ws,client}` and the host harness
`platform/native_linux/sendspin_probe.c`. Gate met against MA 2.10.3
(`player@v1` active, clock converged).

- `apps/sendspin/sendspin_ws.c`: WebSocket client over `hal_tcp` — HTTP Upgrade
  handshake, frame encode/decode (FIN/opcode/mask/payload lengths), Ping/Pong,
  Close, fragmentation reassembly.
- `apps/sendspin/sendspin_protocol.c`: bounded JSON encode/decode for the core
  messages (hand-rolled scanner, matching the SlimProto header-key style);
  captured revision: `client/hello` (`player@v1_support`), `server/hello`,
  `client/state`, `client/time`/`server/time`, `group/update`, `stream/start`,
  `stream/clear`, `stream/end`, `client/goodbye`.
- `apps/sendspin/sendspin_client.c`: state machine, connect/reconnect, time-sync
  loop, `stream/start` intake, binary frame dispatch.
- `apps/sendspin/sendspin_sync.c`: portable C port of the Sendspin time-filter
  (Kalman offset+drift) and `compute_client_time`.
- Cleartext handshake only in this phase (the captured MA revision). The
  spec-current `client/init`/`server/init`/Noise path is deferred to Phase 4.
- Tests: independent wire vectors from Phase 0; fragmented frames; malformed
  length/JSON guards; simulated WebSocket server.

Gate: host tests pass; a native harness connects to real MA and reaches an
active `player@v1` session (`server/hello.active_roles` includes `player@v1`)
with the clock converging.

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

## Phase 4 — Encryption and pairing (spec-current servers)

Applies to a future MA/spec revision that implements the `client/init`/Noise
handshake. MA 2.10.3 does **not** — do not block current work on this.

- Noise `KKpsk2` using mbedTLS primitives; persistent Curve25519 identity keypair
  (CSPRNG); Sentinel PSK fallback; `psk_id`/`psk_category` handling.
- Optional pairing (pairing PSK / static or dynamic code) if required.
- Keep the captured cleartext revision as a fallback for older servers.

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
- Protocol revision skew: MA 2.10.3's cleartext revision is behind the upstream
  spec (Noise + init). Design the client so a future spec-current server can be
  added in Phase 4 without reworking the transport.

## Open decisions

Resolved: implement the captured cleartext revision first (3); outbound
WebSocket to `ws://<ma-ip>:8927/sendspin`, mDNS later (4); PCM-only first (5);
runtime-configurable URL/port (7); MA IP `192.168.11.132` confirmed, LMS disabled
while testing (8).

Still open:

1. Clean-room C implementation vs porting Apache-2.0 `sendspin-cpp` (note:
   `sendspin-cpp` tracks the spec-current encrypted revision; the captured
   revision matches `aiosendspin` 6.0.5).
2. WROOM scope: connection/frame/PCM proof only, then WROVER for sync.
3. Extract `platform/esp32_output/` now, or copy the output into the Sendspin app
   and refactor later.

## Immediate next actions

1. Phase 0 and Phase 1 done: [capture](bench/sendspin-capture-2026-09-13.md) and
   the portable client above.
2. Phase 2: feed `stream/start` PCM16 into the existing Q1.31 reservoir/graph and
   reuse the I2S sink on the WROOM boot app (S03).
3. Optional now: play a track to `EAF Native Probe` and confirm binary Type-4
   audio dispatch against real MA.
