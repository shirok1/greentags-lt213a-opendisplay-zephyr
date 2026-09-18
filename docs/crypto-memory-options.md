# 硬件 AES 与 RAM 预算

[文档导航](README.md) · [内存测量](performance.md) · [认证协议](security.md)

## 实现

应用不再编译 TinyCrypt。`src/crypto.c` 提供 AES-CMAC（RFC 4493）与固定 OpenDisplay
CCM 模式，`src/crypto_zephyr.c` 通过 `bt_encrypt_be()` 使用控制器已有的 nRF51 ECB
外设。模式层与设备适配分离；主机测试将相同模式代码连接到 cryptography 的 AES-block
回调，并独立计算 CMAC/AESCCM 期望值。输入容量与别名约束见 `src/crypto.h`。

模式层保留 13 B nonce、2 B AAD、12 B tag、常量时间 tag 比较、AES 错误传播和失败
载荷清理；只有验证成功后才更新重放窗口。应用 CMAC KDF、会话与帧格式保持兼容。
后端同步轮询的无线时序、错误恢复与性能尚未实机验证。

固定版本 Zephyr `ecb_encrypt_be()` 的 48 B 局部参数包含 key、输入和输出，未主动擦除。
模式层 `wipe` 无法清理该后端返回后的栈副本；此实现不保证完整密钥擦除。
硬件入口依据：[controller crypto](https://github.com/zephyrproject-rtos/zephyr/blob/v4.4.2/subsys/bluetooth/controller/crypto/crypto.c)、
[nRF5 ECB](https://github.com/zephyrproject-rtos/zephyr/blob/v4.4.2/subsys/bluetooth/controller/ll_sw/nordic/hal/nrf5/ecb.c)。
通用 `CRYPTO_NRF_ECB` 与 controller 在
[Kconfig](https://github.com/zephyrproject-rtos/zephyr/blob/v4.4.2/drivers/crypto/Kconfig.nrf_ecb)
中互斥，不应另行启用或直接争用寄存器。

## TinyCrypt 替代对照

2026-09-18，以 `dbb00ad` 为基线，Zephyr v4.4.2、Arm GNU GCC 15.3.1 20260627、
O2 + LTO，保留原缓冲补丁和 1600 B 主栈的构建结果：

| 构建 | Flash B | 静态 RAM B |
| --- | ---: | ---: |
| TinyCrypt 基线 | 110920 | 16172 |
| 硬件 AES + 应用 CMAC/CCM，deep sleep | 109392 | 16172 |
| 硬件 AES + 应用 CMAC/CCM，power-off | 109344 | 16172 |

净减少 1528 B Flash。TinyCrypt 的轮密钥在栈中；固定栈预留不变时静态 RAM 总量不变，
只有证明确实可以缩小栈预算，才能转化为 RAM 链接空间。基线 ELF 中至少 1850 B Flash
可明确归属 TinyCrypt，其余模式代码被 LTO 内联，不能只按库名相加。

tiny-AES-c 的 AES-128 ECB context 同样保留 176 B 展开轮密钥，因此没有选择它作为
缩栈后端。依据：[TinyCrypt headers](https://github.com/zephyrproject-rtos/tinycrypt/tree/1012a3ebee18c15ede5efc8332ee2fc37817670f/lib/include/tinycrypt)、
[tiny-AES-c context](https://github.com/kokke/tiny-AES-c/blob/master/aes.h)。

当前默认已采用未修改 Zephyr、压紧 Huffman 表、1280 B 主栈和 ACL TX 1 / Event RX 2：
deep sleep 为 109524 B Flash / 16360 B RAM，power-off 为 109484 B Flash / 16360 B RAM。
MTU 247 保留；与上表相同，工具链为 GCC 15.3.1、O2 + LTO。
这是构建和主机测试证据，新的栈/缓冲预算仍需实机验证；详见[内存预算](performance.md)。

## 栈测量的含义

原型使用 `-fstack-usage` 的 LTO 最终报告和反汇编核对同一认证分支：

| 函数帧 | TinyCrypt B | 硬件 AES B |
| --- | ---: | ---: |
| main | 576 | 576 |
| od_security_auth | 296 | 176 |
| CMAC | 320 | 136 |
| 软件 AES / do_ecb | 96 | 28 |
| 上述调用链合计 | 1288 | 916 |

48 B ECB 参数已由 LTO 内联计入上层帧，不能重复加算。916 B 不是整个程序最坏栈上界：
直接调用分析已有约 1040 B 的通知候选链，尚未覆盖所有协议回调、启动包装与异常现场。
缩小主栈需要同一 ELF 下覆盖认证、最大帧、通知、刷屏、重连与配置写入的实机水位。
原型 ELF/栈报告在本地忽略目录 `build/crypto-experiment/`，不作为新检出的依赖。

## 验证

统一入口 `uv run --locked python scripts/test.py` 编译真实 C 模式实现并验证完整协议。
密码学桥带 UBSan；用独立 CMAC/AESCCM 原语覆盖认证/KDF、所有 CCM 载荷长度 0..214、
CMAC 长度 0..256、原地处理、每个 tag 字节损坏、各 AES 调用阶段失败与重放窗口。
这些测试不证明设备 AES 输出、无线行为或 ARM 最坏栈安全。
