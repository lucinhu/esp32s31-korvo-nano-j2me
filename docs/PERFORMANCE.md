# 性能与音频实测

> 本页保留初始核心的历史测量；最新上游、240×320 游戏、FPS 开关和 RV32 JIT 的结果见 [升级与游戏测试记录](UPSTREAM_GAMES.md)。

2026-10-22，ESP32-S31-Korvo-1 实机，无 SD 卡。CPU 320 MHz、PSRAM 250 MHz、240×320 游戏画面、音量 40；烧录速率 2000000，控制台 115200。测试仅更新应用区，没有重写游戏分区。

核心仍为未修改的子模块 `179722a408d33745759f6e65d7e9d198487e6c79`，解释执行，无 JIT。最终应用 SHA-256：`2e99403fc6a296a3ab5940670c6034423bc766408a393378129122c7f50d6e35`。

## 同条件对比

CPU 和连续重绘各测两轮，表内耗时为平均值。CPU MIDlet 完成固定的 120×4000 次计算，以计算校验值验证工作量；重绘 MIDlet 每轮完成 240 次完整重绘。游戏各运行 300 个 VM 帧，使用全新独立 RMS 目录、无输入。

| 项目 | 优化前 | 优化后 | 变化 |
| --- | ---: | ---: | ---: |
| 固定计算任务，含加载和销毁 | 14.395 s | 8.931 s | 耗时降低 38.0% |
| 连续重绘，VM 统计帧率 | 28.3 FPS | 30.0 FPS | 提升 6.0% |
| 连续重绘的平均画面提交耗时 | 7.782 ms | 6.207 ms | 降低 20.2% |
| 极品飞车14，启动场景 VM 帧率 | 38.3 FPS | 47.4 FPS | 提升 23.8% |
| 极品飞车16，启动场景 VM 帧率 | 20.4 FPS | 23.3 FPS | 提升 14.2% |

**游戏数据仅代表测试到的启动场景，不是比赛过程帧率或完整兼容性认证。** VM 提交可能重复同一画面，不能把提交次数当作不同画面的数量。CPU 测试的调度帧数随执行速度变化，因此只比较固定计算的耗时，不比较其 FPS。

CPU 两轮计算校验值均为 `-1082513742`；最终原始画面 CRC32 `9e3b7837`、LCD 游戏区域 CRC32 `cbd4506b`，前后相同。重绘测试的两个 CRC32 分别为 `c28d5ef8`、`df7449c1`，前后相同；两款游戏的最终 CRC32 也分别一致。最终测试每项均为 `scale_mismatch=0`、`hot_live=0`、PSRAM 回收差值 0。

基线游戏日志中的 `softl` 脚本名称不被上游接受，运行器明确忽略了整段脚本，输入计数为 0；优化后显式不传脚本，以保持实际输入条件一致。有效按键脚本使用 `soft1`，另行用于回归。连续重绘基线使用 `baseline-render.json` 的 `RenderBench`，不采用旧 `perf-render.jar` 的单次绘制数据。

## 具体改动

- 主工程链接片段将约 27 KiB 的解释器热循环放入 IRAM，减少与 LCD、Java 堆争用 PSRAM；不改上游函数。
- VM 常用小块分配优先使用内部 RAM，总上限 96 KiB；大块数据和 6 MiB Java 堆使用 PSRAM，内部分配失败可回退。
- 最近邻缩放使用坐标表和内部行缓冲；缓存同步限制到变化区域。连续变化画面通过少量采样先证明“需要重绘”，避免反复复制完整影子帧；**采样相同不会直接跳帧**，跳过刷新必须经过完整字节比较。
- 音频在第二核心处理，保留重采样器和未满的输出块，缓存 codec 音量设置。空缓冲填充淡出/静音，避免重复旧 DMA 内容；音调改为短淡入淡出的正弦波。

显示影子缓冲额外占用 `宽×高×2` 字节 PSRAM。内部 RAM 预算限制在主工程，Java 堆大小和方向键三角形、中央正方形保持原有设置。

## 音频完整性

独立 AudioBench 连续播放 3 秒、22050 Hz 单声道 WAV，再播放核心合成的六音 MIDI。每个 Player 保留足够时间等待结束。

| 测试 | 原来停止观察时的位置/状态 | 优化后的位置/状态 |
| --- | --- | --- |
| WAV，长度 3.000000 s | 2.798004 s，STARTED | 3.000000 s，PREFETCHED |
| MIDI，长度 3.139682 s | 0.371609 s，STARTED | 3.139682 s，PREFETCHED |

原接口报告大量空闲帧，却每次最多接收 256 帧；MIDI 流已经前进到下一批，未接收的部分因此丢失。现在按真实物理队列分包，接收 MIDI 启动时不查询空闲量的首批 4096 帧，后续以约 80 ms 水位补充。自然结束会排空尾音，退出游戏则中断队列。

最终音频实测：

- 完整接收 **135380 个源 PCM 帧**，恰好为 WAV 66150 帧加 MIDI 69230 帧。
- 输出 **297472 个 48 kHz 立体声帧**，其中 2771 帧用于淡出/静音补齐；codec 写入错误 **0**，记录到的缓冲欠载 **0**。
- 音量设置从基线每块都调用、共 **547 次**，变为 **1 次**。
- `short=1` 是 WAV 启动时提交整个文件，队列先接受 8192 帧、其余由上游重试；不代表丢样。MIDI 首批及后续补充均完整接收。

重采样用 32 位无符号乘除替代逐声道的 64 位除法，保留有符号除法截断规则。主机测试覆盖 4000–96000 Hz 的 10 种采样率、极值和随机输入，**3361873 个立体声输出帧逐样本一致**，AddressSanitizer/UndefinedBehaviorSanitizer 均通过。实际显示函数还通过 1800 帧矩形刷新对照，覆盖四种常用尺寸、两种极端长宽比、重复帧及尚未提交的像素。

这些结果验证了样本接收、播放完成和 codec 数据输出；没有通过麦克风或人工听音评价扬声器音质。

## 最终实机回归

- 内置演示三轮 90 帧，PSRAM 均回到 **13733944 字节**；FIRE、数字 5 的按下/松开和指针坐标正确到达 Java。
- 176×208、128×160、320×240、240×320 各运行 60 帧，缩放检查与内存回收均通过。
- 六款 Flash 游戏各运行 60 帧，注入 `10:fire,20:soft1,40:5,50:fire`；每款收到 8 个按键边沿，均正常退出，`scale_mismatch=0`、`hot_live=0`、PSRAM 差值 0。
- 未出现 MCU panic；部分游戏仍会抛出并捕获上游 Java 异常，未将启动测试视为完整游玩验证。
- 最终停留于游戏列表，分辨率恢复 240×320、音量 40。三角形方向键和中央正方形在帧缓冲采集中保留。

## 复现与记录

按 README 生成并上传三个测试 JAR 后执行：

```sh
python3 tools/benchmark.py build/performance/results.json
python3 tools/benchmark.py build/performance/input-games.json --only games --game-keys '30:fire,90:soft1,150:5,210:fire'
python3 tools/test_scaler.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain tests/audio_resampler_test.c main/audio_resampler.c -o build/resampler-test
build/resampler-test
python3 tools/serial_console.py smoke
```

测试脚本检查播放结束、缩放结果、内部热数据回收及 PSRAM 回收。基线和最终的逐项结果保存在 [performance-results.json](performance-results.json)。本机原始串口日志保留在 `build/performance/baseline.log`、`baseline-render.log`、`optimized.log`、`regression.log`，最终界面帧缓冲采集为 `build/performance/final-ui.png`。

本轮只针对当前核心版本验证。游戏的 Java 异常、未实现的 API 及兼容性限制仍需结合 [基础验证记录](VALIDATION.md) 判断。
