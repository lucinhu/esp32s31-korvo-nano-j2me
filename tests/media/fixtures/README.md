# 可听 AMR 测试素材

这些文件由本工程生成，没有使用外部录音：

- `tone.amr` / `tone.awb`：600 ms、440 Hz、幅度 6000 的正弦波，经真实 AMR-NB 12.2 kbit/s、AMR-WB 23.85 kbit/s 编码。
- `speech.wav`：espeak-ng 合成的 “Audio test. One, two, three.”，作为试听参考。
- `speech.amr` / `speech.awb`：同一句话的 8 kHz AMR-NB、16 kHz AMR-WB 版本。
- `signal.json`：用 ffmpeg 独立解码后测得的采样数、峰值、RMS 和编码文件 SHA-256。

此前夹具直接填充零载荷：AMR-NB 解码结果为全零，AMR-WB 的 RMS 仅约 93，无法验证出声。`build_media_fixture.py` 现在会拒绝这种素材。幅度检查只针对这组应当有声的测试文件，不影响解码器接受合法静音音频。

普通固件和测试 JAR 构建直接使用这里的文件，无需安装编码器。重新生成需要 ffmpeg、espeak-ng、OpenCORE AMR-NB 和 VisualOn AMR-WB 编码库：

```sh
python3 tools/prepare_amr_fixtures.py --amrwb-library /absolute/path/to/libvo-amrwbenc.so
python3 tools/build_media_fixture.py
```

AMR-NB 使用系统 `libopencore-amrnb`；AMR-WB 本次从 [上游仓库](https://sourceforge.net/p/opencore-amr/vo-amrwbenc/ci/master/tree/) 的 `884dd247dbeb6b2bd2cd3291c4872de95700291f` 构建。编码库源码采用 Apache-2.0，没有复制到本仓库；这些文件只包含自行生成的测试波形和语音。
