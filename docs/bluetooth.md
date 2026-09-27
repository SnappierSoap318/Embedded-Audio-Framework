# Classic A2DP bindings

EAF exposes the same portable SBC ingress to two Classic A2DP sink bindings:
Zephyr (`hal/zephyr/bt_a2dp_zephyr.c`) and ESP-IDF/Bluedroid
(`hal/esp_idf/bt_a2dp_esp_idf.c`). Both advertise one SBC stream endpoint and
republish media payload into `eaf_bt_ingress_t`; the portable
`apps/bt/bt_decoder.c` worker decodes it through `apps/decoders/sbc_oi.c` and
the vendored OI/libsbc decoder into a stereo reservoir.

## Zephyr Classic A2DP binding

`CONFIG_EAF_BT_A2DP` adds `eaf_bt_zephyr_register()`. It requires
`CONFIG_BT_CLASSIC`, `CONFIG_BT_A2DP_SINK` and the EAF ingress queue. Enable the
OI decoder separately with `CONFIG_EAF_BT_SBC`.

Initialize a caller-owned ingress queue and prepare the decode/output resources
for 44100 or 48000 Hz stereo. After `bt_enable()`, call register once before
accepting connections, passing that fixed sample rate and a notification callback.
The singleton registers an SBC sink endpoint and stream callbacks against Zephyr
4.3. It accepts stereo/joint stereo, 16 blocks, 8 subbands, loudness allocation,
and bitpool 2–53. Unsupported or ambiguous negotiated values are rejected.

The Bluetooth callback copies only media payload after Zephyr removes RTP.
No decoding or blocking is performed there. Started/suspended/released/configured
notifications must only post work to the application. Callback delivery and media
ingress are the single serialized producer; do not mutate producer diagnostics
or reset the queue from a concurrent worker. Endpoint resources must live for the
Bluetooth host lifetime; registration failure cannot be retried because the
Zephyr registration APIs do not provide an unregister path.

The application still owns SDP records, pairing/discoverability, controller setup,
worker wakeups and audio lifecycle. Suspend/release suppress new media but do not
purge packets already queued: the owner must coordinate stopped producer/consumer
state before discarding old packets and resetting graph/decoder state for another
stream. Resuming marks the next packet discontinuous so the decoder resets its
history. This is not yet a complete standalone phone speaker application.

`platform/zephyr_smoke` mocks only A2DP registration and exercises the actual
adapter callbacks against pinned Zephyr headers. It tests negotiation rejection,
start/suspend/release gating, payload copies and discontinuity. Its few test-only
Bluetooth preprocessor definitions allow these headers without enabling a host;
the smoke deliberately rejects a real Bluetooth host configuration. Board builds
compile the adapter through Kconfig against the actual stack instead.

The native smoke passes. No physical controller, pairing, radio streaming, SDP
integration or phone interoperability has been tested. A BR/EDR-capable controller
is required; BLE-only hardware cannot run this Classic A2DP path.

## ESP-IDF/Bluedroid A2DP sink

`hal/esp_idf/bt_a2dp_esp_idf.c` adds the Bluedroid equivalent. The application
owns controller and Bluedroid init/enable, the GAP device name, pairing and
discoverability; after `esp_bluedroid_enable()` it calls
`eaf_bt_esp_idf_register(queue, rate, notify, ctx)` once. The binding registers
the A2DP callback, initializes the sink, advertises one SBC stream endpoint and
registers the undecoded-audio callback (`esp_a2d_sink_register_audio_data_callback`).

Bluedroid strips the RTP media header before the callback, so the binding
rebuilds the media-header-first layout the portable ingress expects and forwards
the timestamp. It cannot observe the AVDTP/RTP sequence number, so it supplies a
local monotonic sequence and marks discontinuity on connect, configure, start,
suspend and disconnect events instead. A small descriptor table
(`eaf_bt_codec_desc_t`) pairs each codec's capability advertisement with its
negotiated-configuration check; ESP-IDF v6.0.2 allows a single SEP
(`ESP_A2D_MAX_SEPS`), so one codec is active at a time and AAC/LDAC are later
table entries plus a selection source.

`platform/esp_idf_bt` is the board scaffold: it enables Bluedroid Classic with
`CONFIG_BT_A2DP_USE_EXTERNAL_CODEC`, starts the I2S sink, runs the portable
ingress/SBC decode worker and an output owner draining the reservoir, then
registers the binding and becomes discoverable. Source arbitration, volume/mute
and pause remain T08 work. The project builds under ESP-IDF v6.0.2. The user
confirmed phone pairing and clear audible SBC playback on the ESP32/TAS5805M
carrier after flashing the amplifier initialization correction.

The application initializes NVS before Bluedroid and explicitly initializes and
enables the controller in `ESP_BT_MODE_CLASSIC_BT`. Disabling the BLE host alone
does not select the controller mode: fresh builds also select
`CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY`. The explicit initialization mode handles
existing sdkconfigs that retain the dual-mode controller default.

During ESP32 bring-up, a single-reader UART capture identified a reboot loop at
`esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)` with `ESP_ERR_INVALID_ARG`:
ESP-IDF requires the enable mode to equal the initialization mode. Earlier
truncated captures suggested a coexistence startup hang, but competing serial
readers made that diagnosis unreliable. Stop the VS Code Serial Monitor before
capturing with another reader. With the startup correction flashed, the user
confirmed discovery on a phone; the phone then reported that it could not connect.

Pairing uses the no-input/no-output SSP Just Works capability, with a GAP
confirmation callback and authentication/ACL status logging. Legacy peers use
PIN `0000`. The user confirmed phone connection with this correction, but no sound.

The board application now initializes the TAS5805M over ESP-IDF's I2C master
driver: SDA=21, SCL=27, address=0x2d, PDN=33 and FAULT=34 (external pull-up).
These settings are configurable in the application's Kconfig. It commits a silent
I2S block first to start clocks, resets/releases PDN, configures 32-bit standard
I2S and enters Play via HiZ. I2C errors stop startup and put the amplifier back
in power-down. The user reported that playback was "playing perfectly" after
this correction. UART output still becomes garbled during audio startup;
quantitative stream/decode diagnostics cannot be established from that capture.

## Speaker controls and connection management

`platform/esp_idf_bt/components/main/speaker.c` runs one owner task for all
Classic radio policy. A2DP/AVRCP callbacks only copy bounded, owned events into
a queue; no borrowed metadata pointer escapes the callback and no stack call is
made from application context. It provides:

- **Volume and mute.** AVRCP target `SetAbsoluteVolume` (0..127) and
  `RegisterNotification(VOLUME_CHANGE)`. Volume/mute persist in NVS. The audio
  owner applies a squared-amplitude ramp over 256 frames together with mute, so
  changes do not click. A local change sends a `CHANGED` notification; a remote
  change is applied without echoing, avoiding a volume loop.
- **Track metadata.** AVRCP controller metadata (title/artist/album) plus play
  status and position, requested on connect/track change and re-armed via
  notifications. Control characters are stripped before storage or logging.
- **Connection management.** The last peer is remembered in NVS and
  auto-reconnected with bounded backoff (five attempts, cap 32 s, then a pairing
  window). The device is connectable and non-discoverable while a peer is known;
  it is generally discoverable only during a pairing window.
- **Pairing management.** A 120-second pairing window uses SSP Just Works (IO
  capability none). `forget` disconnects, removes all bonds and the remembered
  peer, then reopens pairing.
- **Stream lifecycle.** Two workers preserve the original producer/consumer
  split: a decoder producer drains the blocking ingress into the reservoir, and
  the audio owner drains the reservoir into I2S. Every A2DP state change marks
  the next ingress packet discontinuous, so the decoder resets its history at the
  boundary; the decoder also resets on disconnect. The audio owner ramps gain up
  from silence on each connect. The reservoir is never cursor-reset while
  running, so a stream boundary cannot race the consumer; a short tail drains
  naturally on pause or suspend.
- **Buttonless control.** `speaker_command()` / `speaker_get_status()` plus a
  UART console: `bt status|pair|reconnect|disconnect|forget|mute|unmute|volume
  0..127`.

Only one PCM producer exists in this application; arbitration with Wi-Fi/LMS/
Sendspin sources remains separate work. The controls above are build-validated
and host-tested where portable; the AVRCP and connection paths await hardware
testing.

A first version merged the decoder into the I2S output loop and used a
generation counter to gate media and reset the reservoir on every A2DP event.
That produced audible stuttering: decoding on the output thread added latency
before each blocking I2S write, and the per-event reservoir reset discarded
buffered audio. The two-worker split above removes both; keep the decoder off the
audio owner's critical path.
