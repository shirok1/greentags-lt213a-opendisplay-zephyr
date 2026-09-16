# OpenDisplay BLE 名称

## 序列号录入（2026-09-16）

固件优先使用 OpenDisplay `DataExtended`（TLV `0x2C`）的 `serial_number`：非空时名称为 `OD` + 序列号，保留大小写；没有此包或序列号为空时，沿用下面的芯片 ID 规则。GAP Device Name 与扫描响应中的 Complete Local Name 一致。配置提交后立即更新名称，断开再扫描即可看到，无需重启；清空序列号或恢复默认配置后回退芯片 ID。

```sh
# 一体入口：扫描、查看设置情况、按编号录入、重新扫描
uv run --locked python scripts/set_serial.py
# 只查看本轮收到广播的设备及真实 serial 设置情况
uv run --locked python scripts/set_serial.py scan
# 隐藏已设置的设备，保留未设置及无法确定状态的设备
uv run --locked python scripts/set_serial.py scan --unset
# 也可直接用已知地址录入或清空
uv run --locked python scripts/set_serial.py DEVICE_ADDRESS 2402859c
uv run --locked python scripts/set_serial.py DEVICE_ADDRESS --clear
```

扫描列表显示编号、已设置/未设置/未知、serial_number、RSSI、名称及地址。没有收到广播名称时，操作系统提供的旧名称会标注“缓存名”。脚本先扫描广播，再按信号强度从高到低逐台连接读取配置；不会仅凭`ODxxxxxx`名称猜测是否设置过，也不会将超时或需要密钥的设备当作未设置。没有广播名称但带OpenDisplay厂商数据/服务UUID的设备也会列入；同名设备按地址分别保留。状态未知时显示原因，交互菜单不会直接对其写入。

默认扫描12秒、每台读取最多20秒，可通过`--seconds 20 --timeout 30`调整。列表是本轮扫描快照；未收到广播的设备无法列出。不带参数进入交互模式，输入列表编号和新serial即可录入，`r`重新扫描、`q`退出，无需复制地址、拆电池或重启。录入后自动刷新列表；纯`scan`只读取，不修改配置。`--unset`也可用于交互模式。

脚本先读取当前配置，仅修改 `data_extended.serial_number`；缺少 DataExtended 时创建该包，其余字段保留。写入前自动保存原配置到 `build/serial-backups/`，也可用 `--backup PATH` 指定一个尚不存在的文件；文件权限为0600，不覆盖已有备份。写入后重新连接并比对完整配置，验证其他字段没有改变。脚本调用官方SDK处理TLV、CRC、分包和认证；已启用认证的设备可传 `--key-file PATH`，文件必须是原始16字节密钥。

序列号接受最多31字节的可打印UTF-8文本（不含NUL/控制字符，不接受纯空白）。完整值保存在配置；BLE传统扫描响应只有31字节，扣除AD头及`OD`前缀后，名称最多容纳27字节序列号。更长的序列号按UTF-8字符边界截断显示，不改存储内容。建议使用不超过27字节的板号，避免不同长编号显示成相同前缀。清空、恢复默认或整片擦除都会移除录入值；普通仅应用区烧录保留配置。

这是一项本项目的命名扩展，保留官方客户端依赖的`OD`前缀；没有改变BLE地址，也不从丝印自动读取编号。

### 实机验证

2026-09-16，CMSIS-DAP烧录并verify通过，配置页保留；在当前测试板录入`2402859c`，官方SDK重连回读完整配置一致。实际扫描响应验证了`OD2402859c`→清空后`OD6935B2`→重新录入后`OD2402859c`；SWD复位后仍为`OD2402859c`，SDK版本/配置查询通过。仅录入serial_number，没有写入背面编号或其他身份字段。

扫描可靠性另有未消除的现象：初次15秒扫描超时，切换/复位流程随后通过，但独立连续扫描回归在第二轮20秒时再次未收到名称；30秒观察只收到两个`local_name=None`事件（RSSI -85/-71 dBm）。这些失败保留在`scan-repeat.log`、`continuous-scan.log`，不能声称所有扫描回归通过。通过SWD核对当时未连接、慢广播已启用（interval=1600），GAP名称正确；控制器当前SCAN_RSP PDU为`44 12 [6-byte address] 0b 09 4f 44 32 34 30 32 38 35 39 63`，确实携带完整`OD2402859c`。因此没有发现序列号读取/名称编码错误，但仅靠这些信息不能确定是空口接收、扫描请求响应链路还是macOS扫描栈的问题，也没有据此改变原有广播策略。

主机回归验证DataExtended位于SecurityConfig之后时的查找、名称为空的回退、恢复默认、31字节长串与UTF-8截断、保留其他配置/密钥、备份权限与拒绝覆盖、回读失败上报。完整测试通过，构建为120900 B Flash / 16376 B RAM（较上一轮PIPE修复增加232 B Flash、16 B RAM，预留栈/缓冲外剩8 B）。当前测试结果不构成最坏栈空间证明。

本轮备份和日志位于`build/serial-number/`：`before-flash.bin`、`original-config.bin`、`set.log`、`hardware.log`、`tests.log`、`build.log`、`flash.log`。`scripts/set_serial.py`同样可以用于后续录入其他板子，传入对应的扫描地址即可。

扫描/交互入口追加验证：`tests/test_serial.py`覆盖同名不同地址、无名称广播、未设置与读取超时/需密钥的区别、扫描无配置写入，以及`--unset`过滤后菜单编号仍准确对应设备。真实只读扫描第一轮发现2台，`OD84F6BD`配置确认为未设置，另1台读取超时标为未知；后一次扫描未收到OD广播，输出0台，没有复用上一轮列表或假装设备仍在线。记录位于`build/serial-scan/`。本轮没有修改任何实机serial或固件。

## 芯片 ID 回退规则与官方依据

以下依据核对于2026-09-07，当时官方 Firmware HEAD 与本项目协议基准均为 `7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb`。

本项目使用的 OpenDisplay 通信协议明确写明：客户端按 `OD` 前缀发现设备，名称格式为 `OD[chip_id]`，广播服务 UUID 为 `0x2446`。因此名称应以 `OD` 开头。[官方 BLE 连接流程](https://opendisplay.org/protocol/ble-flow.html#ble-connection-establishment)

官方 nRF 参考实现将 `OD` 与六位大写十六进制芯片标识拼接，不加连字符或空格。标识取 `NRF_FICR->DEVICEID[1] & 0xFFFFFF`，不足六位左补零；不是 BLE MAC 的后三字节。六位长度与寄存器派生方式是参考实现约定，协议页仅规定 `OD[chip_id]`。[名称拼接](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/main.cpp#L207-L217)、[nRF 标识派生](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/encryption.cpp#L849-L861)

没有配置序列号时，Zephyr端按此规则生成八字符名称。其表达式等价于：

```c
snprintk(name, sizeof(name), "OD%06X",
         (unsigned int)(NRF_FICR->DEVICEID[1] & 0xFFFFFF));
```

本固件已实现上述回退规则。2026-09-07的测试板通过DAP读取 `DEVICEID[1] = 0xCADA67FC`，对应 `ODDA67FC`；2026-09-16当前测试板为 `0xA56935B2`，对应 `OD6935B2`，录入 `2402859c` 后应广播 `OD2402859c`。

官方网页 BLE Tester 的默认设备过滤值也是 `OD`；官方首页的 SDK 示例使用 `OD123456`。[BLE Tester](https://opendisplay.org/firmware/display/index.html)、[官方首页](https://opendisplay.org/)

注意官方另一个 basic-standard 页面将名称留给制造商，但它描述的是另一套拉取式数据流程；本仓实现对应上面的 BLE communication protocol，应沿用其发现规则。[Basic standard 的广播条款](https://opendisplay.org/protocol/basic-standard.html#advertising)
