# 从命令行重复运行点对点测试

两块板子保留当前已烧录的 TX/RX 固件，通过 J-Link 的 SWD 接口重启即可重新测试。脚本不编译、不擦除、不烧录 Flash；每一轮自动清零接收统计，采集 RTT 日志并导出成功率、RSSI 和首径功率。

在项目根目录打开 PowerShell 或 CMD，运行：

```powershell
.\repeat_300k.cmd
```

连续运行 3 轮，每轮分别统计：

```powershell
.\repeat_300k.cmd -Rounds 3
```

也可从任意目录调用完整路径：

```powershell
& 'C:\Users\wangrx\Desktop\UWB_project\DW3_QM33_SDK_1.1.1\repeat_300k.cmd'
```

## 连接和启动顺序

- 保持两套设备供电，两个 J-Link 都通过 USB 连接电脑，SWD 接线保持当前方式。
- 默认 TX 探针序列号为 `1050244938`，RX 为 `1050238642`。
- 关闭占用这两个探针的 Embedded Studio 调试会话、J-Link Commander 和旧 RTT 监控窗口。
- 脚本先重启并暂停 TX，再重启 RX、连接 RTT 并检查接收就绪。RX 就绪后才将 TX 的连接交给 RTT Logger，启动发送。接收端 45 秒内未就绪，或启动后 90 秒没有数据，脚本停止本轮并报错。
- 发送开始后等待 `RX_COMPLETE`，打印成功率及平均功率；所有日志使用独立目录，避免混入上一轮数据。出现超时或接收端重启时不会生成有效结果。

## 输出文件

每次执行生成 `uwb_test_firmware/repeat_时间戳/`：

| 文件 | 内容 |
| --- | --- |
| `results.csv` | 本次执行各轮汇总 |
| `round_1.result.csv` | 第 1 轮包数、成功率、RSSI/首径功率均值及范围 |
| `round_1.rx.log` | 原始接收端日志 |
| `round_1.tx.log` | 原始发送端日志（TX RTT 可读时生成） |
| `round_1.rx.signal_summary.csv` | 全部成功接收包的功率统计 |
| `round_1.rx.signal.csv` | 抽样功率明细 |
| `round_1.tx_control_*.log`、`round_1.rx_reset_*.log` | 自动重启操作日志 |

功率汇总覆盖每个成功接收的唯一 DATA 包。当前固件的明细打印间隔为 1,000 个成功包，因此明细 CSV 不是逐包记录。对异常计时字段，脚本保留原值并将 `timing_valid` 标为 false，`goodput_bps` 留空。

收包侧已取消 `invalid` 和 `invalid_failed` 统计。帧长度、内容、序号校验仍保留，不符合要求的帧不计入成功包。最终 `failed` 和 `lost` 均表示“目标包数减去成功接收的唯一 DATA 包数”，不再划分失败原因；PHY/CRC 错误事件不叠加到丢包数。功率日志中的 `invalid`（CSV 中为 `signal_invalid`）仍表示功率诊断测量失败次数。历史日志和 CSV 保留原始字段，不回写修改。

部分连接状态下 TX 可以正常发送，但其 RTT 内存不可读。最终包数以 RX 收到的带 CRC 保护的 END 标记及 RX 的唯一包统计为依据；TX 明细日志是辅助证据，缺少该文件不会阻止导出完整接收结果。`tx_log_complete` 表示是否额外读到了 TX 的完成日志。

## 固件参数

发包数量只有一个配置来源：`Drivers/API/Src/p2p_test_config.h` 中的 `TEST_PACKET_TARGET`。TX、RX 直接包含该头文件；电脑侧构建、重测和结果校验脚本通过 `get_packet_target.ps1` 读取同一行。数量使用正十进制整数，不使用表达式或带前导零的数字。

修改数量的步骤：

1. 只修改共享头文件中的 `#define TEST_PACKET_TARGET ...UL`。
2. 重新编译并烧录两块板子一次，可在项目根目录运行下面的构建测试命令（会烧录并运行一轮）：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\uwb_test_firmware\run_no_protocol_size_tests.ps1
```

3. 后续重复同一数量的测试，继续运行 `.\repeat_300k.cmd`，无需指定数量，也无需再次烧录。文件名中的 `300k` 是保留的入口名称，实际数量始终读取共享配置。

不操作硬件、只编译 TX/RX 时使用：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\uwb_test_firmware\run_no_protocol_size_tests.ps1 -BuildOnly
```

原来的 `-Target` 和 `-Targets` 参数已移除，防止产生第二个配置来源。重测时，如果已烧录 RX 的数量与共享配置不同，脚本会报错要求先更新两块板子的固件。修改无线参数或逐包打印间隔同样需要重新编译烧录。TX 延迟和功率日志间隔仍由构建脚本的 `-IntervalUs`、`-SignalLogEvery` 配置。

不操作硬件的配置校验测试：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\uwb_test_firmware\signal_validation\test_packet_config.ps1
```

默认 RX RTT 地址为 `0x20000048`，TX 为 `0x20000050`，对应当前固件。更换固件导致 RTT 地址改变时，使用各自构建产物 `.map` 中 `_SEGGER_RTT` 的地址：

```powershell
.\repeat_300k.cmd -RxRttAddress 0x20000048
```

设备或工具路径变化时可传 `-TxSerial`、`-RxSerial`、`-JLinkDirectory`。失败的原始日志会保留；脚本不会通过擦除或重新烧录来恢复连接。
