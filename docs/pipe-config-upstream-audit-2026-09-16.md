# PIPE 与配置协议源码对齐审计（2026-09-16）

本记录为源码研究，不代表本次硬件测试结果。比较本仓研究开始时的 `src/protocol.c`、`config_store.c`、`storage.c`、`main.c`，与官方固定提交 `7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb`。另核对本地已下载的官方 `3e16d94cd0953308fa01035042e1369c4c051a86`：`communication.cpp`、`config_parser.cpp` 相同；`display_service.cpp` 的变更涉及 EP426 波形/关电，不改变这里讨论的 PIPE 状态机。以下本仓行号是审计起始快照，后续修复可能移动行号。

同日后续：无活跃PIPE的DATA静默处理、失败END先尾SACK两项已修复并完成实机对照；配置事务与full/fast计时亦已完成，见 [实机核查与修复结果](pipe-config-hardware-2026-09-16.md)。下表保留修复前差异，不能当作仍未修复的缺陷列表。

## 结论

PIPE 是 W=N=1 的有效兼容子集，正常 SACK、重传去重、序号回绕和 raw 全屏自动结束基本对齐；不提供官方多包乱序缓存与吞吐。存在可收敛的异常路径差异：无会话 DATA、未完成 END 的尾部 SACK、空 DATA、错误分类。配置容器与读写分块可以互通，但严格 CRC/TLV 验证、事务式双槽、清除后立即恢复内置默认都是本仓明确不同的行为，不能笼统称为与官方完全等价。更重要的是“保存成功”不等于“所有字段生效”：本仓只有 SecurityConfig 被运行时消费。

## PIPE 明细

| 项目 | 本仓起始行为 | 官方行为/判断 |
|---|---|---|
| START 协商 | `protocol.c:219-241`，版本 1、W=N=1、frame=min(client,244)，W/N=0 拒绝 | 官方将 W/N=0 归一到 1，N<=W；ACK 报服务器上限而非实际 min 结果。本仓有效值更保守，SDK 正常请求可互通。[协商](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/display_service.cpp#L2871-L2934) |
| SACK 位图 | `protocol.c:63-72`，最高连续 seq 与此前最多 32 帧，首帧 mask=0 | 本仓无乱序缓存，在 W=1 前提下连续掩码正确。官方位图还表达乱序已接收帧。[位图](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/display_service.cpp#L2638-L2667) |
| 重传 | `protocol.c:246-258`，已接收且落后 1..33 帧只回 ACK，不重复写屏；累计帧数 uint32_t，seq uint8_t | 官方仅接受 back<=W 的重复包；本仓比自己的 W=1 更宽容，不影响合规客户端。官方 W>1 可先缓存超前帧，再补洞。[DATA](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/display_service.cpp#L2971-L3051) |
| raw 全屏自动结束 | `protocol.c:253-256`，尾 SACK → `00 82` → 全刷 → `00 73/74`；etag=0 | 相同。无需显式 END，也无法通过后发 END 将本次自动全刷改成 fast。[自动结束](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/display_service.cpp#L2995-L3004) |
| 压缩/局刷结束 | `protocol.c:188-194`，必须 END；压缩流须完整，局刷可空 END | 相同核心语义；官方接受更宽松 END 长度及 selector，当前本仓只接受 1/5 字节或局刷 PIPE 空 END。[END](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/display_service.cpp#L3054-L3132) |
| 无活跃 PIPE 的 DATA | `protocol.c:243-264` 发 fatal NACK 04；也可能中止当前 direct 会话 | 官方静默丢弃。raw 自动完成后尾帧重复会触发此差异，值得修复。[入口](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/display_service.cpp#L2962-L2964) |
| 未完成 END | `protocol.c:189-192` 直接 `ff 82`，没有尾 SACK | 官方活跃、未 fatal 的 END 均先 SACK 再给 END 结果；即使短流/坏压缩也如此。[尾 SACK](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/display_service.cpp#L3071-L3088) |
| fatal 后 DATA | `protocol.c:244,263-264` 首次 NACK 后释放屏幕，后续静默，START 可恢复 | 对齐核心行为。官方保存错误会话 ACK 位置直到 END/START/断连，本仓清状态另设 pipe_failed。[fatal](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/display_service.cpp#L2688-L2727) |
| 空/超长 DATA | 本仓要求 seq 后至少 1 字节，超协商 frame 也报 04 | 官方空命令体静默、仅 seq 的 DATA 接受并计数；大于槽容量报 03。错误码差异可修复，但无须放宽安全边界。[入口](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/display_service.cpp#L2962-L2979) |
| raw 超量 | `protocol.c:45` 拒绝并 NACK 03 | 官方 `pipeConsumePayload` 将尾包裁剪到剩余长度；本仓拒绝多余字节是合理收紧。[消费](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/display_service.cpp#L2767-L2782) |

已有 `tests/test_protocol.c:104-153` 以 7 字节载荷跨越 256 帧，逐帧验证位图、重复包无重复写屏、自动结束顺序、fatal 后静默；这是 host mock 证据，不是 BLE 实机证据。

## 配置明细

| 项目 | 本仓起始行为 | 官方行为/判断 |
|---|---|---|
| READ `0x40` | `protocol.c:108-129`：首包 total+chunk，后包 chunk；通知最多 20 字节 | 相同包头，官方最多 100 字节响应，首包 94 字节数据、后包 96；客户端必须按实际长度拼接，不硬编码块长。[读取](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/communication.cpp#L504-L552) |
| WRITE `0x41/42` | `protocol.c:131-144`：单包 <=200；首分块 2 字节总长+200 数据；按精确总字节数提交 | 官方同常规格式，但主要按 ceil(total/200) 块数判断结束，尾块长度不严格匹配声明 total。本仓更严谨，正常 SDK 互通。[写入](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/communication.cpp#L555-L650) |
| 写入/清除响应 | 本仓 `reply()` 2 字节；官方 4 字节且尾部 00 00 | 本仓符合官方 canonical 协议，本身不是缺陷，无须添加填充。官方头文件明确列出 2 字节响应：[WRITE/CHUNK](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/include/opendisplay_protocol.h#L310-L332)、[CLEAR](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/include/opendisplay_protocol.h#L367-L370)。 |
| 容量 | `config_store.h` 最大 768 字节 | 官方 `config_parser.h:7` 4096 字节；明确的资源限制。[官方容量](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/config_parser.h#L7) |
| 外层 CRC | `config_store.c:15-29` 强制 CRC16-CCITT-FALSE，初值 ffff，多项式 1021，长度前两字节归零；尾 CRC 小端 | 算法一致；官方 saveConfig 不校验外层 CRC，parse 阶段仅 advisory，且检查受 offset 条件控制。不应为机械对齐取消本仓强校验。[算法/持久 CRC32](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/config_parser.cpp#L296-L340)、[advisory 检查](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/config_parser.cpp#L694-L703) |
| 结构验证 | `config_store.c:25-64` 强制 v1、长度为 0 或准确总长、必要四包、无重复、已知类型/固定接线、安全约束 | 官方解析更宽松，支持更多硬件种类。本仓限制应明确保留。 |
| 持久化 | `config_store.c:66-89,106-146` 两个 1 KiB 槽、generation、内部 CRC16、commit 最后写；完成前旧槽仍有效 | 官方 filesystem 文件头使用 CRC32；保存前删除旧文件，再写 header+payload，不提供同样的应用层双槽协议。不同存储格式不是线上协议不兼容。[官方保存](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/config_parser.cpp#L148-L198) |
| CLEAR `0x45` | `protocol.c:145-147`、`config_store.c:143-146`：原子提交零长度墓碑，立即回内置默认，重启仍为默认；擦写失败旧配置保留 | 官方删除配置文件、清内存 global/security/Wi-Fi 配置；handler 不立即重装出厂默认。因此“清空”和“恢复内置默认”不是同一精确语义。[清除](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/config_parser.cpp#L201-L223) |
| 生效范围 | `od_config_get()` 在 src 中仅被协议读取、main 指针变化检测、SecurityConfig 查找消费；一般字段只存储和回读 | 官方成功写入后 `loadGlobalConfig()`；例如 screen_timeout_seconds=0 可立刻关掉保温屏。本仓不得将 power timeout、rotation 等 ACK 成功描述为动态硬件配置支持。[官方重载](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/communication.cpp#L204-L222) |
| 会话切换 | `main.c:346-352`，提交配置 ACK 后检测存储指针改变，重置安全会话并更新加密广告标记 | 官方成功保存后先重载并清加密会话，再 sendResponse；切换密钥/启停认证的最终响应加密状态不同，客户端需重新建立认证。 |
| 中断写入 | `main.c:286-303` 断连或 30 秒无包取消 staging，不动活跃槽；`protocol.c` 新 START 也取消 | 本仓事务行为明确。验证读回不能仅凭首块 ACK 宣布写入成功，必须等最终块。 |

注意：双槽 clear 使旧配置不再被选中，不等于物理安全擦除旧密钥；旧槽可能仍留有字节直到下一次擦写。不要把 `0x45` 描述为安全销毁。

## 推荐测试矩阵

| 场景 | 应检查的可观察结果 |
|---|---|
| raw PIPE 正常上传 | START W=N=1；逐包 SACK；尾 `81→82→73`；屏幕确实变化；仅一次刷新 |
| 重复 DATA/丢失 ACK | 模拟不消费 ACK 后重发相同 seq；写入位置不前进第二次；下一帧可继续 |
| seq 回绕 | payload <=10 字节，至少 257 帧；255→0 掩码连续，无 phantom bits |
| raw 自动结束后末包重复 | 不产生新刷屏、无 fatal NACK；随后版本查询/新 START 可用 |
| 压缩完整/截断/坏流 | 完整流显式 END 才刷新；截断/坏流必须失败不刷新；活跃 END 的尾 SACK 顺序明确 |
| 越窗、超长、空帧 | 04 vs 03 分清；fatal 后后续 DATA 静默，START 恢复；不留屏幕供电 |
| config 单包/大于200 | 精确回读容器；最终 ACK 后软重启、SWD reset 再读一致 |
| config 坏 CRC/未知 TLV/错长度 | NACK，旧配置可读，重启后仍旧配置 |
| config 中途断连/超时 | 未完成内容不生效，新连接读旧配置；后续合法写入可成功 |
| config 多余 continuation/超出 total | NACK 且不提交；必须重发新的 START |
| CLEAR | 当次读默认、重启读默认、无旧 SecurityConfig；连清两次仍成功 |
| 双槽故障注入 | 擦除/载荷/头/commit 各中断，重启只能得到完整旧或完整新配置；与真实电源切断区分 |
| 配置写入生效性 | 修改允许字段分别验证实际运行效果；只 round-trip 不能证明 timeout/rotation 等功能 |
| 加密配置变更 | 旧会话下最终 ACK 可解析，新密钥/无密钥重连；旧会话不能继续操作 |

## 与下一步 fast 全刷的联系

raw 全屏 PIPE 必然自动使用 full 波形，这是官方也存在的行为，不应误判为本仓 fast 实现缺失的唯一原因。研究 full fast 应使用压缩 PIPE+显式 END mode=1，或允许显式 END 的 direct 路径，并分开记录协议选择器被接受、驱动实际执行的波形、BUSY 时长与肉眼成像质量。[官方自动 full 路径](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/display_service.cpp#L2995-L3004)


## fast 全屏的官方驱动证据（继续研究）

**修正先前“fast 没有对齐官方效果”的笼统判断：对本机宣告的 panel_ic=19，官方自身同样没有独立 fast 序列。**

1. 官方将 panel_ic `0x0013`（十进制 19）映射为 `EP213_104x212`，并非其他 122×250 的 2.13 寸型号。[映射](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/display_service.cpp#L785)
2. 官方 `platformio.ini` 固定 bb_epaper 为 `5dccfbbf553a9b0fe2547cbc4e60138e1ff2fb43`。[依赖版本](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/platformio.ini#L55-L61)
3. 该版本面板表此项为 `{104,212,0,epd213_inky_init_sequence_full,NULL,NULL,0,BBEP_CHIP_UC81xx,...}`，也就是 Fast/Partial 指针都为空。[面板表](https://github.com/bitbank2/bb_epaper/blob/5dccfbbf553a9b0fe2547cbc4e60138e1ff2fb43/src/bb_ep.inl#L4092)
4. `bbepRefresh(REFRESH_FAST)` 遇到空 `pInitFast` 明确执行 `pInitFull`，所以这个官方型号选择 fast 也回退 full。[fallback](https://github.com/bitbank2/bb_epaper/blob/5dccfbbf553a9b0fe2547cbc4e60138e1ff2fb43/src/bb_ep.inl#L4820-L4839)
5. 官方全屏 END 的 selector=1 确实传入 REFRESH_FAST，再调用 bbepRefresh；该路径没有针对本型号的额外加速。[selector 与驱动调用](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/display_service.cpp#L2492-L2537)

此结论是 **fast/full 相对行为一致**，不是说本仓寄存器波形与官方 InkyPHAT 初始化完全一致。官方 Full 初始化 `PWR=07 00 0a 00`、`BTST=07 07 07`、`PSR=cf`、`CDI=07`、`PLL=29`、`VCOM=0a`；本仓使用针对实际 LT213A/T5 验证过的厂家初始化。直接搬用通用 InkyPHAT 初始化没有足够硬件依据。[官方 full 序列](https://github.com/bitbank2/bb_epaper/blob/5dccfbbf553a9b0fe2547cbc4e60138e1ff2fb43/src/bb_ep.inl#L3515-L3527)、[本仓 T5 验证](partial-t5.md)

真正实现更快的全屏更新将是新波形与画质验证工作。不能把 122×250 的 SSD1680 fast、T5D 的 LUT 或 EP426 的时序作为这个 104×212 T5 的现成官方 fast。现有 region LUT 路径需要旧/新两平面且有残影记录，简单将整屏范围套上它也不能等同于无前置条件的 full fast。
