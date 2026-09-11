#!/usr/bin/env python3
"""Check owned C code using clang-format, clang-tidy and clangd.
Usage: python3 tools/check_code.py [build directory with compile_commands.json]
External vendor/generated sources are excluded; included project headers are checked.
"""
import concurrent.futures
import json
import shlex
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
build = (root / (sys.argv[1] if len(sys.argv) > 1 else 'build-lint')).resolve()
owned = [root / name for name in ('apps', 'core', 'hal', 'include', 'platform', 'tests')]
formats = sorted(str(p) for directory in owned for p in directory.rglob('*')
                 if p.suffix in ('.c', '.h', '.inc'))
subprocess.run(['clang-format', '--dry-run', '--Werror', *formats], check=True)
entries = json.loads((build / 'compile_commands.json').read_text())
# Zephyr's GCC host flags have no Clang equivalent; preserve defines/includes.
for entry in entries:
    args = entry.get('arguments') or shlex.split(entry['command'])
    entry['arguments'] = ['-fhosted' if arg == '-fno-freestanding' else arg for arg in args
                          if arg not in ('-fno-reorder-functions', '-fno-defer-pop', '--param=min-pagesize=0')]
    entry.pop('command', None)
clang_db = build / 'eaf-clang'
clang_db.mkdir(exist_ok=True)
(clang_db / 'compile_commands.json').write_text(json.dumps(entries))
files = sorted({str(Path(entry['file']).resolve()) for entry in entries
                if any(Path(entry['file']).resolve().is_relative_to(p) for p in owned)})
if not files:
    sys.exit('No project translation units in compilation database')

def check(path):
    failures = []
    for command in (['clang-tidy', '-p', str(clang_db), path],
                    ['clangd', '--tweaks=', '--enable-config=0', '--compile-commands-dir=' + str(clang_db),
                     '--check=' + path]):
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            failures.append(result.stdout + result.stderr)
    return failures

with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    failures = [failure for result in pool.map(check, files) for failure in result]
for failure in failures:
    print(failure)
print(f'{len(files)} translation units checked; {len(failures)} failed checks')
sys.exit(bool(failures))
