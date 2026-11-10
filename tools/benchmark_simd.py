#!/usr/bin/env python3
"""Compare original fill, dual-core scalar fill, and Core 1 SIMD fill."""
import argparse
import json
from pathlib import Path
import re
from serial_console import Console, upload

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--port', default='/dev/ttyUSB1')
parser.add_argument('--fixture', type=Path)
parser.add_argument('--target', choices=['image', 'screen'], default='image')
parser.add_argument('--modes', nargs='+', choices=['serial', 'dual', 'simd', 'ppa'])
args = parser.parse_args()
if args.fixture is None:
    args.fixture = Path('build/dualcore') / ('perf-simd.jar' if args.target == 'image' else 'perf-fill565.jar')
if args.modes is None:
    args.modes = ['serial', 'dual', 'simd', 'simd', 'dual', 'serial'] if args.target == 'image' else ['serial', 'dual', 'simd', 'ppa', 'ppa', 'simd', 'dual', 'serial']
name = args.fixture.name
args.output.parent.mkdir(parents=True, exist_ok=True)
c = Console(args.port, 115200)
original = None
results = []

def command(text, marker, seconds=30):
    c.send(text)
    return c.until(marker, seconds)

def stop():
    if any('running=1' in line for line in command('stats', 'AUDIO_DETAIL')):
        command('stop', 'RUN_END', 120)
        c.until('AUDIO_DETAIL', 20)

try:
    stop()
    original = dict(re.findall(r'(\w+)=([^ ]+)', command('settings', 'SETTINGS')[-1]))
    for text, marker in [('fps off', 'FPS_STATE off'), ('res 240 320', 'RES_OK'),
                         ('scale fit', 'SCALE_STATE'), ('jit off', 'JIT_STATE off')]:
        command(text, marker)
    listing = command('list', 'GAMES_END')
    if not any('/'+name+' ' in line for line in listing):
        upload(c, args.fixture, name, 2000000)
        listing = command('list', 'GAMES_END')
    index = next(int(re.match(r'\d+', line)[0]) for line in listing if '/'+name+' ' in line)
    command('bench 0 1', 'RUN_END', 45); c.until('AUDIO_DETAIL', 20)
    for mode in args.modes:
        command('parallel ' + ('off' if mode == 'serial' else 'on'), 'PARALLEL ')
        command('simd ' + ('on' if mode == 'simd' else 'off'), 'SIMD ')
        command('ppafill ' + ('on' if mode == 'ppa' else 'off'), 'PPA_FILL ')
        lines = command(f'bench {index} 0', 'RUN_END', 180)
        c.until('AUDIO_DETAIL', 20)
        phases = [dict(phase=m[1], checksum=m[2], elapsed_ms=int(m[3])) for line in lines
                  if (m := re.fullmatch(r'FILL_BENCH phase=(\S+) frames=240 draws=1440 checksum=([0-9a-f]+|not-read) elapsed_ms=(\d+)', line))]
        assert {p['phase'] for p in phases} == {'full', 'clipped'} and 'FILL_BENCH_DONE' in lines
        assert 'rc=0' in lines[-1] and 'delta=0' in lines[-1] and 'input=0' in lines[-1]
        perf = dict(re.findall(r'(\w+)=([^ ]+)', next(x for x in lines if x.startswith('PERF '))))
        simd = dict(re.findall(r'(\w+)=([^ ]+)', next(x for x in lines if x.startswith('SIMD '))))
        assert perf['scale_mismatch'] == '0' and perf['hot_live'] == '0'
        assert (int(simd['blocks128']) > 0) == (mode == 'simd')
        assert (int(simd['fills']) > 0) == (mode in ['dual', 'simd'])
        ppa = dict(re.findall(r'(\w+)=([^ ]+)', next(x for x in lines if x.startswith('PPA_FILL '))))
        assert (int(ppa['fills']) > 0) == (mode == 'ppa')
        result = dict(mode=mode, phases=phases, perf=perf, simd=simd, end=lines[-1],
                      ppa=ppa, parallel=next(x for x in lines if x.startswith('PARALLEL ')))
        if results:
            assert {p['phase']: p['checksum'] for p in phases} == {p['phase']: p['checksum'] for p in results[0]['phases']}
            for key in ['native_crc', 'lcd_crc']:
                assert perf[key] == results[0]['perf'][key]
        results.append(result)
        args.output.write_text(json.dumps(results, indent=2) + '\n')
    print('SIMD_COMPARISON_PASS', flush=True)
finally:
    if original:
        try:
            stop()
            width, height = original['res'].split('x')
            for text, marker in [(f'res {width} {height}', 'RES_OK'), ('scale '+original['scale'], 'SCALE_STATE'),
                                 ('jit '+original['jit'], 'JIT_STATE'), ('parallel '+original['parallel'], 'PARALLEL '),
                                 ('simd '+original['simd'], 'SIMD '), ('fps '+original['fps'], 'FPS_STATE')]:
                command(text, marker)
            command('ppafill '+original['ppafill'], 'PPA_FILL ')
        except Exception as error:
            print('PREFERENCE_RESTORE_FAILED', error, flush=True)
    c.serial.close()
