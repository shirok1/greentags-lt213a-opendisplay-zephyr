# PIPE 与配置事务实机核查（2026-09-16）

结论：官方正常上传/配置路径可互通；本次定位并修复两个 PIPE 异常路径差异，完成修复前后实机对照。配置保留严格 CRC 校验和双槽提交，没有为模仿官方宽松处理而降低校验。源码逐条对照及官方链接见 [上游审计](pipe-config-upstream-audit-2026-09-16.md)。

## 设备、版本与备份

- 调试器：RV CMSIS-DAP，SWD 1000 kHz；芯片 nRF51822 QFAB（probe-rs 名称 `nRF51822_xxAB`）。
- BLE：`OD6935B2`，本机 CoreBluetooth 地址 `9C4CBCB8-22D0-1EA1-BB89-A5551E88BE58`。
- 基准：本仓 `3da3bef7ec9b0d1232d3799cda5ad67846b2fd41` 加工作区原有文档/runner 修改；开始时 SWD 读取的程序与本地 `build/zephyr/zephyr.bin` 逐字节一致。两页配置起初均为空，使用内置默认。
- 官方比较版本：`7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb` 与 `3e16d94cd0953308fa01035042e1369c4c051a86`；Python SDK 7.14.1。
- 完整 Flash 备份：`build/alignment-2026-09-16/before-flash.bin`（131072 B），SHA256 `c946abf646d4e53c3fcba52c8211745d593425a60ddde8136e705415645baf63`。
- 原配置容器：同目录 `original-config.bin`（133 B），SHA256 `cbf7952af8807359c1009dd36a3e5e6caa5f0e61be293b21d57041202389a61e`。
- 烧录仅更新应用、带 verify；烧录前后两页配置逐字节相同。本次结束后恢复并核对原配置容器；这指有效配置恢复，不是把配置页重新擦成全 FF。

修复后镜像：120668 B Flash / 16360 B RAM，较基准增加 72 B Flash，RAM 不变；预留栈/缓冲区以外仍剩 24 B RAM。镜像 SHA256 `f6239b67b9e15afec4ced90e60941a7ab149136a08a622a0f2cdc71fbc332ff4`。版本命令仍报告基准提交 SHA，不能据此区分未提交修复；以镜像校验值为准。

以上SHA256为构建的BIN文件。本次实际烧录ELF；最终SWD完整回读确认所有Flash PT_LOAD段与ELF一致，有效配置槽CRC正确且内容等于原配置。ELF/BIN在 `0x1d534..0x1d537` 的4字节链接对齐填充不同（00/ff），因此不把ELF烧录后的整段Flash哈希误当成BIN文件哈希；详见 `final-verification.txt`。

## 两项 PIPE 修复

1. **无活跃 PIPE 的 DATA 静默丢弃。** 基准在 raw 自动结束后重发末帧返回 `ff 81 04 ff 00 00 00 00`；修复后无通知，也不再次刷新。此处理还避免迟到 PIPE DATA 中止新的 direct-write 会话。
2. **失败 END 也先发尾 SACK。** 基准收到未传完的 raw END 直接回复 `ff 82`；修复后依次回复 `00 81 00 00 00 00 00`、`ff 82`。截断 zlib 的同类路径由主机回归覆盖；通知发送失败仍中止事务。

改动在 `src/protocol.c`。新增主机断言在修复前确实失败（`red.log`），修复后通过。没有扩大 PIPE 窗口、增加乱序缓冲或修改波形。

## 实机结果

修复前后均执行 `tests/test_alignment_hardware.py`；基准运行加 `--baseline`，仅允许观察并记录上述已知差异。修复后运行严格断言。

| 测试 | 结果与边界 |
|---|---|
| START 协商 | 请求 W=N=32，收到 `00 80 01 01 01 f4 00 01`：版本1、W=N=1、244字节帧、raw自动结束标记 |
| raw、SACK、序号回绕 | 每帧7字节，共394帧，逐帧核对最高序号及32位掩码；255→0正常 |
| 重传去重 | 第0、31、254、255、256帧重复发送；重复ACK位置不变，后续流仍完整结束；主机测试另验证未重复写屏 |
| raw 自动结束 | 最后DATA触发尾SACK→`00 82`→`00 73`，无需发送END；迟到END只返回NACK，无第二次刷新 |
| 越窗与 fatal | W=1时先发seq=1收到错误04；随后DATA静默；新START可恢复 |
| 不完整 END | 修复后尾SACK→NACK，无刷新成功通知 |
| 压缩 PIPE | 完整zlib数据收齐后不会自动结束；显式END后SACK→END ACK→刷新成功 |
| 局部 PIPE | 以压缩全刷建立etag，再启动8×8旧/新平面局刷；完整DATA后仍等待显式END；etag被接受 |
| 单包配置 | 修改无硬件副作用的厂商board_revision元数据，133B容器精确回读 |
| 多包配置 | 添加DataExtended，423B容器按200+200+23数据字节提交、分块回读，字节及CRC一致 |
| 坏CRC | 翻转尾CRC；末块NACK，原已提交配置仍可读 |
| 提交后SWD复位 | 读回同一423B配置 |
| 写到首块后SWD复位 | 不完整替换不生效，读回上一次完整配置 |
| 中途断连 | 新连接发送续块被拒绝，旧配置仍完整 |
| 超出声明长度 | 最后一块多1字节被拒绝，旧配置仍完整 |
| 清除/恢复默认 | `0x45` ACK后立即读回133B内置默认；SWD复位后仍默认 |
| 恢复原配置 | finally重新写入原容器，并逐字节回读一致 |

394帧的刻意小包测试修复前29.64s、修复后29.85s（包含刷新）；这是序号回绕测试，不是正常230字节大包上传的吞吐基准。重传通过发送端主动重发模拟，没有在空口注入真实丢包。

## 配置对齐应如何表述

- 本仓2字节配置ACK符合官方canonical协议；官方具体Firmware多附加两个零不构成本仓缺陷。
- 容器使用CRC16-CCITT-FALSE（ffff初值、1021多项式、计算时长度字段置零、尾CRC小端）；本仓强制校验，官方解析实现更宽松。本次保留强校验。
- 双槽header/commit是私有存储格式，和官方文件系统格式不同不影响线上读写协议。
- `0x45` 在本仓是恢复内置板级默认；官方清除存储后清空运行配置，不能把两者说成所有内部行为完全相同。
- 清除会使旧槽失效，但不保证物理擦除旧密钥，不能描述成安全销毁。
- 固定引脚/显示尺寸/外设类型仍受限。部分接受的非安全字段仅保存和回读；不能因WRITE得到ACK就认定power timeout、GPIO等动态重配置已生效。
- W=N=1仍是受资源限制的PIPE子集；零W/N、空DATA、宽松END尾长度等异常输入处理仍有差异，详见源码审计，本次未声称逐字节完整等价。

## 全屏 fast：对照完成

使用压缩PIPE及显式 `0x82 mode=0/1`，避免raw自动结束固定全刷遮蔽selector。相同全屏图案，依次FULL→FAST共三轮：

| 轮次 | FULL（mode=0） | FAST（mode=1） |
|---|---:|---:|
| 1 | 3.509s | 3.509s |
| 2 | 3.208s | 3.209s |
| 3 | 3.208s | 3.208s |
| 中位数 | **3.208s** | **3.209s** |

测量起点是发送END，终点是接收 `00 73`；包含BLE往返、刷新等待和驱动关电，不是示波器测得的纯BUSY时间。六次均收到尾SACK、END ACK、刷新成功通知。记录在 `fast.log`。

新增 `tests/test_epd.c` 虚拟GPIO测试逐字节比较同一数据在mode=0与1下的初始化、写入、刷新、关电SPI序列，二者完全一致；在启用和关闭屏幕深睡两种编译配置下通过。这是驱动输出验证，不是实板逻辑分析仪采集。

因此应修正先前的“fast未对齐官方效果”：对当前宣告的panel_ic=19，官方bb_epaper也没有独立Fast序列，选择fast会回退full；本仓的相对行为与它一致。[官方面板表](https://github.com/bitbank2/bb_epaper/blob/5dccfbbf553a9b0fe2547cbc4e60138e1ff2fb43/src/bb_ep.inl#L4092)、[官方回退](https://github.com/bitbank2/bb_epaper/blob/5dccfbbf553a9b0fe2547cbc4e60138e1ff2fb43/src/bb_ep.inl#L4826-L4831)。

这不代表两者Full寄存器值完全一致，也不代表T5不可能加速。真正fast需要另外研究适配T5的波形、电压、温度范围和黑白转换质量，不能简单移植其他2.13寸型号的LUT。本次没有改动面板驱动或尝试未经验证的电压/波形。设备最终显示全屏黑白测试条纹，配置恢复为原容器。

## 验证范围与复现

`uv run --locked python scripts/test.py` 全部通过，包含ASan/UBSan、20000畸形包、220次模拟Flash写入中断、密码学互通、GPIO/SPI虚拟面板、电压遥测与上传SDK回归。新增完整/fast模式SPI序列比较也在深睡开关两种构建下通过。

```sh
uv run --locked python tests/test_alignment_hardware.py DEVICE
uv run --locked python tests/test_alignment_hardware.py DEVICE --fast-only
```

实机脚本会改屏幕、临时写配置及SWD复位，要求默认133B无加密配置；配置会保存并恢复。运行前仍应保存完整Flash，以便连接失败时SWD恢复。日志在忽略目录 `build/alignment-2026-09-16/`：`baseline.log`、`fixed.log`、`host-tests.log`、`build.log`、`flash.log`。

本次没有切断板上真实电源，没有在Flash擦写电气瞬间做掉电注入；SWD在首块后复位和主机220次故障注入不能冒充真实掉电验证。未重新验证加密配置换key、低MTU peer、最坏栈水位或局刷光学质量。
