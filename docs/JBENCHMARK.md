# JBenchmark 实机记录（2026-11-10）

ESP32-S31-Korvo-1 rev0，CPU 320 MHz、PSRAM 250 MHz；nano-j2me 0.5.6，核心提交 `a028d18`。统一使用 240×320、MAX SIZE 按比例放大到 324×432、关闭 JIT、关闭 FPS 叠加、nokia6230 身份、10 MiB Java 堆和独立的全新测试 RMS。3D 为软件渲染，LCD 使用 RGB GDMA 双缓冲和 PPA DMA 脏区同步。

固件为 7,919,712 字节，SHA-256：`cae781247abfec1eb9a6eb20ff2f10a99d7025202cb239e2be5434abc5751f45`。通过 `/dev/ttyUSB1` 更新应用区并校验，游戏分区未重写。

| 基准 | 版本 | 实机结果 |
| --- | --- | --- |
| JBenchmark 1 | 1.1.1 | 1321 |
| JBenchmark 2 | 2.1.1 | 86 |
| JBenchmark 3D | 3.1.0 | HQ 115 / LQ 174 |
| JBenchmark HD | 4.0.3 | 部分完成：平滑 26,498/s，纹理 25,159/s |

JBenchmark 1：Text 404、2D Shapes 263、3D Shapes 176、Fill Rate 141、Animation 337；前两轮为 1322、1323。
JBenchmark 3D：18,195 triangles/s，2,970 KTexels/s。
JBenchmark 2：Image manipulation 36、Text 237、Sprites 435、3D Transform 6、User interface 204。

HD 的双线性过滤为 Success（91%），三线性过滤 Failed/Not supported，Z-buffer 深度检查 Failure（7 bit）。主 Gaming 场景失败。当前通用 Form 导航无法滚动到其余数据行，不推测未读取的 Fill rate 或 Gaming 得分。前一轮平滑/纹理分别为 26,500/s、25,167/s，双线性为 92%。

完整结构化数据见 [jbenchmark-results.json](updates/2026-11-10/jbenchmark-results.json)，包版本、来源及 SHA-256 见 [jbenchmark-packages.json](updates/2026-11-10/jbenchmark-packages.json)。你提供的 `/home/lucinhu/Documents/game-roms/jar/JBenchmark3D.jar` 与设备安装包逐字节一致。JBenchmark 1 也与本机包相同；JBenchmark 2 与本机包仅 Manifest 显示名称中的空格有差别，所有类和资源相同。HD 使用清单中记录的镜像包。所有 JAR 保持原样，仅存于本机和设备 Flash，没有提交到 GitHub。

本次发现并修复了三类问题：Item 合法布局标志被拒绝；Gauge 错误禁止程序更新非交互进度条；M3G Loader 漏计 SkinnedMesh 引用、Graphics3D 缺失 numTextureUnits 属性。修复与回归测试已进入核心 main。另修复了空闲计数达到 255 后不再重绘的帧泵问题；回归测试在旧代码上失败、修复后通过。初次失败/停留在初始化的画面未作为有效得分，最终表格仅采用重跑完成后的结果。软件模拟器的分数需要结合图形与 API 兼容程度理解，不能据此等同于原生手机 GPU 性能。

复现时先在菜单设置上述选项，通过 `settings` 确认，再用 `list` 查看当前索引。本机四个程序依次是 16、17、18、19：

```sh
python3 tools/serial_console.py --port /dev/ttyUSB1 command list
python3 tools/benchmark_jbenchmark.py --port /dev/ttyUSB1 \
  --indices 16 17 18 19 --output build/jbenchmark
```

脚本依次启动、等待计时结束、截取结果并退出，不发送分数到网络。截图会暂停 VM，因此只在等待完整测试时间后截取；仍需查看结果图片，确认不是等待或报错页面。截图来自原生 VM 帧缓冲，不是 LCD 实拍。原始日志仅保留本机，构建和测试产物受 .gitignore 排除。

HD 场景兼容性诊断：map.m3g 返回 4 个根对象，World 位于首项，后面有 3 个 Group。该包的 i.a(int) 回退到最后一个根对象上调用 find，无法取得 ID 100 的 Mesh，i.m() 随后抛出 NullPointerException。直接在 World 上 find(100) 可找到模型。Mipmap 与双纹理单元检查则因当前能力不支持而跳过；这些失败均不会被记为通过。


最终设备回归：90 次局部更新，两块完整帧缓冲差异为 0；DMA 提交/完成/复制均 3545，CPU 回退及超时均 0。三轮演示退出后 PSRAM 均 15,216,240 字节；按键与指针通过；3D摩托 180 帧退出 rc=0、scale_mismatch=0、PSRAM delta=0。设备回到菜单，保留 MAX SIZE 和用户最后开启的 FPS，设置存入 NVS。

按用户要求，HD 原包另复制到 `/home/lucinhu/Documents/game-roms/jar/JBenchmarkHD.jar`，351,431 字节，SHA-256 `58cd27a6fe6fc539fabb3bb0f593cd45ce236e3e242e5d64e7f01b1db07f4890`。
