# 从命令行重复运行 300,000 包测试

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

功率汇总覆盖每个成功接收的唯一 DATA 包。当前固件的明细打印间隔为 1,000 个成功包，因此明细 CSV 不包含 300,000 行。对异常计时字段，脚本保留原值并将 `timing_valid` 标为 false，`goodput_bps` 留空。

部分连接状态下 TX 可以正常发送，但其 RTT 内存不可读。最终包数以 RX 收到的带 CRC 保护的 END 标记及 RX 的唯一包统计为依据；TX 明细日志是辅助证据，缺少该文件不会阻止导出完整接收结果。`tx_log_complete` 表示是否额外读到了 TX 的完成日志。

## 固件参数

当前两块板子的目标数为 300,000，TX 延迟为 500 微秒，RX 开启 RSSI/首径功率统计。重复执行同一配置无需重新烧录。

`-Target` 只校验已安装 RX 固件的目标数，不改变发包数。要改发包数、无线配置或逐包打印间隔，需要重新编译并烧录一次，然后后续继续用本脚本重复测试。现有 `run_no_protocol_size_tests.ps1` 仍用于这种配置变更，会编译并烧录；重复测试请使用 `repeat_300k.cmd`。

默认 RX RTT 地址为 `0x20000048`，TX 为 `0x20000050`，对应当前固件。更换固件导致 RTT 地址改变时，使用各自构建产物 `.map` 中 `_SEGGER_RTT` 的地址：

```powershell
.\repeat_300k.cmd -RxRttAddress 0x20000048
```

设备或工具路径变化时可传 `-TxSerial`、`-RxSerial`、`-JLinkDirectory`。失败的原始日志会保留；脚本不会通过擦除或重新烧录来恢复连接。
