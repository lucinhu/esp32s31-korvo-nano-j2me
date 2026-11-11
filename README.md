# ESP32-S31-Korvo-1 nano_j2me

ESP32-S31（RISC-V）上的 J2ME 模拟器。板级配置来自 `esp32s31-korvo-gbsp`：800×480 RGB LCD、GT1151 多点触摸、ES8389 双声道音频和四线 SDMMC。

**无需 SD 卡**：Flash 中的 `j2me` 分区提供可写 FAT 文件系统，用于测试游戏和存档。接入 SD 卡时仍可扫描卡上的 JAR。

未插 SD 卡时，启动日志会出现 SDMMC 超时，随后显示 `Flash FAT storage: /flash (ESP_OK)` 和 `FLASH READY`，继续使用 Flash。

`third_party/nano_j2me` 是 [lucinhu/nano_j2me](https://github.com/lucinhu/nano_j2me) 的 Git 子模块。当前引用核心 `main` 的 `a2e5ba8`（0.5.6，2026-11-11）。最初从 `f835b44` 更新到 `e910b30`，保留并提交本地 `drawRGB` 借用像素、忽略源透明度及源目标重叠保护补丁（`00318b4`）；随后同步远程 0.5.6，并补齐 JBenchmark 所需的 Item 布局、Gauge 和 M3G 兼容修复。所有核心修改均已合入 `main`，临时分支已删除。板级适配仍放在主工程，已补齐新版 PIM、Samsung、UDP 和 x86-64 JIT 源码清单。版本对比、本地补丁、构建与实机验证记录见 [本次更新记录](docs/updates/2026-11-10/result.txt)，跑分见 [JBenchmark 实机记录](docs/JBENCHMARK.md)，双核、SIMD 和二维 DMA 优化见 [性能更新](docs/DUALCORE.md)。最新的 [继续优化记录](docs/REFINEMENT.md) 包含 Form 长列表滚动、控件硬件填充和 PPA 边缘填充优化；同一矩形填充测试较上一版耗时减少约 30%。2026-11-11 同步 Display 回调和蒙皮加载修复，并进一步优化 LCD 等待与重复帧提交，见 [显示优化记录](docs/FRAME_PIPELINE.md)。 同日修复慢速 GameCanvas 绘制与输入触发的地图闪烁，见 [鲁宾逊修复记录](docs/ROBINSON.md)。

## 获取与编译

首次克隆时一并取得子模块：

```sh
git clone --recurse-submodules https://github.com/lucinhu/esp32s31-korvo-nano-j2me.git
cd esp32s31-korvo-nano-j2me
```

已有主仓库检出后初始化核心：

```sh
git submodule update --init --recursive
```

需要支持 **esp32s31** 的 ESP-IDF，本机使用 `/home/lucinhu/esp/esp-idf/esp-idf-main`（IDF 6.2 开发版）。这不是 ESP32-S3 工程。

```sh
bash tools/idf.sh build
bash tools/idf.sh -p /dev/ttyUSB1 -b 2000000 flash
bash tools/idf.sh -p /dev/ttyUSB1 -b 2000000 j2me-flash
bash tools/idf.sh -p /dev/ttyUSB1 -b 115200 monitor
```

烧录速率为 **2000000**，固件控制台为 **115200**。其他机器通过 `KORVO_IDF_PATH=/path/to/esp-idf` 指定工具链，或自行激活 IDF 后直接运行 `idf.py`。

工程包含 `sdkconfig`、默认配置和 Board Manager 生成文件。LCD/SD/音频 GPIO 与参考工程一致。16 MiB Flash 的应用分区为 8 MiB；`j2me` FAT 分区从 `0x810000` 开始，大小为 `0x7f0000`（8128 KiB），使用磨损均衡。Java 堆默认分配 10 MiB，帧缓冲和大块数据使用 PSRAM；代码和静态资源保留在 Flash，避免整份固件复制占用游戏内存（解释器/JIT 热路径仍位于内部 RAM）；VM 小块宿主数据使用受限私有内存池：可用内部 RAM 至少 256 KiB 且连续空间足够时创建 32 KiB 池，否则使用 PSRAM。本轮双核配置中该池未启用；降低门槛的实测没有得到加速，保留原策略。CPU 320 MHz，PSRAM 250 MHz，沿用参考配置。

`j2me-flash` **重写整个游戏分区及其中存档**，仅用于首次写入或主动更换测试镜像。后续更新固件使用 `flash` 或 `app-flash`，不会重写游戏分区。运行时挂载失败不会自动格式化。

将本机游戏目录打包到 Flash 镜像：

```sh
python3 tools/prepare_flash_games.py /home/lucinhu/Documents/game-roms/jar
bash tools/idf.sh build
bash tools/idf.sh -p /dev/ttyUSB1 -b 2000000 j2me-flash
```

JAR 原文件不修改，也不纳入主仓库；`flash_data/games-manifest.json` 记录原文件名、Flash 文件名、大小和 SHA-256。为兼容 IDF FAT 镜像工具，磁盘名称使用内容哈希，`titles.tsv` 保留中文显示名称。初始镜像的六款测试游戏合计 5,189,962 字节；后续通过串口追加的游戏见 [升级与游戏测试记录](docs/UPSTREAM_GAMES.md)。每次构建根据本机 `flash_data` 生成 FAT 镜像；未准备外部游戏时仍可运行内置演示。

修改板级 YAML 后可重新生成：

```sh
bash tools/idf.sh bmgr -b esp32_s31_korvo1
bash tools/idf.sh build
```

## 游戏和操作

SD 卡使用 FAT 文件系统，扫描下列目录的 `.jar` 文件（不递归，最多 63 个外部 JAR）：

```text
/sdcard/game.jar
/sdcard/roms/j2me/game.jar
/sdcard/j2me/game.jar
/sdcard/j2me/rms/<游戏路径CRC32>/    每款游戏独立的 RMS 存档
/sdcard/j2me/files/                 FileConnection 文件根目录
/flash/games/game.jar              Flash 镜像中的游戏
/flash/j2me/game.jar               串口上传的游戏
/flash/j2me/rms/<游戏路径CRC32>/     无 SD 卡时的 RMS 存档
/flash/j2me/files/                  无 SD 卡时的 FileConnection 根目录
```

启动显示游戏列表。第 0 项是内置硬件演示，无需额外 JAR。点击游戏启动；PREV/NEXT 翻页，RES 切换 240×320、176×208、128×160、320×240，RESCAN 重新扫描文件。修改分辨率在下次启动游戏时生效，固定分辨率游戏应选择对应尺寸。

游戏画面位于中央 392×432 区域。首次启动默认 `MAX SIZE`，Pixel Perfect 关闭。`PIXEL ON` 按最大可容纳的整数倍居中显示：240×320 和 320×240 为 1 倍，176×208 和 128×160 为 2 倍。点击菜单右下角或游戏顶部的缩放按钮切换为 `MAX SIZE`，按比例放大至可用区域的最大尺寸，例如 240×320 显示为 324×432；再次点击恢复整数倍。游戏中可即时切换，不重启 MIDlet，设置写入 NVS，跨游戏和重启均保留；串口对应 `scale pixel` / `scale fit`。游戏界面底部不再显示分辨率文字；顶部保留游戏标题、MENU、缩放与 FPS 开关。界面使用深色底与青绿色强调色。左侧方向键显示四个对应朝向的三角形，中间按键为 FIRE，并提供 A/B/C/D 和左软键；右侧提供完整数字键盘、右软键与 CLEAR。按下与释放会立即更新对应按键的高亮状态，音量区显示当前数值。支持同时按多个触摸键；触摸游戏画面会转换为游戏坐标下的指针事件。VOL-/VOL+ 调节音量，顶部 MENU 结束游戏并返回列表，退出时调用 MIDlet 的 `destroyApp()` 保存 RMS。

右上角 **FPS OFF / FPS ON** 在列表和游戏中均可点击。首次启动默认关闭，开启后在游戏左上角每半秒显示实际画面更新次数/秒；重复提交但未改变画面的帧不计入。因此菜单静止时可以为 0，读数不等于 LCD 刷新率或 VM 调度帧率。关闭立即恢复被遮住的画面；选择会保存到 NVS，重启后恢复。

分辨率、音量、FPS、缩放模式、JIT 执行模式及代码内存选项会自动保存到 NVS，重启恢复；未改变的设置不重复写入。串口 `settings` 查看当前值，菜单下 `reboot` 可验证恢复。首次启动为 240×320、音量 70、FPS 关闭、最大尺寸缩放、JIT 关闭及 mapped 代码内存。

游戏的 Form 菜单和结果页支持焦点跟随滚动；超过一屏的单项文字可用上下键逐段查看，触摸选择使用滚动后的坐标。长文本按实际宽度换行，多行标签和图片参与高度计算，内容不会覆盖标题与软键栏。进度条不会再吞掉上下键。可用 `python3 tools/build_parallel_fixture.py --kind form` 生成长表单演示 JAR。

默认设备身份为核心自带的 `nokia6230` 档案。PCM/WAV、Espressif 组件解码的音频，以及官方 MIDI 库的合成输出，经重采样转换为 48 kHz 立体声，再送入 ES8389；`playTone` 使用板级音调输出。网络、蓝牙、相机及传感器后端没有在本适配中实现。游戏兼容性取决于上游 VM 和类库，支持的 API 不等于每款游戏都可正常游玩。

## Espressif 图片与音频组件

依赖由 `main/idf_component.yml` 声明，正常构建自动下载，版本与哈希记录在 `dependencies.lock`。适配使用上游公开的 `nj_port.install_codecs` 和 `audio_decode` 接口，代码放在主工程：

| 组件 | 接入用途 |
| --- | --- |
| [`espressif/esp_new_jpeg ^1.0.2`](https://components.espressif.com/components/espressif/esp_new_jpeg/versions/1.0.2/readme) | 优先解码 baseline JPEG；默认输出 MIDP 所需的 ARGB8888，原生调用可显式请求 RGB565/RGB888。官方解码器拒绝时尝试核心软件解码器，保留 CMYK 等兼容路径；不支持渐进式 JPEG |
| [`espressif/libpng ^1.6.59`](https://components.espressif.com/components/espressif/libpng/versions/1.6.59/readme) | PNG 解码为 ARGB8888，保留透明度，支持调色板、灰度、16 位输入及 Adam7；与核心一样容许游戏修改调色板后留下的旧 CRC |
| [`espressif/esp_audio_codec ^2.6.2`](https://components.espressif.com/components/espressif/esp_audio_codec/versions/2.6.2/readme) | 通过 Simple Decoder 接入 MP3、AAC/ADTS、AMR-NB/WB、FLAC、M4A，以及核心 PCM 读取器未处理的 WAV；接受 4–96 kHz、单/双声道、16 位 PCM 输出 |
| [`espressif/esp_midi ^1.1.0`](https://components.espressif.com/components/espressif/esp_midi/versions/1.1.0/readme) | 通过 `nj_port.midi_*` 接入 SMF 解析与合成；在 Core 1 输出 22,050 Hz 单声道 PCM，接入已有音频队列 |
| [`espressif/freetype ^2.14.3~1`](https://components.espressif.com/components/espressif/freetype/versions/2.14.3~1/readme) | 为六个 MIDP 字体槽提供 8 位抗锯齿字形；默认嵌入覆盖 GB2312 和 Latin-1 的 Noto 中文子集，保留核心位图字形作为缺字回退 |
| [`espressif/zlib ^1.3.2~1`](https://components.espressif.com/components/espressif/zlib/versions/1.3.2~1/readme) | 通过 `inflate_raw` 解压 JAR 内的类与资源，通过 `inflate` 解压 M3G 的 zlib 数据流；检查输入输出长度，工作缓冲使用 PSRAM |
| `espressif/esp_codec_dev ~1.5` | 继续负责 ES8389 硬件音频输出 |

PNG 的工作内存与输出图像使用 VM 分配回调，JPEG 输出和适配层缓冲也使用这些回调，退出时按核心约定回收；官方二进制库自行管理内部工作内存。图片上限为 4,194,304 像素，JPEG 输出满足 16 字节对齐要求。音频回调按核心约定解码两次：首次计算完整 PCM 大小，第二次写入 VM 提供的缓冲区；每次均关闭解码句柄。完整 PCM 上限为 Java 堆配置的一半，当前为 3 MiB，长 MP3/AAC 等音频仍受这个接口及可用 Java 堆限制。PCM 重采样沿用现有实现。

MIDI 使用独立播放任务进行流式合成，无需预先展开为完整 PCM。提供暂停/恢复、循环、音量、播放位置和重新合成快进式跳转；退出时等待播放任务结束后释放句柄。每个 MIDI 输入上限为 512 KiB，格式 0/1、最多 32 轨；无法打开的文件交给核心合成器。板级输出同时播放一个 MIDI Player，启动另一个会停止前一个的输出。SDK 未提供总时长查询，MMAPI 总时长返回未知；MIDIControl 单事件交由核心接管。

官方 SDK 只处理第一轨。平台先把格式 1 的各轨按绝对 tick 合并为格式 0，保留 tempo、控制事件和每轨 running status，避免《孤岛余生2》这类第一轨只有速度信息的游戏无声。另对 `esp_midi 1.1.0` 的 Note Off 入口增加保护：已经耗尽采样或没有对应活动音符时直接忽略，避免库在循环音符池中无限查找，造成播放中断和退出卡住；正常音符仍使用 SDK 的 RELEASE 路径。

实机验证同时修复了核心 `nj_lib_media.c` 的短 PCM 生命周期问题：设备在 `Player.start()` 中接受全部样本后，Player 仍需进入帧泵，才能关闭 PCM 并发出 `endOfMedia`。该通用修复已包含在子模块的 `main` 中。JPEG 的默认 ARGB8888 输出则在主工程中适配，避免 MIDP 按 Java `int[]` 复制 RGB565 数据。

默认使用随固件生成的小型波表音源，不占用独立音色分区。每个音符先按音高展开为 22,050 Hz、最长 4 秒的采样；官方混音器要求采样率完全相同，不会自行重采样或重复单个波形周期。将自己的 SF2 放在 `/sdcard/j2me/soundfont.sf2` 或 `/flash/j2me/soundfont.sf2` 后，下次创建 MIDI Player 时优先加载，SD 优先；SF2 样本按根音和实际音高转换为相同采样率。SF2 文件上限为 32 MiB，单个音符的源样本及转换后样本各上限 512 KiB，活动样本预算合计 2 MiB；实际还受存储容量限制。未提供、损坏或缺少相应乐器时使用内置波表。内置音源不是完整的 General MIDI 采样音色库，持续音超过 4 秒会自然结束，实际音色需实机试听验证。

串口 `MIDI_SIGNAL` 和 `MIDI_PCM` 输出合成后的非零样本数、峰值与平均绝对幅度，用于区分正常波形和静音计时；仅有播放位置增长或 codec 写入成功不足以证明 MIDI 出声。

2026-10-23 的组件烧录、媒体回归及 MIDI 静音修复见 [实机验证记录](docs/ESPRESSIF_MEDIA.md)。

FreeType 字体占约 2.1 MiB Flash，覆盖 GB2312 简体字、Big5 常用繁体字及符号，共 9,833 个字符；内存分配和有界字形缓存使用 PSRAM。小、中、大字号使用 12/15/19 像素 em，行高和基线按字形范围计算，并为粗体保留余量；测量与绘制使用相同字符宽度。默认字体来自 Noto Sans CJK SC，子集及许可见 [assets/fonts](assets/fonts/README.md)，正常构建无需 fontTools。[仙剑 2008 提示页修复](docs/PAL2008.md) 说明繁体字缺失和软键操作，[字号调整](docs/FONT_SIZE.md) 说明放大效果与验证。需要其他字体时用 `bash tools/idf.sh -B build-media -D KORVO_FONT_FILE=/absolute/path/to/font.ttf build` 指定。

VM 在 Core 0 上调用音频解码，默认关闭 `CONFIG_ESP_AUDIO_CODEC_S31_USE_ASM`，使用官方 S31 C 库；启用 PIE 汇编库须将解码工作移至 Core 1，当前代码会在编译时阻止错误配置。

[`gmf_core`](https://components.espressif.com/components/espressif/gmf_core/versions/1.1.0/readme) 提供流水线基础设施；后续需要统一音频流水线、音效或重采样时，可接入 [`gmf_audio`](https://components.espressif.com/components/espressif/gmf_audio/versions/1.1.0/readme)。[`gmf_ai_audio`](https://components.espressif.com/components/espressif/gmf_ai_audio/versions/1.1.1/readme) 面向语音唤醒、命令识别、AEC 等麦克风处理，当前游戏播放链路不引入这些依赖。

宿主适配测试使用真实 libpng、组件中同版本的 FreeType 源码、zlib 和 VM 注册表；ESP 专用 JPEG、音频、MIDI/SF2 二进制库使用 SDK 接口替身，以宿主线程模拟播放任务，验证缓冲区、生命周期及错误处理，不代表实机解码或音质验证。需先执行一次 IDF 构建下载组件，并安装宿主 libpng 开发文件：

```sh
python3 third_party/nano_j2me/tests/gen_lcdui_fixture.py
cmake -S tests/media -B build-media-host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-media-host -j 3
ctest --test-dir build-media-host --output-on-failure
```

实机媒体夹具覆盖 PNG 透明度/调色板、奇数尺寸 JPEG、CMYK 回退、WAV/MP3/AAC/AMR-NB/AMR-WB/MIDI，以及 MIDI 暂停、跳转和循环：

```sh
python3 tools/build_media_fixture.py
```

输出位于 `build-media/validation`，生成需要 JDK、ffmpeg 和 Pillow；`midi-audition.jar` 先播放 WAV 参考音，再循环播放 60 秒 C4/E4/G4 MIDI，便于扬声器试听。重新配置宿主测试后还会加入短 PCM 结束事件的端到端回归。`tools/media_board_session.py` 使用 IDF Python 环境，在同一串口连接内执行烧录与测试，并排除同一设备节点上的其他普通串口进程；USB 重连后需要重新建立连接。

AMR 夹具使用真实编码的有声素材，来源、信号指标和重新生成方法见 [测试素材说明](tests/media/fixtures/README.md)。构建 JAR 时会用 ffmpeg 独立解码，并检查这些应当有声的素材的峰值与 RMS，拒绝静音测试数据。`amr-audition.jar` 约 19 KiB，以 MP3 作人声对照，交替播放同一句话的 AMR-NB 和 AMR-WB，共四轮；普通构建使用已保存的素材，无需安装 AMR 编码库。

## 串口诊断与传送 JAR

使用 IDF Python 环境中的 Python，安装 `pyserial`；截图另需 Pillow。`tools/idf.sh monitor` 与诊断工具不能同时占用串口。

```sh
python tools/serial_console.py --port /dev/ttyUSB1 command help
python tools/serial_console.py --port /dev/ttyUSB1 command list
python tools/serial_console.py --port /dev/ttyUSB1 command 'res 240 320'
python tools/serial_console.py --port /dev/ttyUSB1 command 'run 1'
python tools/serial_console.py --port /dev/ttyUSB1 command 'key fire down'
python tools/serial_console.py --port /dev/ttyUSB1 command 'key fire up'
python tools/serial_console.py --port /dev/ttyUSB1 command stop
python tools/serial_console.py --port /dev/ttyUSB1 command stats
python tools/serial_console.py --port /dev/ttyUSB1 command storage
python tools/serial_console.py --port /dev/ttyUSB1 command 'fps on'
python tools/serial_console.py --port /dev/ttyUSB1 command 'fps off'
python tools/serial_console.py --port /dev/ttyUSB1 shot build/menu.png
python tools/serial_console.py --port /dev/ttyUSB1 shot build/game.png --game --fast
python tools/serial_console.py --port /dev/ttyUSB1 shot build/full-lcd.png --full --fast
python tools/serial_console.py --port /dev/ttyUSB1 upload /path/to/game.jar --fast
python tools/serial_console.py --port /dev/ttyUSB1 smoke
```

上传仅在游戏列表界面可用，目标是 `/flash/j2me/文件名.jar`，有 SD 卡时优先使用 `/sdcard/j2me/文件名.jar`。RMS 和 FileConnection 也优先使用 SD 卡，否则使用 Flash。通过分块 ACK 和 CRC32 校验完成后才将 `.part` 文件重命名；拒绝覆盖已有 JAR。支持 UTF-8 中文文件名；`--fast` 在传输期间切至 2000000，结束后恢复 115200，省略则使用控制台速率。`storage` 查看容量，上传前预留 256 KiB 给存档及文件系统。最大文件大小 8 MiB，实际可运行大小还受剩余存储、VM 堆和资源占用限制。

2026-11-10 已向当前设备的 Flash 追加《仙剑奇侠传完美版》《帝国时代2》《帝国时代III》《远古帝国2》《波斯王子：时之沙》《俄罗斯方块》《贪吃蛇》《魂斗罗4》《3D滚球》，合计约 1.88 MiB；来源及哈希见 [清单](docs/updates/2026-11-10/classic-games.json)。随后按用户指定写入《3D版田园物语》《模拟人生3》《合金弹头4（完美汉化）》《帝国时代3-亚洲王朝》《孢子生命起源》《仙剑奇侠传(2008)》《鲁宾逊漂流记》，合计 4,755,644 字节；为此移除了初始六款旧测试游戏的 JAR，保留 RMS 存档，原有九款经典游戏保留。实际安装、删除文件、大小和哈希见 [新游戏清单](docs/updates/2026-11-10-dualcore/requested-games.json)。三款原有 `[APP]` 3D 游戏也存储在固件 Flash 中。游戏 JAR 留在用户本机和设备上，GitHub 仅保存清单。

串口也可直接输入 `demo 90` / `run 1 300`，运行指定帧数后返回菜单；省略帧数持续运行。`pointer down 100 140`、`pointer move 110 140`、`pointer up 110 140` 可检查指针。`shot` 导出缩小的 LCD 帧缓冲，`shot game` 导出游戏原始 RGB565 缓冲（不含 FPS）；`shot full` 导出含按键和 FPS 的完整 800×480 LCD；这是内存画面采集，不是屏幕实拍。

当前 USB-UART 在 2 Mbaud 截图时仍偶发丢数据，工具会检测不完整行并回退至 115200；回退后的完整 LCD 截图约需 134 秒。截图会暂停 VM，测 FPS 时请直接读取屏幕叠加或使用 `command fps`，不要在采样期间截图。

内置 `KorvoDemo` 覆盖 Java 线程、动画、中文绘制、按键、指针、音调、RMS。`smoke` 检查三轮加载/运行/销毁后 PSRAM 是否稳定，并检查串口输入是否到达 MIDlet。演示 JAR 已包含在仓库，正常编译无需 JDK；修改演示后可用 JDK 重新生成：

```sh
bash tools/build_demo.sh
```

## 核心升级和代码位置

```sh
git -C third_party/nano_j2me status --short   # 应无输出
git submodule update --remote -- third_party/nano_j2me
bash tools/idf.sh build
python tools/serial_console.py --port /dev/ttyUSB1 smoke
git add third_party/nano_j2me
```

本地核心优化已合入 nano-j2me 的 `main`；递归克隆按主仓库记录的提交恢复，`--remote` 获取该分支的后续更新。升级前保存主工程改动。升级后重新烧录测试；上游接口和源码清单变化可能需要更新主工程的组件集成，不能假定任意新提交都已验证。

| 路径 | 用途 |
| --- | --- |
| `components/nano_j2me/CMakeLists.txt` | 外部编译核心、嵌入式内存参数；不使用上游通用 SPI 屏驱动 |
| `components/esp32_s31_korvo1` | 原板级 YAML 和 GT1151 初始化入口 |
| `components/gen_bmgr_codes` | Board Manager 生成的设备、外设配置 |
| `main/korvo_board.c` | RGB 帧缓冲、触摸、SD、ES8389 和异步音频队列 |
| `main/korvo_port.c` | `nj_port` 回调、PSRAM、RGB565 缩放、文件与时钟 |
| `main/korvo_image.c`、`main/korvo_media.c` | Espressif PNG/JPEG 图片提供器与音频解码回调 |
| `main/korvo_midi.c`、`main/korvo_soundfont.c` | 官方 MIDI 库、播放任务、内置波表与 SF2 加载 |
| `main/korvo_font.c`、`main/korvo_inflate.c` | FreeType 字形与 zlib 解压适配 |
| `main/main.c` | 游戏列表、虚拟按键、串口命令、VM 生命周期 |
| `tests/midlet/KorvoDemo.java` | 内置演示源代码 |
| `tools/serial_console.py` | 上传、截图与板上回归测试 |

核心的 MIT 许可见 `third_party/nano_j2me/LICENSE`。显示及音频板级参数、5×7 界面字体、流式重采样函数沿用用户提供的参考工程。

实机结果和兼容性边界见 [验证记录](docs/VALIDATION.md)。

## 性能与音频

显示端使用坐标表和内部 RAM 行缓冲进行整数倍最近邻缩放，保持 RGB565 像素结果；跳过重复画面，仅同步变化区域。RGB LCD 由 GDMA 持续扫描，使用两个 PSRAM 帧缓冲和 64 字节突发；提交后等待驱动的帧缓冲释放事件，再用 PPA/DMA 把脏矩形同步到释放的缓冲区，避免改写正在扫描的画面。PPA 无法使用时回退到 CPU 复制；不会在等待超时后复用扫描中的缓冲区。`display` 查看提交、完成、DMA/CPU 同步和超时计数，菜单下可用 `displaytest 90` 检查 90 次局部更新后两块完整帧缓冲的一致性。解释器热循环由主工程链接脚本放入 IRAM，子模块无需添加任何属性或补丁。内部 RAM 常用小块预算为 96 KiB，栈尺寸分配可用至 160 KiB，分配前检查驱动余量；失败自动使用 PSRAM。

大块 3D 像素绘制分到两个核心，大块不透明填充可以使用 Core 1 的 128 位 SIMD；Java VM 和 GC 留在 Core 0。屏幕脏区同步优先直接使用二维 DMA，保留 PPA/CPU 回退；PPA 的整数缩放会先做像素等价检查。`parallel on/off`、`simd on/off`、`ppafill on/off` 在菜单下选择加速路径，设置写入 NVS。适用范围和实测对照见 [双核与硬件加速记录](docs/DUALCORE.md)。

音频在第二个 CPU 核心上异步输出，缓存 codec 音量设置；PCM 按实际队列容量完整分包，允许 MIDI 启动时的首批突发数据，后续补充以约 160 毫秒为水位。播放结束排空队列，退出游戏中断输出；空缓冲用静音填充，音调使用带短淡入淡出的正弦波。重采样消除每个声道的 64 位除法，结果与原算法逐样本一致。

初始核心的实测数据见 [性能记录](docs/PERFORMANCE.md)，当前 JIT 分支见 [缓存修复记录](third_party/nano_j2me/docs/JIT_CACHE_LOOKUP.md)，历史 2D/3D 测试见 [c6b1595 记录](docs/JIT_C6B1595.md)，`fe974fc` 数据见 [旧版记录](docs/LATEST_3D.md)，此前 FPS 开关验证见 [升级与游戏测试记录](docs/UPSTREAM_GAMES.md)。`bench INDEX FRAMES [keys]` 使用独立临时 RMS 目录，不改正常游戏存档；按键脚本名称采用上游格式，例如 `30:fire,90:soft1,150:5`。日志中的 `PERF` 区分画面提交次数与实际更新次数，并检查最终缩放画面和内存回收。

本次更新后，`bench` 结束会清理本次创建的临时 RMS；正常存档和历史诊断目录均保留。帧脚本支持显式保持按键，例如 `30:right:down,90:right:up`，供轮询输入的游戏使用。

```sh
python3 tools/build_perf_fixtures.py
python3 tools/serial_console.py upload build/performance/fixtures/perf-cpu.jar
python3 tools/serial_console.py upload build/performance/fixtures/perf-render2.jar
python3 tools/serial_console.py upload build/performance/fixtures/perf-audio.jar
python3 tools/benchmark.py build/performance/results.json
```

上传拒绝覆盖已存在的同名 JAR；重复测试可直接运行最后一条命令。CMake 参数 `KORVO_FAST_RENDER`、`KORVO_INTERNAL_HOT_DATA`、`KORVO_AUDIO_IMPROVED` 默认均为 `ON`，可用 `tools/idf.sh -D参数=OFF build` 单独关闭以排查。解释器使用 switch 分发，实验选项 `KORVO_THREADED_DISPATCH` 保持 `OFF`。当前上游新增的融合指令在本板固定计算测试中较慢，`KORVO_SUPERINSTR` 默认 `OFF`；可单独开启后测量。

### RV32 JIT

本轮执行内存修复、缓存重构及 240×320 对照数据见 [优化记录](docs/JIT_EXEC_POOL_20261026.txt)。

当前子模块包含上游的 RV32 JIT，主工程通过 `KORVO_JIT` 编译选项接入。编译支持默认开启，首次启动时关闭；在菜单发送 `jit on` 可对具体游戏启用。不同游戏中解释器、原生图形与热点管理的比例不同，不能把计算内核的提速直接换算成游戏帧率。旧版测量保留在 [历史测试记录](docs/JIT_C6B1595.md)。

适配层默认使用独立的 256 KiB 可执行映射池：数据分配只负责持有物理页，机器码写入和调用使用可执行映射。提交时先写回数据缓存，再失效对应指令缓存并执行指令同步；启动自检验证同一地址先返回 42、改写后返回 43。每块代码独占完整缓存行，分配表与机器码分离，退出 VM 后回收映射和整个池；关闭 JIT 或仅探测热点时不会创建代码池。保留 `jit memory internal` 原有 32 KiB 路径供受控对照，`jit memory mapped` 恢复默认路径，均须在菜单切换。

本工程的 JIT 配置为 1024 个代码槽、4 路关联、1024 个方法记录、256 KiB 热点状态空间，普通块至少包含 8 条字节码；有调度预算限制的循环仍可单独编译。对应 CMake 参数为 `KORVO_JIT_CACHE_ENTRIES`、`KORVO_JIT_CACHE_WAYS`、`KORVO_JIT_METHOD_ENTRIES`、`KORVO_JIT_METHOD_ARENA_BYTES`、`KORVO_JIT_MIN_BLOCK_INSNS`。这些是本工程配置，上游默认容量保持不变。方法切换使用索引查找，较大的槽号和映射偏移使用相应位宽，避免扩大容量后截断。完整开启时还会占用约 383 KiB 元数据；内存不足会回退到解释执行。

SDK 的 `CONFIG_ESP_SYSTEM_MEMPROT=y` 禁止从堆分配可执行 RAM，本工程为 JIT 设置为 `n`，因此不再启用 SDK 默认的代码只读/数据不可执行分区保护。若需要恢复该保护，应同时关闭 `KORVO_JIT` 并重启设备。

```sh
python tools/serial_console.py --port /dev/ttyUSB1 command 'jit on'   # 在菜单选择，下次游戏生效
python tools/serial_console.py --port /dev/ttyUSB1 command 'jit off'
python tools/serial_console.py --port /dev/ttyUSB1 command 'jit probe' # 只探测热点，不生成/执行机器码
python tools/serial_console.py --port /dev/ttyUSB1 command 'jit memory mapped' # 默认可执行映射
python tools/serial_console.py --port /dev/ttyUSB1 command 'jit memory internal' # 原内存路径，用于对照
python tools/benchmark.py build/jit-on.json --jit on
python tools/benchmark.py build/jit-off.json --jit off
python tools/test_games.py build/jit-games.json --jit on
```

`JIT_BEGIN active=1` 仅表示已安装 JIT；`JIT_STATS executed` 和 `bytecodes` 才证明执行过生成的机器码。`JIT_MEMORY live=0` 与 `JIT_POOL released=1 live=0` 验证退出后代码内存及映射全部释放。`JIT_POOL code_hash` 可比较固定内核生成的机器码，`commit_us` 记录缓存发布耗时。运行器的创建/销毁通过主工程的链接包装调用公开 JIT API，平台接入不依赖运行器补丁；通用 JIT 修改单独保存在优化分支。实测收益与限制见 [升级与游戏测试记录](docs/UPSTREAM_GAMES.md)，源码分析及上游改动建议见 [JIT 修改建议](docs/JIT_REVIEW.md)。

### 无 SD 卡时追加大型 3D 测试游戏

现有 FAT 分区空间不足以容纳三款 3D 游戏时，可选择将原始 JAR 放进应用分区，保留 FAT 中的游戏和存档。此选项默认关闭，不把商业游戏纳入主仓库：

```sh
python tools/prepare_test_games.py /home/lucinhu/Documents/game-roms/jar --output build/test-games
bash tools/idf.sh -DKORVO_TEST_GAME_DIR="$PWD/build/test-games" build
bash tools/idf.sh -p /dev/ttyUSB1 -b 2000000 app-flash
python tools/benchmark.py build/kernels-off.json --only kernels --jit off
python tools/benchmark.py build/kernels-probe.json --only kernels --jit probe
python tools/benchmark.py build/kernels-on.json --only kernels --jit on
```

准备工具为《鬼泣3D》《浴火银河》《3D摩托赛车》建立链接，并编译调用上游原始 `JitPerf` 的 MIDlet 包装。菜单末尾追加带 `[APP]` 的三款游戏和 `JIT FIXED KERNELS`；游戏资源只读，正常运行仍使用独立的可写 RMS。当前 IDF 配置将这些只读资源映射到 PSRAM，会占用约 2 MiB 的额外常驻空间。关闭时使用 `-DKORVO_TEST_GAME_DIR=` 重新构建。

本适配显式管理 `nj_port` 生命周期，在 `nj_run()` 返回后调用 `shutdown`。内置 JAR 通过虚拟只读文件接入，使运行器管理其读取缓冲区；规避当前上游 `jar_data` 借用路径在退出时仍调用 `free()` 的问题，不需要修改核心。

代码池、音频重采样及显示缓冲的独立回归（地址与未定义行为检测）：

```sh
cmake -S tests -B build-adapter-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build-adapter-tests
ctest --test-dir build-adapter-tests --output-on-failure
```
