#!/usr/bin/env python3
"""Compare fixed-work M3G rendering with parallel rendering off/on.

Build the fixture with tools/build_parallel_fixture.py first. Missing fixtures
are uploaded with CRC checks. All partitions must produce identical pixels.
"""
import argparse
import json
from pathlib import Path
import re
from serial_console import Console, upload

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--port', default='/dev/ttyUSB1')
parser.add_argument('--fixture', type=Path, default=Path('build/dualcore/perf-parallel.jar'))
parser.add_argument('--name', default='perf-parallel.jar')
parser.add_argument('--modes', nargs='+', choices=['off', 'on'], default=['off', 'on', 'on', 'off'])
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
c = Console(args.port, 115200)
results = []
original = None

def command(text, marker):
    c.send(text)
    return c.until(marker, 30)

def stop_if_running():
    lines = command('stats', 'AUDIO_DETAIL')
    if any('running=1' in line for line in lines):
        c.send('stop'); c.until('RUN_END', 120); c.until('AUDIO_DETAIL', 20)

try:
    stop_if_running()
    original = dict(re.findall(r'(\w+)=([^ ]+)', command('settings', 'SETTINGS')[-1]))
    command('fps off', 'FPS_STATE off')
    command('res 240 320', 'RES_OK')
    command('scale fit', 'SCALE_STATE')
    command('jit off', 'JIT_STATE off')
    listing = command('list', 'GAMES_END')
    if not any('/' + args.name in line for line in listing):
        upload(c, args.fixture, args.name, 2000000)
        listing = command('list', 'GAMES_END')
    indices = {Path(m[2]).name: int(m[1]) for line in listing
               if (m := re.match(r'(\d+) (\S+\.jar)', line))}
    index = indices[args.name]
    # Prime board/font/stdio state before checking repeated-run memory recovery.
    c.send('bench 0 1'); c.until('RUN_END', 45); c.until('AUDIO_DETAIL', 20)
    for number, mode in enumerate(args.modes):
        command('parallel ' + mode, 'PARALLEL ')
        c.send(f'bench {index} 0')
        lines = c.until('RUN_END', 180)
        c.until('AUDIO_DETAIL', 20)
        phases = [dict(phase=m[1], checksum=m[2], elapsed_ms=int(m[3])) for line in lines
                  if (m := re.fullmatch(r'PARALLEL_BENCH phase=(\S+) frames=36 draws=144 checksum=([0-9a-f]+) elapsed_ms=(\d+)', line))]
        assert {p['phase'] for p in phases} == {'flat', 'gouraud', 'nearest', 'linear-alpha'}
        assert 'PARALLEL_BENCH_DONE' in lines and 'rc=0' in lines[-1]
        delta = int(re.search(r'delta=(-?\d+)', lines[-1])[1])
        assert delta == 0
        perf = next(line for line in lines if line.startswith('PERF '))
        fields = dict(re.findall(r'(\w+)=([^ ]+)', perf))
        assert fields['scale_mismatch'] == '0' and fields['hot_live'] == '0'
        assert 'input=0' in lines[-1]
        parallel = next(line for line in lines if line.startswith('PARALLEL '))
        if mode == 'on':
            assert 'worker_core=1' in parallel and re.search(r'jobs=[1-9]\d*', parallel)
        else:
            assert 'jobs=0' in parallel
        result = dict(run=number, mode=mode, phases=phases, perf=fields,
                      parallel=parallel, end=lines[-1])
        if results:
            assert {p['phase']: p['checksum'] for p in phases} == {p['phase']: p['checksum'] for p in results[0]['phases']}
            for key in ('native_crc', 'lcd_crc'):
                assert fields[key] == results[0]['perf'][key]
        results.append(result)
        args.output.write_text(json.dumps(results, indent=2) + '\n')
    print('PARALLEL_COMPARISON_PASS', flush=True)
finally:
    if original:
        # Restore user settings even when a comparison raises an assertion.
        try:
            stop_if_running()
            width, height = original['res'].split('x')
            command(f'res {width} {height}', 'RES_OK')
            command('scale ' + original['scale'], 'SCALE_STATE')
            command('jit ' + original['jit'], 'JIT_STATE')
            command('parallel ' + original.get('parallel', 'on'), 'PARALLEL ')
            command('fps ' + original['fps'], 'FPS_STATE')
        except Exception as error:
            print('PREFERENCE_RESTORE_FAILED', str(error), flush=True)
    c.serial.close()
