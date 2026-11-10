# 双核与硬件加速（2026-11-10）

2026-11-11 的 LCD 等待与重复提交优化见 [显示优化记录](FRAME_PIPELINE.md)。

后续核心 `8689b5b` 与板级 PPA 边缘填充优化见 [继续优化记录](REFINEMENT.md)：相同填充测试从 4,048.5 ms 降到 2,819.5 ms，控件界面也接入填充加速。下方性能表保留此前版本的原始对照。

Java VM、几何变换和 GC 运行在 Core 0；大三角形的像素绘制由 Core 0 和 Core 1 同时完成。Core 1 的常驻渲染任务优先级为 3，低于 MIDI 的 4 和音频输出的 5，栈为 8 KiB。每个三角形完成后才开始下一个，保持透明混合和深度测试的先后顺序。Java 对象访问、分配和 GC 不进入工作线程。

核心 `27432df` 为 `nj_port` 末尾添加可选的同步 `parallel_for` 回调，`6a13581` 添加 `gfx_fill_rect` 回调。没有实现这些回调的移植继续串行运行；使用新版头文件后需要重新编译移植。板级任务、SIMD 汇编、PPA、二维 DMA、FreeRTOS 信号量和设置保存在主仓库。两个仓库的修改均在 `main`。

可见三角形的面积及裁剪后包围盒达到阈值时，光栅器分成上下两段；小三角形继续串行，避免调度开销。两段沿用原有浮点累加顺序，底段也先推进到中点，以保持像素和深度结果一致。纹理与目标图像存储重叠时使用串行路径。工作线程只借用当前调用的数据，返回前同步完成，不在游戏退出后保留 VM 引用。

默认开启双核绘制。菜单状态下的串口命令：

```text
parallel          # 状态、工作核心、任务数量、耗时和剩余栈
parallel off      # 后续游戏使用串行光栅器
parallel on       # 后续游戏使用双核光栅器
paralleltest 300  # PSRAM 边界、完成同步和哨兵检查
simd on/off       # Core 1 使用 SIMD / 普通 C 填充
simdtest          # 128 轮，两种格式、非对齐起点和行尾保护
ppafill on/off    # PPA 大块不透明填充
acceltest         # 复制性能与 PPA 整数缩放等价检查
settings          # 包含 parallel、simd、ppafill
```

三个加速开关默认开启，使用独立 NVS 键保存，重启后恢复；原有分辨率、缩放、FPS、音量和 JIT 设置格式保持兼容。无法创建工作任务时自动使用串行路径。

不透明的大矩形优先使用 PPA FILL。当前启动验证 RGB565/ARGB8888、Alpha 字节、五种起点对齐、整块及局部填充，以及较大图像的边缘和末行，共 88 种组合，并检查整块分配空间的哨兵。VM 图像数据可能接在对象头后面，描述符基址向下对齐后跳过前缀；PPA 只写裁剪区域。较大的右边窄条通过下一个描述符行的第二个窗口填充，小边缘及无法纳入描述符的末行由 CPU 补齐，保留对象头、行尾和周围像素。完成后才返回 VM。PPA 不可用或关闭时，大块填充由两个核心分段；Core 1 用 `esp.vldbc.32.ip` 和 `esp.vst.128.ip` 做 128 位填充，非对齐头尾使用普通写入。Alpha 混合、XOR、小矩形和不支持的格式保留普通路径。

RGB GDMA 双缓冲与释放同步继续保留。脏区同步优先使用公开的 DMA2D 同格式窗口复制接口，失败时使用 PPA SRM，再回退 CPU；一旦 DMA 已接受请求，等待完成后才复用缓冲，不会超时后直接写入仍由 DMA 使用的区域。Pixel Perfect 的 1 倍显示也使用二维 DMA 窗口复制。

PPA 的 2 倍 SRM 缩放在当前芯片/IDF 上未通过最近邻逐像素检查，因此没有启用；整数倍和 MAX SIZE 的像素结果继续保持。不能仅因为外设支持缩放就假定它与原有采样规则一致。

## 测量方法

同一固件内切换 `parallel off/on`，统一使用 240×320、MAX SIZE、关闭 FPS 叠加和 JIT、10 MiB Java 堆、独立临时 RMS。测试期间没有输入，截图在计时结束后进行。固件及最终数据见 [结构化记录](updates/2026-11-10-dualcore/summary.json)。

固定工作量测试包含纯色、渐变色、最近邻纹理及双线性纹理加透明混合四种路径，每项 36 帧、144 次绘制。计时不包含 Java 读取像素并计算校验值。按 `off/on/on/off` 顺序测试，比较每项 Java 校验值、最终原生画面和 LCD CRC，同时检查退出后的内存回收。

```sh
python3 tools/build_parallel_fixture.py
python3 tools/benchmark_parallel.py build/dualcore/results.json --port /dev/ttyUSB1
```

工具会上传缺少的测试 JAR，在测试后恢复原有设置。设备 Flash 中保留了 `perf-parallel.jar`，测试不包含商业游戏资源。

## 2D 填充结果

`FillBench` 对 240×320 ARGB8888 图像执行 2,880 次大矩形填充，包括全尺寸及裁剪/平移两项。每种模式测两次，顺序为串行、双核 C、双核 SIMD、PPA、PPA、双核 SIMD、双核 C、串行。下表是两项计时之和的均值；不含读取像素、计算校验值及最后显示的时间。

| 路径 | 耗时 | 相对原始路径吞吐量 |
| --- | ---: | ---: |
| 原始单核 | 9,746 ms | 1.00× |
| 双核普通 C | 8,473.5 ms | 1.15× |
| Core 1 SIMD + Core 0 C | 8,463.5 ms | 1.15× |
| PPA 主体填充 + CPU 边界 | 4,042 ms | 2.41× |

SIMD 路径确实在 Core 1 执行，每轮记录 24,778,080 个 128 位块；相对双核 C 的额外墙钟收益约 0.12%，该 PSRAM 负载主要受访存带宽限制。PPA 每轮处理 194,025,600 个像素，减小了 CPU 访存开销，耗时下降约 58.5%。这只是矩形填充的收益，不代表所有游戏帧率提高 2.41 倍。

八轮的两项 Java 像素校验值、最终原生画面和 LCD CRC 完全一致；退出 PSRAM 差值均为 0。数据见 [fill-final.json](updates/2026-11-10-dualcore/fill-final.json)。复现：

```sh
python3 tools/build_parallel_fixture.py --kind fill
python3 tools/benchmark_simd.py build/dualcore/fill-results.json \
  --modes serial dual simd ppa ppa simd dual serial
```

同机复制 176×208 RGB565 像素、重复 20 次，CPU 28.580 ms、PPA SRM 19.152 ms、直接 DMA2D 13.085 ms，全部像素一致。直接 DMA2D 相比原 PPA 复制路径耗时下降约 31.7%；这也是单项复制时间，不是整个显示帧的时间。启动测试数据见 [ppa-persistence.json](updates/2026-11-10-dualcore/ppa-persistence.json)。

## 3D 与 Java 结果

大三角形固定工作量按 `off/on/on/off` 各测两轮，均值如下。四项 Java 校验值以及最终原生 `c4e65f9a`、LCD `658aacbb` CRC 一致；四次退出 PSRAM 差值均为 0。

| M3G 绘制路径 | 单核 | 双核 | 吞吐量 |
| --- | ---: | ---: | ---: |
| 纯色 | 4,741 ms | 3,240.5 ms | 1.46× |
| 渐变色 | 6,591.5 ms | 4,446.5 ms | 1.48× |
| 最近邻纹理 | 7,623.5 ms | 5,043 ms | 1.51× |
| 双线性纹理及透明混合 | 14,724.5 ms | 9,796.5 ms | 1.50× |

数据见 [m3g-final.json](updates/2026-11-10-dualcore/m3g-final.json)。这组对照只切换双核分段开关，其他设置保持相同。

JBenchmark3D 3.1.0 在同一最终固件上比较本次新增的双核、SIMD、PPA 填充三项关闭/开启，每组使用全新临时 RMS：

| 项目 | 关闭 | 开启 | 变化 |
| --- | ---: | ---: | ---: |
| HQ | 112 | 141 | +25.9% |
| LQ | 172 | 209 | +21.5% |
| Triangles/s | 17,573 | 17,542 | −0.2% |
| KTexels/s | 2,922 | 3,286 | +12.5% |

截图为 [关闭](updates/2026-11-10-dualcore/jbenchmark3d-final-off.png)、[开启](updates/2026-11-10-dualcore/jbenchmark3d-final-on.png)，运行及退出记录见 [jbenchmark3d-final.json](updates/2026-11-10-dualcore/jbenchmark3d-final.json)。几何处理仍为单核，小三角形吞吐量基本持平。

五项固定 Java 整数计算各测两轮，全部校验值一致；五项计时合计关闭时均值 892 ms、开启时 895.5 ms，约 0.4% 差异。加速没有改变解释器执行逻辑，不能把像素填充的提升套用到纯 Java 计算。原始数据见 [关闭](updates/2026-11-10-dualcore/kernels-off.json)、[开启](updates/2026-11-10-dualcore/kernels-on.json)。

最终固件 7,941,840 字节，SHA-256 `68bd7b0d24b0ec9732cb02fd19b93597deeaa553c0b9968cafe91c817543741d`。仅更新应用区并校验，没有重写游戏 FAT 分区。

## 正确性与范围

ASan/UBSan 主机测试通过 M3G、渲染目标、深度比较及真实 pthread 并发回归。并发测试覆盖 RGB565、RGB888、ARGB8888、行尾填充、裁剪、视口、透明测试、混合和深度相等情形：234 次实际分派，117,504,000 字节颜色及深度数据与串行结果完全一致；反馈纹理另检查不会并发分派。

新增的填充回归完成 90 个加速矩形，并与串行完整缓冲区比较，包含裁剪、平移、拒绝加速、透明混合、XOR 和行尾哨兵。板级模拟测试延迟 DMA/扫描完成事件，覆盖缓冲所有权、PPA 与 CPU 回退、PPA 边界补齐。板上 SIMD 压力测试为 128 轮、110,720 个 128 位块，所有像素及哨兵差异为 0。

最终设备通过 90 次显示更新、三个分辨率的缩放校验、三次演示加载/退出，以及 FIRE、数字键和指针检查。3D摩托和用户提供的合金弹头4分别采样约 300 帧，退出 `rc=0`、内存差值和缩放错误均为 0；全新测试 RMS 的不存在记录查询由游戏捕获。这里只验证启动及按键采样，没有据此宣称完整游玩或音频并发验证。见 [最终设备记录](updates/2026-11-10-dualcore/device-final.json)。设备回到菜单，恢复 FPS 开启、MAX SIZE、240×320、音量 40，双核/SIMD/PPA 填充默认开启且已保存。

参考用户提供的 ESP32-S31 技术参考手册预发布 v0.1，第 17 章 2D-DMA、第 18 章 PPA；SIMD 核心限制与任务上下文处理同时核对本地 ESP-IDF 的 RISC-V/FreeRTOS 实现。移植使用当前 IDF 的公开 PPA 与 DMA2D 接口，不依赖手写外设寄存器。

性能收益主要来自较大三角形的像素绘制。Java 逻辑、几何处理和小三角形没有因此变成双核；2D 游戏的收益不能由此推算，也不能把单项吞吐量提升当作所有游戏帧率的提升。JBenchmark HD 的既有场景兼容性问题见 [原有记录](JBENCHMARK.md)，本次没有改变其得分判定。

清理了本轮临时主机/适配器构建、手册提取与预览、临时脚本、测试输出和 Python 缓存。保留当前固件构建、CMake 外部 JAR 输入、三份可复用基准 JAR 和有效结果；删除路径与字节数见 [清理记录](updates/2026-11-10-dualcore/cleanup.json)。
