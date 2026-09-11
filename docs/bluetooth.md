# Zephyr Classic A2DP binding

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
