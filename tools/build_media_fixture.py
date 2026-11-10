#!/usr/bin/env python3
"""Build a small on-board image/audio/FreeType verification JAR.

Needs javac, ffmpeg and Pillow; output is local test data, outside flash_data.
"""
from pathlib import Path
import math
import shutil
import struct
import subprocess
import tempfile
import wave
import zipfile

from PIL import Image
from prepare_amr_fixtures import pcm_stats

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build-media/validation"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        # The upstream compile-only stubs omit Player.setMediaTime(long),
        # which the runtime implements. Extend a temporary copy for this test.
        stub_tool = work / "midp_stubs.py"
        stub_tool.write_text((ROOT / "third_party/nano_j2me/tools/midp_stubs.py").read_text().replace(
            '"long getMediaTime()",', '"long getMediaTime()", "long setMediaTime(long)",'))
        stubs = subprocess.check_output(["python3", str(stub_tool),
                                         "--out", str(work / "stubs"), "--quiet"], text=True).strip()
        classes = work / "classes"
        subprocess.run(["javac", "--release", "8", "-nowarn", "-encoding", "UTF-8", "-classpath", stubs,
                        "-d", str(classes), str(ROOT / "tests/midlet/MediaBench.java"),
                        str(ROOT / "tests/midlet/ShortPcmBench.java"),
                        str(ROOT / "tests/midlet/ReplayPcmBench.java"),
                        str(ROOT / "tests/midlet/ReplayMidiBench.java"),
                        str(ROOT / "tests/midlet/MidiAudition.java"),
                        str(ROOT / "tests/midlet/AmrAudition.java")], check=True)
        with wave.open(str(work / "tone.wav"), "wb") as output:
            output.setparams((1, 2, 22050, 0, "NONE", "not compressed"))
            output.writeframes(b"".join(struct.pack("<h", round(6000 * math.sin(2 * math.pi * 440 * i / 22050)))
                                        for i in range(13230)))
        for suffix, codec in [("mp3", "libmp3lame"), ("aac", "aac")]:
            subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(work / "tone.wav"),
                            "-c:a", codec, "-b:a", "48k", str(work / f"tone.{suffix}")], check=True)
        # Real encoders produce audible fixtures. Reject silent/near-silent
        # resources before upload, independently decoding them with ffmpeg.
        fixtures = ROOT / "tests/media/fixtures"
        for name, rate in (("tone.amr", 8000), ("tone.awb", 16000),
                           ("speech.amr", 8000), ("speech.awb", 16000)):
            print(name, pcm_stats((fixtures / name).read_bytes(), rate))
            shutil.copy2(fixtures / name, work / name)
        shutil.copy2(fixtures / "speech.wav", work / "speech.wav")
        # A compact reference keeps the audition JAR below 20 KiB, so it also
        # fits boards whose game partition is close to the upload reserve.
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                        "-i", str(work / "speech.wav"), "-ac", "1", "-ar", "16000",
                        "-c:a", "libmp3lame", "-b:a", "16k", str(work / "speech.mp3")], check=True)
        rgba = Image.new("RGBA", (48, 48), (255, 80, 20, 200))
        rgba.putpixel((0, 0), (255, 0, 0, 0))
        rgba.save(work / "rgba.png")
        palette = Image.new("P", (48, 48), 0)
        palette.putpalette([0, 255, 0, 30, 60, 220] + [0] * (768 - 6))
        palette.info["transparency"] = bytes([128, 255])
        palette.save(work / "palette.png")
        rgb = Image.new("RGB", (51, 37))
        rgb.putdata([(x * 5, y * 7, 100) for y in range(37) for x in range(51)])
        rgb.save(work / "odd.jpg", quality=85)
        rgb.convert("CMYK").save(work / "cmyk.jpg", quality=85)
        track = bytearray(b"\x00\xff\x51\x03\x07\xa1\x20\x00\xc0\x00")
        for note in (60, 64, 67):
            track.extend(bytes((0, 0x90, note, 75, 48, 0x80, note, 0)))
        track.extend(b"\x00\xff\x2f\x00")
        (work / "notes.mid").write_bytes(b"MThd" + struct.pack(">IHHH", 6, 0, 1, 96) +
                                         b"MTrk" + struct.pack(">I", len(track)) + track)
        target = OUT / "espressif-media-bench.jar"
        with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\r\nMIDlet-Name: Espressif Media Bench\r\n"
                             "MIDlet-1: Espressif Media Bench, , MediaBench\r\nMicroEdition-Profile: MIDP-2.0\r\n"
                             "MicroEdition-Configuration: CLDC-1.1\r\n\r\n")
            for file in sorted(classes.glob("*.class")):
                archive.write(file, file.name)
            for name in ["tone.wav", "tone.mp3", "tone.aac", "tone.amr", "tone.awb", "notes.mid",
                         "rgba.png", "palette.png", "odd.jpg", "cmyk.jpg"]:
                archive.write(work / name, name)
        print(f"{target}: {target.stat().st_size} bytes")
        short_target = OUT / "short-pcm-bench.jar"
        with zipfile.ZipFile(short_target, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\r\nMIDlet-Name: Short PCM\r\n"
                             "MIDlet-1: Short PCM, , ShortPcmBench\r\nMicroEdition-Profile: MIDP-2.0\r\n"
                             "MicroEdition-Configuration: CLDC-1.1\r\n\r\n")
            for file in sorted(classes.glob("ShortPcmBench*.class")):
                archive.write(file, file.name)
        print(f"{short_target}: {short_target.stat().st_size} bytes")
        replay_target = OUT / "replay-pcm-bench.jar"
        with zipfile.ZipFile(replay_target, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\r\nMIDlet-Name: Replay PCM\r\n"
                             "MIDlet-1: Replay PCM, , ReplayPcmBench\r\nMicroEdition-Profile: MIDP-2.0\r\n"
                             "MicroEdition-Configuration: CLDC-1.1\r\n\r\n")
            for file in sorted(classes.glob("ReplayPcmBench*.class")):
                archive.write(file, file.name)
        print(f"{replay_target}: {replay_target.stat().st_size} bytes")
        midi_replay = OUT / "replay-midi-bench.jar"
        with zipfile.ZipFile(midi_replay, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\r\nMIDlet-Name: Replay MIDI\r\n"
                             "MIDlet-1: Replay MIDI, , ReplayMidiBench\r\nMicroEdition-Profile: MIDP-2.0\r\n"
                             "MicroEdition-Configuration: CLDC-1.1\r\n\r\n")
            for file in sorted(classes.glob("ReplayMidiBench*.class")):
                archive.write(file, file.name)
            for i in range(3):
                archive.write(work / "notes.mid", f"replay-midi-{i}.mid")
        print(f"{midi_replay}: {midi_replay.stat().st_size} bytes")
        audition = OUT / "midi-audition.jar"
        with zipfile.ZipFile(audition, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\r\nMIDlet-Name: MIDI Audition\r\n"
                             "MIDlet-1: MIDI Audition, , MidiAudition\r\nMicroEdition-Profile: MIDP-2.0\r\n"
                             "MicroEdition-Configuration: CLDC-1.1\r\n\r\n")
            for file in sorted(classes.glob("MidiAudition*.class")):
                archive.write(file, file.name)
            for name in ("tone.wav", "notes.mid"):
                archive.write(work / name, name)
        print(f"{audition}: {audition.stat().st_size} bytes")
        amr_audition = OUT / "amr-audition.jar"
        with zipfile.ZipFile(amr_audition, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\r\nMIDlet-Name: AMR Audition\r\n"
                             "MIDlet-1: AMR Audition, , AmrAudition\r\nMicroEdition-Profile: MIDP-2.0\r\n"
                             "MicroEdition-Configuration: CLDC-1.1\r\n\r\n")
            for file in sorted(classes.glob("AmrAudition*.class")):
                archive.write(file, file.name)
            for name in ("speech.mp3", "speech.amr", "speech.awb"):
                archive.write(work / name, name)
        print(f"{amr_audition}: {amr_audition.stat().st_size} bytes")


if __name__ == "__main__":
    main()
