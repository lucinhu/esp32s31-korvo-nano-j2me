#!/usr/bin/env python3
"""Recover broken JAR chains in an offline FAT12 image using exact source bytes.

Writes a separate image, never a device. Every selected cluster must match its
original JAR chunk and be unowned or belong to that same file. Directory entries,
file contents, RMS chains and all bytes outside the FAT copies remain unchanged.
"""
import argparse
from collections import defaultdict
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct


@dataclass
class Entry:
    path: str
    cluster: int
    size: int
    directory: bool


class FAT12:
    def __init__(self, data):
        self.data = bytearray(data)
        u16 = lambda at: struct.unpack_from('<H', data, at)[0]
        self.sector = u16(11)
        self.cluster_bytes = self.sector * data[13]
        self.fat_start = u16(14) * self.sector
        self.copies = data[16]
        self.fat_bytes = u16(22) * self.sector
        self.root_start = self.fat_start + self.copies * self.fat_bytes
        self.root_bytes = ((u16(17) * 32 + self.sector - 1) // self.sector) * self.sector
        self.data_start = self.root_start + self.root_bytes
        sectors = u16(19) or struct.unpack_from('<I', data, 32)[0]
        self.cluster_count = (sectors * self.sector - self.data_start) // self.cluster_bytes
        if data[510:512] != b'\x55\xaa' or not 0 < self.cluster_count < 4085:
            raise ValueError('Expected a valid FAT12 image')
        for copy in range(1, self.copies):
            at = self.fat_start + copy * self.fat_bytes
            if data[at:at+self.fat_bytes] != data[self.fat_start:self.fat_start+self.fat_bytes]:
                raise ValueError('FAT copies disagree; resolve that before recovery')

    def get(self, cluster):
        at = self.fat_start + cluster * 3 // 2
        pair = self.data[at] | self.data[at+1] << 8
        return pair >> 4 if cluster & 1 else pair & 0xfff

    def set(self, cluster, value):
        for copy in range(self.copies):
            at = self.fat_start + copy * self.fat_bytes + cluster * 3 // 2
            pair = self.data[at] | self.data[at+1] << 8
            pair = (pair & 0xf) | value << 4 if cluster & 1 else (pair & 0xf000) | value
            self.data[at:at+2] = struct.pack('<H', pair)

    def chain(self, cluster):
        result, seen = [], set()
        while 2 <= cluster < self.cluster_count + 2:
            if cluster in seen:
                raise ValueError('FAT chain loop')
            seen.add(cluster)
            result.append(cluster)
            cluster = self.get(cluster)
        return result

    def block(self, cluster):
        at = self.data_start + (cluster - 2) * self.cluster_bytes
        return bytes(self.data[at:at+self.cluster_bytes])

    def content(self, entry):
        return b''.join(self.block(c) for c in self.chain(entry.cluster))[:entry.size]

    def entries(self):
        result = []
        visited = set()

        def visit(raw, parent):
            long = {}
            for at in range(0, len(raw), 32):
                slot = raw[at:at+32]
                if slot[0] == 0:
                    break
                if slot[0] == 0xe5:
                    long = {}
                    continue
                if slot[:11].rstrip() in (b'.', b'..'):
                    long = {}
                    continue
                if slot[11] == 0xf:
                    if slot[0] & 0x40:
                        long = {}
                    long[slot[0] & 0x1f] = slot[1:11] + slot[14:26] + slot[28:32]
                    continue
                short = slot[:8].decode('cp437').rstrip() + '.' + slot[8:11].decode('cp437').rstrip()
                name = short.rstrip('.')
                if long:
                    name = b''.join(long[i] for i in sorted(long)).decode('utf-16le').split('\x00')[0].rstrip('\uffff')
                long = {}
                if slot[11] & 8 or name in ('.', '..'):
                    continue
                entry = Entry(parent + '/' + name, struct.unpack_from('<H', slot, 26)[0],
                              struct.unpack_from('<I', slot, 28)[0], bool(slot[11] & 16))
                result.append(entry)
                if entry.directory:
                    if entry.cluster in visited:
                        raise ValueError('Directory loop')
                    visited.add(entry.cluster)
                    visit(b''.join(self.block(c) for c in self.chain(entry.cluster)), entry.path)

        visit(self.data[self.root_start:self.root_start+self.root_bytes], '')
        return result


def repair(data, sources):
    fs = FAT12(data)
    entries = fs.entries()
    owners = defaultdict(set)
    for entry in entries:
        for cluster in fs.chain(entry.cluster):
            owners[cluster].add(entry.path)
    if any(len(paths) > 1 for paths in owners.values()):
        raise ValueError('Cross-linked files; refusing automatic recovery')
    blocks = {c: fs.block(c) for c in range(2, fs.cluster_count + 2)}
    claimed, report = set(), []
    for entry in entries:
        if entry.directory or not entry.path.lower().endswith('.jar'):
            continue
        name = entry.path.rsplit('/', 1)[-1].casefold()
        source = sources.get(name)
        if source is None:
            if len(fs.content(entry)) != entry.size:
                raise ValueError(f'Missing source for broken archive: {entry.path}')
            continue
        if len(source) != entry.size:
            raise ValueError(f'Source size mismatch: {entry.path}')
        if fs.content(entry) == source:
            continue
        chain = []
        for offset in range(0, len(source), fs.cluster_bytes):
            chunk = source[offset:offset+fs.cluster_bytes]
            matches = [c for c, block in blocks.items()
                       if c not in claimed and c not in chain
                       and owners[c] <= {entry.path} and block[:len(chunk)] == chunk]
            preferred = entry.cluster if not chain else chain[-1] + 1
            if preferred in matches:
                choice = preferred
            elif len(matches) == 1 and chain:
                choice = matches[0]
            else:
                raise ValueError(f'Uncertain chunk at {entry.path}+{offset}: {matches}')
            if not chain and choice != entry.cluster:
                raise ValueError('First cluster must stay unchanged')
            chain.append(choice)
        old = fs.chain(entry.cluster)
        for i, cluster in enumerate(chain):
            fs.set(cluster, chain[i+1] if i+1 < len(chain) else 0xfff)
        claimed.update(chain)
        assert fs.content(entry) == source
        report.append({'path': entry.path, 'bytes': entry.size, 'old_clusters': old,
                       'recovered_clusters': chain, 'sha256': hashlib.sha256(source).hexdigest()})
    # Only FAT entries of the reconstructed archives may have changed.
    assert fs.data[:fs.fat_start] == data[:fs.fat_start]
    end = fs.fat_start + fs.copies * fs.fat_bytes
    assert fs.data[end:] == data[end:]
    for entry in entries:
        if not entry.path.lower().endswith('.jar'):
            assert fs.chain(entry.cluster) == FAT12(data).chain(entry.cluster)
    return bytes(fs.data), report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('sources', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    sources = {p.name.casefold(): p.read_bytes() for p in args.sources.glob('*.jar')}
    if 'jbenchmark.jar' in sources:
        sources['jbenchmark1.jar'] = sources['jbenchmark.jar']
    patched, report = repair(args.image.read_bytes(), sources)
    if args.output.exists():
        raise SystemExit('Refusing to overwrite an existing output image')
    args.output.write_bytes(patched)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(f'Recovered {len(report)} JAR chains; wrote {args.output}')


if __name__ == '__main__':
    main()
