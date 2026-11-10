#!/usr/bin/env python3
"""Measure fixed Java work between changing frames; check paint counts and pixels."""
import argparse
import json
from pathlib import Path
import re
from serial_console import Console, upload

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--port', default='/dev/ttyUSB1')
parser.add_argument('--fixture', type=Path, default=Path('build/dualcore/perf-pipeline.jar'))
parser.add_argument('--runs', type=int, choices=range(1, 6), default=2)
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
c = Console(args.port, 115200)
original = None
results = []

def command(text, marker):
    c.send(text)
    return c.until(marker, 30)

def stop_if_running():
    status = command('stats', 'AUDIO_DETAIL')
    if any('running=1' in line for line in status):
        c.send('stop'); c.until('RUN_END', 120); c.until('AUDIO_DETAIL', 20)

try:
    stop_if_running()
    original = dict(re.findall(r'(\w+)=([^ ]+)', command('settings', 'SETTINGS')[-1]))
    for text, marker in [('fps off', 'FPS_STATE off'), ('res 240 320', 'RES_OK'),
                         ('scale fit', 'SCALE_STATE'), ('jit off', 'JIT_STATE off')]:
        command(text, marker)
    listing = command('list', 'GAMES_END')
    name = args.fixture.name
    if not any('/'+name+' ' in line for line in listing):
        upload(c, args.fixture, name, 2000000)
        listing = command('list', 'GAMES_END')
    index = next(int(m[1]) for line in listing
                 if (m := re.match(r'(\d+) (\S+\.jar)', line)) and Path(m[2]).name == name)
    c.send('bench 0 1'); c.until('RUN_END', 45); c.until('AUDIO_DETAIL', 20)
    for number in range(args.runs):
        c.send(f'bench {index} 0')
        lines = c.until('RUN_END', 120)
        c.until('AUDIO_DETAIL', 20)
        phases = [dict(work=int(m[1]), frames=48, paints=int(m[2]), checksum=m[3], elapsed_ms=int(m[4]))
                  for line in lines if (m := re.fullmatch(
                      r'PIPELINE_BENCH work=(\d+) frames=48 paints=(\d+) checksum=([0-9a-f]+) elapsed_ms=(\d+)', line))]
        assert [p['work'] for p in phases] == [0, 2000, 6000, 12000]
        assert all(p['paints'] == 48 for p in phases), phases
        assert 'PIPELINE_BENCH_DONE' in lines and 'rc=0' in lines[-1]
        assert re.search(r'delta=0\b', lines[-1]) and 'input=0' in lines[-1]
        fields = dict(re.findall(r'(\w+)=([^ ]+)', next(s for s in lines if s.startswith('PERF '))))
        assert fields['scale_mismatch'] == '0' and fields['hot_live'] == '0'
        if results:
            assert [p['checksum'] for p in phases] == [p['checksum'] for p in results[0]['phases']]
            for key in ('native_crc', 'lcd_crc'):
                assert fields[key] == results[0]['perf'][key]
        results.append(dict(run=number, phases=phases, perf=fields, end=lines[-1],
                            hot_pool=[s for s in lines if s.startswith('HOT_POOL')]))
        args.output.write_text(json.dumps(results, indent=2)+'\n')
    print('PIPELINE_COMPARISON_PASS', flush=True)
finally:
    if original:
        try:
            stop_if_running()
            width, height = original['res'].split('x')
            command(f'res {width} {height}', 'RES_OK')
            command('scale '+original['scale'], 'SCALE_STATE')
            command('jit '+original['jit'], 'JIT_STATE')
            command('fps '+original['fps'], 'FPS_STATE')
        except Exception as error:
            print('PREFERENCE_RESTORE_FAILED', error, flush=True)
    c.serial.close()
