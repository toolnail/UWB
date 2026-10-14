# RX FCS 校验开启/关闭实板对照

2026-10-12。只修改独立测试文件 `Drivers/API/Src/examples/ex_crc_frame_probe/crc_frame_probe.c` 和对应独立启动脚本。原点对点 TX/RX、SDK 驱动、配置和原工程未修改。

## 手册与设置

用户指定的 `C:\Users\wangrx\Desktop\research-daily-log\2026_2027\2026_10_6\DW3XXX_Software_API_Guide_4p12.pdf`，第 67 页、第 5.2.45 节：

```c
dwt_configtxrxfcs(DWT_FCS_ENABLE);  /* TX 自动生成 FCS，RX 检查 FCS */
dwt_configtxrxfcs(DWT_FCS_RX_OFF);  /* 只关闭 RX FCS 检查 */
```

未使用 `DWT_FCS_TX_OFF`，未修改 API 函数本身。TX 始终发送原有 DATA 模式，32 字节主体、芯片生成 2 字节 FCS，发送完成后等待 500 us，无发送包数目标，无人为错误注入。两种模式烧录的 TX HEX 的 SHA256 完全一致：`39ee3b92d1b38c1ab9439409daa261c52580d2ecd0f01a6c17b0487aa8b3e792`。

RX 保持单缓冲、手动开启接收，4 MHz SPI，SDK SPI CRC 读写校验。每次开启接收前直接读取芯片缓冲区，完成接收后保持接收器停止，对 34 字节快照读取两次；清除状态后再读取一次。

开启模式等待 RXFCG 或接收错误；关闭模式同时允许 RXFR 表示接收完成。MCU 对所有接收快照计算无线 FCS，电脑再次复算。软件 CRC 只用于实验判定，不丢弃坏帧快照，不修改原点对点程序。

## 实测结果

| 项目 | RX 检查关闭 | RX 检查开启 |
|---|---|---|
| 有效实验目录 | `run_20261012_173029_470/` | `run_20261012_173224_639/` |
| 实际 SYS_CFG | `0x000001CC` | `0x000001C8` |
| DIS_FCE | 1 | 0 |
| DIS_FCS_TX | 0 | 0 |
| 捕获坏帧快照 | 2 | 2 |
| 本次接收前基线匹配上一成功帧 | 2/2 | 2/2 |
| 本次接收期间缓冲区发生更新 | 2/2 | 2/2 |
| 三次读取一致 | 2/2 | 2/2 |
| SPI 读/写校验错误 | 0/0 | 0/0 |
| 快照的软件 FCS 不匹配 | 2/2 | 2/2 |
| RXFCE=1、RXFCG=0、RXFR=1 | 2/2 | 2/2 |

这四个事件均为状态 `0x0180AF03`，读回帧长 34，读取期间 `SYS_STATE=0x00030000`。关闭检查后，本次观察仍然出现 RXFCE，不能把“RXFCE 必须消失”作为设置生效的条件；设置通过 DIS_FCE 实际读回和 SPI 校验验证。

SDK 的 `dw3000_device.c` 在 `ull_isr()` 中有 `rx_fr_dis_fce` 分支：当 RXFR 置位且 DIS_FCE 的缓存值为 1，会进入 RX OK 处理分支。这是 SDK 源码中的处理规则；本实验采用轮询并保存原始状态，没有调用这个 ISR 分支来宣称坏帧数据正确。

| RX 设置 | 事件 | 接收前成功帧 | 错误快照中的序号字段 | 后续成功帧 | 比接收前变化的字节 / 34 | 快照复算 FCS / 读出 FCS |
|---|---:|---:|---|---:|---:|---|
| 关闭 | 1 | 7753 | 7754 | 7759 | 29 | 23BF / 93F6 |
| 关闭 | 2 | 8211 | 16785428，字段损坏 | 8217 | 30 | B50B / B086 |
| 开启 | 1 | 2869 | 352324406，字段损坏 | 2875 | 30 | 89F0 / 26D8 |
| 开启 | 2 | 3011 | 3012 | 3017 | 28 | EE75 / 1D8A |

序号字段符合前后关系的两条快照，分别保留了 7/26 和 13/26 个符合其序号模式的负载字节，其余字节已损坏。另两条连序号也损坏，不能据此识别原始 TX 帧号。四次直接的开启接收前后对照都能证明缓冲区在当前接收尝试中更新，读出的不是上一完整成功帧。

## 结论与核验

当前两块 DWM3000EVB 的这四个自然错误样本中，开启和关闭 RX 校验都能读到当前接收尝试期间更新的损坏原始缓冲区快照。关闭 RX 校验并未修复数据，也未在这些样本中消除 RXFCE；软件复算仍然能检出 CRC 不匹配。

这是两个独立时间段的自然错误样本，不能把不同快照的损坏程度当作同一帧在两种模式下的差异，也不能据此保证所有 CRC 错误帧都能完整读取。本实验比较错误快照可读性，不用于比较正式点对点程序的成功率。

每个有效目录中保留 `RX.rtt.log`、`crc_evidence.csv`、`summary.json`、源码和启动脚本快照、HEX/MAP、烧录与停止日志。`independent_crc_check.json` 使用 Python 标准库 `binascii.crc_hqx` 加位反转复核 FCS，独立于 MCU 的逐位实现，并验证已知向量 `123456789 -> 2189`。逐事件合并核验见 `crc_on_off_comparison.json`。

两轮 `original_files_check.csv` 均确认 13 个原工程/驱动文件的 SHA256 未变化。两轮 TX 都已停止并读回 RAM 标志为 0，当前板子保留开启 RX 校验的独立测试固件。

`run_20261012_172802_068/` 是第一次关闭模式的原始观察：发现 DIS_FCE=1 时仍出现 RXFCE，电脑脚本当时因预设标志必须消失而提前停止。修正判据后进行了上述完整对照；第一次观察没有计入表中的两条关闭模式记录。

## 重复测试

以下命令只构建和烧录独立测试固件，不修改原工程。`MinimumCrcEvents` 是所需坏帧证据数量，不是 TX 发送包数；自然错误不足时，观察会在 600 秒截止。

```powershell
# RX 校验关闭
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\uwb_test_firmware\run_crc_frame_probe.ps1 -ReceiverCrc Disabled -MinimumCrcEvents 2

# RX 校验开启
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\uwb_test_firmware\run_crc_frame_probe.ps1 -ReceiverCrc Enabled -MinimumCrcEvents 2
```
