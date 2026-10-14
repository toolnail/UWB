# 无协议点对点链路：RSSI 与首路径功率记录

测量已接入 `Drivers/API/Src/examples/ex_02a_simple_rx/simple_rx.c`，实现位于同目录的 `rx_signal_metrics.h`。
沿用 `ex_01a_simple_tx/simple_tx.c` 的帧格式，无需修改发包内容。

## 测量范围和输出

- 每个首次通过 CRC、应用负载校验且序号在范围内的 DATA 包都测量一次；重复包、结束标记、无效包和 PHY 错误事件不进入功率统计。
- 接收端启用 `dwt_configciadiag(DW_CIA_DIAG_LOG_ALL)`，使用 Ipatov 前导码诊断。没有开启 STS 时仍可使用。
- 接收端使用手动双缓冲和中断。回调在重新开启接收前保存当前帧的 Ipatov 诊断和 DGC；随后重新开启接收，将上一帧复制到独立队列槽位。SDK 在回调返回后释放硬件缓冲区，主循环完成计算、校验、统计和打印。
- `rx_frame_queue.h` 是默认 64 槽的单生产者/单消费者队列，使用内存屏障发布完整记录；主循环处理完才释放槽位，队列满时丢弃新帧而不覆盖旧记录。它与下文允许覆盖已处理历史的 `rx_signal_history` 不同。
- 诊断只读取所需的六个 Ipatov 寄存器（24 字节）和 DGC；后台使用保存的 DGC、前导码和芯片量化系数调用 SDK 纯数学函数，不再读取实时硬件状态。CIA 未完成或出错时标记该帧诊断无效。
- `RX_SIGNAL`：详细采样日志，含序号和诊断原始值。默认打印第一个有效包，之后每 1000 个有效包打印一次。
- `RX_SIGNAL_STATS`：每次原有 `RX_PROGRESS` 后输出累计功率统计。
- `RX_SIGNAL_COMPLETE`：结束时输出整轮功率统计，在原有 `RX_COMPLETE` 前输出。
- `RX_SIGNAL_CONFIG`：启动时报告当前开关、详细日志间隔和历史容量。
- 逐包完整记录通过 RTT 通道 1 单独输出，不改变上述文本日志。`repeat_300k.cmd` 会生成 `round_1.rx.rssi.csv`（全部成功 DATA 接收，重复包单独标记）及完整性 JSON。原有 `rx.signal.csv` 仍是抽样日志，二者不能混淆。详见 `README_REPEAT.md`。
- `RX_MODE`：芯片 ID、双缓冲模式及队列大小；`RX_PIPELINE`：队列峰值、队列满丢弃、硬件溢出、采集丢弃、恢复后旧记录丢弃、接收重启失败和恢复 epoch。

| 字段 | 含义 / 单位 |
| --- | --- |
| `rssi_dbm` | 总接收信号功率估计，dBm，以逐帧快照调用 `rsl_calculate_signal_power()` |
| `fp_power_dbm` | 首路径信号功率估计，dBm，以逐帧快照调用 `rsl_calculate_first_path_power()` |
| `gap_db` | `RSSI - 首路径功率`，dB |
| `fp_index` | 首路径在 CIR 内的采样位置，不是距离或绝对到达时间 |
| `fp_index_q6` | 首路径位置原始 Q10.6 值，除以 64 得到采样位置 |
| `f1_raw / f2_raw / f3_raw` | 首路径三个幅度诊断值，包含 2 个小数位；除以 4 得到幅度，不是 dBm |
| `cir_power` | CIR 总功率诊断寄存器值，不是 dBm |
| `accum_count` | 累积的前导码符号数 |
| `dgc` | 当前接收包的 DGC 决策值 |
| `diag_valid` | 1 表示两项功率估计有效；0 时功率字段输出 `NA` |
| `reception_id` | 每次硬件成功收帧的本地编号；相同 sequence 的重复接收具有不同编号 |

累计字段包括 `samples / invalid`、`rssi_avg_dbm / min / max`、`fp_avg_dbm / min / max`、`gap_avg_db`。
`samples + invalid` 等于结束时的成功唯一 DATA 包数量。CIA 未完成/出错、零功率、零累积符号和 SDK 的 `SHRT_MIN` 无效哨兵不进入均值；没有有效测量时输出 `NA`。
均值是 dB 数值的算术平均，并非先对线性功率平均再换算 dBm。内部使用 Q8.8 和 64 位累加，打印两位小数用于读取与导出，不代表硬件具有 0.01 dB 精度。

`gap_db` 可用于观察首路径与总功率之间的差异；不能单独据此判定 LOS/NLOS。RSSI 和首路径功率是芯片诊断估计值，实际精度受接收强度、天线、摆放和校准影响。

## 记录与日志频率

默认 RAM 环形记录保留最近 64 个测量包，在调试器中查看 `rx_signal_history` 和 `rx_signal_history_next`。
下一条写入的位置由 `rx_signal_history_next` 指示；满容量后覆盖最旧记录。累计统计覆盖整轮，不受详细日志抽样间隔影响。

可在 `Drivers/API/Src/example_selection.h` 中增加宏，然后重新编译接收固件：

```c
#define TEST_SIGNAL_ENABLE 1          // 0: 关闭诊断读取、记录和功率统计
#define TEST_SIGNAL_LOG_EVERY 1000UL   // 1: 逐包打印；0: 只打印累计统计
#define TEST_SIGNAL_HISTORY_SIZE 64U  // 最近记录数，必须大于 0
```

测量会增加 SPI 访问和计算开销。高频发包时，逐包打印会进一步增加接收处理时间。
当前 RTT 使用非阻塞模式、1024 字节上行缓冲，主机未及时读取时可能跳过日志行；逐包日志不能保证是完整的全包记录。整轮统计和 RAM 最近记录仍在接收端维护。
重新烧录后，应重新验证所需发包间隔下的交付率和吞吐量。

## 使用现有实验脚本

原有 `run_no_protocol_size_tests.ps1` 已加入参数并自动导出信号 CSV：

发包数量统一读取 `Drivers/API/Src/p2p_test_config.h` 中的 `TEST_PACKET_TARGET`，不再通过 `-Targets` 指定。修改这一个定义后，TX、RX 和电脑脚本都会使用相同数量。

```powershell
# 注意：该实验脚本沿用原有行为，会重新构建并烧录 TX/RX，然后运行测试。
.\uwb_test_firmware\run_no_protocol_size_tests.ps1 -IntervalUs 500 -SignalLogEvery 1000

# 逐包详细日志（建议先放慢发包间隔）
.\uwb_test_firmware\run_no_protocol_size_tests.ps1 -IntervalUs 10000 -SignalLogEvery 1

# 仅累计功率统计
.\uwb_test_firmware\run_no_protocol_size_tests.ps1 -IntervalUs 500 -SignalLogEvery 0

# 关闭功率测量，对照原来的接收性能
.\uwb_test_firmware\run_no_protocol_size_tests.ps1 -IntervalUs 500 -DisableSignal
```

输出的原有实验汇总 CSV 增加了功率统计字段。另外生成：

- `*.signal.csv`：实际收到的详细信号日志行。
- `*.signal_summary.csv`：整轮累计统计，与详细日志行数分别计数。

也可以独立导出已保存的 RTT 日志：

```powershell
.\uwb_test_firmware\export_rx_signal.ps1 -InputLog .\uwb_test_firmware\your_rx.log
```

CSV 中 `NA` 导出为空值。没有结束统计的日志只导出已有的详细行，并提示没有 `RX_SIGNAL_COMPLETE`。

## 构建产物与验证

历史固件、编译产物和实验结果已清理。需要新固件时，由 `run_no_protocol_size_tests.ps1` 重新编译并烧录 TX/RX；它会生成对应的 HEX 文件。板子当前已烧录的程序仍可通过 `repeat_300k.cmd` 重复运行，见 `README_REPEAT.md`。

同时修正了 `dw3000_device.c` 和 `dw3720_device.c` 的 RSSI API 内提取 RX 前导码代码的位运算括号。
原表达式先移位掩码，随后取到了寄存器低位，可能导致误选 16 MHz PRF 的功率校准常数。现在先掩码再移位，与首路径功率接口的提取方式一致。

已验证默认 RX、仅累计统计、关闭测量以及 TX 兼容配置均可编译。
`signal_validation/test_rx_signal.c` 用真实 SDK 定点算法对照参考公式，覆盖 16/64 MHz PRF、DW3000/DW3720 量化系数、负数格式、无效诊断、最大/最小值、300000 包累计和历史回卷。
在当前安装了 Visual Studio 2022 Community 的电脑上执行 `signal_validation/run_host_tests.cmd`，可复现日志间隔为 0、1、1000 的主机测试。
该主机测试中的 300000 是固定压力测试样本数，用于验证累加溢出和历史回卷，不是固件发包数量的配置来源。
CSV 导出已用模拟日志验证负数、`NA`、F1/F2/F3 字段和累计计数；模拟值不代表本次实板测量。
