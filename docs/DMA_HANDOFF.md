# 触摸 DMA：现状、证据、下一步（2026-10-28 打通，见文末）

**这条线已经结束：触摸可用、多指可用、固件 216 s 没掉过。**
结论与"已知好镜像"的 sha256 在文末「2026-10-28 12:4x — 通了」一节；
只想知道怎么复现的直接跳那里，中间是这一路的证据与作废的假设。

下面是 2026-10-27 当时的起点（保留原样，便于对照）：触摸本体当时已经能跑
（见 `NOTES.md` 第 183/185 步），但**固件会在一段时间后停摆**；
DMA 的意义是让固件下载和读帧都变成硬件时序，同时顺便解决"位敲靠 pace 猜"的脆弱性。

## 已经确证的事实（都有日志/寄存器读数）

1. **触摸 SE 与 SMMU 的对应关系**（vendor DTB `dtb/boot_unpacked/dtb`）：
   ```
   qcom,qupv3_0_geni_se@9c0000   reg 0x9c0000+0x2000
       iommus = <0x5b 0x5a3 0x0>        → apps_smmu stream 0x5A3
   spi@990000 (SE4) 就在这个 wrapper 下
   apps-smmu@15000000  compatible "qcom,qsmmu-v500"
       reg = <0x15000000 0x100000>, <0x151ce000 0x20>
       qcom,skip-init; num-context-banks-override = 0x4e; num-smr-override = 0x78
       qcom,handoff-smrs = <0x2800 0x402>
       TBU: anoc_1@0x151d1000, anoc_2@0x151d5000, … mdp_1@0x151fd000（每个 0x1000，
           另有 8 字节在 0x151ce2xx）
   ```
2. **apps_smmu 的寄存器是可读的**（touch135-smmuprobe 日志，四个窗口都读成功）：
   ```
   tcru:          [0x00] 0x00000000 [0x20] 0x00000000
   anoc_1_tbu:    [0x00] 0x90520000 [0x20] 0x00100000
   kgsl_smmu:     [0x00] 0x002e0406 [0x20] 0x4c017e18
   apps_smmu_gr0: [0x00] 0x00200406 [0x20] 0x4c017e71
   ```
   也就是说之前 touch132 的 `esr 0x96000010`（同步外部中止）**不是访问控制**——
   那次差别有两点：(a) 先做了 0x85200000 的 DMA 页 `ml_io_map` + 清零；(b) SMMU 用了 64 KiB 映射。
   **下一轮要先读 SMMU、再碰 DMA 页**，并且先用 16 KiB 映射。
3. ~~**qsmmu-v500 按 stream id 索引 S2CR**：`S2CR(n) = 0x15000000 + 0x8000 + 4*n`，
   所以要改的是 **0x1500968C**；TYPE 位 [1:0]：0=translate，1=bypass，2=fault。~~
   **这条已被硬件推翻（2026-10-27，touch145/touch146）**，实测布局是：
   - `0x8000+` 是 **global space 1**，读 0x1500968C 触发同步外部中止
     （`esr 0x96000010`，PC 停在 `ldr`），而同一个映射里 SCR0/IDR0 正常
     （0x00200406 / 0x4c017e71）。GPU 那边早就踩过同一个坑（`GpuPowerBoard.h`：
     "the apps_smmu abort happened on the first gr1 read"）。**永远不要碰 0x8000+。**
   - **global space 0** 里：`SMR = 0x800 + 4*n`（VALID=bit31、MASK=[30:16]、ID=[15:0]）、
     `S2CR = 0xc00 + 4*n`（**TYPE=[17:16]**、CBNDX=[7:0]——不是 TYPE=[1:0]/CB=[15:8]！）。
   - 条目数 = `IDR0[7:0]` = **0x71 = 113**（0…112）。读第 113 项就是 touch146 的 panic：
     `far = 窗口 + 0x9c4`、`esr 0x96000010`、PC 停在 SMR 的 `ldr`。扫描必须**严格小于**这个数。
   - S2CR 按 **stream-match 条目**编号，不是按 stream id；找 0x5A3 要扫 SMR[] 里
     valid 且 id/mask 匹配的那一项。实测表里：`SMR[0]` = valid/id 0，`SMR[3]` =
     `0x84022800`，正好是 **显示** 那条流（DT 里 `qcom,smmu_sde_unsec_cb` 的
     `iommus = <0x5b 0x2800 0x402>`，也正是 `qcom,handoff-smrs = <0x2800 0x402>`）；
     `S2CR[i]` = CBNDX i、translate。**0x5A3 在前 16 项里没有匹配**（全表待确认）。
4. **DMA 页**：`tch_dma_alloc()` 用固定物理页 `0x85200000`（cdsp_secure_heap 结束、video 开始之间
   的空窗 0x85200000–0x856FFFFF，5 MiB），映射 2 页 32 KiB：TX @+0x0000、RX @+0x4000。
   低 4 GiB 的另一块空窗是 0x85E00000–0x87FFFFFF（34 MiB），可作备选。
5. **panic 已解决**：`flush_dcache64(PA)` 会走 `phystokv_range()`，对 ADT DRAM 之外的 PA 直接 panic；
   Device 映射（`ml_io_map`）本来就不需要 cache 维护，已删掉两处 flush，并用逐字节 volatile 访问
   （`ml_io_map` 上不能用 `memset`，会 unaligned abort）。
6. **最近一次 DMA 的现象**（touch129）：`DMA #1 issuing opcode 7` 之后整机静止，无 panic。
   CS 命令正常（`irq 0x1`）、DMA 页映射/清零都打印过，所以死点在**第一条 DMA 数据命令**之后。
7. 位敲（不用 DMA）在 pace 正确时**能出帧**：`fastbb` 那一轮 172 个触点帧、
   `event 0..150 dispatch ok`；所以 DMA 只影响"稳不稳/快不快"，不影响协议本身。

## 下一轮建议的改动顺序（都在 `bsd/dev/liuqin_touch.c`）

1. `tch_spi_setup()` 的 `liuqintouchdma=1` 分支改成：
   ```
   tch_smmu_fix();          // 先读（并可选写 bypass），16 KiB 映射
   tch_dma_alloc();         // 之后才映射/清零 0x85200000
   tch_se_start();          // 探测 + 第一条 1 字节 DMA
   ```
   现在代码里是 `tch_dma_alloc()` 在前（touch132 的顺序），这就是可疑点。
2. 启动行加 `liuqintouchsmmu=2`（读 + 若非 bypass 就写 bypass），并逐行上屏（已经做了：
   `tch_visible_mark` 会保留 `liuqin_console_force`）。
3. 第一条 DMA 之前打印 S2CR 的当前值与写入后的值（**注意**：不再按 SID 读 0x968c，
   改成 SMR/S2CR @0x800/0xc00，见上面第 3 条的更正）；DMA 失败时打印
   `GENI_STATUS` / `M_IRQ` / TX/RX 状态（已有），再加 `SE_GENI_DMA_MODE_EN` 与 `SE_DMA_GENERAL_CFG`。
4. 若 `DMA #1 issuing` 仍冻机：改用 **0x85E00000 窗口**（34 MiB）重试；仍冻则把
   `SE_SPI_LOOPBACK=1` 打开做一次内部回环 DMA，区分"存储路径"还是"SPI 从机"。
5. 若能看到 `TX probe … ok`：把 `tch_xfer()` 的 DMA 分支上限从 0x3f00 用到固件下载
   （`tch_fw_write_sram` 每 256 B 一次调用也会自动走 DMA），下载会变成秒级且无位错。

## 不要踩的坑

- `delay()` 在这台机器上固定约 16 µs，位敲的 pace 必须用**校准自旋**（已实现，`bb_spin_per_us`）。
  自旋标定公式曾经多乘 1000，导致实际比标称快 2.4 倍、镜像静默损坏——现在已修，
  并且下载后会**整段回读校验**（128 B 块，因为位敲 `tch_xfer` 上限 256 B）。
- `fw verify: image identical to the file (201410 bytes)` 是"镜像正确"的唯一证据；
  BLD CRC 寄存器只反映最后一段（`dlm cd21b90f` 不是错误）。
- 恢复逻辑不能按"静默"判死：面板空闲与停摆在总线上没有区别。只有"**中断来了却没有帧**"是证据。
- 构建树已搬到 `~/liuqin-build3`（`/tmp` 会被系统清空，今天丢过两次）：
  `WORK=~/liuqin-build3 ./tools/build-local.sh`，链接器 `LLD=~/liuqin-build3/ld.lld`。
  快照在 `~/liuqin-root/wip/2026-10-27/`（`xnu-modified.diff` + `xnu-untracked-files.tgz` + `liuqin_touch.c`）。

## 本轮结果（2026-10-27）

**17:20，主机侧**：第 1–3 步实现并编译通过；第 4 步的两个杠杆做成启动参数
（`liuqintouchdmaphys=0x85e00000`、`liuqintouchloopback=1`）。当时出了两张镜像
`shim-boot-touch145-dma-smmu.img` / `…-altpage.img`。

**17:2x，上机（touch145）——第一步就 panic，原因是本文第 3 条写错了**：

```
LIUQIN TOUCH: liuqintouchdma=1, trying SE DMA again
Kernel data abort at pc … (esr 0x96000010, far window+0x68c)
```

读 `0x1500968c`（gr1）触发了同步外部中止；同一个映射里 SCR0/IDR0 都读成功
（`x8 = 0x00200406`、`x22 = 0x4c017e71`），所以映射没错，是那个地址不存在。
**touch145 那两张镜像作废**（它会 panic）。

**17:30，修正（touch146）**：`tch_smmu_fix()` 只碰 global space 0——扫 `SMR[0..127]` /
`S2CR[0..127]`（0x800 / 0xc00），打印非零项，对 valid 且 id/mask 匹配 stream 0x5a3 的
那一项把 S2CR 的 TYPE 改成 bypass。镜像 `shim-boot-touch146-dma-gr0.img`（`2505a2c3…`）。

**17:3x，上机（touch146-gr0）——表出来了，但扫描越界，又 panic**：

```
LIUQIN TOUCH: SMMU gr0 scr0 0x00200406 idr0 0x4c017e71 (NUMSMRG field 0x71), SMR@0x800 S2CR@0xc00
LIUQIN TOUCH: SMMU [0..15] SMR … S2CR …
Kernel data abort (esr 0x96000010, far = 窗口 + 0x9c4)
```

三个结论（都有寄存器/反汇编对照）：
1. 表是真的：`SMR[0]` valid/id 0，`SMR[3] = 0x84022800`（显示流 0x2800/0x402，
   对应 DT 的 `qcom,handoff-smrs`），`S2CR[i] = CBNDX i` translate。
2. **条目只有 113 个**（`IDR0[7:0] = 0x71`）；读第 113 项就是这次的 panic
   （`far = SMR(113)`）。所以扫描上界是 `IDR0[7:0]`，不是 128。
3. 我上一条把 `S2CR.TYPE` 当成 [1:0] 也是错的：**TYPE=[17:16]、CBNDX=[7:0]**，
   所以之前打印的 "type" 列全是假的。

**17:42，修正（touch147）**：扫描严格小于 `IDR0[7:0]`；按正确字段解码；
打印所有 id≠0 的 valid SMR（至多 12 条）+ 计数。分流：
1. 有匹配 0x5A3 的 SMR → 把它的 S2CR TYPE 改成 bypass；
2. 没有 → 取一条 **all-zero**（SMR 与 S2CR 都 0）的条目，写 `SMR = VALID|0x5A3`、
   `S2CR = bypass`；
3. 连 all-zero 条目都没有 → 复用一条“valid / id 0 / mask 0”的冗余条目
   （它只匹配 stream 0，最低的那条保留，路由不变）。
写入后都回读打印。两张镜像：

- `shim/out/shim-boot-touch147-smmu2.img`（`ba35b070…`）= 表 + 上面的 1/2/3
- `shim/out/shim-boot-touch147-smmu0.img`（`9275fda8…`）= **对照组**：完全跳过 SMMU
  （`liuqintouchsmmu=0`），用来判断“还冻”到底是不是 SMMU 的锅
- 内核 `65d94d9a…`；`shim/out/shim-boot.img` = smmu2

**判据**：先启动 smmu2，看
1. `SMMU … entries: N valid (M match only stream 0) …` 后面是
   `matches SMR[..]` 还是 `no valid SMR … programming …`，以及回读是否生效；
2. `DMA #1 issuing` 之后有没有 `DMA #1 came back … tx …`。
若 smmu2 仍冻，再启动 smmu0：它也冻 → 不是 SMMU（回去查 SE DMA 描述符/时钟）；
它不冻 → 说明确实是流匹配的问题，下一轮就针对写进去的那条 SMR 调。

**2026-10-27 17:5x，touch147 上机结果：smmu2 与 smmu0 都硬挂重启，无 panic。**
注意 `liuqintouchsmmu=0` 只是不碰 SMMU 寄存器，**DMA 仍然带 SID 0x5A3 过 SMMU**，
所以这不等于"SMMU 无关"；只说明挂点在 SMMU 探测之后。怀疑对象改为：

1. **QUP wrapper 的 AHB 主时钟**：位敲/寄存器读写只证明从端口有电，DMA 要 AHB master
   读写内存。`GCC_QUPV3_WRAP0_CBCR`(0x62008) 的 bit 6/7/8/9 = m_ahb/s_ahb/core/core_2x，
   驱动从来没开过（只开过 S4）。主端口被 gate → 第一条 DMA 事务不完成 → 无 panic 硬挂。
2. **DMA 页 0x85200000 是否真是内存**：它是 DTB reserved-memory 之间的空窗推出来的；
   本移植 ADT 的内核 DRAM 从 0x100000000 开始，低 4 GiB 是固件 carveout。PA 没有内存响应
   时，QUP 的第一条事务一样会挂死。

touch148 为此加了两个探针：进 DMA 分支先打印/补齐 `QUP CBCR`（`tch_qup_dma_clocks()`），
`tch_dma_alloc()` 里对页 0 和 +0x4000 写 pattern 再回读（`DMA page self-test`）。
镜像 `shim-boot-touch148-smmu2.img`（`218f8008…`，正常 bypass/补条目）与
`shim-boot-touch148-smmu1.img`（`28ca5eeb…`，只读 SMMU，保证能走到新探针）；内核 `4414408f…`。
**下一次日志里先看 `QUP CBCR` 原本哪些位是 0、`forced` 后回读变没变，再看自检和 DMA。**

**2026-10-27 17:5x，拿到 touch147-smmu2 的日志尾部**（关键，推翻了"已排除"的结论）：

```
SMMU gr0 scr0 0x00200406 idr0 0x4c017e71, 113 entries
SMMU [3] SMR 0x84022800 id 0x2800 mask 0x402 …      ← 显示流，和 handoff-smrs 一致
SMMU 113 entries: 6 valid (1 match only stream 0), 0 all-zero
SMMU no valid SMR matches stream 0x5a3
SMMU no entry to program; leaving the table alone    ← 关键：bypass 根本没写
DMA #1 tx+rx len 1 tx 0x85200000 rx 0x85204000 mode 1
DMA #1 issuing opcode 1                              ← 之后硬挂重启，没有 came back
```

- 扫描本身成功了（113 项上限是对的）；表里确实没有 0x5A3 的 SMR。
- 但"空槽"判据要求 `SMR==0 && S2CR==0`，这张表的无效条目 S2CR 都等于自己的下标（非 0），
  107 个可用槽全被拒 —— **bypass 从未生效**，所以 "smmu0 也冻" 不能说明 SMMU 无关。
- DMA 页的映射/清零都正常打印过（CPU 写 0x85200000 没挂），挂点精确在 `DMA #1 issuing` 之后。

**touch149**：把"空槽"改成 **SMR.VALID == 0 即可**（SMR 无效时那条 S2CR 不会被任何流使用），
写入 `S2CR=bypass` 再写 `SMR=VALID|0x5A3` 并回读；同时保留 touch148 的
`QUP CBCR`（补 m_ahb/s_ahb/core/core_2x）和 `DMA page self-test`。
镜像 `shim-boot-touch149-smmu2.img`（`bbed2f69…`，写 bypass）与
`shim-boot-touch149-smmu1.img`（`bc6bd7ac…`，只读，用来区分"时钟修好"还是"bypass 修好"）；
内核 `061829e0…`。

**下一次看**：`QUP CBCR` 原始位 → `forced` 回读 → `DMA page self-test` →
`programming free entry N` + `readback SMR … S2CR …`（有没有生效）→
`SMMU SMR[n] before DMA #1` → `DMA #1 came back`。

**2026-10-27 18:0x，touch149 上机：时钟、DMA 页都排除，问题落在 SMMU 的 BYPASS 上。**

```
QUP CBCR 0x4002cbc0 [m_ahb 1 s_ahb 1 core 1 core2x 1 s4 1]      ← 四个时钟本来就是开的
SMMU writing S2CR[6] = 0x00010000, SMR[6] = 0x800005a3 (bypass …)
SMMU readback SMR 0x800005a3 S2CR 0x00020000                     ← BYPASS 被写成 FAULT
DMA page self-test ok                                             ← 0x85200000 是真内存
DMA #1 issuing opcode 7 → 硬挂
```

这是 **qsmmu 的已知怪癖**，不是我们算错：`arm-smmu-qcom.c` 的 `qcom_smmu_cfg_probe()` 会写一次
`S2CR TYPE=BYPASS` 探测，如果回读不是 BYPASS 就置 `bypass_quirk`，然后**保留最后一个 context bank**，
把它的 `CBAR` 设成 `CBAR_TYPE_S1_TRANS_S2_BYPASS`、`SCTLR=0`，要 bypass 的流用
**TYPE=TRANS + 这个 CB** 指过去（`qcom_smmu_write_s2cr()`）。换句话说这颗 SMMU 不能真 bypass，
只能借一个"翻译被关掉的 CB"。

**touch150**（`tch_smmu_bypass_cb()`）就是这么做的：从 IDR1 读几何（`PAGESIZE`/`NUMPAGENDXB`/`NUMCB`），
算 `CBAR(n) = (1<<pgshift) + 4n`、`CB(n) = (numpage+n) << pgshift`，取 `CB = NUMCB-1`（DT 的 78 → 77），
写 `CBAR = 0x00010000`、`CB.SCTLR = 0`，再把 `S2CR[slot] = CB`（TYPE=TRANS）、
`SMR[slot] = VALID|0x5A3`，每一步都有回读。镜像 `shim-boot-touch150-bypasscb.img`（`934e5b7e…`），
内核 `af3ca3e7…`；`liuqintouchbypass=0` 可关。

**下一次看**：`SMMU quirk: idr1 … -> bypass CB …` → `CBAR … (before)` → `CBAR … (after)` →
`SCTLR … (before)/(after)` → `readback SMR 0x800005a3 S2CR 0x…4d` → `DMA #1 came back`。
若在 `CBAR … (before)` 处 panic，说明 gr1 也进不去，这条路就只能换 GPI DMA。

**2026-10-27 18:2x，touch150 上机：SMMU 这条线通了，但 DMA 仍然不完成——而且现在能证明挂在引擎侧。**

```
SMMU quirk: idr1 0x6000004b (pgshift 12, numpage 128, numcb 75) -> bypass CB 74, CBAR at +0x1128, CB at +0xca000
SMMU CBAR 0x00000000 SCTLR 0x0080270e (before)
SMMU CBAR 0x00010000 SCTLR 0x00000000 (after)          ← 写入生效，gr1 可访问，没有 panic
SMMU writing S2CR[6] = 0x0000004a (translate via bypass CB 74), SMR[6] = 0x800005a3
SMMU readback SMR 0x800005a3 S2CR 0x0000004a            ← TYPE=TRANS、CBNDX=74
DMA #1 came back: status 0x00000041 irq 0x00000000 tx 0x00000000 rx 0x00000000 after 1000 polls
```

- **CB 号以硬件 IDR1 为准**：`NUMCB = 0x4b = 75` → CB 74（DT 的
  `num-context-banks-override = 0x4e` 只在这个端口不适用时才算数）。几何：`pgshift 12`、
  `numpage 128` → `CBAR(74) = 0x1000 + 0x128 = 0x1128`、`CB(74) = (128+74) << 12 = 0xca000`，
  都落在 1 MiB 块内，与打印一致。
- **行为变了**：touch149（TYPE 被写成 FAULT）是整机硬挂；touch150 是**等满 1000 次轮询正常返回**，
  并且 `irq 0x00000000`、`tx/rx 的 DMA IRQ 状态全 0`。也就是说**既没有 DMA_DONE，也没有 AHB 错误**，
  连一个 GENI 中断都没有。`status 0x41` 与一次成功的 `CS_ASSERT` 之后的稳态值完全一样
  （`M_GENI_CMD_ACTIVE | bit6`），即**序列器把这条数据命令一直握在手里**。
- 五次重试（restore/cancel/abort/clear-disable/start-trigger）数字一模一样，之后 SE 拒绝一切命令。
  结论：**引擎根本没有发起（或发起后永远等不到）那条内存事务**——SMMU 已经不在嫌疑名单上。

### 对照 Linux 源码后的三个结论

1. **`M_CMD0` 参数没有 "M_CMD_DMA" 这一位**（`spi-geni-qcom.c` 里只有
   `SPI_PRE_CMD_DELAY/TIMESTAMP_BEFORE/FRAGMENTATION/TIMESTAMP_AFTER/POST_CMD_DELAY`，
   `FRAGMENTATION = BIT(2)`）。DMA 与否**只由 `SE_GENI_DMA_MODE_EN` 决定**。
   我们写 `(opcode<<27) | FRAGMENTATION` 与上游等价，这一项没问题。
2. 描述符寄存器的写法（`SE_DMA_TX_IRQ_EN_SET` → `PTR_L/H` → `ATTR=1` → `LEN`，RX 的 `ATTR=0`）
   与 `geni_se_tx_init_dma()` / `geni_se_rx_init_dma()` 逐条一致，也没问题。
3. **缺口在 QUPv3 的 SE 控制块（0x2000 段），这是所有轮次都没读过的寄存器**：
   `geni_load_se_fw()` 在设置引擎传输模式时明确写
   `SE_DMA_IF_EN@0x2004 |= DMA_IF_EN`（注释就是 "Serial engine DMA interface is enabled"）
   和 `SE_FIFO_IF_DISABLE@0x2008`。而本机的 `GENI_IF_DISABLE_RO@0x64` 读 **1**
   （FIFO 接口被关），bootloader 留给我们的 `SE_GSI_EVENT_EN@0xe18 = 0xf` 也说明
   这颗 SE 当初是按 **DMA/GSI** 装载的。**FIFO 接口关掉、DMA 接口没开**时，
   数据命令既取不到 TX 字节也存不下 RX 字节——正是"命令永远 active、零中断"的症状。

另一处与上游的差别是**顺序**：2024 年的重构之后，上游是**先写 `M_CMD0`、再写描述符寄存器**；
我们一直是先描述符后命令。

### touch151：一次镜像同时验三件事

1. `tch_se_dma_if()`：打印 `clk_ctrl(0x2000) dma_if_en(0x2004) ro(0xe20) fifo_if_disable(0x2008) gsi_ev(0xe18)`，
   `DMA_IF_EN` 为 0 就置 1 并回读；`liuqintouchdmaif=0` 只观察不写。
2. `liuqintouchdmaorder=1`，以及探测链里新增的 **`cmd-first`** 一步（先 `M_CMD0`、后描述符）。
3. 失败时的取证：`SE_DMA_DEBUG_REG0(0xe40)`（TX/RX active 与状态机）、
   `SE_DMA_TX/RX_LEN_IN`（**实际搬了多少字节**）、`SE_DMA_TX/RX_IRQ_EN`、
   TX/RX 暂存区前 4 字节（RX 在命令前被清零，**只要有一个字节变了就证明 DMA 真的跑过**，
   哪怕完成信号没来）、以及 bypass CB 的 `FSR/FAR/FSYNR0`（编程 CB 时会把陈旧 fault 清掉再打印）。

产物：内核 `bcd67a63…`、payload `a5b31e4c…`、
`shim/out/shim-boot-touch151-dmaif.img`（`e338028c…`，`shim/out/shim-boot.img` 同一张），
启动行与 touch150 完全相同（567 B，结尾 `liuqintouchdma=1 liuqintouchsmmu=2`）。
补丁：`xnu/patches/0221-liuqin-touch-dma-interface.patch`（13 个 hunk，逆向校验通过）。

### 下一次日志怎么读（按这个顺序）

| 看到 | 含义 | 下一步 |
|---|---|---|
| `dma_if_en 0x0 -> 0x1 (set)` 且 `DMA #1 came back … tx 0x1 rx 0x1` | 就是它 | 直接用 DMA 下固件（每 256 B 一次），跑帧率 |
| `dma_if_en 0x1`（本来就开着）且仍然失败 | 引擎使能不是原因 | 看下面三行 |
| `state: … tx_active 0 rx_active 0` + `len_in tx 0 rx 0` | 引擎一个字节都没搬 | 怀疑 SE 的固件/原语表是按 GSI 装载的（`gsi_ev 0xf`），转 GPI DMA |
| `len_in tx 1 rx 1` 但没有中断 | 数据搬了，只是完成信号没来 | 不该再信 `DMA_DONE`，改轮询 `LEN_IN`/`SE_DMA_DEBUG_REG0` |
| `SMMU cb fsr` 非 0、`far` ≈ 0x85200000 | 流还是被拒 | CBAR/CB 的序号或映射不对 |
| `cmd-first` 那一步成功 | 只是顺序问题 | 把默认顺序改成 1 |

---

## 2026-10-27 21:0x — touch151 崩在 SE+0x2004；touch152 改做上游的"退出 GSI 事件模式"

**上一节那张以 `dma_if_en` 为核心的判据表作废**：这颗 QUP 是 v3.0，`0x2000` 段不存在，
`SE_DMA_IF_EN` 根本读不到（读它就 panic）。

### 这轮确认的事实（都有日志或反汇编出处）

1. **panic 的精确位置**：`pc 0xfffffe00075142ac` 落在 `liuqin_touch_setup_thread` 里，
   指令是 `ldr w21, [x8, #8196]`（`x8 = tch.se` 取自 `[x20, #1456]`），即 **SE+0x2004**。
   `esr 0x96000010` 是 synchronous external abort（DFSC=0x10），不是缺页：
   VA 映射是好的，`0x992004` 这个总线地址没有器件应答。日志里没有
   `SE4 clk_ctrl …` 那一行，正好说明崩在这条读上（编译器把 0x2004 排在了最前面）。
2. **QUP v3.0**：日志 "`QUP hw 0x30000000 (v3.0)`"。`SE_GENI_CLK_CTRL@0x2000`、
   `SE_DMA_IF_EN@0x2004`、`SE_FIFO_IF_DISABLE@0x2008` 是后来 "loadable SE" 才有的
   （上游 `geni_load_se_firmware()` 只在 `proto == GENI_SE_INVALID_PROTO` 时走）。
   能安全读的只有头文件标了 RO 的 `DMA_IF_EN_RO@0xe20`。
3. **DMA 模式不是"置一个位"**（`qcom-geni-se.c` 当前版本）：
   - `geni_configure_xfer_mode(GENI_SE_DMA)`：`SE_GENI_DMA_MODE_EN |= GENI_DMA_MODE_EN`、
     `SE_IRQ_EN = 0xf`、`SE_GSI_EVENT_EN = 0`；
   - `geni_se_select_dma_mode()`：先 `geni_se_irq_clear()`（其中写 `SE_GSI_EVENT_EN = 0`），
     再把 `M_CMD_DONE_EN | M_TX_FIFO_WATERMARK_EN | M_RX_FIFO_WATERMARK_EN | M_RX_FIFO_LAST_EN`
     从 `SE_GENI_M_IRQ_EN` 清掉，最后置 DMA 位；
   - GPI 模式正好相反：`SE_IRQ_EN = 0`、`SE_GSI_EVENT_EN = 0xf`。
   我们的引擎从 bootloader 起就是 `0xf`（GPI 配置），touch129..150 只置了 DMA 位，
   **GSI 事件一直开着**：命令被交给没有 TRE ring 的 GSI 块，于是永远 active、零字节、零中断。
4. **0xe18 很可能根本写不进去**：`after init` 的 dump 里 `gsi 0x0000000f` 是在
   `tch_irq_clear()` 写过 0 之后读到的（而且 setup 里还单独写过一次 0）。如果 touch152 的
   写-回读仍是 `0xf`，那 SE 的所有权在 GSI 块，**SE DMA 这条路就到此为止，只剩 GPI DMA**。
5. **Linux 在此硬件上的选择**：`spi_geni_init()` 读 `GENI_IF_DISABLE_RO & FIFO_IF_DISABLE`，
   为 1 就直接 `spi_geni_grab_gpi_chan()` → `GENI_GPI_DMA`（`geni_se_select_mode(GPI)`）；
   只有拿不到 GPI 通道时才 fallback 到 FIFO。也就是说 FIFO 不是"给 DMA 装不上用的备胎"，
   真机上大家走的是 **GPI**。
6. 失败时的轮询判据：本移植没有 QUP 中断处理程序，所以 touch152 把 `SE_IRQ_EN` 清 0，
   完全靠 `SE_DMA_TX/RX_IRQ_STAT`、`SE_DMA_TX/RX_LEN_IN` 和 `GENI_STATUS` 判断，
   避免 DMA 真通了以后变成中断风暴。

### touch152 改了什么（`xnu/patches/0222-liuqin-touch-dma-gsi-mode.patch`，11 hunk）

1. **完全不碰 0x2000/0x2004/0x2008**（`tch_se_dma_if()` 只剩 `DMA_IF_EN_RO@0xe20` +
   `SE_GSI_EVENT_EN` 的写-回读），并打印 `gsi 0x… -> 0x… (write took|IGNORED)`。
2. `tch_dma_mode()` 按上游实现：`tch_irq_clear()` → `SE_IRQ_EN = 0` → 清四个 FIFO 中断位
   → `SE_GENI_DMA_MODE_EN |= 1`。
3. 默认顺序改成上游的 **cmd-first**；`liuqintouchdmaorder=0` 或探测链里的 `desc-first`
   仍可切回。
4. 完成判据加 `LEN_IN`；1 ms 时抓一次 `DEBUG_REG0/LEN_IN` 快照，失败且全 0 时打印
   "the sequencer is not taking SE DMA"。
5. DMA 失败后、退 bit-bang 之前**再试一次 FIFO 引擎**（`tch_probe_bus("fifo fallback")`）。

产物：内核 `8d4a15c8…`、payload `d9d41857…`、
`shim/out/shim-boot-touch152-gsi-mode.img`（`f2f1ac5f…`，`shim/out/shim-boot.img` 同一张）。
启动行与 touch150/151 完全相同（567 B，结尾 `liuqintouchdma=1 liuqintouchsmmu=2`）。

### 下一次日志怎么读

| 看到 | 含义 | 下一步 |
|---|---|---|
| `gsi 0x0000000f -> 0x00000000 (write took)` 且 `DMA #1 … len_in tx 1 rx 1` | 就是 GSI 模式没退 | 直接把固件下载改走 DMA（256 B 一次），量一下下载时间 |
| `write IGNORED` + `DMA #N never started` | 0xe18 被硬件锁死，SE 归 GSI 块管 | **SE DMA 到此为止**；要么写 GPI DMA（GSI 通道 + TRE ring），要么看下面这行 |
| 紧跟着出现 `SE4 FIFO engine works` | FIFO 接口其实能用 | 先用硬件 FIFO 把固件下载跑起来（比 bit-bang 快得多），GPI 以后再说 |
| `FIFO engine did not answer either` | 两条引擎路都不通 | 回到 bit-bang，下一步只剩 GPI DMA（工作量大，先读 GSI/GPI 文档再动手） |
| `len_in tx N`（0 < N < len） | 引擎动了但没走完 | 看 `SMMU cb fsr/far` 与 `SE_DMA_DEBUG_REG0` 的状态机字段 |
| `SMMU cb fsr` 非 0 | 流还是被拒 | CBAR/CB 序号或映射不对（但 touch150 已证明这条现在大概率不是问题） |

---

## 2026-10-27 21:1x — touch152 实测：**SE DMA 通了**；touch153 只修完成判据

### 先纠正上一节两处读错（照片分辨率害的）

- **QUP 是 v3.8**：`QUP hw 0x30080000 (v3.8)`，不是 v3.0。`SE+0x2004` 仍然一读就 external abort，
  但"v3.0 没有 0x2000 段"的解释作废（v3.8 应该有）；原因未知，可能是这颗 QUP 对每个 SE
  实际只解码 0x1000。**操作结论不变：别碰 0x2000/0x2004；`DMA_IF_EN_RO@0xe20` 读 1。**
- **`SE_GSI_EVENT_EN` 是可写的**：本次 `after init … gsi 0x00000000 seirq 0x0000000f`，
  `gsi 0x0 -> 0x0 (write took)`。"被 GSI 锁死、SE DMA 没戏"的判断作废。

### 决定性的一行

```
DMA #1 tx+rx len 1 … dma_mode 0x1 gsi 0x0 seirq 0x0
DMA #1 came back: status 0x00000000 irq 0x08000081 tx 0x00000003 rx 0x00000003 len_in tx 1 rx 1 after 2 polls
TX probe as configured: ok
SE4 running, chip select by hand
```

`tx/rx 0x3` = DMA_DONE|EOT，命令 status 0x0，`len_in` 等于长度：
**上游的 `geni_se_irq_clear()`（GSI=0）+ `GENI_SELECT_MODE(DMA)` 就是 touch129..151 缺的那一步。**
SMMU 的 bypass CB 也没问题（`SMR 0x800005a3 / S2CR 0x4a`，DMA 期间读还是这个值）。

### 但数据是垃圾，原因是我自己的完成判据

- `SE_DMA_TX_LEN_IN` 是 **DMA 搬进 SE 的字节数**，不是 SPI 移出去的字节数。
  256 B 写命令在 35 次轮询（1.75 ms）就到 `len_in 256`，而 600 kHz 下需要 4.3 ms，
  同一行 `status 0x41` 说明命令还 active。
- 于是下一条命令在引擎忙时写 `M_CMD0` → 6 次
  `DMA failed: status 0x0 irq 0x2`（`M_CMD_OVERRUN`），
  `fw dl part 0 @0x00000 write failed after 0 chunks`；面板只收到半条事务，
  所以 `trim id f6 a8 62 d6 09 40 (unknown)`、`report space 63644x9018`。
- 面板本身没问题：同一个镜像的 CS 命令、DMA_DONE、TX probe 都正常。

### touch153（`xnu/patches/0223-liuqin-touch-dma-cmd-done.patch`，4 hunk）

1. 完成判据 = `M_IRQ_STATUS & M_CMD_DONE_EN`（bit0）；`LEN_IN` 只作诊断。
2. `ok` = `M_CMD_DONE` 且无错误位 且（已武装方向的）`TX/RX_DMA_DONE`。
3. 计数器每条都加，只有前 8 条打印（touch152 的日志一半是 `DMA #8 issuing …` 刷出来的）。
4. 失败诊断：`no M_CMD_DONE: …` / `command done, DMA status short: …`。

产物：内核 `581853df…`、payload `c1acf8e7…`、
`shim/out/shim-boot-touch153-cmd-done.img`（`110f7259…`，`shim/out/shim-boot.img` 同一张）。
另外 `shim/out/fwarea-touch144.bin` 丢了，用 `tools/mkfwarea.py ~/liuqin-root/fw` 重建，
sha256 `bdcb253b…` 与记录一致。

### 下一次日志怎么读

| 看到 | 含义 | 下一步 |
|---|---|---|
| `Novatek trim id 20 00 00 32 65 03 (NT36532)` | 事务顺序修好了 | 看 `fw dl part …` 是否逐段写下去 |
| `DMA #N … after ~86 polls`（256 B） | 600 kHz 下的正常耗时（4.3 ms） | 下一步可以把 `div` 调到 2（9.6 MHz），下载缩到 ~0.3 s |
| 仍然 `unknown id` | 命令顺序还有问题 | 看 `command done, DMA status short` 与 `M_CMD_OVERRUN` 出现的频率 |
| `no M_CMD_DONE: status 0x41 irq 0x2` | 上一条没等完就发了下一条 | 不可能再发生（现在等 bit0）；出现就是 `M_IRQ_CLEAR` 把状态提前清了 |
| `fw dl part 0 … write failed` | 写入某一段失败 | 看该段第一条失败传输的 `status/irq/tx/rx` |



---

## 2026-10-27 21:3x — 事务层已通，数据层不对：`dummy` 少了 1 字节；touch154 加自检 + 自校准

### touch153 的结果：传输层彻底正常

```
DMA #2 tx len 3  … came back: status 0x0 irq 0x1 tx 0x3 rx 0x0 len_in 3  after 2 polls
DMA #3 tx+rx len 7 … came back: status 0x0 irq 0x08000081 tx 0x3 rx 0x3 len_in 7/7 after 4 polls
```

第一次**没有任何** `DMA failed` / `M_CMD_OVERRUN`：`M_CMD_DONE` 就是正确的完成信号。

### 但面板数据不稳定，且发现一个确定的 bug

- 两次启动读到的 trim id 不同（`f6 88 62 d6 21 40` vs `f6 a8 62 d6 09 40`），
  形状像"面板没在驱动 MISO、RX 收到的是 MOSI/CLK 串扰"，和早期 bit-bang 的
  "marginal read" 同一类。
- 读日志时发现：**SE 路径的 `tch.dummy` 一直是 0**（结构体默认值），而 pad 路径明确设 1。
  日志对上了：`DMA #3 … len 7` = 6 + 1 + 0；pad 路径是 6 + 1 + 1 = 8。
  **SE 路径每次读都少一个字节并且错位**（驱动自己的注释：dummy=0 时 trim id 变成
  "3f 20 00 00 32 65" 而不是 "20 00 00 32 65 03"）。这是 touch154 的第一项修复。

### touch154（`0224-liuqin-touch-dma-diagnose.patch`）

1. `tch.dummy = 1; tch.page8 = false;` 进 SE 分支。
2. `tch_dma_diag()`，在 `tch_se_start()` 前跑：
   - 打印 `SE_SPI_LOOPBACK / DEMUX_OUTPUT_INV / TRANS_CFG`；
   - **pad 电平**：mux 到 qup4 后 `TLMM GPIO_IN_OUT` bit0 仍是引脚电平，在 CS 命令前后读
     gpio16..19 —— 回答"引擎的 CS 有没有到 gpio19"；
   - **loopback**：`SE_SPI_LOOPBACK=1`，发 `55 aa 0f f0` 收回来比较 —— 把"SE 内部
     DMA/packing"和"面板/引脚"分开；
   - **自校准**：ROM trim id 在六种 `(顺序, div)` 组合下各读一次，第一个返回 `.. 65 03`
     的被保留（顺序 desc/cmd × div 32/128/8，即 600/150/2400 kHz）。

产物：内核 `b8f91bba…`、payload `23a1777c…`、
`shim/out/shim-boot-touch154-diag.img`（`1636e600…`，`shim/out/shim-boot.img` 同一张）。

### 下一次日志怎么读

| 看到 | 含义 | 下一步 |
|---|---|---|
| `pads … assert 0x?` 的 bit3 由 1 变 0 | CS 确实到了 gpio19 | 看 loopback |
| bit3 不变（或反过来） | SE 的 CS 没接到这颗 pad / 极性反了 | 改成 GPIO 手动 CS（gpio19 拉回 mux 0 自己驱动），或查 SE4 的 CS 映射 |
| `SE loopback … byte exact` | SE 数据通路没问题 | 问题在面板/引脚侧；自校准若全不中，查 CS 时序与时钟 |
| `SE loopback … MISMATCH` | DMA/packing/顺序仍不对 | 用 loopback 做回归，先把这条修成 byte exact |
| 某个 `trim id … MATCH` | 自校准成功 | 后面整场都用该组合，固件下载应当直接开始 |
| 六组全 `no match` | 面板没被正确寻址 | 结合 pad 与 loopback 的结论定位 |

---

## 2026-10-27 21:5x — touch154 实测：CS 到引脚、loopback 全零、面板确定性地答错；touch155 上对照实验

### touch154 的三个事实

1. `pads 16..19 idle 0x8 assert 0x0 release 0x8` → **SE 的 CS 正确到达 gpio19 并翻转**。
   引脚映射、mux 都没问题，不用改 GPIO 手动 CS。
2. `SE loopback tx 55 aa 0f f0 rx 00 00 00 00 (MISMATCH)` → loopback 收全零。
   注意这颗 SE `GENI_IF_DISABLE_RO=1`（FIFO 接口禁用），loopback 可能走 FIFO 接口，结果
   不可全信；但它指向头号嫌疑：**TX 数据有没有真的进移位寄存器**。
3. dummy=1 修复后，面板回答变得**确定**：首次 `ff ff ff ff ff ff`，之后五次全是
   `00 00 02 01 aa aa`。面板在回答、但答错 —— 命令内容不对或面板状态不对
   （整场 probe 又读出 `66 96 09 40 dd bf`，状态在漂移）。

另外：touch154 扫描里的 `desc-first` 标签是假的（`tch_dma_order` 只在命中时赋值，
六次全是 cmd-first），touch155 已修。

### touch155（`0225-liuqin-touch-dma-diag2.patch`，6 hunk）

1. loopback 写 1 后回读打印；
2. **CS 全程跟踪**：一次完整读的五个 pad 采样 + rx 6 字节（FRAGMENTATION 有没有把 CS 保持住）；
3. 前 3 条传输打印 TX 暂存页前 8 字节（CPU 视角）；
4. 扫描前先赋 `tch_dma_order`（修复 touch154 的标签 bug）；
5. 六种组合不中 → **bit-bang 读一次 trim id**（面板还活着吗），再 mux 回 qup4；
6. **gpio20 复位脉冲**，再用 SE 读两次。

产物：内核 `aeffad95…`、payload `7b78161f…`、
`shim/out/shim-boot-touch155-diag2.img`（`e15ca8c4…`，`shim/out/shim-boot.img` 同一张）。

### 下一次日志怎么读

| 看到 | 含义 | 下一步 |
|---|---|---|
| `CS trace … after-page 0x?` bit3 = 0 | FRAGMENTATION 保住了 CS | 看 pad-path 对照 |
| bit3 = 1 | CS 在 page 写与读之间掉了 | 把整次读改成单条 DMA 命令（写+读一次完成），或者用 GPIO 保持 CS |
| `pad-path trim id … (panel alive)` | 面板没坏，问题在 SE 数据路径 | 重点查 TX：descriptor/打包/时序 |
| `pad-path … (no answer)` | 我们的 SE 传输把面板弄挂了 | 看 `reset retry`，复位后是否恢复 |
| `DMA #N TX page 55 aa 0f f0 …` | CPU 侧 TX 数据正确 | 若面板仍答错，说明 SE 没把这些字节移出去 |
| `reset retry … MATCH` | 缺的就是复位 | 把复位收进 SE 路径的正式流程 |

---

## 2026-10-27 22:0x — touch155 判定：面板活着、CS 正确、desc-first TX-only 必挂；touch156 测移位机制

### touch155 的事实

1. `pad-path trim id 20 00 00 32 65 03 (panel alive)` —— 我们一堆 SE 传输之后立刻切回
   bit-bang，id 正确。**面板没坏，问题 100% 在 SE 数据路径。**
2. `CS trace idle 0x1 assert 0x1 after-page 0x1 after-read 0x1 release 0x9` ——
   FRAGMENTATION 把 CS 保持到了读结束，释放正常。分帧没问题。
3. **desc-first 的 TX-only 必挂**（`status 0x41` 1000 polls、`tx 0`、`len_in 3`）：
   描述符先于命令武装 → DMA 的 EOT 在命令激活前发完 → 序列器永远等不到完成。
   → **只能用 cmd-first**（上游今天的顺序）。
4. `reset retry … 00 00 02 01 aa aa` —— 复位无用，面板状态不是原因。
5. cmd-first 的读稳定返回 `00 00 02 01 aa aa`；不同地址读回不同内容 → 命令**部分**到达。

**头号假设**：cmd-first 发出命令的瞬间 TX FIFO 还是空的，第一个字节被移成未知值/丢失，
面板按移位后的命令解析 → 确定性错答。

### touch156（`0226-liuqin-touch-dma-txprobe.patch`，3 hunk）

1. 8 字节 DMA loopback（tx 01..08，打印全部 8 个 RX）——移位/丢字节模式直接现形；
2. FIFO loopback（tx 11 22 33 44）——移位寄存器本身好不好，把 DMA 投递和移位分开；
3. raw read8：8 字节 RX 原样打印（不跳 offset）；
4. raw double-addr：地址字节连发两次（9 字节）——面板若在第二个字节上应答，
   即证明首字节被吃，可以用"TX 前插占位字节"补偿。

产物：内核 `3a4f33ff…`、payload `bcc96930…`、
`shim/out/shim-boot-touch156-txprobe.img`（`3288c293…`，`shim/out/shim-boot.img` 同一张）。

### 下一次日志怎么读

| 看到 | 含义 | 下一步 |
|---|---|---|
| `SE loopback8 … rx 00 01 02 03 04 05 06 07`（右移一位） | 首字节被空 FIFO 替换 | cmd-first + TX 前置占位字节补偿 |
| `FIFO loopback ok rx 11 22 33 44` 且 DMA 不对 | 移位通路好，DMA 投递时序坏 | 试"先武装 TX → 命令 → 再武装 RX"（首字节在、EOT 在命令激活后发，绕开 desc-first 挂死） |
| `FIFO loopback` 也不对 | 移位/打包本身坏，与 DMA 无关 | 查 packing/`SE_GENI_BYTE_GRAN` 配置 |
| `raw double-addr` 第二个 0x04 之后出现 `20 00 00 32 65 03` | 首字节被替换 | 直接上补偿法 |
| `raw read8` 全部 0xff 或 0x00 | RX 捕获没工作 | 看 RX descriptor 与 `SE_DMA_RX_LEN` |

---

## 2026-10-27 22:1x — touch156：loopback 死路、面板 raw 流确定；touch157 改问 FIFO 路径和 page 写

### touch156 的事实

1. `loopback reg written 1 reads 0x1` —— 寄存器可写；但 **DMA 和 FIFO 两条 loopback 都回全零**。
   `GENI_IF_DISABLE_RO=1`（FIFO 接口禁用）能解释：loopback 走 FIFO 接口。**loopback 不能量 TX，划掉。**
2. 面板 raw 流：`raw read8 rx 10 00 00 00 02 01 aa aa`、`raw double-addr … aa aa 55 55`；
   double-address 没救回 id → 首字节不是简单被顶掉。
3. `pad-path trim id 20 00 00 32 65 03 (panel alive)` 再次确认面板好、SE 错。
4. 日志噪声：touch156 的 CS trace 里 DMA #2 是 `M_CMD_OVERRUN`——因为 FIFO loopback 在
   trace 之前跑、把引擎留在了坏状态（diag 测试顺序本身的副作用）。

### touch157（`0227-liuqin-touch-dma-fifotest.patch`，2 hunk）

1. 删掉 loopback8 与 FIFO-loopback 两条传输（又死又污染状态），只留 loopback 寄存器回读；
2. 全不中后 **切 FIFO 路径读面板 trim id**：FIFO 和 DMA 共用移位寄存器——
   FIFO 读对 → 移位/时钟/CS 都好，问题在 DMA→FIFO 投递；FIFO 也错 → 移位通路本身坏；
3. **page-0 读**：故意写 page=0 再读 trim 地址，若与正常读相同 → TX-only 的 page 写没生效。

产物：内核 `22136133…`、payload `3135e5e4…`、
`shim/out/shim-boot-touch157-fifotest.img`（`96a0e219…`，`shim/out/shim-boot.img` 同一张）。

### 下一次日志怎么读

| 看到 | 含义 | 下一步 |
|---|---|---|
| `FIFO-path trim id … MATCH` | 移位通路好，问题在 DMA 投递 | 先武装 TX→命令→再武装 RX，或 TX 前插占位字节 |
| `FIFO-path … no match/read failed` | 移位/时钟/CS 本身坏，与 DMA 无关 | 查 packing/`SE_GENI_BYTE_GRAN`/SPI 相位 |
| `page-0 read` == `raw read8` | TX-only 的 page 写没生效 | 读数全在默认页，重点修 TX-only 传输 |
| `page-0 read` != `raw read8` | page 写生效了 | 问题在地址字节或读的那一步 |

---

## 2026-10-27 22:2x — touch157 定案：page 写完全无效；touch158 直接看 MOSI 波形

### touch157 的事实

1. **page 写完全无效**：写 page=0 与写真实 page 0x3f62，读回逐字节相同
   （`page-0 read rx` == `raw read8 rx` == `10 00 00 00 02 01 aa aa`）。
   面板没收到那 3 个字节，所有读都落在默认页——这解释了一切"稳定但错误"的读数。
2. **FIFO 路径读不了**（`read failed`）：`GENI_IF_DISABLE_RO=1` 是真的，FIFO 接口不可用。
3. **desc-first 挂死时 `debug 0x000000b1`**：`DMA_TX_ACTIVE=1`、`DMA_TX_STATE=0xb` ——
   TX DMA 状态机停在 state 11。cmd-first 干净完成（tx 0x3）。
4. CS 行为干净（assert/page/read 全 0，release 回 1），面板活着。

**结论**：SE 的 TX 字节没有以正确内容出现在 MOSI 上。CS/时钟/完成/RX 捕获全都好。

### touch158（`0228-liuqin-touch-dma-wirecapture.patch`，2 hunk）

`tch_wire_capture()`：SPI 降到最慢（div 4095 = 4.7 kHz，213 µs/bit），发 8 字节已知图样
`a5 5a 3c c3 0f f0 11 88`，在用 CPU 紧循环采样 gpio16/17/18（MISO/MOSI/CLK），
按 CLK 上升沿重建字节，打印 `wire tx …` / `wire rx …` 和位数。
放在 !found 分支、page-0 读之后、切 bit-bang 之前。

产物：内核 `35581035…`、payload `b1ec60ef…`、
`shim/out/shim-boot-touch158-wirecapture.img`（`495ce582…`，`shim/out/shim-boot.img` 同一张）。

### 下一次日志怎么读（wire capture 决定性判据）

| 看到 | 含义 | 下一步 |
|---|---|---|
| `wire tx a5 5a 3c c3 0f f0 11 88` | TX 线完全正确 | 问题在面板的时序容忍：CS 建立/保持、命令间空隙 → 加延时 |
| `wire tx 00 00 …` / `ff ff …` | TX 数据根本没出去 | 查 DMA→FIFO→移位投递（state 11 那条线） |
| `wire tx 52 ad 1e 61 …`（整体右移一位） | 首比特相位/丢一位 | 换 CPHA，或 TX 前插占位 |
| 其它确定值 | 记录后按图案定位（packing/字节序/位序） | 对照 `SE_GENI_TX_PACKING_CFG*` |
| `0 bits` | 移位根本没跑 | 回到时钟/命令层 |

---

## 2026-10-27 22:3x — touch158：duplex TX 线上逐字节正确；嫌疑只剩 TX-only（page 写）

### touch158 的事实

```
wire capture at 4688 Hz, tx a5 5a 3c c3 0f f0 11 88
wire tx a5 5a 3c c3 0f f0 11 88 (64 bits)     ← 逐字节命中
wire rx 00 00 00 00 00 00 00 00               ← 非法命令，面板不应答（预期）
page-0 read rx 10 00 00 00 02 01 aa aa        ← page 写仍然无效
```

1. **duplex 的 TX 完全正确**：DMA 投递/打包/移位/时钟/CS 全部好，"字节被移位或丢掉"
   的所有猜测排除。
2. `wire rx` 全零是预期（`a5 5a …` 不是合法命令），也说明 RX 采样点和引脚正常。
3. 仍然成立：**page 写无效**（page-0 读 == 正常读）。

### 剩下的唯一嫌疑：TX-only（opcode 1）

page 写是 TX-only；duplex 已证明没问题，但 TX-only 从未被捕获过。两种可能：
(a) TX-only 根本没移位；(b) 移位了但 page 值不对。

### touch159（`0229-liuqin-touch-dma-txonly-capture.patch`，3 hunk）

`tch_wire_capture(tx_only, 名字, 图样, 长度)`，一次三种捕获：
1. `duplex`：`a5 5a 3c c3 0f f0 11 88`（对照）；
2. `txonly`：**真实 page 写 `ff 3f 62`**（关键）；
3. `readcmd`：**真实读命令 `04 00×7`**（看面板慢速下的真实应答）。

产物：内核 `e9f8fbf2…`、payload `81177649…`、
`shim/out/shim-boot-touch159-txonly.img`（`9e112669…`，`shim/out/shim-boot.img` 同一张）。

### 下一次日志怎么读

| 看到 | 含义 | 下一步 |
|---|---|---|
| `wire txonly tx ff 3f 62 (24 bits)` | page 写在线上正确 | 问题在时序容忍或 page 语义 → 加 CS/命令间延时 |
| `wire txonly tx 00 00 00` / 位数不对 | **TX-only 没移位** | 最终病根：把 page 写改成 duplex（opcode 7，RX 丢弃） |
| `wire readcmd rx` == 驱动的 `10 00 00 00 02 01 aa aa` | RX 通路与面板应答都对 | 只修 page 写 |
| `wire readcmd rx` ≠ 驱动读到的 | 快速时钟下 RX 采样也有问题 | 两处一起修 |

---

## 2026-10-27 22:4x — touch159 铁证：数据通路全对，病根是 page 写→读的间隔；touch160 自校准延时

### touch159 的线探针（决定性）

```
wire duplex  tx a5 5a 3c c3 0f f0 11 88 (64 bits)
wire txonly  tx ff 3f 62 (24 bits)              ← TX-only 也正确
wire readcmd tx 04 00 00 00 00 00 00 00 (64 bits)
wire readcmd rx 00 3f 20 00 00 32 65 03         ← 面板回出正确 trim id
```

`rx` 的第 2..7 字节就是 `20 00 00 32 65 03`（前两字节是地址/哑字节）。
**整条 SE 通路在 4.7 kHz 下逐字节正确，面板应答完全正常。**

### 600 kHz 下为什么错

两个差异：时钟（4.7 kHz vs 600 kHz）和 **page 写→读之间的间隔**（线探针中间夹了
`printf`，毫秒级；驱动的 `tch_nvt_read_locked` 中间只有几条寄存器写，微秒级）。
div 128（150 kHz）也一样错 → 更可能是**间隔**，因为 150 kHz 的位宽绰绰有余。

### touch160（`0230-liuqin-touch-dma-page-delay.patch`，6 hunk）

1. 新增 `tch_page_dly` / `tch_cs_dly`（µs），默认 0，行为不变；
2. 三处 page 写之后插 `tch_page_dly`（读、SRAM 块写、单字节写）；`tch_cs(true)` 后插 `tch_cs_dly`；
3. diag 里在线探针之后跑时序矩阵 `(page,cs) = (0,0)/(1000,0)/(1000,50)/(5000,50)`，
   第一组返回正确 id 的被保留，后续整场使用；全不中恢复 (0,0)。

产物：内核 `1615f085…`、payload `54f3005b…`、
`shim/out/shim-boot-touch160-timing.img`（`1344daae…`，`shim/out/shim-boot.img` 同一张）。

### 下一次日志怎么读

| 看到 | 含义 | 下一步 |
|---|---|---|
| 某组 `MATCH` + `keeping page delay …` | 根因就是间隔，且已自动生效 | 看 `Novatek trim id 20 00 00 32 65 03`、`fw dl part …` |
| 四组全 `no match` | 间隔不是全部原因 | 把 div 128/32 也加进矩阵，或怀疑 600 kHz 下 RX 采样点 |
| `wire readcmd rx` 变了 | 面板状态漂移 | 先复位再测 |

---

## 2026-10-27 22:5x — touch160：延时矩阵全灭；touch161 测速度阈值与首字节投递

### touch160 的事实

```
page delay 0/1000/1000/5000 us × cs delay 0/0/50/50 → 全部 00 00 02 01 aa aa
wire readcmd rx 00 3f 20 00 00 32 65 03     ← 4.7 kHz 下仍然正确
```

**间隔不是原因**（连 5 ms 都不行），CS 静默也不是。4.7 kHz 下同样的引擎和命令能拿到正确 id。

### 剩下两个变量

1. **时钟本身**：需要知道 SE 自己的读在慢速下是否也正确、阈值在哪；
2. **高速下首字节投递**：cmd-first 先命令后武装描述符，600 kHz 下每字节 13.3 µs、
   武装约 10 µs——首字节可能在 DMA 就绪前就开始移位；4.7 kHz 下每字节 1.7 ms，看不出来。

### touch161（`0231-liuqin-touch-dma-speed-sweep.patch`，1 hunk）

(a) **速度扫描**：div 4095/512/128/32 各读一次 trim id，第一个 MATCH 的 div 保留；
(b) **desc-first duplex**：600 kHz 下先武装描述符再发命令，并把 page 写改成 duplex
（opcode 7，RX 丢弃）——首字节提前进 FIFO，同时绕开 TX-only 的 desc-first 挂死。

产物：内核 `13e94465…`、payload `bec403fe…`、
`shim/out/shim-boot-touch161-speed.img`（`1fd932c6…`，`shim/out/shim-boot.img` 同一张）。

### 下一次日志怎么读

| 看到 | 含义 | 下一步 |
|---|---|---|
| `se read div 4095 (4688 Hz): … MATCH` | SE 读在慢速下正确 → 纯速率问题 | 看阈值；决定可接受速度 |
| `desc-first duplex: … MATCH at rx[6..7]` | **首字节投递是根因** | page 写改 duplex + 全程 desc-first |
| 两者都不中 | 慢速也错 → RX 采样点/相位问题 | 4.7 kHz 下打印 SE 原始 8 字节与线探针对照 |

---

## 2026-10-27 23:0x — touch161 定案：**RX 捕获坏，TX/面板/时钟都好**

### touch161 的事实

```
se read div 4095 (4688 Hz): 00 00 02 01 aa aa (no match)   ← 慢速也错
se read div 512  (37500 Hz): 00 00 02 01 aa aa (no match)
se read div 128  (150000 Hz): 00 00 02 01 aa aa (no match)
se read div 32   (600000 Hz): 00 00 02 01 aa aa (no match)
desc-first duplex:           10 00 00 00 02 01 aa aa (no match)
wire readcmd rx 00 3f 20 00 00 32 65 03                    ← CPU 采样正确
```

1. **SE 自己的读在任何时钟下都错**，而同一时钟、同一条线上 CPU 采样是对的 →
   **TX/MOSI、面板、时钟都正常，坏的是 SE 的 RX 捕获**（移位 → RX FIFO → DMA → 内存）。
2. 顺序无关（desc-first duplex 结果相同）。
3. 之前"page 写无效"的判据作废：RX 本身错，page-0 读与正常读当然一样。
4. 之前"4.7 kHz 能行"也只对**线上**而言，SE 收到的仍是错的。

### touch162（`0232-liuqin-touch-dma-rxcompare.patch`，3 hunk）

同一次传输给出两个视角：
```
wire <name> rx …            ← CPU 逐边沿采样
dma  <name> rx … ee-prefilled 24 bytes (len_in / RX_IRQ_STAT)  ← SE 的 DMA 写进 RX 页的内容
```
RX 页预填 0xee，写偏/没写都会现形。

产物：内核 `65c1cabd…`、payload `0d47e20d…`、
`shim/out/shim-boot-touch162-rxcompare.img`（`1e7342a6…`，`shim/out/shim-boot.img` 同一张）。

### 下一次日志怎么读（只看 `readcmd` 那组）

| 看到 | 含义 | 下一步 |
|---|---|---|
| `dma readcmd rx` 前 8 字节仍是 `00 00 00 00 02 01 aa aa`、之后 `ee…` | SE 写了 8 字节但数据源不对 | 查 `SE_GENI_RX_PACKING_CFG0/1`、`BYTE_GRAN`、`SE_DMA_RX_ATTR` |
| 全是 `ee` | RX DMA 没写（尽管报 DONE） | 目标地址/描述符问题 |
| 从第 4/8 字节才开始变 | 写偏移了 | 按偏移修正 |
| 与 `wire rx` 一致 | RX 其实对 | 矛盾在别处（极不可能） |

---

## 2026-10-27 23:1x — touch162：RX 写对位置、写错内容；touch163 查 RX 配置与触发

### touch162 的两视角对照（同一笔传输）

```
wire readcmd rx 00 3f 20 00 00 32 65 03
dma  readcmd rx 10 00 00 00 02 01 aa aa ee ee …   (len_in 8)
wire duplex rx 00 00 00 00 00 00 00 00
dma  duplex rx 00 00 00 00 00 00 00 00 ee …
```

1. **偏移正确**：RX 页预填 `ee`，前 8 字节被覆盖、后面仍是 `ee` → 写在 `TCH_DMA_RX_OFF+0`，
   长度也对。不是写偏。
2. **内容错**：`10 00 00 00` 恰好是 TX 首字节 `04` 左移 2 位，像是 RX 采到的是 MOSI
   或移位相位偏了；末尾 `aa aa` 像电平串扰。
3. duplex 组线上全零、SE 也收全零 —— 与"空 FIFO 返回 0"一致，不能证明 RX 工作。
4. `irq 0` 是 `tch_cs(false)` 里 `tch_irq_clear()` 清掉的假象。

### touch163（`0233-liuqin-touch-dma-rxvariants.patch`，4 hunk）

1. 打印 RX 配置：`SE_DMA_RX_LEN/ATTR`、`BYTE_GRAN`、`RX_PACKING_CFG0/1`、`RX_FIFO_STATUS`；
2. **variant 1**：`RX_WATERMARK/RFR` 改 1/1（原 13/14）——验证水线是否挡住 RX DMA 触发；
3. **variant 2**：`SE_DMA_RX_ATTR = EOT_BUF`。

产物：内核 `1a484878…`、payload `f5c4d0b7…`、
`shim/out/shim-boot-touch163-rxvariants.img`（`473ec262…`，`shim/out/shim-boot.img` 同一张）。

### 下一次日志怎么读

| 看到 | 含义 | 下一步 |
|---|---|---|
| `dma readcmd-wm1 rx` == `wire … rx` | **水线是原因** | 把 1/1 写进 DMA 路径 |
| `dma readcmd-attr1 rx` == `wire … rx` | RX 需要 EOT 属性 | 改 `SE_DMA_RX_ATTR` |
| 两个都还是 `10 00 00 00 02 01 aa aa` | RX 数据源没接通 | 看 `rx cfg` 的 packing/gran，并对照 FIFO 状态 |
| `fifo_stat` 有字但 DMA 拿到的不是它 | DMA 读 FIFO 的端口/偏移问题 | 改用 FIFO 直接读做对照 |

---

## 2026-10-27 23:2x — touch163：水线/EOT 都无效，packing 正确；touch164 直接问 RX FIFO

### touch163 的结果

```
rx cfg len 8 attr 0x0 gran 0 pack0 0x0007f8fe pack1 0x000ffefe
wire readcmd rx 00 3f 20 00 00 32 65 03
dma  readcmd rx 10 00 00 00 02 01 aa aa …
dma  readcmd-wm1   rx 10 00 00 00 02 01 aa aa …   ← 水线 1/1 无效
dma  readcmd-attr1 rx 10 00 00 00 02 01 aa aa …   ← RX EOT 属性无效
```

1. **水线不是原因**（1/1 == 13/14）；**RX EOT 也不是**。`SE_DMA_RX_ATTR` 写 1 读回仍 0，
   这颗 SE 上 RX 的 ATTR 位写不进去。
2. **packing 与 Linux 完全一致**：`pack0 0x0007f8fe / pack1 0x000ffefe` 就是
   `geni_se_config_packing(8,4,msb=1)` 的笔算值，`BYTE_GRAN=0` 也对 → 打包排除。
3. `fifo_stat 0` 是 DMA 抽干后的读数，不能证明 FIFO 没进过数据。

### touch164（`0234-liuqin-touch-dma-rxfifotest.patch`，2 hunk）

`tch_rx_fifo_test()`：真实读命令按 4.7 kHz 打出，**只武装 TX DMA、不武装 RX DMA**，
让 FIFO 不被抽干；然后读 `RX_FIFO_STATUS` 字数并**直接读 FIFO 的字**。

- FIFO 有正确的字 → RX 数据路径好，坏的只是 FIFO→DMA→内存，**可绕开 RX DMA 用 CPU 读 FIFO**；
- FIFO 空 → RX 数据没进 FIFO，这颗 SE 的接收只能在 GSI/GPI 下工作。

产物：内核 `5513f95e…`、payload `aeb2c2eb…`、
`shim/out/shim-boot-touch164-rxfifo.img`（`1a62b399…`，`shim/out/shim-boot.img` 同一张）。

### 下一次日志怎么读

| 看到 | 含义 | 下一步 |
|---|---|---|
| `wc N`（N>0）且字里能拼出 `20 00 00 32 65 03` | RX 路径好，DMA 读坏 | **改用 CPU 读 FIFO**，最快能出可用驱动 |
| `wc 0` | RX 从未进 FIFO | 写 GPI/GSI DMA，或 SE 只管 TX/CLK/CS、MISO 用 CPU 慢速采（仅调试） |
| `wc N` 但字错 | FIFO 收到别的 | 按字模板对照 packing 定位 |

---

## 2026-10-27 23:3x — touch164：RX FIFO 能收、CPU 能读；touch165 改用 TX-DMA + CPU RX FIFO

### touch164 的事实

```
rx fifo after clocking (no rx dma): stat 0x40000002 wc 2 (64 bits on the wire)
rx fifo word 0 = 0x00000000
rx fifo word 1 = 0x00000000
```

1. **RX FIFO 确实被写入**（`wc 2` = 8 字节 = 64 bit，与线上位数一致），
   **CPU 直接读 `SE_GENI_RX_FIFOn` 也是通的**。
2. 两个字是 0：这次前面没有 page 写，面板对默认页 addr 0x04 的读很可能就是 0——
   **关键是 `wc 2`，不是值**。
3. 定位：**移位→打包→RX FIFO 都好，坏的是 FIFO→DMA→内存**（TX DMA 读内存/写 FIFO 是好的）。

### touch165（`0235-liuqin-touch-dma-rx-via-fifo.patch`，3 hunk）

1. `tch_dma_fifo_xfer()`：**TX 走 SE 的 DMA**（线探针证明逐字节正确），**RX 不武装 DMA**，
   命令运行期间用 `tch_drain_rx()` 从 RX FIFO 直接搬；
2. 新全局 `tch_rx_via_fifo`，`tch_dma_xfer()` 在有 rx 且标志为 1 时转走新函数；
3. diag 里做一次完整 trim id 读（TX-DMA + CPU RX FIFO），命中就把标志打开并保留整场。

产物：内核 `6eeedfd8…`、payload `5e023d96…`、
`shim/out/shim-boot-touch165-rxviadio.img`（`e05062b4…`，`shim/out/shim-boot.img` 同一张）。

### 下一次日志怎么读

| 看到 | 含义 | 下一步 |
|---|---|---|
| `tx-dma + cpu rx fifo: 20 00 00 32 65 03 (MATCH - keeping it)` | **RX 修好** | 看 `Novatek trim id 20 00 00 32 65 03`、`fw dl part …` |
| 不中但 `wc` 非零 | FIFO 有数据但读取时序/打包不对 | 同一传输同时打印 `wire rx` 与 FIFO 字 |
| `wc 0` | FIFO 写入依赖命令形式 | 重测 |

---

## 2026-10-27 23:5x — touch165 的答案：CPU 读 FIFO 也是零；touch166 判死接收侧并改走 pads

### touch165 的日志说了什么（`165.txt`）

```
rx fifo after clocking (no rx dma): stat 0x40000002 wc 2 (64 bits on the wire)
rx fifo word 0 = 0x00000000
rx fifo word 1 = 0x00000000
tx-dma + cpu rx fifo: 00 00 00 00 00 00 (read failed)
wire readcmd  rx 00 3f 20 00 00 32 65 03
dma  readcmd  rx 10 00 00 00 02 01 aa aa ee ee …
se read div 4095/512/128/32: 00 00 00 00 00 00 (read failed)
pad-path trim id 20 00 00 32 65 03 (panel alive)
```

1. **FIFO 里的字是 0，DMA 里是另一套垃圾**（`10 00 00 00 02 01 aa aa`），两条路
   互不一致，说明不是"FIFO→DMA"单点坏了，而是**引擎报告的接收字节本身就是垃圾/零**。
2. 同一传输里，线探针在 gpio16 上拿到 `00 3f 20 00 00 32 65 03`，而 SE 在
   gpio17/18/19 上的 TX/CLK/CS 逐位正确。**所以剩下的可能只有两种**：
   串行输入不是 gpio16（pad/输入映射），或接收块没使能。
3. 另外两条与 Linux 的差异值得先排除：双工 opcode（我们自己用 7，Linux 用
   `SPI_TX_ONLY|SPI_RX_ONLY = 3`）和 `SE_IRQ_EN`（Linux 写 `0xffffffff`，我们写 0）。

### touch166（`0236-liuqin-touch-rx-route-test.patch`，7 hunk）

1. `tch_dma_mode()`：`SE_IRQ_EN = 0xffffffff`，与 `geni_se_irq_clear()` 一致；
   该寄存器 bit0/1 就是 `DMA_RX_IRQ_EN`/`DMA_TX_IRQ_EN`。观察到 `LEN_IN=8` 而
   `SE_DMA_RX_IRQ_STAT=0` 正好是"接收事件没使能"的样子。
2. 新增 `tch_rx_route_test()`：
   - **A**：opcode 7 与 3 各做一次真实 trim id 读；
   - **B**：`SE_SPI_LOOPBACK=1`，发 `de ad be ef` 读回环——**绕开 pad 的接收证据**；
   - 同时打印 `SE_DMA_RX_IRQ_STAT`、`LEN_IN`、FIFO 状态。
3. `tch_rx_fifo_test()` 增加 drain 前后字数对比：**降不到 0 = CPU 没有 FIFO 通道**，
   那么前两步里"CPU 能读 FIFO"的结论要改成"读的是空口"。
4. **兜底**：三条路由都没命中，就在 `tch_dma_diag()` 之后把整条栈切到 pads
   （gpio16..19 回 GPIO、`tch.bitbang=1`、`tch.dma=0`）并继续 bring-up。
   读和写都走已验证的 pads 路径：**这一轮无论如何都能走到固件下载和触摸帧**。

产物：内核 `fd288980…`、payload `0460e34b…`、
`shim/out/shim-boot-touch166-rxconform.img`（`ad03ee98…`，`shim/out/shim-boot.img` 同一张）。

### 下一次日志怎么读

| 看到 | 含义 | 下一步 |
|---|---|---|
| `rx route A1 opcode 3: … (MATCH)` | opcode 是病根 | 全程走 SE，速度恢复；继续 fw dl |
| `rx route B loopback … rx de ad be ef`，A 两条都不中 | 接收块好、**pad 接错线** | 查 TLMM 里 SE4 的串行输入映射，不要再改 SE 寄存器 |
| A 全不中、B 也回零 | 接收块只能 GSI/GPI | 本轮已自动落 pads（`no engine receive route works`） |
| `rx fifo wc after draining: 2` | CPU 无 FIFO 通道 | 之前"CPU 能读 FIFO"的判断作废，`wc` 不再算证据 |
| `whole stack on the pads` → `Novatek trim id 20 00 00 32 65 03` → `fw dl part 0 …` → `state now 0xa? running` | pads 路线在推进 | 等 `fwinfo` 与触摸帧；DMA 之后单独再攻 |

### 为什么不再在 SE 接收上继续盲试

- 已经把 Linux `qcom-geni-se.c`/`spi-geni-qcom.c` 的每一条 DMA 编程（`SE_DMA_RX_PTR/LEN/ATTR`、
  `RX_IRQ_EN_SET`、cmd-first 顺序、packing、`BYTE_GRAN`、水线）逐项对齐，剩下的
  **只有硬件路由/固件模式**这两类，靠读寄存器没法再缩小。
- 真正能"继续缩小"的只有 B（回环）这一条，所以这一轮把它放进去。
- 同时把 pads 兜底做掉：SE 只负责"快"，不是"能工作"的前提；固件下载 240 KiB
  在 5 µs/bit 下约 8 s，触摸帧一帧 65 B 约 2.8 ms，对 120 Hz 够用。

---

## 2026-10-28 00:0x — touch166：双工 opcode 3 收得到、7 收不到；touch167 定为默认并去掉走查

### 一句话结论

`SE_GENI_M_CMD0` 的双工 opcode 必须是 **3**（`SPI_TX_ONLY | SPI_RX_ONLY`，Linux
`spi-geni-qcom` 的写法）。这个 SE 的固件用 7 时会把字节逐位发出去（线探针一直是对的），
但接收侧返回垃圾——这就是 touch14x 一路以来"TX 对、RX 全零/垃圾"的唯一原因。

```
rx route A0 opcode 7: 00 00 02 01 aa aa (no match)
rx route A1 opcode 3: 20 00 00 32 65 03 (MATCH)      ← 同一次启动、同一个时钟、同一块缓冲
```

`SE_DMA_RX_PTR/LEN/ATTR`、`RX_IRQ_EN_SET`、cmd-first 顺序、packing、`BYTE_GRAN`、
水线、SMMU、pad mux 全部本来就是对的，不需要再动。

### touch166 的第二个发现：走查会毁掉总线

`trim id cmd-first div 32: … (MATCH)` 之后，`tch_se_start()` 的 CS/TX 探针
（`CS assert as configured: after-cmd 0x00000041`）一过，下一次读就是
`Novatek trim id ff ff ff ff ff ff`，随后 `fw state 0xfd`、`@0x11a00 fw 08 98 02 ce…`、
`fw chunk @0x00000 write failed` 都是余波。走查还白花约 78 s 启动时间。

### touch167（`0237-liuqin-touch-duplex-opcode3.patch`，7 hunk）

1. 默认 `SPI_DUPLEX = 3`，`liuqintouchopcode=7` 可回退对照；顺手改正当年写错的注释。
2. `tch_dma_diag()` 默认关闭（`liuqintouchdiag=1` 打开）。
3. 默认路径：`tch_dma_order=1`、`div 32`、`tch_irq_clear()`、`tch_nvt_bringup()`，
   然后直接读 trim id，最多 3 次；成功打印
   `SE4 receive is live (duplex opcode 3, div 32, 600 kHz)`。不再跑 CS/TX 探针。
4. 三次都不中才落 pads 兜底。

产物：内核 `0746e1f6…`、payload `cef9af6d…`、
`shim/out/shim-boot-touch167-opcode3.img`（`9a53ecc0…`，`shim/out/shim-boot.img` 同一张）。

### 下一次日志怎么读

| 看到 | 含义 | 下一步 |
|---|---|---|
| `engine bring-up 1: 20 00 00 32 65 03 (panel answered, opcode 3)` | **SE 接收通了** | 看 `fw dl bld: ilm des …` / `fw dl part 0…` / `state now 0xa?` / `fwinfo … ok` |
| `engine bring-up 1..3: … (no answer)` | 复位序列或 CS 命令还在破坏状态 | 把三次的 `irq`/`status` 一起打出来 |
| `no engine receive route works` | 干净状态下 3 也收不到 | 才轮到查 pad 接线；先用 pads 把触摸跑起来 |

### 之前的判断要按这次修正

- "RX FIFO 有数据但 CPU 读出来是零"——那是 opcode 7 下 FIFO 里本来就是垃圾；
  换 3 之后 FIFO/DMA/packing 全部自洽，`tch_rx_via_fifo` 不需要了（保留代码，默认不走）。
- "回环绕开 pad 能定位"——touch166 的回环是用 7 跑的，等于没测；下次用 3 跑才有意义。

## 2026-10-28 00:2x — touch167 上机：opcode 3 一次就通，卡点挪到"第一次写"，而那次失败是判据冤枉的

### 一句话结论

接收侧真的修好了：干净启动里 **第二次** trim 读就是 `20 00 00 32 65 03`，
`SE4 receive is live (duplex opcode 3, div 32, 600 kHz)` 之后连读四次都对
（`fw state`、`@0x11a00`、`@0x2fe00`、trim id）。

新的卡点不是"写不通"，而是 **传送判据太严**：`M_CMD_DONE` 到了、没有错误位、
两个 `LEN_IN` 都走满，但 `SE_DMA_TX/RX_IRQ_STAT` 那一位比命令完成晚，读成 0，
于是这笔传输被判失败——bring-up 的第一次读、以及固件下载的 chunk 0，都死在这上面。

```
DMA #8 issuing opcode 3 …
DMA #8 came back: status 0x0 irq 0x08000081 tx 0x0 rx 0x0 len_in tx 8 rx 8 after 2 polls   ← 数据其实到了
DMA failed: status 0x0 irq 0x08000081 tx 0x0 rx 0x0 …
engine bring-up 1: 00 00 00 00 00 00 (no answer, opcode 3)                                  ← 冤杀的结果
engine bring-up 2: 20 00 00 32 65 03 (panel answered, opcode 3)                             ← 同一条路径，立刻对
…
fw dl bld: ilm des 0x000000 len 0x01d1bb | dlm des 0x100000 len 0x014107                    ← 写寄存器是通的
fw chunk @0x00000 write failed (try 1)
fw chunk @0x00000 write failed (try 2)
fw dl part 0 @0x00000 write failed after 0 chunks
```

`touch fw dl start at 193.184 s` → `touch fw FAIL at 193.392 s`：整轮 **208 ms**。
扣掉 35 ms 的 SW 复位等待还有约 170 ms，而一个 `0xFC00` chunk 在 600 kHz 下要走
**860 ms**——所以两次重试都在写完前几 KB 的时候断了：252 条命令里只要有一条碰上
迟到的状态位，整块就被判废。不是面板没准备好，不是时序，是没有把"引擎已经认下的那笔"
算成功。

另外 `fw dl bit-bang pace 4 us` 这行是误导：引擎活着，下载走的是 DMA。
这行以前无条件打印，touch168 起只在 pads 路径打；新增的 `fw dl path …` 会直说走哪条总线。

### touch168（`0238-liuqin-touch-dma-status-late.patch`，7 hunk）

1. **判据**：`M_CMD_DONE` + 无错误位、但 `TX/RX_DMA_DONE` 为 0 时，再轮询 2 ms；
   仍不出现就以 **`M_CMD_DONE` + 两个 `LEN_IN` 都到长度** 接受这笔（TX 字节全部从内存
   取走、RX 字节全部写回内存），并打印命中的是哪条判据。调用方本来就有 chunk 回读与
   固件 CRC 兜底。
2. **日志**：失败打印不再被 `DMA #N <= 8` 挡住（最多 24 条，含 `cmd/len/status/irq/
   tx/rx/len_in`）；CS assert/deassert 失败也打 `irq/status`；`fw chunk … write failed`
   带最后一次传输的 `cmd/len/irq`。
3. **可见性**：每次下载尝试把 `tch_dma_count` 归零，下载头 8 次传输恢复可见
   ——touch167 恰好死在头几次，而计数早在 bring-up 时就超过 8。
4. **吞吐**：引擎路径一次发 `TCH_DMA_MAX`（16128 B），一个 `0xFC00` chunk 从 252 条命令
   降到 4 条；pads/FIFO 仍按 256 B。

产物：内核 `98a60e2a…`（17,036,504 B）、payload `38477d88…`、
`shim/out/shim-boot-touch168-dmastatus.img`（`1f5b7036…`，`shim/out/shim-boot.img` 同一张）。
启动行不变（567 B）。

### 下一次日志怎么读

| 看到 | 含义 | 下一步 |
|---|---|---|
| `DMA #N accepted on command done + len_in …` / `status latched … after the command` | 判据成立，传输真的完成了 | 看 `fw dl part 0…4` → `state now 0xa? running` → `fwinfo … ver ok` |
| `DMA #N failed: cmd … irq 0x…` | 第一次看到下载内部真正的失败原因 | `0x2`=OVERRUN、`0x4`=ILLEGAL、`0x8`=FAILURE、`0x08000000`=RX_FIFO_LAST（非错误位） |
| `CS assert failed irq 0x…` | CS 命令在双工之后失效 | 给 CS 单独做复位序列 |
| `write failed … last dma cmd 1 len 3` | 失败就在页写那 3 个字节 | 同一笔连做两次重试并打前后 `SE_GENI_STATUS` |

### 之前判断的修正

- "写入没问题，读不回来"——touch167 之前所有这类结论都建立在同一个过严判据上，
  一律作废；一次传输是否成功只看 `M_CMD_DONE` + 无错误位 + `LEN_IN`（状态位只作旁证）。
- "pads 兜底能保证这一轮出帧"——touch167 没有落到 pads（引擎通了），所以 pads 的
  固件下载路径仍然**没有被验证过**；它只在引擎三次读都失败时才会启用。

## 2026-10-28 12:1x — `168.txt` 和 `167.txt` 是同一份文件；touch169 起驱动开机自报 build stamp

### 一句话结论

这一轮 **没有新的硬件数据**：用户附的 `168.txt`（381,522 B）与上一轮 `167.txt`
sha256 完全相同（`58af40c6e9b888505691121739886fe73fbe0d19e3d33fc20bde3947a4d46750`），
内容就是 touch167 那次启动。touch168 的判据改动还没在真机上跑过。

### 怎么确认它只能是 touch167 打印的

四条互相独立的判据（任何一条单独就足够）：

| 日志里 | touch168 的行为 |
|---|---|
| `fw dl bit-bang pace 4 us (2 us was the corrupt one)` | 这句话已被 `if (tch.bitbang)` 包住；该轮引擎 DMA 活着，168 不可能打 |
| `fw chunk @0x00000 write failed (try 1)` | 168 的格式是 `write failed (try 1, last dma cmd N len N irq 0x…)` |
| 整轮没有 `fw dl path …` | 168 每次下载尝试必打一行，写明 `pads` / `FIFO` / `DMA` |
| `DMA #8 came back: … len_in tx 8 rx 8 after 2 polls` | 同一情形 168 打的是 `accepted on command done + len_in …` |

镜像侧也对过：`shim-boot-touch167-opcode3.img` 里 `fw dl path`、`accepted on command
done`、`status latched`、`last dma cmd` 四个串一个都没有；`shim-boot-touch168-dmastatus.img`
里四个全有。时间线（`touch fw dl start at 193.184 s`、`attempt 1 state now 0xfc`、
`fwinfo 6a 18 …`）也与 167 那轮逐字一致。

结论：旧日志被再存了一次，不是新一次启动。**不据此改代码。**

### touch169（`0239-liuqin-touch-build-stamp.patch`，2 hunk）

只加"自报家门"，功能与 touch168 完全一致：

```
LIUQIN TOUCH: driver touch169 dma-late-status+16k-chunks
LIUQIN TOUCH: SPI bring-up in 10 s
```

打印点在 probe 线程最前面（`tsleep` 之前），日志被截断也看得到。

产物：内核 `c62df06f…`（17,036,504 B）、payload `bfe93ed6…`、
`shim/out/shim-boot-touch169-dmastatus.img`（`21e02090…`，32,014,336 B，
`shim/out/shim-boot.img` 同一张）。启动行不变（567 B）。

### 下一次日志怎么读

| 看到 | 含义 | 下一步 |
|---|---|---|
| 第一行不是 `LIUQIN TOUCH: driver touch169 …` | 旧镜像或旧日志 | 直接退回，不看后面 |
| `DMA #N accepted on command done + len_in …` / `status latched … after the command` | 判据成立 | `fw dl path serial engine, DMA` → `fw dl part 0…4` → `state now 0xa? running` → `fwinfo … ver ok` |
| `DMA #N failed: cmd … irq 0x…` | 下载内部真正的失败原因 | `0x2`=OVERRUN、`0x4`=ILLEGAL、`0x8`=FAILURE、`0x08000000`=RX_FIFO_LAST（非错误位） |
| `write failed … last dma cmd 1 len 3` | 失败就在页写那 3 个字节 | 同一笔连做两次重试并打前后 `SE_GENI_STATUS` |

## 2026-10-28 12:2x — touch169 上机：DMA 通了，下载死在"等待没跟着长度走"（16128 B 要 215 ms，轮询只给 50 ms）

### 一句话结论

DMA 路径、迟状态判据、16 KiB 分片全部生效，引擎确实在按 600 kHz 往外发数据；
下载失败的原因是 **自己的超时**：一个 16128 B 的命令要 215 ms，而 `tch_dma_xfer()`
只等 `1000 × 50 µs`（实测 ≈70 µs/轮 ≈ 70–75 ms），时间一到就判失败，
此时命令还是 `ACTIVE`、没有错误位、`LEN_IN` 只走到 5.5–6.2 KB（正好 73–83 ms）。

### 一次就过的部分（touch169 的改动全部验证）

```
LIUQIN TOUCH: driver touch169 dma-late-status+16k-chunks        ← 自报家门，镜像确认
DMA #8 came back: status 0x0 irq 0x08000081 tx 0x00000003 rx 0x00000003 len_in tx 8 rx 8 after 3 polls
engine bring-up 1: 20 00 00 32 65 03 (panel answered, opcode 3)  ← 第一次就答
SE4 receive is live (duplex opcode 3, div 32, 600 kHz)
fw dl path serial engine, DMA                                    ← 走的是 DMA，不是 bit-bang
DMA #29 status latched 1 polls after the command (tx 0x3 rx 0x0) ← 迟状态判据在下载中命中
```

### 失败的三次尝试（一模一样）

```
DMA #27 failed: cmd 1 len 16128 status 0x00000041 irq 0x00000000 tx 0x00000000 rx 0x00000000
    len_in tx 5504 rx 0 after 1000 polls, dma_mode_en 0x1 general_cfg 0xf
fw chunk @0x00000 write failed (try 1, last dma cmd 1 len 16128 irq 0x0)
```

六次超时的 `len_in tx`：**5504 / 5792 / 5632 / 6208 / 5696 / 5792**。

| 量 | 值 |
|---|---|
| 16128 B @600 kHz | **215 ms** |
| 轮询预算 `1000 × 50 µs` + 7 次 MMIO 读 | ≈ **70–75 ms**（70 µs/轮） |
| 观测停止点 5504–6208 B @600 kHz | **73–83 ms** |
| `status 0x41` | bit0 `M_GENI_CMD_ACTIVE` + bit6：命令还在跑 |
| `irq 0x0` | 没完成、也没出错 |

若真是卡住，六个点会停在同一字节数；若是 DMA 长度被截断（13 位 → 7936），也会停在同一值。
都不是：停止点跟着时钟走，所以是**截止时间**，不是总线。

### touch170（`0240-liuqin-touch-dma-wait-scaled.patch`，5 hunk）

1. **等待时间跟着长度走**：`budget = (len × 8 / spi_hz) / 50 µs + 400`，下限 1000、
   上限 100000。16128 B → 4700 轮 ≈ 235 ms 名义（实测约 330 ms）。
2. **超时后取消命令**：`M_GENI_CMD_ACTIVE` 仍立着就写 `SE_GENI_M_CMD_CTRL_REG (0x604)` =
   `M_GENI_CMD_CANCEL (bit2)`（Linux `geni_se_cancel_m_cmd` 的同一条路），等 ACTIVE 落下、
   清 IRQ。以前超时后直接发下一笔，是把重试叠在还活着的命令上。
3. **进度三点采样**：25/50/75% 各记一次 `SE_DMA_TX_LEN_IN`，失败时打印
   `tx progress at 25/50/75% of the wait: a b c of len`。这是下一次的判据：
   递增 = 只是慢（预算或时钟问题）；相同 = 真卡住（才回头查总线）。

### 下一次日志怎么读

| 看到 | 含义 | 下一步 |
|---|---|---|
| 无 `LIUQIN TOUCH: driver touch170 …` | 又是旧镜像/旧日志 | 退回，不看后面 |
| `fw dl part 0 … 15` 逐段出现、`crc …` 有值 | 下载真的通了 | 看 `attempt 1 state now 0xa? running` → `fwinfo … ver ok` |
| `failed … after 4700 of 4700 polls` + `tx progress` 递增 | 还是不够快 | 按实测速率反推 budget，或降 SPI 分频前的第一步先测真实 SCLK |
| `tx progress` 三点相同 | 真的卡住 | 此时有 `status`/`len_in` 实证，再回总线/SMMU |
| `cancel did not clear` | 取消序列无效 | 改用 abort，并打印 `M_CMD_CTRL_REG` 前后值 |

### 顺带确认

- 600 kHz 是当前真实速率（`len_in` 的推进速度 = 600 kbit/s），下载 240 KiB 需要
  **约 13.8 s**；这是 SPI 时钟决定的，与 DMA 无关。下载通了之后，提速（div 32 → 16/8）
  才是下一个收益点。

## 2026-10-28 12:3x — touch170 上机：DMA 打通了（下载 3 s、镜像逐字节相同、`BOOTED`、`0xa3`）；剩下的 0xFD 是 CRC 标志没清，而清除代码写错了地址

### 一句话结论

**DMA 这条线结束了。** 固件下载在 3 s 内完成，201,410 B 回读与文件完全一致，
芯片 `state 0xa3 (NORMAL_RUN)`，fwinfo 与厂商验收值逐项相同。
没有触摸帧的原因不在 DMA，而是 **IC 停在 CRC-fail 重启循环**（报告区恒 `fd`），
而我们一直在写 `0x00000`——ILM 的第一个字节——去"清 CRC 标志"，标志根本没被清。

### 已达标的证据（170.txt）

```
fw dl part 0 @0x00000 len 0x1d1bc bin 0x200: 2 chunk 1615 ms
fw dl part 1 @0x100000 len 0x14108 bin 0x22000: 2 chunk 1113 ms
fw verify: image identical to the file (201410 bytes)
fw dl BOOTED: 201924 B 6 chunks 3 s, state 0xa0@125860 1 polls
attempt 1 state now 0xa3 (running)
fwinfo 19 e6 26 3c 46 50 70 80 00 0a 00 01 f1 01 00 26
report space 18000x28800, 10 fingers
```

| 项 | 值 | 厂商验收值 |
|---|---|---|
| fw_ver / ~fw_ver | `0x19` / `0xe6` | `0x19`，和为 `0xff` ✓ |
| x_num × y_num | `38 × 60` | `38 × 60` ✓ |
| 触摸面积 | `18000 × 28800` | 同 ✓ |
| 下载耗时 | 3 s（600 kHz） | — |
| 回读比对 | 201,410 B 全同 | — |

### 没有触摸的原因

```
dump1 125800: fd fd fd fd fd fd fd fd …          （三次 INT 全是 fd）
beat 12/24/36s: running, 937/1878/2814 event reads (0 failed), 0 contact frames
```

`fd` + 约 13 ms 宽的周期 INT 脉冲 = **IC 在 CRC-fail 重启循环**，这就是
NOTES 第 183 步那次"摸了几下之后突然死掉"的同一个现象，只是这次从开机就在里面。

### 找到的代码错误（touch170 及更早）

`revive` 和 `tch_nvt_bringup()` 里"清 CRC 标志"写的是：

```c
tch_nvt_write8(0x00000, 0xa5);   /* 0x00000 是 ILM 的第一个字节，不是标志 */
```

厂商 dmesg 给出的是 `crc_err_flag_addr=0x1fb535`（NT36523 的 `0x3f135` + `0x1c4400`），
而且 `nvt_stop_crc_reboot()` 的顺序是：**先把 MCU 打回 idle**（`0xaa` 两次写
`swrst 0x1fb43e`）→ 写 `0xa5` 到标志 → 回读等于 `0xa5` 才算清掉，最多 5 次。
所以这个标志很可能是**从某次失败下载之后就一直是置位的**，之后每次启动都掉回循环。

### touch171（`0241-liuqin-touch-crc-flag.patch`，7 hunk）

1. `tch_crc_flag(why, fix)`：读 `0x1fb535` 和 trim id；
   `fix` 时按厂商顺序清除（`0xaa`×2 → 1 ms → `0xa5` → 回读，最多 5 次），
   输出 `crc flag 0x%02x -> 0x%02x in %u tries, trim …`。
2. 调用点：`before download`（每次尝试）、`after boot`（只读，避免把刚起来的固件打回 idle）、
   两条 `revive` 路径（真清）。
3. 删掉两处写 `0x00000` 的假清除。

### 下一次日志怎么读

| 看到 | 含义 | 下一步 |
|---|---|---|
| `before download: crc flag 0x?? -> 0xa5` | 标志之前是置位的，0xFD 的根因就是它 | 看 `after boot: crc flag 0xa5` 之后有没有 `first frame 0a …` / `finger 1 ev …` |
| `after boot: crc flag 0xa5, trim 20 00 00 32 65 03` | 芯片干净、正常跑 | 触摸应该出帧 |
| `-> 0x00` / 非 `0xa5` | 写不进去 | 改成 `0xaa` → `0x35`（unlock）→ `0xa5` |
| flag 本来就是 `0xa5` 且仍 0xFD | 0xFD 另有原因 | 做 INT 期间整块 `0x125800–0x125880` 转储 + `0x51` 握手字节读写 |

## 2026-10-28 12:4x — touch171 上机：CRC 标志假设作废；固件其实是"健康空转 18 s 然后自杀"，缺的是厂商那条 0x51 握手清除

### 一句话结论

`crc flag` 下载前是 `0x00`、引导后还是 `0x00`，**不是 CRC 锁存**。
真正的新事实是时间线：固件开机后**是活的、在扫**（报告区是 `ff 00 00 00 00 00` 的空记录，
18 s 内一个字节都没变），到第 ~81 s 才整块翻成 `fd`。
厂商 probe 在 `nvt_get_fw_info()` 之后会立刻 `nvt_clear_fw_status()` 清掉
`0x125851` 这个握手字节，本驱动从来没清过——固件等不到应答，约 18 s 后自己进恢复循环。

### 171 的关键行

```
driver touch171 crc-flag-cleared
before download: crc flag 0x00 -> 0xa5 in 0 tries, trim 20 00 00 32
after boot: crc flag 0x00, trim 20 00 00 32          ← 假设作废：不是 CRC 锁存
fw verify: image identical to the file (201410 bytes)
fw dl BOOTED: 201924 B 6 chunks 3 s, state 0xa0@125860 1 polls
attempt 1 state now 0xa3 (running)
fwinfo 19 e6 26 3c 46 50 70 80 … (ver 0x19 ok, 38x60 sensors)
```

### 时间线（本轮真正的收获）

| 时刻 | 报告区 | 含义 |
|---|---|---|
| 63.2 s | `ff 00 00 00 00 00 ff 00` | 空记录，**固件活着、在扫** |
| 63.2 → 81.1 s | 一个字节都没变 | 18 s 空转，0 帧 |
| 81.1 s | `fd fd fd …` | 固件掉了，INT 变成 ~13 ms 周期脉冲 |

170 那轮同一翻转在 79.8 s，也是引导后约 18 s。**固定延迟**，说明是固件内部看门狗，
不是总线、不是 SMMU、不是下载。

### 对照厂商流程

```
nvt_get_fw_info()
nvt_clear_fw_status()        ← 写 0 到 EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE
                               (event+0x51 = 0x125851)，回读确认，最多 20 次
```

本驱动直到 touch171 都没有这一步。

### touch172（`0242-liuqin-touch-fw-status-clear.patch`，5 hunk）

1. `tch_clear_fw_status(why)`：读 `0x125851` → 写 0 → 回读，最多 20 次 × 10 ms，
   打印 `fw status 0x51 0x%02x -> 0x%02x in %u clears`。
2. 引导成功（`ready`）后立刻清一次。
3. 心跳行加 `fw 0x51=0x%02x`，且每次心跳发现非 0 就再清（最多 8 次）：
   既保持"有人应答"，又能从日志看出固件何时重新置位。

### 下一次日志怎么读

| 看到 | 含义 | 下一步 |
|---|---|---|
| `after boot: fw status 0x51 0x00 -> 0x00`，心跳一直 `0x51=0x00`，81 s 不再翻 `fd` | 固件被救活 | **这时去摸屏幕**，应有 `first frame 0a …` / `finger 1 ev …` |
| `0x51` 一开始就非 0，清完就开始上报 | 固件在等握手 | 触摸链路通了 |
| 心跳里 `0x51` 又变非 0 且清不掉 | 固件在报错 | 把 `0x51` 的值和 `0x125800–0x125880` 整块打进心跳 |

## 2026-10-28 12:4x — **通了**：触摸可用、多指可用，固件 216 s 没掉过

### 结论

DMA 这条线的目标全部达成：

| 目标 | 结果 |
|---|---|
| SE DMA 发/收 | opcode 3 双工，600 kHz，`tx 0x3 rx 0x3` |
| 固件下载 | 201,410 B，回读与文件逐字节相同，3 s |
| 固件启动 | `state 0xa3 (NORMAL_RUN)`，fwinfo `0x19 / 38×60 / 18000×28800` |
| 触摸上报 | 216 s 内 3,380 个接触帧，0 次读失败，0 次 `fd` |
| 多指 | `finger 2` / `finger 3` 均有派发 |
| 派发到 iOS | backboardd `3464 samples, 182 ups, 0 dispatch failures` |

### 最后一块拼图：`0x125851` 的那次写

`nvt_clear_fw_status()`（vendor 在 `nvt_get_fw_info()` 之后调用一次）读回前后都是 `0x00`：

```
after boot: fw status 0x51 0x00 -> 0x00 in 1 clears
```

所以固件**不是等某个值变成 0，而是等这次写事务本身**——这是它的"主机还活着"应答。
在此之前（≤ touch171）这一步从来没做过，固件在开机约 18 s 后自行进恢复循环：

| 启动 | 63 s | 81 s | 之后 |
|---|---|---|---|
| touch171 | `ff 00 00 00 00 00`（健康空转） | `fd fd fd …` | 一直 `fd`，0 帧 |
| touch172 | 写了一次 `0x51` | 正常出帧 | 216 s 一直正常 |

**别把这次写优化掉。** 即使读出来已经是 0，引导后也必须无条件写一次；
`tch_clear_fw_status()` 的心跳补充只是"非 0 时再清"，不能替代引导后那一写。

### 已知好镜像

```
shim/out/shim-boot-touch172-fwstatus.img  a2a848faa1e5c71cc4f7d18c3fffe3ae38c343e498f524f1d16ff9ae36013883
payload                                  5a4308e3f2d37dbd1f9061791466456e333c29a459972810e666c6be1a397234
kernel (17,036,648 B)                    1bf5423cf4a66809998c45d9da9f7a7b9913e95cc83361d18daa3f8de9345e6e
```

启动行 567 B，未变。补丁序列 241 个，`0242-liuqin-touch-fw-status-clear.patch` 是最后一块。

### 未做/可做（都不是现在该动的）

- SPI 仍是 600 kHz（`div 32`）：固件下载 3 s、轮询 12 ms/帧。想更快可试 `div 16`，
  但当前组合已实测可用，**改之前要单独一轮**。
- 睡眠/唤醒后的恢复没有验证过；驱动里的两条 revive 路径仍在，本轮一次都没触发。
- 触摸坐标已经是 18000×28800 → 显示 1440×2880 的映射，缩放与镜像参数（swap/flip）
  按 `liuqintouch*` 启动参数处理，不属于 DMA 这条线。
