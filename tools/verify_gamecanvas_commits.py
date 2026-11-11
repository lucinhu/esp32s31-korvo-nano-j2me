#!/usr/bin/env python3
"""Check that slow GameCanvas producers only publish complete frames on hardware."""
import argparse
import json
from pathlib import Path
import re

from PIL import Image
from serial_console import Console, screenshot, upload

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--port', default='/dev/ttyUSB1')
parser.add_argument('--samples', type=int, default=8)
parser.add_argument('--embedded', action='store_true', help='Use @diagnostic.jar built with KORVO_DIAGNOSTIC_JAR instead of uploading a temporary JAR')
args = parser.parse_args()
if args.samples < 1:
    parser.error('--samples must be positive')
root = Path(__file__).resolve().parent.parent
fixture = root / 'third_party/nano_j2me/tests/fixtures/flushdemo.jar'
if not args.embedded and not fixture.is_file():
    parser.error('Generate tests/fixtures/flushdemo.jar with the core fixture target first')
args.output.parent.mkdir(parents=True, exist_ok=True)
name = 'regression-gc-idle.jar'
c = Console(args.port, 115200)
original = None
owned_index = None
result = {'samples': []}


def command(text, marker, timeout=30):
    c.send(text)
    return c.until(marker, timeout)


def stop():
    lines = command('stats', 'AUDIO_DETAIL')
    if any('running=1' in line for line in lines):
        command('stop', 'AUDIO_DETAIL', 120)


try:
    c.drain(2.5)  # Allow UART/storage initialization after app-flash resets.
    stop()
    original = dict(re.findall(r'(\w+)=([^ ]+)', command('settings', 'SETTINGS')[-1]))
    for text, marker in [('fps off', 'FPS_STATE'), ('jit off', 'JIT_STATE'),
                         ('res 240 320', 'RES_OK')]:
        command(text, marker)
    # Prime the board's first-run runtime allocations before measuring leaks.
    command('bench 0 1', 'RUN_END', 90)
    c.until('AUDIO_DETAIL', 20)
    listing = command('list', 'GAMES_END')
    if args.embedded:
        name = '@diagnostic.jar'
    else:
        if any('/' + name + ' ' in line for line in listing):
            raise RuntimeError('Regression JAR name already exists; refusing to replace it')
        upload(c, fixture, name, 2000000)
        listing = command('list', 'GAMES_END')
    index = next(int(m[1]) for line in listing
                       if (m := re.match(r'(\d+) (\S+\.jar)', line)) and Path(m[2]).name == name)
    if not args.embedded:
        owned_index = index
    result['embedded'] = args.embedded
    command(f'bench {index} 0', 'FLUSH loop=', 90)
    for i in range(args.samples):
        key = ['6', '8', '4', '2'][i % 4]
        command(f'key {key} down', 'KEY_OK')
        command(f'key {key} up', 'KEY_OK')
        command('pointer down 8 8', 'POINTER_OK')
        command('pointer move 10 8', 'POINTER_OK')
        command('pointer up 10 8', 'POINTER_OK')
        path = args.output.with_name(f'{args.output.stem}-{i}.png')
        screenshot(c, path, True, 2000000)
        with Image.open(path) as img:
            colors = img.convert('RGB').getcolors(img.width * img.height)
            pixels = img.width * img.height
        result['samples'].append({'sample': i, 'pixels': pixels, 'colors': colors})
        args.output.write_text(json.dumps(result, indent=2) + '\n')
        if len(colors) != 1 or colors[0][0] != pixels or colors[0][1] not in [(255, 0, 0), (0, 0, 255)]:
            raise RuntimeError(f'Incomplete committed frame: {colors[:8]}')
    lines = command('stop', 'RUN_END', 120)
    command('stats', 'AUDIO_DETAIL')
    for prefix, label in [('RUN_END', 'end'), ('PERF ', 'perf'), ('DISPLAY_IDLE ', 'idle')]:
        result[label] = next((line for line in lines if prefix in line), '')
    assert 'rc=0' in result['end'] and 'delta=0' in result['end'], result
    assert 'scale_mismatch=0' in result['perf'] and 'hot_live=0' in result['perf'], result
    result['display_test'] = command('displaytest 90', 'DISPLAY_TEST', 90)[-1]
    assert 'mismatches=0' in result['display_test']
    result['passed'] = True
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print('GAMECANVAS_COMMIT_PASS', flush=True)
finally:
    try:
        stop()
        if owned_index is not None:
            command(f'delete {owned_index}', 'DELETE_', 30)
        if original:
            width, height = original['res'].split('x')
            command(f'res {width} {height}', 'RES_OK')
            command('jit ' + original['jit'], 'JIT_STATE')
            command('fps ' + original['fps'], 'FPS_STATE')
    finally:
        c.serial.close()
