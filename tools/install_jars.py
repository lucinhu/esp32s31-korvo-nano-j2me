#!/usr/bin/env python3
"""Install local JARs, optionally freeing space from an explicit removal list.

Only names passed with --remove are candidates, and only as many as required
are removed. Game RMS saves and built-in test packages are preserved.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zlib
from serial_console import Console, upload

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('files', nargs='+', type=Path)
parser.add_argument('--port', default='/dev/ttyUSB1')
parser.add_argument('--remove', nargs='*', default=[], metavar='BASENAME.jar')
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
packages = []
for path in args.files:
    data = path.read_bytes()
    packages.append(dict(source=str(path.resolve()), name=path.name, bytes=len(data),
                         sha256=hashlib.sha256(data).hexdigest(), crc32=f'{zlib.crc32(data):08x}'))
c = Console(args.port, 115200)
result = dict(packages=packages, removed=[], uploaded=[], already_installed=[])
args.output.parent.mkdir(parents=True, exist_ok=True)

def command(text, marker):
    c.send(text)
    return c.until(marker, 120)

def listing():
    return {Path(m[2]).name: dict(index=int(m[1]), path=m[2], bytes=int(m[3]))
            for line in command('list', 'GAMES_END')
            if (m := re.match(r'(\d+) (\S+\.jar).* bytes=(\d+)$', line))}

def free_space():
    return int(re.search(r'free=(\d+)', command('storage', 'STORAGE root=')[-1])[1])

try:
    if any('running=1' in line for line in command('stats', 'AUDIO_DETAIL')):
        command('stop', 'RUN_END'); c.until('AUDIO_DETAIL', 20)
    games = listing()
    missing = [p for p in packages if p['name'] not in games]
    required = sum((p['bytes']+4095)//4096*4096 for p in missing) + 262144 + 8192
    result['free_before'] = available = free_space()
    requested = {p['name'] for p in packages}
    for name in args.remove:
        if available >= required:
            break
        if name in requested:
            raise ValueError('A requested JAR cannot be a removal candidate: ' + name)
        games = listing()
        if name not in games or not games[name]['path'].startswith('/flash/'):
            continue
        candidate = dict(name=name, **games[name])
        answer = command(f"delete {candidate['index']}", 'DELETE_')[-1]
        if not answer.startswith('DELETE_OK '):
            raise RuntimeError(answer)
        result['removed'].append(candidate)
        available = free_space()
    if available < required:
        raise RuntimeError(f'Insufficient space: free={available}, required={required}; add explicit --remove candidates')
    games = listing()
    for package in packages:
        if package['name'] in games:
            if games[package['name']]['bytes'] != package['bytes']:
                raise RuntimeError('Existing JAR has a different size: ' + package['name'])
            result['already_installed'].append(package['name'])
            continue
        upload(c, Path(package['source']), package['name'], 2000000)
        result['uploaded'].append(package['name'])
        args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n')
    games = listing()
    for package in packages:
        assert games[package['name']]['bytes'] == package['bytes']
        package['installed_path'] = games[package['name']]['path']
    result['free_after'] = free_space()
    assert result['free_after'] >= 262144
    print('INSTALL_JARS_PASS', len(packages), flush=True)
finally:
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n')
    c.serial.close()
