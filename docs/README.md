# LT213A 技术文档

文档按使用和维护任务组织，说明当前实现、设计约束与验证边界。

| 你想做什么 | 从这里开始 |
| --- | --- |
| 构建、烧录、上传和配置设备 | [使用指南](guide.zh-CN.md) |
| 理解请求路径、硬件和模块边界 | [架构与资源模型](architecture.md) |
| 实现客户端、PIPE和持久配置 | [协议与配置事务](protocol.md) |
| 排查名称、发现、MTU、通知和重连 | [BLE连接与设备身份](ble.md) |
| 理解认证、CMAC/CCM、会话和密钥切换 | [认证与加密](security.md) |
| 理解全刷、局刷、等待和画质取舍 | [T5屏幕驱动](display.md) |
| 理解广播、遥测、深睡和看门狗 | [电源与稳定性](power.md) |
| 评估资源、吞吐、栈和测量方法 | [性能与内存](performance.md) |
| 选择回归测试、判断证据范围 | [验证指南](validation.md) |

第一次接触项目，可先读使用指南和架构，再按任务选择主题。开发与交付要求见
[CONTRIBUTING](../CONTRIBUTING.md)，agent入口见 [AGENTS](../AGENTS.md)。

协议兼容性以固定的OpenDisplay Firmware `7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb`
和py-opendisplay 7.14.1为基准；依赖由 `west.yml`、`uv.lock` 固定。
参考测量保留必要的构建和负载条件，不作为所有客户端、面板和环境的保证。
