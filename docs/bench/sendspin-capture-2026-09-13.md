# Sendspin Phase 0 capture — Music Assistant 2.10.3 — 2026-09-13

> Identifiers, server names and LAN addresses are placeholders. The original
> device MAC, Sendspin server identity and network addresses were redacted
> before publication; message shapes and timestamps are otherwise as captured.

Result: **Music Assistant 2.10.3 speaks an older, cleartext Sendspin revision**,
not the encrypted revision described in the current upstream spec. The handshake
is a single `client/hello` → `server/hello` exchange over a plain `ws://`
WebSocket with **no `client/init`, no `server/init`, no Noise handshake, and no
Noise transport**. Audio arrives as cleartext binary Type-4 frames. This confirms
the "legacy" path assumed by the plan and means Phase 1 does not need any crypto
to reach activation and play PCM against this server.

> The upstream spec (`github.com/Sendspin/spec`, `main` =
> `8fc2f8f8d8aa324cf385bd3332a284fd3a75520c`, 2026-09-12) now mandates a
> `client/init`/`server/init`/`noise/handshake` sequence and a fragmentation
> envelope. MA 2.10.3 does **not** implement that revision. The effective
> reference for this server is the `aiosendspin` 6.0.5 client library. Plan for
> both: implement the captured revision first; keep Noise for a later MA/spec
> revision (Phase 4).

## Pins

- **Server:** Music Assistant `2.10.3`, schema `65`
  (`server_version`/`schema_version` from `ws://192.168.11.132:8095/ws`).
  `server_id` (Sendspin identity): `example-server-id`.
  Sendspin endpoint: `ws://192.168.11.132:8927/sendspin`; advertised by mDNS
  `_sendspin-server._tcp` with TXT `path=/sendspin`, `name=Music Assistant (example)`.
- **Reference client:** `sendspin` CLI `7.5.0` / `aiosendspin` `6.0.5`
  (Apache-2.0, `Sendspin-Protocol/sendspin`). The protocol library is the
  cleartext implementation; it has no Noise dependency.
- **Capture host:** `192.168.8.88`, interface `enp8s0`, Linux.
- **Upstream spec (divergent):** `8fc2f8f8d8aa324cf385bd3332a284fd3a75520c`.

Raw pcaps are not committed (kept under `/tmp/opencode/`): `sendspin-phase0.pcap`
(handshake + 40 s clock sync, 67 packets) and `sendspin-audio.pcap` (full audio
session, 4.8 MB).

## Method

- Wire capture: `nicolaka/netshoot` (Docker, `--net=host --cap-add=NET_ADMIN
  --cap-add=NET_RAW`) running
  `tcpdump -i enp8s0 -s0 -U -w ... host 192.168.11.132 and port 8927`. No host
  sudo and no `tshark` install required; `tshark` ran in the same container.
- Application capture: `tools/sendspin_probe.py`, an instrumented
  `aiosendspin` player client (logs every cleartext JSON message and every binary
  frame header). Player advertised as `EAF Phase0 Probe` with
  `supported_roles=["player@v1"]` and the PCM `player@v1_support` object below.
- Playback was started manually in MA to the probe player (MA's control API at
  `ws://<host>:8095/ws` requires authentication).

## Transport

`GET /sendspin` WebSocket upgrade (exact request/response captured):

```http
GET /sendspin HTTP/1.1
Host: 192.168.11.132:8927
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Version: 13
Sec-WebSocket-Key: BEfr8jM2J16+t47N8tiNHg==
Accept: */*
Accept-Encoding: gzip, deflate
User-Agent: Python/3.12 aiohttp/3.14.3
```

```http
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: upgrade
Sec-WebSocket-Accept: uljxfk0L+8zoXDhy7lgkL3Hd0Eg=
Date: Sun, 13 Sep 2026 11:23:50 GMT
Server: Python/3.14 aiohttp/3.14.3
```

WebSocket framing is standard RFC 6455: client frames are masked, server frames
are not, all application frames are text (`opcode 0x1`) until audio binary frames
(`opcode 0x2`). Payloads over 125 bytes use the 16-bit extended length. Examples:

```text
client/hello  : 81 fe 01 97 <4 mask bytes> ...   FIN+text, MASK=1, len=0x0197=407
server/hello  : 81 7e 00 d0 7b 22 70 61 79 ...   FIN+text, MASK=0, len=0x00d0=208
```

## Message transcript (cleartext, byte lengths as sent)

### client → server: `client/hello` (407 B, first frame after Upgrade)

```json
{"payload":{"client_id":"eaf-phase0-probe","name":"EAF Phase0 Probe","version":1,"supported_roles":["player@v1"],"device_info":{"product_name":"EAF Phase0 Probe","manufacturer":"EAF","software_version":"phase0"},"player@v1_support":{"supported_formats":[{"codec":"pcm","channels":2,"sample_rate":44100,"bit_depth":16}],"buffer_capacity":262144,"supported_commands":["volume","mute"]}},"type":"client/hello"}
```

Notes: `version` is an integer `1`. The role-support key is the versioned role
name `player@v1_support` (not `player_support`). `supported_formats` is in
priority order; `buffer_capacity` is bytes of pending compressed audio;
`supported_commands` is the subset of `volume`/`mute` the client handles.

### server → client: `server/hello` (208 B)

```json
{"payload":{"server_id":"example-server-id","name":"Music Assistant (example)","version":1,"connection_reason":"discovery","active_roles":["player@v1"]},"type":"server/hello"}
```

### client → server: `client/state` (164 B, after `server/hello`)

```json
{"payload":{"player":{"state":"synchronized","volume":100,"muted":false,"static_delay_ms":0,"required_lead_time_ms":250,"min_buffer_ms":250}},"type":"client/state"}
```

### client → server: `client/time` (67 B) / server → client: `server/time` (130 B)

```json
{"payload":{"client_transmitted":73611376768},"type":"client/time"}
{"payload":{"client_transmitted":73611376768,"server_received":61896583899,"server_transmitted":61896583986},"type":"server/time"}
```

All timestamps are microseconds. The client clock is a monotonic uptime clock
(≈73.6e9 µs here) and the server clock a different epoch (≈61.9e9 µs), so the raw
offset is large (≈ −11,714,794,500 µs) by design.

### server → client: `group/update` (112 B)

```json
{"payload":{"playback_state":"stopped","group_id":"4c3ab809-cb14-4797-a686-4c9af00bff05"},"type":"group/update"}
{"payload":{"playback_state":"playing","group_id":"4c3ab809-cb14-4797-a686-4c9af00bff05"},"type":"group/update"}
```

### server → client: `stream/start` (141 B)

```json
{"payload":{"server_transmitted":61991839289,"player":{"codec":"pcm","sample_rate":44100,"channels":2,"bit_depth":16}},"type":"stream/start"}
```

`server_transmitted` is present on the wire but is not modelled by the
`aiosendspin` message classes (their callback re-serialises without it). It is the
reference point for `required_lead_time_ms`.

### server → client: `stream/end` (85 B)

```json
{"payload":{"server_transmitted":62004590553,"roles":["player"]},"type":"stream/end"}
```

### client → server: `client/goodbye` (61 B)

```json
{"payload":{"reason":"user_request"},"type":"client/goodbye"}
```

## Binary Type-4 audio frame

Every audio frame in the capture was exactly **4417 bytes** (1043/1043 frames):
a 9-byte header followed by a 4408-byte PCM body.

```text
offset  size  field
0       1     message type = 0x04 (AUDIO_CHUNK; IDs 4-7 are the player block)
1       8     server timestamp, signed 64-bit BIG-ENDIAN, microseconds
9       4408  interleaved PCM16 LE payload (1102 stereo frames)
```

Observed header (first audio frame):

```text
04 00 00 00 0e 6f 03 fe 2f
|  \______________________/
|     int64 BE = 61992074799 us
+-- type = 4
```

Body interpretation:

- 4408 bytes = 1102 stereo frames (4 bytes/frame) = 2204 samples per channel.
- **16-bit signed little-endian**, interleaved L,R (the reference server
  transcodes to PCM16 for players; native depth is 16 here).
- 1102 frames at 44100 Hz = **24.99 ms** per chunk.

Cadence and delivery:

- Consecutive timestamps differ by **24988–24989 µs** (one chunk ≈ 25 ms).
- Chunks arrive in small bursts but are paced at real time; the capture saw no
  large pre-roll. This revision has **no `send_ahead` field** (the current spec
  added it); scheduling must derive play time from the audio timestamp via the
  time filter.
- 1043 chunks ≈ 26.1 s of audio across three `stream/start` runs.

## Clock synchronization

- Cadence follows the reference filter: **200 ms** while unsynchronised, then
  **3000 ms** once stable (`_compute_time_sync_interval`); 45 `client/time`
  messages over ~118 s.
- The 2-D Kalman filter converged within ~5 s (`is_time_synchronized == true`).
  Over the session the offset held near **−11,714,794,580 µs** with drift
  **−3 to −5 ppm** (initial estimate −11.5 ppm).

## Not captured (still open for Phase 1 fixtures)

- `stream/clear` (no seek was performed) and `server/command` (volume/mute).
- Roles other than `player@v1` (artwork, visualizer, controller, metadata) and
  FLAC/`codec_header`.
- Server-initiated connections, mDNS client advertisement, reconnection.
- Noise, `client/init`/`server/init`, and the fragmentation envelope — all
  absent in MA 2.10.3.

## Implications for the plan

- **Phase 1 is viable as cleartext.** No Noise is needed to reach
  `server/activate`-equivalent state against MA 2.10.3. Implement
  `client/hello` → `server/hello` → `client/state` → `client/time` in the
  portable client. The `server/activate` message assumed by the plan does not
  exist in this revision; activation is conveyed by `server/hello.active_roles`.
- **`client/init`/`server/init`/Noise are a spec-current feature, not an MA
  2.10.3 feature.** Keep them scoped to Phase 4 and gate on a future MA update.
- **Binary header is `>Bq` (1 + 8 bytes, big-endian), not the plan's guessed
  layout.** Body is PCM16 LE interleaved.
- **`buffer_capacity` and buffer/lead reporting** are negotiated in
  `client/hello`/`client/state`; use the WROOM values from Phase 2.
- **Tooling for reproducible captures** is committed at
  `tools/sendspin_probe.py`; the pcap dissection commands are in "Method".

## Reproduce

```sh
uv tool install sendspin   # provides both sendspin CLI and aiosendspin lib

docker run -d --name eaf-sniff --net=host --cap-add=NET_ADMIN --cap-add=NET_RAW \
  -v /tmp/opencode:/out nicolaka/netshoot:latest \
  tcpdump -i enp8s0 -s0 -U -w /out/sendspin.pcap host <ma-ip> and port 8927

# instrumented client (needs the uv tool venv python)
~/.local/share/uv/tools/sendspin/bin/python tools/sendspin_probe.py \
  ws://<ma-ip>:8927/sendspin 120 | tee /tmp/opencode/sendspin-probe.log
# then start playback to the 'EAF Phase0 Probe' player in MA

docker kill --signal=INT eaf-sniff

docker run --rm -v /tmp/opencode:/out nicolaka/netshoot:latest sh -c '
  tshark -r /out/sendspin.pcap -q -z follow,tcp,ascii,0
  tshark -r /out/sendspin.pcap -Y websocket -T fields \
    -e frame.number -e tcp.srcport -e websocket.payload'
```
