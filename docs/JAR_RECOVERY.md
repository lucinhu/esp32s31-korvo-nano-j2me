# JAR 打开后立即退回菜单（2026-11-11）

故障可复现：菜单前面的经典游戏及 JBenchmark 启动时输出 `cannot read the JAR`，`RUN_END rc=2 presents=0`。同一固件下仙剑 2008 和鲁宾逊能读取，说明需要检查存储。重启后故障仍在。

用 esptool 的 ESP32-S31 **stub v2** 读取当前 FAT 分区，保存为本地 `build/recovery/j2me-20261111-launch-before.bin`，并在设备停止运行时核对整块分区摘要一致。离线检查发现 13 份 JAR 的目录大小仍正确，但簇链中断；另有一份旧仙剑缓存 `RMS/B1FB304C/DOWN.RMS` 同样中断。损坏发生的具体时刻和上游原因尚未确定，本次没有把它归因于字号改动。

恢复按原始 JAR 的每个数据块寻找现存内容，只有数据完全匹配且未被其他文件占用的簇才能接回。恢复后全部 25 份 JAR 的 ZIP 成员 CRC 通过。JBenchmark 2 安装包与用户目录版本只在 Manifest 的显示名称空格上有差别；保留设备原包，所有 class 和资源与用户版本一致。旧仙剑缓存的 6 条 NJRS 记录、记录边界及 NJMT 尾部均完整，接回原有 4 个连续簇，缓存内容未修改。

仅写回物理地址 `0x811000` 起的 8 KiB（两张 FAT 表）。整块 8,323,072 字节分区再次与修复镜像做摘要核对成功：目录、JAR、存档和其余数据字节保持原样。没有格式化，没有重新上传或替换 JAR，也没有重烧应用；当前仍是 `a8aea21` 的大字体固件。校验与地址关系只适用于本轮冻结状态的磨损均衡映射，不能将这个物理地址直接套用到其他设备快照。

随后用正常 RMS 路径逐项启动 25 份文件 JAR，每项 30 帧，全部 `rc=0`，没有读取失败。这个检查覆盖加载/启动，不代表完整通关或所有兼容性功能。另对俄罗斯方块和 JBenchmark 1 做不限帧启动采样，未再立即退出；俄罗斯方块的“是否开启音效”需要用左右软键选择。六项离线恢复单元测试验证 FAT12 相邻半字节、双表一致性、拒绝歧义簇和保护其他存档。

进一步确认俄罗斯方块音效选项后，发现独立的 `Stack protection fault` 重启，开、关音效均出现过：故障位于 SDK `_interrupt_handler`，SP 在 1,536 字节 ISR 栈上界处；这不属于原来 `cannot read the JAR` 的加载错误，目前尚未修复。保留实际日志，没有把启动冒烟测试当作全部游戏兼容性已经完成，也没有关闭栈保护来掩盖重启。

完成上述启动、存档写入与重启后，再读取 FAT 表复核，13 份已恢复 JAR 的全部簇链仍与修复结果一致，两张 FAT 表也一致，见 [复查](updates/2026-11-11-jar-launch/post-startup-validation.json)。

设备最终保留 240×320、音量 40、MAX SIZE、FPS/JIT 关闭、双核/SIMD/PPA 开启，停在启动菜单。清理约 31.7 MiB 本地临时镜像、脚本、重复截图和缓存；旧 ROM 方式的恢复文件未通过 FAT/ZIP 结构检查，未用于恢复，已由通过设备摘要校验的 stub v2 前后镜像取代。有效前后镜像、当前固件和证据保留，详见 [最终状态](updates/2026-11-11-jar-launch/final-device.json) 和 [清理记录](updates/2026-11-11-jar-launch/cleanup.json)。

记录见 [汇总](updates/2026-11-11-jar-launch/summary.json)、[13 份 JAR 恢复](updates/2026-11-11-jar-launch/jar-recovery.json)、[缓存恢复](updates/2026-11-11-jar-launch/rms-recovery.json)、[整区校验](updates/2026-11-11-jar-launch/offline-validation.json) 和 [逐项启动](updates/2026-11-11-jar-launch/launch-sweep.json)。本地前后镜像保留在 `build/recovery/`，不上传游戏和用户存档内容。文件系统中另有历史目录/卷标及未引用簇提示，本轮未自动截断、删除或回收这些内容。

可复用的离线工具为 `tools/repair_fat12_jars.py`，输入为已去除磨损均衡层的 FAT12 镜像及匹配的原始 JAR 目录，只生成新镜像和报告，不访问设备。运行测试：

```sh
python -m unittest discover -s tests -p test_repair_fat12_jars.py -v
```
