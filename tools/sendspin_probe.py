#!/usr/bin/env python3
"""Phase 0 Sendspin wire probe.

Connects to a Sendspin server as a player@v1 client using the reference
aiosendspin library, but logs every cleartext JSON message, every binary frame
header, and audio chunk framing so protocol fixtures are built from observed
bytes rather than guesses.

This targets the MA 2.10.3 / aiosendspin 6.0.5 cleartext revision. See
docs/bench/sendspin-capture-2026-09-13.md for the captured layout.

Requires the uv tool environment that ships both the CLI and the library:

  uv tool install sendspin
  ~/.local/share/uv/tools/sendspin/bin/python tools/sendspin_probe.py \
      ws://<ma-ip>:8927/sendspin [duration_seconds]
"""

from __future__ import annotations

import asyncio
import json
import struct
import sys
import time

from aiosendspin.client import SendspinClient
from aiosendspin.models import BinaryMessageType, DeviceInfo
from aiosendspin.models.player import (
    ClientHelloPlayerSupport,
    SupportedAudioFormat,
)
from aiosendspin.models.types import AudioCodec, GoodbyeReason, PlayerCommand, Roles

_T0 = time.monotonic()
BIN_HEADER = struct.Struct(">Bq")
audio_chunks = 0
audio_bytes = 0
first_chunk_logged = 0


def ts() -> str:
    return f"[{int((time.monotonic() - _T0) * 1000):7d}ms]"


def pretty(data: str) -> str:
    try:
        return json.dumps(json.loads(data), indent=2, sort_keys=False)
    except Exception:
        return data


def hexdump(data: bytes, limit: int = 48) -> str:
    h = data[:limit].hex(" ")
    return h + (f" ...(+{len(data) - limit}B)" if len(data) > limit else "")


def install_instrumentation(client: SendspinClient) -> None:
    orig_send = client._send_message
    orig_json = client._handle_json_message
    orig_bin = client._handle_binary_message

    async def traced_send(payload: str) -> None:
        print(f"{ts()} >> TX TEXT ({len(payload.encode())}B)\n{pretty(payload)}\n", flush=True)
        await orig_send(payload)

    async def traced_json(data: str) -> None:
        print(f"{ts()} << RX TEXT ({len(data.encode())}B)\n{pretty(data)}\n", flush=True)
        await orig_json(data)

    def traced_bin(payload: bytes) -> None:
        global audio_chunks, audio_bytes, first_chunk_logged
        if not payload:
            print(f"{ts()} << RX BINARY empty", flush=True)
            return
        raw_type = payload[0]
        try:
            mtype = BinaryMessageType(raw_type)
        except ValueError:
            mtype = f"unknown({raw_type})"
        if len(payload) >= BIN_HEADER.size:
            _, tstamp = BIN_HEADER.unpack(payload[: BIN_HEADER.size])
            body = payload[BIN_HEADER.size:]
            print(
                f"{ts()} << RX BINARY type={mtype} total={len(payload)}B "
                f"header=[{hexdump(payload[:BIN_HEADER.size])}] ts_us={tstamp} "
                f"body={len(body)}B body0..=[{hexdump(body, 32)}]",
                flush=True,
            )
            if mtype == BinaryMessageType.AUDIO_CHUNK:
                audio_chunks += 1
                audio_bytes += len(body)
                if first_chunk_logged < 8 or audio_chunks % 50 == 0:
                    first_chunk_logged += 1
                    print(
                        f"{ts()}   AUDIO chunk #{audio_chunks}: {len(body)}B "
                        f"frames16={len(body) // 4} chunk_ms={len(body) / 4 / 44.1:.2f}",
                        flush=True,
                    )
        else:
            print(
                f"{ts()} << RX BINARY type={mtype} total={len(payload)}B "
                f"[{hexdump(payload)}]",
                flush=True,
            )
        orig_bin(payload)

    client._send_message = traced_send
    client._handle_json_message = traced_json
    client._handle_binary_message = traced_bin


async def main() -> int:
    url = sys.argv[1] if len(sys.argv) > 1 else "ws://192.168.11.132:8927/sendspin"
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0

    player_support = ClientHelloPlayerSupport(
        supported_formats=[
            SupportedAudioFormat(
                codec=AudioCodec.PCM, channels=2, sample_rate=44100, bit_depth=16
            )
        ],
        buffer_capacity=262144,
        supported_commands=[PlayerCommand.VOLUME, PlayerCommand.MUTE],
    )
    client = SendspinClient(
        client_id="eaf-phase0-probe",
        client_name="EAF Phase0 Probe",
        roles=[Roles.PLAYER],
        device_info=DeviceInfo(
            product_name="EAF Phase0 Probe",
            manufacturer="EAF",
            software_version="phase0",
        ),
        player_support=player_support,
        static_delay_ms=0,
        required_lead_time_ms=250,
        min_buffer_ms=250,
        initial_volume=100,
        initial_muted=False,
        state_supported_commands=[],
    )
    install_instrumentation(client)

    client.add_server_hello_listener(
        lambda p: print(
            f"{ts()} EVENT server_hello name={p.name!r} id={p.server_id} v={p.version}",
            flush=True,
        )
    )
    client.add_stream_start_listener(
        lambda m: print(f"{ts()} EVENT stream_start {m.to_json()}", flush=True)
    )
    client.add_stream_end_listener(
        lambda roles: print(f"{ts()} EVENT stream_end roles={roles}", flush=True)
    )
    client.add_stream_clear_listener(
        lambda roles: print(f"{ts()} EVENT stream_clear roles={roles}", flush=True)
    )
    client.add_group_update_listener(
        lambda p: print(f"{ts()} EVENT group_update {p.to_json()}", flush=True)
    )
    client.add_server_command_listener(
        lambda p: print(f"{ts()} EVENT server_command {p.to_json()}", flush=True)
    )
    client.add_disconnect_listener(lambda: print(f"{ts()} EVENT disconnected", flush=True))

    print(f"{ts()} connecting to {url}", flush=True)
    await client.connect(url)
    print(f"{ts()} handshake complete; server_info={client.server_info}", flush=True)

    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        await asyncio.sleep(5)
        f = client._time_filter
        print(
            f"{ts()} STATUS sync={client.is_time_synchronized()} "
            f"offset_us={getattr(f, '_offset', float('nan')):.1f} "
            f"drift_ppm={getattr(f, '_drift', 0.0) * 1e6:.3f} "
            f"count={getattr(f, '_count', '?')} "
            f"audio_chunks={audio_chunks} audio_bytes={audio_bytes}",
            flush=True,
        )

    print(f"{ts()} sending goodbye + disconnect", flush=True)
    try:
        await client.send_goodbye(GoodbyeReason.USER_REQUEST)
    except Exception as exc:  # noqa: BLE001
        print(f"{ts()} goodbye failed: {exc}", flush=True)
    await asyncio.sleep(0.5)
    await client.disconnect()
    print(f"{ts()} done. audio_chunks={audio_chunks} audio_bytes={audio_bytes}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
