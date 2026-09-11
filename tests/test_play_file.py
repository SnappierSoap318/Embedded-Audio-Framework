"""Exercise the real file HAL and executable using generated temporary fixtures."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import wave

player = sys.argv[1]


def run(path, expected=None):
    result = subprocess.run([player, str(path)], capture_output=True, text=True, timeout=10)
    if expected is None:
        if result.returncode == 0:
            raise AssertionError(f"Unexpected success: {path}: {result.stdout}")
    elif result.returncode or expected not in result.stdout:
        raise AssertionError(f"{path}: {result.returncode}: {result.stdout} {result.stderr}")


with tempfile.TemporaryDirectory(prefix="eaf-playback-") as directory:
    root = Path(directory)
    for width, channels, rate, count in [(2, 2, 48000, 19), (3, 1, 44100, 257),
                                         (4, 2, 48000, 20000), (2, 1, 48000, 0)]:
        path = root / f"pcm-{width}-{channels}.wav"
        with wave.open(str(path), "wb") as output:
            output.setnchannels(channels)
            output.setsampwidth(width)
            output.setframerate(rate)
            output.writeframes(b"\0" * count * channels * width)
        # Python's wave writer omits RIFF's WORD padding on odd data lengths.
        if count * channels * width % 2:
            data = bytearray(path.read_bytes())
            data.append(0)
            struct.pack_into("<I", data, 4, len(data) - 8)
            path.write_bytes(data)
        run(path, f"Read {count}/{count} source frames at {rate} Hz, channels={channels}")
    run(root / "missing.wav")
    run(root)
    fifo = root / "fifo"
    os.mkfifo(fifo)
    run(fifo)  # Must reject, not wait for a writer.
    invalid = root / "invalid.wav"
    invalid.write_bytes(b"RIFF" + struct.pack("<I", 1000) + b"WAVE")
    run(invalid)
    # Header claims more PCM than is physically present.
    data = path.read_bytes()
    invalid.write_bytes(data[:-1])
    run(invalid)
