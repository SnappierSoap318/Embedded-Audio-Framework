"""Real ALSA file/null plugins verify interleaving and post-volume PCM bytes."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import wave

with tempfile.TemporaryDirectory(prefix='eaf-alsa-') as directory:
    root = Path(directory)
    raw = root / 'samples.raw'
    config = root / 'alsa.conf'
    config.write_text('pcm.capture { type file slave.pcm { type null } '
                      f'file "{raw}" format "raw" }}\n')
    env = dict(os.environ, ALSA_CONFIG_PATH=str(config))
    subprocess.run([sys.argv[1], 'capture'], env=env, check=True, timeout=10)
    assert raw.read_bytes() == struct.pack('=ii', 536870912, 0) * 256

# Exercise the player EOF path: sink drain must precede the player's automatic STOP.
with tempfile.TemporaryDirectory(prefix='eaf-alsa-wav-') as directory:
    path = Path(directory) / 'short.wav'
    with wave.open(str(path), 'wb') as wav:
        wav.setnchannels(2); wav.setsampwidth(2); wav.setframerate(48000)
        wav.writeframes(bytes(19 * 4))
    subprocess.run([sys.argv[2], str(path), 'null'], check=True, timeout=10)
