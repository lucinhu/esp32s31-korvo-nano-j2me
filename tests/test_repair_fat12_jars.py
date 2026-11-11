import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
from repair_fat12_jars import FAT12, repair


def fixture(save_cluster=6):
    image = bytearray(20 * 4096)
    struct.pack_into('<H', image, 11, 4096)
    image[13] = 1
    struct.pack_into('<H', image, 14, 1)
    image[16] = 2
    struct.pack_into('<HH', image, 17, 128, 20)
    struct.pack_into('<H', image, 22, 1)
    image[510:512] = b'\x55\xaa'
    image[4096:4099] = image[8192:8195] = b'\xf8\xff\xff'
    source = b'A' * 4096 + b'B' * 4096 + b'C' * 100
    for offset, (name, cluster, size) in enumerate([
        (b'GAME    JAR', 2, len(source)), (b'SAVE    RMS', save_cluster, 37)
    ]):
        at = 12288 + offset * 32
        image[at:at+11] = name
        image[at+11] = 32
        struct.pack_into('<HI', image, at+26, cluster, size)
    image[16384:16384+len(source)] = source
    fs = FAT12(image)
    fs.set(2, 3)
    fs.set(3, 0)  # Lost link: the directory still reports the full JAR size.
    fs.set(4, 0xfff)
    fs.set(save_cluster, 0xfff)
    return bytes(fs.data), source


class RecoveryTests(unittest.TestCase):
    def test_exact_recovery_preserves_rms_and_every_data_byte(self):
        original, source = fixture()
        patched, report = repair(original, {'game.jar': source})
        before, after = FAT12(original), FAT12(patched)
        self.assertEqual(after.chain(2), [2, 3, 4])
        self.assertEqual(after.chain(6), before.chain(6))
        self.assertEqual(after.content(after.entries()[0]), source)
        self.assertEqual(original[12288:], patched[12288:])
        self.assertEqual(original[:4096], patched[:4096])
        self.assertEqual(patched[4096:8192], patched[8192:12288])
        self.assertEqual(len(report), 1)
        self.assertEqual(repair(patched, {'game.jar': source})[1], [])

    def test_never_takes_a_cluster_owned_by_a_save(self):
        original, source = fixture(save_cluster=4)
        with self.assertRaisesRegex(ValueError, 'Uncertain chunk'):
            repair(original, {'game.jar': source})

    def test_ambiguous_orphan_data_is_rejected(self):
        original, source = fixture()
        fs = FAT12(original)
        fs.data[fs.data_start+2*4096:fs.data_start+3*4096] = b'X' * 4096
        for cluster in [5, 7]:
            at = fs.data_start + (cluster - 2) * 4096
            fs.data[at:at+100] = b'C' * 100
        with self.assertRaisesRegex(ValueError, 'Uncertain chunk'):
            repair(bytes(fs.data), {'game.jar': source})

    def test_packed_fat12_updates_preserve_neighbor_nibbles(self):
        original, _ = fixture()
        fs = FAT12(original)
        fs.set(2, 0x123)
        fs.set(3, 0x456)
        self.assertEqual((fs.get(2), fs.get(3)), (0x123, 0x456))
        fs.set(2, 0xfff)
        self.assertEqual(fs.get(3), 0x456)
        fs.set(3, 0xaaa)
        self.assertEqual(fs.get(2), 0xfff)
        self.assertEqual(fs.data[4096:8192], fs.data[8192:12288])

    def test_disagreeing_fat_copies_are_rejected(self):
        original, _ = fixture()
        data = bytearray(original)
        data[8195] ^= 1
        with self.assertRaisesRegex(ValueError, 'FAT copies disagree'):
            FAT12(data)

    def test_source_size_mismatch_is_rejected(self):
        original, source = fixture()
        with self.assertRaisesRegex(ValueError, 'size mismatch'):
            repair(original, {'game.jar': source[:-1]})


if __name__ == '__main__':
    unittest.main()
