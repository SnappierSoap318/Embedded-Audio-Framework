"""Keep OS/MCU dependencies and feature switches out of portable implementation code.
This is a source-level guard, not proof of timing, allocation or thread safety.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parent.parent
for directory in ('core', 'apps'):
    for path in sorted((root / directory).rglob('*')):
        if path.suffix not in ('.c', '.h'):
            continue
        for line_number, line in enumerate(path.read_text().splitlines(), 1):
            include = re.match(r'\s*#\s*include\s*[<"]([^>"]+)', line)
            if include:
                name = include.group(1)
                forbidden = name.startswith(('zephyr/', 'alsa/', 'freertos/', 'esp_', 'driver/'))
                forbidden |= name in ('pthread.h', 'semaphore.h', 'sys/socket.h', 'unistd.h')
                assert not forbidden, f'{path}:{line_number}: OS dependency belongs in HAL'
            # Header guards are allowed; executable C must use TU selection for features.
            if path.suffix == '.c':
                assert not re.match(r'\s*#\s*(if|ifdef|ifndef)\b', line), (
                    f'{path}:{line_number}: conditional compilation in portable C logic')
print('Portable source architecture guards passed')
