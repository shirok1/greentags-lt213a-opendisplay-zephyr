# OpenDisplay 兼容性核对（版本、PIPE、配置）

2026-09-08；初始代码审计后已修复版本响应与 PIPE 自动结束。基准为官方 Firmware `7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb`、本项目安装的 py-opendisplay 7.14.1，以及官方通信协议页。本笔记覆盖上述三个范围，不是完整认证结果。

## 已修复的互操作缺口（下文保留修复前诊断）

1. **固件版本响应缺少 SHA。** `src/protocol.c` 的 `0x43` 返回 `00 43 00 02 00 00`。官方格式为 `[ACK][43][major][minor][shaLen][sha…][patch]`；当前第 5 字节表示零长度 SHA，py-opendisplay `protocol/responses.py::parse_firmware_version` 明确抛出 `Firmware version missing SHA hash`。所以现有刷图成功不代表 `read_firmware_version()` 可用；应带有效的构建标识，patch 放在 SHA 后。此项属于官方客户端实际要求的格式，不是无线 MTU 限制。[官方格式与占位回退](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/communication.cpp#L471-L503)

2. **已声明 PIPE，但未实现 raw 全帧自动结束。** `scripts/generate_config.py` 将 transmission_modes 设为 `0x1b`，包含 PIPE 位 `0x10`。`src/protocol.c` 接受 `0x80` 并协商 W=N=1；最后一个 `0x81` 只发 SACK，仍要求客户端发送 `0x82`。官方实现 raw 全帧收齐即发末尾 SACK、`00 82`、刷新结果；py-opendisplay 默认 `max_queue_size=16` 会尝试 PIPE，并等待该 unsolicited END_ACK。项目脚本的 `max_queue_size=1` 禁用了该路径，因此仍是客户端绕行。压缩与 partial PIPE 必须等显式 END，不能统一自动完成。PIPE 本身是可选能力，但声明后应符合对应语义；可以补齐，或移除能力位直到实现完成。窗口协商为 1 本身不是不兼容。[官方 raw 自动结束](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/display_service.cpp#L2995-L3005)、[partial 显式 END](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/display_service.cpp#L3074-L3094)

## 配置身份与范围

配置包含 system/manufacturer/power/display 四种 TLV，使用 CRC16-CCITT，显示包报告原生 104×212、1bpp。协议要求显示配置存在；20 字节配置响应只是保守分块，客户端可正常拼接，不必为了合规放大。[官方配置读写流程](https://opendisplay.org/protocol/ble-flow.html#configuration-reading-flow)

当前 manufacturer 全零对应 **DIY / Custom**，不是注册过的 Greentags 产品身份；system `ic_type=0xffff` 为本项目自定义未知值，官方已列 MCU 中没有 nRF51822。它避免误报成 nRF52 并被选择错误 OTA。这里是生态注册/识别缺口，不能据此断言通用刷图违反协议。应申请上游型号/MCU 标识后再使用正式值，不能自行占用已分配 ID。[官方枚举定义](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/include/opendisplay_structs.h)

配置存储支持写入、CRC 校验、双页断电提交、清除回到板级默认值。实现限制一块固定显示屏与固定接线；不具备任意 Flex 外设的重配置能力。Wi-Fi、LED、蜂鸣器等未装备硬件的可选功能缺失，不等于不合规。

## 需单独验证的错误响应细节

官方 `sendResponse` 对**第三字节** FE/FF 状态保留明文，并专门排除 PIPE ACK 的序号；初次审计时本项目 `send_response` 检查的是第一字节 FE。后续修复让认证 FE/FF 错误直接走明文 `send_wire`，避免被加密或当作业务 ACK；PIPE ACK 仍正常加密。这存在策略差异，但不能直接概括成“所有第一字节 FF 的 NACK 都应明文”：官方实现并非如此。应构造具体认证后的状态帧，用正式客户端验证，再认定影响。[官方封装条件](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/communication.cpp#L378-L391)

## 修复

版本响应现带 12 位源码提交 SHA（源码归档无 Git 时为 unknown），patch 位于其后；SHA 标识基准提交，不包含未提交修改。raw 全帧 PIPE 收齐后发送 SACK、END ACK，再刷新并返回结果；压缩及局部保留显式 END。上传脚本和实机回归已移除 max_queue_size 特殊设置。主机回归覆盖正式包解析版本、PIPE 序号回绕/重传、自动结束及迟到 END 不重复刷新。

实机验证：CMSIS-DAP / probe-rs 刷入并校验；官方 py-opendisplay 7.14.1 默认构造参数读取版本 0.2.0 / SHA 3ff2e070e113 成功，三轮重连共六次 raw/zlib 全屏刷新成功。最后一轮额外断言 SDK 已协商 PIPE，排除回退 direct-write。构建为 95,644 B Flash / 16,336 B RAM（静态 RAM 无新增）。认证/局部波形等待测项不因此视为通过。

## 认证专项修复

认证错误帧、PIPE nonce 重传策略、绝对会话期限与加密栈占用已修复，实机回归见 [认证互通研究](auth-interoperability.md)。
