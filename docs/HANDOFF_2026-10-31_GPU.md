# 交接：Xiaomi Pad 6 Pro（liuqin / SM8475 / Adreno 730）GPU 收尾

> 给下一个会话用。目标只有一件：**把 GPU 修到能用**（能渲染、能接进 UI）。
> 完整历史在 `NOTES.md`（最新步号见文首“（当前）”，本轮是第 370 步）；本文只写“现在必须知道的”。

## 0. 工作方式（机主定的规矩，别改）

- 机主**不读代码**。每次汇报必须说清：**改了什么、为什么、日志里看哪一行**。
- 每次出镜像都要报 **sha256**；镜像只改 boot，**root 不动**。
- 机主负责刷机并回传日志；**不要自己刷**、不要假设能连设备。
- 日志只认机主回传的 `.txt`（panic 屏照片只在拿不到 .txt 时作数，且要说明是照片）。
- 交接写在 `NOTES.md` 顶部（标题带“（当前）”），**事实与推断分开**。

## 1. 构建与出镜像

```sh
cd ~/Documents/deepseek-harness/default-workspace/iPadOS-liuqin
WORK=/Users/qprs/liuqin-build3 JOBS=8 tools/build-local.sh        # 内核

K=/Users/qprs/liuqin-build3/fakeroot/System/Library/Kernels/kernel.development.liuqin
python3 tools/mkpayload.py "$K" shim/out/liuqin.adt shim/out/payload-NAME.bin \
    --cmdline "$(cat tools/boot-cmdline-gpu.txt)" --fwarea ~/liuqin-root/fwarea.bin
LLD=/Users/qprs/liuqin-build3/ld.lld shim/build.sh "$PWD/shim/out/payload-NAME.bin"
mv shim/out/shim-boot.img shim/out/shim-boot-NAME.img
shasum -a 256 shim/out/shim-boot-NAME.img shim/out/payload-NAME.bin
```

- 源码在 **`~/liuqin-build3/xnu/iokit/NativeAdreno/`**（不是本仓库）：`NativeAdrenoService.cpp`（init/CP/探针）、`NativeAdrenoRing.cpp`（ring framing / fence）。
- boot cmdline 已存 `tools/boot-cmdline-gpu.txt`（**上限约 607 B**，超了会被截断）。
- 符号化 panic：`atos -o ~/liuqin-build3/build/xnu.obj/DEVELOPMENT_ARM64_LIUQIN/kernel.development.liuqin.unstripped -arch arm64 <pc> <lr>`；反汇编用 Xcode 的 `llvm-objdump --arch=arm64 --disassemble --start-address=… --stop-address=…`。
- root（**不要动**）：`~/liuqin-root/ipad71-root-wrap16-gpu11-wr.img` 17,179,869,184 B
  `f7043229b7a628ea69aa171a606fad6d01120a21821652b32814a4b6213e565f`（zip 5,641,617,221 B `f76ac301…`）。

## 2. 现状（已验证的事实）

**能用**：GMU 冷启动 + HFI 六步；OOB 拉起 GX；GPU 自己的 SMMU 上下文（CB2）+ 两条 DT stream；CP 启动、SQE 固件、ring、fence、任意 PM4 执行；CP 自己发起的写（`CP_MEM_WRITE`/`MEM_TO_MEM`/`REG_TO_MEM`）；客户端（turnip/Metal 插件）能起来并提交。

**当前判断（2026-10-31 第 370 步，改用 HakiOS 链路）**：内核侧已经结束——metal98 的
`qcom_scm_restore_sec_cfg(PAS_GPU,0)` 返回 -4，secure SMR 仍空、trust 写不了。机主确认 6 Pro 的
**原生 Linux（Ubuntu）能 boot、几乎全部硬件可用**，但 custom Android 17 的内核闭源且没有
`CONFIG_KEXEC`。**决定换成 HakiOS 链路**：Android fastboot → HakiOS UEFI（Mu-Silicium + HakiOS
改动，或 HakiOS U-Boot）→ `xnuloader` → XNU（port 的 XNU/ADT/fwarea）→ launchd；GPU 的 secure
init 放进 HakiOS UEFI 的 QcomPkg/SCM。kexec（native Linux 先启动）保留为 fallback。

仍然成立的事实（全部来自机主回传日志）：

| 事实 | 日志出处 |
| --- | --- |
| `CP_BLIT`/`CP_EXEC_CS` 被接受、fence 退休、`cp_int 0x0`（无错） | metal62/63 `2234.txt`/`2256.txt` |
| A2D 的 6 条寄存器写被 CP 执行（读回逐字节正确） | metal63 `2256.txt` `blit regs a2d …` |
| A11 重放死在第一个包：`cp_int 0x1`（`CP_OPCODE_ERROR`）、`rptr 72`、`rb_ccu` 读回 0 | metal76 `1138.txt` |
| A11 重放修好后 **CP 全程干净**，但 A2D 仍 `filled 0/512` | metal77 `liuqin-log-20261031-115716.txt` |
| `CP_ROQ_THRESHOLDS`/`CP_MEM_POOL_SIZE` 偏移读回 `0xdeafbead`（无效寄存器） | metal78 `1208.txt` |
| A11 的 CCU cache offset=0 + CCU clean 都退休，A2D 仍 `0/512` | metal79 `liuqin-log-20261031-121832.txt` |
| SPTPRAC 读 GMU-core 窗口得 0/0/0（窗口不对，作废） | metal80 `1238.txt` |
| 安全变体 `base/fmt30/mib8x8/hwcg0` 全 0；`addr mode` 写寄存器 panic | metal82（照片）/ `81.txt` |
| 引擎写事务 `sid=0x1b08 far=0xd5fe77b6` 没有 SMR 映射，SMMU fault | metal81 `81.txt` |
| 复用 GMU SMR slot0 时 **SMR0 本身** data abort（FAR = SMMU base + 0x800，安全寄存器） | metal83（照片） |
| 往 gslot1 写 `0x84001b08` 时 **SMR 值非法** data abort（FAR = base + 0x808；SID 字段 8 位） | metal84（照片） |
| 往 gslot1 写 SID 8（`0x84000008`）**同一处** data abort；8 也不可用 | metal85（照片） |
| 只读 SMMU 表：`IDR0=0x002e0406`，6 个 SMR；slot0/1/2 = 5/0/1，slot3/4/5 空；fault `0x1b08`/`0x1b00` = **SID 27 + SSID 8/0** | metal86 `86.txt` |
| A3 扫槽在 GPU context 之前读 slot0（FAR = base + 0x800）data abort；SID27 还没执行 | metal87（照片） |
| 写 slot3 SID27 时 **slot3 SMR 本身** data abort（FAR = base + 0x80c）：slot3 也是安全槽 | metal88（照片） |
| 写 slot2 SID27 也 data abort（FAR = base + 0x808）：非安全槽容不下 SID27 | metal89（照片） |
| stock `kgsl_iommu.c` 有 `qcom_scm_kgsl_set_smmu_aperture(cb)`（SVC_MP/0x1b）；port 从未发过 | Xiaomi 5.10 源码 |
| 客户端 A2D 块 `CP_BLIT` 后 `CCU_CLEAN`，CP 曾停在这里；目的页读回 `deadbeef` | metal78 `1208.txt` fence 11 |
| 客户端 `T … 0/4096`、`M … 999 wrong`；超时行 `cp bv roq 0x0/0x0/0x0` | metal77/78/79 日志 |
| **`SP_UPDATE_CNTL=0x01fffeff` 让 CP 挂住**（等 SP，`rptr 82`；这个头本身是合法的） | metal75 `0014.txt` |
| `HLSQ_FLUSH`（事件 0x07）**不等引擎**，fence 正常退休 | metal74 `0005.txt` |

**已排除**（不要再重复排查）：SMMU 翻译/stream 匹配（metal61 审计：SID 0→SMR1→CB2、SID 1→SMR2→CB2、`unmatched 0`、`gfsr=0`）；CP 寄存器保护（`prot 0x0`）；GBIF QSB 侧配置；LLCC/系统缓存 SCID（引导链本来就是 0/0）；UCHE 刷（跨提交刷也没用）；客户端读回路径（内核 + DRAM 两路读一致）；HWCG 时钟门控（关掉一样）；RBBM 软复位（`0x73` 本来就是 0）；**a6xx 的 `CP_ROQ_THRESHOLDS`/`CP_MEM_POOL_SIZE` 偏移**（a730 读回 `0xdeafbead`，是无效寄存器）。

## 3. 踩过的坑（别再犯）

1. **`CP_SET_MARKER` 0x101（保持 IFPC 关闭）会导致重启循环**，而且早到没有 panic 画面。要**保持 0x100**（`NativeAdrenoRing::emitFramedFence`，`sheng-gpu-ifpc=1` 才用 0x101）。这是照抄上游 `a7xx_submit` 踩的。
2. **`native_adreno_smmu_window` 在 `startCp` 里是垃圾指针**（它的赋值在 `start()` 里更晚），解引用会 panic 两次。需要 SMMU 窗口就用 route 块里的 `smmu_base`（那里发布到文件静态 `g_smmu_route`）。
3. **SMMU 只实现 SMR 槽 0..2**：写槽 3+ 同步外部中止（panic）；**带 MASK 的 SMR 写也会中止**；CP 起来之后**重复写同一个 SMR 也会中止**。→ 探针**不要写 SMMU**，要换 SID 就在 route 块里写一次（预留 `sheng-gpu-sid=<n>` 的口子，当前已删）。
4. 探针里别发会“等引擎”的包（`SP_UPDATE_CNTL`、`CCU_CLEAN_*`）——会把 CP 挂死，后面的日志全废。
5. boot 参数里 `sheng-gpu-*` 现有：`blit`（探针开关，默认 1，cmdline 里 =1）、`bisect`（A7/A8/A9 二分，默认 0）、`pwrup`（power-up 表，默认 1，**已验证安全**）、`ifpc`（默认 0 = 0x100）、`hwcg`、`sidwild`/`sid`（已废）。
6. **type-4 头有 parity 位（bit 27）**：手抄/重建 PM4 流时必须用 `AdrenoPM4Type4` 按 reg/count 重算，不能照抄 dump 里的头。`488e0701`（`RB_CCU_CNTL`）会被 CP 判 `CP_OPCODE_ERROR`（`cp_int 0x1`），停在第一个包；`408e0701` 才是规范值。A0 能跑是因为它的头恰好都对。
7. **A7XX 的 `RB_CCU_CNTL` 写后要 WFI**（Mesa 9a67f003：静态 CCU 配置要 wait-for-idle 才生效）；客户端旧流可能没带这个 WFI，写下去会等住。
8. **a6xx 的 CP 派发偏移在 a730 上不成立**：`0x08c1/0x08c2/0x08c3` 读回 `0xdeafbead`（无效寄存器死值），写了也不生效；不要拿 a6xx 的 `CP_ROQ_THRESHOLDS`/`CP_MEM_POOL_SIZE` 偏移当 a730 的。metal78 已证伪。
9. **A7XX 的 `RB_CCU_CACHE_CNTL`（0x88e5）bit 31 是 `COLOR_OFFSET`，不是 A6XX 的“使能”位**；客户端写 `0x80000000` 在 sysmem 下是无效 GMEM 偏移。metal79 已证伪：把它改 0 + 补 CCU clean 后 clean 退休，A2D 仍 `0/512`；这条不要再花时间。
10. **A2D 不写、SP 不答、RB/CCU 活** → 先查 **SPTPRAC（SP/TP）电源/时钟**，不要先怀疑 SMMU/CCU。
11. **SPTPRAC 的寄存器在 GPU 窗口，不在 GMU-core 窗口**：`a6xx_reg.h` 的 `0x1F8D0`（status）、`0x1A880`（clock）、`0x1A881`（power）要用 `gpu_read32`/`gpu_write32`；metal80 用 GMU-core 的 `0x58d0/0x880/0x881` 只读到 0，结论作废。metal81/82 用 GPU 窗口读回 `0xffffffff`，说明 **GPU 窗口只有 0x10000**，0x1F8D0 在窗口外；SPTPRAC 状态要走 GMU 的 GPU 窗口（`ShengGmuGpu`）或别的方式。
12. **SMMU fault 记录会给出引擎的真实 SID**：metal81/82 的 `smmu fault … sid=0x1b08 far=0xd5fe77b6` 说明引擎写事务用 SID 0x1b08，不是 DT 的 0/1，也不是 GMU 的 5；SMR 没映射 → 引擎写被 abort，CP 不等，所以 fence 正常但数据不落。
13. **不能复用 GMU 的 SMR slot0**：metal83 把 slot0 写成 SID 0x1b08，结果 **SMR0 本身** data abort（FAR = SMMU base + 0x800，ESR 0x96000010）；这个 qsmmu 的非安全可写 SMR 只有 gslot1/gslot2（slot0 和 slot3+ 都是安全寄存器）。
14. **`sid` 的高低字节要拆开看**：metal86 的 fault `sid=0x1b08`/`0x1b00` 拆开是 **SID 0x1b（27）+ SSID 0x08/0x00**；引擎用的是 **SID 27**，既不是 8 也不是 `0x1b08`。SMMU 的 `IDR0=0x002e0406` 表示 **6 个 SMR**；slot0/1/2 = SID 5/0/1，**slot3/4/5 是空的**（SMR 0，S2CR type 2）。之前 slot3+ 写中止是**带 mask** 的 SMR，不是槽本身；metal87 用 **slot3、mask 0、SID 27**。
15. **A3 的 0..15 盲扫不能在 GPU context 之前跑**：metal87 这次启动在 A3 里读了 slot0（FAR = SMMU base + 0x800），slot0 还是安全/未配置，读就 data abort（PC 1758）。metal88 直接把 GPU slots 固定为 **1/2**、跳过 A3 扫描；只读的 IDR/slot 表放在 GPU context 之后（metal86 已证明安全）。
16. **slot3/4/5 是安全槽，只有 slot1/slot2 可写**：metal88 写 slot3 的 SMR 时 data abort（FAR = base + 0x80c，PC 1815）。metal86 里 slot3/4/5 显示“SMR 0、S2CR type 2”只是**未配置**，不代表可写。引擎 SID27 只能占 slot2（让出 SID1），见 metal89。
17. **`*_ADDR_MODE_CNTL` 那组不能整组盲写**：metal82 的 addr-mode 组里至少一个寄存器（SECVID 类）会 **data abort**（panic 照片，ESR 0x96000010）；要测就一次一个、写前打日志。
18. **stock kgsl 有一条我们从未发的 SCM**：`qcom_scm_kgsl_set_smmu_aperture(context->cb_num)`（`drivers/gpu/msm/kgsl_iommu.c`），SCM `SVC_MP 0x0c` / `CP_SMMU_APERTURE_ID 0x1b`，arg0 `0xffff0000|cb`，args1..3 `~0`，arginfo 4；SMC fn `0x42000c1b`。它把 CP 的 SMMU aperture 指到 GPU context bank。metal90 用 `smc #0`（`SCM_INTERRUPTED` 循环）在 GPU context/route 之后发它；见 §5。
19. **GPU 一直在 secure mode**：stock `adreno_a6xx.c` 在 CP SQE 启动后先 `adreno_zap_shader_load()`，再发 `CP_SET_SECURE_MODE`（payload 0）离开 secure mode；没有 zap 才写 `RBBM_SECVID_TRUST_CNTL=0`（我们写会 abort）。port 两条都没做，所以引擎事务带 secure SID 27、匹配不到非安全 SMR。metal91 的 SCM aperture 成功但没用；metal92 先单独试 `CP_SET_SECURE_MODE`（0x70668001）。

## 4. 日志怎么拿（机主环境）

- **这台机器没有 UART**：内核 console 是 framebuffer 伪串口（ADT 节点 `liuqin-fbcon`，`pexpert/arm/pe_serial.c:704`）。所以“串口”修不了，只能靠下面两条。
- 持久日志环：`/dev/block/sda36`（hfs）**末尾 1025 个 4 KiB 块**（头 `LIUQINLOG` + 4 MiB 环），内核在 UFS 起来后每 `ufslogsecs` 秒刷一次。
  - Android（root）：`adb push tools/liuqin-log-android.sh /data/local/tmp/` 然后 `adb shell su -c 'DEV=/dev/block/sda36 sh /data/local/tmp/liuqin-log-android.sh'`。
  - recovery：`fastboot boot recovery.img` → `tools/liuqin-log-pull.sh /tmp/liuqin.txt`。
- **早期崩溃（UFS 起来之前）盘上什么都没有**，只能靠屏幕/panic 照片。别指望日志。

## 5. 下一步（按顺序）

**当前主线：HakiOS 链路**（2026-10-31 第 370 步）

目标链：

```
Android fastboot
  → HakiOS UEFI（Mu-Silicium + HakiOS 改动，或 HakiOS U-Boot）
  → xnuloader
  → XNU（port 的 XNU/ADT/fwarea）
  → launchd
```

1. **H0：建 `devices/liuqin`**（参考 redwood）
   - clone `hakios-xnu-kernel`，`tools/hakios-device.py new liuqin`；
   - 填 SM8475 SoC 层（`soc/`）、`liuqin.config`、`pull-firmware.sh`、UEFI platform、status；
   - 用 `tools/hakios-device.py sync` 拿 `hakios-uefi` / redwood 作参考。
2. **H1：6 Pro 最小 HakiOS UEFI**
   - `hakios-uefi`（Mu-Silicium + HakiOS）加 SM8475/liuqin platform；
   - `fastboot boot` 起来，GOP/串口、UFS 可读；
   - 这一步不碰 XNU。
3. **H2：xnuloader → port 的 XNU**
   - xnuloader 加载 port 的 XNU（Mach-O `kernel.development.liuqin`）+ `liuqin.adt` + `fwarea.bin`；
   - cmdline 用 `tools/boot-cmdline-gpu.txt`，`sheng-gpu-bringup=0`；
   - 目标：XNU 起来，串口/屏幕有日志；A11 仍 0 没关系；
   - 如果 xnuloader 的 payload 格式不兼容，先 chainload port 的 `shim`（`shim/Image` + payload）。
4. **H3：HakiOS UEFI 里做 GPU secure init**
   - QcomPkg/SCM：PAS init/auth（`a730_zap`）、`restore_sec_cfg`、`set_smmu_aperture`、trust/SMR；
   - 再启动 XNU，`sheng-gpu-bringup=0`，看 A11 是否开始填、`sid` 是否还是 `0x1b08`；
   - 这是 HakiOS 路线成败关键；QSEE 可能同样 -4。
5. **H4：客户端/Metal/UI**
   - A11 通了之后，回到 turnip/Metal、display、touch、UI。

**参考/风险**：
- HakiOS 只支持 redwood（SM7325/a6xx）；6 Pro 的 SM8475/a730 要自己 port。
- HakiOS 的 XNU/driver 可参考，但 port 的 XNU（liuqin、NativeAdreno、display/touch）更靠前；H2 优先加载 port 的 XNU。
- `xnuloader` 的 `fwarea`/payload 兼容性是 H2 的主要工作量。
- H3 的 QSEE SCM 在 UEFI 侧可能和 XNU 一样被拒（`restore_sec_cfg` 在 XNU 是 -4）。

**Fallback：kexec（native Linux 先启动）**
- 原生 Linux 能 boot、硬件全可用；`CONFIG_KEXEC` 自选、无 SELinux、`kexec-tools` 现成；
- kexec 加载 `shim/Image` + `payload-metal79-ccu.bin` + `liuqin.adt`，cmdline `sheng-gpu-bringup=0`；
- custom Android 17 内核闭源且无 `CONFIG_KEXEC`；stock Xiaomi 5.10.81 + stock Android 是另一条。

**不要再刷**：SID/trust/secure-mode 内核图（metal98 之后内核侧已结束）。稳定回退 `metal96`/`metal95`/`metal79`。

## 6. 当前产物速查

| 镜像 | 说明 | sha256 |
| --- | --- | --- |
| `shim/out/shim-boot-metal98-restore-sec.img` | zap 后补 `restore_sec_cfg(PAS_GPU,0)`（已上机；QSEE 返回 -4） | `886e4637…` |
| `shim/out/shim-boot-metal97-trust-after-zap.img` | zap 后写 `RBBM_SECVID_TRUST_CNTL=0`（已上机；仍 data abort） | `23ccdd11…` |
| `shim/out/shim-boot-metal96-post-zap-smr.img` | 只读 post-zap secure SMR（已上机；slot3..5 仍全空） | `d8caa808…` |
| `shim/out/shim-boot-metal95-zap-only.img` | zap 在 CP 前，无 secure-mode 包（已上机；CP 健康，引擎仍 SID 27） | `a0573a5c…` |
| `shim/out/shim-boot-metal94-zap-before-cp.img` | zap 前移 + secure 包（已上机；CP 启动后包仍 opcode error） | `418dc60a…` |
| `shim/out/shim-boot-metal93-zap-secure.img` | zap 在 CP 之后 + secure 包（已上机；zap 成功但 CP 被 reset） | `bea70478…` |
| `shim/out/shim-boot-metal92-secure-mode.img` | `CP_SET_SECURE_MODE 0`（已上机；`cp_int 0x1`，需要 zap） | `1ea366f4…` |
| `shim/out/shim-boot-metal91-scm-aperture.img` | SID1 恢复 + SCM `qcom_scm_kgsl_set_smmu_aperture(2)`（已上机；SCM 成功，A11 仍 0） | `f7e43faa…` |
| `shim/out/shim-boot-metal90-scm-aperture.img` | SCM + 误带 slot2 SID27（已上机；SID27 先 panic，SCM 未执行） | `c12cd0c9…` |
| `shim/out/shim-boot-metal89-sid27-slot2.img` | 放弃 SID1，SID 27 → slot2（已上机；slot2 也 data abort） | `3ee479f4…` |
| `shim/out/shim-boot-metal88-sid27-fixed.img` | 固定 slots 1/2 + SID 27 → slot3（已上机；slot3 安全，data abort） | `f141e5af…` |
| `shim/out/shim-boot-metal87-sid27.img` | SID 27 → slot3（已上机；A3 读 slot0 panic，未执行到 SID27） | `5e787bc0…` |
| `shim/out/shim-boot-metal86-smmu-read.img` | 只读 SMMU IDR/SMR（已上机；6 个 SMR，slot3 空，fault = SID 27+SSID） | `6e13c2b7…` |
| `shim/out/shim-boot-metal85-sid8.img` | SID **8** 写进 gslot1（已上机；同一处 SMR slot2 panic） | `5731a835…` |
| `shim/out/shim-boot-metal84-sid8-gslot1.img` | SID `0x1b08` 写进 gslot1（已上机；SMR 值非法，data abort） | `928ce576…` |
| `shim/out/shim-boot-metal83-sid8.img` | SID 0x1b08 复用 slot0（已上机；SMR0 安全，data abort） | `76044a61…` |
| `shim/out/shim-boot-metal82-sweep.img` | A2D 多假设扫描（已上机；安全变体全 0，addr mode panic） | `15e3d6be…` |
| `shim/out/shim-boot-metal81-sptprac-gpu.img` | GPU 窗口 SPTPRAC（已上机；读回 `0xffffffff`，窗口太小） | `a144f25c…` |
| `shim/out/shim-boot-metal80-sptprac.img` | SPTPRAC 读 GMU-core 窗口（已上机；0/0/0，窗口不对作废） | `30464670…` |
| `shim/out/shim-boot-metal79-ccu.img` | CCU cache offset=0 + CCU clean（已上机；clean 退休，A2D 仍 0/512） | `50dcda14…` |
| `shim/out/shim-boot-metal78-dispatch.img` | CP ROQ/mempool（已上机；a730 读回 `0xdeafbead`，证伪） | `b88f1b16…` |
| `shim/out/shim-boot-metal77-canon.img` | 头重建 + CCU WFI（已上机；CP 全程干净，A2D 仍 0/512） | `7c039475…` |
| `shim/out/shim-boot-metal76-state.img` | 状态前导（已上机；A11 死在第一包的坏 parity） | `bc7b3355…` |
| `shim/out/shim-boot-metal75-faithful.img` | 完整重放（已上机；`CP_OPCODE_ERROR` 前的 SP_UPDATE 停点） | `a932b104…` |
| `shim/out/shim-boot-metal74-stable.img` | 稳定配置 + HLSQ_FLUSH（已上机，`0005.txt`） | `f87a36ed…` |
| `shim/out/shim-boot-metal65-ccu.img` | 老的可启动基线（回退用） | `19d2914070…` |
