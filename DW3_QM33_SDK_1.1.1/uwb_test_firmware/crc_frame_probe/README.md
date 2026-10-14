# 自然 CRC 错误接收缓冲区实验

实验代码：`Drivers/API/Src/examples/ex_crc_frame_probe/crc_frame_probe.c`。TX/RX 从同一个独立文件分别编译；不修改原有 simple_tx.c、simple_rx.c、RSSI 代码、共享数量配置、角色选择和原工程文件。

## 正常发送

TX 使用原 simple_tx.c 的 DATA 格式：32 字节主体，字节 0 为 0xC5，字节 1～4 为递增序号，字节 5 为 DATA 类型 0，字节 6～31 为原来的确定性负载。芯片自动生成并追加 2 字节 FCS，发送完成后延迟 500 us。不修改 FCS、不交替制造 GOOD/BAD、不改信道或发射功率，也没有总发包数量和 END 帧；获得证据后通过 RAM 标志停止 TX。

## RX 取证

RX 保留 CRC 校验、单缓冲和手动重新开启接收。每个成功包更新上一条已验证成功帧的完整 34 字节快照。只有遇到真实 RXFCE 时，才额外记录错误数据：

1. 在重新开启 RX 前，固定读取已知帧长 34 字节，避免陈旧的 RX_FINFO 长度导致误读。
2. 清除接收事件标志，再进行一次新的 SPI 读取，两次之间不复位、不重新开启 RX、不写 RX 内存；读取前将 MCU 数组分别填成 0xA5 和 0x5A。
3. 输出两次快照、上一成功帧快照，以及错误后的第一条成功帧序号。
4. 电脑检查上一成功帧的格式和 CRC、错误快照的 CRC、完整帧是否与上一帧相同，以及两次读取是否相同。

自然错误可能连序号、类型和负载一起损坏。因此，程序区分“缓冲区在本次错误事件后更新”和“原始发送帧号能够可靠识别”，不会把错误快照中的序号直接当作合法帧号。

`CURRENT_FRAME_CANDIDATE_UNCONFIRMED` 仅表示序号前后关系和数据模式支持当前帧候选，尚未证实成功恢复错误帧；`BUFFER_CHANGED_SOURCE_UNCONFIRMED` 表示已取得稳定且 CRC 不匹配的不同缓冲区快照，但不能保证快照完整性及归属；`MATCHES_PREVIOUS_FRAME` 表示快照与上一完整成功帧相同。其他情况保留原始记录并标为无法判定。SPI 读到字节不等价于芯片向主机发布了有效、完整的错误帧。

## 运行

以下命令会构建独立工程、烧录两块板子，等待自然 CRC 错误。没有 packets 数量参数；默认观察上限为 600 秒，获得至少 3 个错误快照及后续成功帧后提前停止。`-MinimumCrcEvents` 控制证据事件数量，不是发送包数。

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\uwb_test_firmware\run_crc_frame_probe.ps1
```

仅编译：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\uwb_test_firmware\run_crc_frame_probe.ps1 -BuildOnly
```

如果观察时间内没有出现自然 CRC 错误，不能据此判断“错误帧无法读取”。输出保存在 `run_时间戳/`：`RX.rtt.log` 是原始证据，`crc_evidence.csv` 是逐事件比对，`summary.json` 是汇总，HEX/MAP 和 flash 日志对应烧录固件。停止时写零并读回 TX 的 `crc_probe_run` RAM 标志。

两块板子保留此自然 CRC 实验固件；复位 TX 后会重新连续发送，普通 `repeat_300k.cmd` 不适用于本实验。

## 2026-10-12 自然错误实板记录

当前有效证据目录：`run_20261012_155534_031/`。芯片 ID 为 0xDECA0302。正常 DATA、硬件自动 FCS、500 us 延迟下，记录到 RXFCE，状态为 0x0180AF03，帧长为 34 字节。

错误前成功帧序号为 1190，错误后第一条成功帧为 1194。错误快照的序号字段已损坏，原始解码值 1075840167 不可当作可靠帧号。错误主体有 30/32 字节不同于上一成功帧；清除标志前后读取相同，复算 FCS 为 D2D4，读出的 FCS 为 600E，上一成功帧 FCS 则验证正确。

只能确认本次 RXFCE 后读出的快照不同于上一条完整成功帧，不能据此确认完整错误帧已向主机提供。分类修正为 BUFFER_CHANGED_SOURCE_UNCONFIRMED；原始 SPI 读取的完整性、实际 SYS_CFG 配置及快照归属仍需进一步核验。测试后已读回 TX 停止标志为零，持续发送停止。

2026-10-12 结论复核：用户提供的 Qorvo 论坛讨论 https://forum.qorvo.com/t/can-you-recover-corrupted-data-after-an-rx-err-event/25800 明确称 RXFCE 帧不向主机提供、无文档化机制取得错误负载。当前实验不足以推翻该结论，也不应作为工程中可可靠读取错误 payload 的依据。原始 RTT 日志保留；此前“可完整读取当前错误帧”的表述撤回，汇总和分类标签同步修正。差异原因尚未查明，不将芯片版本、模式差异或 SPI 错误等可能性当作已经确认的原因。

`run_20261012_153558_223/` 是先前人为 FCS 错误的历史实验，不能作为当前自然错误实验的证据。

## 最新复测

见 [2026-10-12 复测核验](verification_20261012.md)。两轮有效实板实验合计获得 5 次自然 RXFCE，实际配置读回与 SPI 校验均通过。第二轮增加了每次开启接收前的芯片缓冲区快照，两次都确认缓冲区在当前接收尝试中更新，其中一次保留了序号 17040，之前的芯片缓冲区完整匹配成功帧 17039。

当前程序的 RX 验证使用 4 MHz SPI 和 SDK SPI CRC 校验；TX 使用普通 SPI 访问方式，正常自动生成无线 FCS。`BUFFER_UPDATED_DURING_CURRENT_RX_ATTEMPT` 表示本次接收期间原始缓冲区更新；`reliable_error_payload_interface_established=false` 表示没有建立厂商支持的、保证每个错误帧完整可读的接口。这两个判断不冲突。

原始日志、逐事件 CSV、独立 CRC 核验、汇总、源文件快照及对应 HEX/MAP 保存在 `run_20261012_164933_833/` 和 `run_20261012_165715_300/`。板子当前保留第二轮实验固件，TX 已停止。

## RX 校验开启/关闭对照（最新）

见 [CRC 开关实板对照](crc_on_off_comparison.md)。只在本独立测试程序中调用 `dwt_configtxrxfcs(DWT_FCS_RX_OFF)` 关闭 RX 校验；原收发源码和驱动不改动。用启动参数 `-ReceiverCrc Disabled` / `-ReceiverCrc Enabled` 选择模式，默认开启。

两个模式均捕获了两条自然坏帧，开启接收前后的芯片缓冲区对照、三次重复读取及 SPI 校验均通过。关闭模式读回 `SYS_CFG=0x000001CC`，开启模式为 `0x000001C8`；关闭模式本次仍有 RXFCE，因此关闭模式用 RXFR 和软件 CRC 核验，不要求 RXFCE 消失。

最新有效记录为 `run_20261012_173029_470/`（关闭）与 `run_20261012_173224_639/`（开启）。两种模式的 TX HEX 完全一致，各轮 13 个原工程/驱动文件哈希均未变化。板子当前保留开启 RX 校验的独立测试固件，TX 已停止；此前章节描述的是历史实验状态。
