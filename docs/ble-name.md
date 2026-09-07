# OpenDisplay BLE 名称

核对日期：2026-09-07。官方 Firmware 当前 HEAD 与本项目协议基准均为 `7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb`。

本项目使用的 OpenDisplay 通信协议明确写明：客户端按 `OD` 前缀发现设备，名称格式为 `OD[chip_id]`，广播服务 UUID 为 `0x2446`。因此名称应以 `OD` 开头。[官方 BLE 连接流程](https://opendisplay.org/protocol/ble-flow.html#ble-connection-establishment)

官方 nRF 参考实现将 `OD` 与六位大写十六进制芯片标识拼接，不加连字符或空格。标识取 `NRF_FICR->DEVICEID[1] & 0xFFFFFF`，不足六位左补零；不是 BLE MAC 的后三字节。六位长度与寄存器派生方式是参考实现约定，协议页仅规定 `OD[chip_id]`。[名称拼接](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/main.cpp#L207-L217)、[nRF 标识派生](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/encryption.cpp#L849-L861)

Zephyr 端应在启动广播前按此规则生成八字符名称，同时用于 GAP Device Name 和 Complete Local Name 广播字段。其表达式等价于：

```c
snprintk(name, sizeof(name), "OD%06X",
         (unsigned int)(NRF_FICR->DEVICEID[1] & 0xFFFFFF));
```

本固件已实现上述规则。实板通过 DAP 读取 `DEVICEID[1] = 0xCADA67FC`，对应名称 `ODDA67FC`。

官方网页 BLE Tester 的默认设备过滤值也是 `OD`；官方首页的 SDK 示例使用 `OD123456`。[BLE Tester](https://opendisplay.org/firmware/display/index.html)、[官方首页](https://opendisplay.org/)

注意官方另一个 basic-standard 页面将名称留给制造商，但它描述的是另一套拉取式数据流程；本仓实现对应上面的 BLE communication protocol，应沿用其发现规则。[Basic standard 的广播条款](https://opendisplay.org/protocol/basic-standard.html#advertising)
