# 鲁宾逊漂流记走动时地图闪烁（2026-11-11）

使用用户提供的 `/home/lucinhu/Documents/game-roms/jar/鲁宾逊漂流记汉化版_702.jar`，376,293 字节，原包不作修改。SHA-256 见 [本轮汇总](updates/2026-11-11-robinson/summary.json)。

游戏的 `data.k` 继承 GameCanvas，在工作线程中取得 Graphics，完成绘制后调用 `flushGraphics()`。ESP32 上一次地图绘制可能跨越多个 VM 调度帧；Linux 普通执行速度较快，不容易出现相同的中断时机。

核心存在两个会提前显示半成品的路径：

1. 几帧没有提交后，自适应补画直接复制仍在绘制的 GameCanvas 缓冲。
2. 按下、松开方向键或触摸时，运行时自动要求重绘，再次复制这个缓冲。

现在通过现有的逐 VM 状态区分同步绘制与 GameCanvas 主动提交。已经使用 `flushGraphics()` 的当前 GameCanvas，在下一次完整提交前继续显示已提交的画面。按键状态、按键与触摸回调仍照常递送；游戏显式要求的 repaint/serviceRepaints 继续执行。普通 Canvas、没有主动提交的旧 GameCanvas 及界面切换保留原有行为。LCD 双缓冲、DMA2D、PPA、缩放和 JAR 本身保持原样。

原始静默绘制回归在旧实现上出现 6 项失败；只修复超时补画后，输入回归仍出现 21 项失败。完整修复后的公共 API 测试在三轮 VM 生命周期中通过 **9,651 项断言**，覆盖半成品等待 310 个调度帧、计数饱和、按键/触摸、显式重绘、画布切换和旧行为回归。相关八项 ASan/UBSan 测试全部通过。独立 Java 分段绘制夹具捕获 120 帧，混色半帧为 **0**。

本轮同时同步远程的 ImageItem 空替代文字、Power 键和可选堆扩展更新。板级仍使用原来的固定 10 MiB Java 堆。为适配远程新增的 `nj_vm_create_with_heap_limit` 入口，JIT 设置钩子也覆盖了这个入口；保存的 JIT 开关和 probe 模式沿用原有策略。

测试数据和实机截图见 [记录目录](updates/2026-11-11-robinson)。RMS 存档保留，测试夹具使用独立临时 RMS，完成后删除测试 JAR。

实机分段绘制测试在方向键和触摸输入穿插时采样 8 帧，每帧均为完整的 76,800 像素红色或蓝色，没有两帧混合；退出内存差值为 0，90 次 LCD 缓冲一致性检查差异与超时为 0。实际游戏使用独立测试存档进入地图，按数字方向键完成两轮四方向移动，8 份地图采样完整，退出 `rc=0`、`delta=0`、`scale_mismatch=0`。见 [颜色测试](updates/2026-11-11-robinson/board-flush.json) 和 [地图测试](updates/2026-11-11-robinson/game-walk.json)。

核心修复已合入 `main` 的 `a2e5ba8`。最终应用为 7,946,304 字节，烧录 SHA-256 `7d39c8a7585aed189649f870486a79b113712267d472cc8367359d639bffc2de`。最终固件再次通过板上分段绘制、输入和显示一致性检查；原来的游戏已重新打开，240×320、MAX SIZE、FPS 关闭、音量 40、JIT 关闭、双核/SIMD/PPA 开启均保留，见 [最终设备状态](updates/2026-11-11-robinson/device-final.json)。地图采样版本与最终版本包含相同修复，具体构建身份单独记录在汇总中。

本轮清理 272.61 MiB、154 个临时文件，保留当前固件构建、必要夹具、依赖、字体及原有 FAT 恢复文件。见 [清理明细](updates/2026-11-11-robinson/cleanup.json)。
