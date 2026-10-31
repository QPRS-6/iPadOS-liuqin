# iPadOS-liuqin 工程记录

最后更新：2026-10-28（触摸打通）。本文只写**观察到的事实**和**明确的推断**，两者分开标注。
每次里程碑后更新“交接”一节。GPU 部分的最新事实在 `docs/GPU_MILESTONE.md`。

## 1. 目标与当前状态

目标：在 Xiaomi Pad 6 Pro（`liuqin`，SM8475）上启动 XNU（xnu-8796，iOS 16 世代），
运行未修改的 iPadOS 16 用户态。派生自 ios-sweet（SM7150），参考
xiaomipad-6pro-mainline（liuqin 设备树）与 iPadOS-nabu（iPad 身份/触摸）。

状态一句话（2026-10-24）：**已上真机，启动到 iPadOS 锁屏界面**。内核 ~50 秒
`exec SpringBoard`；首启时 LaunchServices 需要建库（几十分钟），建好后 SpringBoard 起
来、CoreAnimation 软件渲染 + IOSurface + IOMFB 把界面送到屏幕。GPU 侧 Adreno 730 已经
冷启动 GMU（HFI 六步全确认）、OOB 拉起 GX、建立 GPU 自己的 SMMU context、启动 CP 并
执行任意 PM4（内核自检 + 用户态 ABI 自检都通过），但还没接进 UI。
**触摸已在真机上可用（2026-10-28，`NOTES.md` 第 220 步 / `docs/DMA_HANDOFF.md` 文末）**：
GENI SPI4 走 SE DMA，固件下载 201,410 B 回读逐字节相同、3 s 引导，
216 s 内 3,380 个接触帧、多指派发正常、0 次读失败。
已知好镜像 `shim/out/shim-boot-touch172-fwstatus.img`（sha256 `a2a848fa…`）。
按键仍无驱动；锁屏能否划开取决于 HID 派发与锁屏策略（见 `docs/HID_LAST_MILE.md`）。

## 2. 本机验证证据（2026-10-21，macOS 14.8.9 / x86_64）

| 检查 | 命令 | 结果 |
| --- | --- | --- |
| 补丁系列 | `tools/verify-patches.sh`（xnu-8796.141.3 + dxb `6639065` + 三个 staging 脚本） | 78/78 干净应用 |
| 主机工具 | `python3 tests/test_tools.py` | 全部通过 |
| shim 编译 | `LLD=<zig ld.lld> shim/build.sh` | `out/Image` 122 880 B，无 ABS64 重定位 |
| boot.img | `tools/mkbootimg.py` + `tools/unpack_bootv2.py` | v2 头、dtb_offset/大小正确、可回读 |
| 触摸 harness | `tools/touch-novatek && make && ./test_nt36523` | 通过（NT36523 协议，非 NT36532） |

**没有做过的**：任何 `fastboot`、任何真机日志。内核编译的进展见 §6
（2026-10-22 已在本机链出 `kernel.development.liuqin`）。

## 3. 硬件事实（来源：主线设备树）

来源都是公开代码，不是猜测：

- `arch/arm64/boot/dts/qcom/sm8450.dtsi`（torvalds/linux）
- `arch/arm64/boot/dts/qcom/sm8475-xiaomi-liuqin.dts`（yzddmr6/linux-sm8450-liuqin）
- `include/dt-bindings/...`、`pm8350*.dtsi`

| 项目 | 值 | 出处 |
| --- | --- | --- |
| SoC | SM8475，1×Cortex-X2 + 3×A710 + 4×A510，ARMv9（本移植按 v8.2 编译） | `compatible = "qcom,sm8475"` |
| GICD | 0x17100000，64 KiB | `intc: interrupt-controller@17100000` |
| GICR | 0x17180000，0x200000，stride 0x40000 | `redistributor-stride` |
| 定时器 | 19.2 MHz；PPI 13/14/11/10 → 虚拟定时器 INTID **27** | `timer { interrupts }` |

2026-10-22 拿到用户提供的 stock `boot.img` / `vendor_boot.img` / `dtbo.img`
（都在 gitignore 的 `backup/` 里）：

- `boot.img`（192 MiB，sha256 `00df87c1…`）：Android boot **v4**
  （`header_size` 0x630，`signature_size` 4096），内核是 PE 格式的通用 Linux
  （`MZ` @0x1000，`image_size` 0x2040000），ramdisk 在 0x206e000、LZ4 压缩的
  first-stage init（21 个 cpio 条目，无 dtb）。**整个镜像没有 FDT**。
- `vendor_boot.img`（96 MiB，sha256 `752b7597…`）：v4，dtb 段 6,443,739 B
  在 0x1687000，是 **14 个 SoC 基础 DTB 的拼接**（cape / cape-v2 / capep /
  diwali / ukee / waipio / waipiop），其中 cape 三个的 `/memory` reg 全是 0
  ——ABL 启动时才填真实内存。cmdline 里有
  `liuqin:12/OS3.0.7.0.VMYCNXM`。
- `dtbo.img`（24 MiB，sha256 `21376daf…`）：44 个 overlay，liuqin 是第 15 项
  （752,518 B，`model = "Liuqin based on Qualcomm Technologies, Inc SM8475"`，
  `compatible = qcom,cape-mtp / qcom,cape`），ABL 按 msm-id/board-id 把
  基础 DTB 和它合并后交给内核。

所以真机 DTB 只能由 ABL 在运行时给出完整版本：离线能拿到结构（GIC 0x17100000、
`armv8-timer` 19.2 MHz/PPI 11、UFS 0x1d84000 都对得上），但内存 bank 必须
上机看。shim 现在会在屏幕上打印 `MODEL` 和 `MEM0BASE/MEM0SIZE…`，第一次
`fastboot boot` 就能读到。


## 4. 移植决策

1. **补丁系列**：从 ios-sweet 的 81 个补丁中移出 3 个纯 sweet 硬件驱动
   （GT9896/PM6150/RPMh 资源 ID），保留 78 个，全部 `sweet→liuqin` 重命名。
   移出的文件在 `xnu/patches/reference/`，不编译，作为移植模板。
2. **机器配置**：`MACHINE_CONFIG=LIUQIN`，`-march=armv8.2-a+crypto`（ARMv9 核可执行），
   16K 页、GICv3、无 PAC（与 sweet 相同的非 PAC ABI）。GICR stride 改为 0x40000，
   虚拟定时器 INTID 改为 27。
3. **帧缓冲**：0xb8000000 / 1800×2880。早期 console（补丁 0010/0012）、内核
   fbcon、IOMobileFramebuffer、HID 坐标归一化都改成这个几何。
4. **显示逻辑尺寸**：直接呈现 1800×2880 @2x（900×1440 pt）。sweet 的
   “iPhone X 1125×2436 居中”逻辑保留代码路径，但常量改成面板原生尺寸。
5. **内存**：默认取高端 bank，`mkadt.py --dtb <stock dtb>` 从本机 DTB 自动读
   `/memory`、`/intc`、`/timer`、`/smp` 帧缓冲；也支持 `--ram-gib 12` 和
   `--dram-base/size`。
6. **身份**：iPad8,1 / D321AP（11" iPad Pro，A12X，无 Home 键，120 Hz），
   `chip-id=0x8027`，`display-scale=2`。`/product` 节点需从 iPad8,1 的 IPSW
   提取（`research/ipad-d321ap/d321ap.adt`，不入 git）。
7. **UFS root**：驱动按 GPT 分区名找 `userdata`（`ufs_find_partition`），
   没有硬编码 LBA，所以分区表变化不影响；`rd=ufsroot0 ufsroot=userdata`。
8. **输入**：暂不提供。HID 服务（补丁 0058）已编入，但没有 liuqin 的触摸/按键
   驱动，backboardd 收不到事件——这是下一步的主要工作。

## 5. 风险与未验证项（按重要性）

1. **内存图**：高端 bank 的基址/大小、以及是否有 carve-out，全部来自通用
   SM8450 布局，没有本机 DTB 核对。上机前必须 `mkadt.py --dtb`，否则可能踩到
   firmware 保留区。8 GB 默认值在 12 GB 机上不会越界（只用到前 6 GiB），
   但反向不成立。
2. **MPIDR**：ADT 里 `reg = core << 8` 沿用 sweet 的写法（sweet 上 8 核可启动）。
   SM8475 是三簇，secondary CPU 的 PSCI 目标 ID 未验证；先用 `cpus=1`。
3. **iPad 身份**：D321AP/0x8027 是按机型选的，没有在 liuqin 上验证过。
   备选是 nabu 用过的 iPad7,3（J207AP），只改 `mkadt.py` 三个常量。
4. **显示**：ABL 的 splash 一直扫描输出；XNU 只是往 0xb8000000 写。面板 DSC/
   双 DSI 初始化、背光（KTZ8866，I2C 0x11）都没接管。
5. **USB**：补丁里的地址是 sweet 的 0x0a600000 / 0x088e2000；liuqin 的 DWC3 在
   0x0a600000，但 PHY 是 eUSB2 repeater，初始化未验证。
6. **内核只在主机上编译过**：2026-10-22 用 Xcode 15.4 链出了二进制（§6），
   但没有在真机上跑过；编译器差异暴露过一个补丁错误（`LIUQIN_FB_LOGICAL_W`
   未定义），说明“能编译”不能替代“能启动”。

## 6. 本机编译（2026-10-22，已产出内核）

装上 Xcode 15.4（15F31d）后，`tools/build-local.sh` 在本机（macOS 14.8.9 /
x86_64，无 Homebrew、无 KDK、无 sudo）一次跑通：

- 979 个目标文件编译、链接成功（Xcode 的 `iig` 生成 DriverKit 派发表，
  56 个符号全部解析）；
- `kernel.development.liuqin`：16,578,448 B，
  sha256 `f7455e21ef8ba77f793dd4c829d63ed472a83dff239061086c3a3bfce361042d`，
  `MH_EXECUTE / ARM64 / PIE`，入口 `0xfffffe000748b488`，10 个段；
- 版本串 `Darwin Kernel Version 22.6.0: Wed Oct 21 16:08:35 UTC 2026; qprs:e6a7809`；
- 没有 `LC_DYLD_CHAINED_FIXUPS`：链接器没有生成 rebase，`mkpayload.py`
  按 `fixups: 0` 处理。

打包链也在本机跑通：`payload.bin` 16,589,552 B（magic `LIUQIN01`，10 段，
`virt_base 0xfffffe0006000000`），`shim-boot.img` 16,719,872 B（boot v2，
header_size 1660）。**boot.img 里放的是 985 B 的合成测试 DTB，不是真机 DTB，
不能拿去刷机。**

编译过程中发现并修掉的真问题：

1. 补丁 0060 在 `LiuqinHID.cpp` 里用了只定义在 `LiuqinFramebuffer.cpp` 的宏
   `LIUQIN_FB_LOGICAL_W`，只有真正编译才会报错；改成字面量 1800/2880 后
   78 个补丁重新验证通过。
2. `tools/mkbootimg.py` 的 v2 头只有 1620 B（`id` 写成 8 B、缺
   `recovery_dtbo_size/offset` 和 `header_size`），`unpack_bootv2.py` 回读时
   dtb 字段错位。已改成 AOSP 的 1660 B 布局，测试同步更新。

## 6.1 为什么必须是 Xcode 15

xnu-8796 链接需要 `iig` 生成的 DriverKit IORPC 派发表。Command Line Tools
不含 `iig`；Xcode 16 的 `iig` 生成 `IORPC.kernelContent`，与 xnu-8796 不兼容。
所以固定 Xcode 15.x（CI 的 `macos-14` runner 同样是 15）。

本机 Xcode 没跑过 `xcodebuild -runFirstLaunch`（要 sudo），所以
`xcodebuild` 起不来；`tools/build-local.sh` 因此不用 xcodebuild，直接用
clang 编 libdispatch 的 `firehose_buffer.c` 得到 `libfirehose_kernel.a`。

## 6.2 第一次真机启动（2026-10-22）

`fastboot boot` 了 `shim/out/shim-boot.img`，屏幕（红底，shim 自己的 console）依次出现：

```
SHIM liuqin milestone 4: xnu loader
INITRD lo=0xB702C000 hi=0xB7FFE2F0
MODEL Liuqin based on Qualcomm Technologies, Inc SM8475
```

说明：ABL 接受了这个镜像并把 16.6 MB 的 payload 放在 0xB702C000；ABL 选择并
合并了 liuqin 的设备树（model 串来自 dtbo 里的 liuqin overlay）；`fdt_changed`
解析正常。

随后屏幕变蓝，这是 **shim 自己的异常处理**（`exc_handler`，蓝底 + `PANIC
SYNC`），不是 XNU 的 panic。当时的构建里 STAGE 只有 `loader` 一个值，无法
区分是 DTB 扫描、段拷贝还是跳转后 XNU 早期代码触发，所以：

- 给 loader 加了分阶段标记：`ldr-dtb` / `ldr-payload` / `ldr-phys` /
  `ldr-copy` / `ldr-adt` / `ldr-args` / `xnu`，panic 屏的 `STAGE` 行会指出
  最后一个完成的阶段；
- 同时打印 `MEMBANKS n=`（ABL 传进来的 DTB 里 /memory 的数量，0 表示没填）。
  实测是 **4 段**，但蓝屏把红屏冲掉了，看不到 base/size；
- 为此加了诊断镜像 `shim/out/shim-diag.img`：payload cmdline 带
  `liuqin_diag=1`，打印完 banks 和 payload 头之后 `wfi` 挂起、不拷贝也不跳转，
  用来把 4 段 `/memory` 完整留在红屏上拍照。

设备是 **8 GB 版本**（用户确认）。诊断镜像实测 `/memory` 有 5 段（2/2 cells）：

| base | size |
| --- | --- |
| 0x0000000080000000 | 0x6A000000 |
| 0x00000000F1400000 | 0x0EC00000 |
| 0x0000000800000000 | 0x39700000 |
| 0x0000000840000000 | 0x140000000 |
| 0x0000000839D00000 | 0x01E00000 |

合计约 7.84 GB，正好是 8 GB 机器。**高 bank 在 0x8_40000000（33 GiB），
不是 0x100000000** —— 这解释了正常镜像为什么在拷贝第一笔写 `0x101004000`
时就 fault。现在 ADT/payload 按 `0x840000000 + 0x140000000`（5 GiB 连续段）
生成，内核落到 `0x841004000`，`/pram` 在 `0x853FF00000`。

诊断镜像（`liuqin_diag=1`）把 ABL 填好的 4 段内存完整打了出来：

```
MEM0BASE 0000000080000000  MEM0SIZE 000000006A000000
MEM1BASE 00000000F1400000  MEM1SIZE 000000000EC00000   (正好到 0x100000000)
MEM2BASE 0000000800000000  MEM2SIZE 0000000397000000
MEM3BASE 0000000840000000  MEM3SIZE 0000000140000000
```

前两段是可信的（0x80000000+1696 MiB、0xF1400000+236 MiB，末尾正好 4 GiB 边界）；
后两段的基址在 0x8_00000000/0x8_40000000（32 GiB 以上），不是普通 RAM 地址，
但它们和“高 4 GiB 之上的 DDR”有什么关系还没确定。把 bank 上限提到 8 之后，
`/memory` 实际有 **5 段**——第 5 段很关键（可能就是真正的高 bank，或者它的
base 不在 0x100000000）。原始 cell 的 RAW 输出那一版
有 bug（`fdt_memory_raw` 没有复位节点状态，后面把 `/sram@17D09400` 的 reg 当成
了 memory 的，`RAW4..7` 是栈残留），源码已修（复位 + 最多 8 段），但本机缺 ELF
链接器，当前诊断镜像还是旧 `Image` 重新封装的，下一版会带上修正。payload 仍按
`0x100000000 + 6 GiB − 1 MiB` 摆放内核。

正常镜像（`shim-boot.img`）实测：红屏打到 `ENTRY` 就蓝屏，`SEGS` 没出来，
说明死在拷贝循环的第一笔写入（目标 `0x101004000`）——高 bank 很可能不在
`0x100000000`，或者那一带是保留区。需要蓝屏上的 `FAR` 确认。

另外 `print_bank()` 的 `switch` 被 clang 编译成 **绝对地址跳转表**，又是同一个
坑（诊断镜像打印第一段就 fault）。`shim/build.sh` 已加 `-fno-jump-tables`，
并把 switch 改成 if 链。

另外，第一次和第二/三次上机的蓝屏其实是 shim 自己的 bug：`loader.c` 里用
`static const char *base_label[4]` 这种字符串指针表，链接基址 0 而加载地址任意，
第一次 `con_hex(base_label[i], …)` 就 fault（`elf2bin --reloc-check` 看不到，
因为链接器把地址直接烧进了镜像）。改成展开打印后 4 段正常输出。**shim 里不要
放指针表/带指针初值的全局数据。**

同一次启动还暴露了一个必炸的匹配错误：内核 `LiuqinCPU` 的 `IONameMatch`
仍是 sweet 的 `qcom,kryo470`，而 ADT 的 cpu 节点写的是 `qcom,kryo780`
（Kryo 780 = SM8475）。已改补丁 0018/0038 并重编内核（sha256 `50bce05b…`），
否则 CPU 驱动永远不会 attach。

下一次上机要抓的是蓝屏上的 `SLOT / STAGE / ESR / ELR / FAR` 五行：ELR 落在
shim 的物理地址段说明还是 loader 的问题，落在 `0xfffffe0007…` 说明已经进了
XNU 的早期汇编。

## 6.3 XNU 第一次跑起来（2026-10-22）

换成真实内存段（`0x840000000`）之后，XNU 真的启动了：屏幕被 fbcon 接管，
kprintf 输出可见（`LIUQIN EARLY 20: kprintf startup complete`），然后 panic：

```
panic(cpu 0 caller 0xfffffe0007ad1dac): Undefined kernel instruction:
pc=0xfffffe0007491348 Instr=d51be228
```

`d51be228` 用 clang 汇编对照确认是 `msr CNTP_CTL_EL0, x8`；pc 落在
`_fiq_context_init+0x14`。源码里这段是：

```c
/* osfmk/arm64/machine_routines.c: _enable_virtual_timer() */
__builtin_arm_wsr64("CNTV_CTL_EL0", cntvctl);
/* disable the physical timer as a precaution ... */
__builtin_arm_wsr64("CNTP_CTL_EL0", CNTP_CTL_EL0_IMASKED);
```

根因：固件没有置 `CNTHCTL_EL2.EL1PCEN`，EL1 访问物理定时器是未定义指令。
这个移植只用虚拟定时器（INTID 27），那段“保险”删除即可。

- 新补丁 `xnu/patches/0079-liuqin-skip-physical-timer.patch`（79/79 干净应用）；
- 内核重编，sha256 `cc1be243…`，确认镜像里不再有那条指令；
- 仍在等待下一次上机验证。

## 6.4 XNU 走到 BSD init（2026-10-22）

把定时器补丁带上后，XNU 一路初始化：timebase、kprintf、zone allocator、
IOKit、网络协议栈（`gif0`/`stf0`/IPsec）、UFS 驱动，最后停在根文件系统：

```
LIUQIN UFS: command 0x00 failed: ocs 0x0 upiu 0x21 response 0x1 status 0x2
panic(cpu 0 caller ...): IOFindBSDRoot: LIUQIN UFS root ufsroot0 unavailable: 5
```

panic 报告里能看到 `Device: D321`、`Hardware Model: iPad8,1`、
`Kernel text base: 0xfffffe0007004000`、完整的 boot-args、zone 表、
backtrace 和 stackshot —— 身份、boot_args、符号化都对了。

`command 0x00` 是 TEST UNIT READY；设备回了 CHECK CONDITION
（response 0x1 = target failure，status 0x2），不是超时。驱动原来只重试 3 次、
没有 sense data，所以看不到原因。补丁 **0080** 加了：

- sense key / ASC / ASCQ 打印（从 response UPIU 的 offset 32 起）；
- TUR 重试 20 次、每次间隔 50 ms；
- LUN 0 不就绪时探测 LUN 1..3 并打印结果。

即使 UFS 修好，userdata 上目前也没有 iPadOS root，所以下一次仍会停在
`IOFindBSDRoot`——真正的下一个里程碑是用 iPad8,1 的 IPSW 构建 HFS+ root
并写入 userdata。

## 7. 下一步（具体动作）

1. 用 `cdcd6694` 内核的镜像再 `fastboot boot` 一次，读 UFS 的 sense data，
   判断 CHECK CONDITION 是 unit attention、LUN 不对还是设备状态问题。
2. 准备 iPad8,1 的 iPadOS 16.x IPSW，构建 HFS+ root 并写入 userdata
   （工具见 `tools/mkiosroot.sh`，需要 Linux 或能跑 sudo 的主机）。
2. 看 `MEM0BASE/MEM0SIZE`，确认真实内存 bank 和 §3 的假设是否一致；不一致就
   按实测 bank 重打 ADT（`--dram-base/--dram-size`）。
3. 过了早期启动后读持久 log（`liuqin-log-pull.sh`）。第一个真正的里程碑不是
   UI，而是看到 XNU 的 `LIUQIN` 早期 console 和 UFS 分区打印。
3. ~~上 PM8350 按键驱动（最小可用：电源键 + 音量），再上 NT36532。~~
   NT36532 已于 2026-10-28 打通（第 220 步）；剩下 PM8350 按键与 HID 派发收尾。
4. 稳定后再试 `cpus=8`，确认三簇 MPIDR 与 PSCI。
5. 内核进真机后，再补 CTF/dSYM（本机脚本目前 `DO_CTFMERGE=0 BUILD_DSYM=0`）。

## 8. 交接

> **换会话续做请先读 `docs/HANDOFF_2026-10-31_GPU.md`**（构建/出镜像命令、现状、踩过的坑、日志怎么拿、下一步）。
> boot cmdline 已存 `tools/boot-cmdline-gpu.txt`（约 607 B 上限）。

- **2026-10-31 GPU 第 370 步（当前）：决定换成 HakiOS 链路**。目标：Android fastboot → HakiOS UEFI（Mu-Silicium + HakiOS 改动，或 HakiOS U-Boot）→ xnuloader → XNU（port 的 XNU/ADT/fwarea）→ launchd。迁移：
  - H0：clone HakiOS，`tools/hakios-device.py new` 建 `devices/liuqin`（SM8475 SoC 层、configs、firmware、UEFI platform、status），参考 redwood；
  - H1：6 Pro 最小 HakiOS UEFI（`hakios-uefi` + SM8475/liuqin platform；fastboot boot；GOP/串口/UFS）；
  - H2：xnuloader → port 的 XNU（Mach-O + ADT + fwarea + cmdline `sheng-gpu-bringup=0`）；
  - H3：HakiOS UEFI 里做 GPU secure init（QcomPkg/SCM：PAS/zap/trust/aperture）→ A11；
  - H4：客户端/Metal/UI。
  - 注意：HakiOS 只支持 redwood（SM7325/a6xx）；6 Pro 的 a730/secure 要自己 port；HakiOS 的 XNU/driver 可参考，但 port 的 XNU 更靠前；UEFI 的 QSEE SCM 可能同样 -4。
  - 交接提示词已更新到 `docs/HANDOFF_PROMPT_HAKIOS.md`（HakiOS 链路版）。
  - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 369 步：交接提示词已写进 `docs/HANDOFF_PROMPT_HAKIOS.md`**（贴给下一个会话的第一条消息；第 370 步已改成 HakiOS 链路版）。内容：已确定事实（secure SID 27 / 所有非安全出口）、稳定回退、root、工作规则、下一步 survey。下一条会话从该文件开始。
  - 相关：`docs/HANDOFF_2026-10-31_GPU.md` §5 已把 HakiOS 链路列为主线、kexec 作 fallback。
  - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 368 步：决定走 UEFI 路线**。目标链：ABL → BootShim → UEFI(SM8475/liuqin) → XNU；UEFI 的 QcomPkg/SCM 做 GPU secure init（PAS/zap/trust/aperture），XNU 用 `sheng-gpu-bringup=0`。阶段：
  - Phase A：6 Pro 最小 UEFI（BootShim + Mu-Silicium/uefi-sheng 的 SM8475/liuqin 平台，fastboot boot，GOP/串口，UFS）；
  - Phase B：UEFI → port shim/payload/ADT → XNU；
  - Phase C：UEFI 里做 GPU secure init → A11 填（成败关键）；
  - Phase D：客户端/Metal/UI。
  - 参考：Mu-Silicium、`uefi-sheng`（6S Pro）、`hakios-uefi`/`hakios-sm7325`、`xnuloader`；port 的 shim/mkpayload。
  - 风险：6 Pro 的 UEFI 平台是月级工程；QSEE 的 `restore_sec_cfg` 在 XNU 是 -4，UEFI 侧可能同样拒绝。

- **2026-10-31 GPU 第 367 步：kexec 链是“Linux 先、XNU 后”**（ABL → native Linux → kexec → XNU）。Linux 负责 GPU secure init，XNU 用 `sheng-gpu-bringup=0` 接管；可以最小化 initramfs + 开机自动 kexec，用户感知是“几秒 Linux 后进 XNU”；每次重启都会经过 Linux。要“直接 XNU 启动”只能走 UEFI/QSEE 平台初始化（即第 368 步的路线）。

- **2026-10-31 GPU 第 366 步：机主确认 6 Pro 的原生 Linux（Ubuntu）能 boot、几乎全部硬件可用**。它可以当 kexec host：`CONFIG_KEXEC` 自己开、无 SELinux、`kexec-tools` 现成、freedreno 做 GPU secure init。下一步在 native Linux 上确认 `CONFIG_KEXEC`/`kexec`/lockdown，再用 metal79 payload + shim Image + `liuqin.adt` + cmdline(`sheng-gpu-bringup=0`) kexec 到 XNU。

- **2026-10-31 GPU 第 365 步：主线内核作为 kexec host 的评估。6 Pro 已有**原生 Linux（Ubuntu）移植**（Coolapk 帖子），SM8475 主线在推进（clock/audio patch；`Xiaomi-SM8475/android_device_xiaomi_liuqin` 是 Android AOSPA 树，不是主线）。**主线不能 boot Android 17**，但可以 boot 原生 Linux，再从 Linux **kexec** 到 XNU；**freedreno 的 zap 会做 GPU secure init**，kexec 后 XNU 用 `sheng-gpu-bringup=0` 保留。关键看原生 Linux 的 **GPU/display/touch/UFS** 完成度；如果够用，这是比 custom Android 更干净的 host**
  - 来源：Coolapk 6 Pro 原生 Linux 帖、`pipa-linux-wiki` 主线页、SM8475 clock/audio 主线 patch、`Xiaomi-SM8475/android_device_xiaomi_liuqin`。
  - 主线 host 的优点：`CONFIG_KEXEC` 自选、无 SELinux、`kexec-tools` 现成、freedreno 开源 GPU 初始化。
  - 缺点：不能跑 Android 17 userspace；原生 Linux 的 GPU/display/touch 可能不完整；XNU 侧仍用自己的 NativeAdreno/触摸驱动。
  - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 364 步：HakiOS 链路评估。HakiOS（`oosexgang/hakios-xnu-kernel`）是 **Android fastboot → HakiOS UEFI（Mu-Silicium 或 U-Boot）→ xnuloader → XNU + HakiOS 驱动 → launchd**，走 `fastboot boot`，系统在 RAM 里跑，**不是 Android/kexec**；支持设备只有 **POCO X5 Pro 5G（redwood, SM7325, Adreno 642L）**，新设备要按 `devices/<name>` 全套 port。**6 Pro(liuqin, SM8475, Adreno 730) 不受支持**；即使 port，`fastboot boot` 链也不跑 stock Android 内核的 kgsl，6 Pro 的 GPU secure 状态仍在；HakiOS 的 GL 在 a6xx（642L）上工作，a730 是 a7xx，secure SID 27 的问题不同。**结论同 UniXNU：HakiOS 链路在 redwood 能用，在 6 Pro 不能解决 GPU；stock Android 正常 boot + kexec 仍是唯一保留 kgsl GPU 初始化的链路**
  - 来源：`/tmp/hackios-xnu`（README、docs/concepts/architecture.md、docs/getting-started/phone.md）。
  - HakiOS 的 XNU patch/driver 对 port 有参考价值（port 已借用 exec_inject 等），但 boot chain 不是 6 Pro GPU 的解法。
  - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 363 步：UniXNU 链路评估。UniXNU 是 **ABL → BootShim(gzip) → UEFI → DarwinBoot → XNU**，走 `fastboot boot`，**不是 Android/kexec**；官方设备只有 **6S Pro(sheng)/Adreno740**，README 说 740 GPU 已工作。**6 Pro(liuqin) 是独立 fork（本 port 的 direct shim）**；即使把 UEFI 移植到 6 Pro，`fastboot boot` 链也**不跑 stock Android 内核的 kgsl**，6 Pro 的 GPU secure 状态仍在；6S Pro 能工作是它的**平台/boot chain 非安全**，不是链路本身。结论：**UniXNU 链路在 6S Pro 能用；在 6 Pro 不能解决 GPU**，仍需 stock Android 正常 boot + kexec（保留 kgsl 的 GPU secure init），或 UEFI/QSEE 里做 GPU 初始化**
  - 来源：`~/liuqin-root/refs/UniXNU/docs/concepts/boot-chain.md`、`docs/guide/overview.md`、`docs/guide/boot.md`、README。
  - 6 Pro 的 `fastboot boot` 与 6S Pro 的 `fastboot boot` 都可能绕过 stock 内核的 GPU secure init；6S Pro 的 QSEE/ABL/UEFI 恰好把它留成非安全，6 Pro 没有。
  - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 362 步：custom kernel 的 `/proc/config.gz` 已确认：`CONFIG_IKCONFIG=y`、`CONFIG_IKCONFIG_PROC=y`、**`CONFIG_KEXEC` 未开**、`CONFIG_KEXEC_FILE` 未开；没有 `/proc/sys/kernel/kexec_load_disabled`；running kernel `5.10.237-Fuutao-Qn_miao-source-gc28e50058`。**kexec 在 custom Android 17 上不可用，BPF 不是问题**。可走路：① 请 custom 作者开 `CONFIG_KEXEC=y`（单 config，不需开源）；② **stock Xiaomi 5.10.81 `liuqin-t-oss` + stock Android（第二 slot）**；③ `a12-510+BPF` 重编不可行（vendor modules/vermagic/ABI/custom patches 缺）**
  - kexec 只需 `CONFIG_KEXEC`（+ 自动选的 `CONFIG_KEXEC_CORE`）；不需要 BPF/KEXEC_FILE。
  - a12-510+BPF 只是 Android 17 userspace 的 5.10 基线，不是 kexec 的替代；重编还需要 liuqin DTS、vendor modules、config、custom 补丁，且模块 vermagic 必须匹配。
  - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 361 步：kexec 的 host 内核问题。`boot_magisk` 是**闭源 custom 5.10.237（Fuutao/Qn_miao）**，拿不到源码；用户 Android 17。**kexec 本身只需要 `CONFIG_KEXEC`，不需要 BPF**；`a12-510 + BPF` 是 Android 17 userspace 对 5.10 内核的要求，不是 kexec 的要求。若 custom kernel 没开 KEXEC：① 请 custom 内核作者开 `CONFIG_KEXEC`/给源码；② 回 **stock Xiaomi 5.10.81 `liuqin-t-oss` + stock Android（第二 slot）**，源码可得，编 KEXEC；③ 从 `a12-510 + BPF` 重编需要设备 DTS、vendor modules、config，且 vendor modules 的 vermagic 必须匹配，不是“补 BPF”能解决**
  - 事实：`boot_magisk` 的 kernel 字符串 `5.10.237-Fuutao-Qn_miao`、clang 22；Image 无 `IKCFG_ST`；config 只在 Android `/proc/config.gz`。
  - kexec 的唯一内核要求：`CONFIG_KEXEC`（`kexec_load` 系统调用 + `machine_kexec`）；BPF/LSM 与它无关。
  - **待确认**：custom kernel 的 `/proc/config.gz` 里 `CONFIG_KEXEC`；stock Xiaomi ROM/boot/vendor_boot 是否可用；设备是否 A/B（第二 slot 跑 stock）。
  - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 360 步：kexec host 准备。解析 `boot_magisk.img`：Android boot v4，kernel 42,201,364 B、ramdisk 1,221,779 B、page 4096；内核是 **`5.10.237-Fuutao-Qn_miao-gc5e20589c`**（custom，clang 22），**不是 stock Xiaomi 5.10.81**；config 未内嵌（Image 里没有 `IKCFG_ST`）。所以 kexec 的 host 内核要先确认 `CONFIG_KEXEC`：如果 custom kernel 已开，只做 kexec loader + XNU shim；没开则要 custom 内核的源码/`/proc/config.gz`，或回 stock Xiaomi（`liuqin-t-oss`）+ stock vendor_boot 重新编**
  - `boot_magisk.img`（`7367b8cd8ab2397af4b23747d92ce61a2688ad2d7efcc45e636d2bd6a51526ed`）：
    - v4 header：kernel 42,201,364、ramdisk 1,221,779、page 4096、cmdline 空；
    - kernel 字符串：`Linux version 5.10.237-Fuutao-Qn_miao-gc5e20589c … clang 22.0.0`；
    - `stock_Image`/`magisk_ramdisk` 已解到 `liuqin-kexec/`；`file` 确认 ARM64 Image；
    - 无 `IKCFG_ST` → 配置不在 Image 里，需 Android 里 `zcat /proc/config.gz`。
  - **XNU kexec 目标**：用 **metal79 的 payload**（没有 zap/restore，避免 XNU 再次 reset GPU）+ 当前 shim `Image` + `liuqin.adt`；cmdline 用 `boot-cmdline-gpu.txt` 但把 `sheng-gpu-bringup=1` 改成 **0**，保留 stock Android 的 trust 0/非安全 SID。
  - **待确认**：host Android 的 `CONFIG_KEXEC`/`CONFIG_KEXEC_FILE`、`/proc/sys/kernel/kexec_load_disabled`、custom ROM/内核源码名；stock ROM/stock vendor_boot 是否可用。
  - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 359 步：路径 B 评估——stock kgsl 剩下的 SCM 只有 **secure pagetable**（`qcom_scm_iommu_secure_ptbl_init`/`secure_map`/`secure_unmap`）。它能建 **secure GPU context**，但只映射 **secure buffer**；turnip/Metal 用的是**非安全 buffer**，引擎的 secure SID 27 写到 secure CB 里，客户端/A11 仍看不到，UI 仍 0。**trust/secure-mode 不在 stock 内核的 SCM 列表里**：`RBBM_SECVID_TRUST_CNTL=0` 是 bootloader/QSEE 在启动时设的，stock 内核能写是因为 bootloader 留了权限；port 的 boot 没留。结论：**路径 B 不能修正常 GPU，只能做 secure-content 诊断**；唯一可行仍是 kexec/secure-world boot-chain**
  - stock `kgsl_iommu.c` 的 SCM 清单：`qcom_scm_kgsl_set_smmu_aperture`（已试，成功但无效）、`qcom_scm_iommu_secure_ptbl_init`/`secure_map`/`secure_unmap`（secure pagetable）、`qcom_scm_restore_sec_cfg`（已试，-4）；没有 trust/secure-mode 的 SCM。
  - 路径 B 若实现，能验证“引擎在 secure 模式下能写 secure buffer”，但不产生可用的 UI/Metal。
  - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 358 步：metal98 的 `qcom_scm_restore_sec_cfg(PAS_GPU,0)` 返回 **`0xfffffffffffffffc`（-4，QSEE 拒绝）**；secure SMR slot3..5 仍全空，A0/A11 仍 0。**内核侧所有路径到此为止**：restore 也救不了**
  - **metal98 事实**（`98.txt` 1,208,243 B `f2416f2995c31c376a7d39245ee7fd0f45affe2e9f22804b0552c9c81a5d133f`）：
    - `LIUQIN ADRENO: 21 restore_sec_cfg -> 0xfffffffffffffffc`；
    - `post-zap smr 3..7 0x00000000 s2cr 0x00020000`（仍空）；
    - A0/A11 `result=0 filled 0/512`；客户端仍 `sid=0x1b08`。
  - **结论**：QSEE 不接受这个 restore（GPU 的 PAS secure-config restore 不支持/参数不对）。非安全内核无法配置 GPU secure SMR/trust；所有内核出口（SMR 槽、aperture、zap、secure-mode 包、trust 写、restore_sec_cfg）都试过。
  - **剩下的唯一路线**：**kexec stock Android**（boot stock 让 ABL/QSEE 初始化 GPU secure 状态 → 不重启 kexec XNU shim；XNU 用 `sheng-gpu-bringup=0` 避免重置 GPU/trust）。QSEE secure-map 只适合 secure buffer，非安全客户端用不上。
  - 产物：见 metal98（`shim/out/shim-boot-metal98-restore-sec.img`，`886e4637…`）；**root 不变**。

- **2026-10-31 GPU 第 357 步：metal97 的 trust 写在 zap 后仍 data abort，内核 trust 出口关闭。但 stock 的 PAS 驱动在 `auth_and_reset` 之后还会调 **`qcom_scm_restore_sec_cfg(PAS_GPU, 0)`**（SCM `SVC_MP 0x0c` / cmd **0x02**，fn `0x42000c02`），port 的 zap loader 漏了这一步。metal98 = 在 zap loader 的 PAS auth 之后补 `restore_sec_cfg`，再启动 CP/A0/A11**
  - **metal97 事实**（照片；PC `0xfffffe0007abc1f8`、LR `0xfffffe0007abc1c8`、ESR `0x96000010`、FAR `0xfffffe5a1a508000`）：
    - `atos`：PC = `NativeAdrenoService.cpp:653`（`gpu_write32(&window,0x0f400,0)`）；
    - zap 全部成功，但 trust 写仍 data abort → 非安全世界写不了 trust。
  - **stock 事实**：
    - `qcom_scm_restore_sec_cfg(device_id, spare)`：`.svc=SCM_SVC_MP(0x0c)`、`.cmd=QCOM_SCM_MP_RESTORE_SEC_CFG(0x02)`、args[0]=device_id、args[1]=spare，arginfo=2；
    - `qcom_pas` 在 `PAS auth_and_reset` 之后调用它，恢复子系统的 secure 配置；port 的 `liuqin_adreno_zap_load` 只做了 auth，没有 restore。
  - **metal98 做法**：`liuqin_adreno_zap_load` 在 auth 成功后发 `restore_sec_cfg(PAS_GPU=13, 0)`；日志 `RESTORE_SEC_CFG OK` 或 `restore_sec_cfg -> …`；不再写 trust（secvid 默认 0）。
  - 判读：
    - `RESTORE_SEC_CFG OK` 且 A11 `512/512`/客户端有数据 → 这就是缺的 secure 配置；
    - `RESTORE_SEC_CFG OK` 但 A11 仍 0 → restore 不配引擎 SID；
    - `restore_sec_cfg -> 非 0` → QSEE 拒绝/服务不存在；
    - 开机挂/panic → 回 metal96/79。
  - 产物：
    - 内核 `kernel.development.liuqin` `27a2ce1b0c161f9f985166e035440b8756ac822eb8949b3d88bb00bb59095b4e`；
    - payload `shim/out/payload-metal98-restore-sec.bin` `c27c89c36781071060aec75e67c2e7947715a794f60cc2f886c2c2bc9db66582`；
    - 引导镜像 `shim/out/shim-boot-metal98-restore-sec.img` 32,071,680 B `886e4637ef9c7f3935d70aba3fb89e3e3875b09b535e992e6ad817f8cc3f0967`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 356 步：metal97 的 **zap 之后**写 `RBBM_SECVID_TRUST_CNTL (0xf400)=0` **仍然 data abort**（PC = `NativeAdrenoService.cpp:653`，FAR = GPU window + 0xf400*4，ESR 0x96000010）。所有非安全出口（SMR 槽、aperture SCM、zap、trust 写）都试完：secure SMR 空、trust 寄存器非安全世界写不了。GPU 引擎的 secure 配置只能由 secure world（QSEE/bootloader）完成；下一步是 kexec stock Android 或 QSEE SCM，不再出内核 SID/trust 图**
  - **metal97 事实**（照片；PC `0xfffffe0007abc1f8`、LR `0xfffffe0007abc1c8`、ESR `0x96000010`、FAR `0xfffffe5a1a508000`）：
    - `atos`：PC = `NativeAdrenoService.cpp:653`（`gpu_write32(&window,0x0f400,0)`），即 trust 写本身；
    - 前面 zap 全部成功（`PAS init/mem/auth -> 0`、`TRUSTZONE RUNS THE ZAP SHADER`、`zap load before CP -> 1`）；
    - 写 0xf400 仍 data abort → **secure world 即使 zap 之后也不让非安全世界写 trust**。
  - **结论（硬边界）**：
    - secure SMR slot3..5 是空的（metal96）；
    - trust 寄存器非安全世界写不了（metal49/97）；
    - `CP_SET_SECURE_MODE` 被 CP 以 opcode error 拒绝（metal92/94）；
    - SCM aperture 成功但无效（metal91）；
    - zap 成功但不建 secure SMR（metal96）。
    - 引擎 SID 27 只能由 **QSEE/secure world** 映射或由它切 secure mode；内核这边没有可写出口。
  - **下一步**：
    1. **kexec stock Android**：boot stock Android（ABL/QSEE 做 GPU secure init）→ 在 Android 里确认 secure SMR 被配置 → **不重启**，kexec XNU shim；SMMU/GPU secure 状态保留，XNU 的引擎用非安全 SID。
    2. **或找 QSEE 的 SMMU secure-map SCM**（stock `kgsl_iommu.c` 的 secure pagetable / `qcom_scm_iommu_secure_*`），在 XNU 里调用。
  - 产物：见 metal97（`shim/out/shim-boot-metal97-trust-after-zap.img`，`23ccdd11…`）；**root 不变**。

- **2026-10-31 GPU 第 355 步：metal96 只读结果——zap 之后 slot3..5 仍 `SMR 0 / S2CR 0x00020000`，**secure SMR 完全没配**；引擎 SID 27 无映射。zap 只认证/运行，不配 secure SMR。metal97 = 试 stock 的另一条出口：zap 之后直接写 `RBBM_SECVID_TRUST_CNTL (0xf400) = 0`**
  - **metal96 事实**（`96.txt` 1,232,963 B `8b087cfc1bb626c100ad25419e4091f064eb07bad75b72f0b6f50d0c20d8c519`）：
    - `zap load before CP -> 1`；
    - `post-zap smr 3..7 0x00000000 s2cr 0x00020000`（全空）；
    - A0/A11 退休、`filled 0/512`；客户端仍 `sid=0x1b08`。
  - **结论**：zap 不会配置 secure SMR；引擎 SID 27 仍然没有映射。要么真正切 secure mode（trust 寄存器），要么在 secure world 里建 SMR。
  - **metal97 做法**：在 `startCp` 的 `RBBM_SECVID_TRUST_CNTL` 处把默认改成 **1**，即 zap 之后写 `0xf400=0` 并读回。之前（zap 之前）这个写 data abort；现在试 zap 之后是否允许。
  - 判读：
    - 写成功、读回 0，A11 `512/512`/客户端有数据 → trust 寄存器是 secure mode 开关，根因找到了；
    - 写成功、读回 0，A11 仍 0 → trust 不是全部；
    - 写 data abort/panic → secure world 仍禁止；回 metal96/95，下一步只能在 secure world 建 SMR。
  - 产物：
    - 内核 `kernel.development.liuqin` `d38bde60e601c86d6e452da011fd86539bd3f2be13d8a5708f1c5f2d29dee0b1`；
    - payload `shim/out/payload-metal97-trust-after-zap.bin` `a91f958ddd6e5ab704bb1bf223e7e4d5e557d3946f63f10c25574fb00c98b2a9`；
    - 引导镜像 `shim/out/shim-boot-metal97-trust-after-zap.img` 32,071,680 B `23ccdd11c33b424aa6b70879b8fdbaa409bc4cf733e265a164b933521c07a82c`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 354 步：metal95 = zap + CP 都健康（A0/A11 退休、无 `cp_int`），但引擎仍 SID 27、A11 0——**zap 没改变 secure 模式**。metal96 = 只读 post-zap 的 SMMU secure SMR（slot3..5），看 QSEE 有没有配置、指向哪个 CB**
  - **metal95 事实**（`95.txt` 1,202,950 B `3beeabc21be02b589df66ba7ed4ac2d07e4728ad125275a8d2fd18d107c4a9ff`）：
    - `zap load before CP -> 1`；`CP started fence=2`；`cp_int 0x0`；
    - A0 `result=0 filled 0/512`；A11 `result=0 filled 0/512`；
    - 客户端仍 `smmu fault sid=0x1b08 far=0xd5fe77b7`、`sid=0x1b00 far=0x28000`；
    - 系统继续启动。
  - **结论**：zap/PAS 运行、CP 健康，但引擎事务仍是 secure SID 27。要么 secure SMR 仍空，要么指向安全 CB；`CP_SET_SECURE_MODE` 是非法包（opcode error）。
  - **metal96 做法**：在 `startCp` 里、A0/A11 之前只读打印 `post-zap smr 3..7` 的 SMR/S2CR（`g_smmu_route`）；不写任何寄存器。
  - 判读：
    - slot3..5 仍 `SMR 0 / type 2` → secure SMR 从未配置；zap 没做这件事，下一步查 zap 的 secure 配置入口；
    - slot3..5 有 SID/ CB → 看 CB 是不是 2；是安全 CB 的话，客户端非安全页表用不上，仍需切 secure mode；
    - A11 仍 0 但无 fault → 再查 A2D state。
  - 产物：
    - 内核 `kernel.development.liuqin` `06a70a0116e80d504ac474496bdb2e7311a683238066b1f01c55874238e96eca`；
    - payload `shim/out/payload-metal96-post-zap-smr.bin` `7ce2a7af77c4445401f8d8d08599c05dd210ff54e763e82f4bd78fe8ec4bc867`；
    - 引导镜像 `shim/out/shim-boot-metal96-post-zap-smr.img` 32,071,680 B `d8caa8082309a7fa643a8c52a7f4062402263b89acd6dc2ccb0548448158343e`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 353 步：metal94 的 zap 在 CP 前成功、CP 也正常启动（`CP started fence=2`）；但 `CP_SET_SECURE_MODE` 仍在 `rptr 38` 处 `cp_int 0x1`，把 CP 打死。推断 **zap 的 PAS auth 已经完成 secure mode 切换**，这条包此时非法。metal95 = 只保留 zap、删掉 secure-mode 包，让 A0/A11 跑**
  - **metal94 事实**（`94.txt` 565,939 B `edbde55afbcbd58fd189127e0791b906d02b8e72ead42eb19403479f1edd439a`）：
    - `zap load before CP -> 1`；`CP started fence=2`；
    - `fence timeout 4 … cp rptr 38 wptr 46`；
    - `CP_SET_SECURE_MODE 0 … result=1`；`cp_int 0x1`；A0/A11 skipped；
    - 系统继续启动。
  - **结论**：zap + CP 启动都正常；**secure-mode 包**是 CP 停在 38 的直接原因。zap 已经运行过，包再来一次就是非法 opcode。
  - **metal95 做法**：保留 `liuqin_adreno_zap_load()` 在 `startCp` 之前；删除 `CP_SET_SECURE_MODE` 包；A0/A11 直接跑。
  - 判读：
    - `zap load before CP -> 1` + A0/A11 正常 + A11 `512/512` → zap 已经离开 secure mode，引擎用 SID 0/1；
    - A11 仍 0、无 `cp_int` → zap 没完成切换；再查 secure 状态/包；
    - CP 启动失败/`fence timeout` → zap 的 reset 影响后续 CP init；把 zap 再提前到 GMU 之前；
    - 开机挂/panic → 回 metal79。
  - 产物：
    - 内核 `kernel.development.liuqin` `99770d9cd06f5d3361b4aa2cefb2552a9017eb04e8f290718eae3d94f804edb9`；
    - payload `shim/out/payload-metal95-zap-only.bin` `465897daedd2879852e983120ed0a122e9dcd3e11d42229fd606f5b25f12a67a`；
    - 引导镜像 `shim/out/shim-boot-metal95-zap-only.img` 32,071,680 B `a0573a5cfe2c261308296f63e8345a4a7fd603d752e1b7d0289ec1e8fdb4a20b`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 352 步：metal93 的 zap **成功**（`zap load -> 1`、`TRUSTZONE RUNS THE ZAP SHADER`），但 **PAS auth/reset 把已经启动的 CP 复位了**（`fence timeout 4`、`rptr 38` 不动），`CP_SET_SECURE_MODE` 没有 CP 执行。metal94 = 把 zap 移到 **`startCp` 之前**，让 CP 在 zap 之后启动**
  - **metal93 事实**（`93.txt` 522,439 B `f0981f440ab7e6a2b55328725df529f7ba26af6fe31728f102e9c14bef4ea5e6`）：
    - `LIUQIN ADRENO: 21 zap: 3 segments, load range 0x1000..0x2000`
    - `LIUQIN ADRENO: 21 TRUSTZONE RUNS THE ZAP SHADER`
    - `NativeAdreno: zap load -> 1`
    - `fence timeout 4 … cp rptr 38 wptr 46`（CP 在 zap 后停了）
    - `CP_SET_SECURE_MODE 0 … result=1`；`cp_int 0x1`；A0/A11 skipped
    - 系统继续启动；有 MacinTalkAUSP/KonaSynthesizer 用户态 crash（和 GPU 无直接关系）
  - **结论**：zap/PAS 本身成功；问题是**顺序**——PAS auth/reset 复位 GPU/CP，而 zap 放在 CP 启动之后，把 CP 打死了。
  - **metal94 做法**：把 `liuqin_adreno_zap_load()` 从 `startCp` 内部移到 `startCp` 调用**之前**；CP 在 zap 之后启动；secure-mode 包在 CP 启动后发。
  - 判读：
    - `zap load before CP -> 1`，CP 正常启动，`CP_SET_SECURE_MODE result=0`，A11 `512/512` → secure mode 根因，引擎离开 secure mode；
    - `zap load before CP -> 0` → PAS/zap 失败；看 `LIUQIN ADRENO: 21 stop`；
    - CP 仍启动失败/`fence timeout` → zap 的 reset 影响了后续 CP init；需要把 zap 再提前到 GPU power 之后、GMU 之前；
    - `CP_SET_SECURE_MODE` 仍 result=1 → zap 认证了但包仍被拒；查 opcode/secure 状态。
  - 产物：
    - 内核 `kernel.development.liuqin` `3d142b72e3fb25f9394fb9cf200d706b3907f55a661eb8fd9d398a0bc50655f9`；
    - payload `shim/out/payload-metal94-zap-before-cp.bin` `9af682e167597e0f4f54deb082d88e178a9bb1e28167f26a3248e20f4cddbc02`；
    - 引导镜像 `shim/out/shim-boot-metal94-zap-before-cp.img` 32,071,680 B `418dc60adeb621857cb5430391b3d0cf198f74fbebda9aeae04fefb9db6cd6c0`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 351 步：metal92 的 `CP_SET_SECURE_MODE` 单独发被 CP 以 `cp_int 0x1`（`CP_OPCODE_ERROR`）拒绝，A0/A11 停住——证实它必须先有 **zap**。metal93 = 调 port 里已有的 `liuqin_adreno_zap_load()`（PAS init/mem/auth，a730_zap）再发 `CP_SET_SECURE_MODE 0`**
  - **metal92 事实**（`92.txt` 560,056 B `05270efb55770b8ff05923997d191922b3e96dbc3b967b44624d1d88683d29c1`）：
    - `SCM SMMU aperture cb2 -> 0 answer 0x0`（仍成功）；
    - `CP_SET_SECURE_MODE 0 submit=0x0 fence=4 result=1 observed=3`；
    - `blit status before cp_int 0x1`，`blit A0/A11 skipped (CP already stopped)`；
    - 系统继续启动，无 panic。
  - **结论**：secure-mode 包本身需要一个已加载/认证的 **zap shader**；单独发就是 opcode error。
  - **metal93 做法**：
    1. 在 `liuqin_adreno.c` 里把 `step_zap_and_secure` 的 PAS 部分导出为 `liuqin_adreno_zap_load()`（PAS shutdown/init image/mem setup/写 segments/auth and reset）；
    2. `NativeAdrenoService` 在 A0/A11 之前调用它（日志 `zap load -> ?`），再发 `CP_SET_SECURE_MODE 0`。
  - 判读：
    - `zap load -> 1` + `CP_SET_SECURE_MODE result=0` + A11 `512/512` → secure mode 根因，引擎离开 secure mode；
    - `zap load -> 0` → PAS/zap 失败；看 `LIUQIN ADRENO: 21 stop` 行；
    - `CP_SET_SECURE_MODE` 仍 result=1 → zap 认证了但 packet 仍被拒；查 opcode/secure 状态；
    - 开机挂/panic → 回 metal79。
  - 产物：
    - 内核 `kernel.development.liuqin` `a0d87ffc252949ac0ed258512bf2c667198595fabbc1f72461efdd0f0ff32d9a`；
    - payload `shim/out/payload-metal93-zap-secure.bin` `f587da986bcd0be9f079623c7475c875432cd78e95a125e5b4a3024c19c2b9b8`；
    - 引导镜像 `shim/out/shim-boot-metal93-zap-secure.img` 32,071,680 B `bea70478308f793bb99f001f9d9c234fc8863b8aa24d0f7c195bbd7072862461`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 350 步：metal91 的 SCM aperture **成功**（`-> 0 answer 0x0`），但 A11 仍 0；查 stock `adreno_a6xx.c` 发现真正的 secure 开关是 **zap shader + `CP_SET_SECURE_MODE`**。port 从没加载 zap、也没发这条 CP 包，所以 GPU 一直在 **secure mode**，引擎用 SID 27。metal92 = 先只发 `CP_SET_SECURE_MODE 0`（0x70668001），看 CP 是否接受**
  - **metal91 事实**（`91.txt` 1,182,600 B `fcfd2232b04df95d81bdd801cdea29e5c3f8c0b487cd0fc57d2a51baaf5d0944`）：
    - `SCM SMMU aperture cb2 -> 0 answer 0x0` —— QSEE 接受并返回成功；
    - A11 仍 `filled 0/512`；客户端仍 `smmu fault sid=0x1b08 far=0xd5fe77b6`、`sid=0x1b00 far=0x28000`；
    - SMMU 表仍是 slot0/1/2 = SID5/0/1，slot3..5 空。
  - **stock 事实**（`adreno_a6xx.c:1147-1175`）：
    - `adreno_zap_shader_load()` 加载 zap；
    - 若 zap 已加载：发 `CP_SET_SECURE_MODE`（`cp_packet(CP_SET_SECURE_MODE,1); 0`）→ 离开 secure mode；
    - 若没 zap：写 `RBBM_SECVID_TRUST_CNTL=0`（我们写它会 data abort）。
    - port 的两条都没做；引擎一直用 secure SID 27。
  - **metal92 做法**：在 A0/A11 之前发一条 `CP_SET_SECURE_MODE 0`（type-7 `0x70668001`，payload 0），看 CP 是否接受；`result=1`/`cp_int 0x1` 表示这条包需要 zap 才可用。
  - 判读：
    - `CP_SET_SECURE_MODE 0 result=0` 且 A11 `512/512` 或客户端有数据 → secure mode 是根因，且不需要 zap；
    - `result=1`/`CP_OPCODE_ERROR` → 需要先加载 zap；下一步做 PAS/zap；
    - result=0 但 A11 仍 0 → secure mode 不是全部，继续查 zap/secure SID。
  - 产物：
    - 内核 `kernel.development.liuqin` `4ccdcbc3388e6559fab9c1d4acd72ad424f144f4c96e8864e0a7e5f01b77d720`；
    - payload `shim/out/payload-metal92-secure-mode.bin` `bf4533f33fff5e2ec9e5624ca6016e7f26c6a9f5ae3029f36beb0b3af7018c62`；
    - 引导镜像 `shim/out/shim-boot-metal92-secure-mode.img` 32,071,680 B `1ea366f4d14792354a1667549416fe3f2c170ea4e568b839d96e30567565d646`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 349 步：metal90 的 panic 是**构建遗漏**——镜像还带着 metal89 的 slot2 SID27 写，SCM 之前就 panic 了；`SCM SMMU aperture` 那行没执行。metal91 把 slot2 恢复成 **SID1**，只保留 **SCM aperture**，让 SCM 真正跑**
  - **metal90 事实**（照片；PC `0xfffffe0007abf6a0`、LR `0xfffffe0007abf658`、ESR `0x96000010`、FAR `0xfffffe50451ec808` = SMMU base + 0x808）：
    - `atos`：PC = `NativeAdrenoService.cpp:1816`（slot2 SMR 写），和 metal89 同一行；
    - 日志里没有 `SCM SMMU aperture`，证明 panic 发生在 SCM 之前。
  - **结论**：metal90 没测到 SCM；不是 SCM 失败。
  - **metal91 做法**：`sm[0x800/4+gslot1]=0x84000001`（SID1），删除 SID27 写和 `engine sid 27` 日志；保留 `nativeAdrenoScmAperture(2)`，在 route/audit 之后调用。
  - 判读：
    - `SCM SMMU aperture cb2 -> 0` 且 A11 `512/512` 或客户端有数据 → SCM aperture 是根因；
    - SCM 0 但 A11 仍 0 → aperture 不是全部；
    - SCM 非 0/-1 → QSEE/PAS 未就绪；先跑 GPU zap/PAS；
    - 开机正常、无 SCM 行 → SCM 调用位置/编译问题，查镜像。
  - 产物：
    - 内核 `kernel.development.liuqin` `d370df41dd093c48d624f69e39a8d346aa1c7371f02dab51175ffb0bafd1435c`；
    - payload `shim/out/payload-metal91-scm-aperture.bin` `7a3f7175b9185f4b4218b208b984aa2bb464659220a8aefdb4433049180fd53f`；
    - 引导镜像 `shim/out/shim-boot-metal91-scm-aperture.img` 32,071,680 B `f7e43faa395d6437111f100bf74dba76b27d68b88eb1f7798158e04ca04782ea`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 348 步：找到 stock 的 secure 调用——`kgsl_iommu.c` 的 `qcom_scm_kgsl_set_smmu_aperture(cb_num)`（SCM SVC_MP 0x0c / cmd 0x1b）。port 从没发过它；metal90 在 GPU context/route 之后发这个 SCM，把 CP 的 SMMU aperture 指到 CB2，再看 A11/客户端**
  - **stock 事实**（Xiaomi 5.10 源码）：
    - `drivers/gpu/msm/kgsl_iommu.c` `set_smmu_aperture()` 调 `qcom_scm_kgsl_set_smmu_aperture(context->cb_num)`；
    - `drivers/firmware/qcom_scm.c`：`.svc=QCOM_SCM_SVC_MP(0x0c)`、`.cmd=QCOM_SCM_MP_CP_SMMU_APERTURE_ID(0x1b)`、owner `ARM_SMCCC_OWNER_SIP`；`args[0]=0xffff0000|((QCOM_SCM_CP_APERTURE_REG&0xff)<<8)|(cb&0xff)`，`args[1..3]=0xffffffff`，`arginfo=QCOM_SCM_ARGS(4)`；
    - SMC fn = `SCM_64|SCM_SIP|(0x0c<<8)|0x1b` = **0x42000c1b**；x1=4，x2=0xffff0002，x3..x5=~0，x6=session。
  - **metal90 做法**：`NativeAdrenoService.cpp` 里加 `nativeAdrenoScmAperture(2)`（复用 `liuqin_adreno.c` 同款 `smc #0` + `SCM_INTERRUPTED` 循环），在 route/audit 之后、GPU buffers 之前调用；日志 `SCM SMMU aperture cb2 -> %d answer 0x%llx`。
  - 判读：
    - SCM 返回 0 且 A11 `512/512` 或客户端 `T/M` 有数据 → **这就是缺的 secure 调用**；
    - SCM 返回 0 但 A11 仍 0 → aperture 不是全部，继续查 secure SID/GPU zap；
    - SCM 返回非 0/-1 → QSEE 未就绪或参数不对；下一步先跑 GPU zap/PAS（`liuqin_adreno.c` 的 `SCM PAS auth and reset`）；
    - 开机挂/panic → SMC 路径问题；回 metal79。
  - 产物：
    - 内核 `kernel.development.liuqin` `54e7124989cad4d72ad5d4e3b491ce67025a140ff523cdfc4f460dbbef3a6a0f`；
    - payload `shim/out/payload-metal90-scm-aperture.bin` `01c856e1326bea11d9a8ca2874328c6d629077a989e2e470d59ca91c591620a1`；
    - 引导镜像 `shim/out/shim-boot-metal90-scm-aperture.img` 32,071,680 B `c12cd0c9f160ff050551e62bf0ea5db707fc500701c38ee07683171dd48dd771`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 347 步：metal88 的 panic 定位到 **slot3 SMR 写**（`NativeAdrenoService.cpp:1815`，FAR = SMMU base + **0x80c**）：slot0、slot3/4/5 **全是安全槽**，这个 SMMU 只有 **slot1/slot2** 可写。metal89 = 放弃 SID1，把 **SID27 写进 slot2**；A11 用 SID0/BR**
  - **metal88 事实**（照片；PC `0xfffffe0007abf6b0`、LR `0xfffffe0007abf658`、ESR `0x96000010`、FAR `0xfffffe463bf2080c`）：
    - `atos`：PC = `NativeAdrenoService.cpp:1815`（`sm[0x800/4+3]=0x8400001b`，slot3 SMR 写），LR = 1783
    - FAR = SMMU base + **0x80c** = SMR slot3
    - slot3 在 metal86 里读到的是 `SMR 0 / S2CR type 2`，但**写**它 data abort → 它是安全槽
  - **结论**：非安全可写 SMR 只有 slot1（SID0）和 slot2（SID1）；slot0/3/4/5 都是安全槽。引擎 SID27 只能占 slot2，代价是让出 SID1（CP 的第二条流）。
  - **metal89 做法**：`sm[0x800/4+gslot1]=0x8400001b`（SID 27，slot2，mask 0，CB2）；不再写 slot3；A11 用 SID0/BR。
  - 判读：
    - A11 `512/512` 或客户端 `T/M` 有数据 → SID27 映射是根因；
    - A11 仍 0、fault 还是 `0x1b08` → slot2 没匹配/写没生效；
    - 开机 panic 在 slot2 写（FAR = base + 0x808）→ SID27 也不能写进非安全槽；这个 SMMU 无法映射引擎 SID，下一步换 SMMU/secure world；
    - 不再 panic、fault 消失但 A11 仍 0 → 映射生效，转 A2D state/格式；
    - 客户端 fence 超时增多 → SID1 让出的代价；A11 的 BR 结果仍有效。
  - 产物：
    - 内核 `kernel.development.liuqin` `a72b6e45da67c7bc560dfe8b9857d7369175c9b1c021c9d3e2e9ea629a1c5fff`；
    - payload `shim/out/payload-metal89-sid27-slot2.bin` `71b3935f38ae5451aeaf81bf3c1395789f2923452a8a9f26249c02fa998b0fb6`；
    - 引导镜像 `shim/out/shim-boot-metal89-sid27-slot2.img` 32,071,680 B `3ee479f451bcbcf70cfc05677a837feb56af87894efb127994791a6140894d64`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 346 步：metal87 的 panic 不在 SID27，而在 **A3 扫 SMR 槽**（`NativeAdrenoService.cpp:1758`）：这次启动它读了 slot0，FAR = SMMU base + 0x800；slot0 在 GPU context 建立前是安全/未配置的，读就 data abort。metal88 = 固定 GPU slots **1/2**、跳过 A3 扫描，然后继续 **SID27 → slot3**
  - **metal87 事实**（照片；PC `0xfffffe0007abf56c`、LR `0xfffffe0007abf53c`、ESR `0x96000010`、FAR `0xfffffe4f9c226800`）：
    - `atos`：PC = `NativeAdrenoService.cpp:1758`（`A3 i=...` 的 IOLog），LR = 1753（A3 循环）
    - GPU ctx step 1..7 都跑完了；route/SID27 还没执行
    - FAR = SMMU base + **0x800** = slot0；A3 循环 `if(i==slot)continue` 这次没跳过 slot0（GMU slot 不是 0），于是读了安全槽
  - **结论**：A3 的 0..15 盲扫发生在 GPU context 之前，slot0 可能随机是安全槽；这不是 SID27 的问题。metal86 的只读表已经给出 GPU slots = 1/2，直接固定即可。
  - **metal88 做法**：`gslot0=1,gslot1=2`，A3 扫描跳过；route 仍写 SMR1/2；然后 **slot3 = SID 27**（`0x8400001b`，mask 0，CB2）；IDR/slot 表在 GPU context 之后读（metal86 已证明安全）。
  - 判读：
    - A11 `512/512` 或客户端 `T/M` 有数据 → SID27 映射是根因；
    - A11 仍 0、fault 还是 `0x1b08` → slot3 没匹配/写没生效；
    - 开机 panic 在 slot3 写（FAR = base + 0x80c）→ slot3 也是安全槽；回 metal86，试 slot4/5 或换 SMMU；
    - 开机不再 panic 且 fault 消失 → SID 映射生效，但 A2D 还有别的问题（转 A2D state/格式）。
  - 产物：
    - 内核 `kernel.development.liuqin` `5127bf0ec0fc194283800e63b0def708cafbcb1e23e0693e528afe8709d84c8b`；
    - payload `shim/out/payload-metal88-sid27-fixed.bin` `a6cca3245316a47615a1b3f155c93b434c026156ba8c35a04109b404553b095e`；
    - 引导镜像 `shim/out/shim-boot-metal88-sid27-fixed.img` 32,071,680 B `f141e5afbdfc76262879ee04584ba5dcf5d89e1d144b2d5b2e57193ea1358a8c`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 345 步：metal86 只读 SMMU 表成功。`IDR0=0x002e0406`（6 个 SMR）；slot0/1/2 = SID 5/0/1，slot3/4/5 = SMR 0、S2CR type2（free）；slot6/7 = 0。fault 的 `sid=0x1b08`/`0x1b00` 拆开 = **SID 0x1b（27）+ SSID 0x08/0x00**；引擎用 **SID 27**，不是 8/0x1b08。metal87 = 在 free 的 slot3 写 SID 27（mask 0，CB2），不再牺牲 SID1**
  - **metal86 事实**（`86.txt` 1,192,300 B `a994210b413316fbe0e0100e9d50b3c4392bfd6bd9733239b4eaa5f8ebdd2234`；无 panic，系统继续启动）：
    - `SMMU IDR0..5 002e0406 00000000 00000000 00000000 0000001c 00000000 base 0xfffffe4fa25b0000`
    - `slot 0 smr 0x84000005 s2cr 0x1`（GMU，SID 5）
    - `slot 1 smr 0x84000000 s2cr 0x2`（SID 0）
    - `slot 2 smr 0x84000001 s2cr 0x2`（SID 1）
    - `slot 3/4/5 smr 0 s2cr 0x00020000`（free，type 2 fault）
    - `smmu audit dt sids 2 unmatched 0`
    - `smmu fault … far=0xd5fe77b6 sid=0x1b08`、`far=0x28000 sid=0x1b00`
    - A11 仍 `0/512`；客户端仍 `T 0/4096`、`M 999 wrong`
  - **结论**：`sid` 的高字节 **0x1b (27)** 是 SID，低字节 0x08/0x00 是 SSID；**引擎用 SID 27**。SMMU 有 6 个 SMR，slot3 是空的；之前 slot3+ 写中止是**带 mask** 的 SMR，不是槽本身不可写。SID 8/0x1b08 都写错了对象。
  - **metal87 做法**：保持 SID0/1 原路由，额外写 **slot3 = 0x8400001b（SID 27，mask 0）**，S2CR = CB2；不牺牲 SID1。
  - 判读：
    - A11 `512/512` 或客户端 `T/M` 有数据 → SID 27 映射是根因；
    - A11 仍 0、fault 的 `sid` 还是 0x1b08 → SMR3 没匹配/写没生效；
    - 开机 panic 在 slot3 写 → slot3 实际也是安全槽；回 metal86，换另一个槽/换 SMMU；
    - 客户端 fence 超时变化 → SID 0/1 未动，理论上不该恶化。
  - 产物：
    - 内核 `kernel.development.liuqin` `4779d3e9525010119ae923ef37c3c46e1acada18bd066344fd5e5df271fdbfc4`；
    - payload `shim/out/payload-metal87-sid27.bin` `7d632276aac273723302877002e1c9e2352d595a11cf296de35850143f3b9676`；
    - 引导镜像 `shim/out/shim-boot-metal87-sid27.img` 32,071,680 B `5e787bc0b5861f8c86d5dee2eb1e922d80b7f126e0f1923b52f882982625892b`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 344 步：metal85 的 SID 8（`0x84000008`）也在同一处 SMR slot2 写 data abort（pc/FAR 与 metal84 完全一致）；这个 qsmmu 连 SID 8 都不收。不再盲写 SID；metal86 = 只读 SMMU 的 IDR0..5 + slot0..7 的 SMR/S2CR，让它自己报有效范围**
  - **metal85 事实**（照片；PC `0xfffffe0007abf6cc`、LR `0xfffffe0007abf67c`、ESR `0x96000010`、FAR `0xfffffe503f7d8808`）：与 metal84 的 panic **完全同址**（line 1806 / SMR slot2）；`0x84000008` 也被拒绝。
  - **结论**：SMR ID 的有效范围比 8 还小，或这个 qsmmu 的 SID 表只有很少的流；**不能再猜 SID**。先读 IDR/SMR 表。
  - **metal86 做法**：保持原来的 SID0/1 路由（`0x84000000`/`0x84000001`），**不写 SID8**；route 块后只读打印：
    - `SMMU IDR0..5 … base 0x…`
    - `SMMU slot 0..7 smr 0x… s2cr 0x…`
    - A11 + 客户端正常跑。
  - 判读：
    - IDR0/IDR1 的 NUMSMR/SID 宽度 + 有效 SMR 槽 → 下一个可用的 SID；
    - 若 SID 表只有 0/1/5，引擎 SID 8 很可能属于**另一个 SMMU**（GPU SMMU），下一步换 SMMU base；
    - A11/client 仍 0 但没有新 panic；SMMU fault 的 sid 仍是 0x1b08。
  - 产物：
    - 内核 `kernel.development.liuqin` `3d532955a061b9822ca1d4eb8ef2ccf342979628873db2ab3f55d98905bbeadf`；
    - payload `shim/out/payload-metal86-smmu-read.bin` `49d385ed5ca63152f182fef5942dbcc51f122b25d152319601c05b27f7005ab4`；
    - 引导镜像 `shim/out/shim-boot-metal86-smmu-read.img` 32,071,680 B `6e13c2b7a5556030e8327cf3d8e44b79085ec949fdd6f053c2293c5f63c192a3`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 343 步：metal84 的 panic 也定位了——pc 在 **SMR gslot1 写**（`NativeAdrenoService.cpp:1806`），FAR = SMMU base + **0x808** = **SMR slot 2**。不是槽不可写，而是 `0x84001b08` 这个 **SMR 值非法**（SID 字段只有 8 位）。fault 的 `sid=0x1b08` 是 **SID 0x08 + SSID 0x1b** 打包。metal85 = 把 **SID 8**（`0x84000008`）写进 gslot1**
  - **metal84 上机事实**（照片；PC `0xfffffe0007abf6cc`、LR `0xfffffe0007abf67c`、ESR `0x96000010`、FAR `0xfffffe503f7d8808`）：
    - `atos` 符号化：pc = `NativeAdrenoService.cpp:1806`（`sm[0x800/4+gslot1]=…`），lr = 1803
    - SMMU base 从上一条 `GPU ctx tables map 0xfffffe503f938000` 附近推断为 `0xfffffe503f7d8000`；FAR = base + **0x808** = SMR slot 2
    - 也就是说：gslot1 本身可写（原来的 SID1 就写进去了），但 **`0x84001b08` 这个 ID 非法**，写入触发 data abort
  - **结论**：这个 qsmmu 的 SMR ID 字段是 **8 位**；`0x1b08` 不是合法 SID。SMMU fault 记录的 `sid=0x1b08` 是 **SID 0x08 + SSID 0x1b** 打包，真正要映射的是 **SID 8**。
  - **metal85 做法**：`sm[0x800/4+gslot1] = sid8 ? 0x84000008 : 0x84000001`；SID1 让出；`sheng-gpu-sid8=0` 回退。A11 用 SID0/BR。
  - 判读：
    - A11 `filled 512/512` 或客户端 `T/M` 有数据 → SID 8 映射是根因；
    - A11 仍 0、fault 的 sid 变成 0x1（SID1 让出）→ A2D 不用 SID8，需换 SID；
    - A11 仍 0、fault 还是 0x08/0x1b08 → SMR 没匹配到 SSID 0x1b，或引擎用别的 SID；
    - 开机挂 → 看 route 日志；这次 SMR 值是合法的，理论上不会 panic。
  - 产物：
    - 内核 `kernel.development.liuqin` `4503c83952ebefa68cfeca13db9efdf01c441c1729e71ae14a3b5a73d3c5bcee`；
    - payload `shim/out/payload-metal85-sid8.bin` `3505a184c6f8d56cedfc286b639caf6fa755f8f234c2ea3342e3591f7debe8ce`；
    - 引导镜像 `shim/out/shim-boot-metal85-sid8.img` 32,071,680 B `5731a8353aec3157a70c36c48ccc379e2ba55a366f27b843deefc10f0fcc6dd2`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 342 步：metal83 复用 GMU 的 slot0 也同样 data abort——FAR = SMMU base + 0x800，正好是 **SMR0**，slot0 和 slot3+ 一样是安全寄存器。metal84 = 不碰 slot0，把引擎 SID 0x1b08 直接写进可写的 `gslot1`（代价：CP 的 SID1 流让给引擎；A11 用 SID0/BR）**
  - **metal83 上机事实**（照片；PC `0xfffffe0007abf754`、LR `0xfffffe0007abf71c`、ESR `0x96000010`、FAR `0xffffffe4fa122c800`）：
    - 最后一行是 `GPU route published smmu 0xfffffe4fa122c000 slot 2 for the sid sweep`
    - 紧接着 `Kernel data abort`；`GPU route engine sid 0x1b08` **没有打出来**
    - `FAR - smmu_base = 0x800` → 正是 SMR0 的偏移；slot0 写就 abort
  - **结论**：这个 qsmmu 的**非安全可写 SMR 只有 gslot1/gslot2**；slot0（GMU SID5）和 slot3+ 都是安全寄存器，不能碰。SID 0x1b08 必须占用一个可写槽，代价是让出原来的 SID1。
  - **metal84 做法**：route 块里 `sm[0x800/4+gslot1] = sid8 ? 0x84001b08 : 0x84000001`，S2CR 保持 CB2；`sheng-gpu-sid8=0` 回退到 SID1。A11 用 SID0/BR，仍有一条 CP 流。
  - 判读：
    - A11 `filled 512/512` 或客户端 `T/M` 开始有数据 → SID 0x1b08 映射是根因；
    - A11 仍 0、SMMU fault 的 sid 变成 0x1（CP 的 BV/SID1）→ 预期代价，A2D 没用到 SID0x1b08，需再想别的槽；
    - 开机挂/重启 → 不是 SMR0 问题；看 route 日志；
    - 客户端 fence 超时增多 → SID1 被让出，CP 的 BV 流受影响；A11 的 BR 结果仍有效。
  - 产物：
    - 内核 `kernel.development.liuqin` `2886ec0802a7fb86a4e7ebbd0902affe0c41b8559c1cfce4b37ec300d817d3bc`；
    - payload `shim/out/payload-metal84-sid8-gslot1.bin` `747ad1cdd2d4a7acdbb76e3685f8df8f9db993bdd787c4b215019a1c8042a062`；
    - 引导镜像 `shim/out/shim-boot-metal84-sid8-gslot1.img` 32,071,680 B `928ce576c46142233ef8b2d3ba51ee3872f484b45d454d0ca2a57635f549e367`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 341 步：metal82 的多假设扫描里，安全变体 `base/fmt30/mib8x8/hwcg0` 全 0；`addr mode` 那组写寄存器把 CPU 打 data abort（panic 照片）。真正的线索在 81.txt 的 SMMU fault：引擎事务 `sid=0x1b08`、`far=0xd5fe77b6`，没有 SMR 映射。metal83 = 关掉扫描 + 把 SID 0x1b08 映射到 CB2**
  - **metal82 上机事实**（照片；PC `0xfffffe0007abc7b0`、ESR `0x96000010`、FAR `0xfffffe3a0973f614`；metal81 日志 `81.txt` 1,265,105 B `4766303a8b9e9bbba576a3ad57eaaea333c28cb0534bf3f23acca5036e18899b`）：
    - `A2D sweep snapshot sptprac 0xffffffff/0xffffffff/0xffffffff blk 0xdeadbead/0xdeadbead/0x0 clk 0x8aa8aa82 rb_ccu 0x68 rbbm 0x1`
    - `A2D base … filled 0/512`
    - `A2D fmt30 … filled 0/512`
    - `A2D mib8x8 … filled 0/8`
    - `A2D hwcg0 … filled 0/512`
    - 之后 **kernel data abort**（addr mode 组某条写；和 `RBBM_SECVID_TRUST_CNTL` 同类）
  - **81.txt 的关键事实**（metal81 的日志）：`smmu fault #1…13 fsr=0x400 far=0xd5fe77b6 sid=0x1b08`。引擎（A2D/SP/RB）**确实在发写事务**，但 SID 0x1b08 没有匹配的 SMR，SMMU 直接 fault；CP 不等引擎，所以 fence 正常、数据不落。
  - **metal83 做法**：
    1. `sheng-gpu-sweep` 默认 0，扫描只作为 opt-in，避免 addr mode 再 panic；
    2. route 块把 GMU 的 SMR slot（SID 5，已启动完）复用为 **SID 0x1b08 → CB2**（`0x84001b08`，S2CR CB2），`sheng-gpu-sid8=0` 可关；
    3. 保留 A11 + 客户端；A11 的 A2D 和客户端 `T/M` 就是判定。
  - 判读：
    - A11 `filled 512/512` 或客户端 `T/M` 开始有数据 → SID 映射是根因；
    - 仍 0 且 SMMU fault 消失 → 引擎写到了 CB2，但 A2D/客户端还有别的问题；
    - 仍 0 且 fault 的 sid 变了/还在 → SID 不是 0x1b08 或 SMR 没生效；
    - 开机挂/重启 → 复用的 GMU slot 影响了 GMU；下一版换 slot 或先读 GMU 状态。
  - 产物：
    - 内核 `kernel.development.liuqin` `2c7f499a0d5e9f7c087e5dc9d7f3dd06b3b6a111f4c59299173de45870674dcb`；
    - payload `shim/out/payload-metal83-sid8.bin` `89981f7436211740161959f1a0abafb306d02ab554deb945585c2817e87c4a47`；
    - 引导镜像 `shim/out/shim-boot-metal83-sid8.img` 32,071,680 B `76044a616649d20883b4a0a442ffabc6e13db151b40246e1257169466d4c7890`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 340 步：不再一轮一个变量。metal82 = 一次启动跑 A2D 扫描：先打 GPU/GMU 快照，再依次跑 base/fmt30/mib8x8/hwcg0/sptprac/addr_mode/blkreset/op0-2/fill2d/base-wfi，每组独立读回，最后打 `first_fill`**
  - metal81（只读 GPU 窗口 SPTPRAC）**没有上机**，直接被 metal82 吸收；metal80 的 GMU-core 读数作废（见第 339 步）。
  - **metal82 设计**：
    1. snapshot：`SPTPRAC` 0x1F8D0/0x1A880/0x1A881、block reset 0x45/0x46/0x73、`RBBM_CLOCK_CNTL` 0xAE、`RB_CCU_CNTL` 0x8E07、`RBBM_STATUS` 0x210；
    2. A2D 变体（每组独立 fence + 读回）：
       - `base`：0x4b/0x07f04b80/0xf25c，512×1，pitch 0，op3；
       - `fmt30`：0x30/0x00f03080/0xf180，512×1；
       - `mib8x8`：0x30/0x00f03080/0xf180，8×8，pitch 4（读第一行 8 dwords）；
       - `hwcg0`：`RBBM_CLOCK_CNTL=0` 后 base，再恢复；
       - `sptprac`：若 0x1F8D0 bit4，写 0x1A881=1 后 base；
       - `addrmode`：写 a6xx_start 的 12 个 `*_ADDR_MODE_CNTL=1` 后 base；
       - `blkreset`：0x45/0x46/0x73=0 后 base；
       - `op0`/`op1`/`op2`：`CP_BLIT` op 变体（可能挂，放后面）；
       - `fill2d`：`CP_EVENT_WRITE` 0x27；
       - `base-wfi`：base + `CP_WAIT_FOR_IDLE`（最后一个，最可能挂）。
    3. 每组日志 `A2D <name> … result=? filled ?/?`；最后 `A2D sweep first_fill=<name> probes=<n> dead=<0/1>`。
  - 判读：`first_fill` 就是根因候选——`hwcg0` → HWCG 时钟门控；`sptprac` → SP/TP 电源；`addrmode` → 地址模式；`blkreset` → 复位；`base`/`fmt30`/`mib8x8` → A2D state/format；`base-wfi` 超时 → blit 已派发但引擎不完成；`base-wfi` 退休但 `first_fill=none` → 根本没派发。
  - 产物：
    - 内核 `kernel.development.liuqin` `6ea9fa2ff4f9afb9ed6c8cd805af90aaaeeac41b8c8c3fed972d0ae471e4dfa6`；
    - payload `shim/out/payload-metal82-sweep.bin` `48a8561c3f1e999983d082a9e1a54b2c57a81695dd7b7f384f8e6b4fa5ddbd5b`；
    - 引导镜像 `shim/out/shim-boot-metal82-sweep.img` 32,071,680 B `15e3d6be4a2f19c2473b7cec2a369153da4c330d272e218baf6ff1559a1a1b7a`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 339 步：metal80 的 SPTPRAC 读数是 0/0/0，但读的是 GMU-core 窗口；`a6xx_reg.h` 的 `0x1F8D0`/`0x1A880`/`0x1A881` 是 **GPU 窗口**偏移，metal81 改到 GPU 窗口读并保留 power-on 尝试**
  - **metal80 上机事实**（`1238.txt` 1,282,313 B `6f80880076d017d9e7913cb1d3043d10e1a996f4004949c63e48ef70ebfff16f`；镜像 metal80 `30464670…`）：
    - `OOB GPU_SET acked (intr=0x80000000)`
    - `SPTPRAC after OOB status 0x0 clock 0x0 power 0x0 (power_off 0 clk_off 0)` —— 三个都 0，不像真实状态
    - A11 仍 `result=0 filled 0/512`，hlsq `result=0`，`cp_int 0x0`
    - 客户端仍 `T … 0/4096`、`M … 999 wrong`；fence 18/31/32 超时
  - **结论**：metal80 的 SPTPRAC 读数**不成立**——它用的是 GMU-core 窗口（`0x58d0/0x880/0x881`），而 `a6xx_reg.h` 的 `A6XX_GMU_SPTPRAC_PWR_CLK_STATUS 0x1F8D0`、`A6XX_GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL 0x1A880`、`A6XX_GMU_GX_SPTPRAC_POWER_CONTROL 0x1A881` 是 **GPU 窗口**的绝对偏移。0/0/0 只是窗口没解码，不是“SPTPRAC 关”。
  - **metal81 做法**：在 `startCp` phase 0 用 `gpu_read32`/`gpu_write32` 读/写 GPU 窗口的 `0x1F8D0`/`0x1A880`/`0x1A881`；状态 bit2=power off、bit4=clk off；若 clk off，写 power control=1 并 poll 20 ms。
  - 判读：
    - `SPTPRAC GPU status … clk_off 0` → SP/TP 是开的，SPTPRAC 这条排除；下一步查 GX block reset / 上电顺序；
    - `clk_off 1` 且 power-on 后 `clk_off 0` → SPTPRAC 是根因；下一版把 power-on 固化；
    - `clk_off 1` 且写不动 → a730 的 SPTPRAC 由 GMU 固件控制；查 OOB/HFI 顺序；
    - 读回 `0xffffffff` 或 `0xdeadbeef` → GPU 窗口不覆盖/偏移错，先修偏移。
  - 产物：
    - 内核 `kernel.development.liuqin` `cdb8f63f2a94c516a69bf08a053c47adf244bbf3747dc4f619ce7242e3c4a253`；
    - payload `shim/out/payload-metal81-sptprac-gpu.bin` `48493720982a3d400f2138b73adcc16b26c974b59687d0893f18983a131e6c9b`；
    - 引导镜像 `shim/out/shim-boot-metal81-sptprac-gpu.img` 32,071,680 B `a144f25c3409802bba642f02f983f3a21e3da785ccd006ca0338ea682b6a0eba`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 338 步：metal79 上机 = CCU cache offset + CCU clean 都退休，A11 仍 0/512；CCU 这条排除，A2D 本身不写。共同嫌疑转到 SPTPRAC（SP/TP 电源/时钟）：SP_UPDATE 等 SP，A2D 的输出要过 TPL1/SP，而 RB/CCU 是活的。metal80 = 读 GMU 的 SPTPRAC 状态（0x50d0+0x800），如果 clk off 就试一次 a630 的 power-on**
  - **metal79 上机事实**（`liuqin-log-20261031-121832.txt` 1,186,162 B `f831c033bae54e75a02613feae222b0ddde96692df9ddd0fda3f7e991ddd4aff`；镜像 metal79 `50dcda14…`）：
    - `blit A11 (canonical 184 dwords, ccu-wfi, ccu-cache0=1, ccu-clean) submit=0x0 fence=5 result=0 observed=5 filled 0/512` → **CP 和 CCU clean 都退休，A2D 仍不写**
    - `blit hlsq-flush submit=0x0 fence=6 result=0 observed=6`、`blit status after cp_int 0x0 hwfault 0x0 rbbm_int0 0x0 prot 0x0 rbbm_status 0x1`
    - 客户端仍 `T s0 r0 v512 c0 g0/4096`、`M … 999 wrong`；fence 12/13/16 超时；`cp state rb_ccu 0x68`
    - SMMU 事实：GMU 用 SID 5→bank1，GPU context 用 SID 0/1→CB2，`sgfsr=0 fsr=0`；引擎写不落不太像 SMMU 翻译问题
  - **结论**：CCU cache offset 不是根因；A2D 的 clean 路径通，但 A2D 没有写。SP_UPDATE 等 SP + A2D 依赖 TPL1/SP + RB/CCU 活 → 共同嫌疑是 **SPTPRAC（SP/TP）电源/时钟**。
  - **metal80 做法**：`powerGx()` 之后读 GMU 的 `SPTPRAC_PWR_CLK_STATUS`（0x50d0+0x800=0x58d0）、`GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL`（0x80+0x800=0x880）、`..._POWER_CONTROL`（0x81+0x800=0x881）；状态 bit2=power off、bit4=clk off；若 clk off，写 power control=1 并 poll 20 ms。
  - 判读：
    - `SPTPRAC … clk_off 0` → SP/TP 是开的，不是这条；下一步查 GX block reset / 上电顺序；
    - `clk_off 1` 且 power-on 后 `clk_off 0` → SPTPRAC 是根因；下一版把 power-on 固化并看 A11/客户端；
    - `clk_off 1` 且写不动 → a730 的 SPTPRAC 由 GMU 固件控制；查 OOB/HFI 顺序（我们是 HFI start 先、OOB 后，原厂相反）；
    - status/clock/power 读回 `0xdeadbeef` → GMU 窗口偏移不对，先修偏移。
  - 产物：
    - 内核 `kernel.development.liuqin` `47049329facb0e228a198da260c58a90a9a705586d3fe7c04d226a8a2d6cc760`；
    - payload `shim/out/payload-metal80-sptprac.bin` `dbd9cfb12303a6e65f2b2b1e5c43659bceef998b6c952e6aa9236b8d5067844e`；
    - 引导镜像 `shim/out/shim-boot-metal80-sptprac.img` 32,071,680 B `304646708c35f44eb1c9dcf620f254b1923aae02f97274886d1ac84e248728aa`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 337 步：metal78 上机 = CP 派发那组偏移在 a730 上是死值（`0xdeafbead`），不是根因；客户端 A2D 块 `CP_BLIT` 后紧跟 `CCU_CLEAN`，CP 就停在这里，目的页读回还是 `deadbeef`。metal79 = 把 A2D 重放里的 `RB_CCU_CACHE_CNTL` 从 `0x80000000` 改成 0（A7XX bit 31 是 GMEM color cache offset，不是使能位），并在 `CP_BLIT` 后补 CCU clean，验证是不是 CCU 配置让 clean 挂住**
  - **metal78 上机事实**（`1208.txt` 1,218,203 B `c5be7caaa57904ce92f53f555a19345e7201802694e6d8a1b81710c3ff39c4c7`；镜像 metal78 `b88f1b16…`）：
    - `CP dispatch before roq1 0xdeafbead roq2 0xdeafbead pool 0xdeafbead dbg 0x0`
    - `CP dispatch wrote 4/4 roq1 0xdeafbead roq2 0xdeafbead pool 0xdeafbead dbg 0x2f`
    - `0xdeafbead` 和 `0x43` 一样是“这个偏移不是有效寄存器”的死值 → **a6xx 的 `CP_ROQ_THRESHOLDS`/`CP_MEM_POOL_SIZE` 偏移在 a730 上不成立**；`wrote 4/4` 只是总线接受，不代表生效。
    - A11 仍 `result=0 filled 0/512`，hlsq `result=0`，`cp_int 0x0`。
    - 客户端仍 `T s0 r0 v512 c0 g0/4096`、`M … 999 wrong`；fence 13/18/52 超时。
    - 客户端 A2D 块（fence 11）：`CP_BLIT op 3` 后紧跟 `CCU_CLEAN_COLOR/DEPTH`；目的 `0x2002c000` 的 `wtgt` 读回仍是 `deadbeef`；CP `rptr 558/599` 停在这块；`cp state rb_ccu 0x68`。
    - 客户端写 `RB_CCU_CACHE_CNTL=0x80000000`；按 A7XX 定义 bit 31 是 `COLOR_OFFSET`（GMEM color cache 偏移），不是 A6XX 的“使能”位；sysmem 目的地下这个偏移无效。
  - **推断（未验证）**：CCU clean 挂住可能是 `RB_CCU_CACHE_CNTL=0x80000000` 把 color cache 指到了无效 GMEM 偏移；A2D 的写可能进了 CCU，没刷回 DRAM（所以读回是原值）。
  - **metal79 做法**：A11 重放里把 `RB_CCU_CACHE_CNTL` 的 payload 强制 0，并在 `CP_BLIT` 后补 `CCU_CLEAN_COLOR/DEPTH`；日志行 `blit A11 (canonical … ccu-wfi, ccu-cache0=1, ccu-clean)`。
  - 判读：
    - `result=0` + `filled 512/512` → CCU cache offset 是根因，A2D 一直能写；下一步把客户端流里的 `0x88e5` 也改 0（需要 kernel 在提交时 patch IB，或改 turnip）；
    - `result=1` + ring dump 停在 `70460001 00000021/20` → CCU clean 本身就不回，转 GX 电源/复位/顺序；
    - `result=0` + `filled 0/512` → CCU 不是原因，A2D 仍不写，转引擎电源/复位。
  - 产物：
    - 内核 `kernel.development.liuqin` `629df9b0200ffbdc33e19faeebd62219706337ff07f39ada6e92a592fe959093`；
    - payload `shim/out/payload-metal79-ccu.bin` `6d90430c44b0e3cef1d3f89a5a8ced68be3ab5be5da156ff2d125c7401d7a147`；
    - 引导镜像 `shim/out/shim-boot-metal79-ccu.img` 32,071,680 B `50dcda14d595444f08e4330d342f87a7048b0f7ccd06e657de07a3a0a193d234`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 336 步：metal77 上机 = CP 救活了，但 A2D 仍 0/512、客户端 compute 仍 999 wrong；`cp bv roq 0x0/0x0/0x0`（0 available）指向“CP 没有派发队列”。metal78 = 补原厂 `CP_ROQ_THRESHOLDS_1/2` + `CP_MEM_POOL_SIZE`/`DBG_ADDR`，先读回 boot 值再写**
  - **metal77 上机事实**（`liuqin-log-20261031-115716.txt` 1,220,846 B `9c437eb5d6b0b6fddff69b8c71cd283db0f55e0364b238bf4507801ba926777b`；镜像 metal77 `7c039475…`）：
    - `blit A0 mode 0 … result=0 filled 0/512`（对照）
    - **`blit A11 (canonical 180 dwords, ccu-wfi) submit=0x0 fence=5 result=0 observed=5 filled 0/512`** → 坏 parity + WFI 修掉后 **CP 没死**
    - `blit hlsq-flush … result=0`、`blit status after cp_int 0x0 hwfault 0x0 rbbm_int0 0x0 prot 0x0 rbbm_status 0x1` → 全程干净
    - 客户端照旧 `T s0 r0 v512 c0 g0/4096`、`M s0 c999 r64 i4096 q141 k3/4 p13/8 F14 6601 compute: 999 wrong`；系统正常启动到 229 s（1.22 MB 日志）
    - 客户端提交仍超时（seq 11/18/53…）：`cp rptr 558/599`、`cp state rb_ccu 0x68`（客户端的 CCU 写这次落地了）、**`cp bv roq 0x0/0x0/0x0`**、`rbbm 0xc00015/0xc20085/0xc00005`、`rbbm3 0x0/0x300000/0x400000`
    - 客户端 IB 的真实头是 canonical（`408e0701`），不是 metal76 重放里那个错的 `488e0701`；metal76 的坏头是重建流时抄错的。
    - Android oracle（`gpu-oracle.txt` 17,777 B `60bd204a5aaae2ab167297eae44ba141bbf882ac5a21e5a94a85365908a896fa`）：这版 5.10 kgsl 实际只暴露 power/devfreq 和 `hwcg`/`ifpc`/`lm`；`/dev/mem` 不存在、没有 devcoredump、DT 里没找到 `gpu@` 节点、dmesg 为空；有用的事实只有 `devfreq/cur_freq=285 MHz`、`hwcg=1`、`ifpc=1`。**Android 侧拿不到寄存器值**，后续对比仍靠源码。
  - **结论（事实 + 推断）**：坏 parity 修掉后 CP 重放全程干净，说明 **CP 本身没问题**；但 A2D 和客户端 compute 都不出写，且 `CP_BV_ROQ_AVAIL_*` 读回 0（没有可用队列项）。**推断（未验证）**：CP 没有把引擎命令派发出去，因为 `CP_ROQ_THRESHOLDS_1/2`/`CP_MEM_POOL_SIZE` 从没配过（原厂每次启动都写，我们的 `startCp` 一个都没写）。
  - **metal78 做法**：在 `startCp` phase 0（CP protect 之前）先读回 0x08c1/0x08c2/0x08c3/0x090e，写原厂 a640+ 值（`0x8040362C`/`0x02000140`/128/47），再读回并打日志。
  - 判读：
    - `CP dispatch before roq1 0x0 roq2 0x0 pool 0x0 dbg 0x0` → 证实 boot 是 0；
    - `CP dispatch wrote 4/4 roq1 0x8040362c roq2 0x2000140 pool 0x80 dbg 0x2f` + A11 `filled 512/512` → 派发队列是根因；
    - 写了但 A11 仍 0/512、`roq` 仍 0 → 不是这组，转 3D/compute 或 ADDR_MODE/CCU；
    - `wrote <4/4` → 有寄存器被保护/只读，看是哪条。
  - 产物：
    - 内核 `kernel.development.liuqin` `c6d46c72e2509c3d652fdbec516dd872336a792d0b8987b9f8140bcf1c7cf3d1`；
    - payload `shim/out/payload-metal78-dispatch.bin` `b8307e8a385804004b04b4dba1edde0973649133c9200dd7ff71d96c3dc751a0`；
    - 引导镜像 `shim/out/shim-boot-metal78-dispatch.img` 32,071,680 B `b88f1b168aa37d6a8d5fcfd69d30109101a3c1d334231b049a85c86fd9673716`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 335 步：metal76 上机 = CP 死在 A11 的第一个包上，原因是 PM4 头的 parity 位错了（`cp_int 0x1` = `CP_OPCODE_ERROR`），不是引擎不回话。metal77 = 用 `AdrenoPM4Type4` 重建重放流的所有 type-4 头，并在 `RB_CCU_CNTL` 后补 A7XX 必需的 WFI**
  - **metal76 上机事实**（`1138.txt` 528,062 B `6d033b5d909ddac75af009b6b2297a21fc864b188c56ccbda4272dcd76e81e06`；镜像 metal76 `bc7b3355…`）：
    - `blit A0 mode 0 submit=0x0 fence=4 result=0 observed=4 filled 0/512 w0 5a5a0000 w512 6b6b0000`（对照，和以前一样）
    - `blit A11 (faithful 179 dwords) submit=0x0 fence=5 result=1 observed=4 filled 0/512 w0 7c7c0000 w512 7d7d0000`
    - `fence timeout 5 … cp rptr 72 wptr 257 … rbbm 0x800005 rbbm3 0x20000 fault 0x0`
    - ring dump：`ring words fence 5 start 71 end 257 count 186`，71 处就是 `488e0701 00000068`（`RB_CCU_CNTL`）——**rptr 72 = CP 只读了这个头就停了**
    - `blit status after cp_int 0x1 … rbbm_status 0x800005`；`cp state … rb_ccu 0x0`（写没落地）；`blit hlsq-flush skipped (CP already stopped)`；客户端 FAILED，但系统继续启动到 244 s（528 KB 日志）
  - **结论（事实 + 规范）**：`cp_int 0x1` 是 `A6XX_CP_OPCODE_ERROR`（`a6xx_reg.h` bit 0）。`488e0701` 的 bit 27 parity 与 `AdrenoPM4Type4(0x8e07,1)` 算出的 `408e0701` 不一致；A0 能退休是因为它的头恰好都对。也就是说 metal76 的 A11 **没走到 A2D，也没走到 CCU clean**；metal64 那次 A6 停的也是同一个坏包，所以“`CCU_CLEAN` 等 RB”这个旧结论**不成立**。
  - 补充（Mesa 9a67f003）：A7XX 的 `RB_CCU_CNTL` 是静态寄存器，写完后**需要 WFI** 才生效；客户端流里没有紧跟的 WFI。
  - **metal77 做法**：重放前用 `AdrenoPM4Type4` 逐个重建 type-4 头（payload 不变），并在 `0x8e07` 后插 `CP_WAIT_FOR_IDLE`（`0x70268000`）。日志行改成 `blit A11 (canonical %u dwords, ccu-wfi) …`。
  - 判读：
    - `cp_int 0x0` + `filled 512/512` → 重放本身没问题，A2D 能写；之前是坏头/缺 WFI，下一步接客户端；
    - `cp_int 0x0` + `filled 0/512` → 状态不是原因，A2D 仍不写，转 3D/compute；
    - `cp_int 0x1` → 还有别的非法包，ring dump 的停点会指出来；
    - 停在 WFI（rptr 在 `0x70268000`）→ RB/CCU 本身没就绪，回头查 CCU 时钟/电源。
  - 产物：
    - 内核 `kernel.development.liuqin` 17,090,880 B `59d5c228521d07d528f5528b229925c7f7b01e1e2985d61c6da2a9f837856bf7`；
    - payload `shim/out/payload-metal77-canon.bin` `d108a0ab2e455d1ef21949d2c2bc3f7db97c0a5aab59d18ef77b9ce2edc2a179`；
    - 引导镜像 `shim/out/shim-boot-metal77-canon.img` 32,071,680 B `7c039475ba7f7b05dd8be659c78dc9c90a778e4b0fd2e9a46358ea6896723f1f`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 334 步：metal75 的完整重放**把 CP 挂死**，但挂点很精确：`rptr 82`、ring dump 停在 `48ab1f01 01fffeff`（`SP_UPDATE_CNTL`）——**CP 会在这个寄存器写上等 SP**。metal76 = 同一条重放，但剔掉所有“要等引擎”的包（`SP_UPDATE_CNTL`、四个 cache 事件、两条 CCU clean），只留纯状态写 + A2D 块，用来回答“是不是状态让填充不落的”**
  - **metal75 上机事实**（`0014.txt` 517,667 B `c08e209f6cb478d0ab4238f06c8c41a818b1c9c4eaace38f7ba3441291fe4d19`；镜像 metal75 `a932b104…`）：
    - `blit A0 … filled 0/512`（对照）
    - `blit A11 (faithful 191 dwords) submit=0x0 fence=5 result=1 observed=4 filled 0/512` → 超时
    - `fence timeout 5 … cp rptr 82 wptr 269 … rbbm 0x800005`、`cp_int 0x1`
    - ring dump：块从 71 起，CP 读到 **81** 就停；71..81 正是 `CCU_INVALIDATE_DEPTH/COLOR`、`LRZ_CACHE_INVALIDATE`、`CACHE_INVALIDATE7` 四条事件 + `SP_UPDATE_CNTL=0x01fffeff`
    - 之后 `hlsq-flush` 被跳过（CP 已停），客户端整段失败（日志只有 517 KB）
  - **结论**：`HLSQ_FLUSH` 不等引擎（metal74 退休），但 **`SP_UPDATE_CNTL` 会等 SP**。`CCU_CLEAN` 那半条已被第 335 步推翻：metal64 的 CP 其实死在更早的坏 parity 包上，没走到 CCU clean。也就是说“引擎不回话”目前只剩 SP_UPDATE 这一条直接证据，RB/A2D 还要等 metal78。
  - **metal76 做法**：仍用客户端 152 dword 的纯状态前导（去掉 `SP_UPDATE_CNTL`、4 个事件、thread-control），在 A2D 块前补上客户端那一句 `CP_THREAD_CONTROL 0x08000001`，A2D 块之后**不再发 CCU clean**；目的地址仍是自己的 scratch 页（颜色 `0xdeadc0f5`）。
  - 判读：
    - `A11 … result=0 filled 512/512` → **纯状态就够了**，A2D 通路可用；之前挂住只是那些同步包。接下来把 CCU clean 换成不等引擎的清理方式（如 `CACHE_CLEAN` 0x31），就能让客户端那条链也走通；
    - `A11 … result=0 filled 0/512` → 状态不是原因，A2D 本身在这块板上不出写事务 → 转 3D/compute 通路（**注意**：metal76 去掉了 CCU clean，0/512 也可能是写进了 CCU 没刷回，下一版先用不等引擎的 `CACHE_CLEAN` 0x31 区分）；
    - `A11 … result=1` → 说明状态块里还有别的同步包，下一轮继续按 ring dump 的挂点裁。
  - 产物：
    - payload `shim/out/payload-metal76-state.bin` `fea4ec5b7051f44f3338d4618615088b58484fd36557d175df4a63f9e494f9dc`；
    - 引导镜像 `shim/out/shim-boot-metal76-state.img` 32,071,680 B `bc7b3355e17cb72828500b3deaac2a4ca8fe53935dfa11bbddf2d6faf0b221c4`；
    - **root 不变**（gpu11 `f7043229…`）。
  - **本轮准备（主机侧事实，未上机）**：
    - 复核 metal76 镜像：payload `fea4ec5b…` 存在；镜像内嵌 cmdline 与 `tools/boot-cmdline-gpu.txt` 逐字节一致（603 B，含 `sheng-gpu-blit=1`）；payload 内含 `blit A11 (faithful` 字符串。
    - Android 侧源码核对（Xiaomi `liuqin-t-oss`，Makefile 5.10.81）：这版 kgsl **没有** `registers` debugfs（只有 `globals` + `snapshot/break_isdb`），寄存器快照只能来自真 fault 后的 devcoredump。据此新增只读采集脚本 `tools/gpu-oracle-android.sh` + `tools/liuqin-gpu-oracle-pull.sh`；**未在设备上跑过**。
    - 源码对比（事实）：同族 a6xx 的 `a6xx_start` 写了 `*_ADDR_MODE_CNTL=1` 一整组、`CP_ROQ_THRESHOLDS_1/2`、`CP_MEM_POOL_SIZE=128`、`UCHE_FILTER_CNTL=0x804` 等；我们的 `startCp` 没有这些（这棵树里没有 a730 专属文件，所以只是同族参考）。**推断（未验证）**：这是“CP 活、SP/RB/A2D 不执行”的一个候选差异，metal77 再按单组加。

- **2026-10-31 GPU 第 333 步：metal74 上机 = **配置稳住了**（进系统、日志 1,282,362 B），并且 `HLSQ_FLUSH` 的 fence **正常退休**（`blit hlsq-flush … result=0`）→ "引擎从不回话"这个结论**不再成立**（metal64 那次 CCU_CLEAN 挂住，更可能是我们漏了 `RB_CCU_CACHE_CNTL=0x80000000` 等 CCU 配置）。metal75 = 把客户端那 162 dword 的寄存器前导**逐条重放** + A2D 块 + 两条 CCU clean，目的地址换成自己的页**
  - **metal74 上机事实**（`0005.txt` 1,282,362 B `7a1629e1273ef2bfef29e5c6cd19f04fc553f00a66af47a67398593efba7e03a`；镜像 metal74 `f87a36ed…`）：
    - `CP ME_INIT power-up list on iova 0x301000 flag 0x80000000`、`framed fence leaves IFPC disabled (sheng-gpu-ifpc=0)` —— 注意后者这行文字仍是旧的，实际 marker 已是 0x100（metal72 改回来了），**文字待改**；
    - `blit status before/after cp_int 0x0 hwfault 0x0 rbbm_int0 0x0 prot 0x0 rbbm_status 0x1 gbif_halt 0x0/0x3 rbbm_gbif_ack 0x0` —— 全程干净；
    - `blit A0 … filled 0/512`（对照，没变）；
    - **`blit hlsq-flush submit=0x0 fence=5 result=0 observed=5`** → HLSQ 那条事件 fence 退休；
    - 客户端照旧 `T s0 r0 v512 c0 g0/4096`、`M s0 c999 r64 i4096`，整机正常（1.28 MB 日志）。
  - **metal75 做法**：新增 `blit A11`：
    1. 把客户端流里 0x55/0x39/0x43/0x1b/0x17（amble、reg-test、thread-control）之外的**全部寄存器写**按原顺序拼成 `turnipFillPreamble[162]`（含 `RB_CCU_CNTL=0x68`、`RB_CCU_CACHE_CNTL=0x80000000`、`RB_MODE_CNTL=0x10`、`SP_MODE_CNTL=5`、`UCHE_CACHE_WAYS=0x840004`、`UCHE_GBIF_GX_CONFIG=0x3200000`、`PC_DBG_ECO_CNTL=0x20080000`、TPL1 bicubic 权重表等）；
    2. 之后接 A2D 块（6 条 A2D 寄存器 + `CP_BLIT op 3`），目的地址 = 自己的 scratch 页，颜色 `0xdeadc0f5`；
    3. 再接客户端那两条 `CCU_CLEAN_COLOR/DEPTH`；
    4. 预填 `0x7c7c0000+i` / `0x7d7d0000+i`，fence 退休后 `dc ivac` 读回，打 `blit A11 (faithful %u dwords) … filled n/512`。
  - 判读：
    - `A11 … filled 512/512` → **只差状态**，客户端失败的原因就在它还写了别的东西（下一步把 amble/thread-control 也放进去）；
    - `A11 … 0/512` 且 `result=0`（fence 正常）→ 状态不是原因，A2D 这条通路本身在此板子上不成 → 转 3D/compute 通路（客户端 `cs 1` 的那条流）或重新审视 CCU/UBWC；
    - `A11 … result=1`（CCU clean 又挂住）→ 说明 CCU 那条同步事件在**这套配置下**也会卡，回头查 CCU 配置。
  - 产物：
    - payload `shim/out/payload-metal75-faithful.bin` `ae2ce3f031465d3f4405a3b9e7a7dae782c8381e18ae711ceb70eb57e85bc8d0`；
    - 引导镜像 `shim/out/shim-boot-metal75-faithful.img` 32,071,680 B `a932b1043d11cee310ca6d9a6728f0b35424a6dc7f62e9af57a9d8f9a4c19cb6`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-31 GPU 第 332 步：开挂重启的**元凶找到了：`CP_SET_MARKER` 0x101（保持 IFPC 关闭）**。metal72（三处全回退）=进系统；metal73（只开 power-up 表）=进系统 → 排除 ①；剩下 ② 就是它。结论：**这块板子上"fence 末尾重新开 IFPC（0x100）"是必需行为**，照抄上游 a7xx_submit 的"永不重开"会把机器搞成重启循环（无 panic 画面）。power-up 寄存器表本身是安全的，默认保持开启。metal74 = 稳定配置（pwrup 开、IFPC 0x100、探针重新打开）**
  - 事实链（全部只用"能进系统 / 重启"一个比特，没有日志）：
    - metal70 / metal71-safe（探针关）→ **重启**；metal65 → 进系统；
    - metal72（①pwrup 关 + ②marker 0x100 + ③sid 覆盖删除 + 探针关）→ **进系统**；
    - metal73（在 metal72 基础上只开 ①pwrup）→ **进系统** → ① 安全；
    - 因此 ②（fence 的 `CP_SET_MARKER` 从 0x100 改成 0x101）就是 reboot 的原因。
  - 影响：`NativeAdrenoRing::emitFramedFence` 的 marker 永久回到 **0x100**（`sheng-gpu-ifpc=1` 才用 0x101）；`CP_ME_INIT` 的 power-up 表默认**开**（metal73 已验证）。
  - 附带修正一个之前的误判：metal70 的 reboot **不是** `HLSQ_FLUSH` 引起的（那次探针根本没跑），所以引擎活性测试可以在稳定配置下重试。
  - **metal74 做法**：稳定配置 + `sheng-gpu-blit=1` 重新打开探针，内容依次是：
    1. `status("before")`（cp_int/hwfault/rbbm_int0/prot/gbif_halt）；
    2. `A0` 填充自己的 scratch 页（对照，预期 0/512）；
    3. **`HLSQ_FLUSH`（事件 0x07）单包** —— 引擎会不会回答：fence 退休=会答，超时=不答；
    4. `status("after")`；A7/A8/A9 二分与 sid 覆盖都关。
  - 判读：
    - 能进系统 + `blit hlsq-flush … result=0` → **引擎有应答**，问题在写通路；
    - 能进系统 + `result=1` → 引擎不跑；
    - 又重启（说明 HLSQ_FLUSH 确实会挂死）→ 下一轮只留 A0，改用不阻塞的办法探。
  - 产物：
    - payload `shim/out/payload-metal74-stable.bin` `db2659866c08fc8fb0b324b7ea78063cbb696bf7c9a2c21dbc5b4c8e0eef2c40`；
    - 引导镜像 `shim/out/shim-boot-metal74-stable.img` 32,071,680 B `f87a36ed5b486eed9cf5c0482eaca1aabc24ee29bd54a2dec83417ae076cea99`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-30 GPU 第 331 步：`metal71-safe`（探针全关）**也重启** → 探针不是元凶（那次探针根本没跑）。于是把 metal64 之后的三处非探针改动**全部回退到 metal65 的行为**（metal65 是最后一张确认能进系统的镜像），出 metal72-revert 做一次单比特判定**
  - 事实：
    - metal70 / metal71-safe 都是 **ladder 结束后立刻重启**，无 panic 画面、无日志；metal65（镜像仍在 `shim/out/shim-boot-metal65-ccu.img`）能进系统；
    - `metal71-safe` 的 cmdline 带 `sheng-gpu-blit=0`，**填充/FLUSH/SMMU 探针一行都不会执行**，仍然重启 → 排除探针；
    - 因此嫌疑收窄到 metal64 之后的三处非探针改动：① `CP_ME_INIT` 开始传 power-up 寄存器表；② fence 不再重新打开 IFPC（`CP_SET_MARKER` 0x100→0x101）；③ route 块新增的 `g_smmu_route` 发布 / `sheng-gpu-sid` 覆盖（后者已删）。
  - **metal72 做法（全部回退，单比特判定）**：
    1. `sheng-gpu-pwrup` 默认改为 **0**（不传表，回到 metal65 行为）；
    2. fence 的 marker 默认改回 **0x100**（重新允许 IFPC，`sheng-gpu-ifpc=1` 才用 0x101）；
    3. `sheng-gpu-sid` 覆盖代码整段删除；
    4. 探针默认改为**关**（`sheng-gpu-blit` 默认 0），cmdline 回到 586 B。
    → 行为上等同 metal65（除“不跑探针”），预期能进系统。
  - 判读：
    - **能进系统** → 元凶就在①②之中；下一轮只打开 power-up 表（`sheng-gpu-pwrup=1`），再下一轮只打开 IFPC 保持 0x101（`sheng-gpu-ifpc=1`），两轮内定位；
    - **仍然重启** → 说明不是这三处，也不是探针（metal65 与 metal72 行为已等价）→ 要怀疑刷机/镜像链路，先用 `shim-boot-metal65-ccu.img` 复验一次设备是否仍能进系统。
  - 产物：
    - payload `shim/out/payload-metal72-revert.bin` `2fbe94dc0e6c8091782e4d834a3b815c364301dc83a0a24327264ae7d2b78286`；
    - 引导镜像 `shim/out/shim-boot-metal72-revert.img` 32,071,680 B `a16bfa2f11f8c1900989f2c34e6476dfe6f86b90f62f875dedfc152b798a667d`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-30 GPU 第 330 步：metal69 **第四次 panic**——扫 SID 时第一次（候选 2）跑完 `filled 0/512`，**第二次**往同一个 SMR 槽写（值 `0x80000003`，mask 0，和 route 同一种写法）就同步外部中止。反汇编确认 panic pc 落在 `str w8,[x21,w10,uxtw #2]`（w8 = `VALID|sid`，偏移 `0x800+slot1*4`）。结论：**在这块板上，CP 起来之后再反复写 SMR 不安全**。metal70 彻底停掉探针里的 SMMU 写，改用**一个包就能问出"引擎到底答不答"的 HLSQ_FLUSH 事件**
  - **metal69 上机事实**（panic 屏照片 `0121e1ba…`；镜像 metal69 `d12a6852…`）：
    - `GPU route published smmu 0x… slot 2…`（route 正常）、`blit A0 … filled 0/512`、`blit S2 mode 0 … filled 0/512`（**候选 SID 2 已测：没有让填充出现**）
    - 紧接着 `Kernel data abort at pc 0xfffffe0007abb9e0, lr 0xfffffe0007abb9e8`，`esr 0x96000010`
    - `llvm-objdump`：pc 处就是 `str w8,[x21,w10,uxtw #2]`（SMR 写，w27=sid、`orr 0x80000000`），前一条 `ldr w9,[x25,#3768]` 取的是 `g_smmu_slot1`
    - 即：**写 `0x80000002` 成功、写 `0x80000003` 中止**，两次之间只隔了一次 A2D 提交
  - 结论：
    1. SID 2 不是引擎用的 SID（至少用它做 CB2 翻译时填充仍为 0）；
    2. 从 CP 起来之后的 SMMU SMR 写不可靠（会 panic），所以**"逐候选扫 SID"这条路作废**——以后只在 route 块里写一次，需要换 SID 就 `sheng-gpu-sid=<n>` 重刷；
    3. 四次 panic 全部是我的探针造成的（两次垃圾指针、一次 mask SMR、一次重复 SMR 写），不是新的 GPU 故障。
  - **metal70 做法**：
    1. 探针里**再没有任何 SMMU 写**（扫 SID 循环删除，只留 route 块的写入 + 可选的 `sheng-gpu-sid=<n>` 单次覆盖）；
    2. 新增**引擎活性测试**：只发一个 `CP_EVENT_WRITE(0x46) HLSQ_FLUSH (0x07)` 包，看 fence 是否退休。metal64 的 `CCU_CLEAN` 挂住了 CP（CP 把事件交给 RB 等回答、等不到），HLSQ_FLUSH 是同一形状但只花一个包：
       - fence 退休 → **引擎会回答**，写丢失发生在写通路（下一步查 UCHE/GBIF 的写事务）；
       - fence 挂住 → **引擎根本不跑**（与 CCU_CLEAN 一致），下一步转为查 GX 电源/复位。
    3. 日志：`blit hlsq-flush submit=… fence=… result=… observed=…`。
  - 判读：
    - `result=0`（退休）→ 引擎有应答，转写通路；
    - `result=1`（超时）→ 引擎不跑，转 GX 电源/复位（下一步：整机 `RBBM_SW_RESET` 周期 + 重新 init，或从 GMU 侧确认 GX 上电状态）。
  - 产物：
    - payload `shim/out/payload-metal70-hlsq.bin` `008a636c869a3668175970b34bfc6645bf74802d7c8a1757b08661bcb26e67f5`（`hlsq-flush` 2 处、`sid override` 1 处、`mask wildcard` 0 处，已核对）；
    - 引导镜像 `shim/out/shim-boot-metal70-hlsq.img` 32,071,680 B `98e8f2f4e360dabe34e2a578e09c5412913171660e5604fad159a97aa05b02c7`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-30 GPU 第 329 步：metal68 **第三次 panic**，但这次落点和证据都干净——不是指针问题，是**这条 qsmmu 不接受带 MASK 的 SMR**：同一个地址上，route 用 `0x84000001`（mask 0）写成功并读回，紧接着写 `0x801f0001`（MASK=0x1f）就同步外部中止。metal69 放弃通配，改成**在一个 boot 里逐个候选 SID 扫过去**（mask 0，硬件一直接受的写法）**
  - **metal68 上机事实**（panic 屏照片 `62e2d8cd…`；镜像 metal68 `12765e89…`）：
    - panic 前的日志正好停在关键两行：`GPU context status 0x0 step 7 (bank 2 asid 2 slots 1/2 sids 0/1)`、`GPU route smr1=0x84000000 s2cr1=0x2 smr2=0x84000001 s2cr2=0x2`
    - `Kernel data abort at pc 0xfffffe0007abe2c4, lr 0xfffffe0007abe2bc`，`esr 0x96000010`，`far 0xfffffe5039339800`（**在 SMMU 窗口内**，不再是垃圾指针）
    - 反汇编：`ldr w8,[sp,#184]`（gslot1）→ `mov w9,#0x801f0001` → **`str w9,[x22,w8,uxtw #2]`**（x22 = `smmu_base`，偏移 = `0x800 + gslot1*4`）→ `str w8,[x22,w9,uxtw #2]`（S2CR）
    - 同一地址、同一次 boot 内，route 刚用 `0x84000001` 写成功并读回 `smr2=0x84000001` → **差别只在值**：带 MASK 的 SMR 写会中止。结论：masked stream matching 在这颗 qsmmu 上不可用（与 ARM 的 IDR0.SMRSM 无关，实测就是不接受）。
  - **metal69 做法**：
    1. 去掉所有 mask 写法；
    2. route 块把**证明可写**的 `smmu_base` 和一个文件静态指针 `g_smmu_route`（外加 `g_smmu_slot1`）发布出来，供后面的探针用——不再用 `native_adreno_smmu_window`（那个在 `startCp` 里是垃圾，已经 panic 两次）；
    3. 探针在一个 boot 内**扫 SID**：保持槽 0（GMU，SID 5）与 `gslot0`（SID 0，CP）不动，把 `gslot1` 依次写成候选 `2,3,4,6,7,8,9,10,11,12,13,14,15,16,17`（`VALID|ID`，mask 0，S2CR=2 → CB2），每个候选跑一次 A2D 填充并检查 `filled`，命中（512/512）即停；扫完把 `gslot1` 恢复成 SID 1；
    4. 日志：`GPU route published smmu 0x… slot 2 for the sid sweep`、每个候选一行 `blit S<n> mode 0 … filled n/512`、结尾 `blit sid sweep tried N hit M`。
  - 判读：
    - 某个 `blit S<k> … filled 512/512` → **引擎用的就是 SID k** → 把 SID k 固定进 route（替换掉 SID 1 那格），这一关收工；
    - 全部 `filled 0/512` → SID 这条排除（16 个候选全扫过），剩下只能是“引擎块被硬件按在复位/断电”，下一步从 GMU 侧查 GX 电源或做完整 `RBBM_SW_RESET` 周期 + 重新 init。
  - 产物：
    - payload `shim/out/payload-metal69-sidsweep.bin` `8e546a3aa9200839373897e365d5c34d7789ea10315404040456e05a5b2fb910`（新串 `sid sweep` / `published smmu` 已核对，`mask wildcard` 已消失）；
    - 引导镜像 `shim/out/shim-boot-metal69-sidsweep.img` 32,071,680 B `d12a685261257b141507c34724d3feb93a8a0cb061105145c57c5cc39f5324db`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-30 GPU 第 328 步：metal67 **又 panic**，同一个根因：探针里 `native_adreno_smmu_window` 这个全局在 `startCp` 里是**不可用的指针**（它的赋值在 `start()` 里，执行序上晚于探针）。符号化确认 panic 指令就是 `str w9,[x8,#0x808]`，x8 来自该全局。metal68 = 通配搬到 **GPU route 块**（那里的 `smmu_base` 是证明可写的本地指针），探针**完全不碰 SMMU****
  - **metal67 上机事实**（panic 屏照片 `9c4c5ab0…`；镜像 metal67 `0cb1b43c…`）：
    - panic 前正常：`blit status before … gbif_halt 0x0/0x3 rbbm_gbif_ack 0x0`、`blit A0 mode 0 … filled 0/512`
    - `panic(cpu 0 caller 0xfffffe0007b3949c): Kernel data abort at pc 0xfffffe0007abb9e0, lr 0xfffffe0007abb9c8`，`esr 0x96000010`
    - `llvm-objdump` 反汇编该处：`ldr x8,[x24]`（x24 = `&native_adreno_smmu_window`）→ `mov w9,#0x801f0001` → **`str w9,[x8,#2056]`** → `mov w9,#2` → `str w9,[x8,#3080]`。即“写 SMR2 / S2CR2”，而 x8 指向的不是 SMMU 窗口
    - metal66 的 panic 是同一条路径（那片代码的循环版本），两次 far 都落在非 SMMU 区域且每次不同 → 该全局值是垃圾
    - **重要连带结论**：metal62–65 里那几行 `blit smmu gfsr/fsr/far …`（`far=0xd5fe77b6 sid=0x1b08`）也是通过同一个指针读的 → **"未映射目的地址没有 fault" 这个结论降级为"未验证"**。仍然可信的 SMMU 事实只有：route 块用 `smmu_base` 写/读到的 SMR/S2CR（metal61 审计：SID 0/1 → CB2、`unmatched 0`）、以及"CP 的写能落、引擎的写不落"。
  - **metal68 做法**：
    1. 通配搬进 GPU route 块（`smmu_base`，同一个块刚写过 SMR1/S2CR1 成功），仍然只写**已实现**的槽：`SMR(gslot1) = VALID | MASK(0x1f)<<16 | ID 1`、`S2CR(gslot1) = gpubank`，日志 `GPU route mask wildcard smr%u=0x%x s2cr%u=0x%x (all SIDs 0..31 -> CB%u)`；`sheng-gpu-sidwild=0` 可关；
    2. 探针里删掉所有 SMMU 访问（通配块 + 末尾的 `blit smmu …`），只留 A0 / A10（通配后填自己的页）/ B10（通配后填未映射地址）；
    3. A7/A8/A9 二分仍只在 `sheng-gpu-bisect=1` 时跑。
  - 判读：
    - `GPU route mask wildcard smr2=0x801f0001 s2cr2=0x2 (all SIDs 0..31 -> CB2)` → 通配生效；
    - `A10 … filled 512/512`（或 1..511）→ 引擎用的确实是别的 SID 且已通；`B10` 引起客户端路径的 `smmu fault … far=0x3ff00000`（那条是 `NativeAdrenoSubmit` 在 SMMU 编程之后读的，可信）→ 同样说明 SID 是根因；
    - 两者都没变 → SID 这条也排除，只剩“引擎块被硬件按在复位/断电”，下一步从 GMU 侧查 GX 电源，或做完整 `RBBM_SW_RESET` 周期 + 重新 init。
  - 产物：
    - payload `shim/out/payload-metal68-sidmask2.bin` `7a03bfcebeab965be1ebac4081fc5554d327f4ccdc0b6cb6f1e6f671ce4a2224`（新串 `GPU route mask wildcard` 已核对）；
    - 引导镜像 `shim/out/shim-boot-metal68-sidmask2.img` 32,071,680 B `12765e891802240103ca29598531717f3e79e7f7dd91bbd315c118e30fcc4f30`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-30 GPU 第 327 步：metal66 **panic**——是我的探针写坏了：SMMU 只实现 SMR/S2CR 的 0..2 号槽，我那个“写 1..15 号槽”的通配循环在**第一个未实现的槽**上同步外部中止（`esr 0x96000010`），内核直接 panic。metal67 改成**只用一个已实现寄存器**：SMR2 的 `SMR_MASK=0x1f` 一条就覆盖 SID 0..31**
  - **metal66 上机事实**（用户提供的 panic 屏照片 `bf584b50…`；镜像 metal66 `ca258d37…`）：
    - panic 前的最后两行正常：`blit status before cp_int 0x0 hwfault 0x0 rbbm_int0 0x0 prot 0x0 rbbm_status 0x1 gbif_halt 0x0/0x3 rbbm_gbif_ack 0x0`、`blit A0 mode 0 submit=0x0 fence=4 result=0 observed=4 filled 0/512`
    - `panic(cpu 0 caller 0xfffffe0007b39530): Kernel data abort. at pc 0xfffffe0007abba80, lr 0xfffffe0007abb9c0`，`esr: 0x96000010`（同步外部中止），`far: 0xfffffe5038b704c0`
    - 用 `atos` 对 `kernel.development.liuqin.unstripped` 解析：pc → `NativeAdrenoGpuService::startCp` `NativeAdrenoService.cpp:862`，lr → 同函数 `:861`，即通配循环里的两条 store（`sm[0x800/4+slot]`、`sm[0xc00/4+slot]`）
    - **结论**：这不是新的 GPU 故障，是我的探针越界写 MMIO；与 metal49 的 `RBBM_SECVID_TRUST_CNTL` 同一种中止形状。**读** 1..15 号槽是安全的（metal60/61 都读过），**写**不安全。
  - **metal67 做法**：只写 2 号槽——`SMR2 = VALID | MASK(0x1f)<<16 | ID 1`，`S2CR2 = 2`（CB2 翻译）。SMMU-500 的匹配条件是 `(sid & ~MASK) == (ID & ~MASK)`，`MASK=0x1f` 即“忽略低 5 位”→ 匹配 SID 0..31；槽 0（GMU，SID 5→CB1）与槽 1（SID 0 精确匹配→CB2）按索引优先，不受影响。日志一行 `blit smmu mask wildcard (smr1 … smr2 … smr3 … s2cr2 …)`。
  - 判读（与 metal66 完全一样的目标，只是这次不会 panic）：
    - `A10 … filled 512/512` 或 1..511 → 引擎用的确实是别的 SID 且已通；
    - `B10` 后 `blit smmu … far 0x3ff00000` → 引擎发了事务且现在能到 SMMU；
    - 两者都没变 → SID 这条也排除，只剩“引擎块被硬件按在复位/断电”，下一步从 GMU 侧查 GX 电源或做完整 `RBBM_SW_RESET` 周期 + 重新 init。
  - 产物：
    - payload `shim/out/payload-metal67-sidmask.bin` `fa785df94bb91f9e190cad27f692a7ed3b36a2da0a7c0a7694cc0f0d43b90208`（新串 `smmu mask wildcard` 已核对）；
    - 引导镜像 `shim/out/shim-boot-metal67-sidmask.img` 32,071,680 B `0cb1b43cb3f38ed22bbf2be7eae536b4ab59ec7ee624e74b51a5f70a3f8a29e8`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-30 GPU 第 326 步：metal65 上机——**HWCG 关掉也没用**（引擎照样不跑），`RBBM_SW_RESET_CMD` 在 a730 上是 `0x73`（`0x43` 读到 `0xdeadbead`，不是寄存器；`0x73` 本来就是 0，没被按在复位里）。而我的 A7/A8/A9 二分因为跳过条件写错（`if(f||seen)`，成功时 `observed` 非 0 也算"死"）**一条都没跑**。metal66 修掉这个 bug，主攻**最后一种能解释全部测量的假设：引擎块用的 StreamID 不在我们匹配的两条里，被 qsmmu 静默 abort**——把 SMR1..15 全部登记成 SID 0..4、6..15 → CB2**
  - **metal65 上机事实**（`2320.txt` 1,215,833 B `ab2b331f02bd90a8dfb78ff0a970974774e7410c95446d78bd47f630b485947a`；镜像 metal65 `19d29140…`）：
    - `HWCG skipped (sheng-gpu-hwcg=0)`、`CP ME_INIT power-up list on iova 0x301000 flag 0x80000000`、`CP self-test … wrote=0x600dc0de`（重建后的那段代码行为正确）
    - `blit reset deassert 0x43 deafbead->deafbead 0x73 00000000->00000000` → `0x43` 不是寄存器（读回 `0xdeadbead`），`0x73` 是 `RBBM_SW_RESET_CMD` 且本来就是 0
    - `blit A0 mode 0 … filled 0/512` → **HWCG 关掉不影响结果**
    - `blit A7/A8/A9 skipped (CP already stopped)` → 我的跳过条件写错（`if(f||seen)`：成功时 `f==0` 但 `seen` 非 0），二分没跑
    - 客户端照样 `T s0 r0 v512 c0 g0/4096`、`M s0 c999 r64 i4096`，fence 超时 4 次（1/11/12/16），整机正常跑到 SpringBoard
  - 结论（排除列表再加两条）：**HWCG/时钟门控不是根因**；**RBBM 软复位不是根因**（寄存器本来就是 0）。剩下的唯一未排除假设：**引擎主用的 StreamID 没有被匹配**。之前"SID 0/1 都匹配上了"只证明**SID 0/1** 匹配上了——CP 的写走的就是其中之一；引擎块可能有自己的 SID（`smmu fault … sid=0x1b08` 这条陈旧记录一直在提示"某个别的 stream"存在）。
  - **metal66 做法**：
    1. 修 `dead` 判定为 `f!=NativeAdrenoRingFenceComplete`；
    2. **SMMU 通配**（`sheng-gpu-sidwild=1` 默认开）：槽位 0 保持 GMU（SID 5 → CB1），槽 1..15 写成 SID `0,1,2,3,4,6,7,…,15`、`S2CR=2`（CB2 翻译），全部精确匹配（mask 0），然后读回 slot1/5/15 打一行 `blit smmu wildcard 15 slots (…)`；
    3. `A10` = 通配后再填自己的 scratch 页（`0xdeadc0d0`）→ 若引擎用的 SID 现在是匹配的，这一格就该出现；`B10` = 通配后填未映射的 `0x3ff00000` → 引擎真的发了写事务就该出 `far=0x3ff00000`；
    4. A7/A8/A9 二分改成只在 `sheng-gpu-bisect=1` 时跑（默认关，避免把 CP 打死、毁掉客户端那半段日志）。
  - 判读：
    - `A10 … filled 512/512`（或 1..511）→ **引擎用的确实是别的 SID**，而且已经通了 → 把那条 SID 固定进 route，收工这一关；
    - `B10` 出 `far=0x3ff00000` → 引擎发了事务且现在能到 SMMU → 同样说明 SID 是根因；
    - 两者都没变、`smmu wildcard` 那行也正常 → SID 这条也排除，剩下的只能是"引擎块被硬件层面按在复位/断电"（下一步用 GMU 侧查 GX 电源，或做一次完整 `RBBM_SW_RESET` 周期 + 重新 init）。
  - 产物：
    - payload `shim/out/payload-metal66-sidwild.bin` `e2f50194a8515b9b987c01d802b2b0f8c9ba46756f41d67793bebc769bcc688f`（新串 `smmu wildcard %u slots` / `sheng-gpu-sidwild` / `sheng-gpu-bisect` 已核对）；
    - 引导镜像 `shim/out/shim-boot-metal66-sidwild.img` 32,071,680 B `ca258d3765861e170667f6de06bd42e6699ac4a26ee14de7a1dd9644a1a13a74`；
    - cmdline 去掉 `sheng-gpu-hwcg=0`（回到标准 HWCG，586 B）；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-30 GPU 第 325 步：metal64 的两处“上游对齐”都没用（power-up 表已交给 CP、IFPC 不再重开，`A0` 仍 `0/512`），但**忠实版探针 A6 把 CP 打停了**——`CCU_CLEAN` 这类事件 CP 自己完不成，必须交给 RB 并等回音。**所以 CP 是分发下去的，是引擎从不回话**。metal65：关掉 HWCG（时钟门控假设）、deassert RBBM 软复位（复位假设）、并把 A6 那 5 个 dword 拆成 A7/A8/A9 逐条二分**
  - **metal64 上机事实**（`2309.txt` 463,307 B `10b9a355cd5448cf9c28bf706e1e0d073623c6403fb3e3584052fbc6bb09044c`；镜像 metal64 `9029b5c2…`）：
    - `CP ME_INIT power-up list on iova 0x301000 flag 0x80000000`、`framed fence leaves IFPC disabled (sheng-gpu-ifpc=0)` → 两处改动都生效了
    - `blit status before cp_int 0x0 hwfault 0x0 rbbm_int0 0x0 prot 0x0 rbbm_status 0x1 gbif_halt 0x0/0x3 rbbm_gbif_ack 0x0`
    - `blit A0 … filled 0/512`（对照，和之前一致）→ **power-up 表 + IFPC 两处都不是根因**
    - `blit A6 submit=0x0 fence=5 result=1 observed=4` → **CP 被打停**：`fence timeout 5 … cp rptr 72 wptr 111 … rbbm 0x800005 rbbm3 0x20000`、`cp_int 0x1`；此后所有提交超时、`client=FAILED`、日志只有 463 KB
    - 停在那条流的第一条包上：`ring 488e0701 00000068 …`（`RB_CCU_CNTL` 写）之后就是 `70970001 08000001 …` 和 `70460001 00000021/00000020`（两条 CCU clean）
    - `cp state … rb_ccu 0x0` → 写进去的 `RB_CCU_CNTL=0x68` 读回来是 **0**（要么 0x8e07 在 a730 上不是这个寄存器，要么写被丢）
    - 新事实：`GBIF_HALT=0` 但 `GBIF_HALT_ACK=3`，而 `RBBM_GBIF_HALT_ACK(0x17)=0` → 3 那两位不代表"GX 侧被 halt"
  - **结论（本轮最重要的一条）**：`CCU_CLEAN_COLOR/DEPTH` 是**同步事件**，CP 无法自己完成，必须下发给 RB 并等它答。它一挂住，说明 **CP 确实把活发出去了，是引擎（RB）从不回话**。所以问题不在 CP 的分发逻辑，而在引擎块本身不跑；`CP_BLIT` 之所以"静默"，只是因为它不是同步包。
  - **metal65 三件事**：
    1. **cmdline 加 `sheng-gpu-hwcg=0`**（603 B，仍在 607 B 以内）——关掉 a730 的 53 条 HWCG 时钟门控，验证"块被时钟门控住、永远不 ungated"。寄存器可读写但引擎不跑，正是门控住的形状。
    2. **deassert RBBM 软复位**：读 `RBBM_SW_RESET_CMD`（mesa 里 0x43 与 0x73 两个地址都算过）后写 0 再读回，日志一行 `blit reset deassert 0x43 … 0x73 …`。若 bootloader 把块按在复位里，写 0 就是释放。
    3. **二分 A6**：`A0`=旧流、`A7`=+`RB_CCU_CNTL` 只有这一条、`A8`=+`CCU_CLEAN_COLOR`、`A9`=+`CCU_CLEAN_DEPTH`；一旦某条超时就跳过后面（CP 已死，再跑没意义），每条都打 `blit %s mode %u …` 和 `cp_int`。
  - 判读（下一份日志）：
    - `HWCG skipped (sheng-gpu-hwcg=0)` + `A0 … filled 512/512` → 时钟门控是根因；
    - `blit reset deassert 0x43 00000001->00000000` 之类且 A0 变 512 → 复位是根因；
    - `A7` 超时 → `RB_CCU_CNTL` 那条写本身就把 CP 打死（它不是普通寄存器写）；
    - `A8`/`A9` 超时 → 确认是 CCU clean（同步事件）→ 引擎不回话这条成立，下一步就专攻"引擎为什么不跑"（复位/时钟/GX 上电）。
  - 过程记录（诚实）：本轮改代码时我的替换脚本锚点写错，把 `startCp` 里 CP_ME_INIT 之后到自检为止的一大段删掉了；已按上下文逐行重建，编译干净，`CP ME_INIT submit` / `CP self-test` / `CP started` 这些行下一份日志里应照旧出现（可作为重建正确性的验证）。
  - 产物：
    - payload `shim/out/payload-metal65-ccu.bin` `ef9e8e9dfd0412fb69c44fb27ab9ff4056a82dfbf58791612c9c6f29b88cf134`（新串 `blit reset deassert` / `sheng-gpu-hwcg` / `blit %s mode` 已核对）；
    - 引导镜像 `shim/out/shim-boot-metal65-ccu.img` 32,071,680 B `19d2914070bf8142b276ed01467ad62d21c68d01697f4de4ab908f983ad25511`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-30 GPU 第 324 步：metal63 上机——CP **确实执行了**那 6 条 A2D 寄存器写（读回来逐字节正确），`CP_BLIT`/`CP_EXEC_CS` 被接受且**零错误**，但引擎不动、SMMU 收不到任何事务；`CP_SET_SECURE_MODE 0` 直接把 CP 打死（安全世界不放行），这条退出安全模式的路也断了。metal64 修两处与上游 a7xx 的**实质差异**：CP_ME_INIT 从来没把 power-up 寄存器表交给 CP、以及 fence 每提交一次就把 IFPC 重新打开**
  - **metal63 上机事实**（`2256.txt` 531,972 B `3d779ebcd45dbdd5ec876a5eb616521d736d56b86ce6e918bcc9e8aa59006ed1`；镜像 metal63 `d8061d04…`）：
    - `blit status before cp_int 0x0 hwfault 0x0 rbbm_int0 0x0 prot 0x0 rbbm_status 0x1`
    - `blit A0 … filled 0/512 w0 5a5a0000`、`blit A1 … filled 0/512`（去掉 `CP_THREAD_CONTROL` 也一样 → 线程选择不是原因）
    - `blit regs a2d 07f04b80 00000000 dest 0000004b 00302000 colour deadc0e1 gras 07f04b80 00000000 tpl1 20000000 sp 0000f25c` → **CP 把 6 条 type4 写全部执行了**，引擎侧看到的寄存器状态与客户端一致
    - `blit secure-mode-0 submit=0x0 fence=6 result=1 observed=5`（`result=1` = 超时）→ **`CP_SET_SECURE_MODE 0` 把 CP 打死**：`cp rptr` 停在 105、`rbbm_status 0x800005`、`cp_int 0x4002`；此后所有提交（含客户端）全部超时，本份日志只有 532 KB（正常 1.2 MB）
    - `blit B smmu gfsr 0x0 fsr 0x400 far 0xd5fe77b6` → 未映射目的地址仍然**没有** fault（但这次 CP 已死，B 的结论以 metal62 为准）
    - 附带：`CP_INTERRUPT_STATUS` **是活信号**（安全模式包让它置了 0x4002），而合法 `CP_BLIT` 让它保持 0x0 → 微码没有报错
  - **结论**：CP 执行一切、引擎一次都不执行、无错、无事务。排除列表再加：SMMU、GBIF/LLC、线程选择、CP 保护、UCHE 刷、客户端读回。**`CP_SET_SECURE_MODE` 这条路废弃**（会打死 CP，且不可恢复）。
  - 附带纠正：`M s0 c999 r64 i4096` 的语义（查 `refs/UniXNU/drivers/NativeMetal/bridge/NativeMetalBridge.rs`）：compute 是 1000 个 float 比 `3*i`，`999 wrong` = 只有 i=0（期望 0.0，缓冲区本来就是 0）对得上 → **一个都没写**；render 是 8×8=64 texel 全错、import 是 64×64=4096 texel 全错。不是"部分成功"。
  - **metal64 改动（两处与上游 a7xx 的实质差异）**：
    1. **`CP_ME_INIT` 现在真的把 power-up 寄存器表交给 CP**：`a7xx_cp_init()` 传 `pwrup_reglist_iova` + `BIT(31)`（"从表里读寄存器值"），而本移植一直传 `pm4[7..9]=0`——**mask 的 bit 8 明确要求用这张表，地址却是 0、标志位也是 0**。表本身是建好并 clean 到内存的（`CP power-up list published 0x0 at 0x847748000`），只是从来没被引用。没有它，GX 从 IFPC 塌陷回来时 CP 无状态可恢。`sheng-gpu-pwrup=0` 保留旧行为做对照。
    2. **fence 不再把 IFPC 打开**：`a7xx_submit()` 在用户 IB 前发 `CP_SET_MARKER 0x101`（IFPC disable）之后**再也不打开**；我们的 framed fence 每条都发 `0x100`（enable），等于每两次提交之间允许 GPU 把 GX 域塌掉。改成保持 `0x101`；`sheng-gpu-ifpc=1` 恢复旧行为。
    3. 探针改成**忠实重放**：`A0` = 旧 27 dword（对照，应仍 0/512）、`A6` = 加上 `RB_CCU_CNTL=0x68` 与 `CCU_CLEAN_COLOR/DEPTH` 事件（客户端 blit 后本来就发这两个事件，之前被我截掉了）、`B6` = 忠实版 + 未映射目的地址。status 行增加 `GBIF_HALT(0x3c45)/ACK(0x3c46)` 与 `RBBM_GBIF_HALT_ACK(0x17)`。
  - 判读（下一份日志）：
    - `CP ME_INIT power-up list on iova 0x301000 flag 0x80000000` + `framed fence leaves IFPC disabled` → 两处改动生效；
    - `blit A0 … filled 512/512` 或 `A6 … filled 512/512` → **引擎开始写了**，接着看客户端 `T … v1024`；
    - `A0 0/512` 但 `A6 512/512` → 是 CCU 那条（颜色缓存没被刷出去）；
    - 两个都 0 且 `gbif_halt 0x0/0x3`、`rbbm_gbif_ack 0x3` → GX 侧 GBIF 停摆这条成立，下一步专门解 halt。
  - 产物：
    - payload `shim/out/payload-metal64-pwrup.bin` `18cc2f9b1e7ec3f252bd4579be26b757c6801f15a979cd28b81aa1dd923d7a87`（新串 `power-up list %s` / `leaves IFPC %s` / `sheng-gpu-pwrup` / `sheng-gpu-ifpc` / `rbbm_gbif_ack` 已核对）；
    - 引导镜像 `shim/out/shim-boot-metal64-pwrup.img` 32,071,680 B `9029b5c20adf51ccf4040d96924daede5a0d3377fc482d2c1eb80515e2338238`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-30 GPU 第 323 步：metal62 上机——**分支 C**：`CP_BLIT` 被 CP 收下（fence 正常退休），但 2D 引擎**既不写我们自己的页，也没有对未映射地址发出任何写事务**（SMMU 没记录 `far=0x3ff00000`）。也就是说写根本没离开 GPU。metal63 把问题切成"CP 有没有把 CP_BLIT 交给引擎"，同一段 Turnip fill 跑四遍 + 一个控制实验**
  - **metal62 上机事实**（`2243.txt` 1,237,828 B `404db06768e41997846a5f48bbde4abc9ec38c740f7496184b8563bb7ef9d608`；镜像 metal62 `46a34344…`）：
    - `CP self-test submit 0x0 fence=3 result=0 observed=3 wrote=0x600dc0de`（CP 自己的写仍然正常，对照组）
    - `blit A submit=0x0 fence=4 result=0 observed=4 dest 0x302000 filled 0/512 w0 5a5a0000 5a5a0001 … w512 6b6b0000`（scratch 页原样，2D 填充无效）
    - `blit B submit=0x0 fence=5 result=0 observed=5`（fence 退休 → CP 收下并执行了这串包）
    - `blit B smmu gfsr 0x0 fsr 0x400 far 0xd5fe77b7 syn0 0x1b084b (stream 0x1b08) syn1 0x6277`——**没有 `far=0x3ff00000`**，那条 `fsr=0x400` 是引导链留下的陈旧记录（与之前每轮同一形状，`far` 每次只差 1，说明它不是我们这次产生的）
  - **判读：分支 C。** 引擎没执行、也没尝试执行；写连 SMMU 都没到。排除列表再加两条：SMMU 无关（无事务）、下游 GBIF/LLC 无关（无事务可丢）。
  - 附带收获：内核侧现在有了一个**完全可控的复现器**（27 个 dword、每轮 4 次提交、不需要客户端参与），迭代不再依赖 Turnip 什么时候跑到那一步。
  - **metal63 做法**（`sheng-gpu-blit=0` 关闭）：同一段 fill（`CP_THREAD_CONTROL 0x08000001` + 6 条 A2D 寄存器 + `RB_A2D_CLEAR_COLOR_DW0..3` + `RB_A2D_DEST_BUFFER_INFO/BASE_LO/HI/PITCH` + `GRAS_A2D_DEST_TL/BR={0,0x1ff}` + `CP_BLIT op 3`）跑四遍，每次都预填 `0x5a5a0000+i`、fence 退休后 `dc ivac` 读回：
    - **A0** 带 `CP_THREAD_CONTROL`（Turnip 原样）→ 应与客户端一致 `0/512`
    - **A1** 不带 `CP_THREAD_CONTROL` → 线程选择是不是元凶
    - **A2** 先发 `CP_SET_SECURE_MODE 0`（opcode 0x66）再跑 → 引擎是不是还在安全模式（安全事务被标 secure → 静默丢弃；`RBBM_SECVID_TRUST_CNTL` 在 metal49 已证明 CPU 写就会外部中止，所以只能由 CP 来退出安全模式）
    - **B** 目的地址 `0x3ff00000`（CB2 内未映射）→ 引擎有没有发出写事务（`far=0x3ff00000`）
    - **D** 控制实验：同样的流但 `CP_BLIT` op 改成 `0x7f`（硬件无法实现）→ 若 `CP_INTERRUPT_STATUS` 仍为 0，说明这个状态寄存器在本机不是活信号，前面"无错误"就没意义；若置位，则说明合法 `CP_BLIT` 是被微码接受的、只是引擎没跑
    - 每步前后读 `CP_INTERRUPT_STATUS(0x823)` / `CP_HW_FAULT(0x821)` / `RBBM_INT_0_STATUS(0x201)` / `CP_PROTECT_STATUS(0x824)` / `RBBM_STATUS(0x210)`；中途把 A2D 寄存器读回来（`blit regs a2d …`）确认 CP 确实执行了那 6 条 type4 写
  - **注意（诚实边界）**：这 27 个 dword 是从客户端流尾部截出来的，客户端前面还有 ~200 dword 的 SP/TPL1/UCBW 状态（含 bicubic 权重、`SP_A2D_OUTPUT_INFO` 之外的东西）。A0 = `0/512` 与客户端一致只是"都不工作"，**不能证明我们的复现是完备的**。若 A0/A1/A2/D 全负，下一步就是把客户端那 233 dword **整条**重放（把它引用的客户端 IOVA 换成本内核的 scratch 区），而不是继续截尾。
  - 产物：
    - payload `shim/out/payload-metal63-blit4.bin` `f22142b315f32832dcf0cab9e638944569f1b55caada878a72ef82cd1adb6372`（新串 `blit %s submit` / `blit status %s` / `blit regs a2d` / `CP_BLIT op 0x7f` / `secure-mode-0` 均已核对）；
    - 引导镜像 `shim/out/shim-boot-metal63-blit4.img` 32,071,680 B `d8061d04e92a78ed853bbd5c1001c36bf74d09d8fc7ee0f67038381b32cdfba9`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-30 GPU 第 322 步：metal61 上机——SMMU 这条线彻底结案（`unmatched 0`，两条 GPU SID 都落在 CB2）；GBIF 系统缓存这条也排除（引导链本来就留 `0/0`，改回去什么都没变）。metal62 换成**直接问 2D 引擎**：把 Turnip 那条 `vkCmdFillBuffer` 的 A2D 尾巴**原样搬进内核自己的 ring**，先填自己的 scratch 页，再填一个 **CB2 里没映射的地址**，靠"填不填得进去 / SMMU 报不报 fault"三选一定方向**
  - **metal61 上机事实**（`2234.txt` 1,208,586 B `275e72c6295b5410132dde0d93487a04062fe91b33ab35f59a75a365e69d8b62`；镜像 metal61 `bf8f4cec…`）：
    - `smmu audit smr 0 0x84000005 id 0x5 mask 0x0 s2cr 0x1 type 0 cb 1`（GMU）
    - `smmu audit smr 1 0x84000000 id 0x0 mask 0x0 s2cr 0x2 type 0 cb 2`
    - `smmu audit smr 2 0x84000001 id 0x1 mask 0x0 s2cr 0x2 type 0 cb 2`
    - `smmu audit dt sid 0x0 -> smr 1 …` / `dt sid 0x1 -> smr 2 …` / `dt sids 2 unmatched 0`
    - `GBIF before cx 00020001 scache0 00000000 scache1 00000000 scid 0/0/0/0/0/0 qsb 00071620 00071620 00071620 00071620 halt 00000000/00000003`
    - `GBIF scache left as the boot chain set it` → `scache now 0/0`
    - 探针逐行不变：`T s0 r0 v512 c0 g0/4096`、`zpass deadbeef ×4`、`draw 0`×74 / `blit 1`×17 / `cs 1`×3、fence 超时仍是 4 次
    - 一次全核忙：`busy fence 11 mask 0xef0587 engine 47`（VPC/UCHE/SP/TPL1/HLSQ 同时置位）
  - **两条结论**：
    1. **SMMU 排除**（这次的解码是对的，且审计在 route 之后）：GMU 的 SID 5 → SMR0 → CB1；GPU 的 SID 0/1 → SMR1/SMR2（mask 0 = 精确匹配）→ **CB2**，`.s2cr` 低 8 位 = CB 号、`(s2cr>>16)&3` = 0 翻译 / 1 bypass / 2 fault。DT 给的两条 SID 全匹配。再加上 `gfsr` 全程 0，SMMU 侧没有任何未匹配或被 abort 的 stream。
    2. **GBIF 系统缓存（LLCC 切片）排除**：引导链留的是 `SCACHE_CNTL0/1 = 0/0`（六个 slice id 全 0），我们 metal60 之前写的 `0x71620` 确实是垃圾，但**把垃圾换回 0 什么都没变**；QSB 四个 side 引导链本来就等于 `0x71620`（metal59 那次"修正"其实是把同一个值又写了一遍，所以当时必然没效果）。
    - 顺带新事实：`GBIF_HALT = 0` 但 `GBIF_HALT_ACK = 3`（两位都"已 halt"）。与"CP 能读能写"矛盾，暂记为**未知**（a730 上 0x3c46 的语义可能与 a6xx 文档不同），不作为本轮依据。
  - **metal62 做法**（内核侧 2D 引擎探针，`sheng-gpu-blit=0` 关闭）：把 `2234.txt` 里 Turnip 真实 fill 的尾部 27 个 dword 原样重放（`CP_THREAD_CONTROL 0x08000001`、`RB_A2D_PIXEL_CNTL=0`、`RB_A2D_BLT_CNTL=0x07f04b80`、`GRAS_A2D_BLT_CNTL=0x07f04b80`、`TPL1_A2D_BLT_CNTL=0x20000000`、`SP_A2D_OUTPUT_INFO=0x0000f25c`、`RB_A2D_CLEAR_COLOR_DW0..3`、`RB_A2D_DEST_BUFFER_INFO/BASE_LO/HI/PITCH`、`GRAS_A2D_DEST_TL/BR={0,0x1ff}` = 512 dword = 2048 B、`CP_BLIT op 3`），目的地址自己选：
    - **blit A**：dest = 自己的 scratch 页 `0x302000`，填 `0xdeadc0de`；预先在 [0,64) 写 `0x5a5a0000+i`、在 [2048,2112) 写 `0x6b6b0000+i`（后者必须保持），fence 退休后 `dc ivac` 再读，打 `blit A … filled <n>/512 w0 … w512 …`。
    - **blit B**：dest = `0x3ff00000`（在 CB2 的 2 GiB aperture 内、但没映射），填 `0xdeadc0df`；随后**自己**去读 CB2 的 `gfsr/fsr/far/syn0/syn1` 打 `blit B smmu …`（不等客户端下一次提交去清）。
  - 判读（下一份日志看三行）：
    - `blit A … filled 512/512` → 引擎真的会写：问题在客户端 BO 或它那段状态里，下一步比两段流的差异。
    - `filled 0/512` 且 `blit B smmu … far 0x3ff00000`（`fsr` 出现 TF 位）→ 引擎执行了、写也到了 SMMU，丢在 SMMU 之后（GBIF/LLC），下一步查 GBIF 的 AXI 通路。
    - `filled 0/512` 且 `blit B smmu … far` 没变 → **`CP_BLIT` 根本没到 2D 引擎**，问题在 CP 的分发侧（`CP_THREAD_CONTROL`/draw-state/微码），这会把整条调查线改向 CP。
  - 产物：
    - payload `shim/out/payload-metal62-blit.bin` `b80cc43fbc261b79cd0e3d94bdb5ac2c82938ea9f2ddd36acf95b68f98ffa78b`（新串 `blit A submit` / `blit B submit` / `blit B smmu` / `sheng-gpu-blit` 各 1 处，已核对）；
    - 引导镜像 `shim/out/shim-boot-metal62-blit.img` 32,071,680 B `46a343440caecf411f31a74bfb1591a195b2853f8f5b3dd1b305e4a08c2f6eb6`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-30 GPU 第 321 步：metal60 的 SMMU 审计结案（`unmatched` 是我们自己的解码 bug + 审计跑早了一步），换方向查 GBIF 系统缓存（LLCC）切片配置——我们一直往 `GBIF_SCACHE_CNTL0/1` 写 `0x71620`，那是六个互不相同的 slice id**
  - **metal60 上机事实**（`2219.txt` 1,141,244 B `8e50353cb3ae597d181f41ede7ab15e3f952c0c95e816d8c29530e103f881b25`；镜像 metal60 `93ebd1f1…`）：
    - `smmu audit smr 0 0x84000005 sid 0x400 mask 0x5 s2cr 0x1 type 0 cb 1`（只有 SMR0 一个 VALID，是 GMU 的 SID 5 → CB1）
    - `smmu audit dt sid 0x0 UNMATCHED` / `sid 0x1 UNMATCHED` / `dt sids 2 unmatched 2`
    - 紧接着的 `GPU route smr1=0x84000000 s2cr1=0x2 smr2=0x84000001 s2cr2=0x2` —— **GPU 的两条 SMR 是审计之后那一行才写的**
    - 探针全部不变：`T s0 r0 v512 c0 g0/4096`（填充/计算/渲染仍然一项都不对）、`zpass deadbeef ×4`、`GBIF QSB sides 4/4 (71620×4)`
    - `cp state rb_nc 0x1811 rb_ccu 0x0 uche_mode 0x0 qsb0 0x20001 rbbm_status 0x1`
    - fence 超时 4 次（1/9/10/15）；第 15 次 `rptr 872 wptr 913 … fence 872/14`，CP 停在离 IB 尾部 **41 dword**处 = 正好是我们那个 framed fence 块的长度
  - **结论：SMMU 这条线排除。** 两个原因：①`ARM_SMMU_SMR_ID` 是 **bits 15:0**、`ARM_SMMU_SMR_MASK` 是 **bits 23:16**（`arm-smmu.h`），metal60 把两者读反了（它把 `0x84000005` 读成 sid 0x400/mask 0x5，实际是 **id 5 / mask 0**）；②审计跑在 route 之前，那时 GPU 的 SMR 还没写。按正确解码：SMR0 = VALID|id 5|mask 0 → 只匹配 SID 5 → CB1（GMU）；route 之后 SMR1 = VALID|id 0、SMR2 = VALID|id 1（mask 0 = 精确匹配）→ SID 0/1 → CB2。**DT 给的两条 GPU SID 都被匹配上了，走 CB2 翻译**。`s2cr` 低 8 位是 CB 号（S2CR_CBNDX），`(s2cr>>16)&3` 是 type：0 翻译 / 1 bypass / 2 fault，日志里 GMU=1（CB1）、GPU=2（CB2），都正常。
  - **新方向（本轮要上机的）**：`a7xx_llc_activate()`（`a6xx_gpu.c:1862`）把 GPU 的 **LLCC 系统缓存切片**写进 `GBIF_SCACHE_CNTL1`（`0x3c02`，**六个 5-bit 域全是同一个 slice id**）+ `GBIF_SCACHE_CNTL0`（`0x3c01`，`(slice<<10)|BIT(8)`）。我们这个移植从 metal50 起一直往这两个寄存器写 `0x71620`——解出来是 slice id **0/1/9/0/0/0** 六个不同值，`CNTL0[14:10]=28`，而且 `CNTL0[8]` 是 0。**引擎的写全是"分配型"写（走系统缓存），CP 自己的写不是**：SCID 指到别人的切片上，那些行会被静默丢掉、不报总线错——正好是门②的形状（CP 写进得去、引擎写一个都不落、日志干净）。而 QSB 四个 side 的 `0x71620` 是**对的**（Linux 原值），不该跟着改。
  - 本轮改动（metal61）：
    1. **不再写 `0x3c01/0x3c02`**，改成：先把 `0x3c00..0x3c06` + `GBIF_HALT/ACK(0x3c45/0x3c46)` 读出来打一行 `GBIF before …`（含 SCACHE_CNTL1 解出的六个 slice id），把引导链留下的值原样留着；只有显式给 `sheng-gpu-scid=<n>` 时才按 a7xx 公式写 `(n<<10)|0x100` / 六份 n。
    2. 事后打 `GBIF QSB sides … scache now <cntl0>/<cntl1>`。
    3. metal60 的审计搬进 route 之后并按正确位域解码（`id` bits 15:0、`mask` bits 23:16、`cb` bits 7:0）。
  - 判读（下一份日志看三行）：
    - `GBIF before … scid a/a/a/a/a/a` **六个值相同且非 0** → 引导链本来就配对了，我们之前一直在破坏它；这轮上机若 `T … v1024` 或 `c1024` 出现，就是它。
    - 六个值相同但为 0、或本来就不一致 → 说明引导链没配，下一轮用 `sheng-gpu-scid=<n>` 逐个试（n 从 0 到 4）。
    - 若这轮全部探针仍然不变 → 系统缓存这条也排除，下一步转"引擎到底有没有发写事务"（把 CB2 里某个测试页改成只读，靠 SMMU 的 permission fault 当探测器）。
  - 产物：
    - payload `shim/out/payload-metal61-scache.bin` `e5c58c00bb9a5ffff843221491af256d2cd7cfa92b18f2416e382ad9fce37a9b`（含新串 `GBIF before cx` / `GBIF scache left` / `sheng-gpu-scid` 各 1 处，已核对）；
    - 引导镜像 `shim/out/shim-boot-metal61-scache.img` 32,055,296 B `bf8f4ceca08e22804744acdd7f15fac6aa20fc37edcdaaf4e32b17b0aaf1c935`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-30 GPU 第 320 步：出 metal60——SMMU stream 匹配审计。做法：把 DT 的 `iommus` 里那组 SID 存下来，在 GPU 上下文起来后把前 16 个 `SMR`/`S2CR` 全打出来并逐条判定"这个 SID 有没有匹配上、落到哪个 CB"**
  - 依据（第 319 步的静态发现）：`NativeAdrenoGpuContext.cpp:74` 只挂 2 条 stream；`NativeAdrenoSmmuContext.cpp:17` 是 `stall_on_fault=0, continue_after_fault=1`；SMMU-500 在未匹配 stream 上是 **abort**（不报 fault），正好解释"CP 的写进得去、引擎的写一个都不落、日志还干净"；
  - 新日志行：
    - `NativeAdreno: smmu audit smr <i> 0x<raw> sid 0x<sid> mask 0x<mask> s2cr 0x<raw> type <t> cb <c>`（每个 VALID 的 SMR 一行）
    - `NativeAdreno: smmu audit dt sid 0x<sid> -> smr <i> … ` 或 `… UNMATCHED (aborted)`
    - `NativeAdreno: smmu audit dt sids <n> unmatched <m>`
  - 判读：`unmatched` 非 0 → 引擎主的 SID 根本没被匹配，写被 SMMU abort；下一步就是把它加进 `routes[]`（或按匹配表把 SMR 补上）。`unmatched 0` 且全部 `type 2 cb 2` → SMMU 这条线也排除，转去查 UCHE→GBIF 的实际写事务（例如用 `CP_MEM_TO_MEM` 从引擎目标地址读回做对照）。
  - 产物：
    - payload `shim/out/payload-metal60-smmaudit.bin` `54af53e619c9030cb48753025b4f00b59e9901e3538dd7cce835b8a04217fccd`；
    - 引导镜像 `shim/out/shim-boot-metal60-smmaudit.img` 32,055,296 B `93ebd1f198887850964c664e0e40252862f10ff8e76ea7af111583245557b341`；
    - **root 不变**（gpu11 `f7043229…`）。

- **2026-10-30 GPU 第 319 步：GBIF QSB 修正上机了、**没有效果**——`zpass` 仍然全是 `deadbeef`，全部探针逐行不变。这条线结案。静态复查自己的代码时发现新方向：我们只往 SMMU 挂了 **2 条 stream**（`NativeAdrenoGpuContext.cpp:74 routes[2]`，SID 来自 DT 的 `iommus`），而上下文配置是 `stall_on_fault=0, continue_after_fault=1`（`NativeAdrenoSmmuContext.cpp:17`）——**SMMU-500 在这种配置下会静默 abort 未匹配 stream 的访问**。如果引擎主（不是 CP 主）的 StreamID 不在这两条里，它的写就会被丢掉，而 CP 的写照常过：正是我们看到的不对称**
  - 事实（`2210.txt` 1,431,000 B `acf894e685875f568f74d79019030952bdf8efeada28d31b18202e9fa75c4a10`；镜像 metal59 `c4b3d996…`）：
    - `GBIF QSB sides 4/4 written at 0x3c03..0x3c06 (read 71620/71620/71620/71620)` → 偏移改对了；
    - `busy … prot 0x0 zpass deadbeef deadbeef deadbeef deadbeef` 从 fence 4 到 49 **一次没变** → RB 发起的写仍然进不了 DRAM；
    - 探针全部不变（`MP4 f0`、`M s0 c999…`、`T v512`、`FS b1e7e f14 s1`）；
    - `smmu fault` 仍是 `fsr=0x400 tlbmcfg=1`、`sid=0x1b08`，与"写被丢"无关（TLB 配置类）。
  - 已排除的假设（按时间）：SMMU 翻译故障、CP 寄存器保护表（`prot` 全 0）、GBIF QSB 侧配置、UCHE/GMEM 缓存未刷（跨提交刷也没用）、客户端读回路径（内核 + DRAM 两路读都是 `deadbeef`）。
  - 新方向（静态发现，待上机验证）：GPU 的 SMMU 上下文只 attach 了 2 条 stream，且 `stall_on_fault=0`；未匹配的 SID 在 SMMU-500 上被 **abort**（不是 fault，所以日志干净）。
  - 下一步（metal60）：读 SMMU 的 `SMR/S2CR` 与 `sACR`/`GBPA`，把 DT 给的所有 GPU SID 逐条判定"是否匹配到 context bank 2"；若引擎主的 SID 缺失，就把它加进 `routes[]`——这就是修复。
  - 产物：本轮**没出镜像**（metal59 `c4b3d996…` 即最新）。

- **2026-10-30 GPU 第 318 步：`ZPASS` 也没落、`prot` 全 0，而**静态比对 Linux 发现一个真 bug**——我们的 GBIF QSB 侧写到了 `0x3c00..0x3c03`，Linux 的 `GBIF_QSB_SIDE0..3` 是 `0x3c03..0x3c06`：SIDE1/2/3 **从来没被配过**，`0x3c01/0x3c02`（`GBIF_SCACHE_CNTL0/1`）反被写脏；而 KMD 的自检读的正是同一个错偏移，所以十几轮以来一直自称"4/4 已写"。metal59 把偏移改对，并把四个侧都读出来**
  - 事实（`2203.txt` 1,187,826 B `73d21526adff2a54e40a955e049dcbb9448461e3b6d688a47b3060e32da6a462`；镜像 metal58 `99e9b0c9…`）：
    - `busy … prot 0x0 …` 全部 48 行：`CP_PROTECT_STATUS` 一次都没非零 → **寄存器保护表没有拒绝过任何客户端写**，这条假设排除；
    - `zpass`：fence 2/3 是 `00000000`，**从 fence 4 起永远 `deadbeef deadbeef deadbeef deadbeef`** → `CP_MEM_WRITE` 的预填（CP 写）落了，**`ZPASS_DONE`（RB 发起的写）一次都没覆盖它**；
    - `busy fence 11 … mask 0xc50061 engine 5` → 唯一一次看到 3D 块忙（`VPC_BUSY|SP_BUSY`，同一次里还有 `TSE/RAS`）→ 引擎不是完全没跑；
    - 所有探针结果与之前**逐行相同**（`MP*` 全 0、`FS b1e7e f14 s1`、`M s0 c999…`、`T v512`）→ UBWC bank-bit 修正对线性缓冲无影响（符合预期）；
    - `draw 0` ×74（512 dwords 窗口）、`blit 1` ×17、`cs 1` ×3。
  - 本轮发现（代码级，不需要上机）：
    - `NativeAdrenoService.cpp` 的 QSB 循环写 `0x3c00+i`（i<4），随后也读 `0x3c00`/`0x3c03` 自检；mesa a6xx 的编号是 `GBIF_SCACHE_CNTL0=0x3c01`、`CNTL1=0x3c02`、`QSB_SIDE0..3=0x3c03..0x3c06`，Linux `a6xx_hw_init()` 写的是 `0x3c03..0x3c06`；
    - 也就是：**SIDE1..SIDE3 保持复位值**，SCACHE_CNTL0/1 被 `0x71620` 覆盖。GBIF 的 QSB 侧就是 GPU 四个总线客户端的 QoS 配置——某个客户端没配侧，它的读写请求可能根本不被授权；这与"CP 自己的写（另一个客户端）进得去、引擎的写一个都不落"完全吻合；
    - 另外静态比对还显示 `CP_RB_*`、`RBBM_SECVID_TRUST_CNTL`、`UCHE_FILTER_CNTL` 等只在别处/别阶段写，不足以解释写丢失。
  - 推断：GBIF QSB 侧配置是目前**唯一**与"引擎写不通"直接对应的初始化缺陷；`ZPASS` 不落是强证据但非铁证（该事件也可能要求真正的采样会话）。
  - 本轮改动（内核 metal59；root 不动）：
    1. QSB 侧改写到 `0x3c03..0x3c06`；`0x3c01/0x3c02` 保持原来的值（避免同时改两个变量）；自检读 `0x3c03..0x3c06` 四个并打印。
  - 产物：
    - payload `shim/out/payload-metal59-qsb.bin` `e2d6ed1d50ae8e0221c14012f4628fe5d06550ec65af07923185ab2dec111707`；
    - 引导镜像 `shim/out/shim-boot-metal59-qsb.img` 32,055,296 B `c4b3d9961cc1577afaf74bdf3c4b3f5df1d4a741e42e0287e17b9162d722a9a2`；
    - **root 不变**（gpu11 `f7043229…`）。
  - 下一轮怎么读：
    - `GBIF QSB sides 4/4 written at 0x3c03..0x3c06 (read 7170620/…)` → 偏移对了；
    - `zpass` 变成非 `deadbeef` → 引擎的写终于落地，接着看 `MP4/MP8/MP10/M s0` 是否翻身；
    - `prot` 仍全 0、`zpass` 仍 `deadbeef` → 继续查 GBIF/UCHE 的其余初始化（`GBIF_SCACHE_CNTL0` 的 SCID、`UCHE_*`）。

- **2026-10-30 GPU 第 317 步：`wtgt` 把"引擎不写内存"钉成铁证——**A2D fill 的落点两次采样都是填充前的 `deadbeef`（连下一次提交的 `CACHE_FLUSH7`+`CACHE_INVALIDATE7` 之后也一样），而同一次启动里 CP 自己的写（`CP_REG_TO_MEM` 目标）在不断变化。并且 512 dwords 窗口下 **`draw` 仍然是 0**，但 `ds`（`CP_SET_DRAW_STATE`）每次提交都有 1–2 个。metal58 加了一条**RB 自己发起、文档明确写"跳过 UCHE"的写**（`ZPASS_DONE` 写采样计数器）作为判别器，并把 UBWC 的 bank-bit 改成 Linux/ABI 一致的 16**
  - 事实（`2155.txt` 1,241,306 B `dfd63a69d14dea4387be7829089f82b37555eef7c8b04afabddedd4cc0b2d9e2`；镜像 metal57 `ff2b0d35…`）：
    - `wtgt` 的 `d` 列这次是活的（`k`/`d` 全部一致，`st 0/0`），说明"内核别名 + 丢 cache 后读"两条路读到的相同；
    - A2D fill 落点：`wtgt fence 8 of 8 try 1 addr 0x2002c000 k deadbeef deadbeef d deadbeef deadbeef`，`fence 9 of 8 try 2` 一字不差 → **fill 没写**，而且跨了一次提交的刷也不出现；
    - 对照：CP 侧写的目标在变（`fence 11 … 0x2002c008 k 09050c08` → `fence 12 … k 40000000`）→ CP 写一直是通的；后来的 `40000000 40400000 …` 是 **CPU** 写进去的浮点输入缓冲（同一 VA 被下一个测试复用），不是 GPU 写的；
    - `ibops`（窗口已 512 dwords）：`draw 0` ×74、`blit 1` ×17、`cs 1` ×3、`ds 1` ×56 / `ds 2` ×2 → 客户端**会发 draw-state 组，但从头到尾没有一条 `CP_DRAW_*`**；
    - `wtgt` 的 `exp` 字段是 0：`RB_A2D_CLEAR_COLOR_DW0` 在流里出现在 dest **之前**，探针把它挂到了上一个目标上（只影响这一列的可读性，不影响 k/d）。
  - 推断：
    - "引擎写不落"已经是三次独立验证（客户端读回、内核读、DRAM 读）；
    - 现在只剩两个互斥的可能：**(a) 引擎根本没执行**（包只落在寄存器堆上）；**(b) 引擎执行了，写死在 RB/CCU→UCHE→DRAM 某一段**。`ZPASS_DONE` 正好落在两者之间：它由 RB 发起、写主存、**跳过 UCHE**；
    - UBWC bank-bit：内核写的是 hbb=3（`RB_NC 0x817`/`UCHE_MODE 0x600000`），而 ABI 告诉 turnip 的是 hbb=16——两边对同一块 UBWC 内存的地址摆法不同，这一条对 IOSurface（全是 UBWC）是致命的。
  - 本轮改动（内核 metal58；root 不动）：
    1. framed fence 里加 **ZPASS_DONE 写探针**：`CP_MEM_WRITE` 用 CP 写 8 个 `deadbeef` 到 fence 页 `+0x80` → `RB_SAMPLE_COUNTER_BASE=+0x80`、`RB_SAMPLE_COUNTER_CNTL.copy=1` → `CP_EVENT_WRITE7(ZPASS_DONE|WRITE_SAMPLE_COUNT, addr)` → `WFI`；framed fence 20 → 41 dwords；
    2. `busy` 行加了 `prot 0x%x`（`CP_PROTECT_STATUS 0x824`，CP 拒绝过的寄存器地址）和 `zpass w0 w1 w2 w3`（那 8 个字的前 4 个）；
    3. UBWC/NC 改成 Linux `a6xx_set_ubwc_config()` 对 hbb=16 的结果：`RB_NC_MODE_CNTL 0x817→0x1811`、`TPL1_NC_MODE_CNTL 0x7→0x41`、`SP_NC_MODE_CNTL 0x27→0x1021`、`GRAS_NC_MODE_CNTL 0x60→0`、`UCHE_MODE_CNTL 0x600000→0`。
  - 产物：
    - payload `shim/out/payload-metal58-zpass.bin` 25,477,552 B `2192e031a519ae242a1f16abbc74d83fad6704066810e67d22c1d28143dc25ca`；
    - 引导镜像 `shim/out/shim-boot-metal58-zpass.img` 32,055,296 B `99e9b0c9e76fc8bbaa8a5c7aa46201c233198b074390100cf25be397cd2aa29c`；
    - **root 不变**（gpu11 `f7043229…`）。
  - 下一轮怎么读（三行）：
    - `zpass` 仍是 `deadbeef deadbeef deadbeef deadbeef` → **RB 自己的写也进不了 DRAM** → 引擎/adapter 层的写通路死了，往下查 RB/CCU/GBIF 的初始化与 AQE/前端是否真的接受这些包；
    - `zpass` 出现计数器（多为 0 或很小的数，不再是 deadbeef）→ **RB 能写**，问题在 A2D/compute 那条路的上游（前端/状态/UBWC 摆法）；
    - `prot` 非 0 → CP 拒了客户端的某个寄存器写，把地址解出来就是被丢掉的那条配置。

- **2026-10-30 GPU 第 316 步：`ibops` 第一次把每次提交的形状打出来——74 次提交里 **`draw 全是 0`**，而 `blit` 有 15 次、`cs` 3 次；并且**内核自己**读了 A2D blit 的落点：`0x2002c000` 在 fill 之后仍是**填充前的 `deadbeef`**，整份 1.5 MB 日志里 `c0ffee42` 只出现过 1 次（在寄存器 dump 里）。于是 gate ② 的范围大幅缩小：**连不需要 shader、不需要 draw state 的 2D/A2D 引擎也不写内存**，不是"draw state 配错"这一层，是引擎的写通路本身**
  - 事实（`2132.txt` 1,526,701 B `0870276316b1b9442187f4ab08e3c96a1719ac8ec8c4015f54cc55566d0aea32`；镜像 metal56 `092723c6…`）：
    - `ibops` 共 74 行，**`draw 0` 全部**；`cs 1` 有 3 次、`blit 1` 有 15 次；`cond 1`（一个 `CP_COND_REG_EXEC`）几乎每行都有；
    - 两条完整 engine stream（前两条带引擎工作的流）：
      - fence 6 = Turnip 冒烟测试的 `vkCmdFillBuffer`，实现成 **A2D blit**：`RB_A2D_BLT_CNTL=0x07f04b80`、`GRAS_A2D_BLT_CNTL=0x07f04b80`、`TPL1_A2D_BLT_CNTL=0x20000000`、`SP_A2D_OUTPUT_INFO=0x0000f25c`、
        `RB_A2D_CLEAR_COLOR_DW0=0xc0ffee42`、`RB_A2D_DEST_BUFFER_INFO=(0x4b, 0x2002c000, 0, 0)`、`GRAS_A2D_DEST_TL=(0, 0x1ff)`、`CP_BLIT op=3`，随后 `CCU_CLEAN_COLOR/DEPTH`；
      - fence 7 = 计算派发：`SP_CS_NDRANGE_0` 等 `SP_CS_*` 状态 + 2 个 `CP_INDIRECT_BUFFER`（`0x20008780/44`、`0x20038000/5`）+ **`CP_EXEC_CS`(16 groups)**；
    - 内核侧采样（`gputgt`，metal56 的"最新映射"版）同一地址 `0x2002c000`：fence 6/7 读到 **`deadbeef`**（就是冒烟测试填充前由 CPU 写进去的图案）→ **fill 没写任何东西**；更晚这个地址变成 `5a5a5a5a`，那是三角形测试的顶点缓冲（**CPU** 写的同样图案、复用了同一个 VA），不是 GPU 写的；
    - 全日志里 **`c0ffee42` 只出现 1 次**（`RB_A2D_CLEAR_COLOR_DW0` 那条寄存器 dump），任何内存采样里都没有 → 这一笔 fill 在任何被看到的地方都没有落地；
    - 本轮 4 次 fence 超时（fence 1/8/15/38），两种形态与第 315 步相同；fence 38 是 `cp rptr 1784 wptr 1784`（时间戳丢失型）；
    - metal56 的探针缺陷：`gputgt` 的 `d*` 列全是 `0xffffffff`——`native_adreno_dram_read32` 这个钩子**从来没有任何地方安装过**；采样的目标也只是"最新映射"，多数是命令缓冲。
  - 推断：
    - 观察不到 draw 的那 74 行有一半不能下结论：`ibops` 只扫 IB 前 260 dwords（1040 B），而带整条管线状态的绘制流比这长；本轮已把窗口改成 512 dwords（2048 B）并加 `ds`（`CP_SET_DRAW_STATE`）计数；
    - **A2D blit 不写内存**这件事是硬结论，而且它不需要 shader、不需要 draw state、不需要 UVM：所以问题不在"绘制状态"，而在引擎写通路（RB/CCU → UCHE → SMMU → DRAM）或"A2D 根本没执行"；
    - 客户端其实已经把该做的缓存动作都做了（`CCU_CLEAN_COLOR/DEPTH`、`CACHE_CLEAN`），我们 ring 的 prologue 也有 `CACHE_FLUSH7`+`CACHE_INVALIDATE7`，所以"缓存没刷"的可能性下降；
    - 还有一个以前没排除的可能：**这些引擎包根本没有被 3D/A2D 前端接受**（例如 a7xx 的 LPAC/AQE 线程、或 `CP_SET_PROTECTED_MODE` 之后这些寄存器写只落在寄存器堆上）。
  - 本轮改动（内核 metal57；root 不动）：
    1. `wtgt fence F of S try T addr 0x… k <w0> <w1> d <w0> <w1> st <k>/<d> exp <E>`：**从流本身**取写目标并采样——`RB_A2D_DEST_BUFFER_INFO(0x8c17)` 的 base、`CP_MEM_WRITE(0x3d)`、`CP_REG_TO_MEM(0x3e)`，再往下钻一层 `CP_INDIRECT_BUFFER`；`exp` 是同一个流里 `RB_A2D_CLEAR_COLOR_DW0(0x8c2c)` 的值。每个地址采样两次：它自己的 fence，以及**再下一次提交之后**（那时下一条提交的 prologue 已经做过 `CACHE_FLUSH7`+`CACHE_INVALIDATE7`）；
    2. `d` 列现在是真的 DRAM：新钩子 `native_adreno_dram_invalidate`（board 里用 `NativeAdrenoMaintainPhysicalPages(...,NativeAdrenoInvalidate)` 实现）先丢掉该页的 cache 行再读 BO 的映射，`NativeAdrenoVm::dramWord()` 提供入口；
    3. `ibops` 扫描窗口 260 → **512 dwords**，并新增 `ds`（`CP_SET_DRAW_STATE`）计数；`engine stream` dump 也放宽到 512 dwords。
  - 产物：
    - 内核 `~/liuqin-build3/fakeroot/System/Library/Kernels/kernel.development.liuqin`（21:40）；
    - payload `shim/out/payload-metal57-wtgt.bin` 25,477,552 B `5f37d6d191676ea9b72877dc8dc6c5b5b479dd17d01b55ad93ff692b8911d209`；
    - 引导镜像 `shim/out/shim-boot-metal57-wtgt.img` 32,055,296 B `ff2b0d35310805aaacc788c55dbd2c3185a54a386e7c11912be6a5677875343b`；
    - **root 不变**：仍是 gpu11 `ipad71-root-wrap16-gpu11-wr.img` 17,179,869,184 B `f7043229…`。
  - 下一轮怎么读：
    - `wtgt … d c0ffee42`（或 `d 5a5a5a5a`）→ **写落地了**，客户端读回是坏的，去修读回/映射属性；
    - `wtgt … d deadbeef` 两次都这样 → 引擎确实一个字节都没写（连 CCU/UCHE 都没进），下一步查 A2D/3D 前端的接受条件与保护模式；
    - `wtgt … d 00000000` → 目标被清过、但没被填；
    - `ibops` 里出现 `draw ≥1` → 绘制流终于被看到，`engine stream` 会把整条打出来。

- **2026-10-30 GPU 第 315 步：把三条超时 IB 完整解码——**它们里面没有一条 draw**。第 314 步说"dump 只有前 28 dwords"是我引用时的截断，日志里其实是完整的 33 行/259 dwords；本轮把 full IB 全部解出来（`tools/pm4decode.py`，带 type7/type4 parity 校验），结论比"3D 卡住"更硬：超时那三条流是**纯状态 + 缓存维护**，一条 `CP_DRAW_*`/`CP_EXEC_CS`/`CP_INDIRECT_BUFFER` 都没有；而超时瞬间 CP 恰好停在我们自己 ring 的 `CACHE_CLEAN` fence 事件上，那时引擎已经空闲。所以**fence 超时和"画不出来"是两件事**，而"客户端到底有没有发 draw"这个问题必须变成每次提交一行的事实（本轮已加）**
  - 事实（`2100.txt` 1,207,329 B `597f17fd…`；解码器 `tools/pm4decode.py`，type7 opcode=bits16..22、payload=bits0..13，type4 offset=(w>>8)&0x7ffff、size=w&0x7f，两者 bit15/bit7/bit23/bit27 是奇偶校验位）：
    - 三条超时流：fence 11 `iova 0x20030000 bytes 1036`、fence 15 同地址同长度、fence 57 `iova 0x68050000 bytes 984`；
      **fence 11 与 15 逐 dword 相同**，fence 57 是同一段程序（目标地址换成 SpringBoard 的 `0x6803xxxx/0x6805xxxx`）；全部 259/246 dwords 都能解码、无 desync；
    - 流的形状（fence 11 的 259 dwords）：`CP_THREAD_CONTROL(0x08000003 = BOTH + CONCURRENT_BIN_DISABLE)` → `CCU_INVALIDATE_COLOR(0x19)`/`CCU_INVALIDATE_DEPTH(0x18)`/`LRZ_CACHE_INVALIDATE(0x28)`/`CACHE_INVALIDATE7(0x33)` → `WFI` → 一串寄存器写（`RB_CCU_CNTL(0x8e07)=0x68`、`SP_CS_CNTL_0=1`、`SP_CS_BASE_LO=0x20008680`、`SP_CS_INSTR_SIZE`、`TPL1_*`、`VSC_*`）→ `CP_SET_DRAW_STATE(0x00040000 = DISABLE_ALL_GROUPS)` → `CP_BV_BR_COUNT_OPS(OP=2 PIPE_SET_BR_OFFSET, offset 1)` → `CP_THREAD_CONTROL(BR/BV)` + `CP_SET_AMBLE`（bin preamble，155/149 dwords @`0x20028000`/`0x2002826c`）→ `CP_REG_TEST(PRED_UPDATE)` → `CP_MEM_WRITE`×4 把 `0x2002c000..0x2002c018` 清 0 → `CP_REG_TO_MEM` → `CP_MEM_WRITE(1)` → `CCU_CLEAN_COLOR/DEPTH` + `CCU_INVALIDATE_COLOR/DEPTH` → `SP_UPDATE_CNTL(0xab1f)=0x01fffe00` → `WFI` → `CP_CCHE_INVALIDATE(0x3a)` → `CP_WAIT_MEM_WRITES` → `WFI`；
    - 超时瞬间的 CP/引擎：fence 11 `cp rptr 412 wptr 421`、`cp ib1 0x20030000 rem 0`、`cp bv rptr 421`；按 `emitFramedFence()` 重建的 ring 块是 360..420，**412 正是主 fence 那条 `CP_EVENT_WRITE7(0x08000031, 0x300004, 0, 11)`**，BV 那条（405，写 `0x30000c`）也没落 → CP 停在"等 idle 的 CACHE_CLEAN"上；
    - `rbbm 0xe50015` 按 RBBM_STATUS 位域 = `CP_BUSY + VBIF_BUSY + VPC_BUSY + SP_BUSY + HLSQ_BUSY + GPU_BUSY_IGN_AHB(_CP)`；同一 dump 下面再读一次是 `rbbm_status 0x1`（只剩 `CP_AHB_BUSY_CX_MASTER`）→ **引擎已经空，CP 却还停在等 idle 的事件上**；
    - fence 57 是另一种形态：`cp rptr 2691 wptr 2711`、`cp ib1 0x68054000`（提交的是 `0x68050000/984`）→ CP **已经跑到后面的提交**，是那条时间戳事件被丢了；两种形态都在 kick（重写 WPTR）后恢复；
    - ring 块最后不满 8 dwords 的行以前被丢掉（fence 11 的块 61 dwords，日志只到 56）——第 314 步"最后 5 dwords 不见"就是这个，本轮修了；
    - GPU 侧结果与第 313 步完全一致：`MG2 v2 p60000/p6aaa9 c0 e0`、`MP13 v0 g0 w0 r0`、`FS b1e7e f14 s1`、`M s0 c999 … F14 6601`、`T s0 r0 v512 c0 g0/4096`。
  - 推断：
    - **fence 超时不是"3D 卡住"，而是 a7xx 的 BV/BR 并发 + 缓存事件机器在我们 ring 上偶尔不落地**（CP_THREAD_CONTROL/CP_BV_BR_COUNT_OPS/CP_SET_AMBLE/thread-mode COND_REG_EXEC 是三条超时流的共同点）；它被 kick 救回，不影响测试结果，和 gate ② 无关；
    - "3D 一律不写内存"仍然没有答案，但它现在只剩两个可能：**(a) 客户端根本没发 draw / 发了被 cond-exec 跳过；(b) 发了、也跑了，但写没落地（cache/GMEM）**。两者的区分本轮做成了三个探针；
    - 十轮 dump 都停在"状态流"上，是因为**只有这些状态流会超时**（超时才触发 dump），绘制流从不超时、因此从来没被打印过——这是本轮最大的方法学修正：不能靠"超时 dump"来观察 3D。
  - 本轮改动（内核，metal56；root 不动）：
    1. `ibops fence N dwords .. draw .. cs .. blit .. ib .. mkr .. cond .. ev .. cp .. reg .. bad`：每次提交一行 opcode 直方图（限流 72 行）——draw/cs/blit 计数直接回答"有没有发绘制"；
    2. `gputgt fence N iova 0x.. bytes .. c0 .. d0 .. c4 .. d4 ..`：fence 退休后用**内核自己**的映射采样该 VM 最新创建的 6 个映射的头两个 dword（`c*` = 内核可缓存别名，`d*` = 新加的非缓存 DRAM 读）——gate ② 缺的那一半：GPU 到底有没有把数据写进内存，而不是客户端读不到；
    3. `busy fence N mask 0x.. polls .. engine ..`：等待期间每 16 次轮询 OR 一次 RBBM_STATUS，退休时打印；draw 真进引擎就会看到 bits16..21；
    4. ring dump 补齐最后一个不满 8 dwords 的行；
    5. 新增 `tools/pm4decode.py`（PM4 解码器 + `tools/pm4_regs.json` 寄存器名表，取自本机 mesa 的 `a6xx.xml`）；
    6. `engine stream fence N …` + 33 行 `eng …`：**第一次把带 draw/dispatch/blit 的流整条打出来**（前两条）。之前只 dump 超时流，而超时流全是状态流，所以十轮都没见过真正的绘制流。
  - 产物：
    - 内核 `~/liuqin-build3/fakeroot/System/Library/Kernels/kernel.development.liuqin` 17,073,640 B（21:24）；
    - payload `shim/out/payload-metal56-ibs.bin` 25,477,152 B `80fbffd4430719079b8f0cdabe6701ff3098afb09a18b646fe6b8fe7a4deeae4`；
    - 引导镜像 `shim/out/shim-boot-metal56-ibs.img` 32,055,296 B `092723c65655be0d13400941d6250fc125e02ab37bff7945664fb65130622938`；
    - **root 不变**：仍是 gpu11 `ipad71-root-wrap16-gpu11-wr.img` 17,179,869,184 B `f7043229b7a628ea69aa171a606fad6d01120a21821652b32814a4b6213e565f`。
  - 下一轮怎么读（四条就够）：
    - `ibops` 里 draw/cs 全 0 → 客户端没发绘制，问题在 Metal→Vulkan→Turnip 的编码层，不在 GPU；
    - 有 draw 但 `busy … engine 0` → draw 被跳过（render-mode cond-exec / marker），去查 `CP_SET_MARKER` 与 `CP_COND_REG_EXEC`；
    - `engine` 非 0 且 `gputgt d0/d4` 有非零值、而客户端读到 0 → 写落地了，坏的是客户端读回路径（cache/映射属性）；
    - `engine` 非 0 但 `d0/d4` 全 0 → 写真的丢了（UCHE/GMEM/CCU），去查 `RB_CCU_CNTL`/`RB_NC_MODE_CNTL` 与 gmem 报告。

- **2026-10-30 GPU 第 314 步：SMMU 这条线**结案**——18 条故障全是 `tlbmcfg=1 tf=0`（TLB 配置类，Linux 里可忽略），`sid=0x1b08 addr=0x6277 far=0xd5fe77b6` 每条都一样；**SMMU 没有丢弃 3D 的写**。但同一次启动里出现了两次 **fence 超时**（fence 11、15），而且超时时 `rbbm=0xe50015`——**GPU 是忙的**：CP 已把 IB 吃完（`ib1 rem 0`）、BV 的 rptr 追平 wptr、fence 缓冲里 GPU 只写了 10 而客户端在等 11。也就是说**3D 管线会被启动、然后卡在最后那个 EVENT_WRITE 之前**，KMD 的 kick 把它救回来，那一笔的写自然没有完成**
  - 事实（`2100.txt` 1,207,329 B `597f17fde1a0586850d35d490e718e9b47df73b107d1c7beee0dd8e1e1b8e1c9`）：
    - `smmu fault #1…`：`fsr=0x400 tf=0 pf=0 ef=0 tlbmcfg=1`、`far=0xd5fe77b6 sid=0x1b08 addr=0x6277`，每条相同 → **可忽略的 TLB 配置故障**，与"没写"无因果；
    - `fence timeout 11 seq 11 next 421: cp rptr 412 wptr 421 … rbbm 0xe50015 rbbm3 0x700000`，
      `cp ib1 0x20030000 rem 0`（IB 已吃完）、`cp bv rptr 421`（BV 追平）、`fence words rptr 400 ts 10 bv 10 words 00000190 0000000a 0…`
      → GPU 写到 fence=10 就停了，客户端等 11；随后 `timed-out IB fence 11 iova 0x20030000 bytes 1036` + 前 28 dwords 的 dump；fence 15 同样；
    - GPU 侧测试结果不变（`MG2 v2 p60000/p6aaa9 c0 e0`、`MP13 v0 g0 w0 r0`、`FS b1e7e f14 s1`、`M … F14 6601`）；
      原始测试仍 `s34 e0 v4/4`（说明超时被 kick 救回后测试照常过）。
  - 推断（工作图景改了）：
    - 不是"3D 完全不执行"，而是**3D 被启动后卡住**（RBBM 忙、IB 吃完、fence 差一个数）：卡在最后那个 EVENT_WRITE 之前，
      也就是中间有一段绘制/等待没走完；kick 之后 CP 继续，但那一笔没写内存；
    - 这与"visibility pass 一跑就卡死 CP"是同一类现象（第 304 步），说明**3D 路径确实在跑，只是跑不完**；
    - `rbbm3 0x700000` 与 `rb_ccu 0x68` 值得在卡住瞬间一起记录（当前 dump 已含），下一步用它定位卡在哪个 draw state。
  - 下一步（我定，下一轮内核）：
    - 把 timed-out IB 的 dump 从 28 dwords 扩到**整条**（1036 B = 259 dwords），并同时打 `CP_BV_*`/`CP_BR_*`、`RBBM_STATUS/STATUS3`、`RB_CCU_CNTL`；
    - 用这条完整流解码卡住的位置（0xAE/0xB6/0x0E 这些包在 a7xx 里分别是什么），判断是缺 draw state、还是某个地址/常量 buffer 指错；
    - 这一步不需要动用户态，root 保持 gpu11。
  - 产物：本轮**没出镜像**（内核已含第 313 步的探针，仍是 metal55 `73919251…`）。
- **2026-10-30 GPU 第 313 步：SMMU 探针第一次跑出东西——**339 次故障，且每笔提交都在发生**（fence 4→129，连原始测试 48 字节的 IB 也触发）。`fsr=0x400` 恒定、`gfsr=0`，`far` 在 `0xd5fe77b6 / 0x24000 / 0x1c000 / 0x28000` 之间循环（0x28000 占 238 次）。但第一版探针把 FSYNR 读在了 0x50/0x54（那是保留位，读出 0/0），所以还分不清这些是"真翻译故障"还是"可忽略的 TLB 配置故障"。已出 metal55：FSYNR 改到正确偏移 0x68/0x6c，FSR 每一位单独打印，日志限流**
  - 事实（`2050.txt` 1,150,335 B `4eb1262b3fbf16f5454fd2e2c77d7e9a2e5fabd7763afaf35a398e93b523f4fc`）：
    - `smmu fault fence=N gfsr=0x0 fsr=0x400 far=0x…` 共 **339 条**；fence 4（`submit backboardd ib 0x20000000/48`，原始测试那种 48 字节 IB）就开始，
      `far` 分段：fence 4–28 `0xd5fe77b6`、29–30/69–70/111–112 `0x24000`、31/71/113 `0x1c000`、其余（238 条）`0x28000`，约每 40 笔提交循环一次；
    - GPU 测试结果与前一版**完全一致**：`MG2 v2 p60000 c0 e0` / `p6aaa9 c0 e0`、`MP13 v0 g0 w0 r0`、`FS b1e7e f14 s1`、`M s0 c999 … F14 6601 compute: 999 wrong`；
    - **黑屏不是崩溃**：`LIUQIN FB: display off`（约 13500 行）之后 `beat` 行是 `panel present 1 powerOff 1 fade 7 swap 35 vsync …`——面板断电、swap 停在 35，但系统继续跑（touch beat、bluetoothd 照常）。
      同一次启动里早段有 `CRASH SiriTTSSynthesiz/KonaSynthesizer/MacinTalkAUSP sig 6`，与之前几轮相同，是既有问题、与本次改动无关。
  - 推断：
    - 故障是**每次提交都发生**、而不是偶发；说明有一个"每笔提交都会碰"的访问在越界或属性不匹配——可能是 KMD 自己写的 fence/时间戳、
      也可能是某个映射的属性与 TLB 里已有的条目不一致（这正是本移植里反复出现的"别名属性不一致"问题）；
    - 编码未知是当前最大障碍：SMMU-500 的 `FSR` 里 bit 10 与 bit 15 分别可能是 TLBMCFG（常可忽略）与 TF（真翻译故障），
      而 `0x400` 只是 bit 10 —— **在拿到 FSYNR 之前不能下结论**；metal55 就是为此把 SID 和地址读出来、并把每一位单独标注。
  - 产物：
    - 内核 `~/liuqin-build3/fakeroot/System/Library/Kernels/kernel.development.liuqin`（20:53 增量重编）；
    - payload `shim/out/payload-metal55-smfault2.bin` 25,476,672 B `1ab4b64bb937dd38768d7233bf284572fab3a59e8c05f3c6e2ee62347d2ce5fa`；
    - 引导镜像 `shim/out/shim-boot-metal55-smfault2.img` 32,051,200 B
      `73919251a748e680f1e0b2fcbfd95512436c95be8a91a0e759e5cc2d56b675c8`（cmdline 与 metal53/54 相同；root 仍用 gpu11 `f7043229…`）。
  - 下次上机怎么读：
    | 日志 | 结论 | 下一步 |
    | --- | --- | --- |
    | `tf=1` | **真翻译故障**：GPU 访问了没映射的地址 → 这就是"3D 写了也没落地"的机制 | 看 `sid=` 是哪条 stream、`addr=` 是多少，和 BO iova 对照 |
    | `tf=0` 且 `tlbmcfg=1`（即当前的 `0x400`） | 可忽略的 TLB 配置故障，与"没写"无因果 | 放弃这条路，转查 3D 初始化/上电 |
    | `sid=0x0` 与 `sid=0x1` 都出现 | 两条 GPU stream 都在碰同一个地址 | 按 SID 区分是 CP 还是 3D 侧 |
- **2026-10-30 GPU 第 312 步：按"3D 不干活"这条线索直接动内核——每笔提交退休后读 GPU 上下文 bank 的 SMMU 故障寄存器（`FSR/FAR/FSYNR`）与全局 FSR，有故障就带地址打印并清掉。这是唯一能一次分清"地址错（被静默丢弃）"还是"压根没执行"的观测点。已出引导镜像 metal54，root 不动**
  - 为什么是它（承接第 311 步）：`NativeAdrenoSmmuContext.cpp` 里 `continue_after_fault=1`——GPU 的越界/未映射访问会被**丢弃**，CP 继续跑、fence 照常退休、什么都不写也不卡死。这正好能解释我们看到的全部现象（CP 包有效、Turnip 的 fill/compute/draw/clear 一个字节都不写）。
  - 这一步的改动（内核，两处，纯诊断）：
    - `iokit/NativeAdreno/NativeAdrenoService.cpp`：bring-up 里已算出的 SMMU 窗口（`smmu_base_`）另存一份全局 `native_adreno_smmu_window`；
    - `iokit/NativeAdreno/NativeAdrenoSubmit.cpp`：新增 `smmuFaultNote(fence)`，在 `wait()`（job 路径与 observeFence 路径）**成功退休后**调用：
      按 bring-up 的 preflight 同一套公式从 IDR1 推出 bank 2 的基址（`(pages+2)*page`），读 `sm[0x48]`（全局 FSR）、
      `cb[0x58]`（FSR）、`cb[0x60]`（FAR）、`cb[0x50]/cb[0x54]`（FSYNR0/1）；非零则 `IOLog("NativeAdreno: smmu fault …")` 并写 1 清掉；
      另外**每 1024 次提交且至今无故障时打一行 `smmu fault probe armed bank2=… gfsr=… fsr=… far=…`**——证明探针真的在跑、且寄存器为 0，
      避免"没打印"被误读成"没有故障"。
  - 产物（**只换引导镜像，root 沿用第 311 步的 `ipad71-root-wrap16-gpu11-wr.img`**）：
    - 内核 `~/liuqin-build3/fakeroot/System/Library/Kernels/kernel.development.liuqin`（20:43 重编，增量成功）；
    - payload `shim/out/payload-metal54-smfault.bin` 25,476,672 B `2138034df2eebf2d9eceadc3f248e275a1f7d14b6a46fae1382dd0f85bfcb488`；
    - 引导镜像 `shim/out/shim-boot-metal54-smfault.img` 32,051,200 B
      `2f51258e1fcd6ca832f754e2b99382294afa6f55c5fab983ae2e752edaa186cf`（cmdline 与 metal53 逐字节相同，587 B；
      镜像内已确认含 `smmu fault probe armed` 字符串）。
  - 下次上机怎么读（三条线，一眼可判）：
    | 日志 | 结论 | 下一步 |
    | --- | --- | --- |
    | 只有 `smmu fault probe armed … fsr=0x0 far=0x0`，没有 `smmu fault` 行 | 3D 的访问**没有**被 SMMU 丢弃 → 它们根本没那么访问过，3D 没执行 | 查 3D 初始化/上电（a730 hw init、GX 域、UCHE/RB/SP 初值）与 CP 的 draw 状态 |
    | 出现多条 `smmu fault … far=0x…` | **3D 在访问错误/未映射的地址**，被静默丢弃 | `far` 就是它在访问的地址：与同期 BO 的 iova 对照，若是 Turnip 的 device address 之外的数 → 查地址翻译；若是 0 或怪值 → 查状态 |
    | 连 `armed` 行都没有 | 探针没跑到（`wait()` 没被调用 / 窗口为空） | 看 `T`/`M` 行是否照常，再决定是否挂到 `observeFence` 或提交路径 |
  上机：只刷这个引导镜像（root 不动），其余流程同前。
- **2026-10-30 GPU 第 311 步：MP13 也读 0——但顺着查下去发现**我这条 GPU 侧读回从一开始就错了**：`nmb_buffer_gpu_address` 返回的是 metal2vulkan 自己的 **Metal 地址空间**（`address.rs`: `SPACE_BASE = 1<<32`），不是 GPU 的 device address；MP11/MP12/MP13 读写的都是虚构地址。**这三行不能作为"GPU 没写"的证据**（第 4 次探针问题）。真正可靠的结论来自 Turnip 自己的冒烟测试：`T s0 r0 v512 c0 g0/4096` 里 **compute 0 个字、triangle 0 个像素**，而这两项是 Turnip 用它自己的地址翻译跑的，不受我的探针影响 → **CP 自己执行的包有效，走 3D 管线的工作一个字节都没写**。下一轮先做不上的代码对照（a730 的 3D 初始化），需要上机时再用内核侧 SMMU 故障寄存器**
  - 事实（`2030.txt` 797,831 B `79800c80545e141b31be50c26a4c7243749ece625180319918c6851d8f762c10`）：
    - `MG2 v2 p60000 c0 e0`——版本行生效，拷贝连接开在 **0x60000000**（slot 1）窗口，BO 分配 + BoMap 全成功；
    - `MP11 v00000000 g00000000 r0`、`MP12 … r0`、**`MP13 v00000000 g00000000 w0 r0`**：
      我们自己的 CP 往那个地址**写**、CPU 和 CP 各读一遍，全是 0——连我们写的字也没落地；
    - 10 个老探针仍全 0；`FS b1e7e f14 s1`、`M s0 c999 r64 i4096 q141 k3/4 p13/8 F14 6601 compute: 999 wrong`；无 `MC` 行；
    - 原始测试 `s34 e0 v4/4`；Turnip 冒烟 `T s0 r0 v512 c0 g0/4096`。
  - 推断（这一轮是**纠错**，不是新结论）：
    - `metal2vulkan` 的缓冲区分两个地址：Metal 侧 `gpu_address()`（`GpuAddress`，`[2^32, 2^47)` 的模拟空间）和真正的
      Vulkan device address（`device_address_of()`，`pub(super)`，未导出）。前端 `-gpuAddress` 给的是前者；
    - 所以 MP11/MP12/MP13 实际是在对一个**虚构地址**做 CP 读写，SMMU 只能报故障/丢弃——它们读到的 0 什么都不说明；
      要真做 GPU 侧读回，必须先让 runtime 导出 device address（给 `metal2vulkan` 加一个小 patch + 桥接导出）；
    - **仍然成立的结论**（不依赖我的探针）：Turnip 冒烟测试里 compute `c0`（16 组 × 64 线程，一个字都没写）、
      triangle `g0/4096`（只有清屏色）——这两项由 Turnip 自己的 stream 和它自己的地址翻译完成，说明**3D 管线（r2d fill/compute/draw/clear）
      不产生内存写**；而 CP 自己执行的包（fence、`CP_MEM_TO_MEM`、原始测试的 512/512）一直是好的；
    - `v512` 这一项**不能单独用来判断**：fill 测试要求"前半 = pattern、后半 = 0xdeadbeef"，共 1024 个字；
      "GPU 什么都没写" 和 "GPU 把整个缓冲都写了" 都会得到 512，必须配合 c/g 两项看；
    - 下一步方向明确：**查 3D 管线为什么不动**。两条线索：①3D 的初始化/上电（a730 的 hw init、GX 域、UCHE/RB/SP 的初值）；
      ②draw 的显存访问是否全在 fault（SMMU `continue_after_fault=1`，故障被静默丢弃——这正好能解释"CP 继续跑、内存没变"）。
  - 下一步计划（先不上机、再上机）：
    - 不上机：把 `~/liuqin-build3/xnu/iokit/NativeAdreno`（**内核真正的源码树，不是 `refs/UniXNU`**）里的 3D 初始化序列
      （`NativeAdrenoA730HwInit.h`、`NativeAdrenoRing.cpp` 的 RB/UCHE 初值）与上游 msm 的 a730 序列逐项对照，找缺项；
    - 上机（需要重编内核 + 引导镜像）：在提交/等栅栏路径里加"每笔提交后读 SMMU 全局 FSR + GPU 上下文 bank 的 FSR/FAR，
      非零就打印并清掉"。若出现 fault，`FAR` 就是 3D 试图访问的地址，能直接指认是地址错还是没执行。
  - 产物：本轮**没出镜像**（gpu11 仍在 `~/liuqin-root/ipad71-root-wrap16-gpu11-wr.img`，sha `f7043229…`；
    它上面的探针结论已被本轮纠错，除非要验证 device address 导出，否则不必再刷）。内核没改，仍 metal53 `830bdf56…`。
- **2026-10-30 GPU 第 310 步：`MP11 v00000000 g00000000 r0`、`MP12 … r0`——当时被读成"GPU 自己读也是 0"，第 311 步已改正：那是 Metal 模拟地址，读数无效。本轮真正的收获是 `MG p60000 c0 e0`：拷贝连接第一次真的建起来了**
  - 事实（`2002.txt` 842,935 B `2eb73c98a0fbd5498359877dcd1a134fdeef67f433168e10299feb7c742b36a2`）：
    - `MG p60000 c0 e0`：拷贝读回自己的连接开在 **0x60000000** 这个窗口（slot 1），BO 分配 + BoMap 全部成功、驱动没有拒绝；
    - `MP11 v00000000 g00000000 r0`（MP1 的 dispatch 输出）、`MP12 v00000000 g00000000 r0`（MP4 的 blit fill 目标）——
      `r0` = 拷贝执行了且自检通过；`g` = **GPU 自己的拷贝**读到的首字，两者都是 0；
    - 10 个老探针仍全 0；`FS b1e7e f14 s1`、`M s0 c999 r64 i4096 q141 k3/4 p13/8 F14 6601 compute: 999 wrong`；
    - 仍然**没有 `MC` 行**（命令缓冲全部 Completed）；原始测试 `s34 e0 v4/4`（backboardd `tdf556af`、SpringBoard `t515b8bed`）。
  - 推断（这次可以定下来）：
    - 自检先往自己的 BO 写了 `0xc0de0000` 并读回验证；随后那次拷贝把 `data[0]` **改成了 0**。
      如果源地址不可读（fault），写不会发生，`g` 会读出 `c0de0000`——所以**源地址确实被 CP 读到了，值就是 0**；
    - 因此：**GPU 侧看到的内存就是 0**，不是映射/缓存问题。blit fill 和 compute 都没有写内存；
    - "命令到了 GPU" 仍然成立（fence 在退休、`CP_MEM_TO_MEM` 我们自己发的也在跑）——所以问题收窄成：
      **CP 的普通写/拷贝会执行，但 Turnip 的 blit/dispatch/draw 不产生任何内存写**；
    - 注意：我们至今"证明好的"那条原始路径（`s34 e0 v4/4`）**全部由 CP 自己完成**（`CP_MEM_TO_MEM`），
      从没走过 SP/TP/RB 这些 3D 块；Turnip 的 fill/dispatch/draw 全都要走 3D 块。所以下一个怀疑对象是
      **3D 块（GX 域 / SP、TP、RB）没上电或没初始化**，而不是 command stream 的编码；
    - 唯一的剩余不确定：`gpuAddress` 给的是不是真正那块内存（若它不是，MP11/MP12 读的零就属于另一块）。MP13 正是为这条准备的：
      如果 CP 能把字写进去、CPU 也读得到，地址就是真的，3D 块的问题立刻成为唯一解释。
  - 这一步的改动（gpu11，仍只加诊断）：
    - `FrontendChecks.h`：`probes()` 多一个可选 `gpuWrite` 回调；新增 **`MP13 v<cpu> g<gpu> w<code> r<code>`**——
      新建 shared buffer（全 0）→ 让 CP 把 `0x5f5e5d5c` **写进它的 GPU 地址** → CPU 读一遍（`v`）→ CP 再读一遍（`g`）；
    - `NativeMetalBridgePhase.h`：把拷贝流抽成 `gpuCopyRun()`/`gpuEnsureChecked()`，新增 `gpuWriteWord()`（反向拷贝）；
      `MG` 行升级为 `MG2 v2 p… c… e…`——**这一行自带版本号**，以后任何日志都能自证是哪个 bundle 出的（第 309 步就是日志归属搞错才白费一轮）；
      连接打通后先用 `Query` 探一次，若连接"开得起来但第一次调用就死"会单独报 `c6`，并自动重连重试一次。
  - 产物：
    - 新 bundle：CDHash `a5478a4f76a19cea3175a6fe9a2844c00974c327`，turnip `1512548c…`，strict 通过、4 KiB 页；
    - 新 root 镜像 `~/liuqin-root/ipad71-root-wrap16-gpu11-wr.img` 17,179,869,184 B
      `f7043229b7a628ea69aa171a606fad6d01120a21821652b32814a4b6213e565f`（= gpu10 镜像 `3332d7c8…` 克隆 + 只换 bundle），
      `fsck_hfs -fn` 卷 OK；
    - zip `~/liuqin-root/ipad71-root-wrap16-gpu11-wr.img.zip` 5,641,617,221 B
      `f76ac301acd180145695ae62178d0536bb112eeef075bf408a896cca68e657d4`；
    - 内核没改，继续 metal53 `830bdf56…`。
  - 下次上机怎么读（就一条：MP13）：
    | 结果 | 结论 | 下一步 |
    | --- | --- | --- |
    | `MP13 v5f5e5d5c g5f5e5d5c w0 r0` | 地址是真的、可写、CPU 也看得见 → **3D 块不干活**成为唯一解释 | 内核侧查 3D 上电/初始化（`RBBM_POWER_STATUS`/`RBBM_STATUS`、GX 域、SP/TP/RB 的 init 序列），并核对 a730 与 a740 的差异 |
    | `MP13 v00000000 g00000000 w0 r0` | 连我们自己写的字都进不去 → `gpuAddress` 给的不是那块内存（或只读映射） | 回头查 Turnip/metal2vulkan 的 device address 与 BO iova 的关系，再重做 MP11/MP12 |
    | `MP13 … w<非0>` | 写那一步就失败了（提交/复制） | 先修通路（`w` 的码与 `MG2` 的 `c`/`e` 一起看） |
- **2026-10-30 GPU 第 309 步：`MG p0 c1 e10000003` 一眼定案——`c1` = 数据 BO 分配失败，`e10000003` = `MACH_SEND_INVALID_DEST`，即这个 `Gpu` 的 mach 端口已经失效：`adrenoUserTest()` 在把 `Gpu` 交给阶段代码之前就 `close(gpu.conn)` 了。不是 GPU 的结论，是探针拿了一条死连接。已出 gpu10：拷贝读回自己开一条连接（开→探 selector→用完关）**
  - 事实（`1903.txt` 1,455,163 B `2b1f3f0a74530c76232e47813ed8b9ec32750b8d9dcfcf51864bf21d723c0776`）：
    - `MG p0 c1 e10000003`、`MP11 … r1`、`MP12 … r1`，**四个进程（backboardd / SpringBoard / chronod / splashboard）完全一致**；
    - 仍然**没有一条 `MC` 行** → 命令缓冲全部 `Completed`（第 308 步的结论保持）；
    - 其余不变：10 个探针全 0（`MP8 s0/64`、`MP9 t0/64`、`MP10 g0/64`）、`FS b1e7e f14 s1`、
      `M s0 c999 r64 i4096 q141 k3/4 p13/8 F14 6601 compute: 999 wrong`、MF 十条照旧；
    - 原始 ABI 测试仍 `s34 e0 v4/4`（backboardd `td603e41`、SpringBoard `t4f84578e`、chronod `t561f665c`、splashboard `t783344e1`）。
  - 推断（定案）：
    - `0x10000003` 是 Mach 的 `MACH_SEND_INVALID_DEST`：`IOConnectCallStructMethod` 的目标端口已死，**任何** ABI 调用都到不了驱动；
    - 根源在插件自己的流程：`adrenoUserTest()` 是自包含的（`NativeAdrenoUserTest.h` 末尾 `close(gpu.conn)`），之后才 `turnipPhase(gpu)` →
      `bridgephase::run(gpu)`，阶段代码拿到的 `Gpu.conn` 是**已关闭的端口名**；
    - 也解释了为什么 `streamText7()` 照样能工作：它走 `mark()`，每次调用都**新开**一个端口（`IOServiceOpen(port, 0x4147xxxx)`，
      被驱动的标记通道接住后立刻拒绝），根本不用 `gpu.conn`。
  - 这一步的改动（gpu10）：
    - `gpuCopySetup()`：用 `origin->openFn(port, 0, 0x4147, &conn)` **自己开一条连接**，再 `probe()` 找沙盒放行的 tunnel selector
      （找不到就退到 mailbox 页面）；BO 分配/映射/提交全走这条新连接；`MG` 行仍用原 `Gpu` 的标记流发（它不需要连接）；
    - 新增 `gpuCopyClose()`：`probes()` 一结束立刻关掉这条连接——驱动**全系统只有 4 个连接槽**，各进程的 Turnip 设备正占着槽位；
    - `MG` 的 `c` 码新增：`3` = 连接开不出来（`e` 为 `IOServiceOpen` 的返回值）、`5` = 既没有 selector 也没有 mailbox 通道。
  - 产物：
    - 新 bundle：exe 22,107,872 B（sha `7fefb6cfd47daf4ec222…`），CDHash `fe334e3c4d6f22aa23e97ddea3c567eb381dcdd8`，turnip `1512548c…`，strict 通过、4 KiB 页；
    - 新 root 镜像 `~/liuqin-root/ipad71-root-wrap16-gpu10-conn.img` 17,179,869,184 B
      `3332d7c800af66f35f16d5c7316e3af268ffef60f24658592f1f73f7e8c0168e`（从校验过的 gpu9 zip `5f653949…` 解回、核对 `ee418a4f…` 一致后只换 bundle），
      `fsck_hfs -fn` 卷 OK；
    - zip `~/liuqin-root/ipad71-root-wrap16-gpu10-conn.img.zip` 5,628,185,541 B
      `575c7dd26d66cdcad8d35b2952c51aaa126fbc83902bbd2b7a2799531d01491b`；
    - 内核没改，继续 metal53 `830bdf56…`。
  - 下次上机怎么读（先 `MG`，再 MP11/MP12）：
    | 结果 | 结论 | 下一步 |
    | --- | --- | --- |
    | `MG p<…> c0` + `MP11 v00000000 g41700000` | **GPU 执行并写了内存**，CPU 映射看不到 | 修 KMD 的 `clientMemoryForType`/映射缓存属性与提交后维护 |
    | `MG c0` + `MP11 v0 g0` + `MP12 g5a5a5a5a` | CP 的 fill 能写、dispatch 不能写 | Turnip 的 dispatch 包/寄存器（`CP_SET_DRAW_STATE`、HLSQ/SP、a7xx 表） |
    | `MG c0` + 两者 `g` 全 0 且 `r0` | 真的没写 | 上内核侧 digest（KMD 在提交前后读 BO） |
    | `MG c3 e<…>` | 连接开不出来（4 槽都被占） | 改成"Turnip 打开前就建好这条连接、全程持有"，或把读回搬进原始测试 |
    | `MG c5` | 没有任何通道（selector 全被沙盒挡） | 查这个进程的 sandbox profile |
    | `MG c1`/`c2` + `e…` | BO 分配被拒，按 `e` 查 |
- **2026-10-30 GPU 第 308 步：gpu8 的 MP11/MP12 报 `rfd`（我的拷贝通路在"建 BO/找地址"这一步就失败了，不是 GPU 的结论），但拿到两个硬事实：**没有任何 `MC` 行 → 每个命令缓冲都是 Completed（runtime 不认为任何一次提交失败）**；前端 `-gpuAddress` 拿得到 Turnip 给的地址（否则连 rfd 都不会出现）。已出 gpu9：地址不再写死——向驱动问这个连接自己的窗口（`NativeAdrenoAbiDeviceInfo.va_start/va_size`，原始测试就是在那儿映射的），并新增 `MG` 行把窗口和驱动的拒绝码打出来**
  - 事实（`1832.txt` 1,407,573 B `52889fdf4fb2a08aa2782ef3aba6e19278e5232aae338c3fe399370a50806387`）：
    - `MP11 v00000000 g00000000 rfd`、`MP12 v00000000 g00000000 rfd`，**backboardd / SpringBoard / chronod / splashboard 四个进程一致**；
      `rfd` 是我这轮定义的"设置失败"码（BO 分配或 BoMap 被拒），说明拷贝通路没建起来，这一轮没有拿到 GPU 侧读数；
    - **全日志没有一条 `MC` 行** → 所有命令缓冲 `status == Completed`：第 1–6、9–12 步的失败**不是**"提交被 runtime 拒绝/失败"造成的；
    - 其余与上一轮完全一致：10 个探针全 0（`MP8 s0/64`、`MP9 t0/64`、`MP10 g0/64`）、`FS b1e7e f14 s1`、
      `MSK 1 visibility`、`M s0 c999 r64 i4096 q141 k3/4 p13/8 F14 6601 compute: 999 wrong`、MF 行十条不变；
    - 每个进程原始 ABI 测试仍 `s34 e0 v4/4`（backboardd `tdc94ae6`、SpringBoard `t50bbf2e6`、chronod `t558a350c`、splashboard `t8c9e2d7a`）；
    - 另见一条 `native-metal: command buffer refused: drawPrimitives: draw: Invalid(… NoPipelineState)`（backboardd 进程里别的 Metal 客户端，不是探针）。
  - 推断：
    - **失败点在我的探针，不在 GPU**：gpu8 把两个 BO 写死在 `0x5f200000/0x5f210000`，而驱动只允许在"这个连接自己的窗口"里映射
      （`NativeAdrenoPublish.cpp` 的 `kClientWindows`：渲染服务器 1 GiB @`0x20000000`，另三个 170 MiB @`0x60000000/0x6AAA9000/0x75552000`），
      四个进程拿到的 slot 不同 → 写死的地址在哪个进程里都可能不在窗口内；
    - 而**原始测试在每个进程都能映射**，因为它用的正是 `GetDeviceInfo` 返回的 `va_start`（`NativeAdrenoUserTest.h` 的 `runTest`）——
      窗口起点是**驱动告诉用户态的**，不需要猜。gpu9 就按这条改；
    - `MC` 全缺席是这轮最有价值的新事实：**runtime 认为每一次提交都成功了**，所以读回全 0 既不是"命令缓冲被拒"，也不是"提交失败"。
  - 这一步的改动（gpu9，仍只加诊断）：
    - `NativeMetalBridgePhase.h` 的 `gpuCopySetup()`：先 `GetDeviceInfo` 拿 `va_start/va_size`，依次试 `va_start`、`va_start+0x2000`、
      窗口末尾；失败再退到 16 个候选地址（四个窗口各取两处）；仍不行才报 `c4`；
    - 新增 **`MG p<page> c<code> e<kern_return>`** 一行（只在第一次拷贝时发）：`p` = 实际成功的基址（页号，一眼看出是哪个窗口）、
      `c` = 设置结果（0 成功、1 数据 BO 分配被拒、2 命令流 BO 分配被拒、4 没有窗口接受映射）、`e` = 驱动返回的拒绝码；
    - MP11/MP12 的 `r` 码含义随之上移（`9` 自检失败、`fa` fence 超时、`fb` submit 被拒、`fc` 包编码失败、`fe` 参数问题）。
  - 产物：
    - 新 bundle：exe 22,107,872 B（sha `755a7b749edbbd3b902d…`），CDHash `6c18b4df5ede5d189a1b69e090aa47ac58691360`，turnip `1512548c…`，strict 通过、4 KiB 页；
    - 新 root 镜像 `~/liuqin-root/ipad71-root-wrap16-gpu9-mg.img` 17,179,869,184 B
      `ee418a4f82351bb4cd59c79740476679ca337628da0ac1f92fbea7bd60462948`（= gpu8 的 `dfc4358c…` 克隆 + 只换 bundle；gpu8 镜像本轮校验仍是原值），
      `fsck_hfs -fn` 卷 OK；
    - zip `~/liuqin-root/ipad71-root-wrap16-gpu9-mg.img.zip` 5,614,905,384 B
      `5f6539490b560cd551ea895a2527d7e553cf60fa20a13b8e0d542e8b60804c0f`；
    - 内核没改，继续 metal53 `830bdf56…`。
  - 下次上机怎么读（先看 `MG`，再看 MP11/MP12）：
    | 结果 | 结论 | 下一步 |
    | --- | --- | --- |
    | `MG p<…> c0` + `MP11 v00000000 g41700000` | **GPU 执行并写了内存**，CPU 的映射看不到 | 修 KMD 的 `clientMemoryForType`/映射缓存属性 + 提交后维护 |
    | `MG c0` + `MP11 v0 g0` + `MP12 g5a5a5a5a` | CP 的 fill 能写、dispatch 不能写 | Turnip 的 dispatch 包/寄存器（`CP_SET_DRAW_STATE`、HLSQ/SP、a7xx 表） |
    | `MG c0` + `MP11`/`MP12` 的 `g` 都 0 且 `r0` | 真的没写 | 查 GPU 执行与地址映射（下一步上内核侧 digest） |
    | `MG c1`/`c2`（带 `e…`） | BO 分配被拒 | 按 `e` 查：`e00002c2` 参数、`e00002d5` 权限、`e…` 资源 |
    | `MG c4` | 连窗口起点都不接受（`p0`） | 说明 `va_start` 也没拿到，需打印 `GetDeviceInfo` 返回值 |
    | `r9`（`MG` 正常） | 拷贝自检失败，通路本身坏了 | 先修通路（自检是拷自己 BO 的已知字） |
- **2026-10-30 GPU 第 307 步：gpu7 上 MP8/MP9/MP10 也全 0——连"直接读 IOSurface 自己的内存"都看不到 GPU 写的东西，所以"Metal buffer 读回坏了"这条解释被排除；剩下的分叉只有"GPU 真没写内存"和"写了、但 CPU 看不到那块内存"。已出 gpu8：MP11/MP12 把同一个 buffer 读两遍（第二遍走 GPU 自己的地址、用插件自己连接的 CP 拷贝），并新增 MC 行报告命令缓冲自己的状态/错误**
  - 事实（`1748.txt` 1,176,998 B `5bb0bcb7813c48048896f51cf6ca9fe361724f93cd35ff3c55cb596d7c0828b7`）：
    - 10 个探针**全部为 0**，包括这次新加的、走 IOSurface 自己内存的三个：
      `MP8 s0/64 x00000000`（清屏进 IOSurface）、`MP9 t0/64 x00000000`（CPU 写好的 buffer → blit 进 IOSurface）、
      `MP10 g0/64 x00000000`（全屏 quad 进 IOSurface）；外加 `MP1 c0`、`MP2 n999`、`MP7 q64/64`、`MP3 p0`、
      **`MP4 f0`（blit fill 对照）**、**`MP5 k0/64`（只清屏对照）**、`MP6 d0/64`；
    - MF 行（每步首个失败原文，3 个进程各一份，内容一致）：`MF1 compute: 999 wrong outputs`、`MF2 render: wrong texels`、
      `MF3 surface render: wrong texels`、`MF4 composition: 64 wrong texels in the sampled corner`、
      `MF5 texture compute: 1012 wrong channels`、`MF6 fill`、`MF9 mode 0: out[24] 0, expected 2`、
      `MFa the GPU wait did not order the copy after the fill`、`MFb heap buffer contents`、`MFc placed buffers overlap`；
    - 报告仍是 `FS b1e7e f14 s1`、`MSK 1 visibility`、`M s0 c999 r64 i4096 q141 k3/4 p13/8 F14 6601 compute: 999 wrong`；
    - 这次**没有卡死**（心跳 `rbbm 0x1`、无 fence 超时、无 SMMU fault 行）；探针那段（0x6600→M 行）里有 **26 次 submit**
      （fence 13→38），说明探针的命令缓冲确实都提交上去了；
    - 每个进程的原始 ABI 测试依旧 `s34 e0 v4/4`（backboardd `tddcbdd1`、SpringBoard `t50db89a0`）。
  - 推断（关键更新）：
    - **IOSurface 的探针是本轮最有价值的一条**：那条内存是宿主任务页经 `BoImport` 导入的，KMD 在每次提交前后对
      imported 页做 clean+invalidate（`NativeAdrenoMemory::maintainImported`），所以"CPU 缓存里是旧行"最多只能解释一部分；
      三个探针全 0 更像是 **GPU 侧没写这块内存**；
    - 但"写了、只是我们看不到"仍不能被证伪：写的位置可能根本不是我们读的那块（Turnip 的 BO/VA 与 CPU 看到的不是一回事）。
      这两者只能靠"**用 GPU 的方式去读 GPU 的地址**"来分开——这就是 gpu8；
    - 另一个从未查过的点：我们一直没打印过 `MTLCommandBuffer` 的 `status`/`error`。命令缓冲若被 runtime 判为 Error，
      `waitUntilCompleted` 照样立刻返回，读回自然全是 0。gpu8 的 `MC` 行补上这个。
  - 这一步的改动（gpu8，只加诊断、不动 ABI/行为）：
    - `bridge/FrontendChecks.h`：`probes()` 多两个可选参数（GPU 读回调 + 上下文）；新增
      **`MP11 v<cpu> g<gpu> r<code>`**（MP1 同一发 dispatch：`v` = CPU 从 buffer 读到的首字、`g` = 通过 GPU 地址拷回来的首字）、
      **`MP12 v<cpu> g<gpu> r<code>`**（MP4 同一发 blit fill 的两种读法）；`dispatch`/`blitTo` 及 MP12 改用 `finish()`，
      命令缓冲没 Completed 时最后发一行 **`MC st<status> <错误原文>`**；
    - `NativeMetalBridgePhase.h`：`gpuReadWords()` —— 在**插件自己的 KMD 连接**上建两个 4 KiB BO（数据/命令流，映射在
      0x5f200000/0x5f210000），每个 dword 发一个 `CP_MEM_TO_MEM`（opcode 0x73，带 WAIT_FOR_MEM_WRITES），把 runtime
      buffer 的 GPU 地址（前端 `NBBuffer -gpuAddress`，即 Turnip 给它的地址）拷进自己的 BO，wait fence 后从 WC 映射读回；
      **先做自检**（拷自己 BO 里 4 个已知字，必须 0xc0de0000 才算通路可用，否则报 `r9`）；
    - 返回码：`0` 成功、`9` 自检失败、`fa` fence 超时、`fb` submit 被拒、`fc` 包编码失败、`fd` BO 设置失败、`fe` 参数问题。
  - 产物：
    - 新 bundle：exe 22,107,872 B，CDHash `5c2be44645f5557a1f6909ed5976b2f32f549f35`，turnip `1512548c…`，strict 通过、4 KiB 页；
    - 新 root 镜像 `~/liuqin-root/ipad71-root-wrap16-gpu8-mp11.img` 17,179,869,184 B
      `dfc4358c9f8d85a488d786b0dadebaba1f3b010a8a7e71785a9e7386d0146056`（= gpu7 + 只换 bundle），`fsck_hfs -fn` 通过（卷 OK）；
    - zip `~/liuqin-root/ipad71-root-wrap16-gpu8-mp11.img.zip` 5,601,678,414 B
      `d24826d6f2467b8b9789a22e65ee3d94859108dfb316837113d314c04c224ff5`；
    - 内核没改，继续 metal53 `830bdf56…`；
    - 说明：gpu7 的 .img 在上机后被宿主改写过（HFS 日志回放，sha 变 `3646c64b…`），本轮是从**校验过的 gpu7 zip**
      （`c0c7cc6e…`）解回来、核对 `8687a35f…` 一致后才改的；随后 gpu7 的 .img/.zip 已被清掉（腾空间），不必再找。
  - 下次上机怎么读（MP11/MP12 是判据，MC 行是旁证）：
    | 结果 | 结论 | 下一步 |
    | --- | --- | --- |
    | `MP11 v00000000 g41700000` | **GPU 执行了、也写了内存**，是 CPU 侧映射/缓存看不到 | 修 KMD 的 `clientMemoryForType`/map 缓存属性（Turnip 的 BO WC 映射）与 post-submit 维护 |
    | `MP11 v00000000 g00000000` 且 `MP12 g5a5a5a5a` | 单线程 dispatch 没写，但 CP 的 fill 写了 → 3D/dispatch 侧问题 | 查 Turnip 的 dispatch 包（`CP_SET_DRAW_STATE`/`HLSQ`/`SP` 配置）与 a7xx 寄存器表 |
    | `MP11`、`MP12` 的 `g` 也全 0 | **GPU 连 CP 拷贝都读不到自己的内存**，或跨连接地址不可见 | 看 `r` 码：`r9` = 我们的拷贝通路坏了；`r0` 而 g 为 0 = 真的没写 |
    | `MC st4 …` / `MC st3 …` | 命令缓冲在 runtime 里就失败了（根本没提交） | 按错误原文查 metal2vulkan/Turnip 的那条路径 |
    | `MP11 g…` 有值但 `r` 非 0 | 拷贝通路本身不稳（超时/被拒） | 先按 `r` 码修通路，别急着解释像素 |
- **2026-10-30 GPU 第 306 步：gpu6 的 7 个探针全 0，连对照项（blit fill、只清屏）也是 0 → 问题不在 shader，而是"经过 Metal frontend 的 GPU 工作，CPU 一个字节都看不到"；同一进程里原始 ABI 测试仍然 `s34 e0 v4/4`（GPU 好的）。已出 gpu7：用 IOSurface 自己的内存绕开 Metal buffer 读回（MP8/MP9/MP10），并把每一步的首个失败原文（MF 行）都打出来**
  - 事实（`1619.txt` 1,064,919 B `c8f7d449d8b45274156560347fcea5b3007341fc930d866c85f4c9f125f324ff`）：
    - 探针：`MP1 c0 w00000000`、`MP2 n999 w00000000`、`MP7 q64/64 w00000000`、`MP3 p0 w00000000`、
      **`MP4 f0 w00000000`（blit fill 对照）**、**`MP5 k0/64 x00000000`（只清屏对照）**、`MP6 d0/64 x00000000`；
    - 报告：`FS b1e7e f14 s1`（失败在 marker 1–6 和 9–12；**第 7 步"编码器拒绝"和第 8 步 QuartzCore 翻译是过的**）、
      `MSK 1 visibility`、`M … F14 6601 compute: 999 wrong`；
    - 同一次启动里**每个进程的原始测试都是 `s34 e0 v4/4`**（backboardd / SpringBoard / chronod / Screenshot），
      心跳末尾 `cp rptr 4729 wptr 4729 ib1rem 0 rbbm 0x1` → GPU 空闲、**这次没有卡死**（可见性 pass 已摘掉）；
    - `T s0 r0 v512 c0 g0/4096` 仍是老样子（8 次）。
  - 推断（重要）：
    - **不是 shader 的问题**：连 CP 自己就能干的 blit fill（`MP4`）和只清屏（`MP5`）都读回 0；
    - **命令确实到了 GPU**：前几轮 kernel 的 `timed-out IB` dump 里是真实 PM4（`70970001 08000003 …`），fence 也在退休；
    - **原始 ABI 的 fill 512/512 是好的** → KMD/CP/内存方向本身没问题；差别只在"Turnip/前端的 BO 内存"这一侧；
    - 所以现在的首要问题变成："GPU 到底写没写、写到哪、CPU 读的是不是同一块内存"。gpu7 就是为这三个问题设计的。
  - 这一步的改动（gpu7）：
    - **MP8/MP9/MP10 绕开 Metal buffer 读回**，全部直接读 IOSurface 自己的内存（`IOSurfaceGetBaseAddress`）：
      `MP8 s<n>/64` = 只清屏到 IOSurface（pass 执行 + GPU 写内存可见？）、
      `MP9 t<n>/64` = CPU 写好的 buffer → blit 到 IOSurface（**CPU 写的数据 GPU 看得见吗**）、
      `MP10 g<n>/64` = 全屏 quad 到 IOSurface（**draw 真的光栅化了吗**）；
    - **MF 行**：`fail()` 现在给每个 marker 记下**第一个失败的原文**，report 里连发 `MF<marker> <54 字>`
      （最多 12 条）——一次就能看到第 1–6、9–12 步各自抱怨什么（例如第 3 步是"surface render: wrong texels"还是"IOSurface textures"）。
  - 产物：
    - 新 bundle：exe 22,107,760 B，CDHash `43743e9170cfea7c1c6a1dc06e8e8cd393f2d756`，turnip `1512548c…`，strict 通过、4 KiB 页；
    - 新 root 镜像 `~/liuqin-root/ipad71-root-wrap16-gpu7-mf.img` 17,179,869,184 B
      `8687a35f250d7608f890f5e3587af5b064b615004b11d5ff336aa5d88f2c4d53`（= gpu6 + 只换 bundle），`fsck_hfs -fn` 通过；
    - zip `~/liuqin-root/ipad71-root-wrap16-gpu7-mf.img.zip` 5,588,370,837 B
      `c0c7cc6e6eb5c5d8f7c20d01e90796fbbc6f127c229d0053fcada556c0186560`；
    - 内核没改，继续 metal53 `830bdf56…`。
  - 下次上机怎么读（先看 MP8/MP9/MP10，再看 MF）：
    | 结果 | 结论 | 下一步 |
    | --- | --- | --- |
    | `MP8 s64/64` + `MP10 g64/64` | render pass 和 draw **都执行且内存可见** | 问题只在 Metal buffer 的读回/内存类型 → 查 runtime 的 buffer 分配与 Turnip 的 BO 映射 |
    | `MP8 s64/64` + `MP10 g0/64` | 清屏执行、**draw 不光栅化** | 查 Turnip 的 a7xx 绘制状态（BV/BR、`CP_SET_DRAW_STATE`、viewport） |
    | `MP8 s0/64` + `MP9 t0/64` | 连 pass 都没执行 / GPU 写不到 IOSurface | 看 MF 里第 3 步的原文，判断是 IOSurface 导入还是执行 |
    | `MP9 t64/64` 但 `MP4 f0` | **CPU 写的数据 GPU 能读**，但 buffer 读回是坏的 | 锁定 buffer 内存类型（HOST_VISIBLE/WC）这一条 |
- **2026-10-30 GPU 第 305 步：gpu5 的"卡住也报告"生效——拿到 `Fs 660d f14` + 第一个失败「compute: 999 wrong outputs」；999/1000 恰好是"输出缓冲没被写过"的形状（期望 3*i，只有 i=0 的 0 对得上），所以下一个假设是"**任何 shader 工作都没执行**"。已出 gpu6：7 个探针一次上机同时验 7 个假设，并把卡死 CP 的可见性 pass 先摘掉**
  - 事实（`1546.txt` 720,448 B `72ad0acf496376f2b00b7d387129d8aa1bdae59b0d342d1b06c6ba2dfbd847ba`）：
    - marker 0x6601…0x660d 全到，0x660e 没到 → 和上一轮一样卡在**可见性 pass**（fence 28、marker 660d→660e 之间）；
    - `MW 660d 90s f14 6601 compute: 999 wrong ou`（看门狗行带上了失败数）、`MS stuck 660d f14`、
      `MS1 6601 compute: 999 wrong outputs`、`M s0 c999 r64 i4096 q141 k3/4 p13/8 Fs 660d f14`；
    - 也就是说：**第 1–10 步里一共 14 条 CHECK 失败，第一个是第 1 步的 compute**；`MA abi _MTLDevice size 528 (536 on 17.7.11)` 也照常出现；
    - 卡死位置与前一轮一模一样（fence 28 永不退休、心跳 `cp rptr 1240 … ib1rem 93`）→ 可复现。
  - 推断（关键）：第 1 步的检查是 `out[i] = 3*i`、输出缓冲新建即全 0；**999 错 = 只有 out[0]（期望 0）对**，正是"缓冲一个字都没被写过"的形状。
    同一形状也出现在 T 行：`c0`（compute 正确字 0）、`g0/4096`（三角形 0 正确、4096 杂散 = 只剩清屏色）。
    所以最可能的根因不是"算错"，而是"**draw/dispatch 根本没执行**"（CP 的 fill/blit 都是好的）。
  - 这一步的改动（gpu6，两个都属于"多验假设"）：
    - **7 个探针**（`bridge/FrontendChecks.h` 的 `frontendchecks::probes()`，在检查之前跑，每个探针一有结果就立刻单独发一行，
      这样即使后面的探针把 CP 卡死，前面的结果也已经进日志）：
      `MP1 c<n> w<hex>` 单线程 dispatch 写 shared 缓冲（期望 out[0]=15）、
      `MP2 n<bad> w<hex>` 第 1 步同形（1000 线程 / 每组 64）、
      `MP7 q<bad>/64 w<hex>` 一个工作组（64 线程）——把"单线程能跑"和"一个工作组能跑"分开、
      `MP3 p<n> w<hex>` 单线程写 **private** 缓冲再 blit 回来（第 2 步的读回路径）、
      `MP4 f<n> w<hex>` blit fill（对照：CP 自己写内存，T 行 512/512 已证明是好的）、
      `MP5 k<n>/64 x<hex>` 只清屏的 render pass（像素等于清屏色 → pass 执行了）、
      `MP6 d<n>/64 x<hex>` 全屏 quad 的 render pass（像素等于 quad 色 → **draw 真的光栅化了**）；
    - **摘掉可见性 pass**（`#define NMB_VISIBILITY_PASS 0`，代码留着）：它每次都卡死整颗 GPU，让整次启动的 GPU 数据全废；
      现在按"跳过"计入报告（新行 `MSK <n> visibility`，M 行旁边 `FS b<bitmap> f<failed> s<skipped>`），**不算通过**。
      另外新增 `FS b%04x f%u s%u` 行：bit i = marker 0x660i 下有失败，一次给出"哪些步失败"，不再只有第一个失败的文字。
  - 产物：
    - 新 bundle：exe 22,091,248 B，CDHash `d479c2b56fabd91e1a9e501e8c5fb9b6f2c41ae3`，turnip `1512548c…`，strict 通过、4 KiB 页；
    - 新 root 镜像 `~/liuqin-root/ipad71-root-wrap16-gpu6-probes.img` 17,179,869,184 B
      `d827328a9e4da5898d47b270412467804318e7369dad7fbf506b2bc308ad8db6`（= gpu5 + 只换 bundle；从 gpu5 的 zip 解回来、
      核对 sha256 `254d5c2c…` 一致后再改），`fsck_hfs -fn` 通过；
    - zip `~/liuqin-root/ipad71-root-wrap16-gpu6-probes.img.zip` 5,575,160,550 B
      `cac7bb1ab350ba708fd994c46a5dd62557855629c17b2f15fa2d100da3706fb3`；
    - 内核没改，继续 metal53 `830bdf56…`。
  - 下次上机怎么读（一次拿到 7 个假设的答案）：
    | 行 | 含义 | 读法 |
    | --- | --- | --- |
    | `MP4 f1` | CP 写内存 + 读回 | 对照，应该一直是 1 |
    | `MP5 k64/64` | 只清屏的 pass | 64 = pass 执行；0 = pass 都没跑；1..63 = viewport/scissor 有问题 |
    | `MP6 d64/64` | 全屏 quad 的 draw | 64 = draw 光栅化正常；0 = draw 完全没跑；1..63 = 只覆盖一部分 |
    | `MP1 c1 w41700000` | 单线程 dispatch | 1 = compute 通；0 = 连一个线程都不写 |
    | `MP7 q0/64` | 一个工作组 | 0 = 一个工作组也全对；64 = 一个线程都没跑 |
    | `MP2 n999 w0` | 1000 线程（已知） | 与 MP1/MP7 对比定位"网格大小"问题 |
    | `MP3 p1` | private 缓冲 + blit | 1 = shader 写 GPU 内存后能读回；0 但 MP1=1 → shared 路径问题 |
    | `FS b.... f.. s1` | 哪些步失败 | bit1 = 第 1 步…；`s1` = 可见性 pass 被跳过 |
  - 预期：如果 `MP4=1 MP5=64/64 MP6=0 MP1=0 MP7=64`，就证明"**只有 clear/fill 执行，draw/dispatch 全不执行**"，
    下一步就该查 Turnip 的 a7xx 计算/绘制启动序列（`CP_SET_DRAW_STATE`/`HLSQ`/`SP` 的 shader 上传与 `CP_THREAD_CONTROL`），
    而不是继续猜 shader 内容。
- **2026-10-30 GPU 第 304 步：放行 ABI 之后 frontend 真的打开了——Metal 检查跑完第 1–10 步，第 11 步的"可见性查询"那次提交把 CP 卡死（fence 28 永不退休）；而旧代码只在 worker 结束后才写 M 行，所以整份报告丢了。已把"卡住也要报告"和 step 11 的细分 marker 做进 gpu5**
  - 事实（`1518.txt` 839,239 B `4cb3602adc2b412799f4599049fd75cd3ae57d09d2f4d22ae5d6b9ee5ed7cbc6`，启动到 ~280 s，内核仍是 metal53）：
    - frontend 起来了：`turnip marker 0x6600`（line 4482）之后 0x6601…0x660b 依次出现 → `native_bridge_metal_open` **这次返回了设备**
      （528 放行有效，第 303 步的目标达成）；全日志只有 **1 次** 0x6600（别的进程没走到 frontend，见下）；
    - **没有 M 行、没有 MA 行**：`run()` 在 `pthread_join` 之后才发这些行，而 worker 卡在 GPU 等里 → 一行都没出来；日志里只有
      `NativeAdrenoMetal: MW 660b 90s`（spot-check 行；30 s 那条因为循环用 `==` 判等被跳过了）；
    - backboardd 的提交：fence 13（Turnip 自测最后一次，`fence 528/12` 那类"慢一跳"）之后，**fence 14–25（第 1–10 步）全部退休**，
      fence 26/27（step 11 的两个 heap fill）也退休；**fence 28 永不退休**：
      `fence timeout 28 … cp rptr 1240 wptr 1260 … rbbm 0x800005 … fence 1240/27`；
    - 卡死位置：`cp ib1 0x2809c000 rem 93` —— fence 28 是 4 个 chained IB 的提交，前 3 个过了，卡在第 4 个（221 dwords，走了 128）；
      beat 2..13 的心跳一直是 `cp rptr 1240 wptr 1443 ib1rem 93 rbbm 0x800005 fault 0` → CP 停在那条 IB 里（beat 2 时已经卡住，~40 s 内）；
    - 其它进程（SpringBoard/chronod/splashboard）的**原始测试**变成 `s11 e0xe00002d6 v-1/-2`：`0xe00002d6 = kIOReturnTimeout`，
      是 CP 被卡死后的连带结果（WaitFence 超时），不是新问题；它们因此没进 Turnip/frontend 阶段；
    - 显示/合成仍在跑（软件渲染）：`swap 749→1148`、`vsync 14301→16430`、`panel present 1`、`lockbusy 0`。
  - 推断：
    - **第 1–10 步的通过/失败数仍然未知**（报告丢了）——这是下一次上机要拿的东西；
    - 卡死点是 step 11 的第三次 `finish`：render pass + `setVisibilityResultMode:Boolean` + `setDepthBias` + 私有 RGBA8 目标。
      可见性查询在 a7xx 上要 BV 线程发 `ZPASS_DONE`、BR 线程等它，和这个 port 之前遇到的"BV/BR 互相等"
      （第 261–265 步、int-261p/262/265）是同一类问题，不是新谜团；
    - 也就是说：**"frontend 打不开"这条门已经拆掉**，下一个问题是"可见性查询那次 pass 会不会把 CP 卡死"，
      再往后才是 bridge 自测报的 compute/render/import 全错。
  - 这一步的改动（只为"卡住也拿得到数据"，不改任何 GPU 行为）：
    - `NativeMetalBridgePhase.h`：看门狗行带失败数（`MW <marker> <s>s f<failed> <首个失败前 26 字>`）；判等 `==` 改 `>=`（30/90 s 必发）；
      **120 s 还没完成就不再 join**：发 `MS stuck <marker> f<failed>` + `MS1/MS2 <首个失败全文>`，M 行末尾新增
      `Fs <marker> f<failed>`；卡住时不做 scanout、不 `delete job`（worker 还在用它）；
    - `bridge/FrontendChecks.h`：step 11 加三个 marker —— `0x660c`（automatic heap 的 fill 退休后）、`0x660d`（placement heap 两次 fill 后）、
      `0x660e`（可见性 pass 退休后），把"卡在哪个子步"定位到一条。
  - 产物：
    - 新 bundle：exe 22,091,248 B，CDHash `7eedcdd68bee312ac90d8a29829117d6a536721c`，turnip `1512548c…`，strict 通过、4 KiB 页；
    - 新 root 镜像 `~/liuqin-root/ipad71-root-wrap16-gpu5-partial.img` 17,179,869,184 B
      `254d5c2cac157bfa82adc6acba905372e9d998fe2c3ca5255f655b755070b95b`（= gpu4 + 只换 bundle；gpu4 的 .img 又被清掉了，
      从 `…gpu4-abi528.img.zip` 解回来、先核对 sha256 `1cb090b1…` 一致后再改），`fsck_hfs -fn` 通过；
    - zip `~/liuqin-root/ipad71-root-wrap16-gpu5-partial.img.zip` 5,562,079,001 B
      `258a87903fa044a7fd94f4068fc6b17cf703d4a74d2b1e3e4074bdf14d442c58`；
    - 内核没改，继续 metal53 `830bdf56…`。
  - 下次上机看什么（这次**一定**会有 M 行）：
    - `MW 660b 30s f<k> …` / `MW 660b 90s f<k> …`：**k = 第 1–10 步累计多少条 CHECK 失败**（0 = 全过）；
    - `MA abi _MTLDevice size 528 (536 on 17.7.11)`（卡住也会发）；
    - `MS stuck <marker> f<k>` + `MS1/MS2`：标记具体卡在哪个子步、第一个失败的全文；
    - `M … Fs <marker> f<k>`：M 行尾巴这次是 `Fs`（stuck），不是 `F0`/`F<n>`。
  - 预期：新 marker 停在 `0x660d`（heap fill 都过、可见性 pass 卡住）——如果停在 0x660b/0x660c，则说明卡点更早，按 marker 调。
- **2026-10-30 GPU 第 303 步：gpu3 上机给出答案——`Fo` 就是 `_MTLDevice` ABI 尺寸断言：iPadOS 16.6.1 的 `_MTLDevice` 是 **528 B**，代码按 17.7.11 的 **536 B** 断言；诊断行 4/4 进程全部落盘；metal53 的门铃补写第一次被证实救回 fence。已放行 528/536 出 gpu4，等下一次上机看 frontend 能否打开**
  - 事实（`1448.txt` 871,825 B `ecd9e6f0b39f38d15babcf992802f618e6810f34c01b93be019732976876f36f`，启动到 ~280 s，内核 = metal53：有
    `fence 7 retired after a repeated WPTR write`）：
    - `NativeAdrenoMetal: MA abi _MTLDevice size 528, want 536` **4 次**，对应 backboardd / SpringBoard / chronod / splashboard
      （用它们各自的 `… s34 e0 v4/4 …` SetMetadata 行对出）；
    - `MA1 abi: _MTLDevice instance size is 528, the frontend expec` + `MA2 ts 536 (the iPadOS 17.7.11 layout)`：全句按 56 字/行切开，
      第二段开头的 `ts` 是 `expects` 的后半截——符合设计；
    - `NativeAdrenoMetal: M s0 c999 r64 i4096 q141 k3/4 p13/8 Fo abi 528!=536` **4 次**（上一次启动只有 1/4 条 M 行落盘）；
    - `MEND` 的文字本身 0 次：所有进程共用一个 `report` 缓冲，它随后被下一个行首清掉/合并；但它的作用达到了——4 条 M 行全部落盘；
    - `NativeAdreno: fence 7 retired after a repeated WPTR write (value 7)` ×1 → **metal53 的补写 WPTR 真救回了一条 fence**，
      第 299 步"命令流没丢、门铃丢了"的假设第一次有实证；但本轮 40 次 submit 里 4 次 fence timeout（fence 1/8/13/17），
      只救回 1 条——**门铃丢失是真的，但只是少数**，另外 3 条补写 WPTR 之后仍然超时（CP 确实没干完）；
    - 心跳 beat 1..5：`cp rptr/wptr 539→1543→2045`、`rbbm 0x1`、`fault 0`、`panel present 1`、`lockbusy 0`、**swap 7→205**、vsync 7349
      （上一次启动 swap 停在 131/132）→ 这次合成没有停；
    - `T s0 r0 v512 c0 g0/4096` ×8，与之前一字不差；
    - `LIUQIN: trust caches unavailable (Image4/AMFI not installed)` → 第 302 步"插件加载不走 trust cache"的推断成立。
  - 推断（有依据）：
    - frontend 打不开**只有**这一条原因。另外三条拒绝路径（device 打开、`objc_allocateClassPair`、`[cls new]`）都没有触发，
      否则 `MA1` 会是它们的原因而不是 ABI 句；
    - 528 vs 536 差 8 B（17.x 在末尾多一个 ivar）。本 port 的动态子类**不加 instance variable**、方法状态全在文件级全局里，
      基类布局只被"本文件没实现的基类方法"使用，而那些方法在 16.6.1 的对象上用它自己的布局，是自洽的 →
      **放行 528 在构造上是安全的**（不是"试试看"）。
  - 这一步的改动（放行）：`_MTLDevice` 检查从 `!=536 就拒` 改成 `既非 536 也非 528 才拒`（其它尺寸仍然拒绝并报出来，保留 tripwire）；
    `MA` 行改成 `_MTLDevice size 528 (536 on 17.7.11)`；frontend 其它行为一律没动。
  - 产物：
    - 新 bundle：`NativeAdrenoMetal` 22,091,184 B，CDHash `c28f4dbc63f71dd8d670598dd231ce4cf845f0b7`，turnip `1512548c…`，
      `codesign --verify --strict` 通过、4 KiB 页；
    - 新 root 镜像 `~/liuqin-root/ipad71-root-wrap16-gpu4-abi528.img` 17,179,869,184 B
      `1cb090b12fa4234302d8af45f86d89c1c0e187d503a8c585b521bb937d5c781f`（= gpu3 + 只换 bundle 里的可执行文件；**gpu3 的 .img
      在本轮上机后被清掉了**，所以这次是从 `…gpu3-abi.img.zip` 解回来、先核对 sha256 与第 302 步一致后再改的），`fsck_hfs -fn` 通过；
    - zip（adb push 用）`~/liuqin-root/ipad71-root-wrap16-gpu4-abi528.img.zip` 5,548,730,933 B
      `f19cffc7c562cc3888adc83bf96034c9b838bbb8202135214c6fabd4d476e941`；
    - 内核没改，继续 metal53 `830bdf56…`。
  - 下次上机看什么（frontend 的结论第一次会出现）：
    - `MA abi _MTLDevice size 528 (536 on 17.7.11)`，不应再有 `!=536/528`；
    - M 行行尾**不该再有 `Fo`**：`F0` = frontend 全套通过；`F<n> <marker> <first failure>` = 检查失败（marker 0x6601..0x660f 定位步）；
      `Fx <reason>` = 设备/库没起来；同时看 `FM <class> <selector>`（缺哪个 selector）；
    - `M s… c… r… i…` 这几个数仍是 bridge 自测的（compute 999/1000 错、render 64 错、import 4096 错），
      **frontend 打开之后才回到 shader/dispatch 那条线**——那才是"GPU 能用"的下一个关口。
- **2026-10-30 GPU 第 302 步：frontend 打不开的原因现在会自己进日志——三处"只诊断"上报（`MA` 行、M 行 `Fo` 后面的短码、`MEND` 结束行）；装好新 root 镜像 gpu3，等一次上机**
  - 改动（**全部是诊断：所有 `return nil` 的返回值、调用顺序、失败分支与之前完全一样**）：
    - `NativeBridgeMetalFrontend.mm`：`native_bridge_metal_open()` 里原先只写 os_log 的拒绝，现在同时记进三个新导出
      `native_bridge_metal_open_reason()`（短，喂 M 行）/`_detail()`（全句，喂 `MA` 行）/`_abi()`（`_MTLDevice` 实测尺寸，
      **检查通过也记**）。四处：device 打开失败、`_MTLDevice` 尺寸检查、`objc_allocateClassPair`、`[cls new]`。
      `NativeBridgeMetalFrontend.h` 加了这三个声明。
    - `NativeMetalBridgePhase.h`：`frontend()` 失败时优先取 frontend 的原因（为空才退回 `nmb_last_error()`）；
      `run()` 在 M 行**之前**多发一行 `MA abi ...`（每次发，不论成败）和最多 3 行 `MA1..MA3`（全句原因，56 字/行），
      M 行**之后**发一行 `MEND <进程名>`。
    - `NativeAdrenoMetalPlugin.mm`：`PUB no device: ...` 和 listed client 的失败原因同样先取 frontend 的原因。
    - **没有动 Rust bridge**：`nmb_last_error()` 是 Rust 的 thread-local、没有 setter，加 setter 要重编那份 308 MB 的
      `libnative_metal_bridge.a`。改用 frontend 自己的三个导出（"等价机制"），phase 优先读它、为空才退回 `nmb_last_error()`。
  - 复现编译命令（本机已验证）：
    `cd ~/liuqin-root/refs/UniXNU && PATH=/Users/qprs/liuqin-metal/bin:/Users/qprs/liuqin-metal/cargo/bin:$PATH
    RUSTUP_HOME=/Users/qprs/liuqin-metal/rustup CARGO_HOME=/Users/qprs/liuqin-metal/cargo python3 scripts/build-adreno-metal-plugin.py`
  - 为什么必须加 `MEND`（有证据）：内核只在新行开始时才 `IOLog` 上一行（`NativeAdrenoPublish.cpp:631`），而 kernel 只有**一个**
    `report` 缓冲、所有进程共用。`1336.txt` 里 `turnip marker 0x6600`（frontend 起点）出现 **4 次**，`NativeAdrenoMetal: M ` 只有 **1 行**
    → 另外 3 个进程的 M 行在落盘前被覆盖了。`MEND` 紧跟 M 行，几微秒内把它顶进日志。
  - **下次日志要读的三行**（本轮的答案）：
    - `NativeAdrenoMetal: MA abi _MTLDevice size <N>, want 536` → 16.6.1 的实际尺寸 N；
    - `NativeAdrenoMetal: M s0 c999 r64 i4096 q141 k3/4 p13/8 Fo abi <N>!=536` → 尺寸拦截被证实；
      若是 `Fo abi no _MTLDevice` / `abi no subclass` / `abi alloc failed` / 别的串，全句在 `MA1..MA3` 里；
    - `NativeAdrenoMetal: MEND <进程>` → 这条 M 行确实落进日志了。
  - 主机侧已确认的事实（**不是上机结果**）：
    - `build-adreno-metal-plugin.py` 在本机跑通。需要：`PATH` 含 `/Users/qprs/liuqin-metal/bin`（glslc、llvm-as）、
      `RUSTUP_HOME=/Users/qprs/liuqin-metal/rustup`、`CARGO_HOME=/Users/qprs/liuqin-metal/cargo` 且其中的 `bin` 在 PATH。
      不带这些会分别死在 `gen-turnip-smoke-shaders.py`（`FileNotFoundError: glslc`）和 rustup（no default toolchain）。
      `gen-turnip-replay.py` 仍失败（缺 host Turnip，"Build the host Turnip first"），脚本按设计跳过 →
      `TurnipReplay.h` 不生成（只影响 replay 诊断，与本次路径无关）。
    - 新 bundle：`NativeAdrenoMetal` 22,091,184 B（比旧的多 192 B），CDHash `03d1410e158392f709a3adc8a89f11794c83fb80`，
      turnip CDHash `1512548c…`，probe daemon `332e689d…`；`codesign --verify --strict` 通过、`Page size=4096`。
    - 新 bundle 里的 Turnip 与**设备上现有那份代码逐字节相同**：`__TEXT` 只有第 0 页 28 B 不同（`LC_CODE_SIGNATURE`
      的 offset/size 与 `__LINKEDIT` 的 size 字段），`__DATA_CONST`/`__DATA` 前缀相同 → 换 bundle 不改变 Turnip 行为。
    - 信任链：metal53 的 payload 和 adt 里搜不到任何插件 CDHash（连 10-28 的 `cad1f66a` 都没有），而 10-29 15:30 装的
      `2798aa5a` 那份 bundle 在 10-30 13:37 的启动里确实被加载了 → **这个 root 的插件加载不过 AMFI 信任检查**
      （`amfi_get_out_of_my_way=1` + `cs_enforcement_disable=1`），换签名不用动 trust cache（推断，依据即这两条）。
  - 产物（sha256）：
    - 新 root 镜像 `~/liuqin-root/ipad71-root-wrap16-gpu3-abi.img` 17,179,869,184 B
      `85ff0632ce3c3aa0346e23b135cfff28e32e9cbc1c3a69b78d69bb39a40bdad1`
      = gpu2 的完整拷贝 + 就地覆盖 `/System/Library/Extensions/NativeAdrenoMetal.bundle` 的 4 个文件；
      与 gpu2 的字节差异只有 bundle 数据区约 39 MB + HFS 目录/日志区约 1 MB；`fsck_hfs -fn` 通过，卷 OK。
    - zip（adb push 用）`~/liuqin-root/ipad71-root-wrap16-gpu3-abi.img.zip` 5,535,656,453 B
      `208cdec140e47cebb2020ebc81a2dc02ccfb8394f7f514ac3c2fa98ef6ade8b9`；
    - 内核**没有改**：继续用 `shim/out/shim-boot-metal53-doorbell.img` 32,051,200 B
      `830bdf56e2a419adc261271b8dc28f4a53ca53a0f4bc25e85803c7eca1d755cb`。
  - 装进镜像的做法（下次可直接照做）：`cp -c` 克隆 gpu2 → `hdiutil attach -readwrite -owners on` →
    **就地覆盖** `NativeAdrenoMetal.bundle` 里的 4 个文件（`NativeAdrenoMetal`、`libvulkan_freedreno.dylib`、`Info.plist`、
    `_CodeSignature/CodeResources`）——不能删整个 bundle 目录，`/System/Library/Extensions` 是 root:wheel 755；
    `chmod 755/644` → `codesign --verify --strict` → detach → `hdiutil attach -nomount -readonly` + `fsck_hfs -fn`。
    装完的文件属主是 501:20，与之前装的那份一致。
  - 上机步骤（root 只在设备当前就是 gpu2/wrap16 时才能整份 dd；不确定就别动 userdata）：
    1. `adb push ~/liuqin-root/ipad71-root-wrap16-gpu3-abi.img.zip /sdcard/` → 设备上解压 →
       `adb shell su -c 'dd if=/sdcard/ipad71-root-wrap16-gpu3-abi.img of=/dev/block/sda36 bs=4M conv=fsync'`；
    2. `fastboot boot shim/out/shim-boot-metal53-doorbell.img`；
    3. 等 5–6 分钟，用 `tools/liuqin-log-android-pull.sh` 拉日志。
  - 下一步（等日志再决定）：N≠536 → frontend 打不开就是这条 ABI 检查，再决定"放行尺寸检查（子类不加 instance variable，
    16.x 布局可能兼容）"还是"改布局"；N=536 → 按 `MA1..MA3` 的原因继续。**在这之前不要动 shader/dispatch 那条线。**
- **2026-10-30 GPU 第 301 步：`Fo` 的原因查清了——`native_bridge_metal_open()` 在设备打开**之后**的几处 `return nil` 只写 os_log、不设 `nmb_last_error()`，所以 `job->error` 是空串。最可能命中的是 `NativeBridgeMetalFrontend.mm:1558` 的 `class_getInstanceSize(base)!=536`（注释写的是 iPadOS **17.7.11**，本 root 是 **16.6.1**）。下一步是"只加诊断、不改行为"的一次上机**
  - 相关行（`~/liuqin-root/refs/UniXNU/drivers/NativeMetal/`）：
    - `NativeMetalBridgePhase.h:46-48`：`frontend()` 调 `native_bridge_metal_open(...)`，失败就从
      `nmb_last_error()` 拷 `job->error`；
    - `NativeBridgeMetalFrontend.mm:1542-1560`：`native_bridge_metal_open` 里设备打开成功后仍有
      `return nil`（`_MTLDevice` ABI 检查等），这些路径**不设** `nmb_last_error()`；
    - `NativeMetalBridgePhase.h:176-184`：`M` 行的 `Fo/Fx/F<n>/F0` 分支。
  - 已量化的事实（第 300 步）：`M s0 c999 r64 i4096 q141 k3/4 p13/8 Fo` =
    compute 1000 错 999、render 64 全错、import 4096 全错、QuartzCore 141 函数
    （kernels 3 成 4 败、配对 13 成 8 败）、frontend 未打开；
    `T s0 r0 v512 c0 g0/4096` = Vulkan 全成功、填充 512/512 对、compute 0 字、
    三角形 0 正像素 / 4096 杂散像素 → **内存通路是好的，错在 shader/dispatch/render 状态**。
  - 本轮决定的最小实验（只加诊断，不改 ABI 行为）：
    ①在那些 `return nil` 前把原因写进 `nmb_last_error()`（或 bridge 的文本流 `0x7fff/0x7ffe`），
    让 `Fo` 后面出现字；
    ②把实际 `class_getInstanceSize(base)` 打出来（16.6.1 到底多少）；
    ③把这条信息同时推进 kernel 可见的文本通道（`missing()`/`streamText`），不用连控制台。
    重编用 `~/liuqin-root/refs/UniXNU/scripts/build-adreno-metal-plugin.py`（+`build-native-metal-bridge.py`、
    `add-gpu-plugin-data.py` 装回 root）。
  - 待刷的内核镜像（已生成）：`shim/out/shim-boot-metal53-doorbell.img` 32,051,200 B
    `830bdf56e2a419adc261271b8dc28f4a53ca53a0f4bc25e85803c7eca1d755cb`
    （内核 `defc5ca3…`，payload `2b45f0d2…`；= metal52 的显示修复 + fence 超时时补写 WPTR）。
  - 下一位接手的第一件事：读第 300/301 步 + `docs/METAL_PORT.md` 的 Stage 3/4，
    然后决定是"先刷 metal53 看 fence"，还是"先重编带诊断的 bundle 一起刷"。
- **2026-10-30 GPU 第 300 步：`M` 行和 `T` 行的含义查到了（源码在 `~/liuqin-root/refs/UniXNU/drivers/NativeMetal/`），GPU 的失败第一次被量化：**fill 通过，compute/triangle 全错**——毛病在 shader/dispatch/render 状态，不在内存通路**
  - 格式串：`NativeMetalBridgePhase.h:176`
    `"M s%x c%u r%u i%u q%u k%u/%u p%u/%u"`，字段含义在
    `bridge/NativeMetalBridge.h:12`：`c`=compute 错（满分 1000）、`r`=render 错（满分 64 = 8x8）、
    `i`=import 错（满分 4096 = 64x64 主机页）、`q`=QuartzCore 函数数、`k`=kernels ok/失败、
    `p`=vertex/fragment 配对 ok/失败。行尾 `Fo`/`Fx`/`F<n>`/`F0` 由 `job->code`/`frontendOpened`/
    `frontendRan` 决定（`Fo` = frontend 从没打开，后面跟 `job->error`，我们这里是空串）。
    我们一直是 `M s0 c999 r64 i4096 q141 k3/4 p13/8 Fo`：**1000 个 compute 输出错 999、
    64 个 texel 全错、4096 个导入页 texel 全错**，QuartzCore 的 141 个函数能编（kernels 3 成 4 败、
    配对 13 成 8 败），但 frontend 根本没起来。
  - `T` 行格式：`NativeAdrenoUserTest.h:566` `"T s%d r%d v%d c%d g%d/%d"` =
    stage / vkResult / 填充验证字数 / compute 字数 / 三角形像素 / 杂散像素。
    我们一直是 `T s0 r0 v512 c0 g0/4096`：**vkResult=0（Vulkan 调用全成功）、填充 512 字全对、
    compute 0 字、三角形 0 个正确像素却有 4096 个杂散像素**。
  - 结论（现在是证据而不是猜测）：
    ①**CPU↔GPU 的内存通路是好的**（填充 512/512 通过、fence 页 CP 写的值 CPU 能读到），
    所以 import/coherency 不是首要嫌疑；
    ②compute dispatch 一个正确字都没写、三角形把 4096 个像素写成了杂散值 → **问题在
    shader 执行/描述符/管线状态（uniform、shared const、descriptor set、binning/BV 初始化）**，
    也就是 Turnip 的 IB 和 a730 期望的状态之间还有差异；
    ③`Fo` 说明 `NativeBridgeMetalFrontend` 没打开，这之前还有一步门的开关。
  - 下一步（kernel 侧能做）：把 Turnip 的 compute IB 里 `CP_SET_*`/`SP_*`/`TPL1` 写入序列与
    上游 a7xx 的 compute 路径对比（尤其 shared constant、descriptor base、binning 前的
    `CP_SET_MARKER 0x…`/`GRAS_BIN_CONTROL`）；user 侧要查 frontend 打开失败的空错误串。
- **2026-10-30 GPU 第 299 步：黑屏这一页翻过去了（机主确认 metal51 已不黑屏）。回到 GPU：kernel 侧还能再动的一处是 fence 门铃，metal53 = metal52 的显示修复 + wait 超时时补一次 WPTR 重写（并记录它是否救回来了）**
  - 事实：`1336.txt` 里 `NativeAdreno: stall` 行 **0 条**，而 `NativeAdrenoSubmit::wait()` 在超时时确实调用
    `noteStall()`（50 ms 重写 WPTR、200 ms 排总线）。也就是说 `noteStall` 的 50 ms 阈值从未被跨过，
    而 ring 的等待超时是真的发生了——那条"自动重敲门铃"的路在这条 path 上没生效。
  - metal53 的改法：`NativeAdrenoRing::wait()` 在第一次超时的瞬间 `kick()`（把同一个 `next_` 再写一次
    WPTR）并继续等一个周期；如果 fence 因此退休，打 `fence N retired after a repeated WPTR write`。
    依据：metal51 的心跳证明超时时 `cp rptr == wptr`（CP 已经把整圈读完）、`rbbm 0x1`、`fault 0`，
    即"命令流没丢、门铃丢了"这一类的形状。
  - 另两件不在 kernel 侧，需要用户态 bridge/插件的源码才能继续：
    ①`metal probe: done (init not called)` —— 系统打开了加速器却没调插件 init；
    ②报告行 `M s0 c999 r64 i4096 q141 k3/4 p13/8 Fo` 从 metal39 到 metal53 一字未变。
    kernel 侧能确认的只有：`open type 1 -> 0x0`（discovery）与 `open 0x4147 -> 0x0`（插件连接）都成功，
    失败的那两次 `0x41470001/0x41470002` 是**设计上**的进度标记（`new_user_client` 里 `(type>>16)==0x4147 && (type&0xffff)`
    分支），不是问题。
  - 产物（metal53）：内核 17,072,680 B `defc5ca3…`；`shim/out/payload-metal53-doorbell.bin`
    `2b45f0d2…`；`shim/out/shim-boot-metal53-doorbell.img` 32,051,200 B
    `830bdf56e2a419adc261271b8dc28f4a53ca53a0f4bc25e85803c7eca1d755cb`。
  - 上机看点：fence 超时是否变成 `retired after a repeated WPTR write`（说明门铃丢失占了多数）；
    若仍超时，`stall` 行是否出现、`ts/bv` 与 `rptr/wptr` 的组合是否变化。
- **2026-10-30 GPU 第 298 步：metal51 上机，心跳把"黑屏"和 GPU 彻底分开了。**①IRQ 位不是 fence 的原因（去掉后仍 `ts 6 bv 6`）；②黑屏时内核活着、CP 空转 2045、`rbbm 0x1`、`fault 0`、vsync 一路涨到 10404（≈57 Hz），**但 swap 数停在 131/132** —— 是显示/合成路径停了，不是 GPU 挂了；③心跳线程自己在 ~160 s 停掉（它读面板状态时用了阻塞 `IOLockLock`），而其它日志一直写到 244 s**
  - `1336.txt`（727,090 B，`0b83818a…`）：init 与 metal50 完全一致（QSB 4/4 0x71620、
    ubwc=1 rb 0x817 tpl1 0x7 sp 0x27、SECVID 不写、HWCG 53/53、protect 46/46）。
  - `framed fence emits CACHE_CLEAN without IRQ (sheng-gpu-fence-irq=0)` 生效了，但
    fence 7/8/15 的超时行与 metal50 一模一样（`ts 6 bv 6`、`ts 7 bv 7`、`ts 14 bv 14`）。
    **结论（推翻 metal50 的猜测）：IRQ 位与"慢一跳"无关**；metal50 那次
    `ts 15 / bv 16` 只是采样正好落在两条事件之间。fence 的真实成因仍未定，但已排除 IRQ 位。
  - 心跳（每 20 s，`beat N start` 在寄存器读之前）给出的时间线：
    | beat | cp rptr/wptr | swap | vsync |
    | --- | --- | --- | --- |
    | 1 (~20 s) | 1543 | 6 | 2407 |
    | 2 (~40 s) | 2045 | 107 | 3543 |
    | 3 (~60 s) | 2045 | 108 | 4687 |
    | 4 (~80 s) | 2045 | 131 | 5826 |
    | 5 (~100 s) | 2045 | 131 | 6970 |
    | 6 (~120 s) | 2045 | 132 | 8114 |
    | 7 (~140 s) | 2045 | 132 | 9260 |
    | 8 (~160 s) | 2045 | 132 | 10404 |
    → CP 从 ~40 s 起就空转（没人提交），`rbbm 0x1`、`fault 0x0`、`ib1rem 0`；
    **vsync 一直涨**（fb workloop 活着），**swap 停在 131/132** → 用户态在桌面起来后
    没多久就不再提交帧。**黑屏不是 GPU 挂，是显示/合成这条线停了。**
  - 心跳线程在 beat 8 之后没了（beat 9 的 `start` 行都没有），而 AMFI/exec 日志继续
    写到 244 s（`exec bluetoothd at 244.298 s`），所以不是内核整体死掉。最可能就是
    `liuqin_fb_state()` 里那个阻塞的 `IOLockLock(fLock)`——说明**有东西一直占着面板锁**，
    而这正好和 `vsyncFired()` 在持锁状态下调用 `client->notify()`（`sendAsyncResult64WithOptions`）
    对得上：`swap()` 用同一把锁，通知一旦阻塞，合成器就再也换不了帧。
  - metal52 改动（显示侧，与 GPU 无关）：
    ①探针改成 `IOLockTryLock`，永不阻塞，多报 `phase`（1=正在发 vsync 通知、2=正在
    present、0=空闲）和 `lockbusy`；
    ②`vsyncFired()` 的通知改成"锁内 retain 客户、锁外 notify"，去掉这个持锁阻塞点；
    ③`present()` 也带上 `phase=2`，这样面板锁被谁占住、卡在哪一段，下一份日志直接写出来。
  - 产物（metal52）：内核 17,072,680 B `b7ce688a…`；`shim/out/payload-metal52-fbprobe.bin`
    `81c3a5f3…`；`shim/out/shim-boot-metal52-fbprobe.img` 32,051,200 B
    `0f7ea060b1b7cd70c4fb31adcdf47cf190e1c5bf6102c4f6b90ce702bf79bc6a`。
  - 上机看点：beat 是否一直打到最后一次日志；`panel … swap … vsync … phase … lockbusy …`
    里 `lockbusy 1`/`phase 2` 就是卡点；swap 数是继续涨还是又停在同一处；fence 超时行
    是否还是 `ts == bv`（若相等但 `ts` 落后，说明 clean 事件本身迟到）。
- **2026-10-30 GPU 第 297 步：metal50 上机 = init 全过、Metal 探头**第一次真的把加速器打开**，但整机在桌面后约 120 s**无声死掉**（UART 停、无 panic、黑屏）。metal51 = 去掉 framed fence 的 IRQ 位 + 心跳线程 + UBWC/QSB 先关回去（隔离变量）**
  - `1322.txt`（637,365 B，`a0378570…`）的 init 三条全过：
    `GBIF QSB sides 4/4 written (read 0x71620/0x71620)`、
    `NC mode ubwc=1 rb 0x817 tpl1 0x7 sp 0x27`、
    `RBBM_SECVID_TRUST_CNTL left alone (secure-only; ...)`；后面
    `HWCG 53/53 written rbbm_clock_cntl=0x8aa8aa82`、
    `CP protect 46/46 written cntl 0x7 (read 0x7, want 0x7) status 0x0` 也都站住。
  - 桌面正常：`exec SpringBoard at 52.304 s`（比 metal48 的 173.445 s 早，
    LaunchServices 库已建好），46 次 `LIUQIN FB`、触摸正常。
  - **新进展**：`|backboardd| metal probe: open 0x4147 rc 0 conn 0x11927`、
    `metal probe: open type 1 rc 0`，`NativeAdreno: original accelerator open type 1
    proc backboardd (discovery)`、`NativeAdrenoMetal: backboard s34 e0 v4/4 …`。
    metal48 同一处是 `IOK SpringBoard: open AGXAccelerator … -> 0xe00002c7`（不支持），
    所以这是 Metal 设备面第一次真开起来（`metal probe: done (init not called)`，
    还没到 init）。
  - 死法：日志停在 173.5 s（bluetoothd exec 之后），没有 panic、没有 panic 屏，
    屏幕黑。`LIUQIN WDT: IOWatchdog registered (accepts check-ins, never fires)`，
    所以没有东西会报它。**死的时候我们这边没有 in-flight 提交**：最后一次提交是
    fence 33（早得多），最后一条 NativeAdreno 活动是 `metal client open … backboardd`。
  - **fence 的新证据（本轮最值钱的一条）**：fence 16 超时那一行是
    `fence words rptr 696 ts 15 bv 16`。同一次提交、同一个 buffer、相邻两条事件：
    BV 那条（`0x08000031`，不带 IRQ 位）**已经写进 16**，BR 那条
    （`0x88000031`，带 IRQ 位 `1<<31`）**还停在 15**。以前每次都是 ts/bv 一起落后
    一跳，IRQ 位和“慢一跳”分不开；这次两条事件在同一次提交里，IRQ 位是唯一区别。
  - 但要诚实：metal48 的日志其实也是在桌面后约 150 s 断的（末尾 `released stuck
    fingers`），而且**机主说黑屏以前就有几率发生**，所以“跑两分多钟就断”是既有问题，
    不是 metal50 引入的；UBWC/QSB 因此**不背这个锅，保持开启**（`=0` 仍可单独关掉，
    用来排查渲染结果不对）。
  - metal51 三处改动：
    ①`emitFramedFence` 的 clean 事件默认不带 IRQ 位（中断路径本来就用不了：CP mask
    写入被拒、ring 靠轮询），`sheng-gpu-fence-irq=1` 还原；
    ②新增心跳线程：每 20 s 打三行——`beat N start`（**在访问任何 GPU 寄存器之前**）、
    `beat N cp rptr … wptr … rb_cntl … rbbm … fault … ib1rem …`、
    `beat N panel present … powerOff … fade … swap … vsync …`（后者直接读
    `AppleCLCD` 的状态，见 `liuqin_fb_state()`）。下一份日志就能回答三个问题：
    最后一行是 `beat N start` → 卡死在那次寄存器读（GPU/GBIF 死了）；beat 一直有而
    屏幕黑 → 内核没事，看 panel 行的 `powerOff`/`vsync` 就知道是系统关屏、fb workloop
    停了，还是画面被送成黑；`sheng-gpu-beat=<秒>`（0 关）。
  - 产物（metal51）：内核 17,072,680 B `23875a43…`；`shim/out/payload-metal51-beat.bin`
    `0fc868e0…`；`shim/out/shim-boot-metal51-beat.img` 32,051,200 B
    `7aa5e4d1d7331d41601536ed482b9039fbc298256049c7b2a6095f3b03b26daa`。
  - 上机看点：`framed fence emits CACHE_CLEAN without IRQ (sheng-gpu-fence-irq=0)`、
    `heartbeat started (20 s)`；然后 fence 超时行里 `ts` 和 `bv` 是否相等、是否不再超时；
    beat 行是否一直打到黑屏之后、panel 行说什么；M 行是否还是 `c999`。
- **2026-10-30 GPU 第 296 步：metal49 上机 = 三条新配置里两条确认成功，第三条把机器打死。`RBBM_SECVID_TRUST_CNTL (0xf400)` 在非安全世界**写不得**（总线外部中止 → panic）；GPU 的 secure/non-secure 状态只能由 bootloader/zap 决定。metal50 = metal49 去掉这一条**
  - 成功、且数值与上游期望逐位一致（这条最重要：UBWC 的猜测是对的）：
    `GBIF QSB sides 4/4 written (read 0x71620/0x71620)`、
    `NC mode ubwc=1 rb 0x817 tpl1 0x7 sp 0x27`。
  - 随后立刻 `Kernel data abort at pc 0xfffffe0007ababe0`，没有走到 HWCG/protect/power-up，
    所以 fence 那条线这一轮**没有新证据**（这轮不是 hang，是 init 里就死了）。
  - 定位（纯主机侧，不用再上机）：内核未 strip，`nm` + `llvm-objdump` 反汇编 → pc 落在
    `NativeAdrenoGpuService::startCp+0x8a0`，该处指令是 `str wzr,[x11,#0x3d000]`；寄存器窗口
    按 dword 索引，`0xf400*4 = 0x3d000`，正是 `gpu_write32(&window,0x0f400,0)` 那条
    RBBM_SECVID_TRUST_CNTL 写。ESR `0x96000010`：EC=0x25（同 EL 数据中止）、DFSC=0x10
    （同步外部中止，非页表 walk）→ 总线拒绝了这次访问，不是我们的指针算错。
  - 推断（明确标注为推断）：这颗芯片把该寄存器设成 secure-only；上游注释
    "access to the RBBM_SECVID_TRUST_CNTL register will be blocked and a permissions
    violation will soon follow" 是字面意思。同一块里的 `0xf800..0xf803`（SECVID TSB）
    照常可写（metal48/49 都写过），所以是**这一个**寄存器，不是整块被关。
  - 因此退出 secure mode 只能靠 zap shader + CP_SET_SECURE_MODE；没有 zap 就不碰它。
    `sheng-gpu-secvid=1` 保留原路径（要复现 panic 用）。
  - 产物（metal50）：内核 17,072,680 B `4f8c3a24…`；`shim/out/payload-metal50-secvid.bin`
    25,476,192 B `c7919964…`；`shim/out/shim-boot-metal50-secvid.img` 32,051,200 B
    `0751ed249fb82845c9b7aad91b16c5f20e87fb15228f5c5c6bb40c8284e60ea5`（cmdline 586 B 不变）。
  - 上机看点：先确认 `RBBM_SECVID_TRUST_CNTL left alone (secure-only; ...)`，再确认
    UBWC/QSB 两行与上面一致，然后看 fence 7/8/15 是否还超时、M 行是否还是 `c999`、
    界面是否仍正常（UBWC 打开后若有花纹/错位，`sheng-gpu-ubwc=0` 可当场回退）。
  - 重新核对 metal48 的 `1259.txt`（同一份日志，主机侧把 fence 行全列了一遍），
    **fence 不是"全卡"，是"迟到一跳"**：客户端一共提交 20+ 次，只有 7、8、15 三笔超时，
    9–14、16–21 都正常退休；每次超时的那一刻 `ts` 正好等于上一个 seqno
    （fence 7 → ts 6、fence 8 → ts 7、fence 15 → ts 14），但 `cp rptr == wptr`
    （229/229、264/264、635/635）说明 CP 把整圈都吃完了。也就是说 N 的 timestamp
    是在 N+1 提交时才出现的，不是丢了；同时 `hwfault 0x0`、`protect 0x0`、
    `roq 0/0/0`、`cp ib1/ib2 rem 0`、CP fault `0x0` 全干净 → 没有 fault、
    没有保护违规、没有卡在 IB 里。原文里"CP 停在最后的 cache clean 上等它退休"这句话
    要按这个"迟到一跳"来读，不是永远不退休。
  - 另外注意 `IOK SpringBoard: open AGXAccelerator ... -> 0xe00002c7`（= kIOReturnUnsupported）
    反复出现，`NativeAdrenoMetal: M s0 c999 ...` 只有一行：Metal 设备面这条路还没真正
    打开，界面的"正常"是软件合成路径给的，不能拿它证明 GPU 合成通了。
- **2026-10-30 GPU 第 295 步：metal48 上机，HWCG/protect 都写进去了但**不是根因**；日志把卡点定到"fence 末尾那条 `CACHE_CLEAN|IRQ` 本身不退休"，而且只有客户端（Turnip）流会卡。metal49 补上 hw_init 里 a730 路径剩下的配置，其中 **UBWC 模式的 bit0 一直是 0**（RB/TPL1/SP），这是本轮最具体的差异**
  - `1259.txt`（668,575 B，`fde1fc72…`）：`HWCG 53/53 written rbbm_clock_cntl=0x8aa8aa82`
    （与期望一致）、`CP protect 46/46 written cntl 0x7 (read 0x7, want 0x7)
    status 0x0`，两处都写进且站住了。系统正常：`exec SpringBoard at 173.445 s`、
    46 次 `LIUQIN FB`、触摸正常、无 panic。**fence 仍超时（7/8/15），M 行仍 `c999`**。
  - 超时形态（fence 7）：`cp rptr 229 wptr 229`、`mrb 0x8000000000300004`
    （= fence_iova+4，即 framed fence 最后那条 `CACHE_CLEAN|IRQ|BIT(27)`）、
    `cp bv rptr 229` 且 `ib1/ib2 rem 0` → **CP 停在最后的 cache clean 上等它退休**，
    不是在等 IB、也不是 BV 没跑到。对照：内核自己的 fence 2（ME_INIT）、
    fence 3（self-test）都正常退休，所以 CACHE_CLEAN 这条路本身是通的，
    只有客户端流执行过之后才卡 → 卡的是"客户端流留在 cache/bus 里的东西"。
  - `CP_PROTECT_STATUS 0x0`：Turnip 没有碰任何被保护寄存器，metal48 的两张表
    可以留在里面（保护仍然该开）。
  - 对照上游 `hw_init()` 的 a730 路径，还缺：①`a6xx_set_ubwc_config()`
    的 **UBWC 使能位**（我们 RB=`0x816`/TPL1=`0x6`/SP=`0x26`，上游 `0x817`/`0x7`/`0x27`，
    差的就是 bit0 = "ubwc mode"；a730 的 hbb=16→hbb_lo=3、swizzle=6 已对，
    `level2_swizzling_dis=0`、`rgb565_predicator=1`、`amsbc=1`、`uavflagprd_inv=2`）；
    ②`GBIF_QSB_SIDE0..3 = 0x00071620`；③没有 zap shader 时手动
    `RBBM_SECVID_TRUST_CNTL (0xf400) = 0` 退出 secure mode。
  - metal49 改动：上面三条都补（`sheng-gpu-ubwc=0`、`sheng-gpu-gbifqsb=0` 可回退），
    QSB 偏移按 GBIF 块基址 `0x3c00` 推（GBIF_HALT=0x3c45、protect 表覆盖
    `0x3c00..0x3cc3`），**不是从寄存器头文件读来的**，所以带上读回日志；
    超时行新增 `cp state rb_nc/rb_ccu/uche_mode/qsb0/rbbm_status`，
    用来看 Turnip 的流有没有把 UBWC/CCU 配置改回去。
  - 产物：内核 17,072,680 B `0751e779…`；`shim/out/payload-metal49-ubwc.bin`
    25,476,192 B `45c83c5e…`；`shim/out/shim-boot-metal49-ubwc.img` 32,051,200 B
    `c8a88c146a218c0abf52f048a0bb72292a0d277b5b7919cd3b9f6a5e2a802d76`
    （cmdline 586 B 不变）。
  - 上机看点：`NC mode ubwc=1 rb 0x817 tpl1 0x7 sp 0x27`、
    `GBIF QSB sides 4/4 written (read 0x71620/0x71620)`、
    `RBBM_SECVID_TRUST_CNTL read 0x0`；然后 fence 7/8 是否还超时、M 行 `c999`
    是否变。若 `qsb0` 读回不是 `0x71620`，说明 0x3c00 这个偏移猜错，用
    `sheng-gpu-gbifqsb=0` 关掉再走。

- **2026-10-30 GPU 第 294 步：metal48 = a730 的 HWCG + CP protect 真正写进 CP 初始化。过程中纠正了两处**表数据错误**：a730_hwcg 是 **53 条**（`linux-ref/a730_hwcg.h` 的 107 条其实是 a730 53 条 + a740 54 条拼接，其中 9 条的**值和 a730 不同**，照抄会把 a740 的时钟门控值写到 a730 上）；protect 的编码不是"start|end<<16"，而是 **bits 0-17 = 起始寄存器，bits 18-30 = 跨度，bit 31 = NORDWR 形式**（`0x1f8c0` 需要 18 位，`0x8630+0x1cf` 溢出 15 位；该编码对 a730 的 46 条与 a740 表**逐条 bit 相同**，两表 protect 列表本身就完全一致，hwcg 则差 9 条）**
  - 生成器 `tools/gen-a730-hwinit.py` 从 `linux-ref/a730_tables.inc`（用户提供的上游
    `a6xx_catalog.c` 片段）+ `a730_hwcg.h` 生成
    `xnu/iokit/NativeAdreno/NativeAdrenoA730HwInit.h`（53 HWCG + 46 protect，
    含 `NATIVE_ADRENO_A730_PROTECT_CNTL 0x7`）；hwcg 偏移与值在生成时逐条
    assert 与上游一致，protect 保留原索引（45/46 是上游两个全零槽，不写；
    47 是最后一跨，靠 `LAST_SPAN_INF_RANGE`）。
  - `NativeAdrenoService::startCp` phase 0（写完 CP/RBBM 寄存器、`RB_CMP_DBG_ECO_CNTL`
    之后，power-up 列表之前）新增两段，等同 `a6xx_set_hwcg(gpu,true)` 与
    `a6xx_set_cp_protect()`：先 53 条 HWCG 再读回
    `RBBM_CLOCK_CNTL`（期望 `0x8aa8aa82`），然后 `CP_PROTECT_CNTL=0x7`、
    46 条 protect、读回 `0x084f` 与 `0x0824`。开关
    `sheng-gpu-hwcg=0` / `sheng-gpu-protect=0` / `sheng-gpu-protect-cntl=<值>`。
    注意 a7xx **没有** `CP_SET_PROTECTED_MODE`（上游只对 !a7xx 发），所以这张
    protect 表在 a730 上是唯一的权限来源。
  - `NativeAdrenoRing::attach` 按 `adreno_hw_init()` 显式把 memptr 的
    `fence`(+4) 与 `bv_fence`(+12) 写成 `completed_`（attach 时恒 0，与清零等价，
    只是把不变量写在 ring 里）；`logTimeout` 在 `CP_PROTECT_STATUS 0x0824`
    非零时打印读/写与地址，用来区分"流在等"还是"CP 被保护挡下"。
  - 产物：内核 `kernel.development.liuqin` 17,072,680 B `5595df8d…`
    （上一版 metal47 是 17,072,592 B `f0db3a12…`）；
    `shim/out/payload-metal48-hwcg.bin` 25,476,192 B `b9a65529…`；
    `shim/out/shim-boot-metal48-hwcg.img` 32,051,200 B
    `e6e1dca54d218a028f1b312bb0c02e5be3e5605f28b82b706ea29e1d843d3bfd`
    （cmdline 586 B 与 metal47 完全一致，仍带 `sheng-gpu-metal=1 sheng-gpu-hfi=1`）。
  - 上机看点（尚未上机，以下都还是假设）：`NativeAdreno: HWCG 53/53 written
    rbbm_clock_cntl=0x8aa8aa82`、`CP protect 46/46 written cntl 0x7 (read 0x7 …)`
    两行；`CP_PROTECT_CNTL` 的位定义（bit0 使能 / bit1 违规报错 / bit2 末跨无限）
    是按上游宏名推的，读回值就是验证点，若读回不是 0x7 说明位猜错、protect 等于没开。
    另外若出现 `cp protect status …`，说明 Turnip/我们自己的 PM4 撞了保护范围，
    用 `sheng-gpu-protect=0` 回退。fence 超时若消失，`M` 行的 `c999` 才有机会变。

- **2026-10-30 GPU 第 293 步：拿到 SM8450/a730 的上游参考（用户提供 `yzddmr6/linux-sm8450-liuqin` 的 `a6xx_catalog.c`、`a6xx_gpu.c`）。确认 a730 有**专属**的 HWCG 表与 protect 表（`a730_hwcg[]`，`a730_protect_regs[]` 48 项，`DECLARE_ADRENO_PROTECT(a730_protect,48)`，SQE 固件 `a730_sqe.fw`），a740 的表不能代替。两张表已存 `linux-ref/a730_tables.inc`（113 条）。对照 `a7xx_submit()`：我们的 ring framing 与上游同构；上游 fence 用的是 `CACHE_CLEAN | BIT(27) | IRQ`，我们的 `RingFenceEnable = 1<<27` 已经等价，**fence 这块不必再改**。真正的缺口是初始化：HWCG 与 CP protect 两张表我们一项都没写**
  - 参考仓库：`https://github.com/yzddmr6/linux-sm8450-liuqin`
    （网络不稳，整仓 clone 失败两次；用户直接提供两个文件后成功解析）。
    另有 `https://github.com/yzddmr6/xiaomipad-6pro-mainline`（可达，未使用）。
  - metal47 结果：屏幕恢复正常、正常进桌面（用户确认）。
    `prog 0x20008680` 读出 `00000005 03820000 00000004 02820000`
    → 程序有效非零；M 行仍 `c999`，fence 仍超时。
  - 下一步（metal48）：把这 113 条表项按 a7xx 时机写进
    `NativeAdrenoService` 的初始化（GMU 起来后、ring attach 前），
    加 `sheng-gpu-hwcg=0` 回退开关；超时行补 `CP_PROTECT_STATUS 0x0824`。
    若 HWCG/protect 缺失正是"GPU 永不 idle"的原因，metal48 会让
    fence 超时消失、`M` 行的 c 值变化。

- **2026-10-30 GPU 第 292 步：metal47 的 shader 探测成功——`prog 0x20008680` 先是全零、随后读出 `00000005 03820000 00000004 02820000`，说明**程序确实在 BO 里且非零**，"编译产物没写进 BO"的假设排除。`0x20034000` 读出的是 PM4（`70c70002 34000000 …`），它其实就是客户端 IB 里 `CP_INDIRECT_BUFFER` 指向的子 IB，不是描述符区。M 行仍 `c999`，fence 仍超时（23/27/28）**
  - metal47 日志 `1148.txt`（680,752 B，`25f88fae…`）：
    - `prog 0x20008680`：第一次全零（程序尚未写入），之后
      `00000005 03820000 00000004 02820000` —— a7xx 指令形态，非零；
    - `prog 0x20034000`：`70c70002 34000000 00000014 …`，是 PM4 流
      （对应 IB 里的 `CP_INDIRECT_BUFFER 0x…34000/5`）；
    - 报告行 `M s0 c999 r64 i4096 q141 k3/4 p13/8 Fo`；
    - 超时 fence 23/27/28：`rptr 1021 wptr 1041`（差 20 = fence 长）、
      也有 `rptr == wptr` 的，`mrb = fence+4`、BV 空闲。
  - 结论：dispatch 的程序有效 → 卡点不在"程序不存在"。剩下两个方向：
    ① `SP_SHARED_CONSTANT_GFX = {3, 0x1000, 0, 0}` 等状态/常量错；
    ② **GPU 与 CPU 看到的不是同一份内存**（CPU 读到的程序有效，
       GPU 却可能读到零）。后者的反证是 fence 页：硬件写出的 rptr
      shadow 与 CPU 读的一致，说明至少那块映射是对的；需要专门验证
      程序 BO 的 SMMU 映射与 CPU 别名是否指向同一物理页。
  - 参考项目 `https://github.com/youzjuzi/dipper-xnu`（2026-10-30 已克隆到
    `/tmp/dipper-xnu`，网络恢复后 `git clone --depth 1` 成功，4.6 MB）：
    这是小米 8（SDM845 / Adreno 630）的 XNU 移植，**桌面已用 A630 原生渲染**
    （`docs/STATUS.md`：显示与输入、Wi-Fi、亮度、锁屏唤醒均实机验证）。
    其 GPU 部分是 `mi8_adreno.c` + `mi8_a630_hw_init.inc`：一张完整的
    初始化表——主表 180 项写、HWCG 表 105 项（与 Linux v7.2.10 的
    `a630_hwcg` 完全一致）、CP protect 表 32 项（与 `a630_protect_regs`
    完全一致），来源是 Linux `a6xx_gpu.c` / `a6xx_catalog.c`（GPL-2.0，
    `docs/GPU_LICENSE.md` 有许可讨论）。
    可借鉴点（下一步）：① 我们的 a730 初始化是否缺 HWCG / CP_PROTECT /
    CP chicken 位（他们整套表都抄了）；② 读 `CP_PROTECT_STATUS 0x0824`
    看 CP 是否在拦截什么；③ 对比他们 ring 的提交/fence 组织。
    注意他们也仍写着 "GPU SMMU ... 仍需处理"，所以不是银弹。

- **2026-10-29 GPU 第 291 步：metal44 的 `CP_MEM_WRITE` fence 把显示弄黑了——没有 CACHE_CLEAN，GPU 合成的帧留在 UCHE 里，显示控制器读到的是没被写过的 buffer。系统本身是好的（SpringBoard 116 s 起来、AKS 调用持续、`M s0 c999 …` 照出），只是屏幕黑。metal47 把 fence 改回"CACHE_CLEAN + 时间戳"（metal43 的语义，20 word），同时修掉 metal46 `prog` 探测的地址算错**
  - metal46 日志 `2322.txt`（654,201 B，`c15959ea…`）：
    - `prog 0x20038680` / `prog 0x20064000`：地址算错了。
      `0x20008680` 在 CB 基址 `0x20030000` **之下**，metal46 却加了
      `0x8680`，读到的是 CB BO 内部（`peek 0x0`、全零）或干脆没映射
      （`peek 0xe00002c2`）。所以这轮仍然没拿到 shader 内容。
    - 超时 fence 1/10/15/19（4 次），签名与 metal44/45 相同：
      `rptr = wptr - fence_len`、`ib rem 0`、`mrb = fence+4`、BV 空闲。
    - 报告行 `M s0 c999 r64 i4096 q141 k3/4 p13/8 Fo` 不变。
  - 黑屏原因（可验证的推断）：metal44 把 fence 末尾的 `CACHE_CLEAN`
    换成了 `CP_MEM_WRITE`，于是**每次提交结束时不再把 GPU 写出的像素
    从 UCHE 清到 DRAM**；合成器/显示控制器读到的帧缓冲是旧的或空的 →
    黑屏。串口日志里系统一切正常（SpringBoard 在跑、有 AKS 调用），
    符合"只有显示内容没发布"。
  - metal47 改动：
    1. `emitFramedFence` 回到 `CACHE_CLEAN` 时间戳
       （`EVENT_WRITE7 0x31|ENABLE|IRQ → fence_iova+4`），另有 BV 轨迹写
       `fence_iova+12`；**不等 BV**（metal41 已证明 BR 等 BV 必然超时）。
       `RingFramedFenceWords` 20。
    2. `prog` 探测地址修正为 `(base & ~0xffff) - 0x27980`
       （= `0x…8680`，程序区）和 `(base & ~0xffff) + 0x4000`
       （= `0x…34000`，`SP_CS_BINDLESS_BASE` 指向区）。
  - metal47 产物：
    - 内核 `f0db3a1252f69e372685060f43e71b45c6ba5ec890da4a33edfa59ea89ff14f9`
      17,072,592 B；
    - payload `shim/out/payload-metal47-clean.bin`
      `f2c7b4e42b91a1ba5d4da68edb9304904fee6877fedeb42bf924ab35160db641`
      25,476,096 B；
    - boot `shim/out/shim-boot-metal47-clean.img`
      `e02844c1948889c6565ddb1e1fbb5ef304f59532d2ecfcbb3382e974eb29dd2c`
      32,051,200 B（cmdline 仍 586 B）。
  - 上机：只 boot metal47。重点：
    - 屏幕是否恢复（这是本轮判据）；
    - `prog 0x…8680` 的 32 个字是否合法 a7xx shader 指令；
    - fence 超时次数是否回到 metal43 的水平（4 次左右）。

- **2026-10-29 GPU 第 290 步：metal45 的诊断没打出来——`blob` 扫描要求 `(lo,hi)` 里 `hi==0`，但子 IB 里是 `(寄存器, 值)` 成对出现，`0x20008680` 后面永远跟着 `0x0000a9b5`，于是每个候选都被跳过。唯一收获是确认 metal44 的签名稳定：`rptr = wptr - 9`、`ib rem 0`、`mrb = fence+4`、`M s0 c999 … Fo` 全部复现。metal46 修掉扫描（逐字判断、掩掉指针低 2 位 tag、跳过 PM4 头），并**直接按 CB 基址推两个固定地址来读**：`(base & ~0xffff)+0x8680`（计算上下文寄存器里的程序地址）与 `+0x34000`（`SP_CS_BINDLESS_BASE`），各 32 dword**
  - metal45 日志 `2316.txt`（710,454 B，`1e58fcfb…`）：
    - 没有任何 `blob`/`blb` 行（原因见上，是扫描条件写错，不是没有数据）；
    - 超时 fence 1/10/15/19，形态与 metal44 完全一致：
      `cp rptr 274 wptr 283`、`494/503`、`642/651`（差 9），
      `cp ib1 … rem 0`，`mrb 0x8000000000300004`，
      `cp bv rptr == wptr`、BV 空闲；
    - 报告行仍是 `M s0 c999 r64 i4096 q141 k3/4 p13/8 Fo`，无 fault。
  - metal46 改动（只读诊断）：
    1. `blob` 扫描改为逐字判断：`sub[j] & ~3` 落在
       `0x20000000..0x7fffffff`、且不是 type4/type7 头就当作指针，
       用 `vm_->peekWords` 读 32 dword；每个子 IB 最多 2 个候选。
    2. 新增固定地址探测 `prog`：`(buffers[0].iova & ~0xffff) + 0x8680`
       和 `+ 0x34000`（分别对应 compute 上下文的程序地址和
       `SP_CS_BINDLESS_BASE` 指向的区域），各读 32 dword，限 3 次。
       不再依赖"能从子 IB 里认出地址"。
  - metal46 产物：
    - 内核 `e3b76556d96b27c921130ea89d3d57948aa18aa4a74a104990077b86118a0524`
      17,072,592 B；
    - payload `shim/out/payload-metal46-prog.bin`
      `7f357195f84050fcba66b42d7f1d2671eedb65ab32b026a1b05b3eb64e162d99`
      25,476,096 B；
    - boot `shim/out/shim-boot-metal46-prog.img`
      `f118c847e272936ede48781ed79bda597b6dd4cf38988c87590af98e6e8c6479`
      32,051,200 B（cmdline 仍 586 B）。
  - 上机：只 boot metal46。重点：
    - `prog 0x…8680` 那 32 个字是不是合法 a7xx shader 指令
      （有结构、不是一片 `00000000`，也不是 PM4 头）；
    - `prog 0x…34000`（bindless 描述符区）内容是否合理；
    - `blob`/`blb` 是否出现，以及指向什么。
    若两处都全零 ⇒ 客户端编译产物从未写进 BO，问题在映射/发布路径，
    与 CP/环无关；若指令正常 ⇒ dispatch 卡在状态/常量，下一步看
    `SP_SHARED_CONSTANT_GFX = {3, 0x1000, 0, 0}` 与 `SP_CS_*` 其余字段。

- **2026-10-29 GPU 第 289 步：metal44 把 fence 换成 `CP_MEM_WRITE` 后，超时位置反而**前移**了——`cp rptr` 恰好是 `wptr - 9`（9 = 新 fence 的长度），也就是 CP 根本没取我们的 fence，它停在客户端 IB 的**尾部事件**里。`cp ib1 … rem 0` 的 rem 只是"取完"不是"执行完"，所以真实停点是 Turnip 命令流最后的 `CCU_CLEAN_COLOR (0x21) / CCU_CLEAN_DEPTH (0x20) / CACHE_CLEAN (0x31)`：GPU 永远不报告 idle，CP 就永远回不到环。fence 怎么改都没用——问题在客户端流内部。metal45 转而 dump 子 IB 引用到的内存（计算上下文里的程序地址等），看 dispatch 的程序/数据到底有没有落到 BO 里**
  - metal44 日志 `2307.txt`（679,572 B，`ce9b9589…`）：
    - 超时 fence 10/15/23（加一个初始化空等）：`cp rptr 274 wptr 283`、
      `494/503`、`812/821`——差值都是 9；`cp ib1 <客户端 IB> rem 0`、
      `ib2 <子 IB> rem 0`；`mrb 0x8000000000300004`（fence +4）；
      `cp bv rptr == wptr`、BV 空闲。
    - `fence words rptr … ts 9/14/22 bv 0`（`bv` 列现在恒为 0，因为
      metal44 不再写 BV 时间戳）。
    - 报告行不变：`M s0 c999 r64 i4096 q141 k3/4 p13/8 Fo`；
      `T s0 r0 v512 c0 g0/4096`；无 fault、无 VM poison。
  - 结论（这一轮已排除的东西）：
    1. 不是 fence 丢事件：`CP_MEM_WRITE` 与 `CACHE_CLEAN` 两种 fence
       都是"CP 根本没走到"；
    2. 不是 BV 等待：等待删掉后位置只是更靠前；
    3. 唯一自洽的解释是**客户端 IB 自己的收尾事件**（CCU clean /
       CACHE_CLEAN）在等 GPU idle，而 GPU 永远不 idle。也就是
       dispatch（或同一 IB 里的绘制）根本没有完成。
  - metal45 改动（只读诊断）：`NativeAdrenoSubmit::submit` 在 dump 子 IB 时，
    把子 IB 里"像 GPU 地址"的字（`0x20000000..0x7fffffff`、4 字节对齐、
    非 PM4 头）当作指针，用 `vm_->peekWords` 读 32 dword 打出来：
    `NativeAdreno: blob 0x… peek 0x… …` + 4 行 `blb`。
    目的：看 `0x20008680`（计算上下文里的程序地址）等处到底是真正的
    a7xx shader 指令，还是全零/垃圾（后者说明 metal2vulkan/Turnip 写的
    程序根本没落到 BO 里，GPU 读到零指令就会挂着不返回）。
  - metal45 产物：
    - 内核 `b611d9ecc17fe31f69db95f02b0274e122f4a72c83fad12ce3a83e0c17b963cf`
      17,072,592 B；
    - payload `shim/out/payload-metal45-blob.bin`
      `03ec0e84fdf6dcdc2e09113ace8005ea1f400715b2aae6c1d37b507907362b51`
      25,476,096 B；
    - boot `shim/out/shim-boot-metal45-blob.img`
      `74fea35ac5739cbc449d89f76e4bd54e94444ec15284382ccf585e258cc0aab1`
      32,051,200 B（cmdline 仍 586 B）。
  - 上机：只 boot metal45。重点：
    - `blob`/`blb` 行里 `0x20008680` 处是不是合法 shader 指令
      （a7xx 指令是有明显结构的 dword，不会是 `00000000` 一片）；
    - 若全零 → 问题在客户端把程序写进 BO 的路径（映射/发布），
      不在 CP/环；
    - 若是正常指令 → dispatch 卡住另有原因，下一步看
      `SP_SHARED_CONSTANT_GFX` 写的那 4 个字（`{3, 0x1000, 0, 0}`）
      与 `SP_CS_*` 的其余状态。

- **2026-10-29 GPU 第 288 步：metal43 去掉 BV 等待后，超时的形态变了但没消失——`cp rptr == wptr`（整环都被取走）而 `fence_iova+4` 仍停在上一笔。说明 CP 卡在 fence 末尾那个 `CACHE_CLEAN` 事件上：它只有在 GPU 报告 idle 时才完成。也就是说"永远不 idle"才是根，BV 等待只是它的第一个症状。metal44 把 fence 换成 `CP_MEM_WRITE`（不需要排空，CP 走到就写），先把环放通，用下一笔 prologue 的 `CACHE_FLUSH7/CACHE_INVALIDATE7` 来发布 GPU 写出的数据**
  - metal43 日志 `2258.txt`（685,051 B，`65ec04cd…`）：
    - 超时只剩 4 次（fence 1 是环形初始化时的空等，其余 fence 10/16/17），
      但形态变了：fence 16 `cp rptr 774 wptr 774`、fence 17
      `cp rptr 815 wptr 815`——**CP 把整条环都取完了，`fence +4` 还是上一笔的值**。
    - 报告行不变：`M s0 c999 r64 i4096 q141 k3/4 p13/8 Fo`；
      `T s0 r0 v512 c0 g0/4096`；无 fault、无 VM poison。
  - 结论：最后那个 `CACHE_CLEAN` 事件要等 GPU idle 才完成，而这颗 CP 在
    当前状态下永远不报告 idle（两种可能：客户端 IB 的 GPU 工作真的没做完，
    或者 `CP_BV_BR_COUNT_OPS` + 处处 `CONCURRENT_BIN_DISABLE` 让 BV/BR
    计数永远不平衡）。CP 卡在那里 → 后续提交排队 → 每笔都超时。
  - metal44 改动（一次只动 fence 语义）：
    1. `emitFramedFence` 改为 `CP_MEM_WRITE (0x3d)` 直接写
       `fence_iova+4 = seqno`，随后 `CP_THREAD_CONTROL both` +
       `CP_SET_MARKER 0x100`；**不再有 CACHE_CLEAN、不再有 BV 时间戳、
       不再有 IRQ**。`RingFramedFenceWords` 26 → 9。
    2. 理由：`CP_MEM_WRITE` 不依赖排空，fence 按 CP 的节奏退休，CP 可以
       继续吃后面的提交；GPU 写出的数据由**下一笔** prologue 的
       `CACHE_FLUSH7/CACHE_INVALIDATE7` 发布（那条路径本来就在）。
    3. 诊断保留：超时里的 BV 状态、子 IB dump、fence words。
  - metal44 产物：
    - 内核 `db7a2f40557fab8be00486492bc125568efadc318e4aab5cd775955779633948`
      17,072,592 B；
    - payload `shim/out/payload-metal44-memfence.bin`
      `17b17115f8cb6cd616582e39f74de7df46b5bca3790906005e515e83e927a475`
      25,476,096 B；
    - boot `shim/out/shim-boot-metal44-memfence.img`
      `83aa788891ba7bf11e101fbc27ee5eb7b8515e8d118c9c0a01eebbbc9d559114`
      32,051,200 B（cmdline 仍 586 B）。
  - 上机：只 boot metal44。重点：
    - fence 超时是否**全部消失**（这是判据 A）；
    - 若消失但 `c999` 不变 → 环/排空不是数据不可见的原因，真正没完成的是
      GPU 侧的 dispatch，下一步转向 shader/state（子 IB 里的
      `SP_CS_*`、`0x20008680` 程序地址、`SP_SHARED_CONSTANT_GFX` 写入）；
    - 若 `c999` 变成正常值 → 之前就是被"永远不 idle"卡住；
    - 若出现 fault/VM poison → `CP_MEM_WRITE` fence 退休太早（BO 被提前
      unmap），需要把 cache-clean 挪到**下一笔**的 prologue 里。

- **2026-10-29 GPU 第 287 步：metal42 证明"给切换加 SYNC"救不了 BV 等待——SYNC 前后完全一样（BV 仍差一笔、fence 仍超时）。结合 metal41：BR 卡在 `fence_iova+12` 的 `CP_WAIT_TIMESTAMP` 时，CP 根本不会切到 BV 去写那个时间戳，要等我们的 stall 处理（50 ms 重写 WPTR、200 ms 抽干总线）把它踢开才继续，于是每笔 fence 都晚一个提交才写出。**这是设计层面的问题，不是时序细节**。metal43 因此去掉这个等待：fence 只由 BR 的主时间戳 + IRQ 决定（fence 31 → 26 word），BV 时间戳仍然照写，只作轨迹**
  - metal42 日志 `2248-2.txt`（580,351 B，`200f04d0…`）：
    - 报告行仍是 `M s0 c999 r64 i4096 q141 k3/4 p13/8 Fo`。
    - 超时 fence 16/17/18，每个都是
      `cp ib1 … rem 0` / `ib2 … rem 0` / `mrb …+12`，
      `fence words ts 15 bv 15`（等 16）、`ts 16 bv 16`（等 17）、
      `ts 17 bv 17`（等 18）——**BV 依旧是"正好差一笔"**，
      SYNC_THREADS 没有改变任何行为。
    - BV 侧：timeout 16/17 空闲在 wptr（`ib1/ib2` 为 0、`roq` 全 0），
      timeout 18 时 `rptr 936`（wptr 957）、`roq 0xe0005`——BV 正在干活，
      所以不是"BV 死了"，是"BR 等它时它还没轮到"。
    - 5 dword 的子 IB 这次读到了：
      `48ab4004 00000003 00001000 00000000` = type4 写
      `SP_SHARED_CONSTANT_GFX[0..3]`（0xab40）= `{3, 0x1000, 0, 0}`。
      44 dword 的子 IB 仍是两条 `CP_CONTEXT_REG_BUNCH`
      （计算上下文寄存器，含程序地址 `0x20008680`）。
  - 结论（可验证的推断）：这颗 CP 在 BR 阻塞于内存等待时不会切到 BV，
    只有外部 kick 能把它推走；因此"BR 等 BV 时间戳"的 fence 在这颗
    固件上必然晚一个提交完成，客户端看到的就是
    `kIOReturnTimeout (0xe00002d6)`——metal2vulkan 会据此把 device 标成
    lost，bridge 再读回全零（c999）也就顺理成章。
  - metal43 改动：
    1. `emitFramedFence` 删掉 `CP_WAIT_TIMESTAMP`（不再等 BV）；
       `RingFramedFenceWords` 31 → 26；thread control 回到不加 SYNC。
       BV 时间戳照写（`fence_iova+12`），只用于超时诊断。
    2. 保留 metal41 的 BV 状态读数与子 IB dump。
  - metal43 产物：
    - 内核 `ec31aa18adfb0da9f721222c99d88c1a8287828e4ecbec5100b9aaf133ca9770`
      17,072,592 B；
    - payload `shim/out/payload-metal43-nowait.bin`
      `8990e7e1678992e2ab70f0fa5c250b19934e89c2fc2b4da990250e89d43842bd`
      25,476,096 B；
    - boot `shim/out/shim-boot-metal43-nowait.img`
      `13a27f891424c1d72b75c190a32ac1a2c00a4050efaaef3cd43253eaafdcc8c2`
      32,051,200 B（cmdline 仍 586 B）。
  - 上机：只 boot metal43。重点：
    - fence 超时是否**全部消失**（这是本轮唯一目标）；
    - 若消失，`M` 行的 `c`/`r`/`i`、`T` 行的 `g0/4096` 是否变化，
      以及 bridge 是否还报 `Fo`/device lost；
    - 代价：fence 现在只保证 BR 完成，BV 可能仍在读刚退休的 buffer。
      若出现 `fault`/VM poison/`retired` 异常，下一步在**下一笔**的
      prologue 里加"等上一笔 BV 时间戳"（此时 BR 尚未阻塞在该笔的
      等待上，kick 语义不同），而不是在这一笔里等。

- **2026-10-29 GPU 第 286 步：metal41 把 BV 侧的真相读出来了——**BV 时间戳永远正好落后一笔**（fence 15→bv 14、16→bv 15、17→bv 16），而 BV 线程本身空闲在 wptr（`cp bv rptr` == wptr、`ib1/ib2` 都为 0、`roq` 全 0、无 fault）。这正是"线程切换的 BV 侧要到下一个同步型 `CP_THREAD_CONTROL` 才生效"的样子：BV 的那一段要等下一笔提交 prologue 的 `SYNC|BR` 才执行，于是每一笔的 fence 都先超时、随后才补上。metal42 把 framed fence 里所有 `CP_THREAD_CONTROL` 都加上 `SYNC_THREADS`，让 BR 在切换点等 BV 到位、BV 当场写时间戳**
  - metal41 日志 `2240.txt`（737,813 B，`d7f4805b…`）：
    - 报告行回来了：`M s0 c999 r64 i4096 q141 k3/4 p13/8 Fo`（与 metal39 相同）。
    - 超时 fence 15（`0x20030000/1036`）、16（`0x60030000/932`）、
      17（`0x60030000/1008`）——连续三笔。
    - 每个超时的 BV 侧：`cp bv rptr` **等于 wptr**
      （767/767、839/839、885/885），`ib1 0x0 rem 0`、
      `ib2 0x0 rem 0`、`roq 0x0/0x0/0x0`、`hwfault 0`——**BV 空闲在写指针处，
      没有任何 IB 在飞**。
    - BR 侧：`cp ib1 … rem 0`、`ib2 … rem 0`，`mrb` 指向
      `fence_iova+12`（BV 时间戳；fence 15 是 `+4`，即 reset prologue 在等
      上一笔主时间戳）。
    - fence words：`ts 14 bv 14`、`ts 15 bv 15`、`ts 16 bv 16`——
      **BV 时间戳正好差一笔**，与"BV 段延迟到下一笔才执行"完全吻合；
      这也解释了为什么每笔 fence 先超时、随后又补上（下一笔的
      prologue 同步了 BV）。
    - 子 IB dump 这次成功：`0x20008780/44` 是两条
      `CP_CONTEXT_REG_BUNCH (0x5c)`，写计算上下文寄存器
      （`0xab00`=5、`0xab1f`=0x60、`0xa9cd`=0x302、`0xa9bb`=0x105、
      `0xa9b0`=0x80804100、`0xa9b4`=**0x20008680**（程序地址）、
      `0xa9b9`=0x80000000、`0xa9db`=0x444008fc、`0xa9c2`=0x00fcfcc0…），
      即 compute 上下文的寄存器组。`0x20038000/5` 这次没打出来，
      因为 5 < 8 落在了行循环外（metal42 已修）。
  - metal42 改动：
    1. `emitFramedFence` 的四个 `CP_THREAD_CONTROL` 全部 | `SYNC_THREADS`
       （`0x80000000`）：BV 切换到 BV 时 BR 等到位，BV 立即写自己的
       时间戳，随后的 `CP_WAIT_TIMESTAMP` 立刻满足——不再把 fence 拖到
       下一笔。这是本轮唯一的语义改动。
    2. `sib` 行在不足 8 dword 时也打印（补零），这样
       `0x20038000/5` 的内容能看到。
  - metal42 产物：
    - 内核 `728cbce3471b7580b1239253ce68d15a4193bd7f4d655526ef68b4b79f001d9c`
      17,072,592 B；
    - payload `shim/out/payload-metal42-bvsync.bin`
      `73ecddc789676c9c0a98ef727892cfc65b89b8810ef4d61cfd208cac4cdbf413`
      25,476,096 B；
    - boot `shim/out/shim-boot-metal42-bvsync.img`
      `9075e8667442847ee90d3a02e2395d6771bdbd9b07621ca492577ca96e9d4b43`
      32,051,200 B（cmdline 仍 586 B）。
  - 上机：只 boot metal42。重点：
    - fence 超时是否消失（若消失，`M` 行的 `c`/`r`/`i` 值是否变化）；
    - 若出现新的卡法（例如 fence 完全不被取走），说明这个固件上
      `SYNC_THREADS` 的语义与预期不同，需要换回非同步并改用
      "BV 只在下一笔执行"的替代做法（例如 BV 时间戳写两次、
      或 fence 只依赖 BR）；
    - `sib` 里 `0x20038000/5` 到底是什么。

- **2026-10-29 GPU 第 285 步：metal40 把"卡在哪"缩到了 BV（binning visibility）线程——超时行的 `cp ib1 … rem 0` / `ib2 … rem 0` 说明 BR 侧把提交的 IB 和它的 `CP_INDIRECT_BUFFER` 调用都跑完了，`mrb 0x800000000030000c` 说明 CP 的取数地址正指着 `fence_iova+12`（BV 时间戳），即 BR 停在 framed fence 的 `CP_WAIT_TIMESTAMP` 里；而 fence 里的 BV 时间戳始终没前进。也就是说 fence 没丢、环没写错，是这一笔的 BV 线程在写 BV 时间戳之前就停住了。metal41 在超时时直接读 A7XX 的 BV 线程状态（`CP_BV_RB_RPTR`/`CP_BV_IB1_BASE`+`REM_SIZE`/`CP_BV_IB2_*`/`CP_BV_ROQ_AVAIL_*`/`CP_BV_HW_FAULT`），并修掉 metal40 子 IB 扫描的 opcode 掩码错误**
  - metal40 日志 `2228.txt`（717,602 B，`6d7db5fb…`）：
    - 这一轮 bridge 连报告行都没出：`NativeAdrenoMetal: backboard s11 ee00002d6 v-1/-2 t0 a256/0`
      ——stage 11 拿到 `kIOReturnTimeout 0xe00002d6`，没有
      `M s0 c999 …` 行。metal40 只加了只读诊断（外加一个扫描
      掩码 bug），却比 metal39 更早失败，再次说明这是**间歇性**的。
    - 超时：fence 4（`0x20000000/48`）、fence 8
      （`0x60030000/1008`）、fence 9（`0x60030000/1360`）。
    - `cp ib1 0x0000000060030000 rem 0`、`ib2 0x0000000060038000 rem 0`
      （fence 8）、`ib2 0x0000000060034000 rem 0`（fence 9）——
      **BR 侧把提交的 IB 和它调用的子 IB 都执行完了**。
    - 每个超时行都是 `mrb 0x800000000030000c`：`CP_MRB_BASE`
      （CP 的取数地址）正好是 `fence_iova+12`，即 BR 正停在
      framed fence 的 `CP_WAIT_TIMESTAMP` 上等 BV。
    - `fence words rptr 312 ts 7 bv 7`（fence 8）、`rptr 360 ts 8 bv 8`
      （fence 9）：**BV 时间戳没有前进**，这一笔的 BV 时间戳从未写出。
    - 反例：fence 4 的 `ts 3 bv 4`——BV 甚至跑在主时间戳**前面**，
      说明 BV 线程本身是活的，坏的情况是它落在后面/停住。
    - fence 8 的 IB 与 metal39 fence 9 的 IB 同形：
      `CP_INDIRECT_BUFFER 0x60008780/44`、`0x60038000/5`、
      `CP_LOAD_STATE6 (0x34,11)`、`SP_CS_BINDLESS_BASE[0]=0x60034003`、
      `CP_SET_MARKER 0x08`、`SP_CS_NDRANGE_0..7`、
      `CP_EXEC_CS {0,16,1,1}`、事件 0x21/0x20/0x31。
    - `subib`/`sib` 一行都没有：扫描里把 opcode 取成
      `((h>>16)&0x7f)|(((h>>23)&1)<<7)`，把 odd-opcode 校验位又或了
      一次，`CP_INDIRECT_BUFFER (0x3f)` 被读成 `0xbf`，永远匹配不上。
      metal41 改为 `(h>>16)&0x7f`。
  - metal41 改动（全部只读，不改帧结构）：
    1. 超时时读 A7XX 的 BV 线程状态并打印
       `NativeAdreno: cp bv rptr … ib1 0x… rem … ib2 0x… rem … roq … hwfault … protect … ds …`：
       `CP_BV_RB_RPTR 0x0a66`、`CP_BV_IB1_BASE 0x0a6d/0x0a6e`、
       `CP_BV_IB1_REM_SIZE 0x0a70`、`CP_BV_IB2_BASE 0x0a71/0x0a72`、
       `CP_BV_IB2_REM_SIZE 0x0a74`、`CP_BV_ROQ_AVAIL_RB/IB1/IB2
       0x0a8f/0x0a90/0x0a91`、`CP_BV_HW_FAULT 0x0a64`、
       `CP_BV_PROTECT_STATUS 0x0a61`、`CP_BV_DRAW_STATE_ADDR 0x0a81`。
       这些直接回答：BV 停在环里（rptr 落后）、停在提交的 IB 里
       （`ib1=0x60030000` rem 非 0），还是停在子 IB 里
       （`ib2=0x60008780`/`0x60038000`）。
    2. `fence words` 扩到 12 个 word（`+8`/`+16` 等也打出来）。
    3. 修掉子 IB 扫描的 opcode 掩码（见上），这次应能打出
       `frontend stream` + `subib` + `sib`。
  - metal41 产物：
    - 内核 `b2db93b749186b3bab6b577d560eae13af48200968b1114f2c1e959ac2279e06`
      17,072,592 B；
    - payload `shim/out/payload-metal41-bvstate.bin`
      `d81338cdd93285f37efe37cd7e20087a333c63a7dde8ca3fcc32aef6fa45127d`
      25,476,096 B；
    - boot `shim/out/shim-boot-metal41-bvstate.img`
      `4327616845be761ad440bcc4ed0dd718350dc5c2a051572a9968a098fc230b1f`
      32,051,200 B（cmdline 仍 586 B，镜像里已核对
      `cp bv rptr` / `subib` 新串）。
  - 上机：只 boot metal41。重点：
    - `cp bv rptr <值>`：BV 的环读指针是否停在这一笔的
      `CP_INDIRECT_BUFFER` 之前（=BV 没进 IB）、还是已经接近 wptr；
    - `cp bv ib1/ib2 … rem`：BV 停在哪条流、还剩多少 dword；
    - `subib`/`sib`：`0x60008780/44` 和 `0x60038000/5` 的内容
      （5 dword 那条是 `CP_MEM_TO_MEM`、自指 IB，还是别的）；
    - `fence words` 里的 `+8`（BV rptr shadow）是否也是 0。

- **2026-10-29 GPU 第 284 步：metal39 的日志把卡住的位置定到了单个 dispatch 之后——多 buffer 提交全是"只设状态"的流（没有 `CP_EXEC_CS`/draw），真正发 dispatch 的是**单 buffer** 的 frontend 流（fence 9 `0x20030000/1008`），它里面有一个完整的 16 组×64 线程 compute dispatch，而环的读指针正好停在 framed fence 最后一个 `EVENT_WRITE7 0x31|IRQ` 之前。metal40 在超时时读 CP_IB1/IB2/SDS/MRB 的 base+rem size 和 fence 里的 BV 时间戳，并在提交单 buffer 流时把它 `CP_INDIRECT_BUFFER` 调用的子 IB 读出来（`0x20008780/44`、`0x20038000/5`），用来区分"流水线没排空"和"CP 还在等 BV 时间戳"**
  - metal39 日志 `liuqin-log-20261029-220508.txt`（772,440 B，`e099c31c…`）：
    - bridge 仍 `M s0 c999 r64 i4096 q141 k3/4 p13/8 Fo`；
      无 VM poison、无 method 失败、`fault 0x0`。
    - 前 3 笔多 buffer 提交（5/6/4 个 buffer，984/1056/880 B，iova
      `0x28050000`/`0x28038000`）**完全解码**：全是状态流——
      `CP_THREAD_CONTROL 0x08000003`、4 个 `EVENT_WRITE7`
      (0x19/0x18/0x28/0x33)、`CP_WAIT_FOR_IDLE`、约 150 dword 的 type4
      寄存器写（`SP_UPDATE_CNTL 0xab1f=0x01fffeff`、`RB_CCU_CNTL 0x8e07=0x68`、
      `UCHE_CACHE_WAYS 0xe17`…），然后 `CP_SET_DRAW_STATE {0x40000,0,0}`、
      `CP_BV_BR_COUNT_OPS {2,1}`、`CP_SET_AMBLE`（`0x20028000`/155 dword，
      `0x2002826c`/149，都是 BIN_PREAMBLE 类型）、`CP_REG_TEST`、
      4 个 `CP_MEM_WRITE` 把 `0x28040000/+8/+0x10/+0x18` 清零、
      `CP_REG_TO_MEM`、事件 0x21/0x20/0x19/0x18、`SP_UPDATE_CNTL 0x01fe00`、
      `CP_WAIT_FOR_IDLE`。**IB 内没有任何 `CP_EXEC_CS`/`CP_DRAW_*`/
      `CP_INDIRECT_BUFFER`**——这三笔只是建状态（metal38 的"前缀相同"
      由此解释）。每个 IB 末尾之后出现的
      `70bf8003 <自身 iova> 0 <自身 dword 数>`（如 `…0xf6`/`…0x108`）不是
      IB 的一部分，而是同一个 BO 里上一笔更长命令流的残留。
    - 卡住的都是**单 buffer** 的 frontend 流：fence 9
      `0x20030000/1008`（252 dword）、fence 10 `0x20030000/1360`、
      fence 19 `0x60030000/1036`。fence 9 的 IB 里**有真正的 dispatch**：
      `CP_SET_AMBLE`（bin preamble）→ `CP_INDIRECT_BUFFER`
      `0x20008780/44` → `CP_INDIRECT_BUFFER` `0x20038000/5` →
      `CP_LOAD_STATE6 (0x34, 11 dword)` →
      `SP_CS_BINDLESS_BASE[0]=0x20034003` → `SP_UPDATE_CNTL 0x01fe00` →
      `CP_SET_MARKER 0x08` → `SP_CS_NDRANGE_0..6`（kerneldim 3、
      local 64×1×1、global 1024×1×1）→ `SP_CS_NDRANGE_7=0xfc` →
      **`CP_EXEC_CS {0,16,1,1}`** → 事件 0x21/0x20/0x31。
      包形与上游 `tu_cmd_buffer.cc` 里 a7xx 的
      `CP_EXEC_CS {0,NGROUPS_X,Y,Z}` + `SP_CS_NDRANGE_*` 一致，
      即前端确实发了一个 16 组×64 = 1024 线程的 compute dispatch。
    - fence 9 的环 dump：46 word = prologue 11 +
      `CP_INDIRECT_BUFFER 0x20030000/0xfc` + framed fence 31；
      `rptr 382 / wptr 391`——读指针停在 fence 最后那个
      `EVENT_WRITE7 0x31|IRQ` 之前（BV 时间戳写与
      `CP_WAIT_TIMESTAMP` 都已被取走）；`fence 376/8`（rptr shadow 376，
      时间戳仍是 8）——fence 从未写出。
    - 因此有两种可能，metal39 无法区分：①dispatch（或 IB 尾部的
      cache clean 事件）让流水线始终不 idle；②CP 其实还在
      `CP_WAIT_TIMESTAMP` 里等 BV 线程写完 `fence_iova+12`，即 BV 侧
      根本没跑完这条流。
  - metal40 诊断（只加读数，不改语义）：
    1. `NativeAdrenoRing::logTimeout` 增加一行
       `NativeAdreno: cp ib1 0x… rem N ib2 0x… rem N sds 0x… rem N mrb 0x… rem N status1 0x…`
       （`CP_IB1_BASE 0x0928/0x0929`、`CP_IB1_REM_SIZE 0x092a`、
       `CP_IB2_BASE 0x092b/0x092c`、`CP_IB2_REM_SIZE 0x092d`、
       `CP_SDS_BASE 0x092e/0x092f`、`CP_SDS_REM_SIZE 0x0930`、
       `CP_MRB_BASE 0x0931/0x0932`、`CP_MRB_REM_SIZE 0x0933`、
       `CP_STATUS_1 0x0825`），以及
       `NativeAdreno: fence words rptr … ts … bv …`（`fence_[3]` 就是
       `bv_fence`，`fence_iova+12`）。BV 时间戳等于超时序号 →
       流跑完了、是排空/事件不完成；等于上一笔 → CP 还在 BV 侧。
    2. `NativeAdrenoSubmit::submit` 对 900..1500 B 的单 buffer 流
       （限 3 次）扫描前 260 dword 里的 `CP_INDIRECT_BUFFER (0x3f)`，
       用 `vm_->peekWords` 把每个目标的头 48 dword 读出来：
       `NativeAdreno: frontend stream 0x…/…`、
       `NativeAdreno: subib 0x…/N dwords peek 0x…`、`NativeAdreno: sib …`。
       这样能看到内核提交列表里根本看不到的子 IB
       （`0x20008780/44`、`0x20038000/5`）。
  - metal40 产物：
    - 内核 `aabb3e2341e6e91bd491530c8b0fcf6ee80a6201a94fe687302b108179367053`
      17,056,208 B；
    - payload `shim/out/payload-metal40-cpib.bin`
      `57928df3469bd072359a78c6f032bcb58e1d76b184957256017c4f6f2ae75a5f`
      25,459,712 B；
    - boot `shim/out/shim-boot-metal40-cpib.img`
      `83bdfd56758bb37ee00c270502666f921f0108d86aed72c112d09ac5c1c6d80c`
      32,034,816 B（cmdline 仍是 586 B 的同一串，已核对镜像里
      `NativeAdreno: subib …` 等新串存在）。
  - 上机：只 boot metal40。重点：
    - 超时行的 `cp ib1 … rem N`：如果 `ib1` 是 `0x20030000` 且 rem 很小，
      CP 已走完这条流、卡在排空；如果 `ib1` 是 `0x20008780`/`0x20038000`，
      卡在子 IB 里；
    - `fence words … bv <值>`：BV 时间戳是否等于超时序号；
    - `subib`/`sib` 行：5 dword 的子 IB 到底是什么（`CP_MEM_TO_MEM`？
      自指 IB？），44 dword 的子 IB 是否是真正的 shader/描述符流；
    - `M` 行 c 值是否仍 999。

- **2026-10-29 GPU 第 283 步：metal38 说明两件事——① bridge 的数据 buffer 根本不在内核提交的 buffer 列表里（提交只带 CS BO），所以内核 retire 时看不到它们，采样为空；② compute IB（984 B、5 个 buffer）的前 96 dword 与 frontend 的 1036 B IB 完全相同（THREAD_CONTROL 0x08000003、4 个 EVENT_WRITE7、WAIT_FOR_IDLE、~40 个 type4 寄存器写），dispatch 还在后面。另外 fence 9/10 在**没有**完成时维护的情况下也会偶发卡住（rptr 差 28/31），说明这些卡顿是间歇性的、不是 cache 维护引起的。metal39 把多 buffer dump 扩到 260 dword（前 3 次），并在 fence 超时时 dump 最新环内容（prologue/IB/fence），用来判断 context entry 是否发出、CP 停在哪**
  - metal38 日志 `2153.txt`（713,991 B，`2560bd5f…`）：
    - `retired` 采样 0 条：bridge 的 compute/render/import 提交只带 1 个
      CS buffer（`buffers 5/6/4` 是 IB 里的资源，不在内核提交数组中）；
    - `multi-buffer submit 5 buffers ib 0x28050000/984` 的前 96 dword
      与 metal33 的 fence 15 IB 同源：`70970001 08000003`（CB|BOTH）、
      4×`EVENT_WRITE7` 0x19/0x18/0x28/0x33、`CP_WAIT_FOR_IDLE`，
      然后是一串 type4 寄存器写（`SP_UPDATE_CNTL 0xab1f`、
      `RB_CCU_CNTL 0x8e07`、`UCHE_CACHE_WAYS 0xe17` 等）；
    - fence 9/10 仍偶发超时（`rptr 363/391`、`406/437`），但这次
      **没有**完成时维护，说明卡顿另有原因（或与 framed 提交的
      prologue/fence 有关，stall handler 的 kick 之后都能恢复）；
    - bridge 仍 `M s0 c999 r64 i4096 … Fo`。
  - metal39 诊断：
    1. 多 buffer 提交 dump 从 96 扩到 **260 dword**，只打前 3 次
       （bridge 的 compute/render/import）——可完整解码，找
       `CP_EXEC_CS (0x33)`、`CP_LOAD_STATE6 (0x32/0x34)`、
       `CP_SET_DRAW_STATE (0x43)` 和 group count；
    2. fence 超时时额外打印**最新一笔环内容**
       （`NativeAdreno: ring words fence … start … end … count …` +
       `NativeAdreno: ring …`），看我们生成的 prologue（是否带
       context entry）、IB 边界和 fence 是否按预期写入。
  - metal39 产物：
    - 内核 `941f4e2b607be7f87a8953e156989d211b518d9d65a56c5233d290ee44be883e`
      17,056,112 B；
    - payload `shim/out/payload-metal39-fullib.bin`
      `069dd353b0626f4941bb354cc69fb93c30ecf47b3598a0e480c523475b168d6c`
      25,459,616 B；
    - boot `shim/out/shim-boot-metal39-fullib.img`
      `74d0c78b7e9f5eae3d4d2b9d82104f3bf317dad7f4f9635951450dd2c44f7721`
      32,034,816 B。
  - 上机：只 boot metal39。重点：
    - `mib` 260 dword 里有没有 `CP_EXEC_CS`/dispatch 的 count；
      若根本没有 dispatch 包，说明 metal2vulkan/Turnip 把 compute 编成了
      别的形式（或提前返回）；
    - `ring` dump：最新提交的 prologue 是否 37 个 word（带 reset）、
      fence 是否 31 个 word；CP 停的位置与环内容的对应关系；
    - `M` 行 c 值是否仍 999。

- **2026-10-29 GPU 第 282 步：metal37 的 clean+invalidate 让 Turnip smoke 的 fence 9/10 恢复，但 fence 11（bridge device priming 的 1036 B）和 15/16 又卡——因为 Turnip **复用同一个 BO** 当下一条命令缓冲（932/1008/1360/1036 都在 iova 0x20030000），CPU 正在写它时被上一笔作业完成时的维护撞上，任何“完成时批量维护 job 里的 buffer”都避免不了这个竞争。且 Turnip 的 BO 映射本来就是 `kIOMapWriteCombineCache`（uncached）、我们的 `clientMemoryForType` 也给 `kIOMapInhibitCache`——**c999 不是 CPU cache 脏零**。metal38 撤掉完成时维护（回到 metal34 的稳定组合），只留只读诊断：多 buffer 提交 dump 前 96 dword + 非命令 buffer 的 retired 采样**
  - metal37 日志 `21383.txt`（657,354 B，`59e0c2b1…`）：
    - fence 9/10 不再超时（clean 保住了 CPU 脏行）；
    - 但 fence 11（1036 B，metal2vulkan 建 device 的 priming）：
      `rptr 500 wptr 509`（差 9）超时；fence 15（`rptr 742/767`）、
      fence 16（`rptr 839/839`）也超时；
    - bridge 仍 `M s0 c999 r64 i4096 q141 k3/4 p13/8 Fo`；
    - `retired` 采样只打到了命令流（`70970001 08000003 …`），
      没打到数据 buffer——说明采样的前 8 个都是 IB。
  - 关键认识：
    1. Turnip 每帧复用同一个 CS BO（`0x20030000`），所以“完成时维护
       入队时记录的 buffer”会撞上 CPU 正在写的下一条 CB；
    2. Turnip 的 BO 用户映射用 `kIOMapWriteCombineCache`，
       我们的 `clientMemoryForType` 用 `kIOMapInhibitCache`，
       CPU 读的就是 DRAM——**输出为零说明 GPU 没写，不是 cache 旧值**；
    3. fence 11/15 是同一个 1036 B 的 metal2vulkan 流，时好时坏
       （metal34 过、metal37 卡），像状态/竞争问题而非固定坏包。
  - metal38 改动：
    1. `retireJobAfterCompletion` 不再做任何 cache 维护（只读）；
    2. 采样只打**非命令流** buffer（首字 type 7，或 type 4 且 count≠0
       的跳过），最多 16 个，读前只对这一个 buffer 做 clean+invalidate；
    3. `NativeAdrenoSubmit::submit` 对**多 buffer 提交**（bridge 的
       compute/render/import 是 5/6/4 个 buffer）打印前 96 dword
       （`NativeAdreno: multi-buffer submit …` + `NativeAdreno: mib …`），
       最多 6 次——不用等超时就能解码 bridge 的命令流。
  - metal38 产物：
    - 内核 `c27ebe5325896f77495a7a7352b5a83c49b56dc7d7652c930c12ddd6d60b303c`
      17,056,048 B；
    - payload `shim/out/payload-metal38-mib.bin`
      `0a9d1f734c969208719a615098f7ff6c9e9af6eb4c21bf78a404b85c56f4c007`
      25,459,552 B；
    - boot `shim/out/shim-boot-metal38-mib.img`
      `dc260620e4926353d487efee5f3e44318d3c5d1db0eb2f0da933dc73fe48a2fa`
      32,034,816 B。
  - 上机：只 boot metal38。预期稳定性回到 metal34（smoke/bridge 跑、
    fence 15 仍可能卡），重点看两类新行：
    - `multi-buffer submit` + `mib …`：bridge 的 compute IB 前 96 dword
      ——找 `CP_EXEC_CS (0x33)`、`CP_SET_DRAW_STATE (0x43)`、
      `CP_LOAD_STATE6`、group count，判断 dispatch 是否真的编码进去；
    - `retired iova … words …`：数据 buffer 的实际内容。
      出现 3*i 位型 → GPU 写了，问题在读回/同步；
      全零 → dispatch 没执行，问题在 IB 编码或 shader state。

- **2026-10-29 GPU 第 281 步：metal36 证明“作业完成时 INVALIDATE 可写 mapping”本身有害——它把 CPU 在上一笔提交之后写入下一条命令缓冲的**脏行**丢掉（`dc ivac` 不写回），GPU 读到垃圾 IB → fence 9/10 卡住；bridge 仍跑到 M 行但仍是 `c999`。metal37 改成 clean+invalidate（`publishForDevice(...,writes=true,...)`），并抽样打印 retire 后 mapping 的前 4 个字，用来判断 GPU 到底写没写**
  - metal36 日志 `2121.txt`（855,192 B，`6b1b9b78…`）：
    - raw test 正常，Turnip smoke 的 932 B（fence 8）正常，但
      fence 9（1008 B）`rptr 382 wptr 391`（差 9）、fence 10（1360 B）
      `rptr 406 wptr 437`（差 31）都超时——与 metal34（无 invalidate）
      对比，唯一差异就是完成时的维护；
    - bridge 仍到 0x650f/0x6600，`M s0 c999 r64 i4096 q141 k3/4 p13/8 Fo`
      不变；fence 15 仍卡（rptr 736/767，差 31）。
  - 机理：smoke 是“提交 CB1 → CPU 录 CB2 到 BO → 提交 CB2”。CB1 的
    fence 完成时我们 `acquireDeviceWrites()`（INVALIDATE）把 CB2 那些
    **CPU 已写脏但尚未提交**的 cache 行直接丢弃 → GPU 读到旧/零 IB →
    CP 卡死。这和“输出 buffer 读回零”是同一枚硬币的两面：只 invalidate
    对 CPU 写过的 buffer 不安全。
  - metal37 修法：
    1. `retireJobAfterCompletion` 改用
       `publishForDevice(0,bytes,/*writes=*/true,false)`：
       CLEAN（保住 CPU 脏行）再 INVALIDATE（丢掉 GPU 写前的旧行），
       两侧数据都不丢；
    2. 每作业（`jobs==1`）最多 8 次打印
       `NativeAdreno: retired iova 0x… bytes … words %08x %08x %08x %08x`
       ——如果 GPU 真写了 compute 输出，应能看到 3*i 的浮点位型
       （如 0x00000000 0x40400000 0x40c00000 …）；全是 0 则说明
       dispatch 根本没写到内存。
  - metal37 产物：
    - 内核 `5800d1ff26fde7c2d1b8a2c7953430d3fbf8a3c3dd98e113721f03282221a803`
      17,055,960 B；
    - payload `shim/out/payload-metal37-cleaninv.bin`
      `7cb4a28a8f31a8ea7a89b59c842da63b1a5c9641e034956e099cd7194ce25905`
      25,459,472 B；
    - boot `shim/out/shim-boot-metal37-cleaninv.img`
      `2777079e8daa5ad631a6b99cef33c1477b9ca09db1e2f83117402ed69007e9d9`
      32,034,816 B。
  - 上机：只 boot metal37。判读：
    - Turnip smoke 的 fence 9/10 应恢复（metal34 行为）；
    - `retired iova …` 行是关键：
      - 出现非零且像 3*i 的数据 → GPU 写了，`c999` 是读回/可见性问题；
      - 全是 0 → dispatch 没执行（shader/描述符/状态），下一步查
        metal2vulkan 的 compute 编码或 Turnip 的 CS 生成；
    - `c` 值是否从 999 下降，以及 `Fo` 是否仍在。

- **2026-10-29 GPU 第 280 步：metal35 证明“framed prologue + 普通 plain fence”不行——Turnip smoke 的第二/第三个 IB（1008、1360 B）都卡住，且 `wptr-rptr` 正好都是 7 = plain fence 的长度（CP 不执行它）；bridge 因此根本没跑到（无 M 行）。metal36 恢复 framed fence（metal34 的组合），保留 metal35 新加的“作业完成时 invalidate 可写 mapping”**
  - metal35 日志 `liuqin-log-20261029-210042.txt`（818,012 B，`cde5d5b5…`）：
    - raw test 正常（backboardd/SpringBoard 都 `s34 e0 v4/4`），Turnip smoke
      的 932 B IB（fence 6）也正常；
    - 但 fence 9（1008 B）：
      `fence timeout 9 … cp rptr 266 wptr 273 … fence 264/8`（差 7），
      fence 10（1360 B）：`cp rptr 288 wptr 295`（差 7）；
    - 没有 `M` 行——bridge 阶段被 smoke 卡住，没跑；
    - 无 VM poison、无 observe 失败（SMR 修复保持）。
  - 判读：metal34 同样是 framed prologue + framed fence，fence 9/10 都过；
    换成 plain fence 后它们停在 fence 上。fence 6 是 reset prologue（首包带
    context entry）却过了，说明不是“prologue+fence 一律不行”，而是
    非 reset prologue 之后 plain fence 不被执行；framed fence 的显式
    THREAD_CONTROL 才能让 CP 走到时间戳。结论：**prologue 与 framed fence
    必须成对使用**。
  - metal36 = metal34 的 framed prologue + framed fence，加上 metal35 的
    cache invalidate（`NativeAdrenoVm::retireJobAfterCompletion` 里对
    作业可写、非 coherent 的 mapping 调 `acquireDeviceWrites`），
    用来单独验证 c999 是否是 CPU cache 里的零。
  - metal36 产物：
    - 内核 `d28a47b89beafdc3a3beb124254211695ea9aca9c409c33ffa509b145d008d13`
      17,055,888 B；
    - payload `shim/out/payload-metal36-framed2.bin`
      `dbae053c9702ff121a2ea8ff4e005c41d162afddcfceb3e06cce712017092ba9`
      25,459,392 B；
    - boot `shim/out/shim-boot-metal36-framed2.img`
      `e23830e580ce665f3d2591bd07e25f1333b27038644e2a784905fc369b079679`
      32,034,816 B。
  - 上机：只 boot metal36。判读：
    - Turnip smoke/bridge 应恢复 metal34 的进度（0x650f/0x6600、M 行）；
    - 重点看 M 行的 `c` 值：`c999` → `c0`/小值 = cache invalidate 修对了；
      仍 `c999` = GPU 没写输出或读回路径另有问题；
    - fence 15（frontend 的 1036 B IB）预计仍会卡——它**在 plain 模式
      （metal33）也会卡**（rptr=wptr），所以是 IB 内容本身的问题，
      与 framing 无关，下一步单独查它（已有 dump：CP_THREAD_CONTROL
      0x08000003、EVENT_WRITE7 0x19/0x18/0x28/0x33、WAIT_FOR_IDLE、
      SP_UPDATE_CNTL 0xab1f(A7XX)、CP_BV_BR_COUNT_OPS、EXEC_CS …）。

- **2026-10-29 GPU 第 279 步：metal34 的 framed 前导**跑通了**（raw test/Turnip smoke 都正常，无 trap、无 VM poison），但 fence 15 仍卡，且这次 rptr=736/wptr=767——正好差 31 个 word = framed fence 的长度：CP 停在了 BV 时间戳等待上（CB_DISABLE 的命令流没有 BV 线程去写它）。同时 bridge 结果仍是 `M s0 c999 r64 i4096 …`。metal35 两处改动：framed 保留 prologue/context entry 但改用普通 BR ring fence；任务完成时对作业里可写 mapping 做 CPU cache invalidate（Metal 新 buffer 是 CPU 零填充的，GPU 写完 CPU 仍读到缓存的零，正是 999/1000 全错的形态）**
  - metal34 日志 `2041.txt`（705,478 B，`134ab020…`）：
    - 无 `VM poison`/`observe` 失败；`backboard s34 e0 v4/4`、
      `SpringBoa s34 e0 v4/4`、Turnip `T s0 r0 v512` 都正常；
    - bridge 仍然走完 0x650f/0x6600，`M s0 c999 r64 i4096 q141 k3/4 p13/8 Fo`
      （与 metal33 完全相同的结果）；
    - fence 15：`fence timeout 15 … cp rptr 736 wptr 767 … fence 736/14`；
      framed 提交 = prologue(37) + IB(259) + framed fence(31)，rptr 停在
      767-31=736 → **CP 执行完 prologue 和 IB，停在 framed fence**
      （第一包就是 BV 线程的 `CP_THREAD_CONTROL`/BV 时间戳写 + BR
      `CP_WAIT_TIMESTAMP`）；CB_DISABLE 下没有 BV 线程写时间戳 → 永远等。
    - SpringBoard 后续提交 fence 19/21 也超时/半途，同一卡死 CP 的连带。
  - 修法（metal35）：
    1. `NativeAdrenoRing::submitWords`：framed 仍发 prologue/context entry，
       但 fence 一律用普通 `emitFence`（TYPE4 scratch + EVENT_WRITE7 到
       ring fence 地址），不再等 BV 时间戳；`emitFramedFence` 保留不用。
    2. `NativeAdrenoVm::retireJobAfterCompletion`：在 fence 完成后、释放
       引用前，对**该作业里可写且非 coherent 的 mapping** 调
       `acquireDeviceWrites()`（CPU cache invalidate）。原因：Metal 的
       新 buffer 由 CPU 零填充，cache 里是脏零行；GPU 非一致写只到 DRAM，
       CPU 读回仍是零 —— compute 恰好 999/1000 错（i=0 期望 0）。
  - metal35 产物：
    - 内核 `bef16dd02563466f9ba22d1669d6d4ff557030dfeed18a601a37f15d6227fbfb`
      17,055,888 B；
    - payload `shim/out/payload-metal35-nobvfence.bin`
      `60dc7b905effe344f41adbe2b8762c06445bb749460af820d87ec361e73c7b9f`
      25,459,392 B；
    - boot `shim/out/shim-boot-metal35-nobvfence.img`
      `8dee6e2004cfcebc7bc14dd0457f5c7befdb83f6394e893408c76afa98ee5693`
      32,034,816 B。
  - 上机：只 boot metal35。判读：
    - 若 framed fence 是唯一阻塞：fence 15/19/21 不再超时，bridge 的 M 行
      之前不再卡死，`Fo` 可能变真正打开；
    - 若 `c999` 变成 `c0`（或接近）→ cache invalidate 修对了；
    - 若结果仍全零、但 fence 正常 → 下一步查 metal2vulkan 的读回路径
      （是否走 vkInvalidateMappedMemoryRanges）或 shader/描述符。
    - 若 prologue 或 fence 出现新的卡点（rptr 又停在奇怪位置），看差值：
      37=prologue、259=IB、7/11=plain fence。

- **2026-10-29 GPU 第 278 步：metal33 是转折点——VM 不再 poison，bridge 自检**跑完全部阶段**（`M s0 c999 r64 i4096 q141 k3/4 p13/8 Fo`），fence 12-14（5/6/4 buffers、984/1056/880 B）是 compute/render/import 的真实提交且都 retire；但结果全错（compute 999/1000 错、render 64 错、import 4096 错），随后 fence 15（backboardd 1036 B）把 CP 卡死：rptr=wptr=217、fence 停在 14、无 fault。IB dump 里有 `CP_BV_BR_COUNT_OPS (0x1b)`（payload 2,1）——正是 Adreno740 注释里“跨 client 共享 ring、不重置 BR/BV 计数器 → 两个线程互相等死”的形态。metal34 打开 framed 提交（每个 client 切换带 context entry 清 BR/BV 计数器），并给 ring 自己发的所有 THREAD_CONTROL 加 CONCURRENT_BIN_DISABLE**
  - metal33 日志 `2028.txt`（663,050 B，`42886f98…`）：
    - 无 `observe`/`VM poison`/`BoFree` 失败；Turnip smoke 与 bridge 都到了终点；
    - bridge 结果行（第 4756 行）：
      `NativeAdrenoMetal: M s0 c999 r64 i4096 q141 k3/4 p13/8 Fo`
      ——stage 0（无错误），compute_bad=999、render_bad=64、
      import_bad=4096、QuartzCore 141 个函数、kernel 3/4、pair 13/8、
      `Fo`=frontend 未打开。说明 GPU 确实执行了 metal2vulkan 的提交，
      但输出全零/不对（CPU 读到的是未写入的缓冲区，或 shader 没跑对）。
    - 之后的卡死：
      `fence timeout 15 seq 15 next 217: cp rptr 217 wptr 217 … fence 217/14`，
      IB dump 里有 `40a9a801 …`（type7 op 0x1b = CP_BV_BR_COUNT_OPS，
      payload 0x2,0x1）、5 个 `CP_WAIT_FOR_IDLE (0x26)`、1 个 `EXEC_CS`；
      rptr 追到 wptr 但 fence 不写——CP 停在 BR/BV 计数器等待里，无 fault。
    - 之后 SpringBoard 的 smoke 提交（fence 16-18）也超时/半途，
      是同一个卡死 CP 的连带（我们的 stall handler 的 kick 让部分命令
      继续推进）。
  - 修法（metal34）：
    1. `NativeAdrenoRing` 新增 `RingCcuBinDisable = 1<<27`，prologue 的
       `br/syncBoth/both` 与 framed fence 的 `br/bvThread/both` 全部 OR 上
       它（这颗 CP 对清掉 CB_DISABLE 的 THREAD_CONTROL 会 trap，
       metal21/22）；
    2. `NativeAdrenoSubmit` 默认走 **framed**（`liuqin-gpu-framed=0`
       可退回 plain）：prologue 在 client 切换时带
       `CP_RESET_CONTEXT_STATE(0xf)`，清 on-chip BR/BV 计数器，正是
       Adreno740 int-249..254 的修法；`useContext()` 已在 submit 前调用。
  - 风险：framed prologue 里的 `SYNC_THREADS` 位以前单独测过会 trap
    （metal22 的 `0x80000003`），现在带着 CB_DISABLE 一起发；
    若仍 trap，raw test 会第一时间失败（prologue 就停），
    可用 `liuqin-gpu-framed=0` 回退。
  - metal34 产物：
    - 内核 `b8f661c9cf912c4df542914a7b4f101221186ea830a5d4766ff27a6ee8870a4b`
      17,055,888 B；
    - payload `shim/out/payload-metal34-framed.bin`
      `95ec68254e8e278a9a7085f3ddfdfd2b1dcb2214b4a520bbf2dfd0fcb36e2606`
      25,459,392 B；
    - boot `shim/out/shim-boot-metal34-framed.img`
      `365165de5a21b35f298b64833ca14a5002c37841b0becf706c25a302d8137f7c`
      32,034,816 B。
  - 上机：只 boot metal34。预期/判读：
    - 若 framed prologue 能跑：fence 15 那种 1036 B IB 不再卡死，
      bridge 的 `M` 行/stage 应继续稳定，`Fo` 可能变成真正打开；
    - 若 prologue 立刻 trap（raw test 就失败、rptr 停在很小值）→
      SYNC_THREADS 仍不能发，需要改成不带 sync 的最小 context-reset
      前导（THREAD_CONTROL CB|BOTH + CP_RESET_CONTEXT_STATE）；
    - 结果数值全错（c999 等）是下一层问题：GPU 执行了但输出不对，
      要考虑 cache 可见性（写入后 CPU 读到旧值）或 metal2vulkan/
      Turnip 在 a730 上的 shader/描述符路径。

- **2026-10-29 GPU 第 277 步：找到并修掉 VM 被 poison 的根因——这颗 QSMMU 的 SMR 必须是 `VALID|MASK(bit14)|sid`（0x84000000|sid），板级代码一直这么写，但 ArmSmmu4K kit 的 `valid_smr()` 只认 `VALID|sid`（0x80000000|sid）；于是第 64 次全域 `observe()` 把实际 route 判成 StateMismatch，把整个 VM poison 掉，bridge 因此 DeviceLost。已把 kit 的 `valid_smr()` 改成同一个值，出 metal33**
  - metal32 日志 `2020.txt`（574,342 B，`e3c9ba33…`）：
    - `observe route 0 slot 1 sid 0: smr 0x84000000 s2cr 0x2 (bank 2 expected smr 0x84000000)`
    - `observe route 1 slot 2 sid 1: smr 0x84000001 s2cr 0x2 (bank 2 expected smr 0x84000001)`
    - 实际 SMR/S2CR 与板级写入完全一致，但 `observe streams op 7 … verified 0`
      仍失败——说明 **kit 的期望值**不对，不是硬件被改。
  - 根因对照 `NativeAdrenoService.cpp`（GPU context bringup 之后）：
    ```c
    sm[0xc00/4+gslot0]=gpubank;
    sm[0xc00/4+gslot1]=gpubank;
    sm[0x800/4+gslot0]=0x84000000u;   /* VALID | MASK(bit14) | sid */
    sm[0x800/4+gslot1]=0x84000001u;
    ```
    注释明确写着：kit 写的 `VALID|sid` 会被这颗 qsmmu 丢掉（GMU 路由
    也需要同样处理），ladder 用 `VALID|0x400<<16|sid` 才能匹配。
    而 `kits/ArmSmmu4K/ArmSmmu4KStreams.c` 的
    `valid_smr(id) = 1<<31`、`valid_s2cr(id)=0`，`observe()` 按
    `0x80000000|sid` 比对 → 与板级实际值不一致 → StateMismatch。
  - 修法：`valid_smr()` 非 EXIDS 时返回 `(1<<31)|(0x400<<16)`
    = 0x84000000（EXIDS 仍返回 0）。attach、observe、quarantine 都改用
    这个值，和板级/硬件一致；`ArmSmmu4KAttachStreams` 从一开始就写成
    硬件要求的形态，随后板级那段重复写变成 no-op。
  - metal33 产物：
    - 内核 `fcdd7bccde2c0c9c45e7dd2b9342ae9f718687ffa977fb2ef5748f0f7aa1dd3f`
      17,055,888 B；
    - payload `shim/out/payload-metal33-smrmask.bin`
      `ce0553a5ee6461b31acb9d2da0443a736d55ee63508dfa74fe31f536d2153852`
      25,459,392 B；
    - boot `shim/out/shim-boot-metal33-smrmask.img`
      `472ff78b3193e0c6d8523881d2639a7a537e9d678cad490ecf0e312fcecea0b0`
      32,034,816 B。
  - 上机：只 boot metal33。预期：
    - 第 64 次 VM 操作不再 poison（没有 `observe streams op 7` /
      `VM poison` / `BoFree … unmap failed`）；
    - bridge 不再因 VM 失败而 `DeviceLost`，0x6503 的 compute 应能继续
      往下走（或暴露下一个真正的失败点）；
    - 若仍然失败，先看是否 `observe route` 的实际值又和 0x84000000 不同
      （例如 EXIDS 被固件打开）。

- **2026-10-29 GPU 第 276 步：metal31 把 StateMismatch 定到 `ArmSmmu4KObserveStreams` 的 route 扫描——`verified 0` 说明在验证第一个 owned stream 之前就不匹配，即某个 owned slot 的 SMR/S2CR 与 attach 时不同；context 镜像本身通过了（fault 0x400 不在 FSR_FAULT 掩码里，enabled=1）。metal32 打印当前 owned route 的 slot/sid/smr/s2cr 实际值**
  - metal31 日志 `2009.txt`（550,075 B，`2ae999ac…`）：
    - `NativeAdreno: observe streams op 7 phase 0 id0 0x4c017e18 scr0 0x2e0406 gfault 0x0 verified 0 ctx sctlr 0x167 ttbr0 0x20008a775c000 tcr 0x80803520 fault 0x400 enabled 1`
    - 判读：
      - `global()` 通过：scr0=0x2e0406，`scr0&0x407==0x406`；gfault=0；
      - context 镜像读回 sctlr=0x167、ttbr0=0x20008a775c000、tcr=0x80803520
        （boot 时 preflight 的 0x…758000 是 attach 前固件遗留值，attach 后
        编成我们自己的根表，当前值与 config 一致）；
      - `fault 0x400`：0x400 不在 `FSR_FAULT=0xc00001fe` 里，按驱动约定
        不算 context fault；enabled=1；
      - **`verified 0`**：scan 在验证第一个 owned stream 之前就返回
        StateMismatch —— 即某个 owned slot 的 `SMR`/`S2CR` 与 attach 时
        期望值（boot 日志 `smr1=0x84000000 s2cr1=0x2` / `smr2=0x84000001
        s2cr2=0x2`）不同。
    - 说明 routes（SMR/S2CR）在启动后被改过，或 scan 的期望计算与实际
      不一致；context/表 audit 这次都不是失败点。
  - metal32：observe 失败时按 owned 顺序打印
    `NativeAdreno: observe route i slot s sid n: smr 0x… s2cr 0x… (bank 2 expected smr 0x…)`
    —— 直接看出是 SMR 还是 S2CR 变了、变成什么。
  - 另：`/tmp/ld.lld` 在两小时后被系统清掉了，shim 构建因此一度用了旧
    boot 镜像；已用 `/Users/qprs/liuqin-build3/ld.lld`（zig 包装脚本）重建
    `/tmp/ld.lld`，并重链内核、重打 payload/boot。
  - metal32 产物：
    - 内核 `a97b42129a1cb282e1df66fbe8d4fac9488d0257ddad7a969bd6b331ed9fb68a`
      17,055,888 B；
    - payload `shim/out/payload-metal32-routes.bin`
      `89c509b4d0024394a21bb7ca7656529bdd119450b035369bd5c8670d5985afdb`
      25,459,392 B；
    - boot `shim/out/shim-boot-metal32-routes.img`
      `7104e9b7ce66f9dbd4c20f1b0b33285c70cd8b3c2dce0ceca9925abd4955089e`
      32,034,816 B。
  - 上机：只 boot metal32。看到 `observe route …` 后判断：
    - 若 SMR 的 EXIDS/EXIDVALID 位被清或 SID 变了 → 有别的 agent（SQE/
      固件）在改 SMR；
    - 若 S2CR 不再是 `0x2`（例如变成 FAULT/别的 bank）→ stream 被重定向，
      查谁写 S2CR；
    - 若 smr 与 expected 完全一致但 observe 仍失败 → scan 的期望值或
      id0/EXIDS 语义与实际不符，需要把 `valid_smr/valid_s2cr` 的实际值也打出来。

- **2026-10-29 GPU 第 275 步：metal30 证明这次 BoFree 的失败点不是 range 比较、也不是 acquireDeviceWrites，而是 `observeCheap()` 的第 64 次全域 `observe()` 返回 `ArmSmmu4KStateMismatch`（op 7 / stage 11，且没有 “unmap state:” 行）；metal31 把 observe 拆开打印（streams vs context image vs 表 audit），定位到底哪一半不一致**
  - metal30 日志 `liuqin-log-20261029-172950.txt`（527,199 B，`b25dabc0…`）：
    - `NativeAdreno: VM poison op 7 stage 11 objects 15 spans 15`；
    - 紧接着 `BoFree handle 0xa iova 0x20044000 bytes 16384 unmap failed 0xe00002ca (stage 11 op 7 imported 0)`；
    - **没有** metal29 加的 `NativeAdreno: unmap state: …` 行，
      也没有 `map … live update failed`/`publish failed`。
  - 解读：unmap 里 stage 11 覆盖三处：`observeCheap()` 失败、
    `acquireDeviceWrites` 失败（会是 op 8）、range 比较失败（会有
    `unmap state:` 行）。这次三者里只有 `observeCheap()` 符合：
    `observe()` 每 64 次表操作跑一次全域检查，返回 7（StateMismatch）。
  - 重要背景：每次 submit 的 `invalidateTlb()` 只走
    `ArmSmmu4KObserveOwnedContext`（**只看 context 镜像**，不看 routes、
    也不走表 audit），所以 routes/表 的不一致可以一直存在，直到第 64 次
    操作的全域 `observe()` 才暴露。
  - `NativeAdrenoVm::observe()` 依次做：
    1. `ArmSmmu4KObserveStreams`：全局 scr0 策略 + context 镜像 +
       所有 slot 的 route 扫描（owned 的 SMR/S2CR 必须精确匹配）；
    2. `ArmSmmu4KLiveAuditContext`：按 `ranges_` 走一遍页表（与表内容比对）；
    3. 再做一次 ObserveStreams。
  - metal31 拆开日志：
    - streams 失败：
      `NativeAdreno: observe streams op … phase … id0 0x… scr0 0x… gfault 0x… verified … ctx sctlr 0x… ttbr0 0x… tcr 0x… fault 0x… enabled …`
      ——`ctx` 是实际读回的 context 镜像，`gfault` 是全局 fault；
    - 表 audit 失败：
      `NativeAdreno: observe audit op … reads … writes … complete … ranges a/b leaves …`
      ——`ranges` 是 `ranges_` 里 lower/upper 的条数，`leaves` 是表状态里
      已映射的叶子页数；两者对不上就说明范围记账和页表不一致。
  - metal31 产物：
    - 内核 `cf2f6f04bc3d310e9e0dce3cdc860bc8ede04c4287d11628014a88252aadd250`
      17,055,888 B；
    - payload `shim/out/payload-metal31-observe.bin`
      `9c5c752e8718b223718bd18789aa45e883360402b7a11ef8cc884c221309116f`
      25,459,392 B；
    - boot `shim/out/shim-boot-metal31-observe.img`
      `fb330f8909ef21e038dfad77ad96be70657327019a52ddb5b922e7cf9b36c121`
      32,034,816 B。
  - 上机：只 boot metal31。看 `observe streams` / `observe audit` 哪条出现：
    - streams + ctx sctlr/ttbr0 与 attach 时不同 → context 被谁改了
      （SCTLR/TTBR）；`scr0`/`gfault` 变化 → 全局策略/故障；
    - audit + ranges 与 leaves 不一致 → 我们的 `ranges_` 记账与页表
      分叉（map 的 insert/erase 或 live update 的合并语义）；
    - 若两条都没有（只是 op 7 但路径不同），再扩日志。

- **2026-10-29 GPU 第 274 步：metal29 显示这次最早毒掉 VM 的不是 BoFree 而是 SpringBoard raw test 的 **BoMap**（method 0x3 返回 0xe00002ca），之后的 BoFree 全是 NotReady 连带；说明 map 和 unmap 都会走到 `NativeAdrenoVm::fail()`。metal30 在 `fail()` 里打印 op/stage/objects/spans，并在 map 内打 stage 20..24、publish 失败和 live-update 失败（含 op/budget）**
  - metal29 日志 `1723.txt`（551,858 B，`53712b09…`）：
    - 这次 SpringBoard 成功 open 了 0x4147（`IOK SpringBoard: open AGXAccelerator type 16711 -> 0x0`），
      raw test 跑到 **stage 5** 就失败：
      `NativeAdrenoMetal: SpringBoa s5 ee00002ca v-1/-2 t0 a256/0`；
    - 驱动侧第一条失败是
      `NativeAdreno: method 0x3 proc SpringBoard failed 0xe00002ca`（第 3734 行，**BoMap**）；
    - 紧随其后的 `BoFree handle 0x1 iova 0x60000000 bytes 4096 … (stage 10 op 0)`
      和 `BoFree handle 0x3 iova 0x20028000 …` 都是 VM 已 poison 的连带
      （unmap 入口 `!attached_ || poisoned_` → NotReady）。
    - 也就是说：metal28 的 unmap StateMismatch 和 metal29 的 map 失败，
      都只是“谁先碰到 poison”的差别；真正要抓的是 `fail()` 被调用时的
      `ArmSmmu4KStatus` 和调用点。
  - metal30 诊断：
    - `NativeAdrenoVm::fail()` 第一次（最多 8 次）打印：
      `NativeAdreno: VM poison op <ArmSmmu4KStatus> stage <trace stage> objects <n> spans <n>`
    - `map()` 的 trace stage：20 进入、21 observeCheap 后、22 publish 后、
      23 live update 前、24 live update 成功后；
    - publish 失败：`NativeAdreno: map 0x… bytes …: publish failed 0x… (imported … wc …)`；
    - live update 失败：`NativeAdreno: map 0x… bytes … spans … upper …: live update failed op … (budget …)`。
  - metal30 产物：
    - 内核 `f790b55f932c435b0a549e6d81375a1881e3763a94bcf46df1c8ce74d626ab11`
      17,055,888 B；
    - payload `shim/out/payload-metal30-vmpoison.bin`
      `ffcea6c02de31416e0f48b692dc89cfddd6d8027b9538a477725749cbc434d23`
      25,459,392 B；
    - boot `shim/out/shim-boot-metal30-vmpoison.img`
      `c5cad100b04feaa52454e16d60900d9cfade9ffcd278d49147f26f27924321ec`
      32,034,816 B。
  - 上机：只 boot metal30。看 `VM poison op … stage …`：
    - op 7（StateMismatch）且 stage 23 → live update 里发现表状态不一致（重复
      iova / 旧描述符未清）；
    - op 8/9/10/11（IoError/Timeout/Busy/Fault）→ 看是 observe 还是 update；
    - stage 22（publish 失败）→ 内存维护/隔离问题，与 SMMU 表无关；
    - 那条 `map … live update failed` 还会给出 budget，可排除预算耗尽。

- **2026-10-29 GPU 第 273 步：metal28 把 BoFree 失败收窄到 `NativeAdrenoVm::unmap` 的“快照不一致”——stage 11、op 7 = `ArmSmmu4KStateMismatch`（不是 cache 维护 IoError）；失败 BO 是 handle 0xa / iova 0x20044000 / 16384 B / 非导入。metal29 打印具体是哪一项不一致（记录 spans 不在表里，或 iova/physical/bytes/permissions/writeback/shareability 哪个字段对不上）**
  - metal28 日志 `1714.txt`（574,648 B，`d5b89cad…`）：
    - `NativeAdreno: BoFree handle 0xa iova 0x20044000 bytes 16384 unmap failed 0xe00002ca (stage 11 op 7 imported 0)`
      紧接在 0x6501（device）之后、0x6502 之前，与 metal27 同一点；
    - 第二条 `BoFree handle 0x1 iova 0x20000000 bytes 32768 … (stage 10 op 0)` 是
      VM 已被 poison 的连带结果（`unmap` 入口就返回 NotReady）。
  - 解读：`fail()` 永远返回 `kIOReturnIOError`，所以返回码不能区分；
    `op 7` 才是真正的原因 = `ArmSmmu4KStateMismatch`，`stage 11` 说明失败在
    unmap 的这两处（都在 SMMU 表更新之前）：
    1. `spans > spanCount()-at`：按 `record->iova` 在 live 表里找不到这个
       mapping 的 span 块（`at` 走到表尾，或剩余 span 不够）；
    2. 逐 span 比较失败：`r.iova != record->iova+span.offset`、
       `r.physical != span.physical`、`r.bytes != span.bytes`、
       `permissions/writeback/shareability` 与记录不一致。
  - 也就是说：BO 的映射记录与 VM 的 live range 表在 unmap 时对不上，
    而不是缓存维护失败。可能的方向：同一 iova 的旧记录残留/重复插入、
    插入排序假设被破坏、或 map 时记录的属性与提交到表里的属性不同。
  - metal29 新增（`NativeAdrenoVm::unmap`）：
    - 找不到块时：
      `NativeAdreno: unmap state: handle 0x… iova 0x… spans … not in table (at … of …)`；
    - 字段不一致时：
      `NativeAdreno: unmap state: handle 0x… iova 0x… span i/n (off 0x… pa 0x… bytes …) range (iova 0x… pa 0x… bytes …) perms a/b wb a/b share a/b`。
  - metal29 产物：
    - 内核 `3298fa37a39559f7da0c06109f14d47b4fb25b9c3d1fec183917d168ca253a44`
      17,055,816 B；
    - payload `shim/out/payload-metal29-unmapstate.bin`
      `073cabc3fa6b6e32cf1cbc703796ce9be3e33f8e3a1cfcce46e81c612c863aae`
      25,459,328 B；
    - boot `shim/out/shim-boot-metal29-unmapstate.img`
      `a00f4833ea5a7de70861ffe3f96e8674fbf35ba375c77c282134773671edb25d`
      32,034,816 B。
  - 上机：只 boot metal29。桥段仍会 DeviceLost，但会多一行 `unmap state:`；
    据它决定修法：
    - “not in table” → 查是否有记录在 unmap 前被别的路径 erase
      （例如同一 BO 二次映射、或 slot 复用）；
    - 字段不一致 → 按不一致的字段查 map 时写入值（尤其 writeback/shareability）。

- **2026-10-29 GPU 第 272 步：metal27 抓到 DeviceLost 的源头——bridge 建好 device 后第一次 BoFree（method 0x4）返回 `0xe00002ca`（IOError），`NativeAdrenoVm::unmap` 失败 → `NativeAdrenoVm::fail()` 把整个 VM poison 掉；此后没有任何新 submit，bridge 报 DeviceLost。metal28 给 BoFree 加详细日志（handle/iova/bytes、unmap 停在 stage 10..14 的哪一步、ArmSmmu4KStatus op）**
  - metal27 上机日志 `liuqin-log-20261029-170717.txt`（610,005 B，`095e1d74…`）：
    - 探针仍正常：`GMU AHB fence status … 0x68000: 0x0`，无 WPTR/stall 行；
    - 方法失败日志只有三条：
      - `method 0x9 proc backboardd failed 0xe00002c2`（SetMetadata，
        BadArgument；第 2373 行，早于 GPU 测试，先记下）；
      - `method 0x8 proc backboardd failed 0xe00002c7`（GetStatus
        Unsupported——正常，没 attach status）；
      - `method 0x4 proc backboardd failed 0xe00002ca`（**BoFree**，
        第 3999 行），紧接在 0x6501（device）之后、0x6502（library）之前。
    - BoFree 失败前有 31 条 `IOMemoryDescriptor physical with managed page
      0x…:0x4000`（导入的 16 KiB 页），最后一批就在失败前（第 3993-3997 行）。
    - 失败后**再无任何 `NativeAdreno: submit`**（最后一条是 fence 11，
      第 3982 行），bridge 的 compute 之后报 `M s6503 … DeviceLost`。
    - 结论：DeviceLost 不是 Turnip 自己置的，是 kernel 的 BoFree 失败让 VM
      被 poison（`NativeAdrenoVm::fail()` 返回 kIOReturnIOError 并 quarantine
      所有映射），后续分配/映射当然失败。Turnip 侧只是把失败当设备丢失。
  - 可能的根因（待 metal28 的 stage/op 确认）：
    - `NativeAdrenoVm::unmap` 中 `fail()` 会在四处被调用：
      observeCheap（stage 10）、`acquireDeviceWrites`（stage 11，
      imported 页的 cache 维护）、`ArmSmmu4KLiveUpdateContextBatch`
      （stage 12/13）、`retireAfterQuiescence`（stage 14）；
    - 失败 BO 是导入的 16 KiB 页（0x4000）那一批；要区分是导入页的
      cache 维护失败、SMMU 表更新失败，还是快照不一致。
  - metal28 新增日志：
    - `NativeAdreno: BoFree handle 0x… iova 0x… bytes … unmap failed 0x… (stage … op … imported …)`
      ——`stage` 是 unmap 内部 `native_adreno_trace_stage`（10 进入、
      11 observeCheap 后、12/13 SMMU 更新、14 更新后），`op` 是
      `ArmSmmu4KStatus`；
    - `NativeAdreno: BoFree handle 0x… bytes … release failed 0x… (stage … imported … mapped …)`
      ——如果 unmap 成功、是 `NativeAdrenoMemory::release` 失败，则
      stage 为 30..34（descriptor complete/release 各步）。
  - metal28 产物：
    - 内核 `5f1bb022c5dbaa19a6c95c89e3c22fca5b09b8561fcfb6585effb9ad491cdaf4`
      17,055,816 B；
    - payload `shim/out/payload-metal28-bofree.bin`
      `49d59ca24d4b7a283d5c7f511fc68796ae653bc6191425aa0034e4fd87003c1f`
      25,459,328 B；
    - boot `shim/out/shim-boot-metal28-bofree.img`
      `c32c8638395555151d4fe74dc90b7cb781d0aeb777ef801c05a2c23fd4d8ee52`
      32,034,816 B。
  - 上机：只 boot metal28。预期桥段仍 DeviceLost，但会多一条 BoFree
    细节行；据 stage/op 决定下一步：
    - stage 11/op IoError（cache 维护）→ 查 imported 页的
      `NativeAdrenoMaintainPhysicalPages`；
    - stage 12/13（SMMU 更新）→ 查表更新/预算；
    - stage 14（快照不一致）→ 查 unmap 前的 ranges/record 一致性；
    - release 失败 → 查 descriptor complete/prepare。
  - 另：client 槽仍是 4 个，backboardd 占满后 SpringBoard/chronod/
    Preferences 的 Turnip 初始化都被 Busy 拒；后面跑多进程 GPU 合成要扩槽。

- **2026-10-29 GPU 第 271 步：metal26 证实 GMU fence 偏移修对了（探针 0x68000→0x0，0x6a000→0xdeadbeef；不再有 “still dropped”），但 bridge 仍然 `M s6503 … DeviceLost`；同一份日志里 SpringBoard 的 Turnip 因 `IOServiceOpen(0x4147)` 返回 `0xe00002d5`（忙）初始化失败——4 个 client 槽被 backboardd 的连接占满；metal27 给每个 ABI 方法加失败日志，抓 DeviceLost 前到底是哪个调用失败**
  - metal26 上机日志 `1657.txt`（583,578 B，`f82cac61…`）：
    - 第 278 行探针：
      `NativeAdreno: GMU AHB fence status at window delta 0x68000: 0x0 (0x6a000 would read 0xdeadbeef)`
      ——0x9b13 读回 0，0x9313 才是 0xdeadbeef，偏移正确；**全日志没有一条
      `WPTR write … still dropped`，也没有 stall 行**。
    - Turnip smoke 仍通过（`T s0 r0 v512 c0 g0/4096`），无 panic、
      无 fence timeout；
    - bridge 仍然 `M s6503 c0 r0 i0 q0 k0/0 p0/0 DeviceLost`（第 4444 行），
      0x6503 之后只有 metal2vulkan 的 PrecisionNote，没有任何新的
      `NativeAdreno: submit`。
    - 新发现：`|SpringBoard| TU: error: … IOServiceOpen(IOAcceleratorES, 0x4147)
      failed: 0xe00002d5 (VK_ERROR_INITIALIZATION_FAILED)`（第 4422 行），
      驱动侧对应 `NativeAdreno: metal client refused 0xe00002d5 type 0x4147
      proc SpringBoard`——`AGXDeviceUserClient::mount()` 的 4 个 client 槽
      （`NativeAdrenoMaxClients=4`，slot0 给 backboardd）被 backboardd 的多次
      连接占满，其他进程的 Turnip 设备起不来。之后 chronod、backboardd
      也有同样的 Busy 拒绝。
    - 结论：DeviceLost 不是 fence 超时、不是 GMU 误写（metal26 已排除），
      也不是 panic；需要知道是哪个 ABI 方法先失败。
  - metal27：在 `runCore()` 返回前加失败日志（每种方法最多 3 次，跳过
    WaitFence 的 timeout）：
    `NativeAdreno: method 0x%x proc %s failed 0x%x`
    ——`method 0x8`（GetStatus）的 `0xe00002c7`（Unsupported）是正常的
    （没有 attach status，Turnip 会当成功），其余任何失败都值得看。
  - metal27 产物：
    - 内核 `074706ebcc555528cec5d29ec0c4407df121ca70fd19b30f258a29be3993f9d6`
      17,055,816 B；
    - payload `shim/out/payload-metal27-methodfail.bin`
      `a4112dc381ec1044945e4531d579aa2ed13f7018f25e89c0b8fb76851d66e484`
      25,459,328 B；
    - boot `shim/out/shim-boot-metal27-methodfail.img`
      `0809958cb193211676a4798f3e040f0cf069caf70eb2d97487669c23f6084c8c`
      32,034,816 B。
  - 上机：只 boot metal27，root 不动。预期：
    - 先出现 `GMU AHB fence status … 0x0` 探针；
    - 若 bridge 仍 DeviceLost，看它前面最近的 `method … failed 0x…`：
      若失败的是 BoFree/BoMap/Submit/WaitFence，就能直接定位到 kernel 侧；
      若一条 failure 都没有，则丢失是 Turnip/metal2vulkan 内部置的
      （下一步要往 Turnip 的 `tu_knl_darwin.cc` 加日志或复现）。
    - 另有 client 槽问题：backboardd 的连接占满 4 槽后 SpringBoard/chronod
      的 Turnip 初始化失败；若后面要跑多进程 GPU 合成，需要扩槽或让
      不用的连接及时关闭。

- **2026-10-29 GPU 第 270 步：metal25 日志暴露 GMU fence 状态偏移用错了板子——0x6a000 是 sheng 的窗口差，这颗 8475 是 0x68000（窗口 0x2d68000/GPU 0x2d00000），正确窗口字是 0x9b13 而不是 0x9313；metal25 每次提交都在读 0xdeadbeef 并往错误 GMU 寄存器写 0x7，可能正是这次 bridge 报 DeviceLost 的原因；metal26 修正偏移并加一次性探针**
  - metal25 上机日志 `1642.txt`（658,902 B，`2b7524c9…`）：
    - 无 panic；raw test `backboard s34 e0 v4/4`；Turnip 仍通过
      （`T s0 r0 v512 c0 g0/4096`）；bridge 这次**没有挂住**，worker
      正常结束并打出 `M s6503 c0 r0 i0 q0 k0/0 p0/0 DeviceLost`
      （第 4381 行），随后是 0x65f1 fail。
    - 但每次提交都有：
      `NativeAdreno: WPTR write <n> still dropped after 201 writes (status 0xdeadbeef)`
      （11 次，对应 11 条提交）——**这是驱动的新 bug，不是真丢写**。
  - 根因：Adreno740 里的 `0x6a000` 是 **sheng** 的
    `GMU core - GPU` 窗口差（sheng 上 `0x23b13*4-0x6a000` = 窗口字 0x9313）。
    这颗 8475 的窗口来自生成的 `sheng_gmu_window_physical[]`
    （`ShengGmuSource.h`）：
    `[0]=0x2d68000`（GMU core）、`[3]=0x2d00000`（GPU）→ 差 **0x68000**，
    所以 `GMU_AHB_FENCE_STATUS` 的窗口字 = `0x23b13 - 0x1a000` = **0x9b13**
    （字节 0x26C4C）——和生成头里 `REG_GMU_AHB_FENCE_RANGE_0 = 0x9b11`
    的 rebase 一致。用 0x6a000 会读到窗口字 0x9313（读回 0xdeadbeef），
    并且 CLR 会写到窗口字 0x9314（**错误寄存器**，每次提交都写 0x7）。
  - 修法（metal26）：
    - `NativeAdrenoService::gmu_fence_status()` 的 delta 改为
      `sheng_gmu_window_physical[0]-sheng_gmu_window_physical[3]`（=0x68000），
      不再硬编码 0x6a000；
    - 首次调用时只读探针打印一行：
      `NativeAdreno: GMU AHB fence status at window delta 0x68000: 0x… (0x6a000 would read 0x…)`
      ——下一次上机就能确认 0x9b13 读回 0，0x9313 才是 0xdeadbeef；
    - 只有 bit0 真置位时才会写 CLR 并重写 WPTR。
  - 判断：metal25 的 `DeviceLost` 很可能是那次错误 GMU 寄存器写（0x9314←7）
    造成的；如果 metal26 不再出现 “still dropped”，bridge 的
    compute 才有干净的环境重测。
  - metal26 产物：
    - 内核 `4431f5ccf360c5e70c7facf31cd3a4c4ecf3e38fb5065b1fa83d44ad5c41a37f`
      17,055,744 B；
    - payload `shim/out/payload-metal26-fencedelta.bin`
      `fb620237d8d299a13ecd82fef23036a96a6ff0ea5a17d6730f83329b4b72243a`
      25,459,248 B（cmdline 586 B 与 metal24/25 相同）；
    - boot `shim/out/shim-boot-metal26-fencedelta.img`
      `f785cf5c1ec7f282d94739cb4a93eb80335c7ef70d4d900ba309f86d6766c42a`
      32,034,816 B。
  - 上机：只 boot metal26，root 不动。预期：
    - 探针行 delta 0x68000，正确字读回 0（若 0x9313 也读 0，则说明两个
      窗口都读 0，仍以不再出现 “still dropped” 为准）；
    - 不再有 `WPTR write … still dropped`（真丢写时才会出现，且最多 201 次后
      仍会继续提交）；
    - bridge 是否仍 `DeviceLost`、M 行 stage 是否前进，是下一步的关键。

- **2026-10-29 GPU 第 269 步：把 UniXNU/Adreno740 新驱动里的三项修复移植进我们的 NativeAdreno——fenced WPTR（GMU AHB fence status）、per-client `useContext` context 切换、stall 时 `kick`+TLB 失效+`drainBus`；内核/boot 镜像 metal25 已出，未上机**
  - 来源：用户给的 `https://github.com/UniXNU/Adreno740`（clone 在 `/tmp/Adreno740`）。
    它的 `NativeAdrenoCpInit.cpp` 与我们的快照一致，但 Ring/Submit/Service/Publish 更新。
    AdrenoMetal（`/tmp/AdrenoMetal`）的 bridge 源码与我们 bundle 里的基本一致。
  - 移植内容（保留我们的全部诊断日志/类型 1 discovery，未整文件覆盖）：
    - `NativeAdrenoRing.h/.cpp`：
      - `NativeAdrenoRingIo` 新增 `fence_cookie`/`fence_status`（可选，缺省不检查）；
      - `writeWptr()`：写完 `CP_RB_WPTR` 后读 GMU_AHB_FENCE_STATUS
        （`ShengGmuCore` 窗口，`0x23b13*4-0x6a000`），bit0 置位就清
        （`0x23b14`）并重写，最多 201 次；读不到时**只关掉检查、不 poison**；
      - `submitWords()` 的 WPTR 写改走 `writeWptr()`；
      - 新增 `stallState()`（22 个 CP/RBBM 寄存器）、`kick()`（重写已发布的
        WPTR）、`drainBus()`（GBIF halt/ack 0x3c45/0x3c46）、`ibBefore()`；
      - 新增 `context_`/`useContext(ctx)`：client 变化 → 下一次 framed prologue
        带 context entry（`reset_pending_`）。
    - `NativeAdrenoService.cpp`：新增 `gmu_fence_status()`（经 `gmu_io_` 读
      GMU core 窗口），`ring_io` 初始化时带上 `&gmu_io_` + 该函数。
    - `NativeAdrenoUserClient.h/.cpp`：每个 client 在 `open()` 里取一个自增
      `context_`，`submit()` 前 `ring_->useContext(context_)`（取代原来在
      open 里无条件 `requestContextReset()`）。
    - `NativeAdrenoSubmit.h/.cpp`：`noteStall()`——fence 超时后按“同一 fence 已
      等待时长”分两级：≥50 ms 失效 TLB + `kick()`；≥200 ms `drainBus()`；
      计数在 `native_adreno_unstick[10]`。**只在超时路径触发**，正常路径行为不变。
  - **framed 仍默认关闭**（boot 行无 `liuqin-gpu-framed` → plain submit），
    所以 context entry 目前是 dormant；`liuqin-gpu-framed=1` 才走 framed prologue。
    原因：metal15 的 framed prologue 停在第 13 个 word，且这颗 CP 对任何清掉
    CB_DISABLE 的 `CP_THREAD_CONTROL` 都 trap（第 267/268 步）。
  - 构建：`WORK=~/liuqin-build3 LLD=/tmp/ld.lld tools/build-local.sh`，
    exit 0（只有既有 warning）。
    内核 `~/liuqin-build3/fakeroot/System/Library/Kernels/kernel.development.liuqin`
    `f70f9517e34db7333ed5ed09eb93db0219fa8bf0c3a57e28e2a782d98e70557b`，17,055,672 B。
  - metal25 产物：
    - payload `shim/out/payload-metal25-wptr.bin`
      `69920e3cd5665a87e4696bf45041c461adc44f12de5097d5ff7521315d205742`
      25,459,184 B（cmdline 586 B，与 metal24 逐字节相同；ADT `LIUQINFW` ✓，
      fwarea 0x800000 @ 0x84253c000）；
    - boot `shim/out/shim-boot-metal25-wptr.img`
      `8de6b0ecf86667982f3f17a514ed99f0014651657b56ebc02e72b9b16c0ce003`
      32,034,816 B。
  - 上机预期（只 boot，不动 root；root 仍是 gpu2 镜像）：
    - 启动路径与 metal24 相同：GPU 服务发布、CP 自检 `0x600dc0de`、
      Turnip `T s0 r0 v512`、bridge 到 0x6503；
    - 新日志只会在异常时出现：
      `NativeAdreno: WPTR write <n> dropped by the GMU (status 0x…)`、
      `NativeAdreno: stall fence <f> pending <us> us: invalidating translations and rewriting WPTR`、
      `NativeAdreno: stall fence <f> pending <us> us: bus drained, halt ack 0x../0x..`；
    - 若一条都没有，说明 WPTR 从未被丢、也没触发 stall handler（符合 metal24
      没有 fence timeout 的情况），驱动行为与 metal24 等价。
  - 注意：`useContext` 的 context entry 只有在 framed 打开后才会真正发出；
    下一步若要验证 context switch + 这颗 CP 的 framed prologue，需要单独的
    boot 镜像（或 boot 行加 `liuqin-gpu-framed=1`）。

- **2026-10-29 GPU 第 268 步：确认这颗 CP 不能“打开 concurrent binning”——任何清掉 CONCURRENT_BIN_DISABLE 的 `CP_THREAD_CONTROL` 都会 trap；Turnip 的 `tu_init_hw` 恰好无条件发 BOTH(bit27=0) → 已把 Turnip 源码改成所有 thread-control 都带 CB_DISABLE，重建库并换进新 root 镜像；metal24 关闭 CP 试验、保留 Turnip marker 日志**
  - metal23（`1526.txt`，572,705 B，`2fe41594…`）：
    - ME_INIT 加 bit 8 后 CP 仍能启动、自检通过（bit 8 不破坏启动），
      但 plain `CP_THREAD_CONTROL(3)` 仍然挂；
    - 挂掉之后 `rbbm3` 出现 `0x100000`（bit20，XML 未定义）——像 CP trap
      后被置位；CB_DISABLE|BOTH 的测试因为 CP 已被前一条停住而连带超时。
  - 综合 metal21/22/23：`0x08000003`（CB_DISABLE|BR|BV）能完成；
    `0x00000003` / `0x80000003` 都会停 → 这颗 a730 gen1 的 CP
    **不能在运行时清 CB_DISABLE（打开 concurrent binning）**。
  - Turnip 侧根因：`tu_init_hw()` 对 A7XX 无条件
    `CP_THREAD_CONTROL(BOTH)`（bit27=0），所以每条命令流第一条就 trap。
  - 修复（Turnip 源码，`.cache/mesa-work`）：
    - `tu_cs.h::tu7_thread_control()` 的 payload OR 上
      `CP_THREAD_CONTROL_0_CONCURRENT_BIN_DISABLE`；
    - `tu_cmd_buffer.cc`（14 处）、`tu_query_pool.cc`（1）、
      `tu_lrz.cc`（2）直接 `CP_THREAD_CONTROL_0_THREAD(...)` 的地方
      同样 OR 上 CB_DISABLE（含 patchpoint 的 patch/original value）；
    - `ninja -C build/mesa-tu-ios src/freedreno/vulkan/libvulkan_freedreno.dylib`
      重建：17,720,928 B；ad-hoc 签名后 17,877,664 B，
      sha256 `37e999d4…`。
  - 新 root 镜像 `~/liuqin-root/ipad71-root-wrap16-gpu2.img`（16 GiB，
    `827314b86a0f33efffde1e3738b791a14981ac9d2d4850f69dd5c536d84d9da2`）：
    只把 bundle 里的 Turnip 换成新版，`codesign --force --deep -` 重签，
    `codesign --verify --deep --strict` 通过，fsck OK。
  - metal24 内核（`01f4d2fd…` 17,054,896 B）：CP 试验改为
    `liuqin-gpu-cptest` 开关（默认关，避免把 CP 停掉）；保留
    Turnip marker/T 行日志、fence 超时诊断、260 dword IB dump、
    CP timer、plain submit。
  - payload `705b5d5e…` 25,458,400 B；boot
    `shim/out/shim-boot-metal24-tucb.img`
    `308c2077b7306e7c50270b37a8e2cef7ea7def45beb8259069e7659f5c66aca1`
    32,034,816 B。
  - 上机：adb push 新 root → `dd if=/sdcard/… of=/dev/block/sda36 bs=4M
    conv=fsync` → boot metal24 → 拉日志。预期 Turnip 第一条
    thread-control 不再 trap，`submit …/932` 能 retire，日志出现
    `turnip marker` 和 `NativeAdrenoMetal: T …`。
  - 若 Turnip 后续又因别的 CB/线程包 trap，同样按日志定位；若
    rendering 结果不对，再查 a730 gen1 与 a740 的差异。

- **2026-10-29 GPU 第 267 步：metal22 定位——`CP_THREAD_CONTROL(0x08000003)`（CB_DISABLE|BR|BV）能完成，`0x80000003`（SYNC_THREADS|BR|BV）会挂；即“同步到 BV / 改变 CB 状态”这一步 CP 过不去；metal23 给 ME_INIT 加 bit 8 并重排测试**
  - `1508.txt`（554,357 B，`45d64b6e…`）：
    - SRAM 仍 0→1；
    - `cp test thread-control-cb-both submit=0x0 fence=4 result=0 observed=4`
      → **`CP_THREAD_CONTROL(0x08000003)` 完成**（CP 接受 CB disable + 两个线程）；
    - `cp test thread-control-sync-both ... result=1`：
      rptr 停在 48（该提交起点），fence 5 没回来 → **SYNC_THREADS 挂**；
    - 之后的 plain 3 / events / SPU / preamble 全部因为 CP 已停而超时。
  - 结合 metal21（plain `3` 直接挂）：CP 能接受
    “CB_DISABLE|BR|BV”（不改变 CB、不同步），但只要
    **清掉 CB_DISABLE**（plain 3）或 **显式 SYNC_THREADS** 就停。
    按 adreno_pm4.xml 的说明，清 CB_DISABLE 会隐式触发线程同步/
    skipsaverestore 抢占，和 SYNC_THREADS 走的是同一套机制。
  - 推断（**未证实**）：BV 线程没有被 ME_INIT 真正启动，或缺少
    BIN preamble amble，所以任何“等 BV/同步线程”的包都会挂。
    Turnip 的 `tu_init_hw` 对 A7XX 无条件发 `CP_SET_THREAD_BOTH`
    （payload 3），因此每条命令流第一条就挂。
  - metal23：把 ME_INIT 的 mask 从 `bits 0,1,3,6` 改成
    `bits 0,1,3,6,8`（sheng 注释里 Linux a7xx_cp_init 的完整 mask），
    并把 trial 顺序改成 plain-3 → sync-both → cb-both，看 bit 8 是否
    启动/修复 BV 同步。
  - 产物 metal23：kernel `1188fe04…` 17,054,336 B；payload
    `1ca92b4a…` 25,458,256 B；boot `shim/out/shim-boot-metal23-meinit8.img`
    `e56d3cac4c68fa15b2b6db81b2da83a881cd26fe07be029abe1cb906201347b7`
    32,034,816 B。
  - 上机：只 boot metal23，等 5 分钟拉日志。看 CP 是否仍能启动/
    自检（bit 8 不能破坏 ME_INIT），以及 plain-3 是否变成 result=0。
  - 若 bit 8 无效：下一步在 ME_INIT 里显式启动 BV / 配 BIN preamble
    amble，或改 Turnip 的 tu_init_hw 在 A730 gen1 上保持
    CONCURRENT_BIN_DISABLE（需要重建 Turnip 库）。

- **2026-10-29 GPU 第 266 步：抓到停住 CP 的具体包——`CP_THREAD_CONTROL(3)`（Turnip IB 的第一条）单独提交就会让 CP 停住；SRAM init 已排除（0x050f 0→1）；metal22 试 3 种 thread-control 变体定位可接受形式**
  - `1500.txt`（579,563 B，`b2a95111…`）：
    - `CP self-test ... fence=3 ... wrote=0x600dc0de` 仍然通过；
    - `perfctr SRAM status before 0x0` → `after 0x1 (cmd 0x050e=1)`：
      SRAM 初始化/等待序列工作正常，**不是** sheng 注释里那个
      “SRAM still clearing”问题；
    - `cp test thread-control-both submit=0x0 fence=4 result=1 observed=3`
      → 仅 2 word 的 `CP_THREAD_CONTROL(BR|BV)` 提交，CP 只取到
      rptr 39（正好在 thread-control 包之后、fence 包 39..45 之前），
      fence 4 永不回来；
    - 后面的 events/SP_UPDATE_CNTL/preamble28 全部连带超时，因为 CP
      已经停住。
  - 结论：**`CP_THREAD_CONTROL` payload=3 是停住 CP 的包**；Turnip 的
    932 B IB 第一条就是 `70970001 00000003`，所以整条命令流一步都没跑。
    这与 sheng 注释里“客户端第一个 IB 在第一条 CP_THREAD_CONTROL 上停住”
    的现象一致，但原因不是 SRAM（已排除）。
  - metal22 在 CP 启动后依次试（每步 500 ms）：
    1. `CP_THREAD_CONTROL(0x08000003)` = CONCURRENT_BIN_DISABLE|BR|BV
    2. `CP_THREAD_CONTROL(0x80000003)` = SYNC_THREADS|BR|BV
    3. `CP_THREAD_CONTROL(0x00000003)` = BR|BV（Turnip 的形式）
    4. Turnip 前 11 word（thread control + 4 events + wait idle）
    5. `SP_UPDATE_CNTL` + wait idle
    6. 前 28 word
    看哪一种形式能让 CP 继续执行。
  - 产物 metal22：kernel `6275ca00…` 17,054,336 B；payload
    `8dc1d28a756a252f8ecf7c9642e0abbd5eab8a18df0dddae49029445d21004c6`
    25,458,256 B；boot `shim/out/shim-boot-metal22-tcvariants.img`
    `1fb4599c407f776fedda10e17120f8a9f8e0bee32e880351482a69b2e008d67a`
    32,034,816 B。
  - 上机：只 boot metal22，等 5 分钟拉日志，看哪条 `cp test` 首次
    `result=0`（完成）；之后据此决定是改 Turnip 的 thread-control
    形式，还是在驱动里预置 CP 状态。

- **2026-10-29 GPU 第 265 步：metal20 dump 出整条 932 B Turnip IB（a7xx 命令流，末尾是 CP_BLIT/A2D clear），RBBM_STATUS3 bit24=0 → 不是 SMMU fault；CP busy 但所有 block idle → 在某个命令上停住；metal21 在 CP 启动后直接重放 Turnip 的包来二分定位**
  - `1453.txt`（618,754 B，`7638ed1a…`）：
    - `rbbm 0x800005`：只有 CP_AHB/CP_BUSY/GPU_BUSY_IGN_AHB，
      VPC/UCHE/SP/TPL1/VSC/HLSQ 全 0；
    - `rbbm3 0x0`：bit24 `SMMU_STALLED_ON_FAULT` = 0 → 不是 IB 取指
      SMMU fault，CP 是停在命令流里；
    - 整条 IB（233 dword）解码：
      - 0..27：THREAD_CONTROL(3)、events 0x19/0x18/0x28/0x33、
        CP_WAIT_FOR_IDLE、SP_UPDATE_CNTL(0xAB1F=0x01fffeff)、再
        CP_WAIT_FOR_IDLE、RB_CCU_CNTL/UCHE_CACHE_WAYS/TPL1_DBG_ECO_CNTL/
        SP_CHICKEN_BITS…
      - 169..232：CP_SET_DRAW_STATE(0x43)、VSC_BINNING_START(0x1b)、
        THREAD_CONTROL 切换、CP_SET_AMBLE(0x55) 四个（地址
        0x20028000/0x2002826c…）、CP_REG_TEST(0x39)、
        RB_A2D_* 寄存器 + CP_BLIT(0x2c)（A2D clear，清色 c0ffee42，
        目标 0x2002c000）、CP_EVENT_WRITE 0x21/0x20 结束；
    - 这些看起来都是合法 a7xx 命令，不是坏数据；因此用内核侧重放做二分。
  - metal21：在 CP 自检之后、publish 之前，按顺序做：
    1. 直接 MMIO 读 `0x050f`（RBBM_PERFCTR_SRAM_INIT_STATUS），若 bit0=0
       则写 `0x050e=1` 并轮询最多 200 ms，打印 before/after；
    2. ring 重放 `CP_THREAD_CONTROL(3)` + fence；
    3. 重放 Turnip 前 11 word（thread control + 4 events +
       CP_WAIT_FOR_IDLE）；
    4. 重放 `SP_UPDATE_CNTL=0x01fffeff` + `CP_WAIT_FOR_IDLE`；
    5. 重放前 28 word（preamble28）。
    每步 500 ms 超时，日志
    `NativeAdreno: cp test <名字> submit=… fence=… result=… observed=…`。
    哪一步开始超时，就是 CP 停住的包。
  - 产物 metal21：kernel `7ebb1baf…` 17,054,336 B；payload
    `9b4d9116…` 25,458,256 B；boot `shim/out/shim-boot-metal21-cptest.img`
    `af07fddd7584cbc7dbc82d882071431e3b6376de6146185665ecc46ae86b9c00`
    32,034,816 B。
  - 上机：只 boot metal21，等 5–6 分钟拉日志，重点看
    `perfctr SRAM status` 和 5 条 `cp test`（哪条 result=1/timeout）。
  - 风险：如果第 2 步就卡住，CP 会停，后面的 publish/Turnip 都不会工作，
    但系统仍能进 UI（软件渲染）；日志足够定位。

- **2026-10-29 GPU 第 264 步：Turnip 的 932 B IB 开头解码完毕——是 a7xx 的 command-buffer preamble（THREAD_CONTROL/CCU invalidate/SP_UPDATE_CNTL 等），CP 忙但所有 GPU block 都 idle，说明卡在某个 wait/嵌套 IB；metal20 dump 整条 260 dword 的 IB 并读 RBBM_STATUS3 的 SMMU_STALLED_ON_FAULT**
  - 重要更正：Mesa `freedreno_devices.py` 里 **FD730（chip_id 0x07030001）= CHIP.A7XX**
    （`a7xx_base`+`a7xx_gen1`）——Adreno 730 是 **a7xx**，不是 a6xx。
    所以 sheng 的 A7XX framing 家族是对的；之前 plain submit 只是绕开了
    raw test 的 prologue 卡点，并非“GPU 是 a6xx”的证据。
  - `1438.txt`（658,042 B，`2d6df396…`）里 metal19 dump 的前 28 dword：
    - `70970001 00000003` = CP_THREAD_CONTROL(BR|BV)
    - 4×`70460001` events 0x19/0x18/0x28/0x33 = CCU_INVALIDATE_COLOR/
      DEPTH、CACHE_CLEAN、CACHE_INVALIDATE7
    - `70268000` = CP_WAIT_FOR_IDLE
    - `48ab1f01 01fffeff` = SP_UPDATE_CNTL（A7XX，更新全部 stage）
    - `70268000` 再等 idle
    - `408e0701 68` = RB_CCU_CNTL、`400e1701` = UCHE_CACHE_WAYS、
      `40b60001`/`48b60201` = TPL1_DBG_ECO_CNTL(1)、
      `40ae0301`/`48ae0801`/`40ae0901` = SP_CHICKEN_BITS(1/2)
    → Turnip 在跑 a7xx 的 device init preamble，不是错误数据。
  - 超时行：`rbbm 0x800005` = RBBM_STATUS 只有 bit0/2/23
    （CP_AHB busy、CP_BUSY、GPU_BUSY_IGN_AHB）；**bit16..22 的
    VPC/UCHE/SP/TPL1/VSC/HLSQ 全是 0** → 所有 block 都 idle，
    CP 却 busy。这更像 CP 在某个 CP_WAIT_* 上轮询，或嵌套装 IB 取指
    没返回，而不是 GPU 还在跑长命令。
  - metal20 诊断（同 metal19 + 更多）：
    - `native_adreno_ib_log` 扩到 4×264 word，超时时 dump 整条 260 dword；
    - 超时行增加 `rbbm3 0x%x`（RBBM_STATUS3 bit24 =
      `SMMU_STALLED_ON_FAULT`），可直接判断是不是 IB 取指 SMMU fault。
  - 产物 metal20：kernel `131000a8…` 17,054,336 B；payload
    `0359924e…` 25,457,840 B；boot `shim/out/shim-boot-metal20-ibfull.img`
    `368a7faa3e1df8343041159f4dceba53874c8d93aef123e11f4c56fe3dec2e50`
    32,034,816 B。
  - 上机：只 boot metal20，等 5–6 分钟拉日志；看 `timed-out IB` 后的
    全部 `ib` 行和 `rbbm3` 值。

- **2026-10-29 GPU 第 263 步：metal18 确认卡住的是 Turnip 的第一次真实提交——raw test 的 48 B IB 全部正常（backboardd/SpringBoard 两轮 v4/4），Turnip 的 932 B IB（backboardd 0x20030000 fence 8、SpringBoard 0x60030000 fence 9）让 CP 停在 IB 执行里；metal19 在 fence 超时时 dump 这条 IB 的前 28 个 dword**
  - `1426.txt`（667,555 B，`a7e70df2…`）：
    - `submit backboardd buffers 1 ib 0x20000000/48 fence 4/5` →
      `backboard s34 e0 v4/4`；
    - 然后 backboardd Turnip markers `0x6000..0x6004`；
    - `submit backboardd buffers 1 ib 0x20030000/932 fence 8` →
      `fence timeout 8 … cp rptr 85 wptr 92 fence 81/7`；
      CP 只吃了 4 word 的 IB 包，932 B 的 IB 没执行完；
    - SpringBoard raw `ib 0x60000000/48 fence 6/7` → `s34 e0 v4/4`；
      Turnip markers 0x6000..0x6008；`submit SpringBoard ib
      0x60030000/932 fence 9` 同样不返回；
    - chronod `ib 0x6aaa9000/48 fence 10` 后 `s11 ee00002d6`
      （ring 已被 fence 8 堵住）；Preferences fence 11；
    - `W 6efe s1 u…` 是 Turnip watchdog 的采样行。
  - 结论：**raw 用户提交路径完全正常**；卡点在 Turnip 生成的第一条
    Vulkan 命令流（932 B IB）。可能是 IB 里某个 a6xx/a7xx 命令、嵌套
    IB 的 iova 映射、或 CP 对该命令流的 trap。
  - metal19：`NativeAdrenoRing::logTimeout` 在超时时按 fence 找到
    `native_adreno_ib_log` 的对应项，打
    `NativeAdreno: timed-out IB fence F iova 0x… bytes N` +
    前 28 个 dword（每行 7 个），用于离线解码 opcode。
  - 产物 metal19：kernel `00bfc985…` 17,054,336 B；payload
    `bfadf375dd52bf62a46d41afccfd2cf85f5e55958c8709c256542b5655943838`
    25,457,840 B；boot `shim/out/shim-boot-metal19-ibdump.img`
    `42110b6aaddc99b0cc0371322c027ffa647c074d273752a34857130f5d7c6f02`
    32,034,816 B。
  - 上机：只 boot metal19，等 5–6 分钟拉日志，找
    `timed-out IB fence 8` 后面的 `NativeAdreno: ib …` 行。

- **2026-10-29 GPU 第 262 步：metal17 原始 GPU test 两轮全部通过（`s34 e0 v4/4`，backboardd + SpringBoard）；之后一个后续提交（fence 8，疑似 Turnip 阶段）把 CP 卡在 IB 取指，chronod 因此超时；metal18 加 submit 进程/参数和 Turnip marker 日志**
  - `1416.txt`（600,809 B，`2ca92250…`）：
    - `NativeAdreno: timestamp timer attached (CP always-on counter)`；
    - `NativeAdrenoMetal: backboard s34 e0 v4/4 t6c37320 a256/0`，
      SpringBoard 同样 `s34 e0 v4/4`；`s34` = 两轮 17 步全过，
      `v4/4` = 0x101 和 0x401（write-combine）两轮都校验 4 个 dword；
    - 随后（SpringBoard 报告之后、没有新的 `metal client open`）
      `fence timeout 8 seq 8 next 92: cp rptr 85 wptr 92 rb_base 0x200000
      rb_cntl 0x20c ib1 0x0/3736059565 rbbm 0x800005 fault 0x0
      fence 81/7`：
      提交从 ring 81 开始（4 word IB 包），CP 只走到 85 就停在 IB 执行里，
      没取后面的 7 word fence；
    - 之后 chronod 打开连接、test 报 `s11 ee00002d6 v-1/-2`（ring 已被
      fence 8 堵住）。
  - 判读（**未证实**）：fence 8 很可能是 raw test 之后的 **Turnip 阶段**
    （`turnipPhase()` 在同一个连接上提交 Vulkan 工作）卡住；也可能是
    SpringBoard 的第二次 Metal 设备。需要日志区分。
  - metal18 诊断：
    - `NativeAdrenoSubmit::submit()` 打
      `NativeAdreno: submit <proc> buffers N ib 0x…/bytes fence F status …`；
    - 插件 marker 0x6000..0x6fff（Turnip 阶段进度）打
      `NativeAdrenoMetal: turnip marker 0x…`，0x6fff/0x6ffe 时把
      `report2` 的 `T …` 行打出来；0x7fff 开新行时 flush 上一行文本。
  - 产物 metal18：kernel `3c140023…` 17,054,336 B；payload
    `fb4e2c101d19a9f9b3340238980d46f1844a2eab5c4603b93ef4c5f7cbba916f`
    25,457,840 B；boot `shim/out/shim-boot-metal18-turnipdiag.img`
    `3e3d31d40291543c1a21a592e73e3bd691c87d0f4bc6cfe38d55f5deb3067caa`
    32,034,816 B。
  - 上机：只 boot metal18，等 5–6 分钟拉日志，找 fence 8 前后的
    `NativeAdreno: submit …`、`turnip marker`、`T …` 行。

- **2026-10-29 GPU 第 261 步：plain submit 成功——GPU 真的执行了用户 IB 并校验出 4 个 dword（`v4`）；唯一错误是 GetTimestamp 返回 Unsupported（没挂 timer）→ metal17 挂 CP always-on counter timer**
  - `1409.txt`（634,340 B，`52a3614c…`）：
    - **没有** `fence timeout`；
    - `NativeAdrenoMetal: backboard s16 ee00002c7 v4/-2 t0 a256/0`，
      SpringBoard/chronod/Preferences 同样；
    - `v4` = 测试的 3 个 `CP_MEM_WRITE` 值（0x0adc0de1/0xfeed0002/
      0x600d0003）+ 1 个未覆盖哨兵（0xdeadbeef）全部校验通过 →
      **Adreno 730 上用户提交（CP_INDIRECT_BUFFER + fence）真的工作了**；
    - `e00002c7` 来自 GetTimestamp：test 的失败 marker 是
      `0x414742c7`（0x4000|0x2c7），不是 submit/wait；
    - 因为 service 调 `native_adreno_publish(..., timer=nullptr, ...)`，
      `NativeAdrenoUserClient::timestamp()` 返回 Unsupported；test 把
      error 记下并跳过第二次 0x401（write-combine）run（故 vWc=-2）。
  - metal17：publish 前用
    `static NativeAdrenoTimer published_timer` +
    `NativeAdrenoTimeIo {&g_ring_window, gpu_read32}`，
    `attach(..., NativeAdrenoTimeCp)`（CP_ALWAYS_ON_COUNTER dword 0x980/0x981），
    成功才传给 publish；日志会打
    `NativeAdreno: timestamp timer attached (CP always-on counter)`。
  - 产物 metal17：kernel
    `97f4f7ebeaadcff47d36c3cf456c08fd06e6cbc95c320d84ba32aa1b9bd6bb12`
    （17,054,336 B）；payload
    `35da5dbcb08c6a0fd100555e6e9a3adb41d34509bcfc9c24c7c40307e8bf82c8`
    （25,457,840 B）；boot `shim/out/shim-boot-metal17-timer.img`
    `786ca7d2ed36062edf96c45859340201b7e30e14ce8cf7f1fe29371125ed0c9c`
    （32,034,816 B）。
  - 上机：只 boot metal17。预期 `NativeAdrenoMetal: … e0 v4/4`；
    若第二次 0x401 run 因 write-combine 映射失败，会看到 `v4/<…>`
    和对应错误码（那是下一个问题）。
  - 之后：加 `NativeAdrenoMetal.publish`（+ `.clients`）走真正 GPU 合成。

- **2026-10-29 GPU 第 260 步：metal15 定位 fence 超时——CP 只吃了 framed prologue 的 ~13 个 word 就停住；Adreno 730 是 a6xx，不该用 sheng A740 的 A7XX BR/BV framing；metal16 把用户提交改成 plain submit（`liuqin-gpu-framed=1` 可恢复）**
  - `1358.txt`（557,187 B，`578b3d00…`）里的诊断：
    `fence timeout 4 seq 4 next 109: cp rptr 50 wptr 109 rb_base 0x200000
    rb_cntl 0x20c ib1 0x0/3736059565 rbbm 0xe00005 fault 0x0 fence 48/3`
    - CP 确实动了（rptr 0→50，fence[0]=48），但停在 prologue 内部；
      reset prologue 从 37 开始（11 常规 + 26 reset），rptr 50 只走了 13 个
      word，**还没到 IB（CP_INDIRECT_BUFFER）**；
    - rb_base/rb_cntl 正常、fault 0，说明不是掉电或寄存器丢失；
    - fence[1]=3 仍是 boot 自检的最后 fence，插件提交的 fence 4 从未写回。
  - 结论：`submitFramed` 的 A7XX（Adreno 740）BR/BV prologue 在 A730（a6xx）
    上把 CP 停住了；boot 自检用的 `submit()`（plain）在同一 GPU 上一直是通的。
  - metal16：`NativeAdrenoSubmit::submit()` 默认走 `ring_->submit()`（plain：
    IB 包 + 7 word fence），不再发 prologue；`liuqin-gpu-framed=1`
    恢复 A7XX framing 便于对照。
  - 产物 metal16（kernel `906828e9…` 17,053,888 B；payload
    `c9c9d5312b1ffece74045fa19a609e9fc438e4326717b92632ab948979a71b44`
    25,457,392 B；boot `shim/out/shim-boot-metal16-plain.img`
    `0511ead2d163dad9f0ac72e883d39eeb93ad04a4425d6eae80299ab7718fdd3e`
    32,034,816 B）。
  - 上机：只 boot metal16，等 5–6 分钟拉日志。预期 test 结果从
    `s11 ee00002d6` 变成 `s<step> e0 v4/4`（4 个 dword 全部校验）；
    若仍超时，看新的 `fence timeout` 行（现在可能停在 IB 或 fence）。

- **2026-10-29 GPU 第 259 步：metal14 不再 panic，插件 test 在 backboardd/SpringBoard/chronod/splashboardd 都跑到了 submit+WaitFence，但 fence 超时（`s11 ee00002d6 v-1/-2`）；metal15 加 fence 超时诊断（CP rptr/wptr、RB base/cntl、IB1、RBBM、fault、fence 字）**
  - `1344.txt`（755,803 B，`4a5a3578…`）观察：
    - `metal probe: hdr cffaedfe…`、`open 0x4147 rc 0`、`open type 1 rc 0`；
    - `original accelerator open type 1 proc backboardd (discovery)` ✓；
    - 4 个进程各一条
      `NativeAdrenoMetal: <proc> s11 ee00002d6 v-1/-2 t0 a256/0`；
    - 无 panic；触摸/按键都 ready；UI 帧 47 次。
  - 判读：test 的步骤计数 11 = Query/GetDeviceInfo/2×BoAlloc/2×BoMap/
    2×BoCpuMap/2×IOConnectMapMemory64/Submit 全部成功；**第 12 步
    WaitFence 返回 `0xe00002d6` = `kIOReturnTimeout`**，即 submit 写进 ring
    但 CP 没写回 fence。`a256/0` 是 admitted selector 0x100。
  - 说明插件本身、type-1 discovery、驱动 open、BO map、submit 路径都通了；
    剩下的是 **GPU/CP 实际执行**。
  - metal15：`NativeAdrenoRing::logTimeout()` 在 fence 超时时（最多 4 次）
    打一行：
    `NativeAdreno: fence timeout <f> seq <n> next <n>: cp rptr … wptr …
    rb_base … rb_cntl … ib1 …/<size> rbbm … fault … fence …/…`
    - rptr 追上 wptr → CP 取了 ring，卡在 IB 取指/执行（IB 映射/包问题）；
    - rptr 不动 → CP 没在跑（GX 掉电 / CP 停）。
  - 产物 metal15（kernel `35d03d27…` 17,037,408 B；payload
    `0de52de2dfa8d41026e5c89a32ac4f7daf69dce91522586825c5a98e38c5d92c`
    25,440,912 B，`LIUQINFW` ✓；boot
    `shim/out/shim-boot-metal15-fencediag.img`
    `6c4ea8662d327320cd5e6f755a6eed009298ed9e633ae2cc887ca8344289940a`
    32,018,432 B）。
  - 上机：只 boot metal15（root 不动），等 5–6 分钟拉日志，找
    `NativeAdreno: fence timeout`。
  - 未证实：超时是 GX 掉电（boot 后 ~130 s 空闲）还是用户 IB 取指 fault；
    下一步按 rptr/IB1 判断。

- **2026-10-29 GPU 第 258 步：type-1 修复生效——Metal 真的加载插件并跑了 test（marker 0x41470013/23/3f），随后内核 panic 在 `gpu_read32`；原因是 ring 的 `gpu_window` cookie 是栈上局部变量，attach 后悬空；改成 static 常驻窗口，出 metal14**
  - 现象（用户照片，metal13）：backboardd 连续
    `open AGXAccelerator type 0x41470013/0x41470023/0x4147003f -> 0xe00002c7`
    （插件 test 的进度 marker）→ 说明 **iOS 16 的 type-1 discovery 修好了**，
    Metal 已 `NSBundle` 加载插件并进入 `NativeAdrenoUserTest`；
    随后 `Kernel data abort`，`panic(cpu 4 caller 0xfffffe0007b0a934)`。
  - atos（`kernel.development.liuqin.unstripped`）：
    - pc `0xfffffe0007ab51dc` → `NativeAdrenoGpuService::gpu_read32`
      (`NativeAdrenoService.cpp:208`)；
    - lr `0xfffffe0007ab3fa0` → `NativeAdrenoRing::submitWords`
      (`NativeAdrenoRing.cpp:219`)。
  - 根因：`NativeAdrenoService.cpp` 里 `NativeAdrenoGpuWindow gpu_window{...}`
    是初始化块内的**局部变量**；`ring_.attach(&ring_io,...)` 把
    `NativeAdrenoRingIo` **按值复制**，其中 `cookie=&gpu_window`。
    初始化函数返回后 cookie 悬空，插件 test 在 127 s 后 submit 时
    `readRptr` → `gpu_read32(dangling)` 数据中止。
    （bringup 期间的 submit 还在同一作用域内，所以没炸；ring cache 的
    cookie `&gpu_ctx_cache_` 本来就是成员，没问题。）
  - 修复：文件作用域 `static NativeAdrenoGpuWindow g_ring_window{nullptr,0};`，
    初始化块里改成引用 `NativeAdrenoGpuWindow &gpu_window=g_ring_window;`
    再赋 base/bytes。窗口指向的 `maps_[0]` / `alt_gpu_map_` 都是 service 成员，
    生命周期一致。
  - 产物 metal14（`WORK=/Users/qprs/liuqin-build3 LLD=/tmp/ld.lld
    tools/build-local.sh`）：
    kernel `664f5c8522d3e7d70b5cd581ad8e50382f88f0a52bf06795feb1410451943230`
    （17,037,152 B）；
    payload `0e3a334a0cf3bb0921e76a2d36cc3bc3c7aa0efe3223a32986f16ff5c68c77d3`
    （25,440,800 B，`LIUQINFW` ✓）；
    boot `shim/out/shim-boot-metal14-ringwin.img`
    `be3506e70b9f6fb8276af35e9e4bdfd52165a03870e3cc7b3b88acf19c1cf059`
    （32,018,432 B）。
  - root 侧（本轮）：fastboot 只把镜像写到 ~13.4 GB（backboardd 6.3 GB 处为 0）；
    改用 Android `adb` + `dd` 写 `/dev/block/sda36`（16 GiB，40 MB/s），
    写完 `dd` 校验 backboardd/插件首块均为 `cffaedfe`，分区 start
    `426745816`（512B 扇区）与内核 hfs `0x32df3fb` 一致。
  - 上机：**只 boot metal14**，root 不动。等 5–6 分钟拉日志，预期：
    `metal probe: hdr cffaedfe…`；
    `original accelerator open type 1 proc backboardd (discovery)`；
    `metal client open type 0x4147 proc backboardd`；
    `NativeAdrenoMetal: backboardd s<step> e<err> v<ok>/<wc> …`（test 结果行）；
    之后 Turnip 阶段 `NativeAdrenoMetal: T …`。
  - 若又 panic：用 atos 对 pc/lr 继续定位；若 test 卡住不再是内核 panic，
    看 `NativeAdrenoMetal:` 最后一行。

- **2026-10-29 GPU 第 257 步：metal11 日志暴露两个独立问题——`fwarea.bin` 被错传成 `--ramdisk`（ADT 记成 `RAMDisk`，内核找 `LIUQINFW` 找不到 → GPU 服务未发布、触摸固件缺失）；设备 root 里插件数据块仍是 0（probe hdr 全 0、dlopen not a mach-o）→ 出 metal12（`--fwarea`）并需要重新完整刷 root**
  - `1236.txt`（513,460 B，`adb71a13…`）观察：
    - `LIUQIN FW: no LIUQINFW firmware area in the memory map`；
    - `NativeAdreno: no GMU firmware in the firmware area or the kernel`、
      `GMU boot needs firmware and bound windows` → 没有 `IOAcceleratorES`
      （backboardd 多次 `matching IOAcceleratorES -> 0 found`），所以也
      没有 type-1 open；
    - `LIUQIN TOUCH: touch firmware missing from the firmware area` → 触摸消失；
      按键走 SPMI 不依赖固件，仍正常（与用户描述一致）；
    - `|backboardd| metal probe: size 22090992 mode 0100755`、
      `hdr 00000000000000000000000000000000`、
      `plugin dlopen failed: … (not a mach-o file)`，拷到 `/var/tmp` 后同样
      not a mach-o → **设备 root 里插件 22 MB 数据块是 0**，不是 pattern
      错误。
  - 原因：上一次 payload 用了 `--ramdisk /Users/qprs/liuqin-root/fwarea.bin`，
    mkpayload 在 ADT 里写的是 `RAMDisk`；内核要的是 `LIUQINFW`
    （`mkpayload.py --fwarea`）。已用 `--fwarea` 重出：
    payload `d2f438b45ead3e582873a321dd9d35f30dafb43eedcba861d157699e4f5a610d`
    （25,440,656 B，ADT 里 `LIUQINFW` ✓，最后一段与 `fwarea.bin`
    `bdcb253b…` 相同）；
    boot **metal12** `shim/out/shim-boot-metal12-fwarea.img`
    `1fba6e146241e86f9e4b5e4f002381b2b3e1e65d47a3a697dbaacc052c297b9a`
    （32,018,432 B）。
  - 上机：先 **完整重刷** `~/liuqin-root/ipad71-root-wrap16-gpu.img`
    （plain `fastboot flash userdata`，不要 `-S`，确认输出 OKAY/没有
    sparse 错误），再 boot metal12。预期：GPU/触摸回来；
    `|backboardd| metal probe: hdr cffaedfe…`、`open 0x4147 rc 0`；
    然后 `NativeAdreno: original accelerator open type 1 proc backboardd
    (discovery)`，若 Metal 随后加载插件则出现
    `metal client open type 0x4147` / `NativeAdrenoMetal:`。
  - 若 probe 仍是 hdr 全 0：说明刷写没有真正写进插件数据块（和之前
    sparse/partial 一样），下一步要从 Android 侧按 raw offset 直接核对/写入，
    或换一种刷写方式；**不要再假设“刷了整包”就一定写全了**。
  - 未证实：type-1 成功是否足够触发 Metal 的 `NSBundle` 加载；等 metal12
    日志再判断。

- **2026-10-29 GPU 第 256 步：iOS 16.6.1 的 Metal 发现先 `open type 1` 才加载插件——驱动现在只给 backboardd 一个 type-1 discovery 连接（不挂 core）；注入库 probe 改为只读文件 + 直接 open 0x4147/1，不跑 test；出 metal11（只 boot）**
  - 依据：`liuqin-log-20261029-121547.txt`（868,140 B，`c99f6133…`）里
    backboardd 在 ~128 s 读 `MetalPluginName`/`MetalPluginClassName` 后
    `open AGXAccelerator type 1 -> 0xe00002c7`，之后再无插件相关日志；
    全日志 `NativeAdrenoMetal` 出现 **0 次**。推断（**未证实**）：iOS 16 的
    Metal 发现要 type 1 成功拿到 port 才会 `initWithAcceleratorPort:`；
    sheng 的 iOS 17 不需要（所以原版驱动一直拒绝 type 1 也能跑）。
  - 但同一日志里 `/usr/lib/libliuqintouch.dylib present, 53760 bytes,
    magic 00000000 00000000`，而 wrap16 镜像里该文件是 `cffaedfe`——
    设备上这个文件的数据块可能仍是旧的/没写全（第 248 步的 sparse 残留）；
    触摸不受影响（内核把 54,656/53,792 B 的嵌入库写到
    `/var/tmp/liuqin-touch-*.dylib`）。所以这次 probe 也顺便回答
    “设备 root 里到底有没有有效插件”。
  - 内核（`xnu/iokit/NativeAdreno/NativeAdrenoPublish.cpp`）：
    - `AGXDeviceUserClient` 加 `discoveryOnly_`/`discoveryCalls_`；
    - `native_adreno_new_user_client`：`type==1` 且进程名是 `backboardd` 时
      返回一个**不 mount core** 的 client（`setDiscoveryOnly`），其它进程仍拒绝；
    - 该 client 的 `externalMethod` 前 8 个 selector 打
      `NativeAdreno: original client selector 0x… -> unsupported` 后拒绝；
      其它 foreign open 打 `NativeAdreno: foreign open type 0x… proc …`；
    - 成功日志：`NativeAdreno: original accelerator open type 1 proc backboardd
      (discovery)`。
  - 注入库（`host/liuqin_touch_inject.c` 的 `metal_probe_thread`）：保留
    lstat/read 插件头 + dlopen + `objc_getClass`，**不再**调
    `initWithAcceleratorPort:`（避免和 Metal 自己的 test 并发），改为直接
    `IOServiceOpen(service, 0x4147)` 和 `IOServiceOpen(service, 1)` 各一次并打
    rc/conn。dylib 54,656 B `e0a1947cec…`（`tools/build-inject.sh`）。
  - 构建产物（`WORK=/Users/qprs/liuqin-build3 LLD=/tmp/ld.lld
    tools/build-local.sh`，`/tmp/ld.lld` 需重建为
    `exec …/ziglang/zig ld.lld "$@"`）：
    kernel `83ebc6fc1384d8a63685a69ecb29c51474fa27a765872bb0018ff6621e293663`
    （17,037,152 B）；
    payload `35a20a171d5015ab62707103ac80df3787c640619e4b3ccc46214861192909fe`
    （25,440,656 B，含 `~/liuqin-root/fwarea.bin` 8 MiB ramdisk——**漏掉它
    会没有 LIUQINFW**）；
    boot **metal11** `shim/out/shim-boot-metal11-origopen.img`
    `8f506f58e0315d59b0a94eb401658fea64f05ade4ea51d804738c5441ff5d083`
    （32,018,432 B）。
  - 上机：**只 boot metal11**，root 不动。等 4–5 分钟拉日志，按顺序找：
    1. `NativeAdreno: original accelerator open type 1 proc backboardd
       (discovery)` → type 1 已放行；
    2. `NativeAdreno: metal client open type 0x4147 proc backboardd` +
       `NativeAdrenoMetal: …` → Metal 真的加载了插件并跑了 test；
    3. `|backboardd| metal probe: size 22090992 … hdr cffaedfe…`、
       `metal probe: open 0x4147 rc 0`、`metal probe: open type 1 rc 0`
       → 设备 root 里插件有效、两条驱动路径都通；
    4. 若 probe 报 `metal probe: open failed 2` → 设备 root 里没有插件
       （镜像没刷进去/刷错分区），需要重刷
       `~/liuqin-root/ipad71-root-wrap16-gpu.img`。
  - 若第 1 条有、第 2 条没有：看 `foreign open type …` 和
    `original client selector 0x…`，判断 Metal 在加载插件前还要 type-1
    连接上的哪个方法；这是下一步（可能需要实现 IOGPUDeviceUserClient 的
    少量 selector，或绕过：在 backboardd 的注入库里直接调用插件）。

- **2026-10-29 GPU 第 255 步：把 `NativeAdrenoMetal.bundle`（test 模式）加进 wrap16 原镜像，出全量 root；Data 保持 wrap16 seeded 状态；boot metal7 验证插件是否 `open type 0x4147`**
  - 基准：用户刚刷的 `~/Downloads/ipad71-root-wrap16.img.zip`（`b8f079a9…`）；
    解压出的 `ipad71-root-wrap16.img` sha256 `6ea2b959…`（与历史 wrap16 一致）。
  - 做法：`cp -c` 克隆；`hdiutil attach -readwrite`（noowners）后把
    `~/liuqin-root/refs/UniXNU/build/adreno-metal-plugin/NativeAdrenoMetal.bundle`
    rsync 到 `/System/Library/Extensions/NativeAdrenoMetal.bundle`；**不放**
    `NativeAdrenoMetal.publish`（test 模式）。文件 hash 与 build 一致：
    `NativeAdrenoMetal` `21b7d540…`、`libvulkan_freedreno.dylib` `1833ed2c…`；
    host 侧 `codesign --verify --deep --strict` 通过。
  - 校验：`fsck_hfs -fn` → `The volume iOSRoot appears to be OK`；
    OLD/NEW 对比 `launchd.plist` + `.sig`、`SpringBoard`、`backboardd`、
    `com.apple.springboard.plist` 全部相同；`/System/Library/Extensions` 只多出
    `NativeAdrenoMetal.bundle`。
  - 产物：**`~/liuqin-root/ipad71-root-wrap16-gpu.img`**（16 GiB）
    sha256 `4b4873368c21347c8fc8f0f457f7c5878eb2cbcf898b4801a85a27224f888106`；
    原始 wrap16 留在 `~/liuqin-root/wrap16-gpu/ipad71-root-wrap16.img`。
  - 为什么用全量而不是 sparse：新增文件必然改 HFS+ 元数据（catalog/bitmap/
    journal），设备启动后 Data 元数据已与 wrap16 镜像分叉，sparse 混写元数据
    有 catalog/bitmap 不一致风险（第 248 步的教训）；全量镜像自洽，且 Data
    就是 wrap16 的 seeded 状态，刷完仍应直接进桌面。
  - bundle 文件 owner 是 `501:20`（主机无 sudo，noowners 挂载导致），不是
    root:wheel；但注入的 `libliuqintouch.dylib` 本身 unsigned 都能加载，
    owner 不影响 dyld/AMFI，暂不处理。
  - 上机：`fastboot flash userdata ~/liuqin-root/ipad71-root-wrap16-gpu.img`，
    boot **metal7**（`shim/out/shim-boot-metal7-grantui.img`，`d69a2d4d…`，
    grant 名单含 backboardd/SpringBoard/Setup/chronod + property log）。
  - 日志预期（5–6 分钟后）：
    `NativeSandboxGrant: AGXAccelerator grants backboardd`（或 SpringBoard）、
    `NativeAdreno: metal client open type 0x4147 proc …`、
    `NativeAdrenoMetal: …`（test 结果行）；没有 open 就查
    `MetalPluginName read by …` 和 `open AGXAccelerator type …` 记录。
  - 若测试把 UI 卡住：重刷 `~/Downloads/ipad71-root-wrap16.img.zip` 回桌面。
  - 下一步：test 通过后加 `NativeAdrenoMetal.publish`（backboardd 走原生
    Metal，CARenderServer 用 GPU 合成），需要时再加 `.clients`。

- **2026-10-28 GPU 第 251 步：用户刷的是 inject 整包，但 `2330.txt` 里注入完全没跑——launchd 读的是单文件 `/System/Library/xpc/launchd.plist`，不是 individual plist；已改单文件并出 inject2**
  - `2330.txt`（`5024f4b6…`）：`exec SpringBoard` 50.659 s；无 `LIUQIN LAUNCHER`/
    `liuqin setup`；SpringBoard 直接起真身。20 分钟进度条是 LaunchServices 建库。
  - 单文件 `launchd.plist` 里 job key
    `/System/Library/LaunchDaemons/com.apple.SpringBoard.plist` 的 `ProgramArguments`
    已改成 `/usr/libexec/liuqin-springboard`；individual plist 也保持改好。
  - inject2：
    full `~/liuqin-root/ipad71-root-metal-inject2.img`（16 GiB）
    `ecfe8d611890b346556756b12e3291a3b190434f6a24ac916f349f07ff77d115`；
    sparse `~/liuqin-root/ipad71-root-metal-inject2.simg`
    `a553e6b3cef3e290b014f2cb744895e90a62c0c4810fdc56edddd43cb25cac8b`
    （816 KiB，基准 inject1 `6e0b4f63…`，即用户刚刷的整包）；
    zip `...inject2.img.zip`
    `a7a4f9541858a58fd12feb42bce51dbe6fa5b45c5c41b9e599ca626bf74e52a9`。
  - 上机：刷 inject2 sparse/完整，boot metal4；日志应出现
    `|SpringBoard| liuqin setup: ... patched`；Setup 跳过。LaunchServices 建库仍要
    几十分钟，这次别中途重启。详见第 251 步。


- **2026-10-28 GPU 第 252 步：用户照片=内核 panic，`launchd_cache_loader` 在 49.5 s panic “No service cache”；原因是我改了 `/System/Library/xpc/launchd.plist` 但 `.sig` 没更新，launchd 缓存校验失败。改回原 plist，改用“把 launcher 放到 SpringBoard 路径、真身改名 SpringBoard.real”的办法，出 inject3**：
  - 观察（照片）：`launchd_cache_loader` → 读 `/System/Library/xpc/launchd.plist`
    和 `.sig` → 建 launchd cache → panic，随后 `Stackshot Succeeded`。这是
    launchd 单文件 plist 的签名/缓存校验，不是 GPU。
  - 修复：
    1. 从 setupapp 原镜像恢复 `/System/Library/xpc/launchd.plist` 和 `.sig`；
    2. 真 SpringBoard 改名为 `SpringBoard.real`；
    3. 把 `liuqin-springboard` launcher 放到
       `/System/Library/CoreServices/SpringBoard.app/SpringBoard`（launchd job 路径不变），
       launcher 里 exec 的是 `SpringBoard.real`；
    4. individual plist 也指回路径。
    这样完全不改 launchd 的 plist/签名，和 backboardd launcher（`/usr/libexec/backboardd`）
    同一个模式。
  - 注意：launcher 的进程名是 `SpringBoard`，它的 `LIUQIN LAUNCHER` 行会被
    `liuqinnull` 的 `SpringBoard` 捕获；exec 成 `.real` 后 dylib 的
    `|SpringBoard| liuqin setup:` 不会匹配（`SpringBoard.real` 不在名单），要抓
    dylib 的日志需把 `SpringBoard.real` 加进 `liuqinnull`（新 boot 行），
    但 Setup 是否跳过看屏幕即可。
- **2026-10-29 桌面恢复（关键）：wrap16 root + metal4 进桌面；wrap16 是 known-good，已备份**：
  - 用户手里的 `~/Downloads/ipad71-root-wrap16.img.zip`（4.9 GiB，sha256
    `b8f079a9b53130e5ef1ece9498770c8ebffb3a11e57a783bf00a0c6087851884`）刷入，
    boot **metal4**（`c5595544…`）后进桌面。这是目前唯一确认能进桌面的组合。
  - 已备份到 `~/liuqin-root/ipad71-root-wrap16-good.img.zip`（同 sha256）。
  - 结论：wrap16 的 **Data 是 seeded 的**（LaunchServices/容器/激活都在），所以能进桌面；
    clean/inject 的失败不是插件/prefs 单独造成，而是完整镜像把 Data 重置、
    以及改 launchd plist/SpringBoard 可执行文件触发 launchd cache 校验 panic。
  - 以后所有改动（GPU 插件等）都应在 wrap16 上用 **sparse 只改系统文件**，
    保留 Data；不要再用完整镜像重置 Data，也不要改 `launchd.plist`/替换
    SpringBoard 可执行文件。
  - 下一步 GPU：把 `NativeAdrenoMetal.bundle` 以 sparse 方式加到 wrap16 的
    `/System/Library/Extensions`（test 模式，无 `.publish`），boot metal7
    （grant 名单 + `MetalPluginName` 日志），看 `metal client open type 0x4147` /
    `NativeAdrenoMetal:`；通过后再加 `.publish`/`.clients`。

  - inject3 产物：
    sparse `~/liuqin-root/ipad71-root-metal-inject3.simg`
    `5310844eb7f6890ad25ff81601632c23e728ea4fea4f2b599bec3a18a7bba37d`（993 KiB，
    基准 inject2，即用户刚刷的整包）；
    full `~/liuqin-root/ipad71-root-metal-inject3.img`（16 GiB，hash 计算中）；
    zip `~/liuqin-root/ipad71-root-metal-inject3.img.zip`（5.1 GiB）
    `ff8e4e4a4b0d2af07d2a8b3baa5f0c5e96e7a884e2f10eb335563a3f159bb933`。

- **2026-10-28 GPU 第 250 步（上一版）：`2248.txt` 说明不是内核卡死，而是 Setup 在等激活；ios-sweet data ark 需要 Data seed（我们的 Data 是空的），改做 HackiOS 式 SpringBoard 注入，出 inject root**
  - 日志 `2248.txt`（`fece2b92…`）：`mobileactivationd` 111.8 s、`budd`、`SpringBoard`
    104.1 s、`LIUQIN FB` 80,000 swaps、touch 持续；无 panic。40 分钟是 Setup 等激活，
    不是 hang；不会自己完成。
  - 旧正常 root 有跑过 setup 的 Data/容器；我们的完整镜像重置了 `Data/System`，
    `sweet-root-tweaks.py` 的 activation data ark 没有容器可 seed。
  - 做 `libliuqinsetup.dylib`（`SBLockdownManager` 激活状态→2，
    `SBSetupManager _setSetupRequiredReason:`→0，含 image 回调）+ `liuqin-springboard`
    launcher（嵌 dylib、设 `DYLD_INSERT_LIBRARIES`、exec 真正 SpringBoard）；
    SpringBoard 的 launchd job 已改成 launcher；`Setup.app` 保留。
  - 镜像 `~/liuqin-root/ipad71-root-metal-inject.img`（16 GiB）
    `6e0b4f63a369daeba66244616eaec46aab693664aa95945c3f7e0fd4fac7057c`；
    sparse `~/liuqin-root/ipad71-root-metal-inject.simg`
    `5c06949cfe3712e2a83aebee246c65ce5a83ff50a45ed1425353b10ba012e143`
    （253 KiB，基准 setupapp 镜像）。
  - 上机：刷 sparse/完整，boot **metal4**；日志找
    `|SpringBoard| liuqin setup: ... activation patched ... reason patched`；
    然后应跳过 Setup 到锁屏/桌面。详见第 250 步。

- **2026-10-28 GPU 第 249 步（上一版）：用户刷完整 skipsetup root 后“进不去桌面”；把 `Setup.app` 恢复、保留 prefs/auto-dim/plugin，出 setupapp sparse/full；先用 metal4 恢复 UI**
  - 推测：`Setup.app.disabled` 让 SpringBoard 没有可交接的 Setup UI（黑屏/无桌面）；
    metal10 的 probe 在 backboardd 跑插件 test 也可能卡 UI。先排除两者。
  - setupapp `Setup.app` 恢复，prefs/auto-dim/正确 plugin 数据都保留。
  - sparse `~/liuqin-root/ipad71-root-metal-setupapp.simg`
    `c7f97332e01a47b46689498c5b24d8ad37f432a3b63a97661cac794fc8107a3f`
    （296 KiB，基准 `ipad71-root-metal-skipsetup.img`）；完整镜像
    `95a604b58225d66745435f6dd81a1988b4ed06ab862cf9051d7fed64b2ae2a9a`（16 GiB）。
  - 上机：刷 setupapp sparse，boot **metal4**（`c5595544…`，无 probe，UI 进程不在
    grant 名单，插件 test 不会在 Setup/SpringBoard 跑）先看 UI；UI 回来后 GPU 再用
    metal7/metal10。
  - 如果仍黑屏，拉日志看 SpringBoard/backboardd 是否 crash/loop；详见第 249 步。

- **2026-10-28 GPU 第 248 步（上一版）：metal10 找到真正原因——设备上插件文件内容是 HFS catalog 文本（hdr `fari.SafariLinkE`），不是 Mach-O；主机镜像同一块是 `cffaedfe`；root 只写了目录/大小，没写插件 22 MiB 数据块**
  - metal10 日志 `liuqin-log-20261028-210024.txt`（`3c8c2564…`）：
    size 22,090,992、mode 0100755 正常，但 hdr =
    `66 61 72 69 2e 53 61 66 61 72 69 4c 69 6e 6b 45` = `fari.SafariLinkE`，
    原路径和 `/var/tmp` 拷贝都不是 Mach-O。
  - 主机镜像：`F_LOG2PHYS` 插件第一块在 raw offset 13,824,487,424（块 3,375,119），
    raw 读出 `cffaedfe`；文件 sha256 `21b7d540…` 与 build 相同。
  - 结论：设备 root 是 sparse/partial 刷入，只写了 HFS 元数据（路径/大小），
    插件的 22 MiB 数据块仍是旧 catalog 文本；sparse 版 skipsetup 也修不了。
  - 修复：刷 **完整 16 GiB** skipsetup root
    `~/liuqin-root/ipad71-root-metal-skipsetup.img`
    `bf6c24d4abf8183d8db06921968d6d0a72eec25f5c948f000d0f1415ea0d5db5`，
    再 boot metal7（grant 名单）或 metal10（probe）。
  - 这解释了为什么属性读取正常、type 1 正常，但插件永远不 open：设备上的
    插件文件根本不是 Mach-O。
  - 详见第 248 步。

- **2026-10-28 GPU 第 247 步（上一版）：metal8 probe 报硬错误“设备上 dlopen 插件 = not a mach-o file”；主机镜像文件是好的；metal10 加文件头/size/mode + `/var/tmp` 拷贝 probe**
  - metal8 日志 `liuqin-log-20261028-204747.txt`（`2328b8d3…`）：
    `|backboardd| metal probe: plugin dlopen failed: ... (not a mach-o file)`。
  - 主机镜像里的 `NativeAdrenoMetal` size 22,090,992，sha256 与 build bundle 相同
    `21b7d540…`，无 compressed 标志；所以是设备侧 HFS 读取/extent/mmap 或路径问题。
  - metal10 的 probe 先打印 `lstat` size/mode 和前 16 字节，再 copy 到
    `/var/tmp/NativeAdrenoMetal.probe` 重新 `dlopen`（touch dylib 从 `/var/tmp` 能加载）。
  - 镜像 `shim/out/shim-boot-metal10-copyprobe.img`（`54d64d83…`，32,018,432 B，
    payload `a5390d60…`，内核 `8dab7539…` 17,037,152 B）；**只 boot，root 不动**。
  - 上机看 `metal probe: hdr ...`/`copy dlopen ...`：hdr 若是 `cffaedfe` 而原路径
    dlopen 失败、拷贝成功 → HFS 文件/mmap；hdr 不是 Mach-O → 文件在设备上被读坏/截断。
  - 详见第 247 步。

- **2026-10-28 GPU 第 246 步（上一版）：metal7 证明 grant 名单已生效但插件仍未被实例化；在注入 backboardd 的库里直接 dlopen 插件并调用 initWithAcceleratorPort:，出 metal8（只 boot）**
  - metal7 日志 `liuqin-log-20261028-203436.txt`（`9cf8b16a…`）：backboardd
    `grants backboardd`；SpringBoard/chronod/Setup 只读 `MetalPluginName`/
    `MetalPluginClassName`，没有 open，仍无 `metal client open type 0x4147`。
  - 问题不在 grant；是插件未被实例化。bundle 主机侧 codesign/符号都正常。
  - 诊断：`libliuqintouch` 加 `metal_probe_thread`，在 backboardd 里
    `dlopen` 插件、取 `NativeAdrenoMetalDevice`、`IOServiceMatching("IOAcceleratorES")`、
    调 `initWithAcceleratorPort:`；日志走 `|backboardd|`。
  - 镜像 `shim/out/shim-boot-metal8-probe.img`（`b342a017…`，32,018,432 B，
    payload `8dc1c9fa…`，内核 `cc335c4b…` 17,037,152 B）；**只 boot，root 不动**。
  - 上机找 `metal probe:`：
    `plugin dlopen failed: ...` → bundle/AMFI；`initWithAcceleratorPort -> ...` +
    `metal client open type 0x4147` → 插件本身没问题，剩余是 framework/原始 IOGPU
    发现链（下一步 IOGPUFamily/type 1，sheng prelink）。
  - skipsetup root（`bc3786c9…` sparse / `bf6c24d4…` full）仍待刷。详见第 246 步。

- **2026-10-28 GPU 第 245 步（上一版）：20:16 日志是旧 root（用户还没刷 skipsetup）；`MetalPluginName` 已被 backboardd/SpringBoard/chronod/Setup 读到——发现链通，插件 type 0x4147 没 open 是 grant 名单缺 UI 进程；0249 加名单，出 metal7；skipsetup root 禁用 Setup.app**
  - 日志 `liuqin-log-20261028-201620.txt`（`67f63d69…`）：
    `MetalPluginName`/`MetalPluginClassName` read by backboardd/SpringBoard/chronod/Setup；
    仍无 `metal client open type 0x4147`。
  - `Setup` 还在是因为 root 仍是 `ipad71-root-metal-nopublish.img`，不是 sparse 失败。
  - 插件 test 线程在 SpringBoard/Setup/chronod 里 open，但名单只有
    backboardd/iconservicesagent/WidgetRenderer_Default/PosterBoard；0249 扩为
    `backboardd,iconservicesagent,WidgetRenderer_Default,PosterBoard,SpringBoard,Setup,chronod`。
  - 镜像 `shim/out/shim-boot-metal7-grantui.img`（`d69a2d4d…`，32,018,432 B，
    payload `1b437c6c…`，内核 `c43800ac…` 17,037,152 B）。
  - skipsetup root（含 `Setup.app.disabled`）：
    full `bf6c24d4…`（16 GiB）；sparse `bc3786c9…`（808 KiB，基准
    `ipad71-root-metal-nopublish.img`）。
  - 上机：刷 skipsetup sparse + boot metal7；预期 Setup 跳过、SpringBoard 起，
    日志出现 `NativeSandboxGrant: ... grants SpringBoard` 和
    `metal client open type 0x4147`。
  - 详见第 245 步。

- **2026-10-28 GPU 第 244 步（上一版）：做跳过 Setup 的 root（iOS 16 的 `ForceNoBuddy`/`SetupState` + 关自动熄屏）**
  - 原 root 的 `com.apple.purplebuddy` 只有 `SetupDone` 等，缺 iOS 16 的
    `ForceNoBuddy=true` / `SetupState="SetupUsingAssistant"`；`com.apple.springboard`
    也没有 `SBDisableAutoDim` 等键（锁屏后会熄屏）。
  - 已在 clone 上合并这些键（owner 501/20，0600），并保持 plugin bundle。
  - 产物：完整 `~/liuqin-root/ipad71-root-metal-skipsetup.img`（16 GiB）；
    稀疏 `~/liuqin-root/ipad71-root-metal-skipsetup.simg`
    `27fa66ab…`（364 KiB，91 块，基准是刚刷的 `ipad71-root-metal-nopublish.img`）。
  - 刷 sparse + boot metal6；若仍进 Setup，下一步 seed mobileactivationd 的
    data ark 或禁用 Setup.app。详见第 244 步。

- **2026-10-28 GPU 第 243 步（上一版）：metal5 证明 grant 已生效；0xe00002c7 = kIOReturnUnsupported（驱动拒绝原始 type 1），插件仍无 type 0x4147 open；出 metal6 记 MetalPluginName 读取**
  - metal5 日志 `liuqin-log-20261028-195419.txt`：`NativeSandboxGrant: AGXAccelerator
    grants backboardd`，随后 `open AGXAccelerator type 1 -> 0xe00002c7`。
  - 0xc7 是 **`kIOReturnUnsupported`**（不是 NotPermitted）：grant 已放行，type 1
    是驱动对原始 IOGPU 的正常拒绝。
  - 插件仍没有 `metal client open type 0x4147`；问题在 Metal 发现链没走到插件，
    可能因为原始 IOGPU type 1 失败（本 port 的 `IOAcceleratorES` 是 stub，
    没有 sheng 的原始 `IOGPUFamily`）。
  - `0248` 给 `MetalPluginName`/`MetalPluginClassName` 的读取加日志。
  - 当前镜像 `shim/out/shim-boot-metal6-proplog.img`（`b06cfab6…`，32,018,432 B，
    payload `b024aa7d…`，内核 `f6d5ddaf…` 17,037,152 B）；只刷 boot。
  - 上机看 `NativeAdreno: MetalPluginName read by ...`：
    有 → 查 bundle 加载/类实例化；没有 → 下一步接原始 IOGPUFamily 或等效 type 1 用户客户端。
  - 详见第 243 步。

- **2026-10-28 GPU 第 242 步（上一版）：metal4 确认触摸/按键恢复；插件仍未 open（type 1 -> 0xe00002c7），给 sandbox grant 加日志出 metal5**
  - `liuqin-log-20261028-172812.txt`（`ee171bbc…`）：`Liuqin Touch Screen ready`、
    `Liuqin Buttons ready`、`LIUQIN KEYS: power/volume` 都有，触摸/按键恢复；
    HID 90 次重试有效。
  - 插件仍只有 `IOAcceleratorES -> AGXAccelerator` 和
    `open AGXAccelerator type 1 -> 0xe00002c7`，没有 `metal client open type 0x4147`。
  - `0247` 给 `native_sandbox_grant()` 加日志（属性 class/进程名/list/匹配结果）。
  - 当前镜像 `shim/out/shim-boot-metal5-grantlog.img`（`7e301323…`，32,018,432 B，
    payload `49a44db4…`，内核 `cefc87d4…` 17,037,152 B）；只刷 boot，root 不动。
  - 上机看 `NativeSandboxGrant:`：
    `grants backboardd` / `does not list backboardd` / `property is OSData` /
    `has no NativeSandboxGrantProcess`，据此修 grant；之后才可能有
    `metal client open type 0x4147` 和 `NativeAdrenoMetal:`。
  - 246/246 补丁验证中；详见第 242 步。

- **2026-10-28 GPU 第 241 步（上一版）：metal3 日志显示触摸/按键是 HID 晚发布竞态（backboardd 50 s 早于 HID 服务）；给注入库加 90 次重试，出 metal4**
  - `metal1` 的旧 root 下 backboardd 79 s 启动，82.9 s `Liuqin Touch Screen ready`；
    `metal3` 的新 root 下 backboardd 50 s 启动，57 s `create ... failed`，HID 服务还没发布。
    内核触摸/按键事件其实都在（`LIUQIN HID: touch 0 down`、`volume up down`）。
  - 修复在 `host/liuqin_touch_inject.c`：`make_service()` 的 NULL 分支不再 `break`，
    最多 90 次、每次 1 s 重试；重编 dylib `aaef27a0…`（53,792 B），重新生成
    `bsd/dev/liuqin_dylib.h`。
  - 当前镜像 `shim-boot-metal4-hidretry.img`（`c5595544…`，32,018,432 B，
    payload `b232056e…`，内核 `8af2e95d…` 17,037,152 B），启动行 586 B。
  - 上机：**只刷 boot 镜像 metal4**，root 不用再动（已经是 `ipad71-root-metal-nopublish.img`）；
    等 HID 服务发布/重试，触摸/按键应恢复。插件 test 仍可能不跑（sandbox grant 待修）。
  - 下一步：给 `native_sandbox_grant()` 加日志，查 `open AGXAccelerator type 1 ->
    0xe00002c7` 为什么没被 grant 跳过；再等 `metal client open type 0x4147`。
  - 详见第 241 步。

- **2026-10-28 GPU 第 240 步（上一版）：Turnip/bridge/`NativeAdrenoMetal.bundle` 全部按 iOS 16.0 构建；出 metal3 诊断镜像 + root sparse diff（插件 test 模式）**
  - 插件 test 模式：root 里**不放** `NativeAdrenoMetal.publish`，插件跑
    `NativeAdrenoUserTest` 后返回 nil，UI 仍软件渲染；`0246` 把插件结果行
    `NativeAdrenoMetal: ...` 和 `metal client open type 0x4147` 打进内核日志。
  - 当前镜像 `shim/out/shim-boot-metal3-diag.img`（`329fc13f…`，32,018,432 B，
    payload `771d6700…`，内核 `44c81d52…` 17,037,152 B）；启动行 586 B。
  - root sparse diff `~/liuqin-root/ipad71-root-metal-nopublish.simg`
    （`52e4bcee…`，39,126,964 B，基准 `ipad71-root-wrap18.img` `bf7ecd5a…`）；
    完整改好的 16 GiB 镜像 `~/liuqin-root/ipad71-root-metal-nopublish.img`。
  - 245/245 补丁干净应用，`NativeAdrenoPublish.cpp` 与构建树逐字节相同，
    `validate-repo` PASSED。
  - 上机：先刷 root sparse diff（前提是设备 root 就是 wrap18；不确定就刷完整 16 GiB），
    再 boot metal3，等 5–6 分钟，日志存 `metal3.txt`。黑屏可能仍是显示休眠 +
    按键线程 120 s 未开始，不影响日志。
  - 期待：`NativeAdreno: metal client open type 0x4147 proc backboardd`、
    `NativeAdrenoMetal: backboardd s... e... v...`，随后 `T ...`（Turnip）。
    详见第 240 步、`docs/METAL_PORT.md`。

- **2026-10-28 GPU 第 239 步（上一版）：metal1 真机确认 accelerator 发布成功；open 被 sandbox 拒绝；黑屏是显示休眠/按键线程未开始；用户态工具链 bootstrap 中**
  - `metal1.txt`：`metal publish 0x0`、`accelerator published (IOAcceleratorES)`，
    backboardd `IOAcceleratorES -> 1 found`，`AGXAccelerator` 已出现。
  - `open AGXAccelerator type 1 -> 0xe00002c7`（kIOReturnNotPermitted）：原始 IOGPU
    的打开被 sandbox 拒绝；`0245-liuqin-native-adreno-sandbox-grant.patch` 已写好，
    `shim-boot-metal2-sandbox.img`（`d8b470db…`）已构建但未上机（插件还缺）。
  - 黑屏：`LIUQIN FB: display off` 在 101 s，日志到 160 s 仍在跑、无 panic；
    `LIUQIN KEYS: thread up, waiting 120 s` 之后没有 `arbiter version`，说明按键线程
    还没开始轮询，电源键无法唤醒。日常先回 `shim-boot-gpu1-cmddb.img`（`bd44bcf3…`）。
  - 用户态：Rust/Python 3.11/glslang/Mesa 已就绪；Turnip 被 Xcode bison 2.3 挡住，
    正在构建 GNU bison 3.8.2；之后 bridge + `NativeAdrenoMetal.bundle`（需适配 iOS 16.6.1）。
  - 详见第 239 步。

- **2026-10-28 GPU 第 238 步（上一版）：用户选原生 Metal；内核先发布 AGXAccelerator（`sheng-gpu-metal=1`），出 `shim-boot-metal1-accel.img`，等 metal1.txt**
  - 当前镜像 `shim/out/shim-boot-metal1-accel.img`（`08515212…`，32,018,432 B，
    payload `13c0131b…`，内核 `7cb67731…` 17,037,096 B）；启动行 586 B，
    在 gpu1 那行上加 `sheng-gpu-metal=1`。
  - 内核侧（`xnu/patches/0244-liuqin-native-metal-publish.patch`，5 hunk）：
    `LiuqinStubs.cpp` 先于 `NativeAdrenoAccelerator.cpp` 链接；
    `NativeAdrenoService.cpp` 在 CP 起来后调用 `native_adreno_publish()`，
    分配/发布 `AGXAccelerator`，设 `MetalPluginName=NativeAdrenoMetal`、
    `MetalPluginClassName=NativeAdrenoMetalDevice`、`NativeSandboxGrantProcess`；
    metaclass 缺失时回退旧 `NativeAdrenoGpuClient`，不让新路径拖垮旧 GPU 服务。
  - 上机先看：`NativeAdreno: metal publish 0x0` →
    `NativeAdreno: accelerator published (IOAcceleratorES)`；
    backboardd/SpringBoard 的 `IOAcceleratorES` 匹配应从 0 变 1。
    插件 bundle 还不存在，所以 Metal 设备/UI 仍不会出现，这是预期。
  - 下一步（见 `docs/METAL_PORT.md`）：加 `zzw-native-adreno-sandbox-grant.patch`
    让插件能 open；再构建 `NativeAdrenoMetal.bundle` + Turnip/metal2vulkan。
    注意 root 是 **iOS 16.6.1**，sheng 插件按 iOS 17.0 构建，需要适配。
  - 主机侧：243/243 补丁干净应用，应用后配置文件与服务源码与构建树逐字节相同；
    `validate-repo` PASSED。

- **2026-10-28 GPU 第 237 步（上一版）：`gpu1.txt` 确认内核 GPU 全链路通过（ColdStart/CP/self-test/GPU ready）；UI 仍无 Metal，下一步用户态二选一**
  - 修正后的 `shim-boot-gpu1-cmddb.img`（`bd44bcf3…`）已在真机跑通：
    `ColdStart ... ack=0xde`、`OOB GPU_SET acked`、`GPU context status 0x0 step 7`、
    `CP started fence=2`、`CP self-test ... wrote=0x600dc0de`、
    `GPU ready, publishing service`（76.336 s）。中断 mask 仍只丢 bit28，fence 轮询，
    和 hfi26 一致；没有 GPU fault/panic。
  - **但 UI 还不是 GPU 渲染**：backboardd/SpringBoard 反复
    `IOServiceMatching("IOAcceleratorES") -> 0 found`（358 次），`AGXAccelerator -> none`。
    内核发布的是 `NativeAdrenoGpuService`（自定义 ABI），不是 iOS Metal 发现链要的
    `IOAcceleratorES` + `MetalPluginName` + `NativeAdrenoMetal` bundle。
  - 下一步二选一：
    1. **用户态 ABI 自检**（小）：root 里已有 `/usr/libexec/liuqin-gpu-test`
       （`a3a46027…`），缺 LaunchDaemon；加 `sheng-gpu-gputest=1` 后应打
       `gputest: ... PASS`。不改内核，只动 root/启动行。
    2. **原生 Metal**（大）：按 sheng 的 `GpuPowerProbe.cpp`/`NativeAdrenoAccelerator.cpp`/
       `drivers/NativeMetal` 发布 `AGXAccelerator`，装 `NativeAdrenoMetal` bundle +
       `NativeBridgeMetalFrontend` + Turnip；目标 `IOAcceleratorES` 匹配 1、Metal 设备出现。
  - 详细日志判读和产物见第 237 步；本步没有重编内核、没有改 root。

- **2026-10-28 GPU 第 236 步（上一版）：GPU 停在 ColdStart 之前——启动行把 `cmddb` 拼成了 `cmdb`；已出 `shim-boot-gpu1-cmddb.img`，等 184.txt**
  - 当前镜像换成 `shim/out/shim-boot-gpu1-cmddb.img`（`bd44bcf3…`，32,018,432 B，
    payload `3e4c79f6…`，内核仍是 touch183 的 `a291772a…` 17,037,056 B，未重编）。
  - 根因（日志 + 源码）：`181.txt` 每次停在 `GMU boot needs sheng-gpu-cmddb-version`；
    启动行里是 `sheng-gpu-cmdb-version=1`（少一个 `d`）。这是拼 567 B 触摸行时引入的，
    从 touch167 起 GPU 的 ColdStart 就没开始过。
  - 只改启动行一个字符，启动行 568 B（cmdline 字段 608 B，上限 607）；内核/触摸/按键
    代码未动，按键仍默认开。`validate-repo` PASSED。
  - 上机后先看 `ColdStart status=0 ...`、`GPU ready`、`CP self-test ... wrote=0x600dc0de`；
    若起来，下一步是用户态 Metal（`docs/GPU_MILESTONE.md` “Still open”）。
  - 触摸仍是 181 的行为；GPU 起来后如果触摸/GPU 互相影响，再按 `184.txt` 分开定位。

- **2026-10-28 按键第 235 步（上一版）：找到 v7 映射表偏移/PPID 掩码的根因，touch183 诊断镜像已出，等 183.txt**
  - 当前镜像换成 `shim/out/shim-boot-touch183-keys-spmi.img`（`e6d31a50…`，32,018,432 B，
    内核 `a291772a…` 17,037,056 B，payload `7aacb1ff…`）；启动行仍是 567 B 原样。
  - 这一版**默认开按键**（`liuqinkeys=0` 可关），只是诊断：线程先睡 120 s，再碰 SPMI。
  - 根因（Linux 源码事实，不是上机观察）：v7 的 APID→PPID 表在
    `core + 0x2000 + 4*apid`（旧代码用 `0x0b00`），且 PPID 只有 12 位（旧代码用 13 位）。
    旧的 26 ms/次读很可能就是读未实现的 `0x0b00` 窗口造成的，touch183 会实测。
  - 上机后先看 `LIUQIN KEYS:` 的 `first map read` 耗时、`bus0 scan`、
    两条 `ppid ... -> apid N`，再按电源/音量键看 `power down/up` 等边沿。
  - 触摸仍是 181 的行为（触摸的初始化/下载/轮询路径一行没改），但这一版不是最终发布配置；
    按键验证完后要把默认关闭改回来。

- **2026-10-28 交接（上一版）：触摸可用但不稳，按键未做，两份文档先读**
  - **正在用的镜像**：`shim/out/shim-boot-touch181-endless.img`
    （`5a27d487…`，payload `2d0d369b…`，内核 `30fb9db7…`，17,036,912 B，
    启动行 567 B 未变）。源码树与它逐字节对应，`touch182` 已回退。
  - **触摸现状（观察）**：能用；固件在使用中每 30–90 s 崩一次（报告区变 `fd`，
    state 仍 0xa3、`0x51` 仍 0x00），每条 revive 会真正重下固件（约 3 s），
    有时恢复、有时不恢复；revive 计数在解出有效帧后清零，不会再"用满 10 次永久放弃"。
    181 上出现过一次约 7 分钟黑屏：日志整份停住、**没有 panic**，原因未明。
  - **按键现状**：代码在 `bsd/dev/liuqin_touch.c` 末尾的 PMIC keys 段，
    **默认关闭**（`liuqinkeys=1` 才启用），从未成功读过一次按键。
  - **下一步做按键，先读 `docs/KEYS_HANDOFF.md`**（硬件地址、SPMI arbiter v7 规则、
    `ppid 0x013 → apid -1`、26 ms/次读的代价、五步方案、六个坑、构建与判读命令）。
  - 三个必须记住的点：
    1. 固件引导后**必须无条件写一次 `0x125851`**，否则约 18 s 后报告区变 `fd`；
    2. DMA 判据是"命令完成 + 无错误位 + `LEN_IN` 走满"，等待时间必须按 `len×8/spi_hz` 放大；
    3. 双工 opcode 是 3；`revive` 必须真的调用 `tch_fw_download()`。
  - 触摸这条线的完整证据链在 `docs/DMA_HANDOFF.md`（文末"通了"以下到最后一节）。

- **2026-10-25 重要操作提醒：Android 会吃掉 userdata 上的 iPadOS root**：
  移植把 iPadOS root 放在 **`userdata`** 分区（`rd=ufsroot0`，分区名默认 `userdata`）。只要机器
  正常开机进了 Android，它的首启动/加密/fsck 流程会重格式化或重新加密这个分区，**root 就没了**。
  正确流程：关机 → **按住「音量减 + 电源」进 FASTBOOT** → `fastboot boot <image>`；不要用
  `fastboot reboot`（会按默认顺序启动系统）。万一误入 Android，立刻长按电源 10 s 强制关机。
  为了快速恢复，用 `tools/sparsediff.py`（OLD = 全零镜像）从 `ipad71-root-nodaemon.img` 生成了
  稀疏恢复镜像 **`~/liuqin-root/ipad71-root-nodaemon-restore.simg`**（10 GiB，sha256
  `4c38879f…`，2,634,638/4,194,304 块），恢复命令
  `fastboot -S 512M flash userdata ~/liuqin-root/ipad71-root-nodaemon-restore.simg`，
  比整包 16 GiB 快一些。以后再想彻底避免这个风险，可以考虑把 root 放到别的分区并给 boot line
  加 `ufsroot=<分区名>`（内核按 GPT 分区名查找，不硬编码 LBA）。

- **2026-10-27 触摸第 187 步：不靠重下固件，先用 INT 低电平把它从深睡里叫醒（touch141）；另外 /tmp 被清空过一次**：
  用户问"能不能不让他静默死亡"。把 vendor 行为再对一遍：
  - `nvt_esd_check_func()`：**超过 NVT_TOUCH_ESD_CHECK_PERIOD 没有中断就整体重下固件**——
    说明"这块屏会自己安静下来、需要重新加载"是 vendor 也知道的行为，不是我们独有；
  - 而 Novatek 深睡（host cmd 0x11）的唤醒方式正是**把 INT 拉低一小会儿**（`nvt_int_wake` 的老思路）。
  观察到的死亡形态也符合深睡而不是崩溃：`state` 仍 0xa3、寄存器可读、缓冲静止、**INT 完全不拉**；
  如果是崩进 CRC 重启循环，INT 会周期性脉冲、state 会读 0xFC。
  所以 touch141 在"静默看门狗"前面加了一级：
  1. 连续 5 s 没有触点帧 → `tch_int_wake()`（gpio21 输出低 200 us 再放开），只打印一行；
  2. 仍然 20 s 没帧 → 才走 `0xa5`×2 → `0x69` → 重下（约 3 s），最多 10 次。
  这样正常使用中即使面板打盹，也只是一次几十微秒的 INT 脉冲，不会每 20 s 重下。
  另外：约 15:55 系统清空了 `/tmp`，`tools/build-local.sh` 重新 clone 了 xnu 并只套用仓库里的补丁序列
  （丢了今天的 `liuqin_dylib.c`、`kern.exec_inject`、快速位敲等）。已用
  `~/liuqin-root/wip/2026-10-27/`（`xnu-modified.diff` + `xnu-untracked-files.tgz` + `liuqin_touch.c`）
  把树恢复到 14:53 状态后重打补丁，并重建 `/tmp/ld.lld`（zig ld.lld 的包装脚本，也存了一份在 wip 目录）。
  之后**每轮都刷新这份快照**。
  产物：内核 `d0ffcab9…`、`shim/out/shim-boot-touch141-intwake.img`（sha256 `e3d9c962…`）。

- **2026-10-27 触摸第 186 步：touch139 能用了 30 s 又死；"fw verify 51 字节不符"其实是校验本身读不了（touch140）**：
  touch139 日志（sha256 `008e947d…`，8548 行）：
  - 三次重写都报 `fw verify: 51 of 201410 bytes differ (first at SRAM 0x01000)`，
    速度从 4→8→16 us 都一模一样——**确定性**，说明不是位错；
  - 之后仍然 `BOOTED`，`first frame 0a 1f 36 70 7f 0a`、`finger 1 ev 2 raw 7990,28799`、
    `event 0..300 dispatch ok`（307 个触点帧），**然后 30 s 后再次停止**，
    而且这次 **INT 也不再断言**，所以 touch137/138 那套"中断边沿三次无效帧"的看门狗**完全没触发**。
  查代码发现 verify 的锅：`tch_nvt_read()` 的缓冲区是 `NVT_BUF_MAX = 176`，
  而校验按 256 B 一块读（`len + overhead = 258 > 176`）→ 每次读都直接返回 -1，
  于是"每个采样块算一个坏块"：ILM 30 块 + DLM 21 块 = **正好 51**。
  也就是说那 51 是"没读成"，不是"数据不同"——三次重写纯属浪费（还顺带踩到 `have_header`
  在 retry 时没清零、把尾部记录重复写了 14 次的 bug）。
  touch140：
  1. `NVT_BUF_MAX` 176 → 320，256 B 校验读现在真的会读；
  2. retry 时 `have_header = false`（每轮重新解析 section 表）；
  3. **静默看门狗**：因为固件会"既不报点也不拉 INT"，唯一的信号只能是沉默本身——
     连续 20 s 没有触点帧就重下固件（`0xa5`×2 → `0x69` → 下载，约 3 s），最多 10 次；
     正常使用时不触发，空闲时会周期性重下（代价 3 s / 20 s，先换取可用性）。
  产物：内核 `154475e6…`、`shim/out/shim-boot-touch140-revive20.img`（sha256 `ad9ce7f1…`）。

- **2026-10-27 触摸第 185 步：找到真凶——下载的固件镜像确实是坏的（787 字节不符），根因是我把自旋标定写错了 1000 倍（touch139）**：
  touch137 日志（sha256 `38dbe35d…`，6559 行）：
  ```
  LIUQIN TOUCH: fw verify: 787 of 201410 bytes differ, first at SRAM 0x00100
  LIUQIN BOOT: touch fw verify BAD at 85.521 s (+187 ms)
  ```
  采样（每 4 KiB 读 256 B）就有 787 字节不符，换算下来整幅约 6% 是错的——**"字节对"从来只是假象**：
  BLD CRC 只反映最后一段、逐块回读只看了每块前 16 B，所以一直没发现。这也解释了全部怪现象：
  固件能启动（代码大体完整）、fwinfo 正常，但运行一会儿就掉进 vendor 所说的 CRC-fail 重启循环。
  根因（本轮的乌龙）：`bb_calibrate()` 里把 "每微秒多少圈" 算成了 `n*1000/(ns/1000)`，多了 1000 倍，
  结果被 `per_us < 100000` 的合理性检查拒掉，保留默认 **60**——而真实值是 141，
  于是位敲实际比标称**快 2.4 倍**（pace 4 us 实际约 1.7 us，约 300 kHz），刚好越过这块面板的可靠边界。
  改动（touch139）：
  1. 标定公式改成 `n / (ns/1000)`，并按实测结果设置自旋圈数；
  2. 下载改成 **"写完立刻整段采样回读，不符就重写"** 的循环（最多 3 次，每次 pace 翻倍 4→8→16 us），
     直到 `fw verify` 报告与文件一致才开始 boot_ready；
  3. 保留 touch138 的其余改动（删除 boot 后补发的 host 命令、INT 边沿三次无效帧 → vendor 式重下）。
  产物：内核 `58356f50…`、`shim/out/shim-boot-touch139-cleanimg.img`（sha256 `01805194…`）。

- **2026-10-27 触摸第 184 步：touch136 反而完全没触摸 —— 就是 boot 后补发的那两条 host 命令（touch138 撤回）**：
  用户 revive 日志（sha256 `c08a4bc4…`，7685 行）：
  - 下载仍然 2 s、`BOOTED`、`state 0xa3`、fwinfo 正常；
  - 但整轮 **0 个 contact frame**（`beat 30/60/…/180s: running, … 0 contact frames`），
    报告区是 `ff 00 00 00 00 00 ff 00 …`（6 字节周期的空记录），和 fastbb（touch135）刚开机时一样；
  - 20 次 INT 边沿，但每次读到的都是这个空模式；
  - 用户确认这份日志就是 **touch136**（revive 那版），所以没有 `fw verify` 行是正常的 ——
    touch137 的验证版**还没上机**。
  与能用的 touch135 相比，touch136 唯一的"主动"改动是 **boot 之后又发了一遍
  `{0xD0,0x70,0x90}`（heatmap off）和 `{0xD0,0xAE,0x00}`**（下载路径里那条一直都在，135 也有）。
  这两条在 boot 之后重发很可能把固件打进了别的模式（0x70/0x90 本来就是 heatmap 相关，
  0xAE 又与 FW CRC 状态相关）——所以 touch138 **把它们整个撤回**，恢复 135 的行为。
  touch138 另外两点：
  1. `fw verify` 改成**每 4 KiB 采样 256 B**（原来整段回读，耗时且怀疑没跑），并在开头先打印一行
     `fw verify: reading the image back`，这样日志里一定能看到它是否执行；
  2. 挂死判据放宽成"**INT 边沿 + 读到无效帧**"三次（不再只认 0xFD，`ff 00 …` 这种也算），
     三次后走 vendor 的恢复：`0xa5`×2 → `0x69` → 重新下载（2 s）。
  产物：内核 `2ad3e17e…`、`shim/out/shim-boot-touch138-verify2.img`（sha256 `621f456c…`）。

- **2026-10-27 触摸第 183 步：触摸真的通了（快速位敲），但一次手势后固件进入 0xFD 死循环（touch137）**：
  用户 fastbb 日志（sha256 `43dd5506…`，8607 行）第一次给出"能用"的完整证据：
  ```
  LIUQIN TOUCH: first frame 0a 26 70 6f b8 08 ff ff ff ff
  LIUQIN TOUCH: finger 1 ev 2 raw 9840,28600 of 18000x28800
  LIUQIN HID: touch 0 down at 984,2860 edge
  |backboardd| event 0: type 1 finger 0 0.547,0.993 -> dispatch ok
  … event 50 / 100 / 150 …
  ```
  也就是说：**提速到 2 s 下载之后，帧真的解出来了、事件真的派发到 iOS**（之前读 65 B 要 25 ms，
  读到的是撕裂/半新半旧的数据，这正是 0xFD 假象的来源）。
  然后是死法：
  ```
  beat 90s:  running, 7647 event reads (0 failed), 172 contact frames
  beat 270s: running, 22797 event reads (0 failed), 172 contact frames
  ```
  - 总线完全正常（0 次读失败）、`state` 一直是 0xa3；**172 个 contact frame 之后再没增加**；
  - 91 s 之后出现周期约 300 ms、宽 11 ms 的 INT 脉冲，脉冲期间读到的报告区是 `fd fd fd …`；
  - 对照 vendor：`nvt_wdt_fw_recovery()` 把"中断携带 0xFD/0xFE"当作固件挂死，
    并且 SPI_FLASH 分支里还有一段"IC 处于 CRC fail 重启循环"的恢复（状态读 0xFC，
    `nvt_sw_reset_idle()` ×2 + 清 CRC_ERR_FLAG 0xA5）——**和我们的现象完全对得上**。
    也就是说：抬手之后固件自己挂进重启循环，只有重下固件能救。
  touch137 的改动：
  1. 下载后**整镜像回读比对**（ILM 0x1d1bb + DLM 0x14107，按 256 B 块读，
     现在位敲够快，代价 2–4 s）：打印 `fw verify: image identical to the file (…)`
     或差异字节数 + 首个差异 SRAM 地址。这是唯一能回答"镜像到底对不对"的证据
     （BLD CRC 寄存器只反映最后一段）。
  2. 挂死判据改成 vendor 的：**只有"中断边沿 + 报告区 0xFD"才计数**，三次触发
     （普通抬手后缓冲也是 0xFD，按读次数计会误报，touch136 那种写法会变成每次抬手都重下）。
  3. 恢复顺序按 vendor：`0xa5`（sw_reset_idle）×2 → `0x69` → 重新下载（2 s）→ 清计数。
  产物：内核 `2e75d72f…`、`shim/out/shim-boot-touch137-verify.img`（sha256 `a8713bc6…`）。

- **2026-10-27 触摸第 182 步：位敲提速 40 倍（下载 84 s → 2 s）；apps_smmu 其实完全可读；固件"假运行"= 0xFD（touch136）**：
  用户 smmuprobe 那一轮的日志（sha256 `b612a167…`，6686 行）给出三个结论：
  1. **提速成功**：`bit-bang spin 60 iterations/us (141562 ns for 20000 nops)` 之后，
     `fw dl part 0 … 1186 ms`、`part 1 … 817 ms`、总 `201924 B 6 chunks 2 s`（之前 68–84 s），
     `dlm readback` 与文件一致 —— 位敲的问题确实是 `delay()` 的固定开销，不是 GPIO 速度。
  2. **apps_smmu 完全可读**（与 touch132 的 panic 相反）：
     ```
     smmu-probe tcru:          [0x00] 0x00000000 [0x20] 0x00000000
     smmu-probe anoc_1_tbu:    [0x00] 0x90520000 [0x20] 0x00100000
     smmu-probe kgsl_smmu:     [0x00] 0x002e0406 [0x20] 0x4c017e18
     smmu-probe apps_smmu_gr0: [0x00] 0x00200406 [0x20] 0x4c017e71
     ```
     说明 SMMU 没有被锁死；touch132 那次外部中止更可能是**先做了 0x85200000 的 DMA 页映射/清零**、
     或者那次用了 64 KiB 映射造成的顺序问题。**结论：DMA 路线可以再试，但顺序要改成"先 SMMU、后 DMA 页"。**
  3. **固件"假运行"**：4 次 INT dump（间隔 3 ms 连读）都是 `fd fd fd …`（96 字节全 0xFD），
     `2fe00` 全 FF、`11a00` 是静态数据、`nodummy` 只多一个 0x24；同时 `state 0xa3`、`fwinfo 19 e6 26 3c` 正常。
     按 vendor 的 `nvt_wdt_fw_recovery()`，`point_data[1..6]` 连续 10 次全 0xFD/0xFE = **固件挂了**。
     → 我们能下载"字节正确"的固件、能把它启动到 state 0xa3，但报点引擎没起来。
  touch136 的应对：
  - 补上 vendor boot 后的两条 host 命令：`{0xD0, 0x70, 0x90}`（heatmap off）与 `{0xD0, 0xAE, 0x00}`（crc/host ready），
    发完立刻 dump 一次报告区；
  - 实现 vendor 的 WDT 恢复：报告线程统计连续 0xFD，满 10 次置 `tch_fw_dead`；
    恢复线程做 **软件复位 0x69 + 重新下载**（现在只要 2 s），最多 2 次；
  - INT 边沿日志上限 12 → 40，INT dump 4 → 8 次。
  产物：内核 `ec20c50b…`、`shim/out/shim-boot-touch136-revive.img`（sha256 `4f3a8c3f…`）。

- **2026-10-27 触摸第 181 步：SPI 慢的根因是 `delay()`（每次约 16 us，每位调 3 次）——位敲改自旋+影子写（touch135）**：
  从 touch133 的数字反推：下载 201924 B / 84 s = **392 us/byte = 49 us/bit**；
  把 pace 从 4 us 加到 8 us，每 byte 只多 24 us（预期 +16 us/bit ≈ +128 us/byte）——
  说明**时间不是花在要求的延时上**。`bb_pace()` 调的是 `delay()`，而这个移植里
  `delay(N)` 的固定开销约 **16 us**，位敲每位调 3 次 ⇒ ~48 us/bit，正好对上。
  另外每位还有 5 次 TLMM 访问（其中 2 次是读-改-写），每次也是微秒级。
  改动：
  1. `bb_pace()` 改成**按 nops/us 校准的自旋**（启动时用 `mach_absolute_time()` 量 20000 个 nop，
     得 `bb_spin_per_us`；`liuqintouchspin=N` 可覆盖），每位耗时从 ~16 us 降到几百 ns；
  2. `bb_set()` 用**影子寄存器**，把每次读-改-写变成单次写（CLK/MOSI/CS 都走影子），
     TLMM 访问从 5 次/位降到 4 次/位、少了 3 次读。
  预期：位敲约 5–8× 提速（下载 84 s → 10–20 s，65 B 帧读 25 ms → 3–5 ms），完全不依赖 SMMU。
  同时加了 **SMMU 可访问性探针**（`liuqinsmmuprobe=1`）：依次 map+读
  `0x151ce000`(tcru) → `0x151d1000`(anoc_1_tbu) → `0x3da0000`(kgsl_smmu) → `0x15000000`(apps gr0)，
  每次读之前先打印，屏幕最后一行就是中止的那个窗口。
  新证据（DTB）：`ufshc@1d84000` 带 `iommus = <&apps_smmu 0xe0 0>` 且 `dma-coherent`，
  而我们的 UFS 用物理地址跑到 16 MiB/s ⇒ **apps_smmu 是上电工作的、stream 0xE0 在 bypass**；
  那么 touch132 那次 `esr 0x96000010` 更可能是**访问控制（XPU/TZ）**而不是掉电。
  产物（同一内核 `64ac5940…`，两条启动行）：
  `shim/out/shim-boot-touch135-fastbb.img`（`c9ee963f…`，默认 pace 5 us，测提速+INT dump）、
  `shim/out/shim-boot-touch135-smmuprobe.img`（`d740b2f0…`，带 `liuqinsmmuprobe=1`，测 SMMU 可达性）。

- **2026-10-27 触摸第 180 步：FIFO 引擎确认不可用（TX probe 无完成中断）；位敲下载校验通过；事件缓冲恒为 0xFD = 固件没在报点（touch133 日志）**：
  用户提供的 touch133 日志（sha256 `51c8436c…`，7645 行，启动行带 `liuqintouchfifo=1`）：
  1. **FIFO 真的不可用**：`liuqintouchfifo=1, using the FIFO engine anyway` 之后，
     `CS assert as configured: … irq 0x00000001`（CS 能跑），但
     `TX probe … failed irq 0x00000000`——数据命令**既不完成也不报错**；
     依次试 cancel/abort/clear-disable/start-trigger，`first-irq` 变成 `0x2`（M_CMD_OVERRUN_EN）。
     结论：`GENI_IF_DISABLE_RO` 的 bit0 说的是实话，这个 SE 没有可用的 FIFO 数据路径。
     回退逻辑生效：`SE4 engine did not start, falling back to bit-bang`，之后位敲探测正常。
  2. **慢速下载的内容是对的**：默认 pace 4 us 跑完 84 s，逐块回读校验**一次都没报错**（没有 `fw chunk … read back` 行），
     并且新增的 `fw dl dlm readback @0x100000: 0e 01 01 00 00 00 00 00 | file 0e 01 01 00 00 00 00 00`
     **与文件完全一致**。`firmware checksum bad` 之后把 pace 提到 8 us，但这一轮已经启动成功（state 0xa3）。
     至此可以确认：`crc post dlm cd21b90f` 是**尾巴那条 info 记录的期望 CRC**，不是 DLM 数据坏。
  3. **真正的问题**：INT 触发时 dump 出来的整个报告区是 **`fd fd fd …`（96 字节全 0xFD）**，
     两次连读、3 ms 后再读都一样，同时 `state 0xa3`、`fwinfo 19 e6 26 3c` 都正常。
     对照 vendor 源码 `nvt_wdt_fw_recovery()`：它把 `point_data[1..6]` 全是 `0xFD`/`0xFE` 当作
     **固件挂了的恢复判据**（连续 10 次就重新下载固件）。也就是说：**应用固件"在跑"（状态 0xa3），
     但报点引擎没有产出**——0xFD 是"FW 未就绪"的填充值。
     本轮唯一没法区分的是"固件真挂"还是"我们读这个区域读坏了"：1/4 字节的 state/fwinfo 读得对，
     96 字节的报告区却恒定 0xFD。
  下一步（touch134，`liuqintouchpace=20` 即读也放慢到 20 us）：INT 时同时 dump
  `0x125800`（含 0x125818 一段）、`0x2fe00`、`0x11a00`，并在最后用 **dummy=0** 再读一次 `0x125800`。
  - 若慢读后出现真实帧（`0a …`），是**读总线**的问题（位敲在 5 us 下丢位）；
  - 若依旧 0xFD、而别的地址也没有帧，是**固件没扫描**（回 DLM/初始化序列那条线）。
  产物：内核 `f2abc8fa…`、`shim/out/shim-boot-touch134-slowread.img`（sha256 `d1753ffe…`）。

- **2026-10-27 触摸第 179 步：apps_smmu 的寄存器本身就读不到（外部中止）→ DMA 卡死在 SMMU；改试 FIFO 引擎（touch133）**：
  touch132 上机拿到 panic dump（用户照片）：
  ```
  LIUQIN TOUCH: DMA staging page at 0xfffffe3a21644000 (phys 0x85200000, 32 KiB)
  Kernel data abort, pc 0xfffffe000750ff50 (= liuqin_touch_setup_thread+0x6bc, 内联的 tch_smmu_fix)
  esr: 0x96000010   far: 0xfffffe3a21654000
  ```
  - `esr 0x96000010` = EC 0x25（同 EL 数据中止）+ DFSC **0x10 = 同步外部中止**：
    地址映射本身是好的（不是 translation fault），是**从设备侧拒绝了这次访问**；
  - `far` 正好是 `ml_io_map(0x15000000, 0x10000)` 返回的新 VA（0xfffffe3a21654000），
    也就是**第一条 SMMU 寄存器读取**就中止了。apps_smmu 在 vendor DTB 里带 `qcom,skip-init`
    （由 bootloader/安全世界持有），它的时钟/电源没为我们打开，非安全态读被拒。
  - 结论：**DMA 卡在 SMMU 上，而 SMMU 连读都读不了**。要往下走必须先给它上电（GCC 时钟 + RPMh 投票）
    甚至要拿 handoff SMR 表，那是另一个工程量级的活，不是触摸这条线能顺手做的。
  于是改用**不需要 DMA 的硬件 SPI**：SE 的 FIFO 接口。之前驱动只因为 `GENI_IF_DISABLE_RO` bit0 = 1
  就直接放弃了 FIFO，**从未真正跑过一次 FIFO 数据**。touch133 加 `liuqintouchfifo=1`：
  - 强制 `tch.fifo_capable = true`，走 `tch_fifo_xfer()`（`geni_se_setup_m_cmd` + 轮询 FIFO，
    600 kHz 硬件时钟，没有位敲抖动、不经过任何 DMA/SMMU）；
  - `tch_se_start()` 失败时**自动回退位敲**，机器仍然能用；
  - 保留 INT 缓冲 dump（touch130）与逐块校验/慢速下载（touch128）。
  产物：内核 `0c855dc3…`、`shim/out/shim-boot-touch133-fifo.img`（sha256 `2f10e320…`），
  启动行带 `liuqintouchfifo=1`（**没有** `liuqintouchdma`）。
  下次上机看：`liuqintouchfifo=1, using the FIFO engine anyway` → `CS assert … irq 0x1` →
  `TX probe … ok` → 若 FIFO 真能用，245 KB 固件下载会是**秒级**、且无位翻转。

- **2026-10-27 触摸第 178 步：touch131 照片定位到"第一条 DMA 传输"；apps_smmu stream 0x5A3 的 bypass 尝试（touch132）**：
  touch131 屏幕（可读）：
  ```
  LIUQIN TOUCH: after init SE4 status/clk/spi ...           （init 正常）
  LIUQIN BOOT: touch bus probe at 742.914 s (+3 ms)
  LIUQIN TOUCH: CS assert as configured: after-cmd 0x00000041 first-irq 0x00000000 irq 0x00000001
  LIUQIN TOUCH: DMA #1 tx+rx len 1 tx 0x85200000 rx 0x85204000 mode 0x00000001
  LIUQIN TOUCH: DMA #1 issuing opcode 7 (the transfer starts here)   ← 到此为止
  ```
  - CS 命令成功（`irq 0x1` = M_CMD_DONE），低端页映射/清零都过了（`DMA page fixed`、`staging page … 32 KiB` 都打出来了）；
  - **死在第一条 DMA 数据命令发出之后**：没有 panic、没有后续打印，整机静止。
  这与 SMMU 翻译错误把互联挂住的症状完全一致（stall 型 fault 不会给 panic）。
  从 vendor DTB 确认：`qcom,qupv3_0_geni_se@9c0000`（SE4 所在 wrapper）的
  `iommus = <&apps_smmu 0x5a3 0>`；`apps-smmu@15000000` 是 `qcom,qsmmu-v500`，
  reg = `<0x15000000 0x100000>, <0x151ce000 0x20>`。qsmmu-v500 按 stream id 索引 S2CR
  （`0x8000 + 4*n`），所以我们的寄存器是 **0x1500968C**。
  touch132 改动（`tch_smmu_fix()`，启动参数 `liuqintouchsmmu`：0 关、1 只读、2 读+改）：
  - 映射 0x15000000 一页，打印 `SCR0`、`IDR0`、`S2CR[0]`、`S2CR[1]`、`S2CR[0x5a3]` 和 TYPE 位；
  - `liuqintouchsmmu=2` 且 TYPE != bypass 时，写 `S2CR[0x5a3] = (v & ~3) | 1`（bypass）并回读；
  - **只在 IDR0 合理（非 0、非 0xffffffff）时才写**，避免把别的寄存器当成 S2CR 改掉；
  - 之后照旧 `tch_se_start()` → 第一条 DMA。写 S2CR 是易失配置，改错重启即恢复，最坏结果是再挂一次。
  产物：内核 `3a626947…`、`shim/out/shim-boot-touch132-smmu.img`（sha256 `6d60030d…`），
  启动行带 `liuqintouchdma=1 liuqintouchsmmu=2`。

- **2026-10-27 触摸第 177 步：129 的"死在 touch spi cfg"其实是屏幕看不见后面的话；DMA 走的确实是 apps_smmu 的 stream 0x5A3**：
  用户照片里 touch129 屏幕停在 `LIUQIN BOOT: touch spi cfg at 57.342 s`，之后没有一行。
  查代码：`tch_visible_mark()` 调 `liuqin_boot_mark()`，而 **`liuqin_boot_mark()` 结尾会把
  `liuqin_console_force = 0`**；`tch_visible_mark` 只在 `liuqin_marks_draw` 为真时才把它设回 1，
  而我们的启动行是 `liuqinmarks=0` —— 于是"touch spi cfg"这条 mark 之后，
  整段 bring-up（FIFO 判断、after init dump、DMA 页映射、DMA #1…）**只进了持久日志环、没上屏**。
  屏幕的"最后一行"因此不能当作死点；129 真正的停止位置在那一串看不见的行里。
  修法：`tch_visible_mark()` 保存并恢复 `liuqin_console_force`（调用者要可见就一直可见），
  并在 DMA 路径加了分步打印（mapping → ml_io_map 返回值 → page fixed → staging → SE start →
  `DMA #n issuing opcode` → `DMA #n came back: status/irq/tx/rx/轮询次数`）。
  顺带从 `dtb/boot_unpacked/dtb`（vendor boot 的 14 个 DTB）里查到了 DMA 的关键事实：
  `qcom,qupv3_0_geni_se@9c0000`（SE4 = spi@990000 就在这个 wrapper 里）带
  `iommus = <0x5b 0x5a3 0x0>` —— **apps_smmu 的 stream 0x5A3**；`spi@990000` 在 base DTB 里是
  `status = "disabled"`（真正启用它的是某个 overlay）。也就是说：SE 的 DMA 事务要过 apps_smmu
  stream 0x5A3，如果 ABL 没把它设成 bypass，第一次 DMA 就会触发翻译错误 → 互联挂死，
  这正是 touch129 整机不动的合理原因。下一步（如果 131 的屏幕显示停在 `DMA #1 issuing`）：
  只读地读 apps_smmu 的 S2CR（stream 0x5A3）看 TYPE 位是 bypass(0) 还是 translate(1)/fault(2)，
  再决定是写 bypass 还是放弃 DMA 回位敲。
  产物：内核 `3548a855…`、`shim/out/shim-boot-touch131-dma-visible.img`（sha256 `f423bb1a…`），
  带 `liuqintouchdma=1`；DMA 起不来会自动回退位敲并继续做 INT 缓冲 dump（touch130 的诊断也在里面）。

- **2026-10-27 触摸第 176 步：touch127 日志解读——复位循环确实没了，INT 都动了，但一帧都解不出来；DLM 校验"不匹配"是误报**：
  用户补发的 `touch.txt`（sha256 `c8b23049…`，8191 行）用 boot-args 确认是 **touch127** 那一轮
  （没有 `liuqintouchdma`、没有 `beat`、没有 `bit-bang pace`；`previous boot tail` 里那段
  `idle … poke` 属于上一轮 touch124）。这一轮的关键新事实：
  - **不再有** `recovery … resetting the panel` / `reset pulse` —— 闲置复位循环被修掉了，面板从头到尾是 running；
  - 固件启动正常：`fw dl BOOTED` → `attempt 1 state now 0xa3 (running)`，
    `fwinfo 19 e6 26 3c 46 50 70 80 …`（ver 0x19、38x60、18000x28800）全对；
  - **gpio21（INT）真的断言了**：143.8 / 145.8 / 149.4 / 158.x s 各有一次
    `gpio21 asserted (level 1)` → 55 ms 后 `released`；说明面板在扫描、并且确实有事件；
  - **但 `tch_frame_valid()` 一次都没过**：整份日志没有 `first frame`、没有 `finger …`；
    事件缓冲读回的是一个**恒定的** `ff 00 00 00 00 00 ff 00`（10 秒内不变），
    偶尔 `fd fd fd …`。也没有 `event … dispatch ok`，即触摸根本没到 HID。
  结论：问题已经从"面板被复位"缩小到"**事件缓冲读出来的东西不是帧**"。
  另外把 DLM 校验的事查清楚了：固件尾部 `end-0x10` 处有一条记录
  `sram=0x114208 len=0xff src=0x100 crc=0xcd21b90f`，而芯片的 `R_DLM_CHECKSUM` 读回的正好是
  **0xcd21b90f** —— 我们却拿它和头部 DLM 段的 `0x667268cf` 比。也就是说
  `firmware checksum bad` 是**拿最后一段的校验和去比整段 DLM 的期望值**，属于误报，固件很可能是好的
  （ILM 的 CRC 一直精确匹配 `e3173a51` 也支持这一点）。
  下一步（touch130）：DMA 优先 + **INT 触发时把 0x125800 开头 96 字节原样 dump 出来**（前 4 次边沿，
  每次连读两遍 + 同时读 state/fwinfo 证明总线还活着），用来判断"帧到底在不在这个地址、长什么样"。
  产物：内核 `c9c61886…`、`shim/out/shim-boot-touch130-dma-diag.img`（sha256 `3a54d95b…`），带 `liuqintouchdma=1`。

- **2026-10-27 触摸第 175 步：死磕 DMA —— 低端页其实只被 cache flush 挡住（touch129）**：
  重新读 touch126 的 panic 位置：`tch_dma_alloc()` 里那次 `memset` 早就改成了 64 位写，
  真正走 `phystokv_range()` 的是 **`tch_dma_xfer()` 里的 `flush_dcache64(pa,…,TRUE)`**——
  它把参数当**物理地址**，`phystokv_range()` 对 ADT DRAM 之外的 PA 直接 panic。
  而 0x85200000 这一页现在是 `ml_io_map()` 出来的 **Device 映射（不缓存）**，本来就不需要
  任何 cache 维护，这两处 flush 纯属 kmem_alloc 老路径的残留。本步改动：
  1. 删掉两处 `flush_dcache64`；`memset`/`memcpy` 换成逐字节 `volatile` 访问
     （Device 内存上 clang 会发宽指令，之前正是它触发 unaligned abort）；
  2. **DMA 顺序修正**：原来先写 `SE_GENI_M_CMD0` 再写 TX/RX 指针/长度；Linux 的顺序是
     `select_mode(DMA)` → `geni_se_tx/rx_init_dma()`（指针、ATTR、LEN、IRQ_EN）→ 最后才发命令。
     已按 Linux 顺序重写，并在失败时打印 `GENI_STATUS`/`M_IRQ`/TX/RX 状态和轮询次数；
  3. `tch_cs()`/`tch_cs_probe()` 不再无条件 `tch_fifo_mode()`（那会在 DMA 传输中间把 DMA 关掉）；
  4. 暂存页从 1 页扩成 **2 页（32 KiB）**：TX 在 +0x0000、RX 在 +0x4000，
     单次传输上限从 128 B 提到 0x3f00（256 B 的固件分片和 65 B 的帧都够用）；
  5. `tch_se_start()` 失败时**自动回退位敲**，镜像不会因此完全没触摸。
  产物：内核 `5e1c141d…`、`shim/out/shim-boot-touch129-dma.img`（sha256 `46c20449…`），
  启动行含 `liuqintouchdma=1`。
  下次上机看：`DMA page fixed at phys 0x85200000` → `DMA #1 tx+rx len 1 …` →
  `TX probe … ok` → 若成功，245 KB 固件下载应从 74 s 变成几秒且无位翻转。
  若整机卡死（互联被 DMA 事务挂住），说明 QUP 的 SMMU 没有旁路——那就要先读 Apps SMMU
  的流表（需要 vendor DT 的 stream ID），或者改用 GPI DMA。**未上机。**

- **2026-10-27 触摸第 174 步：`liuqintouchdiv` 根本不影响位敲总线——"慢速"实验其实从没慢过（touch128）**：
  查代码发现：`tch.div` 只写进 GENI 串口的 `GENI_SER_M_CLK_CFG`（`tch_fifo_xfer`/DMA 路径），
  而位敲路径完全不用它——位敲的快慢只由 `tch.bb_pace_us` 决定（`tch_bb_xfer` 里每个半周期
  `delay(bb_pace_us)`）。下载时驱动把 pace 硬设成 **2 us**，正常读帧是 5 us；
  touch123/124 的"降速重试"（div 16→32→64）改的是一个**没人用的寄存器**，
  所以用户看到的"慢速还是没有"完全合理。touch128 因此改成：
  1. 新增 `tch_dl_pace`（默认 **4 us**，`liuqintouchdlpace=N` 可覆盖，1..50），下载真的变慢；
  2. CRC 不过时的阶梯改成 **pace 翻倍**（4→8→16→32），不再动 `div`；
  3. 下载末尾加 **DLM 前 16 字节回读**并与文件比对（ILM 一直对、DLM 一直错，
     回读能区分"总线丢字节"和"CRC 期望值本身不对"）；
  4. 恢复线程每 30 s 打一行 `beat`: 状态、事件缓冲读成功/失败次数、带触点的帧数
     （新计数器 `tch_evt_reads` / `tch_evt_errors`）——下次日志能直接区分
     "总线不通"、"面板不扫"、"帧到了但没解码"三种情况。
  5. 下载**逐块回读校验**（每 0xfc00 块写完读回 16 字节，2 ms 后再读一次，
     不符就整块重写一次；最终仍以 BLD CRC 为准，不让校验本身变成新的失败点）。
  产物：内核 `8249b233…`、`shim/out/shim-boot-touch128-slowdl.img`（sha256 `b290d5eb…`）。
  稳定性路线（按收益排序）：① 位敲降到 4–10 us 并每次都回读校验（本步）；
  ② 帧读取双读比对（未做）；③ 传输层换成 DUP/SE-DMA（受低 4 GiB 映射阻塞，见第 172 步）。
  **touch127/128 都还没有上机。**

- **2026-10-27 触摸第 173 步：触摸"一下又没了"的根因是驱动自己在闲置时复位面板（touch127）**：
  用户重新拉了一份慢速（div 32）启动的完整日志（`touch.txt`，9697 行），里面写得很清楚：
  固件在 165.5 s 成功启动（`attempt 1 state now 0xa3 (running)`，FWINFO 0x19、38×60、18000×28800 全对），
  但 **`recovery 1: no contacts yet, resetting the panel` 在 5 秒后就把它复位了** —— 恢复线程把
  "没人碰屏幕"（0 个触点）当成故障；报告线程还每 2 s 戳一次 INT、每 3 次戳（约 6 s）就脉冲一次复位线。
  日志里 `idle 600 ticks, reset pulse` / `idle 1200 ticks` / … 反复出现，面板每 6 秒被踢回 bootloader，
  之后每次重新下载又要 68–75 s 且"did not boot"，所以触摸从来活不过几秒。
  第二个根因：**复位后的下载是在报告线程每 10 ms 读事件缓冲的同时跑的**——
  两条 SPI 事务交错，`fw dl bld` 读回垃圾（`ilm des 0x2f74a2`）、SRAM readback 全 `ff`、
  `crc pre` 全 0；而开机时第一次下载（那时报告线程还没起）是好的。
  改动（`bsd/dev/liuqin_touch.c`）：
  1. 报告线程**删掉整个闲置看门狗**（不再复位、不再每 2 s 戳 INT）；
  2. 恢复线程改成**按控制器自己的状态判断**：每 5 s 读 `0x125860`（0xa0..0xaf）或 FWINFO 版本字节+补码，
     连续 3 次（15 s）都说"不在跑"才动手；先 `tch_int_wake()`，仍不应答再复位；
     只要 trim id 还能读（ROM 应答），就只做**软件复位 + 重新下载**，不脉冲复位线；
     最多 3 轮，`liuqinrecover=0` 可整条关掉；
  3. **SPI 总线串行化**：`tch_spi_owner` 原子锁包住每次寄存器读写（`tch_nvt_read/write8`、
     `tch_fw_write_sram`、`tch_fw_reg_write`），下载期间 `tch_fw_active=1` 让报告线程整段让开总线。
  产物：内核 `8d136364…`、`shim/out/shim-boot-touch127-noreset.img`（sha256 `d95470c0…`），
  启动行与 touch124 相同（**不带** `liuqintouchdma`）。
  下次上机该看到：开机约 165 s `fw dl BOOTED` / `attempt 1 state now 0xa3`，之后**不应再出现**
  `recovery … resetting the panel` 或 `idle … reset pulse`；`panel silent (check 1 of 3)` 若出现且随后
  `panel answers again`，说明状态判断在工作。然后直接触摸，日志里应出现
  `finger N ev 2 raw …` 与 `|backboardd| event … dispatch ok`。
  **注意**：touch127 还没有上机——上面的结论来自 touch124 的日志，不是新的上机结果。

- **2026-10-27 触摸第 172 步：DMA 固定低端页仍 panic —— 内核的 phys→KVA 只认 ADT 的 DRAM 段（不能直接用低 4 GiB）**：
  touch126 日志：`DMA page fixed at phys 0x85200000` 之后立刻
  `panic: physokv_range: illegal PA: 0x85200000 (dram_vm_init.c:486)` / `phys base 0x840000000, size 0x13ff00000`。
  原因：`osfmk/arm64/arm_vm_init.c` 的 `phystokv()/phystokv_range()` 只在
  `[gPhysBase, gPhysBase+real_phys_size)`（即 ADT 声明的 33 GiB DRAM 段）内合法；
  低 4 GiB 的空闲窗口不在其中。`ptov_table`（最多 8 项）在树里**从未被填充**，`ml_io_map` 的实现来自
  预编译库（只用 phystokv），所以**要支持低端 DMA 页必须让内核认识这段内存**。
  已经备好的信息：低 4 GiB 的两个空闲窗口（`0x85200000–0x856FFFFF` 5 MiB、`0x85E00000–0x87FFFFFF` 34 MiB），
  来自 base DTB 的 36 个 reserved-memory 区（含尺寸，已在上面记录）。
  明天要做的最小方案：
  1. `tools/mkadt.py` 支持**第二段 phys-map 区间**（或给平台专家加一个 `dram-low` 属性）；
  2. 在 `arm_vm_init.c` 里把该区间加入 `ptov_table`（或让 phystokv 接受它）并建立 KVA 映射；
  3. `tch_dma_alloc()` 继续用固定低端页（已经实现），启动行带 `liuqintouchdma=1`。
  今晚可用版本仍是 `touch124`（div 32 + CRC 降速重试 + 闲置恢复，不含 DMA）。

- **2026-10-27 触摸第 171 步：固定低端 DMA 页的 unaligned 崩溃已修（touch126）**：
  touch125 上机 panic：`Unhandled kernel data abort … Unaligned`（esr `0x96000061`），
  崩在 `tch_dma_alloc`——**对 Device 内存映射调用 `memset()`**（`ml_io_map` 的映射不允许非对齐访问）。
  改为**对齐的 64 位写清零**（`volatile uint64_t *`，PAGE_SIZE/8 次）。
  payload `shim/out/shim-boot-touch126-dmalow2.img`（`liuqintouchdma=1`）。

- **2026-10-27 触摸第 170 步：DMA 用固定低端页（0x85200000）——touch125**：
  从 base DTB 精确读出全部 `/reserved-memory`（36 个区，见下表要点）：
  `hyp@80000000+0x600000`、`aop@80800000`、`smem@80900000+0x200000`、`cpucp_fw@80b00000+0x100000`、
  `cdsp_secure_heap@80c00000+0x4600000`（到 0x85200000 结束）、`video@85700000+0x700000`、
  `slpi@88000000+0x1900000`、`cdsp@89900000+0x2000000`、`ipa@8b900000`、`mpss@8bc00000+0x13200000` …
  → **低 4 GiB 有两块明显空闲窗口**：`0x85200000–0x856FFFFF`（5 MiB）与 `0x85E00000–0x87FFFFFF`（34 MiB）。
  改动（`tch_dma_alloc`）：不再用 `kmem_alloc_contig`（会给 34.5 GiB 的地址），改为
  **固定物理页 `0x85200000`**，用 `ml_io_map` 映射并清零，把该物理地址交给 QUP DMA；
  `liuqintouchdmaphys=<低级地址>` 可覆盖（默认即该窗口）。启动行加 `liuqintouchdma=1` 试 DMA。
  payload `shim/out/shim-boot-touch125-dmalow.img`。

- **2026-10-27 触摸第 169 步：SPI 分频默认 16 → 32（touch124）；DMA 的内存前提已查明**：
  用户反馈：降速重试版"7 分钟后有触摸，但一下又没了"→ 说明慢速下载能成功，但**运行时读帧同样会丢位**。
  因此把 `tch.div` 默认值从 **16 改成 32**（下载与运行时读都更慢更稳），校验不过仍会继续翻倍（32→64）。
  DMA 路线的前提（供明天做）：
  - QUP 的 DMA 只能寻址 **32 位物理地址（< 4 GiB）**，我们现在的缓冲落在 `0x8a0150000`（34.5 GiB）→ 第一次传输就冻机；
  - 低 4 GiB **存在**，但被固件保留区占满（从 base DTB `/reserved-memory` 读到：`hyp@0x80000000`、
    `aop@0x80800000`、`smem@0x80900000`、`cpucp_fw@0x80b00000`、`cdsp_secure_heap@0x80c00000`、
    `video@0x85700000`、`slpi@0x88000000`、`cdsp@0x89900000`、`ipa_fw@0x8b900000`、`mpss@0x8bc00000` …）；
  - 明天做法：在这些保留区之间挑一块**经核对空闲**的窗口（如 0x8a000000–0x8affffff 附近，需先读各 reserved 的 reg 尺寸），
    写进 ADT 作为**第二段 DRAM**，并在 `tch_dma_alloc` 里定向分配（而不是让分配器随便给高地址）。
  payload `shim/out/shim-boot-touch124-div32.img`。

- **2026-10-27 触摸第 168 步：DMA 试跑 = 冻机/panic（照片确认），改走"降速重试"（touch123）**：
  `liuqintouchdma=1` 上机后进入 panic（照片上是内核 dump：zone 信息、`LIUQIN UFS`、`LIUQIN TOUCH: spi master mode 0 …`
  之后崩溃）。→ 驱动注释里"SE DMA 不可用"的判断依然成立，DMA 路线作废。
  于是采用**降速重试**：首次 div 16；若下载校验（ILM/DLM CRC）不过，就把 `tch.div` 翻倍
  （32 → 64）再试，日志 `firmware checksum bad, slowing SPI to div N`；
  代价是坏轮里下载时间会变长（74 s → 150 s → 300 s），但**每一轮都是可靠数据**。
  payload `shim/out/shim-boot-touch123-slowbus.img`。

- **2026-10-27 触摸第 167 步：根因指向 SPI 位敲不可靠（CRC 不匹配）→ 两条修法：降速重试 + 试回 DMA**：
  对比"能用的一轮"(h122) 与"不能用的一轮"(h123) 的下载日志：
  - h123：`fw dl part 1 … 2 chunk 30264 ms`（异常慢）、
    `crc pre … dlm exp 667268cf got cd21b90f`（**DLM 校验和不匹配**）、
    `bld: dlm len 0x510000`（读回垃圾）→ 固件数据坏 → 不扫描；
  - h122：part 1 正常、bld 正常。
  即 **SPI 位敲（bit-bang）在临界时序下会丢位**，导致固件下载损坏。
  修法一（已实现）：`tch_fw_crc_ok` 记录校验结果；bring-up 每次重试若发现上一轮校验不过，
  就把 `tch.div` 翻倍（降速）再下，日志 `firmware checksum bad, slowing SPI to div N`。
  修法二（本版试）：驱动里写着"SE DMA 会在第一次传输冻机会停在 0x8a0150000（34.5 GiB）"，
  但我们的 ADT 现在是 `0x840000000 + 0x140000000`（33~38 GiB），**该地址在范围内**，
  所以用 `liuqintouchdma=1` 试回 DMA：若可用，245 KB 下载从 74 s 变成几秒且无位翻转。
  payload `shim/out/shim-boot-touch122-dma.img`（DMA 试跑）；降速重试版为 `touch121`（同一内核，
  不带 `liuqintouchdma`）。

- **2026-10-27 触摸第 166 步：touch120 仍然"有时有、有时没有"——面板 bring-up 可靠性是当前唯一硬件级问题**：
  touch120 日志：bring-up 期间 `attempt 1 state now 0x00 (not ready)` → 之后 `state now 0xa3 (running)`、
  `fw state 0xa3 (running)`（状态寄存器说在跑），但整轮只有 **6 条** event，
  之后 `recovery 1` + 反复 `poke 1..3` + `reset pulse` 都没有换来任何帧。
  结论（今晚到此）：**注入、库、HID、iOS 消费全部稳定可用**；
  剩下的都是**面板本身 bring-up 的可靠性**（状态说 running 但 SPI 读回无记录/FF），
  需要专门一轮把时序/供电/SPI 模式/复位这几件事查清，而不是继续加重试。
  下一步（明天）：
  1. 在失败轮里 dump SE4/SPI 状态与 INT 电平（每 5 s 一行），确认是"总线不通"还是"面板不扫描"；
  2. 对照 `refs/novatek/nt36532_vendor_map.md` 的下载/初始化时序，检查我们漏掉的步骤（如 doze=off、scan enable）；
  3. 若确认是显示休眠导致，再挂 IOMFB 电源状态做"显示唤醒 → 面板 resume"。

- **2026-10-27 触摸第 165 步：闲置唤醒两级（脉冲 INT → 脉冲复位）——touch120**：
  touch119 日志：事件 0…900 都 `dispatch ok`（触摸确实能用一段），之后闲置再停；
  `poking the panel` 出现 4 次也**没能叫醒**（说明单靠 INT 脉冲不总是有效）。
  因此把闲置路径做成两级：每 2 秒脉冲一次 INT；**每第 3 次（约 6 秒）额外脉冲复位 gpio20**
  （100 ms 低 → 高，不重下固件，仍然很轻）；重路径（复位+重下 245 KB 固件）仍只由恢复线程在
  5 秒无帧时兜底。
  payload `shim/out/shim-boot-touch120-idle2.img`。

- **2026-10-27 触摸第 164 步：闲置唤醒——INT 脉冲（touch119）**：
  帧线程新增**闲置看门狗**：每 10 ms 一轮，若 `tch_valid_frames` 没增长则 `idle++`；
  `idle` 每满 200（≈2 秒）就 `tch_int_wake()`：把 gpio21 拉低 200 µs 再恢复为
  "mux 0 (GPIO) + pull-up + 输入"，打印 `idle N ticks, poking the panel`。
  若几次唤醒无效，仍由恢复线程（5 秒无帧）走复位+重下固件那条重路径兜底。
  payload `shim/out/shim-boot-touch119-idlewake.img`。

- **2026-10-26 触摸第 163 步：闲置停摆——面板复位+重下固件能救但很慢；怀疑与"显示休眠/面板 doze"有关**：
  touch118 日志：解锁后闲置一会儿触摸就没了（`event` 只有 5 条、`released stuck` 5 次）。
  恢复线程确实触发了（`recovery 1: no contacts yet, resetting the panel`），随后
  `reset 后读回全 0（state 0x00 / trim id 00…）` → `bring-up attempt 1, state 0xff` → **重新下载固件**（141 s），
  所以"能救但慢（约一分钟）"。
  驱动里**没有任何 doze/sleep 命令**（grep 只有 tsleep），因此更像是：
  1. 面板自身在无触摸一段时间后进入 doze/休眠；或
  2. iOS 让显示休眠（锁屏超时）→ 面板随之休眠，而我们的驱动没有"唤醒"通路。
  下一步（明天）：
  - 在 IOMobileFramebuffer 的电源状态切换处挂钩，显示唤醒时给面板发**唤醒/复位**；
  - 或试"INT 线脉冲唤醒"（部分 Novatek 面板用 INT 由 host 拉低唤醒），先小步验证；
  - 把恢复做成"只在真的停摆时触发、尽量轻（先试唤醒命令，失败再全量重下）"。
  参考：`refs/novatek/nt36532_vendor_map.md`（事件缓冲 0x125800、状态 0x125860 等）。

- **2026-10-26 触摸第 162 步：恢复逻辑做扎实（touch118）——按真实帧计数、能发现"中途停摆"**：
  touch117 上机：库写入/校验 ✓、服务 ready ✓、但只派发了 **5 条**事件，
  之后 `evt data ff…/00… (no record)` 不断、`recovery 1: no contacts yet` 触发一次仍不扫描
  → 面板 bring-up 偶发失败（这次从头就没扫描）。
  两处改进：
  - `tch_valid_frames++` 从"只统计前 6 条日志帧"改为**统计每一帧**；
  - 恢复线程改为**持续监视**：每 5 秒检查帧计数，若没有增长就复位+重下固件（最多 10 轮，
    期间只要帧数增长就把轮次清零继续盯着）→ 既能救"从头没起来"，也能救"中途停摆"。
  payload `shim/out/shim-boot-touch118-recover2.img`。

- **2026-10-26 触摸第 161 步：🐛 找到"用一分钟就坏"的直接原因——帧线程 60 秒后**不再读**事件缓冲区（touch117）**：
  touch116 日志：库派发正常（`event 0..350 … dispatch ok`），随后彻底没有样本、
  看门狗 `released stuck fingers` 触发 **50 次**。
  源码 `liuqin_touch_thread()`：
  ```
  polls++;
  if (!go && polls > 6000) { tsleep(…, ticks); continue; }   // ← 直接跳过读取
  ```
  注释写的是"一分钟后退到每秒一次"，实现却是**一分钟后再也不读**。而这块屏的 INT（gpio21）**从不断言**，
  只能靠轮询 → 正好"一分钟"后完全没有数据。
  修法：删除该退避分支，线程终身 10 ms 轮询事件缓冲区。
  payload `shim/out/shim-boot-touch117-pollfix.img`。

- **2026-10-26 触摸第 160 步：用户指出 "FF 帧时触摸也能用" → FF 是正常噪声；改为给库加派发日志（touch116）**：
  之前把 `first frame … ff ff` 当成"面板没扫描"，但用户说明：**出现 FF 的那些启动里触摸是正常的**。
  所以 FF 只是启动噪声，问题在事件派发/消费一侧。
  库新增派发日志（前 5 条 + 每 50 条）：
  `event N: type T finger F x,y -> dispatch ok/FAILED`，可区分"没有事件""派发了但 iOS 不理""派发失败"。
  同时新增 `tools/embed-dylib.sh`（把 `shim/out/libliuqintouch.dylib` 重新嵌进内核头），
  现在**改库只需重建 boot 镜像**（内核自带写入 `/var/tmp/…`），不必刷分区。
  payload `shim/out/shim-boot-touch116-evlog.img`（sha256 `2fa76bfb…`）。

- **2026-10-26 触摸第 159 步：整段 bring-up 恢复（touch115）**：
  用户说明只刷过 112 和 114（113 未用），所以 h117 日志来自 **touch114**（含重试阶梯 + UFS 日志关闭）。
  该日志显示：固件重试后 state 0xa3 running、库注入/服务 ready/看门狗都在工作，
  但 `first frame … ff ff ff ff`、只有 6 条固定坐标的 `finger`、且 `gpio21 did not assert`
  → **状态说 running，SPI 读回全 FF、INT 不触发**（面板其实没扫描）。
  touch115：新增**整段恢复线程** `liuqin_touch_recover_thread`：
  报告线程启动后，若 6 秒内没有解出任何触点（新增全局 `tch_valid_frames` 计数），
  就**脉冲复位 gpio20**（100 ms 低 → 高）→ 重新 `tch_novatek_info()`（含固件下载/重试）→ 再等 3 秒；
  最多 3 轮，每轮打印 `recovery N: …`，成功打印 `recovery N: panel scans now`。
  payload `shim/out/shim-boot-touch115-recover.img`。

- **2026-10-26 触摸第 158 步：固件重试生效（state 0x7d → 0xa3 running），但这轮 SPI 读回的全是 FF、INT 不触发 → 面板实际没在扫描**：
  touch113 日志：`bring-up attempt 1, state 0x7d` → `attempt 1 state now 0xa3 (running)` ✓ 重试阶梯有效；
  库从 `/var/tmp/liuqin-touch-00000373.dylib` 注入 ✓、`Liuqin Touch Screen ready` ✓、`reading /dev/liuqin-touch` ✓、
  看门狗还打印了 `released stuck fingers` ✓。
  但触摸数据本身不对：
  - `LIUQIN TOUCH: first frame 1a 43 8a 31 a6 09 ff ff ff ff` / `first frame ff ff …`（读回垃圾/全 FF）；
  - 只有 **6 条** `finger …`，都在 128 s 附近，坐标固定（`17290,12710`、`14460,15240`），像是噪声；
  - `gpio21 did not assert for a touch, polling the event buffer` —— **INT 一直不触发**，驱动只能轮询。
  结论：状态寄存器说"running"，但 SPI 读回 0xFF + INT 不动，说明这一轮面板**并没有真的进入扫描**
  （或 MISO/时序有问题）。下一步应是**整段 bring-up 重试**（GPIO 复位 → 重新下载 → 重新初始化），
  而不是只重发固件下载。

- **2026-10-26 触摸第 157 步：屏幕上的 `LIUQIN UFS:` 来自周期性统计行（也强制绕静音）；一并接到 `liuqinmarks`（touch114）**：
  `bsd/dev/liuqin_ufs.c` 里每 1024 次操作打印的吞吐/延迟统计、以及 `ufs_gcc_report()`，都用
  `liuqin_console_force = 1` 强制显示（"即使 liuqinmute=1 也要可见"）。这两处改为受 `liuqin_marks_draw` 控制，
  于是 `liuqinmarks=0` 时**屏幕完全干净**（日志照旧进环）。
  payload `shim/out/shim-boot-touch114-quiet3.img`。

- **2026-10-26 触摸第 156 步：内核自带库**成功**（/var/tmp/… 写入并校验通过），但这一轮固件没进扫描态（touch113 加 bring-up 重试）**：
  touch112 日志（用户提供）：
  - 挂载时三个目录都 ENOENT → `no writable directory`，但**重试线程 1 秒后成功**：
    `LIUQIN LIB: wrote 53712 bytes to /var/tmp/liuqin-touch-00000c8f.dylib, read back cffaedfe (0)`；
  - 注入切到该文件：`EXECENV backboardd: DYLD_INSERT_LIBRARIES=/var/tmp/liuqin-touch-00000c8f.dylib`；
  - 库运行、服务被接受：`|backboardd| Liuqin Touch Screen ready` + `reading /dev/liuqin-touch`；
  - **但触摸控制器没扫描**：`fw state 0xfc (not ready)`、`evt data ff … (no record)`、
    `LIUQIN TOUCH: finger` **0 条**（这次固件下载是跑了、fwinfo 也 ok，但没产出事件）。
  因此"没触摸"的原因从注入/库转到了**固件 bring-up 的偶发失败**。
  改动：`tch_novatek_info()` 里加**重试阶梯**（最多 3 次）：状态不就绪就重新 `tch_fw_download()`，
  每次打印 `bring-up attempt N, state 0x..` 与 `attempt N state now 0x..`。
  payload `shim/out/shim-boot-touch113-fwretry.img`。

- **2026-10-26 触摸第 155 步：根因确认——HFS 读这个文件会返回"别的文件的数据"（probe 的 magic 是 `436f6f72 64696e61` = "Coordina"）**：
  touch111 日志（用户提供）：
  - `LIUQIN LIB: cannot create /private/var/tmp/liuqin-touch-00001632.dylib (2)`（挂载时路径还不可用，ENOENT）；
  - 因此注入回退到 `/usr/lib/libliuqintouch.dylib`；
  - 但探针读出 **`present, 53760 bytes, magic 436f6f72 64696e61`**（ASCII "Coordina"）——
    **设备上该文件的内容不是 Mach-O**，而大小却与 wrap16 镜像一致（53760）。
    这说明**我们的 HFS 读取对这个文件返回了错误的数据块**（镜像里那份是好的，magic `cffaedfe`）。
    这和之前 dyld 报 "not a mach-o file" 完全一致。
  修法：让**内核自己写**一份新文件（新分配、连续），并：
  - `liuqin_dylib_write()` 支持**多目录**（`/var/tmp`、`/private/var/tmp`、`/private/var/mobile/Library/Caches`）；
  - 挂载时若失败，起**重试线程**（每 1 s 一次、最多 30 次，`liuqin_dylib_ready` 控制）；
  - 成功写入后**回读校验** magic，通过才覆盖注入路径；否则保留镜像副本并打印原因。
  payload `shim/out/shim-boot-touch112-kernellib3.img`（sha256 `5925e6f1…`）。

- **2026-10-26 触摸第 154 步：内核写库后没触摸 → 写完回读校验 + 失败回退（touch111）**：
  touch110（内核自带库、写入 `/private/var/tmp/liuqin-touch-<serial>.dylib`）上机**没触摸**。
  由于拿不到日志，直接加自证与保底：
  - 写完立刻重新打开该文件、`vn_rdwr` 读前 8 字节，打印
    `LIUQIN LIB: read back cf fa ed fe (0), wrote N bytes to <path>`；
  - 若魔数不是 `cffaedfe` 或读失败 → **不覆盖注入路径**（保留 `/usr/lib/libliuqintouch.dylib` 那份），
    并打印 `LIUQIN LIB: keeping the flashed copy instead`。
  这样无论内核写库是否成功，**触摸都不会因此丢失**；成功时才切到内核写出的那份。
  payload `shim/out/shim-boot-touch111-kernellib2.img`（sha256 `a05c5125…`）。

- **2026-10-26 触摸第 153 步：屏幕上的字其实来自"每次 exec 一条的时间轴标记"（touch110）**：
  用户："这版的日志还是会打印在屏幕上"。查 `liuqin_boot_mark()`（`bsd/dev/liuqin_ufs.c`）：
  它给**每次 exec** 打 `LIUQIN BOOT: exec <name> at …`，并且**故意 `liuqin_console_force = 1`**
  （设计如此："即使 liuqinmute=1 也画，作为时间轴"）。开机上百个 exec → 屏幕被刷满。
  改为受新开关控制：**`liuqinmarks=0`** 不画（默认：`liuqinmute=1` 时不画）；
  `liuqin_boot_mark`、`tch_visible_mark`、`touch int watch` 三处 `console_force` 都走该开关；
  日志照旧进日志环（printf 仍然输出，只是不画屏）。
  payload `shim/out/shim-boot-touch110-quiet2.img`（sha256 `55e02186…`）。

- **2026-10-26 触摸第 152 步：内核自带触摸库，开机写出（touch109）——以后只换 2 MB boot 镜像即可迭代**：
  动机：每次改库都要整包刷 17 GB，且 sparse 刷写曾把库写坏（dyld 报 "not a mach-o"）。
  做法（照 launcher 曾经的思路，但移到内核）：
  - `shim/out/libliuqintouch.dylib` 生成 `bsd/dev/liuqin_dylib.h`（53,712 字节的 C 数组）；
  - 新文件 `bsd/dev/liuqin_dylib.c`：根分区挂载后立即用 `NDINIT`+`vn_open(O_CREAT|O_EXCL)`+
    `vn_rdwr(UIO_WRITE)` 写到 **`/private/var/tmp/liuqin-touch-<serial>.dylib`**
    （每次启动新文件名，避开移植里已知的 HFS truncate bug），成功后调用
    `liuqin_exec_inject_set("<那个路径> backboardd")`；
  - `bsd/kern/kern_exec.c` 增加 `liuqin_exec_inject_set()`（写 `exec_inject_conf`）；
  - 在 `liuqin_ufs.c` 的挂载钩子里调用（早于 backboardd 的 exec）。
  注意：该文件属于 **BSD 构建目标**（`bsd/conf/files`），不是 `iokit/conf/files.arm64`
  （后者会用 IOKit 的 C++ 标志编译，`BSD_KERNEL_PRIVATE` 没定义 → `struct nameidata` 不完整）。
  payload `shim/out/shim-boot-touch109-kernellib.img`（sha256 `ebd87d25…`，内核变大到 ~25 MB payload）。

- **2026-10-26 触摸第 151 步：修回归——`poll()` 在该字符设备上不可用导致完全没触摸（wrap18）**：
  wrap16 上机**完全没触摸**。原因：我给读取线程加了 `poll(fd)`，但内核 cdev **没有实现 select/poll**
  （`touch_cdevsw` 只有 open/close/read/write），`poll()` 立刻返回，随后阻塞 `read()` 永远等不到数据。
  改法：**恢复原来的阻塞 `read()` 循环**（这是已知能工作的形态），看门狗改为**独立线程**
  （每 100 ms 调一次 `release_stuck()`，250 ms 无样本且有手指按着就合成 UP）。
  同时保留上一版的"不写 /dev/console"和"status 60 s 一条"。
  dylib `77d8681c…`；镜像 `~/liuqin-root/ipad71-root-wrap18.img`（sha256 `bf7ecd5a…`）；
  payload 仍用 `shim/out/shim-boot-touch107-watchdog.img`（内核未变）。

- **2026-10-26 触摸第 150 步：关掉屏幕上的日志输出（wrap17 / touch108）**：
  屏幕上出现的文字来自库里的 `say()`：它会**直接 open /dev/console 写**（绕过 console 静音开关），
  另外 `status:` 每 5 秒一条。改为：
  - **删除 /dev/console 写入**，只保留 `write(2, …)`（走内核 /dev/null tee 进日志环，需要时仍可拉取）；
  - `status:` 改为 **60 秒**一条。
  dylib `0db838bd…`；镜像 `~/liuqin-root/ipad71-root-wrap17.img`（sha256 `d4fe0c84…`）；
  payload `shim/out/shim-boot-touch108-quiet.img`。

- **2026-10-26 触摸第 149 步：修"手指卡住"和"一会失灵"（wrap16 / touch107）**：
  现象：滑锁屏后手指一直处于按下状态；过一会就完全没反应。
  库（`host/liuqin_touch_inject.c`）三处改动：
  1. **读取改成带超时的 `poll()`**（100 ms）：原来用阻塞 `read()`，一旦驱动不再送数据（手指抬到面板外、
     或帧流中断），最后一个手指就永远停在按下状态；
  2. **看门狗 `release_stuck()`**：超过 250 ms 没有任何样本而仍有手指按着，就为每个手指合成 `TOUCH_UP`
     并派发（日志 `released stuck fingers`）；
  3. **状态行**：每 5 秒打印 `status: N samples, M ups, K dispatch failures`，用来判断"失灵"是
     驱动不再送样本（N 不再增长）、还是派发失败（K 增长）、还是 iOS 侧不再消费。
  dylib `861d6aa1…`；镜像 `~/liuqin-root/ipad71-root-wrap16.img`（sha256 `6ea2b959…`）；
  payload `shim/out/shim-boot-touch107-watchdog.img`（sha256 见文件）。

- **2026-10-26 触摸第 148 步：触摸已通；按键硬件位置从设备 DT 里查清（新文档 `docs/KEYS.md`）**：
  用户确认**触摸已可用**。按键方面：库/HID 侧已就绪（`Liuqin Buttons ready`），缺内核按键驱动；
  树里没有 SPMI/PMIC 代码。用用户 dump 的 `dtbo.img` + `vendor_boot.img` 离线解析出：
  - **电源键 = PMK8350（SPMI SID 0）PON 块 `0x1300`**（`qcom,pmk8350-pwrkey`，irq 0x13，`linux,code 0x74`）；
  - **音量键 = PM8350（SID 1）`pinctrl@8800` 的 GPIO**（`gpio_keys/vol_up`）；
  - **SPMI arbiter = `/soc/qcom,spmi@c42d000`**（`qcom,spmi-pmic-arb`，reg 0xc42d000/0x4000、0xc400000/0x3000 等）；
  - PMIC SID：pmk8350@0、pm8350@1、pm8350c@2。
  详细寄存器/计划写在 **`docs/KEYS.md`**（三步：最小 SPMI 读 → PON 电源键 → PMIC GPIO 音量键）。
  内核侧还加了**按键软件自测**（`liuqinbuttonstest=N`，每 N 秒合成 Home），用于在真驱动之前验证
  "按键 → HID → iOS"链路（用户表示系统内不需要 Home 键，可改合成音量键）。

- **2026-10-26 触摸第 147 步：触摸已通（用户确认），开始修按键——先做软件链路自测（touch106）**：
  现状：库(`host/liuqin_touch_inject.c`)已能处理 `BUTTON_DOWN/UP` 并映射到 HID 的
  Power/Menu/Volume usage；但**内核没有任何按键驱动**，且树里**没有 SPMI/PMIC 代码**
  （只有 Adreno 用了 RPMh 寄存器），电源键/音量键都在 PMIC 上 → 需要单独的 PMIC 驱动（较大一块）。
  为了先把"按键 → HID → iOS"这条软件链路验证掉：`bsd/dev/liuqin_input.c` 新增自测线程，
  启动参数 **`liuqinbuttonstest=N`** 时每 N 秒合成一次 Home 键（`LIUQIN_BUTTON_DOWN/UP` +
  `LIUQIN_BUTTON_HOME`，与将来真驱动走同一路径），并打印 `LIUQIN BUTTONS: Home pressed`。
  payload `shim/out/shim-boot-touch106-btntest.img`（sha256 `48e0ac66…`，`liuqinbuttonstest=8`）。

- **2026-10-26 触摸第 146 步：🎉 全链路打通——库进了 backboardd、虚拟服务被事件服务器打开、开始读 /dev/liuqin-touch**：
  touch105（magic 探针版）+ 整包刷过的 `wrap15` 日志（10,790 行左右）：
  - `LIUQIN INPUT: /usr/lib/libliuqintouch.dylib present, 53128 bytes, magic cffaedfe 0c000001`
    → 设备上那份**确实是有效的 64 位 Mach-O**（之前 "not a mach-o" 是 sparse 刷写没写全导致的，
    整包刷之后消失）；
  - **没有** 任何 `not a mach-o` / `inserted dylib … could not be loaded`（严格模式下才会出现）；
  - `LIUQIN INPUT: Liuqin Touch Screen ready` 与 `|backboardd| Liuqin Touch Screen ready`、
    `Liuqin Buttons ready` —— **事件服务器接受了我们的两个虚拟服务**（`opened == true`）；
  - `LIUQIN INPUT: reading /dev/liuqin-touch` + `|backboardd| reading /dev/liuqin-touch`
    —— 库的读取线程已经在跑；backboardd 无崩溃、锁屏正常。
  这条链子现在完整：内核 `kern.exec_inject` → dyld 加载库 → 虚拟服务创建 → **事件服务器打开** → 读取触点。
  下一步只剩"手指事件是否真的被 iOS 消费"的验证与收尾。

- **2026-10-26 触摸第 145 步：dyld 拒绝原因是 "not a mach-o file"，但镜像里那份是好的 → 内核探针改成读 magic（touch105）**：
  touch104（严格模式）终于让 dyld 说出原因：
  `terminating because inserted dylib '/usr/lib/libliuqintouch.dylib' could not be loaded:
   tried: '/usr/lib/libliuqintouch.dylib' (not a mach-o file)`。
  但主机侧核对：`wrap15.img` 里那份 **magic `cffaedfe`（64 位 Mach-O）、sha256 `aec5145b…`**，
  与本机构建完全一致，且整包刷已核对过 sha256。所以要么设备上的读取路径有问题，要么 dyld 看到的不是这份内容。
  于是给内核的文件探针加一项：用 `vn_rdwr()` 读该文件**前 8 字节**并打印（`... present, N bytes, magic xxxxxxxx xxxxxxxx`），
  一眼就能判断设备上那份到底是不是 Mach-O。（之前同类"文件在但内容不对"的问题已多次出现。）
  payload `shim/out/shim-boot-touch105-magic.img`（sha256 `0517084a…`）。

- **2026-10-26 触摸第 144 步：dyld 确实看到了变量、但从未加载库（UUID 不在日志里）→ 开"严格模式"逼它报错（touch104）**：
  touch103 日志：dyld 的环境打印里**有** `DYLD_INSERT_LIBRARIES=/usr/lib/libliuqintouch.dylib`
  （日志第 1278 行 `|backboardd| ibliuqintouch.dylib` 就是它被 tee 打断的片段），
  但用 `dwarfdump` 取到库的 UUID `E687BC46-…` 在整份日志里**出现 0 次** → **dyld 从未加载它**，
  且没有任何报错（dyld 源码 `DyldProcessConfig.cpp` 里 `allowInsertFailures` 为真时就是静默跳过）。
  该位是 AMFI 输出标志的 **bit 5**（`AMFI_DYLD_OUTPUT_ALLOW_FAILED_LIBRARY_INSERTION = 1 << 5`）。
  touch104：启动参数 `liuqininsertstrict=1` 时，`liuqin_amfi.c` 的回答改成 `0xdf`（清掉 bit 5），
  dyld 于是会打印 `terminating because inserted dylib '…' could not be loaded: <原因>` 并让 backboardd 退出
  （这一轮没有 UI 是预期的，只为拿原因）。
  库符号面已核对：32 个未定义符号全部是 CF/dispatch/libc 常规符号，依赖只有 CoreFoundation + libSystem，
  平台 iOS 16.0 —— 不像符号解析问题。
  payload `shim/out/shim-boot-touch104-strict.img`（sha256 `e412dba3…`）。

- **2026-10-26 触摸第 143 步：dyld 打印了 499 条库加载，但没有我们的库、也没有报错 → 再让 dyld 打印它的环境（touch103）**：
  touch102 日志：backboardd 的 dyld 确实在打印库加载（499 行，说明 `DYLD_PRINT_LIBRARIES` 幸存），
  但**没有 `/usr/lib/libliuqintouch.dylib`**，也没有任何 "inserted/could not load" 报错——正是
  AMFI 允许"插入失败不致命"时的静默行为。所以要么库根本没进 dyld 的待加载列表，要么加载被拒。
  touch103：`liuqinprintlibs=1` 时同时注入 `DYLD_PRINT_ENV=1`（dyld 源码
  `DyldProcessConfig.cpp:1033` 里确认支持），让 dyld 打印它保留下来的环境变量——
  这样能直接看到 `DYLD_INSERT_LIBRARIES` 是否还在。
  payload `shim/out/shim-boot-touch103-printenv.img`（sha256 `d4c4a5ff…`）。

- **2026-10-26 触摸第 142 步：注入到位但库里没有任何输出 → 怀疑 dyld 把 DYLD_* 剪掉了（touch102 加 DYLD_PRINT_LIBRARIES）**：
  wrap15 上机：`EXECENV backboardd: DYLD_INSERT_LIBRARIES=/usr/lib/libliuqintouch.dylib` ✓，
  设备上库大小 53128（= 新版）✓，backboardd 在跑（`LIUQIN FB: backboardd sel 57/15` 正常），
  但 **库的 constructor 一行都没打印**（连 "no setup thread" 都没有）。
  观察到：backboardd 的 exec **之后没有任何 `LIUQIN AMFI: policy syscall 90 from backboardd`**，
  而同期 notifyd 等其他进程都问了；dyld 若没拿到 AMFI 的回答，会按默认策略**清空所有 DYLD_ 变量**
  → 库不会被加载，正好对应"零输出"。
  touch102：启动参数 `liuqinprintlibs=1` 时，内核在给 backboardd 注入 `DYLD_INSERT_LIBRARIES` 的同时
  再加一个 `DYLD_PRINT_LIBRARIES=1`，用它判定环境是否幸存（dyld 会逐条打印加载的库）。
  payload `shim/out/shim-boot-touch102-printlibs.img`（sha256 `27ea885c…`）。

- **2026-10-26 触摸第 141 步：内核注入成功，但库的 constructor 阻塞了 backboardd 主线程 → 没有锁屏（wrap15 改为后台线程）**：
  touch101 日志确认 **内核注入生效**：
  `EXECENV backboardd: DYLD_INSERT_LIBRARIES=/usr/lib/libliuqintouch.dylib`（来自内核，绕开 launchd），
  `exec backboardd at 63.156 s`，无 backboardd 崩溃。但**没有锁屏**。
  原因在 `host/liuqin_touch_inject.c`：`make_service()` 在 constructor 里同步等待事件服务器打开服务，
  最多 20 次 × 2 s（两个服务共 ~80 s），而 constructor 跑在 **backboardd 主线程**的 dyld 启动阶段
  → 窗口服务器永远到不了主循环。
  改为：constructor 只起线程，线程里先睡 3 s 再建 client（用自己的 dispatch queue，不再依赖主队列），
  重试缩短为 8 次 × 0.5 s。dylib 新 sha256 `aec5145b…`。
  镜像 `~/liuqin-root/ipad71-root-wrap15.img`（sha256 `8fb820f2…`）；
  稀疏（基准 wrap14）`~/liuqin-root/ipad71-root-wrap14-to-wrap15.simg`（sha256 `ec66318b…`，63 块）。
  payload 仍用 `shim-boot-touch101-inject.img`（内核注入那版）。

- **2026-10-26 触摸第 140 步：照 hackios 的做法——内核 `kern.exec_inject` 注入（touch101），不再替换 backboardd**：
  查 hackios 内核（`oosexgang/hackios-xnu-kernel`，`xnu/hakios.patch` 613–700 行）发现关键机制：
  > *the sysctl kern.exec_inject holds "LIBRARY NAME NAME ..."; every program whose file name is one of the NAMEs
  > is started with DYLD_INSERT_LIBRARIES=LIBRARY ... launchd refuses to pass DYLD_ variables from a job
  > description, which is why the kernel adds this one.*
  即**内核在 exec 时替指定程序注入**，backboardd 保持原版（我们折腾一整天的 wrapper 路线正是错误方向）。
  已移植到 `bsd/kern/kern_exec.c`：
  - `SYSCTL_STRING(_kern, OID_AUTO, exec_inject, …)`；
  - `exec_inject_env()`：按**程序文件名**匹配，生成 `DYLD_INSERT_LIBRARIES=lib1:lib2`；
  - 在 `exec_extract_strings()` 里追加进环境（hackios 原样逻辑）；
  - 启动参数 `liuqininject=` 可覆盖（`off` 关闭），**默认**
    `/usr/lib/libliuqintouch.dylib backboardd`；名字也可用逗号分隔（内核启动参数不支持空格）。
  镜像 `wrap14` 里 `/usr/lib/libliuqintouch.dylib` = `b807830e…`（与本机构建一致）✓，
  `/usr/libexec/backboardd` = 原版 1,184,544 B ✓。
  payload `shim/out/shim-boot-touch101-inject.img`（sha256 `0379619f…`）。

- **2026-10-26 触摸第 139 步：架构真相——iPad 的 HID 事件系统在用户态（backboardd 内），内核侧缺 `IOHIDEventSystem`**：
  touch100（`liuqinhidnoplugin=1`，不发布 `IOCFPlugInTypes`）：backboardd 仍读了 4 次该属性（说明它无条件读），
  但仍**没有任何 `newUserClient`**。从 dyld 共享缓存抽出
  `BackBoardHIDEventFoundation`（356 KB，`ipsw dyld extract … -o /tmp/mts`）后看到它的 API 面：
  `IOHIDEventSystem` / `IOHIDEventSystemClient` / `IOHIDEventSystemConnection`、`kIOHIDEventTypeDigitizer`、
  `digitizerServicePersistentProperties`、`isPencilDigitizer` —— 即 **HID 事件系统服务端在 backboardd 用户态**，
  客户端（SpringBoard 等）连它；内核侧的 `IOHIDEventSystem` 才是 feed 事件的来源。
  而我们树里**没有 `IOHIDEventSystem.cpp`**（只有同名 stub 类，`iokit/Kernel/arm/hid/` 下无该文件），
  Apple 的这份实现要另外取（Apple 开源 IOHIDFamily）。这也解释了"服务发布得再对，也没人来开"的现象：
  内核侧缺了那个把事件送回 backboardd 的 broker。
  **今日净进展**：root 迁到 32 GiB 的 `hfs` 分区并能启动；原版 backboardd 恢复（锁屏正常）且确定"替换 backboardd
  会使 launchd 不再启动它"；内核触摸驱动与帧解码确认正常；内核 HID 触摸服务发布+派发确认；拿到 backboardd 的
  精确匹配字典（0xff00/0xf、DeviceUsagePairs、HIDServiceSupport）并证明它在 150 s 重新发布后会认出设备、
  读取 `IOCFPlugInTypes`；确认 iPadOS 16.6.1 **没有 MultitouchSupport 二进制**，事件系统在用户态。

- **2026-10-26 触摸第 138 步：对照实验——不带 `IOCFPlugInTypes` 时 backboardd 会不会直接打开设备（touch100）**：
  touch99（绝对路径）日志（`BUILD Oct 26 2026 18:20:07`）：relaunch 正常、backboardd 读了 4 次
  `IOCFPlugInTypes`，但仍无任何 `newUserClient`（插件没加载成功）。
  与其继续猜 CFPlugIn 的解析规则，先做一个便宜的对照：启动参数 **`liuqinhidnoplugin=1`** 时不发布
  `IOCFPlugInTypes`。若 backboardd 于是**直接** `IOServiceOpen` 我们的设备 → 插件那条路可以放弃，
  直接实现报告通路；若仍不开 → 说明它确实依赖插件，再去研究 CFPlugIn。
  payload `shim/out/shim-boot-touch100-noplugin.img`（sha256 `68351444…`）。

- **2026-10-26 触摸第 137 步：backboardd 终于"看见"了设备并读取 `IOCFPlugInTypes`（touch99 改绝对路径）**：
  touch98 日志（`BUILD Oct 26 2026 18:12:30`，覆盖到 231 s）关键段落（150 s relaunch 之后）：
  ```
  LIUQIN HID: multitouch device relaunched
  LIUQIN HID: touch relaunched under the device
  LIUQIN IOK: notification drain by backboardd -> AppleMultitouchDeviceAppleMultitouchDevice
  LIUQIN HID: IOHIDDevice property get IOCFPlugInTypes by backboardd
  LIUQIN HID: IOHIDDevice property get IOCFPlugInTypes by SpringBoard  (4 次)
  ```
  → **backboardd 的 digitizer 匹配已经命中**（0xff00/0xf + DeviceUsagePairs 生效），并且它按 CFPlugIn 约定
  去读 `IOCFPlugInTypes`。但仍无 `newUserClient`（插件没成功加载）。
  本轮把映射值从相对路径改为**绝对路径**
  `/System/Library/Extensions/AppleMultitouchSPI.kext/PlugIns/MultitouchHID.plugin`
  （`CFPlugInTypes` = `0516B563-…`，工厂 `IOHIDPlugInFactory`，root 里该 plugin 的 Info.plist 已确认）。
  payload `shim/out/shim-boot-touch99-absplugin.img`（sha256 `6b04b5f9…`）。

- **2026-10-26 触摸第 136 步：A 路线的关键发现——backboardd 的匹配字典（touch98 补 DeviceUsagePairs）**：
  从当前内核日志（`BUILD Oct 26 2026 18:03:04`）里抓到 backboardd 的 IOKit 匹配字典原文：
  - `IOServiceFirstMatch {IOProviderClass=IOService, IOPropertyMatch={HIDServiceSupport=true}, IOServiceNotificationUser}` → notify
  - `IOServiceFirstMatch {IOProviderClass=IOHIDDevice, PrimaryUsage=0xf, PrimaryUsagePage=0xff00, …}` 
  - `IOServiceFirstMatch {IOProviderClass=IOHIDDevice, DeviceUsagePairs=[{DeviceUsage=0xf, DeviceUsagePage=…}] …}`
  即 **backboardd 的主屏 digitizer 是"IOHIDDevice + 0xff00/0xf"**（正是 iPad 的多点触控设备），
  而且第二组匹配用的是 **`DeviceUsagePairs`** 属性——我们之前只发布了 `PrimaryUsage*`，没发 `DeviceUsagePairs`。
  另外查明：这个 iPadOS root 里 **`MultitouchSupport.framework` 没有二进制**（只有 Info.plist，
  缓存 2574 个镜像里也没有它）→ 那条 CFPlugIn 路在这里不存在，之前文档的结论只适用于 iPhone 16.7 用户态。
  touch98：在 `AppleMultitouchDevice::start()` 里补 `DeviceUsagePairs = [{DeviceUsagePage:0xff00, DeviceUsage:0xf}]`
  （保留 `PrimaryUsage*`、descriptor、`IOCFPlugInTypes`）。
  payload `shim/out/shim-boot-touch98-usagepairs.img`（sha256 `b8dd8f14…`）。

- **2026-10-26 触摸第 135 步：日志版本再混淆 → HID 模块加"BUILD"戳（touch97）**：
  用户给的 touch96 日志只有 7376 行，且内容仍是旧 stub 的
  `IOHIDDevice stub newUserClient type 0x484944 by gamecontrollerd`、`buttons publishes in 120 s` ——
  又是旧内核的启动。为此在 `AppleMultitouchDevice::start()` 里加一条醒目的
  `LIUQIN HID: BUILD <__DATE__> <__TIME__> (multitouch device)`，
  以后任何日志都能一眼确认版本（touch97 的戳应为 `Oct 26 2026 18:0x`）。
  payload `shim/out/shim-boot-touch97-mtdev2.img`（sha256 `09d66e07…`）。

- **2026-10-26 触摸第 134 步：最小设备路线开工——给 AppleMultitouchDevice 补 digitizer 属性/描述符（touch96）**：
  查了 root 里的插件：`/System/Library/Extensions/AppleMultitouchSPI.kext/PlugIns/MultitouchHID.plugin`
  的 `CFPlugInTypes` = **`0516B563-B15B-11DA-96EB-0014519758EF` → `0CB0FBB9-…`**（`IOHIDPlugInFactory`），
  与我们 stub 里发布的映射一致，所以映射本身不是问题——缺的是**设备属性**（真实 iPad 的 multitouch 设备带
  digitizer 用法对与 report descriptor，插件才会去 attach）。
  本轮：把旧的 `LiuqinHID.cpp`（含 `IOHIDDevice`、`AppleMultitouchDevice`、`LiuqinHIDDeviceUserClient`）
  恢复为构建目标（替换 LiuqinHIDSweet），并在 `AppleMultitouchDevice::start()` 里补：
  `PrimaryUsagePage=0xff00` / `PrimaryUsage=0x0f` / `Transport="SPI"` / `VendorID=0x05ac` / `ProductID=0x0273`
  以及 10 指 digitizer 的 `ReportDescriptor`（contact id / tip switch / 16-bit X,Y），保留 `IOCFPlugInTypes`。
  payload `shim/out/shim-boot-touch96-mtdev.img`（sha256 `42162215…`，干净启动，`liuqinhidtest=0` 关掉按键自测刷屏）。

- **2026-10-26 触摸第 133 步：IOHIDDevice 移植的可行性实测（补头文件 12 轮，结论：需要专门一轮来做）**：
  把 Apple 的 `IOHIDDevice.cpp`(2682 行)、`IOHIDDeviceElementContainer`、`IOHIDElementContainer`、
  `IOHIDElementPrivate`、`IOHIDReportElementQueue`、`IOHIDEventQueue`、`IOHIDFamilyPrivate` 加进构建，
  逐轮补齐缺失依赖：
  - 头文件：`TargetConditionals.h`、`IOKit/hidsystem/{IOHIDDescriptorParser*,IOHIDParameter,IOHIDSystem,
    IOHIDTypes,IOHIDShared,IOHIDevice,IOHIDWorkLoop,IOLLEvent}.h`、`IOKit/hid/*`、`IOKit/graphics/*`、
    `HIDDriverKit/*`（从 Kernel.framework 的 Headers 目录整目录拷入 `iokit/HIDDriverKit/`）。
    `IOKit/hid|hidsystem` 下改成**转发头**指回 `iokit/Kernel/arm/hid/`，避免与 SDK 版冲突。
  - 缺的 key 常量已写进 `iokit/Kernel/arm/hid/LiuqinHIDCompat.h` 并在 7 个源文件里前置包含
    （`kIOHIDProtectedAccessKey` 等 8 个）。
  - **仍缺**：`IOSimpleReporter` / `IOReportLegend`（Apple 的 IOKit 上报 API，我们只有空壳 IOReporter）、
    `kIOHIDDeviceForceInterfaceRematchKey`、`kIOHIDDKStartKey`、`kHIDUsage_AppleVendor_MultipleInterfaces`
    → 这些是"完整移植 IOHIDFamily"才会遇到的尾部长尾。
  **构建已回退到绿色**（files.arm64 只留 IOHIDEvent / EventServiceQueue / EventServiceUserClient /
  LiuqinHIDSweet），头文件与兼容层的改动都保留在树里，供下一轮继续。
  下一轮的更优做法（省掉 IOReporter 长尾）：**不编 Apple 的 IOHIDDevice.cpp**，而是基于旧 stub 里已有的
  `IOHIDDevice`+`LiuqinHIDDeviceUserClient`+`AppleMultitouchDevice`（备份在 `/tmp/LiuqinHID.cpp.bak`）
  做一个最小设备：发布 `IOCFPlugInTypes`（MultitouchSupport 的 CFPlugIn）、报告描述符与
  `PrimaryUsagePage/Usage`，并实现 MultitouchSupport 会调的那几个 selector。

- **2026-10-26 触摸第 132 步：新版内核确认"零客户端"；探查 IOHIDDevice 移植的可行性**：
  用户 touch92 的日志（构建戳 `Oct 26 2026 17:34:31`）：
  `LIUQIN HID: touch registered` ✓、事件在派发 ✓，但 **`newUserClient` 出现 0 次** ——
  这个 iPadOS 16.6.1 用户态**不会打开 IOHIDEventService**。同时 `key 0x40 …` 刷了 8.8 万行
  （`liuqinhidtest` 自测按键把日志环冲掉，以后不传该参数）。
  决定按文档线索走 `IOHIDDevice`：树里其实**有全套 Apple 源码**（IOHIDDevice.cpp 2682 行、
  IOHIDElementPrivate.cpp 2256 行、IOHIDElementContainer、IOHIDReportElementQueue、IOHIDFamilyPrivate 等），
  但直接加进 `files.arm64` 编译时依次缺头文件：`TargetConditionals.h`、
  `IOKit/hidsystem/IOHIDDescriptorParser.h`、`IOKit/hid/IOHIDKeys.h`、`IOHIDParameter.h`、
  `IOKit/hidsystem/IOHIDSystem.h`（后者树里没有，需要从 Apple 源导入或写最小替代）。
  已把 `IOHIDDescriptorParser*`、`IOHIDUsageTables`、`IOHIDKeys/Properties/Shared/Types`、
  `TargetConditionals.h` 复制到 `iokit/IOKit/hidsystem/`、`iokit/IOKit/hid/` 供 `<...>` 形式包含。
  **构建已回退到可用状态**（`files.arm64` 只留 IOHIDEvent / EventServiceQueue / EventServiceUserClient /
  LiuqinHIDSweet），内核恢复可编译。下一步是补齐缺头（IOHIDParameter.h、IOHIDSystem.h）并写设备子类。

- **2026-10-26 触摸第 131 步：关掉屏幕打印（touch93-clean）+ 日志拉取脚本**：
  用户要求停止屏幕打印（他能通过 ADB 读日志了）。touch93-clean 不传 `liuqinshowlog`，
  恢复 `liuqinmute=1` + `liuqinfb_noconsole=1`，屏幕只显示 iOS 画面；持久日志仍在（`ufslogsecs=1`）。
  新增 `tools/pull-hfs-log.sh`：一条命令从 `hfs`（`/dev/block/sda36`，32 GiB）尾部读日志并解析，
  `DEV=` 可覆盖设备节点。
  payload `shim/out/shim-boot-touch93-clean.img`（sha256 `4950b22f…`）。

- **2026-10-26 触摸第 130 步：日志版本混淆 + 触摸链路的事实确认（touch92）**：
  用户提供的 hfs 日志（83,441 行）显示：
  - **触摸帧正常**：`LIUQIN TOUCH: finger 2 ev 2 raw 9730,28740 of 18000x28800` ✓
  - HID 服务注册 ✓，事件也在派发：`LIUQIN HID: touch 0 down at …` / `touch 0 up` ✓
  - 但按键测试是 **`key 0x40 down (0 clients)`** —— **没有任何用户态进程打开我们的服务**
  - 且日志里仍有 `LIUQIN HID: IOHIDDevice property get IOCFPlugInTypes by SpringBoard` 这类行，
    而新内核已删除该 stub → **这份日志来自旧内核的启动**（版本混淆，之前多次误判都源于此）
  对策：HID 文件里加**构建戳** `LIUQIN HID: build <__DATE__> <__TIME__>`；屏幕日志白名单去掉 `CRASH`
  （崩溃守护进程把 HID 行全冲掉了），只留 backboardd/launchd/EXEC FAIL/LIUQIN LAUNCHER/INPUT/TOUCH/HID/newUserClient。
  payload `shim/out/shim-boot-touch92-hidlog.img`（sha256 `8e10c3b1…`）。
  结论性事实：**触摸数据与内核 HID 都到位，卡在"没有客户端来打开服务"**——这正是 iPad 用户态与 iPhone 用户态的差异点
  （sweet/raphael 的 iPhone 16.7.16 里 backboardd 会打开它；我们的 iPad 16.6.1 里 SpringBoard 走的是
  IOHIDDevice + CFPlugIn/MultitouchSupport 那条路，而 `IOHIDDevice` 才是这个用户态真正会打开的对象——
  之前 gamecontrollerd 就成功打开过 type `0x484944`）。

- **2026-10-26 触摸第 129 步：用屏幕日志直接看"手指帧有没有进来"（touch91-touchlog）**：
  touch90（sweet 服务实现）+ wrap14（原版 backboardd）：锁屏稳定，但**触摸完全无反应**。
  为判断是"硬件帧没进来"还是"进来了没人消费"，屏幕日志白名单加入
  `LIUQIN TOUCH`（驱动解帧）与 `LIUQIN HID` / `newUserClient`（HID 服务与打开），
  这样手指按住屏幕时，屏幕会实时显示这两类行，拍一张即可判定。
  payload `shim/out/shim-boot-touch91-touchlog.img`（sha256 `127adca3…`，`ufslogsecs=6`、`liuqinshowlog=2`）。

- **2026-10-26 触摸第 128 步：观察各家实现后的路线修正 + 用 sweet 原版 HID 服务替换我们的（touch90）**：
  看 `Gsjsjzhznsz/ios-raphael`（K20 Pro，触摸✅）与 `HeliumStudio-Dev/ios-raphael`（= ios-sweet）：
  两者都走**内核侧 HID**（patch 0058 "HID services (Apple IOHIDFamily-1915 user client)"，
  GT9886/GT9896 → `SweetHIDTouch` → backboardd），**从不改 `/usr/libexec/backboardd`**。
  而我们的锁屏丢失正是"换掉 backboardd"造成的——`wrap14`（原版 backboardd）实测**锁屏回来了**，
  这条因果链就此定案；用户态注入那条路（需要替换/注入 backboardd）风险高且非必要。
  又发现：**我们的树里本来就有完整 IOHIDFamily 源码**（`iokit/Kernel/arm/hid/` 58 个文件），
  sweet 的核心只有 539 行 `SweetHID.cpp`，我们的 `LiuqinHID.cpp` 是它的衍生版（1180 行）。
  于是把 **sweet 的服务类（LiuqinHIDButtons/LiuqinHIDTouch）原样搬进来**，只保留我们文件里的
  `IOHIDEventService` 基类实现（我们树没有 `IOHIDEventService.cpp`），合并为
  `iokit/Kernel/arm/hid/LiuqinHIDSweet.cpp`（561 行），`iokit/conf/files.arm64` 指向它，
  旧 stub 移到 `/tmp/LiuqinHID.cpp.bak`。内核 sha256 `8489d4b0…`；
  payload `shim/out/shim-boot-touch90-sweetHID.img`（sha256 `1054fa50…`，干净启动：无屏幕日志）。

- **2026-10-26 触摸第 127 步：关掉屏幕日志（touch89-clean）**：
  用户要求去掉一直闪的日志。payload 里**不再传 `liuqinshowlog`**，并恢复 `liuqinmute=1` +
  `liuqinfb_noconsole=1`（屏幕只显示 iOS 画面，不再有内核文字/滚动日志）。其余（root=hfs、
  tee 名单、`ufslogsecs=1` 的持久日志）不变，日志仍写在 hfs 尾部，需要时再拉。
  payload `shim/out/shim-boot-touch89-clean.img`（sha256 `f68e270c…`）。

- **2026-10-26 触摸第 126 步：确认 launchd 真没启动 backboardd → 用 root job 直接问 launchd（wrap13/touch88）**：
  过滤后的屏幕日志只有内核探针那行 `LIUQIN INPUT: …/com.apple.backboardd.plist present`，
  **完全没有 launchd 提 backboardd 的行、也没有 `exec backboardd`/`LIUQIN EXEC FAIL`** ——
  说明 launchd 根本没尝试启动这个 job（若有尝试，二者必出其一）。
  于是加一个我们自己的 root job（**同时写进 plist 目录和 launchd 缓存**）：
  `com.liuqin.kick` → `/usr/libexec/liuqin-kick`（68,416 B，sha256 `a091b5fe…`），
  它依次执行 `launchctl list`、`launchctl print system/com.apple.backboardd`、
  `launchctl kickstart -k system/com.apple.backboardd`、再 `launchctl list`，
  launchctl 的 stdout/stderr 会经 devnull tee 进入内核日志（因此也会出现在过滤屏幕上）。
  判读：若出现 `LIUQIN KICK: alive` → **缓存/plist 里新增 job 能被 launchd 启动**（这本身是个重要能力），
  随后 `launchctl print` 会直接告诉我们 backboardd 这个 service 在不在 launchd 的表里；
  若 kickstart 成功 → backboardd 被 launchd 启动（带 MachServices），UI 可能立刻出现。
  镜像 `~/liuqin-root/ipad71-root-wrap13.img`（sha256 `7d9df319…`）；
  稀疏（基准 wrap12）`~/liuqin-root/ipad71-root-wrap12-to-wrap13.simg`（sha256 `cc775f13…`，271 块）；
  payload `shim-boot-touch88-kick.img`（sha256 `d6cdaf58…`）。

- **2026-10-26 触摸第 125 步：屏幕日志改成**白名单过滤**（touch87）**：
  touch86 仍显刷屏——因为崩溃的守护进程本身在刷，只截最后 3 KB 依然是别人的垃圾。
  `liuqinshowlog=2` 时只打印**含关键字的行**（backboardd / launchd / EXEC FAIL / LIUQIN LAUNCHER /
  LIUQIN INPUT / CRASH），于是屏幕上永远停着 launchd 对 backboardd 说的话，不受其它输出影响。
  （`liuqin_show_log` 提升为文件级变量，供日志线程与 dump 函数共用。）
  payload `shim-boot-touch87-filter.img`（sha256 `83be4c98…`，`ufslogsecs=6`）。

- **2026-10-26 触摸第 124 步：屏幕日志自激循环的修复（touch86）**：
  touch85 上机后屏幕一直在刷 —— 因为屏幕日志打印的文本又进了日志环，下一次 dump 的内容几乎全是上一次的打印，
  形成自激。修法：dump 时**丢弃自带标记的行**（`LIUQIN SCREEN LOG`、`LIUQIN ROOT:`、`LIUQIN LOG TAIL:`），
  并把周期从 2 s 放长到 6 s（`ufslogsecs=6`）。
  payload `shim-boot-touch86-screenlog2.img`（sha256 `fb2e9a04…`）。

- **2026-10-26 触摸第 123 步：修掉上一步诊断里的空指针（touch85）**：
  touch84 拍屏显示内核在 `load_init_program: attempting to load
  /usr/appleinternal/sbin/launchd.development` 之后立刻 **Kernel data abort**
  （pc/lr 都在我们新加的代码里）——因为 exec 失败后 `imgp->ip_ndp` 可能为 NULL，而诊断代码直接读了
  `ip_ndp->ni_cnd.cn_nameptr`。改成**激活前先拷到局部缓冲**，激活后再打印。
  顺带确认：`liuqinshowlog=1` 的屏幕日志工作正常（照片上就是 `=== LIUQIN SCREEN LOG ===` 段）。
  payload `shim-boot-touch85-execfail2.img`（sha256 `314f9179…`）。

- **2026-10-26 触摸第 122 步：launchd 不启动 backboardd → 给 exec 失败加日志（touch84）**：
  现象：hfs 能启动但没有 UI（"看不见锁屏"），日志里既无 `exec backboardd` 也无 `LIUQIN LAUNCHER`。
  查代码发现 `printf("exec %s")` 在 **mach-o 激活成功之后**（PLATFORM_IOS 分支里）才打印——
  如果 launchd 尝试启动 job 但 `exec_activate_image()` 失败（例如 BAD_MACHO），**不会有任何痕迹**。
  所以在 `exec_activate_image()` 之后加了一条失败日志：
  `LIUQIN EXEC FAIL: <path> error <errno> (pid N)`（`bsd/kern/kern_exec.c`）。
  配合 `liuqinshowlog=1`（屏幕日志），launchd 每次失败的 spawn 都会出现在屏幕/日志里。
  payload `shim-boot-touch84-execfail.img`（sha256 `166cbe3e…`）。

- **2026-10-26 触摸第 121 步：hfs 启动成功（wrap12 无 panic），但 launchd 仍未启动 backboardd；加"屏幕日志"模式（touch83）**：
  wrap12 + `touch78-hfs`：**不再 panic**，`LIUQIN UFS: "hfs" … 32768 MiB, read-write`，
  探针二次运行显示 `/System/Library/xpc/launchd.plist present, 753133`、
  `/usr/libexec/backboardd present, 118976`（我们的 launcher 在位）。
  但日志里**没有 `exec backboardd`、没有 `LIUQIN LAUNCHER: started`**，SpringBoard 起来了（50.1 s、70.4 s 两次）
  却等不到窗口服务器——launchd 没启动该 job（plist 与缓存条目都指向 `/usr/libexec/backboardd`，文件存在）。
  由于日志拉取一直不顺，内核新增**屏幕日志模式**：启动参数 `liuqinshowlog=1` 时，日志线程每 `ufslogsecs`
  把日志环**最后 3 KiB** 打到控制台（`=== LIUQIN SCREEN LOG ===` … `END`），于是**随时拍一张屏幕**就能读到
  最新日志（含 launchd 的消息）。panic 路径也复用同一函数。
  payload `shim-boot-touch83-screenlog.img`（sha256 `4fac23ba…`，`ufslogsecs=2`，无 mute/无 noconsole）。

- **2026-10-26 触摸第 120 步：panic 摘要一击命中——hfs 是 rw，真因是"我把缓存改名了"；wrap12 恢复缓存**：
  touch82 的 panic 摘要在屏幕上打出：
  `LIUQIN ROOT: "hfs" blocks 8388608, read-write`（**hfs 读写正常，排除只读**），
  `LIUQIN LOG TAIL:` 里是 loader 自己的话：
  `Code: /System/Library/xpc/launchd.plist Sig: …launchd.plist.sig` /
  `Using unsecure cache: /System/Library/xpc/launchd.plist` /
  `abort: Failed to open fd [2]No such file or directory` /
  `pid 22 exited -- exit reason namespace 7 subcode 0x1, description Failed to open fd …`
  结论：**wrap11 把缓存改名成 .orig 导致 ENOENT**（loader 是"打开已有文件"，不是"新建"）——
  上一版实验本身是错的。wrap10 那次失败另有原因（最可能是 truncate/覆盖写，与已知 HFS 截断 bug 吻合）。
  wrap12 = wrap11 + 缓存改回原名（内容与 wrap10 等价）。
  镜像 `~/liuqin-root/ipad71-root-wrap12.img`（sha256 `8bc771fd…`）；
  稀疏（基准 wrap11）`~/liuqin-root/ipad71-root-wrap11-to-wrap12.simg`（sha256 `f221d857…`，39 块）。
  关键工具：**panic 摘要打印**（`liuqin_panic_summary()`）让"没有 ADB"也能读证据，后续继续用它。

- **2026-10-26 触摸第 119 步：真凶显现——`launchd_cache_loader` 是 **SIGABRT**，launchd 把它当 boot task 失败后主动 panic；内核加"panic 摘要"打印（touch82）**：
  拍屏读到（`liuqininitnopanic` 拦不住，因为这是 launchd 自己的 userspace panic）：
  `panic: userspace panic: boot task failure: launchd_cache_loader - exited due to SIGABRT`
  + `[launchd] Boot task failed: launchd_cache_loader - exited due to SIGABRT` / `Panicking in 3 seconds.`
  对照：**userdata 上同一个 loader 正常跑完**（`exec launchd_cache_loader` → 问 AMFI → 无崩溃），
  所以差别在 hfs 上它要写的那份缓存写不进去（或 hfs 只读）。
  为了在**没有 ADB**的情况下也能拿到证据，内核新增 `liuqin_panic_summary()`
  （`bsd/dev/liuqin_ufs.c`，由 `osfmk/arm/model_dep.c` 的 `Debugger()` 末尾调用）：
  panic 时在**屏幕最下方**打印 `LIUQIN PANIC SUMMARY` → 根分区名/块数/是否 rw + 日志环最后 2 KiB 的文本
  → `LIUQIN PANIC SUMMARY END`。因为它是最后打印的，拍照一定拍得到，且含 loader 自己 stderr 的行。
  payload `shim-boot-touch82-summary.img`（sha256 `222c486c…`）。

- **2026-10-26 触摸第 118 步：为"拍屏"做一版内核——initproc 退出不再 panic + 打开屏幕文字（touch81-onpanic）**：
  用户那边日志拉不出来，改用拍照。但默认 panic 之后整屏都是 panic dump，loader 的报错会被顶掉。
  新内核改动（`bsd/kern/kern_exit.c`）：启动参数 **`liuqininitnopanic=1`** 时，initproc 退出**不 panic**，
  只打印 `LIUQIN PANIC: initproc exit suppressed (liuqininitnopanic=1), the machine stays up; see the lines above`，
  于是屏幕会停在失败前最后打印的内容上，直接拍照即可；同时日志环形区（1 秒落盘）也还留在盘上。
  该参数在 `liuqin_init_nopanic()` 里按需解析（内核没有 constructor，早前用 constructor 导致链接期找不到符号）。
  payload `shim-boot-touch81-onpanic.img`（sha256 `9e63d988…`）：**去掉了 `liuqinmute=1` 和
  `liuqinfb_noconsole=1`**（否则屏幕没有文字），tee 名单含 `launchd,launchd_cache_loader,backboardd,SpringBoard`。
  内核 sha256 `9e1d4878…`。

- **2026-10-26 触摸第 117 步：缓存移走也没用 → 把 `launchd_cache_loader` 的 stderr 接进日志（touch80）**：
  wrap11（缓存移走、只剩 .orig）上机仍然 `No service cache`。`launchd_cache_loader` 是以 launchd 自己的
  进程跑的（SETEXEC），它失败 = initproc 退出 = panic，而它的 stderr 被丢进 /dev/null（我们看不到原因）。
  另外查了缓存的 `SystemLibraryTreeState`：里面**只记录 framework/二进制**（没有 LaunchDaemons 的 plist），
  所以"plist 被我们改过"并不会触发重建——重建的触发条件另有其因，必须听 loader 自己说。
  touch80：`liuqinnull=launchd,launchd_cache_loader,backboardd,SpringBoard`，把 loader 的报错收进日志。
  payload `shim/out/shim-boot-touch80-hfs.img`（sha256 `32667eda…`）。

- **2026-10-26 触摸第 116 步：`No service cache` 的机制——launchd 触发缓存重建，重建要"覆盖"753 KB 文件，撞上 HFS 截断 bug（wrap11 把缓存移走）**：
  panic 图（hfs，55.4 s）顺序：`exec launchd` 55.211 → 一批早期 daemon → **`exec launchd_cache_loader` 55.433**
  → `initproc exited … No service cache`。即 launchd 判定缓存不可用 → 跑 loader 重建 → 失败 → 退出。
  重建 = 重写已存在的 `/System/Library/xpc/launchd.plist`（753,133 B）→ 正是我们已知的 HFS 驱动 bug：
  **truncate/覆盖已存在文件会失败并弄脏卷**（launcher 早前 errno 5/22、`hfs: error truncating data fork!`）。
  userdata 上能过，是因为那边的重建曾经写成功（或没触发），而全新 `hfs` 上必挂。
  修法（实验）：把缓存**移走**（`launchd.plist` → `launchd.plist.orig`，`.sig` 同理），让 loader 只能**新建**
  文件、走不到 truncate 路径。
  镜像 `~/liuqin-root/ipad71-root-wrap11.img`（sha256 `6ab05f60…`）；
  稀疏（基准 wrap10）`~/liuqin-root/ipad71-root-wrap10-to-wrap11.simg`（sha256 `6d165e1e…`，64 块）；
  配 `shim-boot-touch79-hfs.img`（`ab6e4fb9…`，挂载后立即打印探针）。

- **2026-10-26 换分区：`hfs` = `/dev/block/sda36`（32 GiB），日志拉取修正**：
  分区表确认：`userdata -> sda35`（202,340,332 KiB ≈ 193 GiB）、**`hfs -> sda36`（33,554,432 KiB = 32 GiB）** ✓。
  之前拉到的"hfs 日志"其实是 `userdata` 的（内容里只有 `LIUQIN UFS: "userdata"`、无 `hfs` 字样）：
  recovery 的 toybox `readlink` **不支持 `-f`**，脚本的探测循环因此跳过了 hfs。
  `tools/liuqin-log-pull.sh` 改成：`dev=${DEV:-}` 可用环境变量直指设备节点；否则按
  `hfs` → `userdata` 找 by-name 链接并取 `readlink`（不带 -f）的结果。
  用法：`DEV=/dev/block/sda36 tools/liuqin-log-pull.sh /tmp/log-hfs.txt`。

- **2026-10-26 触摸第 115 步：hfs 分区上 launchd 报 `No service cache` 后退出（panic）；探针提前到挂载后**：
  整包刷 `hfs` 后启动：跑到 `exec launchd_cache_loader at 50.156 s` 之后 panic：
  `initproc exited -- exit reason namespace 7 subcode 0x1 description: No service cache`
  （namespace **7 = OS_REASON_LIBXPC**）。镜像里 `/System/Library/xpc/launchd.plist` 是**存在**的
  （753,133 B，10-25 22:21 我们改过的时间戳，`.sig` 是原版 9-2-2023），
  `/private/var/db/com.apple.xpc.launchd/config` 也在 → 不是"文件缺失"，更像设备上的副本不完整或树状态不匹配
  导致重建失败。
  为拿到 panic 前的证据：`liuqin_touch_probe_files()`（新导出，`bsd/dev/liuqin_touch.c`）在
  **`liuqin_log_start()` 之后立刻调用**（`bsd/dev/liuqin_ufs.c`），探针列表也加上
  `/System/Library/xpc/launchd.plist` 与 `/usr/libexec/backboardd`——挂载后 1 秒内落盘，
  panic 也带得走。
  内核 sha256 `8025a452…`；payload `shim-boot-touch79-hfs.img`（sha256 `ab6e4fb9…`）。

- **2026-10-26 换分区（续）：`hfs` 给了 32 GiB，日志区安全；日志拉取脚本已改**：
  `hfs` = 32 GiB，持久日志区落在 `hfs` 末尾（32 GiB − 4 MiB），不碰文件系统 ✓。
  `tools/liuqin-log-pull.sh` 现在按顺序找 `hfs` → `userdata` 分区（recovery ADB 里
  `/dev/block/by-name/hfs` 或 `/dev/block/bootdevice/by-name/hfs`），报错信息也改成两者都提。
  刷 `hfs` 第一次必须整包：`fastboot -S 512M flash hfs ~/liuqin-root/ipad71-root-wrap10.img`，
  然后 `fastboot boot shim/out/shim-boot-touch78-hfs.img`（`3e691556…`）。
  开机确认 `LIUQIN UFS: "hfs" on LUN 0: … read-write …`。

- **2026-10-26 换分区：root 改到 userdata 后面新建的 `hfs` 分区（payload touch78-hfs）**：
  内核本来就按 GPT 分区名找 root（`ufsroot=` 启动参数，默认 `userdata`，见 `bsd/dev/liuqin_ufs.c`
  的 `PE_parse_boot_argn("ufsroot", ...)` 与 `ufs_find_partition`），所以换分区只需改启动行：
  `rd=ufsroot0 ufsroot=hfs`。固件区 `LIUQINFW` 来自 payload 的内存映射（`liuqin_fw_load()` 打印
  "LIUQIN FW: N files at 0x…"），不落在分区上，因此换分区不影响固件。
  **但持久日志区在 root 分区的最后 (4 MiB + 4 KiB)**（`liuqin_log_thread`: `su.first_block +
  su.nblocks - LIUQIN_LOG_BLOCKS`），所以 `hfs` 必须比镜像大约 5 MiB 以上；若 `hfs` 恰好 16 GiB，
  日志会压在文件系统尾部 → 又损坏，届时要加 `liuqinlog=0`（代价是没有日志）。
  镜像 `shim/out/shim-boot-touch78-hfs.img`（sha256 `3e691556…`）；刷分区用
  `fastboot -S 512M flash hfs <img>`，稀疏包同样可以刷到 `hfs`（块偏移是分区内的）。

- **2026-10-26 触摸第 114 步：这一轮 launchd 连 backboardd 都没拉起来（无 exec 记录）；加"第一行"探针（wrap10 + touch77）**：
  wrap9 上机：HFS 挂载**干净**（`hfs_recording_init: creating the hotfile btree`，无 corruption 日志），
  内核探针也读到 `/usr/lib/libliuqintouch.dylib 52904` 与 plist 985——但整轮 **1231 条 exec 里没有
  `exec backboardd`**，`|backboardd|` 一条也没有，而 **SpringBoard 崩了 102 次**（它等不到窗口服务器）。
  也就是说 launchd 没把 backboardd 任务跑起来，而且没有可见报错（launchd 自身 stderr 不在 tee 名单）。
  wrap10：launcher 的 `main()` **第一句**就打印 `LIUQIN LAUNCHER: started (uid/pid/ppid)`；
  payload `touch77` 把 tee 名单改成 `launchd,backboardd,SpringBoard`，这样 launchd 的抱怨也能看到。
  launcher `shim/out/liuqin-backboardd`（sha256 `cf64ce2a…`）；
  镜像 `~/liuqin-root/ipad71-root-wrap10.img`（sha256 `c2e6f2ca…`）；
  稀疏（基准 wrap9）`~/liuqin-root/ipad71-root-wrap9-to-wrap10.simg`（sha256 `7426dcaf…`，72 块）；
  payload `shim/out/shim-boot-touch77.img`（sha256 `09d5c382…`）。

- **2026-10-26 触摸第 113 步：根因是"HFS 驱动覆盖已有文件（truncate）会损坏卷"（wrap9 改用唯一文件名）**：
  wrap7 上机把 errno 打出来了：`/private/var/mobile/Library/Caches/... (errno 5 = EIO)`、
  `/private/var/tmp`、`/var/tmp`、`/tmp` 全是 `errno 22 = EINVAL`。同一轮内核日志里有
  `hfs: error truncating data fork!`、`BlockMarkFreeInternal() trying to free unallocated blocks`、
  `hfs_mark_inconsistent: Runtime corruption detected on iOSRoot, fsck will be forced` ——
  **截断/覆盖已有文件会让本移植的 HFS 驱动把卷标脏**。时间线也吻合：wrap4 是**新建**文件（成功、FNV 正确），
  wrap5 起都是**覆盖**（失败/写坏）。
  用 `fsck_hfs -fy` 检查镜像本身是 **OK** 的，所以损坏发生在运行时。
  wrap9 的 launcher：**每次用带 pid 的唯一文件名** + `O_CREAT|O_EXCL`（绝不 truncate），写完 `fsync`
  再**回读比对 FNV**，不一致就不插入；目录候选仍是 mobile 的 Caches → `/private/var/tmp` → `/var/tmp` → `/tmp`。
  launcher `shim/out/liuqin-backboardd`（sha256 `fe52e0b6…`）；
  镜像 `~/liuqin-root/ipad71-root-wrap9.img`（sha256 `7f5031c1…`，**已用 fsck_hfs 验证 OK**）；
  稀疏（基准 wrap8）`~/liuqin-root/ipad71-root-wrap8-to-wrap9.simg`（sha256 `2b7188af…`，69 块）。
  注意：设备上的卷可能已被标脏，**建议整包刷 wrap9.img**（它刚 fsck 过是干净的）。

- **2026-10-26 触摸第 112 步：写文件成功但回读内容不同——加"内嵌 vs 文件"逐字节对拍（wrap7）**：
  wrap6 上机：launcher 这次**写成功了**（`/private/var/mobile/Library/Caches/libliuqintouch.dylib`），
  但回读首 4096 字节 **FNV `7fc402c8`**，而内嵌副本（本机核对：二进制里就是 `0a50e64b`、与库逐字节相同）
  应该是 `0a50e64b`。同时 `inserting` 那行打印成了空路径（顺序问题，已修）。
  wrap7：launcher 增打 `embedded first bytes …` / `file first bytes …` / `embedded FNV … file FNV … file size …`，
  用来判断是"写进去的字节不对"还是"读到的是别的文件"。
  launcher `shim/out/liuqin-backboardd`（sha256 `316293e7…`）；
  镜像 `~/liuqin-root/ipad71-root-wrap7.img`（sha256 `3f2ec28a…`）；
  稀疏（基准 wrap6）`~/liuqin-root/ipad71-root-wrap6-to-wrap7.simg`（sha256 `04029462…`，51 块）。

- **2026-10-26 触摸第 111 步：连写库都失败了——`/private/var/tmp` 不可用（wrap6 多路径回退 + errno）**：
  wrap5 上机：launcher 打印 `cannot create /private/var/tmp/libliuqintouch.dylib`（wrap4 那轮却成功，
  说明该目录这次不可写/不存在）。改成**多候选路径**（mobile 自己的 `Library/Caches` 优先，然后
  `/private/var/tmp`、`/var/tmp`、`/tmp`），每个失败都打印 `errno`，成功则记下路径用于 INSERT。
  launcher `shim/out/liuqin-backboardd`（sha256 `51d2979b…`）；
  镜像 `~/liuqin-root/ipad71-root-wrap6.img`（sha256 `7ca4fc1f…`）；
  稀疏（基准 wrap5）`~/liuqin-root/ipad71-root-wrap5-to-wrap6.simg`（sha256 `e769e601…`，69 块）。

- **2026-10-26 触摸第 110 步：库内容对了、backboardd 不再崩，但 dyld 静默跳过了插入库（wrap5 问原因）**：
  wrap4 上机（整包刷 + erase）：launcher 自写库并自校验 **`52904 bytes, first-4096 FNV 0a50e64b,
  stub-reference no`**，`EXECENV backboardd: DYLD_INSERT_LIBRARIES=/private/var/tmp/libliuqintouch.dylib`，
  **backboardd 零崩溃**。但 dyld 的 `DYLD_PRINT_LIBRARIES` 输出里始终没有这个库，也没有任何错误——
  原因是我们给 AMFI 的回答置了 **bit 5（ALLOW_FAILED_LIBRARY_INSERTION）**，dyld 于是**静默忽略**加载失败
  （读过 dyld 源码：`allowInsertFailures` 为真时不打印、不终止）。
  同一份库放在 `/usr/lib` 时是能加载的（早前 dyld 报的是它的依赖缺失，证明它被加载过）→ 差异在**路径/属主**。
  wrap5：launcher 里加一次 `dlopen(库, RTLD_NOW)` 并打印 `dlerror()`，把拒绝原因问出来；
  同时保留内嵌库 + 自写 + 自校验。
  launcher `shim/out/liuqin-backboardd`（sha256 `b9514eb6…`，118,7xx B）；
  镜像 `~/liuqin-root/ipad71-root-wrap5.img`（sha256 `8cc1d232…`）；
  稀疏（基准 wrap4）`~/liuqin-root/ipad71-root-wrap4-to-wrap5.simg`（sha256 `a126c8e5…`，50 块）。

- **2026-10-26 触摸第 109 步：库改为**内嵌在 launcher 里**（wrap4），不再依赖镜像里的文件**：
  用户反馈"上次就是整包刷的"，但设备上的 dylib 仍是 `FNV b1f2ddf1`（不是本机的 `0a50e64b`）——
  分区 225 GiB、镜像只 16 GiB，排除"超出分区"，更像是整包刷写没写全（`-S 512M` 分块，尾部未写）。
  另外 `forcefix` 那个"改坏基准"的 sparse 把**HFS+ 元数据/日志块**也带上了，导致 userdata 不一致——
  **这条技巧作废**（sparse 绝不能包含元数据块）。
  新做法：`host/liuqin_dylib_blob.py` 把 `libliuqintouch.dylib` 生成 C 数组 `host/liuqin_dylib_blob.h`，
  launcher 启动时把它写到 `/private/var/tmp/libliuqintouch.dylib`（root 可写、mobile 也可写），
  再 `setenv("DYLD_INSERT_LIBRARIES", 那个路径)` 并 `execv` 真 backboardd。
  于是**只要 launcher 这一个文件更新到位**，库的内容就对了（launcher 已多次证明能更新）。
  launcher 现在 118,640 B（sha256 `cb554b78…`，内含 52,904 B 的库，首 4 KiB FNV `0a50e64b`）。
  镜像 `~/liuqin-root/ipad71-root-wrap4.img`（sha256 `689dc61f…`）——**建议整包刷**，并留意 fastboot 每块是否成功。

- **2026-10-25 触摸第 108 步：wrapper 已在跑、环境变量已到，但 dyld 仍在抱怨旧 UUID——让 launcher 自证文件（wrap3）**：
  wrap2 上机：wrapper 生效（`LIUQIN LAUNCHER: uid 501, inserting …`、
  `EXECENV backboardd: DYLD_INSERT_LIBRARIES=…`），但 backboardd 仍然崩溃 63 次，理由都是
  `Library not loaded: /usr/lib/libLiUqinHIDPrivate.dylib`，且 "Referenced from" 的 UUID 是
  **`CF0D48F4…`**——而镜像里 dylib 的 UUID 是 **`D9606C5E…`**（本机 `otool -L` 也确认已无 stub 依赖，
  大小 52,904、首 4 KiB FNV `0a50e64b`、不含 `libLiUqinHIDPrivate` 字符串）。内核在设备上读到的也是
  52,904 字节。两边对不上，说明 dyld 加载的不是镜像里那个文件（或日志来自上一轮）。
  wrap3：launcher 在 exec 前**自己校验并打印**该文件——`<size> bytes, first-4096 FNV <hash>,
  stub-reference yes/no`，若发现仍带 `libLiUqinHIDPrivate` 字符串就**不插入**（避免 backboardd 崩溃循环）。
  launcher：`shim/out/liuqin-backboardd`（sha256 `9afc390b…`）。
  镜像 `~/liuqin-root/ipad71-root-wrap3.img`（sha256 `48734561…`）；稀疏（基准 wrap2）
  `~/liuqin-root/ipad71-root-wrap2-to-wrap3.simg`（sha256 `1f1e0861…`，64 块 / 262 KB）。

- **2026-10-25 触摸第 107 步：wrapper 成功注入，但 dylib 依赖了链接用的 stub —— 改成 dlsym（wrap2）**：
  wrap 镜像上机：**wrapper 生效**——`|backboardd| LIUQIN LAUNCHER: uid 501, inserting
  /usr/lib/libliuqintouch.dylib into /usr/libexec/real/backboardd`，随后
  `EXECENV backboardd: DYLD_INSERT_LIBRARIES=/usr/lib/libliuqintouch.dylib`（**环境变量终于到了**）。
  但 backboardd 立刻崩：`CRASH backboardd reason ns 6 code 0x1: Library not loaded:
  /usr/lib/libLiUqinHIDPrivate.dylib` —— 我们为链接手造的那个 stub `.tbd` 被写成了 dylib 的
  **`LC_LOAD_DYLIB` 运行时依赖**（`otool -L` 里能看到），于是每次加载都失败、backboardd 崩溃重启 72 次。
  修法：dylib 不再链接任何私有库，改为**运行时 `dlsym`** 解析那 10 个 IOKit 私有函数
  （`IOHIDEventSystemClientCreateWithType`、`IOHIDVirtualServiceClient*`、`IOHIDEventCreateDigitizer*` 等；
  先 `dlopen` IOKit.framework，失败再退到 `RTLD_DEFAULT`），任一符号缺失就打印并放弃。
  改后 `otool -L` 只剩 CoreFoundation 和 libSystem。新 dylib sha256 `b807830e…`。
  镜像：`~/liuqin-root/ipad71-root-wrap2.img`（sha256 `ff409bbe…`）；
  稀疏（基准 wrap）：`~/liuqin-root/ipad71-root-wrap-to-wrap2.simg`（sha256 `2bb8a84f…`，62 块 / 254 KB）。
  payload 仍用 `shim-boot-touch76.img`（`c8616f59…`）。

- **2026-10-25 触摸第 106 步：照 ios-topaz 的做法——wrapper 占住原路径，缓存完全不动（touch76 + wrap 镜像）**：
  看了 `wlw333554/ios-topaz`（默认分支 **evergo**，Redmi Note 12 4G 的 ios-sweet 移植，**触摸/按键/Wi-Fi 都可用**；
  `MakrSas/ios-topaz` 只是 sweet 的旧快照）。它的注入方式写在 `userland/gpu/bbd_wrap.c` 注释里：
  > launchd does not pass DYLD_* variables from a job to a system daemon, so this stands in for the
  > window server: it adds our loader to DYLD_INSERT_LIBRARIES and execs the real one
  > (/usr/libexec/real/backboardd: same name, same pid, same Mach services launchd prepared)
  即：**真 backboardd 移到 `/usr/libexec/real/backboardd`，wrapper 占住 `/usr/libexec/backboardd`
  原路径**——launchd 的 job（和缓存）完全不用改。这比我们之前"改缓存 ProgramArguments"稳得多，
  也和"环境变量被 posix_spawn 摘掉"的现实相容（wrapper 用 execv，不过 posix_spawn）。
  我们照此实现（保留"exec 失败就睡眠"的保险，避免 init 消失导致 panic）：
  - wrapper：`shim/out/liuqin-backboardd`（68,496 B，sha256 `79567994…`，INSERT=`/usr/lib/libliuqintouch.dylib`）
  - 镜像 `~/liuqin-root/ipad71-root-wrap.img`（sha256 `1bdb514b…`）：`/usr/libexec/real/backboardd` = 真身，
    `/usr/libexec/backboardd` = wrapper，`/usr/lib/libliuqintouch.dylib` 就位，**缓存恢复原样**
    （`ProgramArguments` = `/usr/libexec/backboardd`，无 `EnvironmentVariables`）。
  - 稀疏差（两个基准都给）：`ipad71-root-env-to-wrap.simg`（sha256 `03214bc2…`，580 块）与
    `ipad71-root-exec2-to-wrap.simg`（sha256 `e464f032…`，748 块）。
  - payload `shim-boot-touch76.img`（sha256 `c8616f59…`），tee 名单 `backboardd,SpringBoard,real/backboardd`。

- **2026-10-25 触摸第 105 步：exec → exec2 的稀疏差分**：
  `tools/sparsediff.py ipad71-root-exec.img ipad71-root-exec2.img ipad71-root-exec-to-exec2.simg`
  → `~/liuqin-root/ipad71-root-exec-to-exec2.simg`（**258,316 B / 63 块**，sha256 `a8aa1022…`）。
  只写 `/usr/libexec/liuqin-backboardd` 那一个文件（launcher 从"失败即退出"改成"失败睡眠"）。
  **基准必须是 `ipad71-root-exec.img`**。

- **2026-10-25 触摸第 104 步：panic 的真正机制——launchd 用 SETEXEC，任务进程就是 pid 1**：
  exec 版上机又 panic（`namespace 9 code 0x1` = EXEC/BAD_MACHO，"SIGKILL of init"）。结合 launchd 源码，
  机制终于清楚：`job_start_child()` 用 **`POSIX_SPAWN_SETEXEC`**，所以 launchd 把**自己的进程**交给任务
  ——缓存里 backboardd 的 ProgramArguments 换成我们的 launcher 后，**launcher 就是 pid 1**；
  它一旦 exec 失败并返回，init 就消失，内核立刻 panic（而且是"执行 exec 的进程"记 EXEC/BAD_MACHO）。
  改法：launcher 在 `execve` 失败时**不退出**，打印 errno 后 `sleep(60)` 循环——睡眠的 init 不会 panic，
  日志（1 秒落盘）就能保住失败原因。
  新 launcher：`shim/out/liuqin-backboardd`（68,496 B，sha256 `1587154b…`）。
  镜像：`~/liuqin-root/ipad71-root-exec2.img`（sha256 `c16010b2…`）；
  稀疏（基准 env）：`~/liuqin-root/ipad71-root-env-to-exec2.simg`（sha256 `7cec6f9c…`，285 块 / 1 MiB）。
  payload 用 `shim-boot-touch75.img`（`5690a31e…`，tee 名单已含 liuqin-backboardd）。
  判读：若出现 `LIUQIN LAUNCHER: execve(/usr/libexec/backboardd) failed, errno N` → 按 errno 查
  （EPERM/EACCES 多为签名/平台限制；ENOEXEC 为 Mach-O 问题）；若 exec 成功则继续看 dyld/LIUQIN INPUT。

- **2026-10-25 触摸第 103 步：env → exec 的稀疏差分（供"设备当前是 env 版"时秒刷）**：
  `tools/sparsediff.py ipad71-root-env.img ipad71-root-exec.img ipad71-root-env-to-exec.simg`
  → `~/liuqin-root/ipad71-root-env-to-exec.simg`（**930,204 B / 227 块**，sha256 `fd9eb48d…`）。
  差分内容：新增 `/usr/libexec/liuqin-backboardd`、缓存里 backboardd 的 `ProgramArguments` 改成它
  （dylib 两版相同，不写）。**基准必须是 `ipad71-root-env.img` 那一版**，否则会出现之前的"混血"。

- **2026-10-25 触摸第 102 步：直接上 exec 版 launcher（touch75 + exec 镜像）**：
  按用户要求跳过"只打印"的安全版，直接把缓存里 backboardd 的 `ProgramArguments` 换成
  `/usr/libexec/liuqin-backboardd`（launcher，68,400 B，sha256 `1c224f0a…`），它 `setenv`
  两个 `DYLD_*` 后 `execve("/usr/libexec/backboardd", {"/usr/libexec/backboardd", …}, environ)`，
  失败会打 `LIUQIN LAUNCHER: execve failed, errno N`；缓存里同时保留 `KeepAlive=true`、
  `_PanicOnCrash` 改名、env 只留 `DYLD_PRINT_LIBRARIES`。dylib 仍在 `/usr/lib`。
  - 完整 `~/liuqin-root/ipad71-root-exec.img`（sha256 `8ce54341…`）
  - 稀疏 `~/liuqin-root/ipad71-root-exec.simg`（sha256 `644d3067…`，269 块 / 1 MiB，基准 = base.img）
  - payload `shim-boot-touch75.img`（sha256 `5690a31e…`），tee 名单 `backboardd,SpringBoard,liuqin-backboardd`
  判读：`EXECENV backboardd` 里应出现 `DYLD_INSERT_LIBRARIES=…`（因为 launcher 用 execve 而非
  posix_spawn），随后 `|backboardd| dyld: loaded: /usr/lib/libliuqintouch.dylib` 与 `LIUQIN INPUT: …`；
  若 `LIUQIN LAUNCHER: execve failed` 则看 errno；若 panic 则说明 adhoc 执行不被允许 → 转路 B。

- **2026-10-25 触摸第 101 步：缓存生效了，但 `DYLD_INSERT_LIBRARIES` 被单独滤掉——改走缓存里的 launcher（touch74 + ltest 镜像）**：
  touch73 + env 镜像上机：**`EXECENV backboardd: vars=8 dyld=1`**，里面有
  `DYLD_PRINT_LIBRARIES=1`（来自缓存），但 **`DYLD_INSERT_LIBRARIES` 不在**——说明 launchd 的
  `setenv`（core.c 的 `SLIST_FOREACH(ei, &j->env)`）并没有过滤，是 **libsystem 的 `posix_spawn`
  在 spawn 时把它摘掉了**（其它 `DYLD_*` 不受影响）。所以"环境变量注入"这条路对 INSERT 无效。
  下一步：**通过缓存把 `ProgramArguments` 指到我们自己的 launcher**（缓存已被证明生效）。为降低风险先做
  只打印、不 exec 的安全版：`host/liuqin_launcher_test.c` → `/usr/libexec/liuqin-launcher-test`
  （sha256 `3f55d553…`），缓存里把 backboardd 的 `ProgramArguments` 换成它、`KeepAlive=false`。
  - 镜像 `~/liuqin-root/ipad71-root-ltest.img`（sha256 `bafd31b8…`）/ 稀疏 `ipad71-root-ltest.simg`
    （sha256 `00c6bb9b…`，260 块 / 1 MiB，基准 = base.img）
  - payload `shim-boot-touch74.img`（sha256 `3b740d0d…`），启动行 tee 名单加了 `liuqin-launcher-test`
  判读：日志出现 `|liuqin-launcher-test| LIUQIN LAUNCHER: alive` → **缓存能启动我们的 adhoc 程序**，
  下一步就换成真正的 launcher（setenv + execve backboardd）；若无输出/panic → adhoc 执行不被允许，
  转路 B（内核 IOHIDFamily 移植）。

- **2026-10-25 触摸第 100 步：真相——launchd 读的是 `/System/Library/xpc/launchd.plist` 缓存，不是 LaunchDaemons 里的 plist**：
  touch73 + probe 镜像上机：`LIUQIN IMAGE: built Oct 25 2026 19:33:35`（确认是 touch71 内核那一轮）、
  启动行里 `liuqinnull=…,liuqin-probe` 与 `ufslogsecs=1` 都生效，但**没有 `exec liuqin-probe`、也没有
  `LIUQIN PROBE`**，而且 launchd 只有 35 行日志、一句都没提这个 job。
  查 root：`/System/Library/xpc/launchd.plist`（+`.sig`）是一份 **505 个 job 的缓存**，按 plist 路径索引，
  backboardd 的条目里就有 `ProgramArguments`/`MachServices`/`UserName`——**launchd 用的是这份缓存**
  （启动行的 `launchd_unsecure_cache=1` 就是让它忽略 `.sig`）。这解释了此前所有怪现象：
  改 `/System/Library/LaunchDaemons/*.plist` **从来没被用到**（环境变量注入无效、ProgramArguments 改动无效，
  连那次 panic 也不能归因于 launcher job）。
  移植里已有工具 `tools/patch-launchd-plist.py` 就是干这个的。
  新镜像：**只改了缓存里 backboardd 的 `EnvironmentVariables`**（`DYLD_INSERT_LIBRARIES` +
  `DYLD_PRINT_LIBRARIES`），`ProgramArguments` 保持原样（不换程序 → 没有 init 被杀风险），
  dylib 照旧放 `/usr/lib`；顺手把该 job 的 `_PanicOnCrash` 改名以免它崩溃时连带 panic。
  - 完整：`~/liuqin-root/ipad71-root-env.img`（sha256 `7b62f3b4…`）
  - 稀疏：`~/liuqin-root/ipad71-root-env.simg`（sha256 `4e027abf…`，264 块 / 1.0 MiB，基准 = base.img）
  刷 `fastboot -S 512M flash userdata ~/liuqin-root/ipad71-root-env.simg`（若设备当前不是 base，则刷完整
  `ipad71-root-env.img`），再 boot `shim-boot-touch73.img`（`d942825a…`，1 秒落盘 + tee 名单）。

- **2026-10-25 触摸第 99 步：路 A 的判别实验——launchd 能不能跑我们的 adhoc 二进制（touch73 + probe 镜像）**：
  只加**自己的**一个 job，不碰任何系统 job：`com.liuqin.probe` → `/usr/libexec/liuqin-probe`
  （68,288 B，sha256 `7b77a11c…`，ad-hoc 签名）。程序只做两件事：往 stderr 打
  `LIUQIN PROBE: alive (uid … pid …)`、写 `/private/var/tmp/liuqin-probe-ok`。
  job plist：`UserName=root`、`RunAtLoad`、`POSIXSpawnType=Interactive`。
  镜像（都从 `touch3.img` 剥掉注入改动重建的基线克隆而来）：
  - 基线 `~/liuqin-root/ipad71-root-base.img`（sha256 `53b3850f…`）
  - 探针 `~/liuqin-root/ipad71-root-probe.img`（sha256 `fe284851…`）
  - 稀疏差 `~/liuqin-root/ipad71-root-probe.simg`（sha256 `b260b056…`，79 块）
  **前提**：稀疏差只对"设备当前 == base.img"有效；若不确定，直接刷完整 probe.img。
  payload `shim-boot-touch73.img`（sha256 `d942825a…`）在启动行加了
  `liuqinnull=backboardd,SpringBoard,liuqin-probe`（让 devnull tee 捕获探针的 stderr）与 `ufslogsecs=1`。
  判读：日志出现 `|liuqin-probe| LIUQIN PROBE: alive` → **adhoc 二进制可执行，路 A 可行**；
  若 panic（又是 `EXEC_EXIT_REASON_BAD_MACHO`）→ 这条路要换签名方式（ldid + platform identifier）；
  若无声无息 → launchd 没认这个 job。

- **2026-10-25 触摸第 98 步：panic 那轮的日志为什么拉不到（机制）＋ touch72 把间隔压到 1 秒**：
  console 环在 **RAM** 里，落盘是周期性（`ufslogsecs`）的；panic 会立刻停机，**panic 那几秒的 console
  （含 panic 文本本身）永远留在内存里**，所以恢复后拉到的永远是"上一次启动"的内容。之前更糟：日志线程
  **先睡后写**，而 panic 发生在首次写入之前 → 整轮零字（已在 0218 修成"启动即写"）。
  touch72 = touch71 内核 + `ufslogsecs=1`（落盘间隔 1 秒），把 panic 前的可捕获窗口压到最小。
  镜像 `shim/out/shim-boot-touch72.img`（sha256 `0bce2144…`，payload `e7444d26…`）。
  注意：**panic 文本本身仍然拉不到**（只能拍屏幕，就像这次的照片）；能拉到的是 panic 前 ≤1 秒的 console，
  那里面会有 launchd 起任务/exec 失败之类的前因。

- **2026-10-25 触摸第 97 步：链式稀疏镜像把 root 搞成混血——重建干净基线（必读）**：
  写回 `touch3.simg` 后**仍然 panic**。原因不是 touch3 本身：`touch3.simg` 是 **touch2→touch3 的差分**，
  而设备当时是 **touch4**（launcher 版）。稀疏镜像里的 DONT_CARE 块"不写"，于是 touch4 独有的块
  （launcher 文件、其目录项）**留在原地**，同时 plist 被写回 touch3 版本 → launchd 执行到半坏的
  launcher → `EXEC_EXIT_REASON_BAD_MACHO` → init 被杀 → panic。
  **教训：稀疏镜像只能刷在它差分所对应的那个状态上**；状态不明时必须刷完整镜像。
  已重建干净基线（= 原始配置：无 dylib、plist 无 EnvironmentVariables、ProgramArguments 原样）：
  - 完整：`~/liuqin-root/ipad71-root-nodaemon.img`（17 GiB，sha256 `31fe61fb…`）
  - 稀疏（零基准，相当于整包）：`~/liuqin-root/ipad71-root-nodaemon-restore.simg`
    （10.7 GiB，sha256 `105d3045…`，2,634,646/4,194,304 块）
  恢复：`fastboot -S 512M flash userdata ~/liuqin-root/ipad71-root-nodaemon-restore.simg`
  然后 boot `shim/out/shim-boot-touch71.img`（sha256 `cacf9191…`，带"启动即落盘"的日志修复）。
  今后规矩：**每个实验都从完整镜像克隆（`cp -c`）并单独保留**，稀疏包只用于"当前设备状态 == 该包基准"
  的情况。

- **2026-10-25 触摸第 96 步：launcher 版 panic，而且那轮日志没落盘——先修日志盲区（touch71）**：
  touch4（launcher 换掉 backboardd 的 ProgramArguments）上机 **panic**：
  `unexpected SIGKILL of init with reason -- namespace 9 code 0x1`
  = `OS_REASON_EXEC` + **`EXEC_EXIT_REASON_BAD_MACHO`**：init（launchd）在一次 exec 中因目标被判"坏 Mach-O"
  而自己被杀（launchd 用 `POSIX_SPAWN_SETEXEC` 起任务，失败会落在执行 exec 的进程上）。
  而且**那轮 panic 的日志没有落盘**：panic 发生在 "root device ready at 47.339 s" 之后几秒，而日志线程
  原本是**先睡 2 秒再写**，第一次写入之前就 panic 了。
  修（补丁 `0218-liuqin-log-flush-immediately.patch`）：把 `tsleep` 挪到"没有新内容"分支里，**启动时立即写一次**，
  之后再按 `ufslogsecs` 周期；这样哪怕挂载后 1 秒就 panic，console 也已经在盘上。
  镜像 `shim/out/shim-boot-touch71.img`（sha256 `cacf9191…`，payload `7ad4438e…`）——**已编译，未上机**。
  恢复用 touch3 的 root 稀疏包（`af86b73a…` 是 touch4；touch3 是 `9b4938aa…`），
  即 `fastboot -S 512M flash userdata ~/liuqin-root/ipad71-root-touch3.simg`，然后 boot touch71。

- **2026-10-25 触摸第 95 步：launchd 确实忽略 EnvironmentVariables——改用 launcher 包装（touch4 镜像 + touch70）**：
  touch69 上机（环境计数阳性对照有效）：`EXECENV mount: vars=3 dyld=0`、
  **`EXECENV backboardd: vars=7 dyld=0`**（7 个都是 launchd 自己加的 XPC_*/PATH）。
  所以 iOS 的 launchd **不把任务的 `EnvironmentVariables` 传给子进程**，环境变量注入这条路走不通。
  改用 launcher：新程序 `host/liuqin_backboardd_launcher.c`（arm64 iOS，ad-hoc 签名，68,320 B，
  sha256 `c22886f6…`）在自身进程里 `setenv("DYLD_INSERT_LIBRARIES", …)` 后
  `execve("/usr/libexec/backboardd", {"/usr/libexec/backboardd", …}, environ)`——pid/argv[0]/MachServices
  都不变，而 dyld 终于能看到变量。
  root 镜像 `ipad71-root-touch4.img`（sha256 `5f98044b…`）：装入 `/usr/libexec/liuqin-backboardd`，
  把 `com.apple.backboardd.plist` 的 `ProgramArguments[0]` 指向它并删掉 `EnvironmentVariables`；
  稀疏包 `ipad71-root-touch4.simg`（**409,964 B / 100 块**，sha256 `af86b73a…`）。
  刷：`fastboot -S 512M flash userdata ~/liuqin-root/ipad71-root-touch4.simg`，再 boot
  `shim-boot-touch70.img`（sha256 `f1e00bd6…`，带镜像横幅与环境计数）。
  验证：`EXECENV backboardd` 现在应出现 `DYLD_INSERT_LIBRARIES=…`；随后看
  `|backboardd| dyld: loaded: /usr/lib/libliuqintouch.dylib` 与 `LIUQIN INPUT:`。
  若开机没有 UI（launcher 的 exec 失败），用 touch3 的稀疏包 `9b4938aa…` 回滚即可。

- **2026-10-25 触摸第 94 步：touch70 = 环境计数 + 镜像自识别横幅**：
  补丁 `0216-liuqin-exec-env-count.patch`（每个 exec 打 `EXECENV <name>: span=…` / `vars=N dyld=M`
  和前三个环境串）＋ `0217-liuqin-image-banner.patch`（`liuqin_input_init` 打印
  `LIUQIN IMAGE: built <日期> <时间>`）。
  横幅解决最近两轮的混淆：拉回来的日志到底是不是新镜像跑的，一眼可辨。
  镜像 `shim/out/shim-boot-touch70.img`（sha256 `f1e00bd6…`，payload `21a71be9…`）——**已编译，未上机**；
  root 用 `ipad71-root-touch3.simg`（`9b4938aa…`）那版即可，只换 boot 镜像。
  另外记录日志落盘节奏（问过）：UFS 分区可写后（开机 1–3 s）启动写线程，每 `ufslogsecs` 秒
  （默认 10，我们启动行是 2）把"自上次以来新增"的字节写到最后 4 MiB + 4 KiB；只保留最近 4 MiB。

- **2026-10-25 触摸第 93 步：exec 环境里连一个 DYLD_ 都没有——先确认扫描本身有效（touch69）**：
  touch68 上机：`EXECENV` **0 行**（连别的进程也没有），而 `exec backboardd` 正常打印。两种可能：
  launchd 确实没传环境，或者我的扫描点（`imgp->ip_endargv..ip_endenvv`）在这段代码时还没填好。
  touch69（补丁 `0216-liuqin-exec-env-count.patch`）加阳性对照：每个 exec 打印
  `EXECENV <name>: span=…` / `vars=N dyld=M` 和前三个环境串，能立刻看出扫描是否有效、
  backboardd 到底收到多少个环境变量、其中有没有 `DYLD_*`。
  镜像 `shim/out/shim-boot-touch69.img`（sha256 `859d98c4…`，payload `b665dcb9…`）——**已编译，未上机**；
  只换 boot 镜像（root 仍是 touch3 那版）。

- **2026-10-25 触摸第 92 步：`|backboardd|` 仍然没有输出——改为在内核里直接看 exec 的环境（touch68）**：
  touch3 镜像上机后：`|SpringBoard|` 有 409 行，**`|backboardd|` 依然 0 行**，`LIUQIN INPUT` 也只有内核
  那两行文件检查。说明 tee 没有捕获 backboardd 的任何输出——要么它没有输出，要么它的环境里根本没有
  `DYLD_*`（dyld 就不会去问 AMFI，也不会打印任何东西）。
  touch68（补丁 `0215-liuqin-exec-env-trace.patch`）：在 `kern_exec.c` 已有的 LIUQIN exec 追踪里，
  顺着 `imgp->ip_endargv..ip_endenvv` 扫环境，把每个 `DYLD_*` 变量打出来：
  `EXECENV <proc>: DYLD_INSERT_LIBRARIES=/usr/lib/libliuqintouch.dylib`。
  这样能一次性区分：**launchd 没把 env 传给 backboardd**（打印为空）vs **传了但 dyld 剪掉/没加载**
  （有 EXECENV 行、没有 `|backboardd|` 的 dyld 输出）。
  镜像 `shim/out/shim-boot-touch68.img`（sha256 `97bd7013…`，payload `fac2f934…`）——**已编译，未上机**；
  只需换 boot 镜像（root 用 touch3 那版即可）。

- **2026-10-25 触摸第 91 步：`StandardErrorPath=/dev/console` 反而破坏了日志捕获——改回 /dev/null（touch3 镜像）**：
  查移植补丁 **0048（devnull tee）**：把 `liuqinnull` 名单（默认 `backboardd,SpringBoard`）里进程写往
  `/dev/null` 的输出抄进 console 环，前缀 `|name|`。日志里只有 `|SpringBoard|` 的 409 行、**没有
  `|backboardd|`**——因为上一版我把 backboardd 的 stderr 指到了 `/dev/console`，绕过了 tee，而
  mobile 身份又打不开 `/dev/console`，于是 dyld/库的输出整段消失。
  改回：plist 只保留 `EnvironmentVariables`（`DYLD_INSERT_LIBRARIES` + `DYLD_PRINT_LIBRARIES=1`），
  删掉 `StandardErrorPath`/`StandardOutPath`，让输出回到 `/dev/null` 被 tee 捕获。
  镜像：`~/liuqin-root/ipad71-root-touch3.img`（sha256 `055a744a…`）与
  `~/liuqin-root/ipad71-root-touch3.simg`（sha256 `9b4938aa…`，59 块）。
  刷 `fastboot -S 512M flash userdata ~/liuqin-root/ipad71-root-touch3.simg`，boot `shim-boot-touch67.img`，
  然后日志里搜 `|backboardd|`：应能看到 dyld 的 `loaded:` 行或加载错误，以及我们库的 `LIUQIN INPUT:`。

- **2026-10-25 触摸第 90 步：文件都在、backboardd 却没来问 AMFI；把它的 stderr 接到串口（touch2 镜像）**：
  touch67 上机：内核两个文件检查都通过——
  `LIUQIN INPUT: /usr/lib/libliuqintouch.dylib present, 53400 bytes`、
  `... com.apple.backboardd.plist present, 1077 bytes`（**稀疏镜像确实刷进去了**）。
  AMFI 策略日志按进程去重后共 180 个进程来问（`call 90`），**SpringBoard 在列，backboardd 不在**——
  要么它的 dyld 没走到那一步，要么加载失败的错误写进了 /dev/null。
  另发现：`com.apple.backboardd.plist` 里 **`UserName = mobile`**——backboardd 以 mobile 运行，
  所以它打不开 `/dev/console`；但 launchd 是以 root 打开 `StandardErrorPath` 再 setuid 的，
  因此 fd 2 仍可写。于是给这个 job 加了：
  `StandardErrorPath`/`StandardOutPath = /dev/console`，以及 `DYLD_PRINT_LIBRARIES=1`，
  让 dyld 自己把加载过程和错误写到串口上。
  镜像：`~/liuqin-root/ipad71-root-touch2.img`（sha256 `c8ca5f29…`，**完整**）与
  `~/liuqin-root/ipad71-root-touch2.simg`（sha256 `ee1dbb1c…`，**241,836 B / 59 块**，秒级刷写）。
  刷：`fastboot -S 512M flash userdata ~/liuqin-root/ipad71-root-touch2.simg`，再 boot
  `shim-boot-touch67.img`（sha256 `876d18ac…`）。
  判读：若串口出现 `dyld: loaded: /usr/lib/libliuqintouch.dylib` 说明加载成功，接下来只看
  `LIUQIN INPUT:`；若出现 `dyld: Library not loaded` / `code signature` 之类，就是库验证那一层，
  届时按 AMFI 的做法把 `CS_REQUIRE_LV`/adhoc 标志补上。

- **2026-10-25 触摸第 89 步：dyld 确实来问了（call 90），但库仍未加载——加两层诊断（touch67）**：
  touch66 上机：`LIUQIN AMFI: first policy syscall (call 90, from launchd)`——**dyld 的 dyld-policy
  查询已经到达我们的策略并得到允许**（90 = 0x5a），但 `LIUQIN INPUT` 仍是 0 条，backboardd 也没有崩溃
  或 dyld 报错。所以问题不再是"环境变量被剪"，而是"库有没有被加载/文件在不在"。
  touch67（补丁 `0214-liuqin-injection-diagnostics.patch`）加两层诊断：
  1）AMFI 策略日志改为**按进程去重**（`policy syscall %d from %s (pid %d)`），能看到 backboardd 自己
     有没有来问；
  2）内核在触摸驱动启动时用 `vnode_lookup` + `vnode_getattr` **直接检查 root 上两个文件**是否存在、
     多大（`LIUQIN INPUT: /usr/lib/libliuqintouch.dylib present, N bytes` / `... missing (err)`），
     用来确认稀疏镜像是否真的刷进去了。
  镜像 `shim/out/shim-boot-touch67.img`（sha256 `876d18ac…`，payload `dd586cbf…`）——**已编译，未上机**；
  只需换 boot 镜像，root 不用重刷。

- **2026-10-25 触摸第 88 步：注入库没被加载——原因是内核没有 AMFI 的 dyld-policy 回答（touch66 修好）**：
  刷入带 dylib 与 `DYLD_INSERT_LIBRARIES` 的 root 后上机：内核 `liuqin-touch: /dev/liuqin-touch ready`
  正常，但**一条 `LIUQIN INPUT` 都没有**——注入库没有被加载。
  查 Apple 开源 dyld（`DyldDelegates.cpp`）：dyld 启动时调 `amfi_check_dyld_policy_self()`，
  **调用失败就把输出标志清零**，随后 `pruneEnvVars()` 把 `DYLD_*` 全删掉。我们的内核里没有任何名为
  "AMFI" 的 MAC 策略（只有 "LIUQIN AMFI"），mac_syscall 返回 `ENOPOLICY` → dyld 认为进程受限。
  hackios 的 `bsd/kern/kern_minimac.c` 早有答案：策略名必须是 **"AMFI"**，`mpo_policy_syscall` 处理
  请求 **0x5a**（`{flags in, ptr flags out}` → 回 `0xff` 全允许）和 0x60（boot-arg 状态 → `0x86`）。
  照抄进 `bsd/kern/liuqin_amfi.c`（补丁 `0213-liuqin-amfi-dyld-policy.patch`）：策略改名 "AMFI"、
  加 `.mpo_policy_syscall`，并打一条首次调用的日志（`LIUQIN AMFI: first policy syscall (call N, from X)`）。
  镜像 `shim/out/shim-boot-touch66.img`（sha256 `5c53d649…`，payload `d03ed6ef…`）——**已编译，未上机**；
  **root 不用重刷**（上一版稀疏镜像已经带了 dylib 和 plist）。0213 在已应用 0212 的干净树上 check 通过。

- **2026-10-25 触摸第 87 步：用户态注入库 + root 镜像改造完成，完整/稀疏镜像已出（待刷机验证）**：
  1）`host/liuqin_touch_inject.c`（348 行，改编自 hackios 的 MIT `inject.c`，保留出处声明）：
     构造函数里 `IOHIDEventSystemClientCreateWithType(NULL, 4 /*simple*/, NULL)` →
     `IOHIDVirtualServiceClientCreateWithCallbacks`（触摸屏 page 0x0D/usage 0x04 + DisplayIntegrated，
     按键 page 0x0C/usage 0x01），并"直到 event server 打开为止重复创建"；读 `/dev/liuqin-touch`
     的样本，`send_touch()` 用 `IOHIDEventCreateDigitizerEvent` + `IOHIDEventCreateDigitizerFingerEvent`
     + `IOHIDEventAppendEvent` 组帧后 `IOHIDVirtualServiceClientDispatchEvent`，含边沿手势位
     （`DIGITIZER_FROM_EDGE_TIP`）。日志经 `/dev/console` + stderr 输出（`LIUQIN INPUT:` 前缀）。
  2）`tools/build-inject.sh`：用 iPhoneOS SDK 交叉编译 arm64 iOS 16 dylib；私有 IOKit 符号没有 .tbd，
     脚本现场生成 stub .tbd 供链接（运行时从 dyld cache 绑定）。产物
     `shim/out/libliuqintouch.dylib`（53,400 B，sha256 `66ccf6fa…`）。
  3）root 镜像：用 **`cp -c`（APFS clone，秒级、零额外空间）** 复制 `ipad71-root-nodaemon.img` →
     `ipad71-root-touch.img`，挂载后装入 `/usr/lib/libliuqintouch.dylib`，并用 **plistlib** 在
     `com.apple.backboardd.plist` 里加 `EnvironmentVariables.DYLD_INSERT_LIBRARIES =
     /usr/lib/libliuqintouch.dylib`（`plutil -insert` 会把二进制 plist 写坏，勿用）。
     **完整镜像** `~/liuqin-root/ipad71-root-touch.img`（17 GiB，sha256 `127a21b1…`）；
     **稀疏镜像** `~/liuqin-root/ipad71-root-touch.simg`（**319,852 B**，只有 78 个块不同，
     sha256 `90493d61…`）。已复核：镜像内 dylib 的 sha256 与构建产物一致，plist 的 9 个键完整。
  刷机：`fastboot -S 512M flash userdata ~/liuqin-root/ipad71-root-touch.simg`，再
  `fastboot boot shim/out/shim-boot-touch65.img`。
  踩坑记录：第一次在 `cp` 还没结束时就去挂载/编辑目标镜像，导致稀疏包变成 8 GiB 且不可信——
  **复制完成并 `cmp` 验证之后才能改**。
  验证点：内核 `liuqin-touch: /dev/liuqin-touch ready`；用户态 `LIUQIN INPUT: service opened by the
  event server`、`LIUQIN INPUT: reading /dev/liuqin-touch`；随后手指触屏应能点亮/滑动。
  若完全没有 `LIUQIN INPUT`，说明 dyld 拒绝插入（AMFI/dyld 策略），下一步补 AMFI 的 dyld-policy
  回答，或改走依赖注入。

- **2026-10-25 触摸第 86 步：照 hackios 路线开工——内核侧 `/dev/liuqin-touch` 完成（touch65）**：
  补丁 `0212-liuqin-touch-device.patch` 新增 `bsd/sys/liuqin_input.h`（`struct liuqin_touch_sample`
  {time,type,finger,x,y}，坐标归一化到 0..65535）和 `bsd/dev/liuqin_input.c`（512 条环形队列的
  `/dev/liuqin-touch`：`read()` 阻塞取样本，`write()` 可写样本，方便从用户态注入假触摸）；在
  `bsd/dev/liuqin_touch.c` 里对每个解出的接触点发 `DOWN`/`MOVE`/`UP`（UP 带最后位置），
  `liuqin_input_init()` 挂在已经调用 `liuqin_adreno_init()` 的那段 BSD 初始化上。
  镜像 `shim/out/shim-boot-touch65.img`（sha256 `97ce4695…`，payload `50f10e98…`）——**已编译，未上机**；
  0212 在已应用 0211 的干净树上 check 通过。
  下一步：用户态 dylib（改编 hackios 的 MIT `inject.c` 触摸部分，`IOHIDVirtualServiceClient*` +
  `IOHIDEventCreateDigitizerEvent`）→ 改 backboardd 的 launchd plist 加 `DYLD_INSERT_LIBRARIES`
  → 重建 root 镜像验证注入是否被 AMFI/dyld 放行。

- **2026-10-25 触摸第 85 步：参考项目给出一条短得多的路——用户态虚拟 HID 服务注入（hackios/UniXNU）**：
  查了两个跑 iOS 用户态、触摸可用的项目（都靠 UEFI 引导）：
  1）**hackios**（iOS 16.5 / xnu-8796）：内核只有一个 **227 行**的 `/dev/redwood-touch` 字符设备
     （`struct redwood_touch {time,type,finger,x,y}` + 环形缓冲）；用户态 `libredwood.dylib`（**MIT**）
     用 `DYLD_INSERT_LIBRARIES` 注入 **backboardd**，在里面用**虚拟 HID 服务 API** 注入事件：
     `IOHIDEventSystemClientCreateWithType(0, 4 /*simple*/, …)` →
     `IOHIDVirtualServiceClientCreateWithCallbacks(…, &callbacks_v2, …)` →
     `IOHIDEventCreateDigitizerEvent` + `IOHIDEventCreateDigitizerFingerEvent` + `IOHIDEventAppendEvent` →
     `IOHIDVirtualServiceClientDispatchEvent`。这三个符号**在我们 16.6.1 root 的 dyld cache 里都存在**；
     他们还处理了 `kIOHIDDigitizerEventFromEdgeTip`（返回手势/上滑必须的边沿位）。
  2）**UniXNU/sheng**：`ShengHidInput/NativeHidInput.cpp` 发布**带 report descriptor 的 `IOHIDDevice`**
     （report 3 = 十指 touch screen digitizer），由 Apple 的 **`IOHIDEventDriver`**（IOHIDFamily 2031.120.4，
     预链接进内核）解析并派发 —— 也就是"正规"架构，代价就是那 250 KB 的移植。
  → **结论：走 hackios 那条路，完全不需要移植 IOHIDFamily。** 我们内核已经在解帧，剩下三件事：
  （a）内核加一个字符设备把接触点样本暴露给用户态（改编 `redwood_input.c`）；
  （b）一个小 dylib（改编他们 MIT 的 `inject.c` 触摸部分）把样本变成 digitizer 事件；
  （c）把这个 dylib 注入 backboardd。详见 `docs/HID_LAST_MILE.md`。

- **2026-10-25 触摸第 84 步：发布 `IOCFPlugInTypes` 后仍无人 attach（touch64 上机）**：
  `StandaloneHIDAud` 确实读了桩上的 `IOCFPlugInTypes`，但整轮唯一的 `newUserClient` 仍是
  `0x484944 by gamecontrollerd`，`MultitouchSupport` 没有来。说明**仅发布插件映射不足以触发 attach**。
  剩两种解释，取决于 ios-sweet 的答复：
  1）16.6.1 上 `MultitouchSupport` 只对**内核驱动（AppleMultitouchSPI）已经认领**的设备生效，手工造的
     IOHIDDevice 再发布什么也进不去；
  2）attach 是由 backboardd 的 HID 栈经 `IOHIDEventService` 驱动的（也就是 ios-sweet 走的路），
     16.6.1 的栈还在要某个我们没发布的东西。
  另：ios-sweet 文档确认为 Goodix GT9896/I²C + "HID digitizer events" —— **传输不是差异**，
  他们同样是往 IOHIDEventService 里发 digitizer 事件。

- **2026-10-25 触摸第 83 步：反汇编定位到真凶——`MultitouchSupport` 走 CFPlugIn，需要 `IOCFPlugInTypes`（touch64）**：
  用 `ipsw dyld a2s` 把 dyld cache 里的 `HIDServiceSupport` 字符串定位到
  **`/System/Library/PrivateFrameworks/MultitouchSupport.framework`**（IOKit.framework 里那份只是副本）。
  该字符串周围的常量把整条 attach 协议写清楚了：
  `AppleMultitouchDummyV2`、`IOCFPlugInTypes`、`LocationID`、`Event Driver Properties`、
  `IOHIDInterface`、`IOHIDEventService`、`HIDServiceSupport`、UUID
  `0516B563-B15B-11DA-96EB-0014519758EF`、以及 `AppleMultitouchDriver.kext/Contents/PlugIns/
  MultitouchHID.plugin`。
  也就是说 iOS 上 MultitouchSupport 用的是 **CFPlugIn** 路线：对候选服务调用
  `IOCreatePlugInInterfaceForService()`（按那个 UUID 找到插件包并实例化），**由插件**再用 `'HID'`
  用户客户端跟设备对话。没有 `IOCFPlugInTypes` 的服务根本无法被 attach——正好解释了前面所有探针。
  root 里插件在 `/System/Library/Extensions/AppleMultitouchSPI.kext/PlugIns/MultitouchHID.plugin`。
  touch64（补丁 `0211-liuqin-hid-cfplugin-types.patch`）：`AppleMultitouchDevice` 发布
  `IOCFPlugInTypes = {UUID: "AppleMultitouchSPI.kext/PlugIns/MultitouchHID.plugin"}`。
  镜像 `shim/out/shim-boot-touch64.img`（sha256 `641c60ca…`，payload `65211b44…`）——**已编译，未上机**；
  0211 在已应用 0210 的干净树上 check 通过。

- **2026-10-25 触摸第 82 步：结构也排除——黑盒探测到此为止**：
  touch63 上机：`touch relaunched under the device`，backboardd 照样排空 `LiuqinHIDTouch`（2 次），
  但整轮唯一的 `newUserClient` 还是 `0x484944 by gamecontrollerd`（1 次）。**provider 链也不是那道门。**
  至此这些都有真机证据：固件下载/启动、帧解析与坐标、服务发布、backboardd 用自己的字典匹配到 2/2、
  通知投递与排空（backboardd 亲自）、`IOHIDDevice` 路径对 gamecontrollerd 完全可用；类名/provider/
  属性/16.7 用户态全部排除。
  唯一没有答案的是：**backboardd 的 HID 栈为什么拒绝打开我们发布的任何服务**。它排空通知、不读任何
  digitizer 属性、从不调用 `newUserClient` —— 所有可观测项都已对账，决定发生在用户态 HID 库内部。
  后续只有两条：反汇编用户态（`ipsw dyld` + `otool`），或向 ios-sweet 作者求证他们的移植差在哪。

- **2026-10-25 触摸第 81 步：没人对设备做 HID 专属检查；改成"事件服务挂在设备下"（touch63）**：
  touch62 上机（属性读取带进程名）：`backboardd`、`SpringBoard`、`StandaloneHIDAud` 都**只读**
  `IOCFPlugInTypes`/`IOUserServiceProperties` 这类通用键，只有 `gamecontrollerd` 读
  `VendorID`/`ProductID`/`Transport`（手柄识别）。**没有任何进程对设备做 digitizer 专属检查**——
  说明差异在**结构**而不是属性：真机上事件服务是**挂在 `IOHIDDevice` 下的驱动**（`IOHIDEventDriver`），
  backboardd 找到的是那个服务，它的 provider 链里带着设备。
  touch63（补丁 `0210-liuqin-hid-touch-under-device.patch`）：只改父子关系——`AppleMultitouchDevice`
  现在持有 `LiuqinHIDTouch`（设备 → 事件服务），不再都挂在 IOResources 下。
  判读：backboardd 终于 open → provider 链就是那道门；仍不开 → 剩下的未知在用户态 HID 栈内部，
  只能靠反汇编（或问 ios-sweet 作者）。
  镜像 `shim/out/shim-boot-touch63.img`（sha256 `3de81a82…`，payload `2f678605…`）——**已编译，未上机**；
  0210 在已应用 0209 的干净树上 check 通过。

- **2026-10-25 触摸第 80 步：backboardd 亲自排空三个通知——它看得见、就是不开（touch62 查它读什么）**：
  touch61 上机（新增排空者进程名）：
  `drain by backboardd -> LiuqinHIDButtons`、`-> LiuqinHIDTouch`、`-> AppleMultitouchDevice`，
  另外 `drain by SpringBoard -> AppleMultitouchDevice`。**backboardd 的 HID 栈是活的**：两类通知都收到、
  服务对象都取走，但一个都不 open。所以它的判定依据只剩"服务长什么样"，而唯一还能观测的就是它读哪些属性。
  touch62（补丁 `0209-liuqin-hid-property-reader.patch`）：属性日志加上读取者进程名
  （`IOHIDDevice property get Transport by <proc>`）。
  判读：若 `Transport`/`VendorID`/`ProductID` 的读取者是 `backboardd` → 那就是它的准入检查，下一版按值补齐；
  若只是 `gamecontrollerd` → backboardd 的决定依据在内核看不见的地方，只能反汇编用户态。
  镜像 `shim/out/shim-boot-touch62.img`（sha256 `4ef50f0c…`，payload `99688c1b…`）——**已编译，未上机**；
  0209 在已应用 0208 的干净树上 check 通过。

- **2026-10-25 触摸第 79 步：`IOHIDEventSystem` 只是被读属性、没人打开；查"谁在排空通知"（touch61）**：
  touch60 上机：`IOHIDEventSystem stub started`，属性被读过（`IOName`、平台动作、`IOBusyInterest`、
  `BSD Name`），但**没有任何 `newUserClient`** —— 那个类名是用户态内部的名字，不是内核服务。
  同一轮还看到设备桩被读 `Transport`/`VendorID`/`ProductID`（gamecontrollerd 的识别路径），
  打开者仍只有 gamecontrollerd。
  之前所有"backboardd 看见了但不开"的结论都建立在**假设**"排空 backboardd 通知的就是 backboardd"之上。
  touch61（补丁 `0208-liuqin-iokit-drain-process.patch`）把排空者的进程名打出来：
  `LIUQIN IOK: notification drain by <proc> -> <class><name> (armed N)`。
  判读：若 `backboardd` 排空了 `HIDServiceSupport`/`IOHIDDevice` → 它确实看过我们的服务，问题在它的 HID 栈
  要而我们不能给；若排空者是别的东西（或没人排空）→ backboardd 的 HID 栈根本没收到通知，问题在更外面一层。
  镜像 `shim/out/shim-boot-touch61.img`（sha256 `9e6fe41a…`，payload `b7c4a2c7…`）——**已编译，未上机**；
  0208 在已应用 0207 的干净树上 check 通过。

- **2026-10-25 触摸第 78 步：类名不是过滤条件；改探 `IOHIDEventSystem` 服务（touch60）**：
  touch59 上机：`AppleMultitouchDevice started`、该类也有 ping，但**唯一打开者仍是 `gamecontrollerd`**
  （这次它读了 `GameControllerSupportedHIDDevice`、`ProductID` 后放弃）；backboardd 照旧只排空不打开。
  → 类名不是它等的过滤器。
  touch60（补丁 `0207-liuqin-hid-event-system-probe.patch`）：既然用户态 HID 栈里有 `IOHIDEventSystem`
  这个类名而内核里没有，就发布一个同名桩（`IOHIDEventSystem : IOService`），带同样的日志 user client、
  属性日志和 pid 日志。
  判读：有人打开它 → 这就是 HID 栈在等的对象，连接类型 + selector 日志会告诉我们真身要实现什么；
  没人打开 → 那个类名只是用户态库内部的，下一步只能反汇编。
  镜像 `shim/out/shim-boot-touch60.img`（sha256 `2189def0…`，payload `86cb63ca…`）——**已编译，未上机**；
  0207 在已应用 0206 的干净树上 check 通过。

- **2026-10-25 触摸第 77 步：打开者是 `gamecontrollerd`；改用 `AppleMultitouchDevice` 类名（touch59）**：
  touch58 上机：`IOHIDDevice stub newUserClient type 0x484944 by gamecontrollerd`——
  **捡走桩的是游戏手柄守护进程**，而 backboardd 收到了 9 次 `ping type 0x65 for IOHIDDevice`
  并且排空过（`drain -> IOHIDDevice`），却始终不打开。
  推断：iOS 上 digitizer 是 `IOHIDDevice` 的**子类 `AppleMultitouchDevice`**，`IOProviderClass =
  IOHIDDevice` 也能匹配子类，所以用户态很可能在匹配之后**按具体类名过滤**。
  touch59（补丁 `0206-liuqin-hid-multitouch-device.patch`）：把桩改名为
  `AppleMultitouchDevice : public IOHIDDevice`（usage、DeviceUsagePairs、日志 user client、pid 与
  属性日志全部保留）。
  判读：backboardd 终于打开 → 类名就是那道过滤，接着按 selector 序列实现；
  仍只有 gamecontrollerd → 过滤条件是别的（report descriptor/transport/事件服务属性），下一个探针
  去查 `IOHIDDevice` 该发布哪些属性。
  镜像 `shim/out/shim-boot-touch59.img`（sha256 `d1e0c01b…`，payload `f8e5c26e…`）——**已编译，未上机**；
  0206 在已应用 0205 的干净树上 check 通过。

- **2026-10-25 触摸第 76 步：读的全是通用 IOKit 键——查是谁打开的（touch58）**：
  touch57 上机：拿到客户端后，桩上被读的属性是 `IOName`、`IOPlatform*Action`、`IOBusyInterest`、
  `IORematchPersonality`、`IOCFPlugInTypes`、`IOUserServiceProperties` —— 这些是 IOKit
  服务/用户客户端机制（`IOService::open`、CFPlugIn 查找、DriverKit user service 检查）对**任何**
  服务都会读的通用键，**没有任何 HID 相关键**（`ReportDescriptor`/`PrimaryUsage*`/`Transport`），
  而且客户端从未被关闭。所以"不动"的决定发生在别处——很可能**打开者不是我们以为的进程**。
  touch58（补丁 `0205-liuqin-hid-device-open-pid.patch`）：在 `newUserClient` 里打出打开者的
  pid/进程名：`IOHIDDevice stub newUserClient type 0x484944 by <procname>`。
  判读：`backboardd`/`SpringBoard` → 我们的桩到了正确的消费者手里；`gamecontrollerd`（它正好在同一秒
  被 exec）或别的守护进程 → 桩被错误的 HID 客户端捡走了，要查的是"digitizer 自己的客户端为什么不连"。
  镜像 `shim/out/shim-boot-touch58.img`（sha256 `f4002f0f…`，payload `a74bfb69…`）——**已编译，未上机**；
  0205 在已应用 0204 的干净树上 check 通过。

- **2026-10-25 触摸第 75 步：客户端被拿到但零调用——开始追"它读了什么"（touch57）**：
  touch56 上机：`IOHIDDevice stub gave out a logging client`，然后**一条 `device client sel` 都没有**。
  也就是说用户态 open 成功后、发第一个请求之前就退出了——它的前置检查在别处（最可能是设备的注册表
  属性：`ReportDescriptor`、usage、transport 等，或客户端早期状态）。
  touch57（补丁 `0204-liuqin-hid-device-property-probe.patch`）：在设备桩和日志客户端上补三样东西——
  对桩的每次 `getProperty()` 打印 `LIUQIN HID: IOHIDDevice property get <key>`（用户态的注册表读取
  会落到这里）、`device client closed/died` 让"放弃"可见、外加原有的 selector 日志。这样"为什么放弃"
  就会变成一个具体的 key 名。
  镜像 `shim/out/shim-boot-touch57.img`（sha256 `a1bac3fc…`，payload `7eab6cba…`）——**已编译，未上机**；
  0204 在已应用 0203 的干净树上 check 通过。

- **2026-10-25 触摸第 74 步：先用"记日志的 user client"探出 HID 设备调用序列（touch56）**：
  与其盲移 250 KB Apple 源码，先给 `'HID'` 连接类型一个**只记日志的 `IOUserClient`**
  （补丁 `0203-liuqin-hid-device-client-probe.patch`）：按 `kIOHIDLibUserClient*` 的枚举把
  selector 翻译成名字（deviceIsValid/open/close/createQueue/disposeQueue/addElementToQueue/
  removeElementFromQueue/queueHasElement/startQueue/stopQueue/updateElementValues/
  postElementValues/getReport/setReport/getElementCount/getElements/setQueueAsyncPort/
  releaseReport/resumeReports），并打印 scalar/structure 参数规模；每个方法都返回
  `kIOReturnUnsupported`。这样一次开机就能看到用户态真实的调用序列，直接指出"真实移植先要实现哪个
  API、参数长什么样"。
  镜像 `shim/out/shim-boot-touch56.img`（sha256 `89d6b055…`，payload `a118bd29…`）——**已编译，未上机**；
  0203 在已应用 0202 的干净树上 check 通过。
  备注：`IOHIDFamily/IOHIDDevice.cpp` 的依赖闭包（IOHIDInterface/IOHIDEventSource/
  IOHIDReportElementQueue/descriptor parser…）已盘点清楚，约 30 个文件；`IOHIDDescriptorParser`
  实现在 `IOHIDSystem/IOHIDDescriptorParser/` 下（一堆小 .c），移植时一并带上。

- **2026-10-25 触摸第 73 步：探针命中——用户态用 'HID' 类型打开 `IOHIDDevice`（决定性）**：
  touch55 上机：
  `LIUQIN IOK: notification ping type 0x65 for IOHIDDevice id 0x1000001c1` +
  `LIUQIN HID: IOHIDDevice stub newUserClient type 0x484944`。
  `0x484944` = `0x00484944` = **`kIOHIDLibUserClientConnectManager`**（`IOHIDLibUserClient.h` 里注释
  就是 `/* HID */`），而 `IOHIDDevice::newUserClient()` **只接受这个类型**并转给
  `newUserClientInternal()` → **`IOHIDLibUserClient`**。
  → 缺口至此完全确定：内核需要真正的 **`IOHIDDevice` + `IOHIDLibUserClient`**，触摸必须以
  **HID 设备**的形式提供**多点触控 report descriptor** 和输入报告；`IOHIDEventService` 那条路对
  digitizer 是死路。
  移植清单（IOHIDFamily-1915.140.3）：`IOHIDDevice.cpp` 95 KB、`IOHIDLibUserClient.cpp` 78 KB、
  `IOHIDElementPrivate.cpp` 73 KB、`IOHIDDeviceElementContainer.cpp` 2 KB、descriptor parser 若干、
  `IOHIDFamilyPrivate`/`IOHIDWorkLoop`/`IOHIDInterface` 等，约 250 KB；`IOHIDDevice.cpp` 还
  include 了 macOS 味道的 `<IOKit/hidsystem/IOHIDSystem.h>`，需要打桩。
  第二个半场是**报告格式**：用户态自己解析 vendor 多点触控（0xff00/0xf）的输入报告，descriptor
  必须合它的胃口；先把 `IOHIDLibUserClient` 跑通、把用户态的请求（读 report descriptor、建队列、
  头几个报告）打进日志，再照着定型。详见 `docs/HID_LAST_MILE.md`。

- **2026-10-25 触摸第 72 步：`IOHIDDevice` 桩探针（touch55）**：
  按评估结论做探针（补丁 `0202-liuqin-hid-device-probe.patch`）：加一个**类名就叫 `IOHIDDevice`**
  的内核类（`IOService` 子类），带 `PrimaryUsagePage 0xff00`、`PrimaryUsage 0xf`、`Transport SPI`、
  `Product` 和一条 `DeviceUsagePairs`，由**同一个晚启动定时器**（`liuqinhidrelaunch`，默认 150 s）
  在 IOResources 下创建，`newUserClient` 打印每次连接的类型。
  判读：出现 `LIUQIN HID: IOHIDDevice stub newUserClient type 0x…` → 用户态确实会打开
  `IOHIDDevice`，要移植的是 `IOHIDDevice` + `IOHIDLibUserClient`（类型号还能告诉我们用哪个）；
  一条都没有 → 用户态既不要 `IOHIDEventService` 也不要 `IOHIDDevice`，只剩反汇编它的 HID 栈。
  镜像 `shim/out/shim-boot-touch55.img`（sha256 `d098e712…`，内核 `b3a93796…`，payload
  `cf5f846c…`）——**已编译，未上机**；0202 在已应用 0201 的干净树上 check 通过。

- **2026-10-25 触摸第 71 步：可行性评估——`IOHIDEventSystem` 不是缺的那块，真正可能是 `IOHIDDevice`**：
  拉了 Apple 开源 **IOHIDFamily-1915.140.3**（与 sweet 同版本）逐文件核对，结论写在
  `docs/HID_LAST_MILE.md`：
  1）`IOHIDFamily/IOHIDEventSystem.cpp`（11 KB）**不是事件路由器、也没有 user client**
     （全文 0 处 `newUserClient`/`UserClient`）——它只是"调试中转"：start 时给 `IOHIDEventService`
     类注册通知，对每个服务 `service->open(this, …)` 然后把事件 `HIDLog` 出来。真正的分发在
     `IOHIDEventService::dispatchEvent()` 里遍历 `_clientDict`，也就是**已经 open 进来的客户端**，
     正是本移植桩里已有的东西。→ **移植 `IOHIDEventSystem` 完全无用**，评估直接否掉。
  2）用户态那侧 `IOHIDEventSystemClient` 在 macOS 由 `IOHIDSystem/IOHIDUserClient.cpp` 的
     `IOHIDEventSystemUserClient`（`kIOHIDEventSystemConnectType`）服务，而 iOS 没有这个 kext。
     所以 iOS 上事件只能经**每服务的 user client（`IOHIDEventServiceUserClient`, 'esuc'）**或
     **`IOHIDDevice`** 路径到达 backboardd。
  3）内核 trace 显示 backboardd 的 HID 匹配只有 4 条：3 条 `IOProviderClass = IOHIDDevice`
     （`PrimaryUsagePage 0xff00/PrimaryUsage 0xf` 等）+ 1 条 `IOService + HIDServiceSupport`。
     **它从不按名字匹配 `IOHIDEventService`/`IOHIDEventSystem`**，也从不 open 我们的服务。
     最合理的解释：iOS 16 的触摸屏是 **IOHIDDevice**（Apple 多点触控设备的 vendor usage
     page 0xff00 / usage 0xf），`HIDServiceSupport` 只是给其它类型服务做登记。
  下一步探针（便宜、能二选一）：在 IOResources 下发一个**类名就叫 `IOHIDDevice`** 的桩服务，
  带 `PrimaryUsagePage 0xff00 / PrimaryUsage 0xf`，user client 打印每次连接的类型，
  并且晚启动（复用 touch51 的"排空后再匹配"技巧）。它若被连接 → 真正要移植的是
  `IOHIDDevice` + `IOHIDLibUserClient`；若没人连接 → 只能反汇编用户态 HID 栈
  （`ipsw dyld` + BackBoard.framework）来确定它到底要什么。

- **2026-10-25 触摸第 70 步：`/product` 生效但无效——最后只剩内核 `IOHIDEventSystem`**：
  touch54b 上机：`path IODeviceTree:/product -> IOPlatformDevice`（之前是 `none`），**`/product` 现在
  能被 backboardd 正确读取**；但 HID 侧毫无变化——`newUserClient` 仍然是 0 条，按键仍是
  `(0 clients)`。所以设备树也不是那道门。
  至此所有"便宜"的假设都排除了：注册/匹配/ping/排空全通、属性正确、DRAM 修正、`/product` 补齐、
  16.7 用户态 HID 接口面与 16.6.1 完全相同。**唯一剩下的缺口就是内核里的 `IOHIDEventSystem`**
  （这个用户态的 libIOHID 通过它打开服务并转发事件；本移植只有桩 `IOHIDEventService`）。
  `/product` 节点保留（让设备树更忠实，也是 mkadt 原本的设计）。
  下一步只有两条：移植 `IOHIDEventSystem`（Apple 开源 IOHIDFamily），或查清 ios-sweet 在同一份
  HID 补丁下为何能出输入（未解）。

- **2026-10-25 触摸第 69 步：`/product` 首次构建把 DRAM 基址改错导致 shim panic（touch54→54b）**：
  上机 touch54 直接蓝屏，是 **shim 自己的异常处理**（`shim/loader.c` 的 `exc_handler`），
  `STAGE ldr-copy`、`ESR 0x96000010`、`FAR 0x101004000`。原因不在 `/product`，而在重新生成 ADT 时
  忘了给 DRAM 参数：`mkadt.py` 的默认是 `0x100000000+0x17ff00000`，而 `mkpayload` 用 ADT 里的
  dram-base/size 决定内核被拷到哪里，于是 loader 把内核段拷到 4 GiB——这块板子上是空洞，外部中止。
  对照 payload 头即可确认：能用的一直是 `phys 0x840000000 / mem 0x13ff00000`（ADT `dram-base`
  `0x840000000`、`dram-size` `0x13ff00000`）。正确命令：
  `python3 tools/mkadt.py shim/out/liuqin.adt --dtb dtb/boot_unpacked/dtb --dram-base 0x840000000 --dram-size 0x140000000`
  （`docs/LOCAL_BUILD.md` 里原来漏了这两个参数，已补上并注明后果）。
  重建 `shim/out/shim-boot-touch54b.img`（sha256 `cc04b22d…`，payload `d3a6043d…`，ADT 里带 61 条
  `/product` 属性，payload 头与 touch53 一致）——**已编译，未上机**；touch54 那张作废。

- **2026-10-25 触摸第 68 步：16.7 用户态同样没有直接 attach；补上 D321AP `/product`（touch54）**：
  预检 iPad6,7 的 **16.7.16（20H392）**（就是 ios-sweet 跑的那个构建）dyld cache：
  `IOHIDEventServiceOpenService` 0、`IOHIDEventSystemClientCreateWithType` 3、
  `IOHIDEventSystem` 74、`HIDServiceSupport` 4 —— **和 16.6.1 完全一致**，所以换 root 不可能改变
  HID attach 路径（两个 IPSW 的 DeviceTree 载荷都是 iBoot `bvx2`/LZFSE 容器，必须用 `ipsw` 才能读）。
  转而补唯一还能看到的差异：我们 ADT 里没有 `/product`（每轮日志都是
  `path IODeviceTree:/product -> none`），sweet 有。做法：
  `ipsw img4 im4p extract Firmware/all_flash/DeviceTree.j317ap.im4p -o d321ap.adt`（iPad8,1=D321AP
  的 IPSW `iPad_Pro_A12X_A12Z_16.6.1_20G81`，板子 j317ap），放到 `research/ipad-d321ap/d321ap.adt`，
  再 `python3 tools/mkadt.py shim/out/liuqin.adt --dtb dtb/boot_unpacked/dtb` → ADT 里出现 61 条
  `/product` 属性（`iPad Pro (11-inch)`、`artwork-device-idiom pad`、`graphics-featureset-class APPLE5`、
  `artwork-scale-factor 2`、`thin-bezel 1` …）。
  工具：blacktop `ipsw` 的预编译 macOS x86_64 包（无需 Homebrew/Go；Go 的 `go install` 因
  模块里有 replace 指令而不可用）。
  镜像 `shim/out/shim-boot-touch54.img`（sha256 `80965ba8…`，payload `b419b5a8…`，ADT `cba15be7…`）
  ——**已编译，未上机**。若 `/product` 真是 backboardd 打开 HID 服务的前置条件，这一步就是最后一里；
  否则只剩移植 `IOHIDEventSystem`。

- **2026-10-25 触摸第 67 步：结论——缺的是内核 `IOHIDEventSystem`，不是触摸驱动（touch53 上机）**：
  touch53 上机：**一条 `newUserClient` 都没有**——backboardd 把服务对象取走（和 AppleCLCD 一样，
  那是它通用的 IOKit 监视），但从不打开。
  查这台 root（iOS **16.6.1**，20G81）的 dyld cache 字符串：
  `HIDServiceSupport` 4 处、`IOHIDEventSystemClientCreateWithType` 3 处、`IOHIDEventSystem` 74 处、
  **`IOHIDEventServiceOpenService` 0 处**。也就是说这个用户态**没有"直接打开某个 event service"
  的 API**，它靠内核的 `IOHIDEventSystem` 去打开并转发事件；而本移植只有桩 `IOHIDEventService`
  （补丁 0058），内核里没有 `IOHIDEventSystem`。**所以触摸和按键都不可能送进 backboardd**，
  与触摸驱动本身无关。
  ios-sweet 用同一份 HID 补丁在 **iOS 16.7（20H392）** 上能出输入，说明它的用户态走的是
  这条 16.6.1 root 没有的路径。
  触摸侧至此全部完成：固件启动、帧解析、坐标标定、`liuqin_hid_touch()` 组 digitizer collection、
  服务被匹配/ping/排空都已验证。
  两条更大的路（都不是小补丁）：
  1）把 `IOHIDEventSystem`（Apple 开源 IOHIDFamily）移植进内核，并让桩服务接入——数天工作量，
     桩 `IOHIDEventService` 也要长成真身；
  2）把 root 换成 **iOS 16.7**，对齐 ios-sweet 的已验证组合。

- **2026-10-25 触摸第 66 步：用户态确实来取服务了，但没 open；查它用什么类型开（touch53）**：
  touch52 上机出现 `LIUQIN IOK: notification drain -> LiuqinHIDTouch…`——**整条通知链是通的**
  （内核匹配 → ping → 用户态排空并拿到服务对象），唯独不 open。
  而 `newUserClient()` 原来只在**失败**时打日志，所以"成功了但类型不同"这种情况看不见。
  touch53（补丁 `0201-liuqin-hid-open-type-probe.patch`）：每次 open 尝试都打
  `newUserClient type 0x…`，成功再打 `user client created type 0x…`，并且**接受 type 0 和 1**。
  判读：出现 `newUserClient type 0x…` → backboardd 确实在开，看类型是否等于 `'esuc'`（0x65737563）；
  没有 → backboardd 拿到服务后决定不开，问题在它的回调逻辑（它期望 IOHIDEventService 具备什么）。
  镜像 `shim/out/shim-boot-touch53.img`（sha256 `d1412f0c…`，内核 `34bc4ba1…`，payload
  `91eadec2…`）——**已编译，未上机**；0201 在已应用 0200 的干净树上 check 通过。

- **2026-10-25 触摸第 65 步：ping 到了、open 没到；查用户态有没有排空（touch52）**：
  touch51 上机：`buttons relaunched` / `touch relaunched` 之后各出现一条
  `LIUQIN IOK: notification ping type 0x65 for LiuqinHIDButtons/Touch id 0x…`。
  `0x65` = 101 = `kIOServiceMatchedNotificationType`，**内核确实为两个新服务唤醒了 backboardd**；
  但 backboardd 仍然不 open，重启后的按键也还是 `(0 clients)`。
  下一步是看用户态有没有来取服务：ping 之后 backboardd 的 IOKit 库要调 `IOIteratorNext`，
  落到内核的 `IOServiceUserNotification::copyNextObject()`（既发服务、又把 `armed` 置回）。
  touch52（补丁 `0200-liuqin-iokit-drain-probe.patch`）在这个函数里打
  `LIUQIN IOK: notification drain -> <class><name> (armed N)`。
  判读：有 drain 行 → 用户态确实取到了服务，不开是 backboardd 自己的决定（回调逻辑或 libIOHID
  版本，root 16.6.1 vs 内核 HID 代码 16.7）；没有 drain 行 → 用户态 IOKit 通知路径本身断了。
  镜像 `shim/out/shim-boot-touch52.img`（sha256 `ea5e0a98…`，内核 `3aa40312…`，payload
  `972a241e…`）——**已编译，未上机**；0200 在已应用 0199 的干净树上 check 通过。

- **2026-10-25 触摸第 64 步：ping 机制查清——已存在的服务只会静默入队（touch51 用"晚建实例"）**：
  touch50 上机：**0 条 ping**。查 IOKit 源码得到解释：`IOServiceUserNotification::handler()`
  只在 `armed` 为真时 ping，而 `armed` 只在 `IOUserNotification::next()` 把队列**排空之后**才置位
  ——也就是说**通知注册时就已经存在的服务只会进初始队列、永远不 ping**；只有"客户端排空之后
  才第一次匹配到"的服务才会唤醒它。我们的 HID 服务在内核初始化时就 start 了，永远落在初始队列里，
  这也解释了 touch47/48 的"晚 register"为什么没用：重新注册不是新的 first-match。
  touch51（补丁 `0199-liuqin-hid-relaunch.patch`）：定时器（`liuqinhidrelaunch`，默认 150 s）
  在 IOResources 下**新建**一对 `LiuqinHIDButtons`/`LiuqinHIDTouch` 实例并 start——新实例 =
  新的 first-match，正好落在 ping 生效的窗口里。
  判读：`relaunch in 150 s` → `buttons/touch relaunched` → 如果出现
  `LIUQIN IOK: notification ping ... for LiuqinHIDTouch` 就说明 backboardd 被唤醒；
  再出现 `LIUQIN HID: LiuqinHIDTouch open by ... -> 1` 就是最后一公里通了。
  重启后的新实例就是 `liuqin_hid_touch()` 投递的目标，所以 150 s 以后在锁屏上滑动就是真检验。
  镜像 `shim/out/shim-boot-touch51.img`（sha256 `330397a7…`，内核 `9aa5be0f…`，payload
  `d0734690…`）——**已编译，未上机**；0199 在已应用 0198 的干净树上 check 通过。

- **2026-10-25 触摸第 63 步：内核内匹配能命中（2/2），问题在外层——加通知 ping 探针（touch50）**：
  touch49 上机：`LIUQIN HID: selftest: HIDServiceSupport match found 2 services, 2 ours`
  ——**用 backboardd 一模一样的字典在内核里匹配，两个服务都能命中**。所以注册表、属性、匹配本身
  都没问题。（`matchPropertyTable` 一条都没有也解释清楚了：IOKit 的匹配走 passive 匹配，
  根本不调用那个虚函数，之前那个探针无效。）
  也就是说失败点在更外层：内核能找到，但 backboardd 从不打开。按 IOKit 源码，
  `addMatchingNotification()` 对新注册的通知**会补发已存在的匹配**（`installNotification()`
  建 existing 迭代器并逐个 `invokeNotifier()`），用户态投递再走
  `IOServiceUserNotification::handler()` 的 mach "ping"。所以下一步观察这个 ping。
  touch50（补丁 `0198-liuqin-iokit-notification-ping.patch`）：在 handler 的 sendPing 分支打
  `LIUQIN IOK: notification ping type … for <service> id …`。判读：
  1）出现两条（我们的 touch/buttons）：内核确实把匹配投递给了 backboardd → 拒绝发生在
     backboardd 内部（root 是 16.6.1，而内核 HID 代码来自 16.7 的 IOHIDFamily，用户态 ABI
     不一致，或它的回调逻辑）；
  2）一条都没有：用户态通知投递本身断了（mach 消息/端口/armed 状态），修法在内核侧。
  镜像 `shim/out/shim-boot-touch50.img`（sha256 `ba24c872…`，内核 `62aa2f25…`，payload
  `7657d15f…`）——**已编译，未上机**；0198 在已应用 0197 的干净树上 check 通过。

- **2026-10-25 触摸第 62 步：立即发布后仍然没有任何匹配；改成内核内自测（touch49）**：
  touch48b 上机（`liuqinhidpublish=0`，服务在内核初始化时就 `published now`）：日志里
  **`matchPropertyTable` 依然是 0 条**——一个在 backboardd 匹配前 ~100 s 就已进入注册表的服务，
  也没走到匹配器。说明问题不在发布时机，而在"匹配合本身"。
  touch49（补丁 `0197-liuqin-hid-registry-selftest.patch`）：发布后立刻在**内核里**跑一遍
  backboardd 用的同一个匹配（`IOService::getMatchingServices`，
  `IOProviderClass = IOService` + `IOPropertyMatch HIDServiceSupport = true`），并打印
  "找到几个服务、其中几个是我们的"：
  `LIUQIN HID: selftest: HIDServiceSupport match found N services, M ours`。
  判读：`M = 2` → 注册表侧没问题，差异在用户态匹配（属性编码、通知，或 backboardd 实际匹配的字典
  需要另存一份）；`N = 0`（连内核侧都找不到）→ 我们的服务不在匹配器走的那个 plane 里，
  问题在发布/provider，而不是属性。
  镜像 `shim/out/shim-boot-touch49.img`（sha256 `2fdb5971…`，内核 `69226339…`，payload
  `a388254f…`）——**已编译，未上机**；0197 在已应用 0196 的干净树上 check 通过。

- **2026-10-25 触摸第 61 步：探针结果——匹配根本没到我们（touch48 上机）**：
  touch48 上机日志里 **`matchPropertyTable` 一条都没有**。而这次启动用的是
  `liuqinhidpublish=120`，服务到 **220 s** 才发布，backboardd 的 `HIDServiceSupport`
  匹配在 **139 s** 就武装好了——也就是说这次测的是"发布晚于监听"的顺序，通知并没有带来任何匹配。
  下一次实验（touch48b）把 `liuqinhidpublish=0`（服务在内核初始化时就进注册表，远早于 139 s），
  内核里仍然带着匹配探针，于是两种可能被干净地分开：
  1）139 s 前后出现 `LIUQIN HID: LiuqinHIDTouch matchPropertyTable -> 1`：匹配能到，backboardd
  看到了但拒绝打开 → 查属性表（class 名/usage/transport/user client 版本）；
  2）仍然一条都没有：服务压根没进匹配器 → 查注册/发布路径或 IOService plane，修法在内核侧。
  镜像 `shim/out/shim-boot-touch48b.img`（sha256 `861b5102…`，payload `8cd26815…`，
  内核同 touch48 `9c3b7eb1…`）——**已编译，未上机**。

- **2026-10-25 触摸第 60 步：晚发布无效；加 `matchPropertyTable` 探针（touch48）**：
  touch47 上机：`buttons publishes in 120 s` → 220 s 时 `published late`（backboardd 早在 ~139 s
  就武装了 `HIDServiceSupport` 通知），之后按键仍是 `key 0x40 down (0 clients)`，**没有任何
  `open by`，也没有一条 HID user-client 日志**。所以"发布太早"不是原因。
  新线索：这颗 root 是 **iOS 16.6.1（20G81）**（`ipad71-root-nodaemon.img` 里的
  `SystemVersion.plist`），而 ios-sweet 是 16.7（20H392），他们的 HID user client/queue 是照
  IOHIDFamily-1915.140.3（16.7）编的。ABI 不一致能解释"打开被拒"，但解释不了"根本没有 open"。
  touch48（补丁 `0196-liuqin-hid-match-probe.patch`）：重写 `IOHIDEventService::matchPropertyTable()`
  并打日志——每次用户态匹配都会过这里。判读：
  1）出现 `LIUQIN HID: LiuqinHIDTouch matchPropertyTable -> 1`：服务的属性表被匹配到，
  但 backboardd 拒绝打开 → 查属性（class/usage/transport/version）；
  2）一条都没有：服务不在 backboardd 搜索的那个 plane 里 → 查发布/注册。
  镜像 `shim/out/shim-boot-touch48.img`（sha256 `018d3a1c…`，内核 `9c3b7eb1…`，payload
  `47e65659…`）——**已编译，未上机**；0196 在已应用 0195 的干净树上 check 通过。

- **2026-10-25 触摸第 59 步：HID 服务发布了但 backboardd 从不打开；先试"晚发布"（touch47）**：
  `liuqinhidtest=6` 打出 `key ... (0 clients)`，`IOHIDEventService::open()` 也一条 `open by` 都没有，
  所以 backboardd 匹配到了服务却**从不打开**。证据链：
  1）内核里只有我们的 `IOHIDEventService` 桩，没有 `IOHIDEventSystem`/`IOHIDEventDriver`/`IOHIDDevice`；
  2）backboardd 确实在 boot ~50 s 注册了正确的通知
  （`IOServiceFirstMatch <IOProviderClass IOService> <IOPropertyMatch HIDServiceSupport true> -> notify`）；
  3）两个 HID 服务在**内核初始化时**（boot ~44 s）就 registerService 了，早于那个通知；
  4）`handleOpen`/`newUserClient` 都会打日志，而 backboardd 的 IOKit trace 里有 AppleCLCD、
     IOPMRootRegion 等 open，唯独没有我们。
  实现和 ios-sweet 的 0058 逐字节等价（他们那边 `SweetHIDTouch -> backboardd` 是通的），
  所以差异更可能在**出现时机**而不是代码内容。
  touch47（补丁 `0195-liuqin-hid-publish-late.patch`）：服务照常 start、照常收事件，但把
  `registerService()` 推迟到 `liuqinhidpublish` 秒（默认 120，0 = 旧行为）；启动行保留
  `liuqinhidtest=45`，按键在 45/90/135 s 各按一次并打印客户端数。
  判读：日志出现 `LIUQIN HID: touch published late`（~120 s）后，
  若跟着 `LIUQIN HID: <service> open by <client> -> 1` 或 `key ... (N>0 clients)`，
  说明只是时机问题，把发布固定到触摸驱动启动时（~80 s）即可；
  若仍然没人打开，下一步就得移植真正的 `IOHIDEventSystem`（工作量大得多）。
  镜像 `shim/out/shim-boot-touch47.img`（sha256 `a560d5eb…`，内核 `bf5fc67a…`，payload
  `693fb1ef…`）——**已编译，未上机**；0195 在已应用 0194 的干净树上 check 通过。

- **2026-10-25 触摸第 58 步：滑动被拆成一串点击；HID 服务没人打开（touch46）**：
  touch45 日志：`HID: touch 0 down at 1003,2879` → `up` → `down at 970,2455` → `up` …一次手势
  **55 次 up**，锁屏不动。两件事分开看：
  1）**"空帧即抬起"太激进**：两次上报之间读到的帧本来就是空的（和抬起长得一样），第一版每读到一个空帧
  就松手，于是拖动被切成点击。touch46 改成"连续 ~6 个空帧才算抬起"（约 120 ms），并且去掉循环里
  第二次 109 字节读取——同一块单条缓冲每 tick 读两次会和控制器抢。
  2）**HID 事件没有消费者**：`IOHIDEventService::open()` 里会打
  `LIUQIN HID: <service> open by <client>`，整份日志**一条都没有**；`dispatchEvent()` 把事件
  发给 `_clients` 数组，而它是空的。也就是说：芯片→帧→`liuqin_hid_touch()` 全通，
  事件在核内被丢掉，因为 backboardd 从来没打开这个服务。
  所以"不动"已经不是触摸驱动的问题，而是 HID 服务与 backboardd 之间的接线（按键那条路也因此从没验证过）。
  touch46（补丁 `0194-liuqin-touch-drag-hysteresis.patch`）修第 1 条；
  `shim-boot-touch46-hidtest.img` 是同一内核 + `liuqinhidtest=6`：按键服务每 6 s 按一次 Home，
  并打印 `LIUQIN HID: key 0x.. down (N clients)`——**这个 N 就是"有没有消费者"的答案**：
  0 = 服务发布了但没人连（要补 HID 接线），>0 = 接线是通的，只剩触摸事件格式问题。
  镜像 sha256：`8b5d786b…`（hidtest）/ `f6022977…`（普通）；内核 `db8cfc85…`。

- **2026-10-25 触摸第 57 步：单指解析正确，补上"空帧即抬起"（touch45）**：
  touch44 上机日志：`fwinfo … ver 0x19 ok, 38x60 sensors, sensor space geometry`、
  `report space 18000x28800`，然后 `finger 1 ev 2 raw 5120,12360 of 18000x28800` →
  `HID: touch 0 down at 512,1236`，接着 raw 平滑走到 3298,9016 → 827,5013 → 36,3325，
  **id 稳定为 1、event 恒为 2**，说明厂商布局（6 字节记录、id/event 在 +0、x/y 大端 16 位在
  +1/+3）是对的，`raw/10` 正好是面板像素（18000x28800 = 10 × 1800x2880）。
  同时 `gpio21 asserted (level 1)` 说明 **INT 线其实是好的**，只是第一条帧到得比第一次边沿早，
  线程已经切到轮询。
  **但整份日志没有任何 `HID: touch … up`**：抬起时控制器发的是空帧（`ff ff ff…`，手势末尾是
  `fd fd fd…`），而驱动只从"有效帧"里回收 slot，于是最后一根手指永远按着——backboardd 会因此
  忽略之后所有输入。
  touch45（补丁 `0193-liuqin-touch-release-on-empty.patch`）：帧里一条有效记录都没有时，
  把所有按下的 slot 全部抬起（误抬一次只丢一帧，卡住一根手指会毁掉整个会话）。
  镜像 `shim/out/shim-boot-touch45.img`（sha256 `0c607b5b…`，内核 `296b9634…`，payload
  `14e9150f…`）——**已编译，未上机**；0193 在已应用 0192 的干净树上 check 通过。
  下一步是用户可见的验证：单指从屏幕底部往上划（解锁手势），看锁屏是否响应。

- **2026-10-25 触摸第 56 步：上报通了（HID 收到触摸），但帧布局是旧款的（touch44）**：
  touch43 上机第一次出现真实上报：`block 0a 28 05 5a c2 65 …` → `first frame …` →
  `LIUQIN HID: touch 3 down at 1217,1799`、`touch frame 1`，随后
  `gpio21 did not assert for a touch, polling the event buffer`（INT 一直不拉，靠轮询兜住）。
  也就是说：**控制器在扫描、帧落在 0x125800、已经走到 HID 投递**——这条链第一次跑通。
  但手指编号和坐标是乱的。两个原因：用户是多指乱滑（一帧多条记录本来就会这样），
  以及真正的 bug——**这颗芯片不用旧款的 12 位 x/y 打包**。
  看厂商模块的 `nvt_ts_work_func`：记录直接从事件缓冲起、步长 6，字节 0 = `(id<<3)|event`，
  **x 是 +1 处的大端 16 位、y 是 +3 处的大端 16 位**，只接受 event 1/2（3 = 抬起，走 slot 跟踪）。
  我们按旧款 `x=(p[1]<<4)|(p[3]>>4)` 解，全是噪声。
  touch44（补丁 `0192-liuqin-touch-frame-layout.patch`）：换成厂商布局的
  `tch_rec_id/event/x/y`；fwinfo 里的 `0x4650`/`0x7800` 是**传感器空间**（18000×30720，
  是面板的十倍），原来的 `<=4096` 判据把它们当垃圾扔了，现在接受并用它们做缩放；
  前 6 条有效记录打印 `finger <id> ev <e> raw <x>,<y> of <max>x<max>` 便于标定。
  镜像 `shim/out/shim-boot-touch44.img`（sha256 `cdaca631…`，内核 `95f97a58…`，payload
  `931c90ef…`）——**已编译，未上机**；0192 在已应用 0191 的干净树上 check 通过。
  下次**单指**从一角慢速划到对角，看 `raw` 是否单调、比例是否正确；若镜像/交换则用
  `liuqintouchflip/swap`。

- **2026-10-25 触摸第 55 步：把上报路径画到屏幕上（touch43）**：
  touch42 的第二次上机复现了同样的结果（`touch fw BOOTED at 131.365 s`，67 s 传输），
  之后只有 `report thread up, INT on gpio21` 和 `gpio21 in/out 0x00000000`，再没有别的——
  事件缓冲的诊断只进日志，屏幕上什么也看不出来。
  touch43（补丁 `0191-liuqin-touch-report-probe.patch`）做三件事：
  1）修正 fwinfo 解码：有效判据改成 `fw[0]+fw[1]==0xff`（这个 map 的 `fw[4..7]` 不是几何值），
  打印 `ver 0x19 ok, 38x60 sensors`，上报空间仍用面板 1800x2880；
  2）上报路径加屏幕标记：前 6 次 gpio21 电平变化 `touch int <l>`、前 4 次事件缓冲变化但解不出
  记录 `touch evt <b0 b1 b2 b3>`、前 6 次解出记录 `touch frame <n>`；
  3）删掉上报循环里遗留的候选地址扫描（`0x11a00`/`0x2fe00`/`0x3f004`…，那是 NT36523 时代的，
  每 100 ms 白读五次）。
  判读：`touch frame` = 上报通了（剩下坐标和 HID 投递）；只有 `touch evt` = 缓冲在动但解码不接受
  （字节已经打在屏幕上）；只有 `touch int` = 中断通、缓冲地址还不对；一条都没有 = 芯片没在扫描，
  下一步照抄厂商 boot 后的 mode/handshake 序列。
  镜像 `shim/out/shim-boot-touch43.img`（sha256 `c0298530…`，内核 `ee178fa5…`，payload
  `0a8e70f3…`）——**已编译，未上机**；0191 在已应用 0190 的干净树上 check 通过，
  应用后与编译用的驱动逐字节相同。

- **2026-10-25 触摸第 54 步：touch42 上机——固件起来了（`state 0xa0@125860`）**：
  换用 NT36532 自己的 map 之后，屏幕最后一条是 `touch fw BOOTED`：
  `fw dl BOOTED: 201924 B 6 chunks 68 s, txinfo 0x03@200, crc post 0x04 ilm e3173a51
  dlm cd21b90f, state 0xa0@125860 1 polls`，fwinfo `19 e6 26 3c …` = 版本 0x19 +
  反码 0xe6、`0x26 0x3c` = **38×60**，正是 `docs/TOUCH_FW_RE.md` 里记的验收值。
  **"下载→校验→boot_ready"这一章到此结束**，touch39–41 失败纯粹是 NT36523 的地址。
  细节：`txinfo` 在 0x1FC914 上是真寄存器（读 0x03，不是 0xff），200 ms 内不变 0；
  `crc post 0x04` = done=1、ilm/dlm 标志 0，`R_ILM` 等于容器 ILM 校验和 `0xe3173a51`，
  `R_DLM` 读到 `0xcd21b90f`——那是容器里**header 段**的校验和（最后写的段），
  说明这两个 R_ 寄存器更像"最后拷贝的段"，不是按我们的写入顺序配对；这颗芯片没开
  BLD CRC enable，所以不拦启动。
  fwinfo 报 "broken" 是我们自己的解码问题（这个 map 的 max_x/max_y 不在 0x04 起），
  `10 fingers / 0 buttons / chip 0x00` 同样是解码偏移不对——只影响手指标定，不影响启动。
  下一步在**上报路径**：`report thread up, INT on gpio21` 后面还没有 `evt@125800` 行。
  镜像 `shim/out/shim-boot-touch42.img`（sha256 `4fa9606c…`）。

- **2026-10-25 触摸第 53 步：根因是地址表用错了芯片族——拿到 NT36532 自己的 map（touch42）**：
  touch41 上机：不再 panic，六段全写完（ILM 39.4 s、DLM 27.5 s、两段 info 各 91/88 ms），
  但 `txinfo 0xff@200`、`crc post 0xff`、复位状态两个基址都 `0xff`，芯片没起来。
  **真正原因是整张地址表都用了 NT36523 族**（`0x2FE00`/`0x3F10D`/`0x3F7F1`/`0x3F128`…），
  这颗 NT36532 根本不是那套地址——这解释了 touch39–41 里所有"读回 0xffffff / 永远不清 /
  写了没反应"，而 SRAM 回读能对是因为那是写 SRAM，不走寄存器。
  找到真表的过程（可复现，写进 `refs/novatek/nt36532_vendor_map.md`）：TWRP 设备树里的
  `nt36532_touch.ko`（未 strip）可以从 GitHub 直接下
  （`ymdzq/device_xiaomi_liuqin-TWRP`，`prebuilt/liuqin/vendor/lib/modules/1.1/`，
  sha256 `403d1fcc…`），里面有 `NT36532_cascade_memory_map`（200 字节）和 `NT36532_hw_info`
  （hw_crc=2）；字段语义不靠猜结构体顺序，而是看模块自己的代码读了 struct 的哪个偏移
  （`nvt_boot_ready` +132/+136、`nvt_tx_auto_copy_mode` +140、`nvt_check_spi_dma_tx_info` +144、
  描述符写入 +152/+156/+164/+168/+172/+176、CRC 读取 +180/+184/+192）。
  关键差异：事件缓冲 `0x125800`（复位状态 `0x125860`、host cmd `0x1258D0`、FWINFO `0x125878`）、
  `BOOT_RDY=0x1FB50D`、`TX_AUTO_COPY_EN=0x1FC925` 且 **hw_crc=2 要写 0x56**、
  `SPI_DMA_TX_INFO=0x1FC914`、ILM/DLM 描述符与校验和在 `0x1FB5xx`、软复位地址 `0x1FB43E`，
  而且官方 NT36532 流程**没有** BLD CRC enable 这一步。
  touch42 = 补丁 `0190-liuqin-touch-nt36532-map.patch`：换掉全部下载地址、描述符写在
  tx_auto_copy 之前、去掉 BLD CRC enable、fw_crc_enable 用 7/3 字节 + `0xD0`、
  复位状态轮询和报告线程都改到 `0x125800`/`0x125860`、FWINFO 改到 `0x125878`。
  镜像 `shim/out/shim-boot-touch42.img`（sha256 `4fa9606c…`，内核 `77ceb679…`，payload
  `b1b69aa3…`）——**已编译，未上机**；0190 在已应用 0189 的干净树上 check 通过，
  应用后与编译用的驱动逐字节相同。上机看屏幕最后一条 `touch fw ...`。

- **2026-10-25 触摸第 52 步：touch40 上机——四段全部写完、CRC 预读有结果，然后被我们自己的 memcpy 打断（touch41）**：
  touch40 屏幕上的 panic 屏幕照给了全部信息：
  1）**下载完整跑完**：`part 14 @0x114108` / `part 15 @0x114208` 各 88/89 ms（touch39 从没走到这两段），
  `sec 2` 在 133.213 s、`sec 4` 在 133.391 s，即整包 201924 字节只用了约 53 s。
  2）**panic 是我们自己的 bug**：`tch_fw_reg_write()` 里 `uint8_t buf[6]`，而 cascade 的 reset-status
  清零是 7 字节写（`memcpy(buf+1, data, 6)`），DEVELOPMENT 内核带 UBSan，`__memcpy_chk` 直接
  panic：`object size check failed: ... (5 < 6) @ubsan.c:716`。改成 `buf[8]`（补丁 0189）。
  **这也是第一次越过下载、进入 CRC 使能/boot_ready 序列的一次**——但它在 `boot_ready` 之前就死了，
  芯片根本没被要求启动。
  3）`fw dl crc pre: txinfo 0xff@200 done 1 ilm 1 dlm 1 | ilm got ffffffff | dlm got ffffffff`：
  `SPI_DMA_TX_INFO` 在数据已确认写入 SRAM 的情况下**依然读到 0xff**，和描述符寄存器一样像是
  "这颗芯片没实现该寄存器"，所以这个轮询在 NT36532 上没有判据价值（浪费 200 ms 而已）。
  而 CRC 标志是在 **boot_ready 之前**读的——官方是在 boot 失败之后才 `nvt_read_bld_hw_crc()`，
  bootloader 要到尝试启动时才填这些标志和 `R_` 校验和（所以 `got ffffffff` 不奇怪）。
  touch41 因此**在 boot 尝试之后**再读一次，结论行里记作 `crc post`。
  另外这几个标志位"1 是 pass 还是 error"官方源码没说（隔壁 `DMA_CRC_FLAG` 是 bit0=1 表示 error），
  所以真正的判据是芯片自己起没起：`touch fw BOOTED`。
  镜像 `shim/out/shim-boot-touch41.img`（sha256 `f10ea7b3…`，内核 `d0eaad01…`，payload
  `cd900db6…`）——**已编译，未上机**；0189 在已应用 0188 的干净树上 check 通过，应用后与编译用的
  驱动逐字节相同。上机后看屏幕最后一条 `touch fw ...`：`BOOTED` / `did not boot` / `FAIL`。

- **2026-10-25 触摸第 51 步：把下载进度画到屏幕上（touch40）**：
  touch39 那次是**在结论前 13 秒断电**——因为 `liuqinmute=1` 时屏幕上看不到任何 TOUCH 行，
  没人知道下载还在跑。touch40 只加“看得见的进度”，下载逻辑一行没改（补丁
  `0188-liuqin-touch-fw-progress-marks.patch`，`tch_visible_mark()`）：
  `touch fw dl start` → `touch fw sec 1..4`（每段一次，`+N ms` 是距上一条的耗时）→
  `touch fw crc done <d> ilm <i> dlm <m>` → `touch fw BOOTED` / `touch fw did not boot`
  （写失败时 `touch fw FAIL`）。**结论现在直接写在屏幕上，不用等拉日志。**
  实测速度来自 touch39：0.33 ms/字节，四段共 201924 字节 ≈ 67 s（touch40 实测整包 53 s）。镜像
  `shim/out/shim-boot-touch40.img`（sha256 `bd5ac9b4…`，内核 `74dcd553…`，payload `b4724b6b…`）
  ——**已上机**：标记全部按预期出现，下载跑完，随后 panic（见第 52 步）；0188 在 186 个补丁的
  干净树上 check 通过，应用后与编译用的驱动逐字节相同。
  上机流程：开机 → 等屏幕上 `touch fw ...` 最后一条（约 2.5 分钟）→ 断电 → 进 recovery 拉日志。

- **2026-10-25 触摸第 50 步：touch39 上机——写掩码修复确认生效，日志在结论前 13 秒被拔（半段结果）**：
  上机镜像正是 touch39（日志里有只有 0187 才有的 `fw dl:` / `fw dl bld:` / `fw dl part 0 … 2 chunk`
  / `fw dl readback` 四行）。**关键在于 `fw dl readback @0x00000: 48 00 01 a2 eb 02 00 00 |
  file 48 00 01 a2`——SRAM 回读与文件逐字节相同**，这是第一次在这颗芯片里看到固件数据，
  写掩码（`|0x80`）这条修复算实锤。另外：
  1）`fw dl bld: ilm des 0xffffff len 0xffffff | dlm des 0xffffff len 0xffffff`：四个描述符寄存器
  回读全 ff。写路径同一套掩码、同一套帧格式又刚被 readback 证明可用，所以更可能是这些寄存器
  只写不可读，而不是写失败；真正的判据还是 CRC 行。
  2）日志到 boot 133.6 s 就结束（`fw dl part 0` 在 119.3 s 完成，之后只有 14 s），全库没有
  `part 1`、没有 `fw dl crc`、没有结论行，说明**下载还在跑就断电去 recovery 拉日志了**。
  3）实测速度：119228 字节用了 39609 ms = **0.33 ms/字节（约 25 kHz，40 µs/bit）**，整个
  201924 字节约 67 s，结论行落在 boot ~150 s。下次**开机后等 4 分钟**再断电，否则会重演这次。
  日志里那些 `firmware did not report ready`、`evt@11a00`、`cand 0x1f64e moved` 是**上一轮
  旧内核**留在 4 MiB 环里的内容（touch38 在 boot 126.26 s 的结论），不是这次的结果——环的
  有效数据只到 `total`，后面是旧数据。
  下一步：同一张 touch39 镜像再跑一次，等到 `fw dl BOOTED:`/`fw dl did not boot:` 出现
  （boot ≥150 s）再断电进 recovery 拉日志；判读表在 `docs/TOUCH_NEXT.md` 顶部。

- **2026-10-25 触摸第 49 步：根因是 SRAM 负载的地址字节少写了 bit 7（touch39）**：
  按 TOUCH_NEXT 的三步做完，第 2、3 步直接暴露根因。**依据是 `refs/novatek` 的官方源码，
  不是在机器上试出来的**，所以下面每条都标了出处：
  1）**`tch_fw_write_sram()` 的地址字节是 `addr & 0x7f`，没有 bit 7**，整块负载在 SPI 上成了
  "读"命令，SRAM 从来没被写入——于是描述符正常、序列全部跑完、`SPI_DMA_TX_INFO` 一直不清、
  芯片永远不 ready。官方 `nvt_write_sram()` 用 `CTP_SPI_WRITE(fwbuf, len+1)`
  （`nt36xxx_fw_update.c:687`），而 `CTP_SPI_WRITE` 对 `buf[0]` 做 `SPI_WRITE_MASK()`（`|0x80`，
  `nvt_nt36xxx.h:444` / `nvt_nt36xxx.c:392`），和 `nvt_write_addr()` 完全一样。
  已改成 `(addr & 0x7f) | 0x80`。注意 `tools/TOUCH_FW_RE` 里的反编译伪码写的是
  `buf[0] = addr & 0x7f`，掩码在 `CTP_SPI_WRITE` 内部——这就是当初漏掉它的原因。
  2）**漏写最后一个 info 段**：官方解析器（`nt36xxx_fw_update.c:458`）在认出 header 记录
  （源偏移 < `end` 且长度非 0）之后，cascade 的最后一个 info 记录从 `end - 0x10` 读，而不是
  `0x30 + 0x10 * n`。老公式在 `0x30+0x10*13` 读到的是诱饵（源 `0x200`、长度 0），
  头部的后半段（源 `0x100` → SRAM `0x114208`，0xff 字节）一次都没写过。现在会写：
  4 段 / 6 个 chunk（原来 3 段 / 5 个）。
  3）**`nvt_boot_ready()` 对硬件 CRC 的芯片不能清 bit**：官方只在 `hw_crc == 0` 时写 0 和
  POR_CD `0xA0`（`nvt_nt36xxx.c:536`），这一族 `hw_crc = 2`，原来写 0 等于取消刚发出的启动请求。
  4）`nvt_fw_crc_enable()` 的 cascade 分支是 7/3 字节写（清 6 字节 reset 记录 + `0xAE,0x00`），
  原来 2/2；这一条是从 cascade 分支推的，官方对这颗 id 并没有表项，若 CRC 对却仍不 ready，
  就回到 2/2 对照一次。`nvt_spi_dma_setup()` 确认**不需要**：`fw[0x29] == 0x00`，官方同样不调用。
  日志改成单行紧凑格式：起始行、每段一行、描述符回读一行、CRC 判定一行、结论一行，结论行里
  重复 txinfo / CRC 标志 / 两个校验和（丢行也能判读）；新增 `fw dl readback @0x0` 直接读回 SRAM
  头 8 字节，比 CRC 引擎更早回答"数据落没落"。
  镜像 `shim/out/shim-boot-touch39.img`（sha256 `5d557c11…`，内核 `a02455c5…`，payload
  `67748796…`）——**已上机（半段日志，结果见第 50 步）**；`tools/verify-patches.sh` 186/186
  全过（含 0187）。
  下一次上机看：`fw dl crc` 行 `done 1 ilm 1 dlm 1` 且 `got == exp` = 数据对了，随后应有
  `fw dl BOOTED:`；`readback` 与文件一致但 `txinfo` 不清 = 描述符/拷贝引擎；CRC 全对但仍不
  ready = 第 4 条或事件缓冲基址（`0x2FE00`/`0x2FD00`，两个都轮询）。

- **2026-10-25 触摸第 48 步（交接）：触摸状态基线，新会话从这里接**：
  已完成并验证：芯片识别（NT36532 cascade，trim id `20 00 00 32 65 03`）、引脚
  （cs19/clk18/mosi17/miso16）、SPI 帧格式（页 15/7、页写独立 CS、地址后 1 个 dummy）、
  固件随 payload 进内核（fwarea 0x2B000，`liuqin_fw_get`）、以及**完整的固件下载实现**
  （`tch_fw_download`/`tch_fw_write_sram`/`tch_fw_bld_bank`：描述符 → 0xFC00 分块写 →
  BLD/DMA CRC 使能 → boot_ready → 状态轮询）。未解决：下载跑完后芯片仍不 ready，
  `SPI_DMA_TX_INFO(0x3F7F1)` 不清，自动拷贝引擎未完成。
  **下一步（已写进 `docs/TOUCH_NEXT.md` 顶部）**：1）下载日志改单行紧凑格式（当前多行打印
  会被交错丢行）；2）读 `BLD_ILM_DLM_CRC_ADDR(0x3F133)` 的 crc_done/ilm/dlm 标志与
  `R_ILM/R_DLM_CHECKSUM(0x3F120/0x3F124)`，和文件里的 `0xE3173A51`/`0x667268CF` 对比，
  判断数据是否落对；3）核对官方 `nvt_spi_dma_setup()`（cascade 分频）是否需要。
  参考材料都在仓库里：`refs/novatek/*`（Google 官方源码）、`docs/TOUCH_FW_RE.md`（容器格式 +
  写入配方 + 验收值 `fw_ver=0x19`/`PID=0x5973`/`38×60`）、本文件 §8 全部历史。
  当前最新镜像：`shim/out/shim-boot-touch38.img`（sha256 `022d6cb2…`，内核 `4e907172…`，
  payload `d31b965e…`）——已上机，结论是"描述符写入成功、序列跑完、芯片不 ready"。

- **2026-10-25 触摸第 47 步：描述符已写、序列跑完，芯片仍不 ready；且日志会丢行（touch38 结果）**：
  touch38 上机：`bld descriptors set (ilm 0x0/0x1d1bb crc 0xe3173a51, dlm 0x100000/0x14107 crc 0x667268cf)`
  出现，chunk 1/2/3 也都写了；但日志里**缺 chunk 4/5 与 `fw part 14`**，而时间线（chunk 3 在
  ~120 s、verdict 在 ~126 s）恰好等于 2 µs pace 下 DLM 两块的用时——说明**这些行是被日志交错/
  截断丢掉的，不是没执行**（同段日志里还能看到整行空白的损坏行）。最终仍是
  `firmware did not report ready`，且 `dma tx info` 没打出 clear。
  下一步建议：1）把下载过程改成**单行带计数的紧凑日志**（避免交错丢行），并读
  `BLD_ILM_DLM_CRC_ADDR(0x3F133)` 的 `crc_done/ilm_crc_flag/dlm_crc_flag` 与
  `R_ILM/R_DLM_CHECKSUM(0x3F120/0x3F124)` 判断数据是否真的落对；2）对照官方的
  `nvt_spi_dma_setup()`（cascade + `spi_dma_div_cnt_val` 时才调用）看是否还需要配 DMA 分频；
  3）若 CRC 标志显示 DLM 校验失败，则重点查 DLM 段的写入地址/长度。

- **2026-10-25 触摸第 46 步：补齐 cascade 的 BLD 描述符与 CRC 使能（touch38）**：
  按官方 `nvt_set_bld_crc_bank` / `nvt_set_bld_hw_crc` / `nvt_bld_crc_enable` / `nvt_fw_crc_enable`
  实现：写入前先给 ILM/DLM 写描述符（`0x3F128`←sram、`0x3F118`←len、`0x3F100`←crc；
  DLM 为 `0x3F12C/0x3F130/0x3F104`，每个都是"页命令 + [reg|0x80, 数据]"两次 CS 事务），
  写完 SRAM、轮询 `SPI_DMA_TX_INFO(0x3F7F1)` 之后：读改写 `BLD_CRC_EN(0x3F30E)` 置 bit7、
  清 `EVENT_MAP_RESET_COMPLETE(0x2FE60)`、向 `EVENT_MAP_HOST_CMD(0x2FE50)` 写 `0xAE`，
  最后 `BOOT_RDY(0x3F10D)` 写 1→5 ms→0，再轮询状态到 `0xA0..0xAF`。
  新增日志：`bld descriptors set (ilm … dlm …)`、`dma tx info clear after N polls`、
  `firmware booted, state ready after N polls`。镜像 `shim/out/shim-boot-touch38.img`
  （sha256 `022d6cb2…`，内核 `4e907172…`，payload `d31b965e…`）——**已编译，未上机**。

- **2026-10-25 触摸第 45 步：auto-copy 打开了、5 块写完，但 `dma tx info` 一直不清（touch37 结果）**：
  touch37 上机：`tx auto copy mode enabled` → `fw chunk 1..5` 全部写完 → **没有**出现
  `dma tx info clear`（200×1 ms 内 `0x3F7F1` 一直非 0）→ `firmware did not report ready`，
  `fwinfo` 仍是乱值。也就是说 cascade 的自动拷贝引擎没有把数据"交接完成"，boot_ready 自然无效。
  缺的是官方在写入前做的 **`nvt_set_bld_hw_crc()`**（给 bootloader 写 ILM/DLM 描述符，自动拷贝引擎
  要靠它知道拷贝目标）以及随后的 `nvt_bld_crc_enable()` / `nvt_fw_crc_enable()`。
  需要的寄存器（NT36523N map，已确认）：
  `ILM_DES_ADDR=0x3F128`、`ILM_LENGTH_ADDR=0x3F118`、`G_ILM_CHECKSUM_ADDR=0x3F100`、
  `DLM_DES_ADDR=0x3F12C`、`DLM_LENGTH_ADDR=0x3F130`、`G_DLM_CHECKSUM_ADDR=0x3F104`、
  `BLD_CRC_EN_ADDR=0x3F30E`、`DMA_CRC_EN_ADDR=0x3F136`、`BLD_ILM_DLM_CRC_ADDR=0x3F133`、
  `DMA_CRC_FLAG_ADDR=0x3F134`；官方实现分别在同仓库
  `nt36xxx_fw_update.c:817 nvt_set_bld_crc_bank / :861 nvt_set_bld_hw_crc` 与
  `nt36xxx.c:471 nvt_bld_crc_enable / :496 nvt_fw_crc_enable`。
  下一步（touch38）：照着这三处把描述符寄存器写好、开 BLD/DMA CRC、清 reset status，再做 boot_ready。

- **2026-10-25 触摸第 44 步：改用 Cascade（NT36523 族）地址后重新烧机（touch37）**：
  按上一步的结论改了四处：`BOOT_RDY_ADDR = 0x3F10D`（不再写 POR_CD）、
  写入前 `nvt_write_addr(0x3F7E8, 0x69)`（TX auto-copy 模式）、写入后轮询
  `SPI_DMA_TX_INFO = 0x3F7F1` 到 `0x00` 再做 boot_ready、事件缓冲改回 `0x2FE00`。
  日志会新增 `tx auto copy mode enabled` 与 `dma tx info clear after N polls` 两行，
  之后是 `firmware booted, state ready after N polls`（成功）或
  `firmware did not report ready`。镜像 `shim/out/shim-boot-touch37.img`
  （sha256 `b95b67dc…`，内核 `39d17d2b…`，payload `f3e55395…`）——**已编译，未上机**。

- **2026-10-25 触摸第 43 步：固件 5 块全部写完，但没起来——**地址用错了芯片族**（touch36 结果 + 下一步）**：
  touch36 上机完整走完下载：
  `fw chunk 1 @0x00000` → `chunk 2 @0x0fc00` → `part 1` → `chunk 3 @0x100000` →
  `chunk 4 @0x10fc00` → `part 14` → `chunk 5 @0x114108`，随后 `firmware did not report ready`。
  **写入本身成功了**，问题在收尾地址：`firmware booted` 依赖 `nvt_boot_ready` 的两个寄存器，
  我按 NT36525 的 map 用了 `BOOT_RDY 0x1F141 / POR_CD 0x1F61C`；但这颗是 **Cascade**
  （vendor 日志明说 "Cascade Chip"，trim 表里的 0x3F004/0x3F0FE/0x3F135 也都在 0x3Fxxx 区），
  对应的是 **NT36523/NT36523N 的 map**：
  `EVENT_BUF_ADDR = 0x2FE00`、`BOOT_RDY_ADDR = 0x3F10D`、`ILM_DES_ADDR = 0x3F128`、
  **`TX_AUTO_COPY_EN = 0x3F7E8`**、**`SPI_DMA_TX_INFO = 0x3F7F1`**（NT36523 map 没有 POR_CD 字段）。
  所以下一步（touch37）要改三处：
  1）`NVT_BOOT_RDY_ADDR` 改成 **0x3F10D**，去掉 POR_CD 写入（map 里没有该字段）；
  2）写入前执行 cascade 的 **`nvt_tx_auto_copy_mode()`：`nvt_write_addr(0x3F7E8, 0x69)`**；
  3）写入后轮询 **`SPI_DMA_TX_INFO(0x3F7F1)` 直到读到 0x00**，再做 boot_ready；
  4）事件缓冲也应按 cascade 用 **0x2FE00**（而不是 0x11A00）。
  另外可选的完整路径：`nvt_set_bld_hw_crc()`（写 ILM/DLM 描述符寄存器，`ILM_DES_ADDR=0x3F128` 等）
  → `nvt_bld_crc_enable()` → `nvt_fw_crc_enable()`，让 bootloader 校验通过后再启动。

- **2026-10-25 触摸第 42 步：真相是"用户两分钟就关机"，不是挂死；touch36 加速 + 进度（touch36）**：
  用户说明 touch35 那次只开了 **两分钟** 就断电了——所以日志里的 4.4 小时是**环形缓冲里上一轮**
  的内容，实际是：57 s 开始写 ILM（约 52 s 完成），109 s 开始写 DLM，120 s 左右断电，DLM 还差
  一分多钟。**没有挂死**，只是慢 + 没有进度提示。touch36 因此：
  1）下载期间把 `bb_pace_us` 从 5 降到 **2**（约 2.5 倍加速），结束/出错时恢复；
  2）每块打一行 `fw chunk %u @sram 0x%05x len 0x%x`，能直接看出进度与卡点。
  镜像 `shim/out/shim-boot-touch36.img`（sha256 `fdb0ce21…`，内核 `8f225de5…`，
  payload `3d734a54…`）——**已编译，未上机**。预期总写入时间从 ~4 分钟降到 ~1.5 分钟；
  判断标准是出现 `firmware booted, state ready after N polls`（成功）或
  `firmware did not report ready`（写完了但没起来）。

- **2026-10-25 触摸第 41 步：下载开始真正写入，但 DLM 段之后线程再无输出（touch35 结果）**：
  touch35 上机：`fw download: end 0x200 cascade 1 info 14 parts 16 size 245760` →
  `fw part 0: sram 0x0 len 0x1d1bb bin 0x200`（ILM 写入门槛已过低，256 字节分段生效）→
  **约 52 秒后** `fw part 1: sram 0x100000 len 0x14107 bin 0x22000` 开始，**此后再无任何 TOUCH
  输出**，而系统继续正常运行了 4.4 小时（log 尾部仍是其它进程）。说明 setup 线程卡在 DLM 段的
  写入路径里（不是崩溃，也不是失败返回），而且实际速率比估算慢约 3 倍（`delay()` 精度）。
  下一步：在 `tch_fw_write_sram` 里按块加进度日志（`chunk k/n @sram`），并给每块加重试/超时上限，
  先定位是"慢"还是"真的挂死"；同时把每个 256 字节分片的 `tch_xfer` 返回值和 CS 读回记下来。

- **2026-10-25 触摸第 40 步：下载启动即失败——`tch_xfer` 单次限制 256 字节（touch35）**：
  touch34 上机：`fw download: end 0x200 cascade 1 info 14 parts 16 size 245760`、
  `fw part 0: sram 0x0 len 0x1d1bb bin 0x200` 都对（`len = size + 1` 与官方一致），但紧接着
  `fw part 0 write failed`。原因：`tch_xfer()` 在软件 SPI 分支里有 `len > 256 → -1` 的保护，
  而每块是 `0xFC00`。修法：**在同一个 CS 窗口内**把负载按 256 字节分段发送（SPI 上 CS 之间的
  字节流对从机是连续的一条消息，分段不改变协议）。镜像 `shim/out/shim-boot-touch35.img`
  （sha256 `b3e113a4…`，内核 `f628c6dc…`，payload `ae316617…`）——**已编译，未上机**。
  预期：`fw part 0/1/12` 依次写完（100 kHz 下 245 KB 约 30–60 秒），随后
  `firmware booted, state ready after N polls` 与真实的 `fwinfo`（`ver 19/~e6`、`38×60`）。

- **2026-10-25 触摸第 39 步：固件下载已实现（touch34）**：
  拿到 Google `novatek_touch` 模块源码后，所有未知量都确定了：
  `BOOT_RDY_ADDR=0x1F141`、`POR_CD_ADDR=0x1F61C`、`EVENT_BUF_ADDR=0x11A00`、
  `NVT_TRANSFER_LEN=0xFC00`，容器格式（12 字节 ILM/DLM 记录 + 0x30 起 16 字节 info 记录、
  cascade 用 `end/2` 算 info 数）也来自官方 `nt36xxx_fw_update.c`（源码存 `refs/novatek/`）。
  `liuqin_touch.c` 新增：`tch_fw_write_sram()`（每 0xFC00 一次页写 + 一次 CS 事务的数据阶段）、
  `tch_fw_download()`（解析容器 → `0x69` 复位 → 逐段写 → `0x1F141=1`/5 ms/`=0` + `0x1F61C=0xA0`
  → 轮询事件 `0x60` 到 `0xA0..0xAF`）、`tch_fw_state_ready()`；并在 `tch_novatek_info()` 里
  「识别到 id 且状态未 ready 时」自动下载一次。验收：日志出现
  `fw download: end 0x200 cascade 1 info 14 parts 16 size 245760`、三段
  `fw part N: sram … len … bin …`、最后 `firmware booted, state ready after N polls`，
  随后 fwinfo 应变成真实值（`fw_ver=0x19`、`x_num=38`、`y_num=60`）。镜像
  `shim/out/shim-boot-touch34.img`（sha256 `e914ddaa…`，内核 `738ce383…`，
  payload `1b2f3d92…`）——**已编译，未上机**。这是整条触摸链上最后一块功能代码。

- **2026-10-25 触摸第 38 步：装了 angr 反编译器，下载协议已反编译确认（见 `docs/TOUCH_FW_RE.md`）**：
  这台机器没有 Java/Ghidra/brew，用 `python3 -m venv /tmp/revenv && pip install angr` 装好了
  angr 9.2.102（自带 C 反编译器，纯 Python）。用 `angr.Project(..., main_opts={'base_addr':0})`
  + `CFGFast(function_starts=[...])` + `analyses.Decompiler` 反编译出两个关键函数
  （注意：ELF 虚拟地址 = objdump 的 `.text` 偏移 + 0x41000，例如 `nvt_write_firmware` 在 0x4eca4）：
  1）`nvt_write_firmware`：28 字节 section 表（+12 文件偏移 / +16 目标地址 / +20 长度），
     每块 `min(size, 0xFC00)`，先 `nvt_set_page(addr)`，再单次 CS 事务写
     `[addr & 0x7F] + 最多 0xFC00 字节`，地址/偏移按 0xFC00 推进；
  2）`nvt_boot_ready`：`nvt_write_addr(addr1, 1)` → `__const_udelay(21475000)` →
     若面板未激活：`nvt_write_addr(addr1, 0)`、`nvt_write_addr(addr2, 0xA0)`；
     `addr1/addr2` 是 per-chip hw_info 结构体 +132/+136 两个字段（尚未映射回绝对地址）。
  剩余唯一未知：这两个字段的绝对地址 + `nvt_update_firmware`（0x4d22c）里容器→section 表的
  解析（angr 归一化时触发 ZeroDivisionError，需要换反编译器或继续手读汇编）。

- **2026-10-25 触摸第 37 步：固件容器格式的初步解析（下一步接着解）**：
  对 `novatek_nt36532_m81_fw_csot.bin`（245760 字节）做结构性观察，得到以下**待验证假设**：
  1）前 **0x100 字节是头部**，紧接着 0x100 处又有一份类似头（`00 02 00 00 | 0 | bb d1 01 00 | 00 20 02 00`）；
  2）头部里 0x08 = `0x1D1B0`(119216)、0x0c = `0x22000`(139264)、0x10 = `0x100000`、0x14 = `0x14107`、
     0x30 起是 **16 字节一条的 info 记录**（`{addr, 0, size, crc32}`），非零的有三条：
     `addr 0x1132D0 size 0x352D0`、`addr 0x100000 size 0x22000`、`addr 0x104400 size 0x26400`，
     第 12 条（0xf0）是 `addr 0x114108 / 0xFF` 的信息段标记；"info_sec_num = 14" 与 14 条记录吻合；
  3）vendor 日志里的 `ilm_dlm_num = 2` 对应其中两条主体段（ILM @0x100000 + DLM）。
  接下来要把这几条与 `nvt_write_firmware` 里"28 字节 section 表（+12 偏移/+16 地址/+20 长度）"
  的字段对齐——即**驱动解析后生成的表**是从这个 0x100 头 + 记录表展开来的，两者字段顺序不同。
  解出映射后即可实现：按表把每段数据从文件偏移拷贝出来，按 ≤0xFC00 分块 `set_page`+写，
  最后 `nvt_boot_ready` + 状态轮询。

- **2026-10-25 触摸第 36 步：搬运链路验证通过（touch33 结果），下载只差固件容器格式**：
  touch33 上机：`LIUQIN FW: novatek_nt36532_m81_fw_csot.bin 245760 bytes`、
  `LIUQIN TOUCH: touch firmware 245760 bytes at 0xfffffe583a093000 (00 02 00 00 00 00)`、
  `fw state @0x11a60: d6 dd 77 a8 (not ready)` —— **固件已随 payload 进内核、驱动取到**，
  芯片状态确实不是 `0xA0..0xAF`（应用固件没跑），与"必须下载"的判断一致。
  下载本体还差的最后一块是**固件容器格式**：写入端已经清楚（28 字节 section 表：+12 文件偏移、
  +16 目标地址、+20 长度；每块 ≤0xFC00，`nvt_set_page(addr)` 后
  `CTP_SPI_WRITE([0x80|(addr&0x7F), data…])`；最后 `nvt_boot_ready`（写 1→延时→写 0→写 0xA0）
  再由 `nvt_check_fw_reset_state` 轮询），但 section 表是**驱动在运行时**从 `nvt_update_firmware`
  （内联的 `nvt_bin_header_parser`）里解析出来的，固件文件自己的布局还没有对上；
  已尝试按候选偏移扫描 section 表但没有匹配（说明实际布局不是"裸 28 字节表"）。
  已经拿到一份 vendor 源码（`Rasenkai/caf-tsoft-Novatek-nt36xxx` 的 `nt36xxx.c`，3372 行，
  含 `nvt_set_page`/`nvt_check_fw_reset_state`/`nvt_get_fw_info`，但**不含**固件下载部分）；
  下一步要么继续从 `.ko` 里反汇编 `nvt_update_firmware` 的解析段，要么找一份带 `nvt_update_firmware`
  的 vendor 源码（NT36532/NT36523 的 Xiaomi 分支）。

- **2026-10-25 触摸第 35 步：固件已进入 fwarea，驱动能取到并读状态（touch33，固件下载的第一步）**：
  把 `novatek_nt36532_m81_fw_csot.bin`（245760 字节）放进 `~/liuqin-root/fw/`，用
  `tools/mkfwarea.py` 重建 `fwarea.bin`——现在它作为 LIUQINFW 的第 6 个文件挂在
  **offset 0x2B000**（fwarea 8 MiB，sha256 `bdcb253b…`）。驱动侧新增：
  1）`liuqin_fw_get("novatek_nt36532_m81_fw_csot.bin", &size)` 取镜像并打印大小/前 6 字节，
     证明固件真的随 payload 到了内核；
  2）只读的固件状态轮询：在两个候选基址各读 `+0x60` 的状态字节，`0xA0..0xAF` 判定
     "running"，否则 "not ready"（对应 upstream 的 `nvt_check_fw_reset_state`）。
  这是固件下载的**第一步**（搬运 + 状态判读），不改动任何写操作，可单独验证。
  镜像 `shim/out/shim-boot-touch33.img`（sha256 `cf4f3dfb…`，内核 `f0340fbd…`，
  payload `f21bda43…`）——**已编译，未上机**。
  下载协议本身的关键信息（从模块 `nvt_write_firmware`/`nvt_bin_header_parser` 反汇编得到）：
  固件按 **28 字节的 section 表**描述，每项 `+12` = 文件内偏移、`+16` = 目标地址、`+20` = 长度；
  写入按 **≤0xFC00 字节**分块，每块先 `nvt_set_page(addr)`，再
  `CTP_SPI_WRITE([0x80|(addr&0x7F), data…])`；全部写完后 `nvt_boot_ready`
  （写 1 → 延时 → 写 0 → 写 0xA0）再由 `nvt_check_fw_reset_state` 轮询状态。剩余未知：
  section 表在文件中的起始位置、`nvt_boot_ready` 用的两个寄存器地址（在 vendor 驱动结构体
  +132/+136，尚未映射回具体地址）。

- **2026-10-25 触摸第 34 步（决定性真值）：TWRP 里 vendor 驱动每次启动都会下载固件（touch32 之后的计划）**：
  在能触摸的 TWRP 里读到 `/proc/nvt_fw_version` 与 dmesg，得到两条硬证据：
  1）`update_firmware_request: filename is novatek_nt36532_m81_fw_csot.bin` →
  `nvt_get_fw_need_write_size: fw_need_write_size = 245760(0x3c000)` →
  `nvt_bin_header_parser: cascade_2nd_header_info=1, ilm_dlm_num=2, info_sec_num=14, partition=16`
  → `nvt_get_fw_info: fw_ver=0x19, fw_type=0x01, PID=0x5973`。也就是说 **ABL/TWRP 交接后固件不在，
  必须由驱动重新下载**（这解释了：trim id 读得到、应用内存读不到、不扫描、无 INT）。
  2）下载完成后的**验收值**：`fw_ver=25(0x19), x_num=38, y_num=60, button_num=0`（`/proc/nvt_fw_version`），
  PID `0x5973`；固件文件 245760 字节，与 TWRP 设备树里的
  `prebuilt/liuqin/vendor/firmware/novatek_nt36532_m81_fw_csot.bin` 完全一致。
  实现路径：`nvt_update_firmware → nvt_write_firmware(0xdca4) → nvt_boot_ready → nvt_check_fw_reset_state`。
  `nvt_write_firmware` 的调用序列已确认是 `memset / nvt_set_page / memcpy / CTP_SPI_WRITE(分块) /
  usleep_range / CTP_SPI_WRITE / CTP_SPI_READ`，协议就是"设页 + 分块写 + 触发 + 轮询状态"。
  下一步要做的事：1）把 245760 字节固件带进 payload（最省事是塞进已有的 fwarea 并在 ADT/启动参数里
  给出偏移，GPU 驱动已有从 fwarea 读固件的先例）；2）在驱动里实现分块下载 + `nvt_boot_ready`
  + 状态轮询；3）用 `fw_ver=0x19 / PID=0x5973 / x_num=38 / y_num=60` 作为验收标准，通过后报点、
  坐标标定都水到渠成。

- **2026-10-25 触摸第 33 步：候选基址扫描结果——没有一路跟随手指（touch32 结果）**：
  touch32 上机：5 个候选基址里 `0x1f64e`（`5a 77 82 00 a7 b6 34 8d`）和 `0x11a00`
  （`8a 5a ce 18 75 6b 4d d2`）**各只变化一次**（首次采样到稳定值之间的跳变），之后完全不动；
  `0x3f004`/`0x2fe00` 全 `ff`。也就是说**没有任何候选地址随手指变化**，报点缓冲仍未找到，
  或控制器确实没有在写报点。另外 `0x1f64e` 的块每次启动值都不同（`5a 76 83 00…` /
  `5a 77 82 00…`），格式也不满足 upstream 的 `fw_ver + ~fw_ver == 0xFF`，说明这个页也不是
  FWINFO。结论：**继续靠"猜地址"已经没有效率**，需要外部真值——在能正常触摸的 TWRP 里读
  vendor 驱动暴露的 proc 节点（`/proc/nvt_fw_version`、`/proc/NVTSPI` 等）拿到固件信息与
  寄存器读法的标准答案，或者用逻辑分析仪抓一次 vendor 驱动的 SPI 事务。

- **2026-10-25 触摸第 32 步：`0x11A00` 有数据但格式不对；改为候选基址扫描（touch32）**：
  touch31 上机：`@0x11a00 state d6 dd 77 a8 | fw 0c 98 02 cc 18 55 4c 96 77 (ver 98/~02 unusable)`，
  `@0x2fe00` 仍是全 `ff`；事件块轮询打出一串相同的 `evt@11a00 8e 7a cc 18 75 4b 4d d0`。
  也就是说 `0x11A00` 确实能读到东西，但**不符合任何已知格式**（状态不在 `0xA0..0xAF`、fw 校验失败），
  且不同地址读到的字节序列互相重叠（`cc 18` 同时出现在 fw 和点数据里），像是"地址还不对、
  读到同一条流的错位"。vendor 模块 trim 表里还剩一个可疑字段 `0x1F01A`（对应内存映射那一组）。
  touch32 因此每 100 ms 采样 5 个候选基址（`0x1F01A`/`0x1F64E`/`0x11A00`/`0x3F004`/`0x2FE00`）
  各 8 字节，哪个随手指移动而**变化**就打 `cand 0xXXXXX moved: …`（最多 12 条），
  一次启动就能确定真正的报点缓冲。镜像 `shim/out/shim-boot-touch32.img`
  （sha256 `5f01371e…`，内核 `592fc38e…`，payload `45202624…`）——**已编译，未上机**。

- **2026-10-25 触摸第 31 步（找到地址错误）：事件缓冲应是 `0x11A00` 而不是 `0x2FE00`（touch31）**：
  拿到 upstream nt36xxx 驱动完整源码后确认两处关键事实：
  1）**芯片族内存映射** `{evtbuf, pipe0, pipe1, flash_csum, flash_data}` 对 NT36525/NT36676F 是
  `{0x11a00, 0x10000, 0x12000, 0x14000, 0x14002}` —— **事件缓冲在 0x11A00**，而本移植一直按
  nabu 的 `0x2FE00` 读，读到全 `ff` 完全合理（那是未映射区）；点数据长度 `POINT_DATA_LEN 65`；
  2）**FWINFO 在同一页的寄存器 0x78**（`0x11A78`，17 字节，`fw_ver=buf[1]`、`~fw_ver=buf[2]`，
  校验 `buf[1]+buf[2]==0xFF`，x/y 分辨率在 buf[5..8]），**状态寄存器 0x60** 读回 `0xA0..0xAF`
  才表示固件在跑；resume 流程是 `bootloader_reset` + `check_reset_state(REK)`，**没有固件下载**
  ——所以固件是常驻的，之前的"RAM 加载"推测不成立。touch31 因此：启动识别后打印两个基址
  （`0x11A00` 与旧的 `0x2FE00`）各自的 **state 与 fw** 字节，并在报点线程里同时轮询
  `0x11A00` 的事件块（有非 `00`/`ff` 内容就打印 `evt@11a00 …`）。镜像
  `shim/out/shim-boot-touch31.img`（sha256 `a7746a08…`，内核 `80d56ff7…`，payload
  `e3159428…`）——**已编译，未上机**。

- **2026-10-25 触摸第 30 步：touch28/29 的"快轮询"其实没生效（诊断代码 bug），touch30 修正（touch30）**：
  touch29 的 log 里一条 `block` 行都没有——原因是轮询门控写成 `if (++idle < 64 || polls >= 6000) continue;`，
  63/64 个 tick 都被跳过，而 109 字节块读又放在 `continue` 之后，所以**它一次都没执行**；
  `polls` 也只在真正读的时候才自增，`polls >= 6000` 实际上要跑一小时才成立。也就是说
  touch28/29 上机时仍然是 0.64 秒才读一次。touch30 把门控改成「线程生命周期前 6000 个 tick
  （约 1 分钟）内每个 10 ms tick 都读，之后退回 1 秒一次」，块读和帧读都在这个路径上，
  并删掉了没用的 `idle` 变量。镜像 `shim/out/shim-boot-touch30.img`（sha256 `4ccee25a…`，
  内核 `31f50e3e…`，payload `c5cf085a…`）——**已编译，未上机**。这次才真正验证
  「10 ms 轮询 + 109 字节块」这两条假设；若仍无 `block`/`gpio21 asserted`，
  就按计划实现 `nvt_boot_ready` + `nvt_check_fw_reset_state`（再不行上固件下载）。

- **2026-10-25 触摸第 29 步：10 ms 快轮询 + 全程滑屏仍无 INT/无数据；改为按 vendor 的 109 字节事件块读（touch29）**：
  touch28 上机（用户从开机一直滑到进桌面，覆盖了整个快轮询窗口）：依然只有
  `evt data ff ff ff ff ff ff ff ff`，没有任何 `gpio21 asserted`。与 vendor 读法还剩两处差异、
  都很便宜：1）vendor 的 work func 用 **len=109** 读事件块（1 个计数 + 18 条 6 字节记录），
  我们只读 65，有些控制器在没读满整块时不肯打开事件 FIFO 输出；2）vendor 是在 set_page 之后
  从**地址 0** 读这块。touch29 因此每次轮询都额外读一块 **109 字节**（`block ...` 首次出现时打印
  前 12 字节），其余（10 ms 轮询、INT 边沿、缓冲变化日志）保持不变。镜像
  `shim/out/shim-boot-touch29.img`（sha256 `58621392…`，内核 `31137130…`，
  payload `b1975a04…`）——**已编译，未上机**。若 `block` 一行都不出现，说明控制器确实没在扫描，
  剩下的就是 vendor probe 之后的那套：`nvt_boot_ready`（向 per-chip 地址写 1、0、0xA0）+
  `nvt_check_fw_reset_state`，都不行再上 `nvt_update_firmware`。

- **2026-10-25 触摸第 28 步：手指在锁屏上滑动 10 秒，INT 一次没跳、事件缓冲全是 `ff`（0178 结果 + touch28）**：
  touch27 的 log：`gpio21 in/out 0x0`、全程只有一条 `evt data ff ff ff ff ff ff ff ff`。
  事件缓冲读回全 `ff` 在这颗芯片上表示"未映射/未填充"（安静时应该是 `00`），加上 10 ms 轮询
  也没抓到任何 INT 边沿，说明**控制器根本没有在报点**。trim id 和 chip-info 页仍然能读，所以
  SPI 通路没问题。vendor 的 resume 路径给了线索：`nvt_ts_resume` 会先调
  **`nvt_update_firmware`** + `nvt_check_fw_reset_state`，然后才 `nvt_irq_enable` ——
  应用固件是在 resume 时（重新）加载的，在它跑起来之前芯片只应答寄存器读、不扫描。
  touch28 先补最后一个便宜的空档：无 INT 时事件缓冲原来 1 秒才读一次，可能漏掉帧；
  现在**线程启动后的头 1 分钟每 10 ms 读一次**（之后退回 1 秒），并继续记录每次缓冲变化和
  gpio21 边沿。镜像 `shim/out/shim-boot-touch28.img`（sha256 `d607b693…`，内核 `63a7e73b…`，
  payload `d9ce8d9e…`）——**已编译，未上机**。若这一分钟里 `evt data`/`gpio21` 仍然毫无反应，
  下一步就实现 **`nvt_update_firmware`**（协议在各 vendor 模块里、镜像
  `novatek_nt36532_m81_fw_csot.bin`、传输已验证）。

- **2026-10-25 触摸第 27 步：白苹果与触摸无关（是带 GPU 测试 daemon 的 userdata）；芯片在跑但报点还没出来（0178）**：
  白苹果的真因：刷进 userdata 的是 `ipad71-root-optin.img`，里面带一个 RunAtLoad 的 GPU 测试
  daemon（`com.liuqin.gputest.plist`），系统起来后用户态没接管显示，屏幕停在 ABL 画的苹果。
  刷回 `ipad71-root-nodaemon.img`（同一 root、无 daemon）后**锁屏恢复**；`liuqintouch=0` 也一样
  白苹果，说明与触摸代码无关。touch26 还纠正了上一步的结论：**探测在任何复位之前**时，
  `before reset: chip info 5a 76 83 00`、trim id `20 00 00 32 65 03`、`fwinfo 52 37 03 10 …`
  都是真实数据 ⇒ 应用固件一直在跑，之前的"bootloader 模式"是我们自己的复位脉冲造出来的，
  所以 touch26 不再复位一颗活着的控制器。目前唯一缺口：手指在锁屏上滑动时，报点线程
  **既没看到 INT 跳变、也没解出任何有效帧**。0178 因此加了三组诊断：gpio21 每次电平变化
  打一行（最多 12 次）、事件缓冲前 8 字节每次变化打一行（不管能不能解码，最多 12 次）、
  以及 FWINFO 判据改为「非全 00/ff 即有效」（这颗芯片不用 nabu 的 `fw[0]+fw[1]==0xff` 约定，
  也顺手消灭了 255 fingers/255 buttons 的假值）。镜像
  `shim/out/shim-boot-touch27.img`（sha256 `13547c5a…`，内核 `78f36eee…`，
  payload `d9ff455e…`）——**已编译，未上机**。

- **2026-10-25 触摸第 26 步（识别成功）：读到 NT36532，但芯片处于 bootloader 模式（0177）**：
  touch25 上机：`reset low->high: cs gpio19 clk gpio18 mosi gpio17 miso gpio16 ->
  20 00 00 32 65 03`、`Novatek trim id 20 00 00 32 65 03 (NT36532)` —— **与 vendor 完全一致**，
  地址（0x1fb104）/页事务边界/引脚（cs19 clk18 mosi17 miso16）/dummy 字节全部对齐了。
  但 FWINFO（应用内存 0x1f64e）读回 `ff`、INT 从不跳变 ⇒ **控制器没有在扫描，它停在 ROM
  bootloader 里**，只有 trim id 这类 ROM 区域能应答。注意 id 是**紧跟在 100 ms 复位脉冲之后**
  出现的，正是那个脉冲把它送进了这个状态；而我们的复位变体仍排在「复位前探测」之前。
  0177 因此：1）**把探测移到任何复位之前**，只把 gpio20 拉高不脉冲，读 trim id + chip-info
  页(0x1f64e) + 事件缓冲(0x2fe00)，并明确打印应用固件是否在跑；2）`NVT_FWINFO_ADDR` 改为
  chip-info 页 `0x1f64e`（vendor trim 表里就是它），不再用 nabu 的 0x2fe78；3）只有探测失败
  才做复位脉冲和其余诊断，保证不会再去复位一颗正在跑的芯片。镜像
  `shim/out/shim-boot-touch26.img`（sha256 `a7453576…`，内核 `31169215…`，
  payload `bc7492b3…`）——**已编译，未上机**。若确认是 bootloader 模式，下一步就是
  `nvt_download_firmware`（协议在 vendor 模块里、固件在 TWRP 树里、传输已验证）。

- **2026-10-25 触摸第 25 步（找到真身）：id 出来了，只差一个 dummy 字节（0176）**：
  touch24 的 48 种扫描里出现决定性一行：
  `bitbang 24: cs gpio19 clk gpio18 mosi gpio17 miso gpio16 -> 3f 20 00 00 32 65`。
  这正是 vendor 的 id `20 00 00 32 65 03` **整体早了一个字节** —— 地址和数据之间需要**一个 dummy
  字节**。而且这一行的引脚正好就是设备树自己的分组：**CS = gpio19**（`qup_spi4_cs`），
  16..18 里 **CLK=18、MOSI=17、MISO=16**（之前那次 "cs17" 果然是假象）。
  0176 因此：引脚固定为 `cs19 clk18 mosi17 miso16`；`dummy=1`（读传 `len+2` 字节、
  数据取 `rx[2]`）；其余保持（vendor 的 trim 地址 `0x1fb104`、页写独立 CS 事务、复位前先探测）。
  预期日志：`live probe: cs gpio19 clk gpio18 mosi gpio17 miso gpio16 -> 20 00 00 32 65 03`。
  镜像 `shim/out/shim-boot-touch25.img`（sha256 `b7544f8a…`，内核 `89acd863…`，
  payload `8f4366f1…`）——**已编译，未上机**。

- **2026-10-25 触摸第 24 步：地址对了但读到 `03 ff…` ⇒ 页写必须单独一次 CS（0175）**：
  touch23 用正确的 `0x1fb104` 读出来是 `03 ff ff ff ff ff`（期望 `20 00 00 32 65 03`）。
  关键线索是开头的 `03`——vendor 的 id 正好以 `03` 结尾，其余全 1，这是**读错了页**的典型表现。
  原因在事务边界：vendor 的 `nvt_set_page()` 是**独立的一次 `CTP_SPI_WRITE`**（CS 拉低→发 3 字节→
  CS 释放），之后 `CTP_SPI_READ` 才重新拉低 CS 发地址并读出数据；而我们一直把页字节和读放在
  **同一个 CS 窗口**里，页从来没被当成独立消息锁存。0175 因此把 `tch_nvt_read()` 和
  `tch_nvt_write8()` 都改成**两个 CS 窗口**（页一次、地址+数据一次），bring-up 的
  `0x69/0xA5/0x35` 也走同样的路径。镜像 `shim/out/shim-boot-touch24.img`
  （sha256 `79a9584d…`，内核 `19f1923c…`，payload `e99ecb40…`）——**已编译，未上机**。

- **2026-10-25 触摸第 23 步（决定性）：TWRP 的 dmesg 给出真正的 trim 地址 0x1FB104（0174）**：
  从跑着的 TWRP 里取到三样 dump。`/proc/device-tree` 确认触摸节点是
  `/soc/spi@990000/novatek@0`（`novatek,NVT-ts-spi`、status ok、pinctrl
  pmx_ts_active/pmx_ts_suspend、**没有 supply 属性**）；
  `regulator_summary` 显示面板那几条轨全部 **enabled**（`pm8350c_l2` 1.8 V = DSI iovdd、
  `display_panel_dvdd_buck/ldo`、`vsp/vsn`），**供电不是问题**；
  `dmesg` 里 vendor 驱动的日志直接给出答案：
  `mode=0, max_speed_hz=9600000`、`irq-gpio=322(=tlmm21)`、`lcd_id-gpio=376(=tlmm75)`、
  `This panel is CSOT`，以及
  `nvt_ts_check_chip_ver_trim 1875: chip_ver_trim_addr=0x1fb104, swrst_sif_addr=0x1fb43e,
  crc_err_flag_addr=0x1fb535` 和 `buf[1]=0x20, buf[2]=0x00, buf[3]=0x00, buf[4]=0x32,
  buf[5]=0x65, buf[6]=0x03`。**真正的 trim id 地址是 0x1FB104，值是 `20 00 00 32 65 03`**；
  我们一直读的是 nabu harness 的 `0x3f004`，这颗芯片在那个地址上返回的就是 `ff`——正好是
  我们每次探测看到的全 1。0174 因此把 `NVT_CHIP_VER_TRIM_ADDR` 改为 `0x1fb104`
  （`0x3f004` 保留为次选），判据改为「6 字节里任意相邻的 `65 03` 都算命中」，
  其余（复位前先探测、vendor 帧格式、mode 0、48 种分配扫描）保持不变。镜像
  `shim/out/shim-boot-touch23.img`（sha256 `19b54cf6…`，内核 `2eb844da…`，
  payload `e57b90dd…`）——**已编译，未上机**。

- **2026-10-24 触摸第 22 步：帧格式对齐后芯片仍不驱动 MISO；改为从 TWRP 里取真值**：
  touch22 上机（vendor 帧格式、`dummy=0`、复位前先探测）：所有模式/路径的读数都是
  `03 ff ff ff ff ff | page 01 ff ff ff`——首字节随我们自己的地址相位变（trim 地址 `03`、
  chip-info 页 `01`），其余是空闲高电平。也就是说**协议已经不是瓶颈，芯片在我们这次启动里
  根本没驱动总线**。TWRP 是可用的参考系统，下一步不再猜，而是从 TWRP 里 dump 真值：
  `tar -C /proc/device-tree -cf /tmp/dt.tar .`（拿到 base DTB 合并后的触摸节点、父总线、
  供电属性，这是 mainline DTS 一直缺的部分）、`dmesg | grep -iE 'nvt|novatek|nt36|touch'`
  （vendor probe 的真实顺序、读到的 chip id/fw_ver、有无 firmware 下载）、
  `/sys/kernel/debug/regulator/regulator_summary`（哪条轨给触摸供电、开没开）。
  另外两个零构建实验：TWRP 优先不断电重启后 `fastboot boot shim-boot-touch22.img`
  （它会先探测再复位），以及 `ls /vendor/lib/modules/` 看模块加载方式。

- **2026-10-24 触摸第 21 步（关键）：从 TWRP 的 prebuilt 模块拿到 vendor 驱动，发现我们的读偏移了一个字节（0173）**：
  TWRP 设备树（`ymdzq/device_xiaomi_liuqin-TWRP`）里带着整套 vendor 模块和固件：
  `prebuilt/liuqin/vendor/lib/modules/1.1/nt36532_touch.ko`（未 strip、带符号）以及
  `novatek_nt36532_m81_fw_csot.bin` 等四个固件。反汇编后把所有协议问题都定死了：
  `nvt_set_page` 写 `[0xff, addr>>15, addr>>7]`（我们的 15/7 是对的）；
  `nvt_write_addr` 在 page 之后写 `[(addr&0x7f)|0x80, val]`（高字节位由 CTP_SPI_WRITE 置，
  我们也对）；**`CTP_SPI_READ(buf,len)` 只传 `len+1` 字节、数据从 `buf[1]` 取——地址之后没有
  dummy 字节**，而我们一直取 `rx[2]`，**每次读到的数据都整体偏移了一个字节**，这正是 id 永远
  对不上、数值「看着合理但不对」的原因。另外 `spi_setup` 用的是 mode 0、8 bit（与我们相同）；
  probe 只读 `novatek,irq-gpio`/`lcd_id-gpio`、应用 DT 的 pinctrl state，**不驱动任何 reset
  GPIO、也不碰 regulator**；它确实会调 `nvt_download_firmware`（复位后要重新下载 RAM 固件）。
  0173 因此把读改成 vendor 的帧格式（`dummy=0`，共 `len+1` 字节、数据从 `rx[1]`），bring-up
  延时也对齐 vendor（0x69 后 18 ms、0xA5 后 15 ms、0x35 后 10 ms），id 判据接受 `65 03` 在
  `[0..1]` 或 `[3..4]`。镜像 `shim/out/shim-boot-touch22.img`（sha256 `3b36b920…`，
  内核 `df0feb01…`，payload `a8713339…`）——**已编译，未上机**。

- **2026-10-24 触摸第 20 步：MISO 从未被驱动，「应答」是假象；改为「先探测、后复位」（0172）**：
  touch20 的矩阵结论很干净：**4 种 SPI 模式 × 5/25 µs、加上拉，MISO 全部读成 `ff`** ——
  控制器根本没驱动这根线。之前 sweep 里 combo 10 的非零字节是**焊盘残留**（它的邻居几微秒前
  还在驱动它），矩阵在干净状态下跑就是空闲高电平。所以本 session 里芯片从未真正应答过。
  而 TWRP 能触摸，于是剩下一个便宜可测的假设：**芯片可能还带着上一次启动（ABL 或 TWRP）加载的
  RAM 固件在跑**，而我们每张图开机第一件事就是给 gpio20 一个复位脉冲——正好把那份固件清掉，
  之后自然全程静默。0172 因此改成：**先探测、后复位**——只把 gpio20 拉高（不脉冲）、等 20 ms，
  在已发现引脚上用 4 种 SPI 模式读 id/FWINFO；只有这一步失败才做 vendor bring-up 和其余诊断。
  配合的操作步骤很关键：先在 TWRP 里确认触摸可用 → 不断电重启到 bootloader → `fastboot boot`
  这张图。若固件能挺过 fastboot 交接，就能在复位之前抓到它（日志出现
  `live controller answered in SPI mode N` 和真 id）。镜像 `shim/out/shim-boot-touch21.img`
  （sha256 `7970bc19…`，内核 `14447148…`，payload `1ed4f3e3…`）——**已编译，未上机**。

- **2026-10-24 触摸第 19 步：三种「应答」互不相同 ⇒ 只剩 SPI 模式没试（0171）**：
  touch19 上机读到第三种不同的值：`03 ff ff ff ff ff | page 01 ff ff ff`（touch17 是
  `83 80 3a 4f bb 9d`、touch18 是 `83 81 3a 47 bb 9c`、有的直接读还是 `ff`）。因为 0168
  放宽过判据（「任何非 00/ff 字节都算」），这次直接判成 ANSWER 提前返回，诊断矩阵根本没跑——
  不过有效 id 闸门生效，没有把乱码送进 HID。同一个地址三次读到三种结果，说明某个**读参数**
  随定时在变；唯一没试过的就是 **SPI 模式**（采样沿错会把真实位和上一位混起来，正好产生这种
  「看似合理但随启动变化」的数据）。0171 因此：1）判据收紧为「真正 `65 03` id 或自洽
  FWINFO」；2）bit-bang 支持 CPOL/CPHA，矩阵在已知引脚上把 **4 种 SPI 模式 × 两种时钟
  （5 µs / 25 µs）** 各测一遍，每次前发 vendor bring-up，同时读 id 与 FWINFO，另外保留
  MISO 上拉对照。镜像 `shim/out/shim-boot-touch20.img`（sha256 `51664b94…`，
  内核 `a6205f95…`，payload `8599afa0…`）——**已编译，未上机**。判读：某模式给出 `65 03`
  即答案；各模式互不相同 ⇒ 采样沿确实是关键；上拉后读 `ff` ⇒ MISO 没被驱动、之前是非零
  是耦合，下一步应抓 TWRP 下的 vendor SPI 波形。

- **2026-10-24 触摸第 18 步：数据不稳定 ⇒ 是「读得不稳」而不是协议不对（0170）**：
  touch18 上机：`>>16/>>8` 页拆分读到全 `ff`（那两个页没东西），`>>15/>>7` 才返回数据，
  所以 15/7 是对的；但数据**在多次启动/多次访问之间会变**：touch17 是
  `83 80 3a 4f bb 9d`，这次是 `83 81 3a 47 bb 9c`，而紧邻的直接读还是 `ff`。这说明不是协议
  错配，而是**读得不稳**（线/采样/帧对齐有一处不可靠）。有效 id 的闸门也起作用了：没有可信
  id 就只做 INT 观察，没把乱码当坐标送进系统。0170 在已知引脚上（cs17 clk18 mosi19 miso16，
  15/7 拆分）每次复位后各测一种变体并打一行：基线（5 µs、1 个 dummy）、**25 µs 慢时钟**、
  **MISO 上拉**（被驱动 vs 悬空）、**0 个 dummy**、**2 个 dummy**（帧对齐）。判读：某变体给出
  `65 03` 即答案；慢时钟与基线不同 ⇒ 采样/上升沿裕量不足；上拉后全 `ff` ⇒ 线是悬空的，
  之前的非零字节是时钟/数据线耦合；改 dummy 数仍无效 ⇒ 问题在页/寄存器寻址或 bootloader 状态。
  镜像 `shim/out/shim-boot-touch19.img`（sha256 `76559e9d…`，内核 `7643b423…`，
  payload `5deba1a5…`）——**已编译，未上机**。

- **2026-10-24 触摸第 17 步（突破）：芯片应答了，引脚角色确定（0169）**：
  touch17 把 vendor bring-up 放到 48 种分配之前，结果**只有一种分配应答**：
  `bitbang 10: cs gpio17 clk gpio18 mosi gpio19 miso gpio16 -> 83 80 3a 4f bb 9d |
  page b7 c0 1d 27 <-- ANSWER`。也就是说这块板上的角色是 **MISO=gpio16、CS=gpio17、
  CLK=gpio18、MOSI=gpio19**（DT 里名为 cs 的 gpio19 其实是 MOSI），这也解释了之前
  gpio16 驱不高——因为那是控制器在驱动的 MISO。数据是**确定性**的（紧随其后的重读拿到同样的
  `83 80 3a 4f bb 9d`）且随地址变化，所以是真芯片在回话，不是串扰。但内容不是合法 Novatek
  id、FWINFO 也不自洽，最大嫌疑是**页地址拆分**：本移植沿用的 harness 用 `addr>>15 / addr>>7`
  （NT36523 的来源），而 vendor 的 NT36xxx 驱动把 page 寄存器写成 bits 23:16 / 15:8。
  0169 因此：从发现的引脚出发，先发一次 bring-up，然后用**两种页拆分**分别读 trim id、
  FWINFO、chip-info page；只有拿到 `65 03` 结尾的 id 或自洽 FWINFO 才启动 HID 报点线程，
  否则只做 INT 观察（避免把乱码当触摸坐标送进系统）。镜像
  `shim/out/shim-boot-touch18.img`（sha256 `6dd4b194…`，内核 `64aec0f1…`，
  payload `a8d23b6f…`）——**已编译，未上机**。

- **2026-10-24 触摸第 16 步：TWRP 能触摸 ⇒ 硬件/供电没问题，问题在我们复现 vendor 的顺序（0168）**：
  用户确认「硬件没问题，TWRP 能触摸」。也就是说同样是 ABL `fastboot boot` 之后，TWRP 的内核
  + vendor 驱动能让这颗芯片正常工作，所以供电和引脚都是好的，缺的是初始化顺序。
  查 mainline 合入的 NT36xxx 驱动（AngeloGioacchino Del Regno，2020，源自 downstream/vendor
  同协议）得到 probe 顺序：reset gpio 以 **GPIOD_OUT_HIGH** 申请（高=出复位）→ 等 10 ms →
  写 `{0x00,0x69}`（bootloader reset）等 35 ms → `{0x00,0xA5}`（sw reset idle）等 15 ms →
  `{0x00,0x35}`（unlock）等 10 ms → `set_page(0x0001f64e)` 后从事件 `0x4e` 读 7 字节 chip id。
  **关键点：复位进 bootloader 必须发生在第一次读之前**，而我们之前只在一种猜测的引脚分配上
  发过这串命令；如果引脚角色不对，复位字节就发到了空气里，之后所有读自然全静默。
  0168 因此把 **vendor bring-up 放到 48 种分配的每一种之前**（各自带 vendor 的延时），
  然后同时读三处：`0x3f004` trim id、`0x2fe78` FWINFO、`0x1f64e` chip-info page；只要出现
  非 `00`/`ff` 的字节、`65 03` 结尾或自洽 FWINFO 就算命中。镜像
  `shim/out/shim-boot-touch17.img`（sha256 `c91622bf…`，内核 `53cc4991…`，
  payload `816f932d…`）——**已编译，未上机**。若依然全静默，下一步应拿 TWRP 下的 SPI 抓包
  来对齐协议，而不是继续猜。

- **2026-10-24 触摸第 15 步：纯软件手段穷尽，芯片电气上完全不应答（0167 结果）**：
  touch16 的 log：`gpio20 100 ms low->high (io 0x3)` 后静默、反相 `high->low (io 0x0)` 静默、
  「在 qup4 mux 下释放复位」静默、**48 种角色（24 种 × CS 低/高有效）全部 `00`/`ff`**、
  vendor bring-up 无反应、INT 60 秒 0 跳变。也就是说：时钟/引脚/GENI/软件 SPI、所有引脚角色、
  两种 CS 极性、复位线 gpio20、vendor ROM bring-up 都试过或测过，控制器一个 bit 都没回。
  软件侧到此为止，剩下的都是电气问题，需要用测量来分流（详见 `docs/TOUCH_NEXT.md` 的表）：
  逻辑分析仪抓 gpio16..19 看波形是否到焊盘/到芯片；万用表量触摸的 VDD/VDDIO——**如果这条
  供电轨是 0，就说明 die 根本没上电**，那正是一条「RPMh/PMIC 投票」的路（类似 nabu 移植里
  的 `ldoc11`），也正好能解释本 session 所有静默现象；另外可回 stock Android 验证硬件与
  vendor 的初始化顺序。驱动本身已经把总线之前的一切做完了：只要任何一次读通，id/FWINFO/
  报点线程就会自动接上。

- **2026-10-24 触摸第 14 步：复位确实释放了，但芯片仍然全程静默（0167）**：
  touch15 的 log 确认 `gpio20 reset released (ctl 0x280 io 0x3)`（驱动到高、回读高），
  但释放之后：24 种角色仍全 `00`/`ff`、vendor bring-up 仍无反应、INT 观察又 60 秒 0 跳变。
  所以 gpio20 是真实发现（保留），但不是全部原因——芯片出复位后既不响应 SPI 也不扫屏。
  按选定方向继续纯软件试探，0167 加了：1）三种复位形态，每次之后都在最可能的
  `cs19 clk17 mosi18 miso16` 上读 trim id + FWINFO：`100 ms 低→高`、`100 ms 高→低`（反相）、
  `拉低期间把 16..19 mux 回 qup4 再释放`（防止 die 在复位沿锁存接口 strap）；
  2）**24 种角色扫两遍**——CS 低有效一遍、**CS 高有效**一遍（板上若有反相器，我们原来的
  “拉低=选中”永远选不中）；3）再发一次 bring-up，最后照旧启动 INT 观察。镜像
  `shim/out/shim-boot-touch16.img`（sha256 `7f771816…`，内核 `5f464e78…`，
  payload `ba347486…`）——**已编译，未上机**。如果这次仍全静默，纯软件手段就穷尽了，
  下一步应改为测量（逻辑分析仪抓 gpio16..19 + INT，万用表量 VDD/VDDIO）或找 vendor 驱动源码。

- **2026-10-24 触摸第 13 步（关键）：触摸复位是 gpio20，一直被按着（0166）**：
  touch14 的 INT 观察 60 秒 **0 次跳变**，说明触摸 MCU 根本没在跑。翻 `backup/dtbo.img`
  （这台平板真正启动用的 vendor overlay）找到了原因：触摸节点是 `novatek,NVT-ts-spi`，
  IRQ 是 gpio21、lcd-id 是 gpio75，而它旁边的 vendor 引脚状态写着
  `pmx_ts_reset_suspend: pins = gpio20, bias-pull-down`（复位按住）和
  `mi_ts_active: pins = gpio20, gpio21, drive 8mA, bias-pull-up`（释放复位）。
  **gpio20 就是触摸控制器的复位线（低有效），而本移植依据的 mainline DTS 完全没提它**，
  我们的任何启动路径都没驱动过它，它就停在下拉状态——**控制器整场都被按在复位里**。
  这一条解释了本 session 的所有现象：24 种引脚分配全都不应答、INT 从不动、gpio16 驱不高
  而另外三根悬空（die 的焊盘没电/在复位，只看到板上偏置）、vendor 的 0x69/0xA5/0x35
  也没有反应（写进了一个在复位里的芯片）。0166 在焊盘自测之前按 vendor 的 active 状态
  驱动复位线：**gpio20 拉低 5 ms → 拉高 → 等 30 ms** 让 MCU 起来，并打印引脚状态和 INT 电平；
  其余（自测、24 种角色、bring-up）不变。镜像 `shim/out/shim-boot-touch15.img`
  （sha256 `2229534d…`，内核 `631a7a16…`，payload `308b5900…`）——**已编译，未上机**。

- **2026-10-24 触摸第 12 步：24 种角色全不应答；改为不用 SPI 的判活测试（0165）**：
  touch13 的完整 log 确认：**24 种 cs/clk/mosi/miso 分配全部失败**（多数 `00 … | fw 00 00 00 00`，
  bitbang 1/10/12 是 `ff`），随后在 cs19 clk17 mosi18 miso16 上发的 vendor bring-up
  （`0x69`/`0xA5`/`0x35`）也没能唤醒它：`after bring-up: 00 … | fw 00 …`，
  `controller still silent after the bring-up`。也就是说这颗控制器在 gpio16..19 的任何
  组合下都不应答 SPI。0165 换一个**完全不用 SPI** 的判活办法：gpio21 是控制器的 INT 输出，
  如果触摸 MCU 在跑，手指按上去它一定会动。bring-up 放弃后启动一个线程，**每秒 1000 次
  采样 gpio21、持续 60 秒**（并解除静音，屏幕上也可见），打印每一次跳变和总跳变数。
  结果只有两种：有跳变 → MCU 活着，是我们总线/引脚的问题；60 秒 0 跳变（且确实按了屏幕）
  → MCU 没在跑，问题在供电/复位序列或 RAM 固件，下一步得做固件下载（工作量大，先确认）。
  镜像 `shim/out/shim-boot-touch14.img`（sha256 `62691529…`，内核 `a70689fa…`，
  payload `c9726c5e…`）——**已编译，未上机**。

- **2026-10-24 触摸第 11 步：gpio16 被外部拉住，24 种角色全试 + vendor bring-up（0164）**：
  touch12 放慢时钟、加稳定延时后结论清楚了：**gpio17/18/19 是正常焊盘**（驱 0/1 跟随、
  立即上升、加偏置后悬空），**gpio16 连驱高都做不到，等 50 µs 仍是 0** —— 板子（或控制器）
  用比 6 mA 更强的东西把它拉低。bitbang 0 的 `ff` 也解释了：它的时钟在 gpio16、根本抬不起来，
  控制器没被时钟，浮空的 MISO 读到高；时钟正常时（bitbang 1..5）MISO 读低。没有任何分配
  返回有效 id/FWINFO。0164 因此做两件事：1）**把 24 种角色全试一遍**（gpio16..19 任一可作 CS，
  其余三个做 clk/mosi/miso 的 6 种排列），每次读 trim id + FWINFO；2）全失败后，在**最可能的
  分配**（cs19 clk17 mosi18 miso16，因为 gpio16 是那条被外部拉低的线、最像 MISO）上发
  **vendor 的 bring-up 序列**：`0x69`（bootloader reset）→ 40 ms → `0xA5`（sw reset idle）
  → 20 ms → `0x35`（unlock）→ 20 ms，然后再读 id 和 FWINFO。这正是 vendor 驱动在信任总线前
  做的事，而且 NT36532 的 ROM 即使 RAM 固件没跑也会应答。镜像
  `shim/out/shim-boot-touch13.img`（sha256 `6a7fdedb…`，内核 `cada30d5…`，
  payload `98930c54…`）——**已编译，未上机**。

- **2026-10-24 触摸第 10 步：自测读数有计时缺陷；放慢时钟、加 FWINFO 判据（0163）**：
  touch11 的焊盘自测结果：gpio17/18/19 驱 0/1 回读都跟随，只有 gpio16 `drive1 reads 0`；
  偏置测试四个脚都显示 "driven"（0 和 1 各一次）。但**自测本身有两个计时缺陷**：
  驱完几纳秒就回读、改完偏置只隔了 `nop` 级延时，读到的其实是上一个电平（焊盘有电容、
  内部拉电阻很弱）；而且 bit-bang 每比特只有三次 MMIO（约 300 ns），只报 trim id。
  真正有价值的一条是：**bitbang 0（时钟在 gpio16，可能根本抬不起来）读到 `ff`，
  而 bitbang 2（时钟在 gpio17、MISO 在 gpio18）读到 `00`** —— 有有效时钟时 MISO 会变，
  说明控制器很可能在工作，`00` 更像是「命令/地址它不认」而不是「总线死了」。
  0163 因此：把 bit-bang 半周期改成 `delay(5)`（约 100 kHz，可用 `liuqintouchpace` 调），
  自测改成驱完等 10 µs、改偏置等 2 ms，并加 `rise: immediately … after 50 us …` 判断
  是「慢」还是「被拉死」；每组分配同时读 trim id 和 **FWINFO**，只要 id 以 `65 03`
  结尾或 FWINFO 自洽（`fw_ver + ~fw_ver == 0xff`）就认为找对了。镜像
  `shim/out/shim-boot-touch12.img`（sha256 `c3001804…`，内核 `b55b0f4e…`，
  payload `3ed3e25b…`）——**已编译，未上机**。若所有焊盘 rise 正常而 FWINFO 仍全 0，
  下一步才是 vendor 的复位写序列（0x69/0xA5/0x35）。

- **2026-10-24 触摸第 9 步：软件 SPI 跑通但 MISO 全 0，先测焊盘（0162）**：
  touch10 上机**没有再卡死**（boot 正常继续，后面 exec 行照常），六种
  clk/mosi/miso 分配全部返回 `00 00 00 00 00 00`，`no pad assignment answered`。
  两种可能：一是我们这边没真正驱动/读取焊盘（OE 位、输出位、mux 不对），控制器
  根本没收到时钟；二是控制器侧不驱动 MISO（可能被 hold 在复位、或触摸固件没在跑）。
  0162 在排列搜索之前先做**焊盘电学自测**：把 gpio16..19 逐个配成输出、驱 0 再驱 1 并
  **回读自己的焊盘**（验证 mux/OE/输出位），再把每个焊盘配成输入分别上拉/下拉
  （跟着偏置变 = 悬空、不变 = 被驱动，可据此直接找出哪个是 MISO），最后读 gpio21
  （已知 INT 为 0）作为「输入通路正常」的参照。镜像 `shim/out/shim-boot-touch11.img`
  （sha256 `90bad726…`，内核 `9029359d…`，payload `487778eb…`）——**已编译，未上机**。
  判读：out 行都跟随 → 我们的驱动没问题，是控制器不回话；有 `<-- does not follow`
  → 我们的 OE/输出位配错；某个 in 行 `(driven)` → 那个脚就是芯片驱动的 MISO。若确认
  焊盘没问题而控制器仍不回话，下一步是 vendor 的复位序列（0x69/0xA5/0x35），注意
  TDDI 的触摸固件在 RAM 里，复位后可能需要下载 `novatek_nt36532_m81_fw_csot.bin`。

- **2026-10-24 触摸第 8 步：SE-DMA 的地址不可能成功，改用软件 SPI（0161）**：
  touch9 上机后整段 bring-up 都上了屏：暂存页 **phys 0x8a0150000（34.5 GiB）**、
  `CS_ASSERT` 仍然成功（`after-cmd 0x41 irq 0x1`），最后一行是
  `DMA #1 tx+rx len 1 tx 0x8a0150000 rx 0x8a0150100`，然后整机冻住。**卡点就是第一次
  SE-DMA 传输**，而且这是内存映射层面的问题，不是驱动细节：本移植的 ADT 只描述 32 GiB
  以上的内核 bank（`--dram-base 0x840000000`），QUP 的 DMA 拿到的就是这个 34 GiB 的物理
  地址；Linux 在这里是通过 apps-SMMU 给它 IOVA 的，Qualcomm 自己的 U-Boot GENI SPI 驱动
  也只在低地址 DRAM 上跑过。要让 SE-DMA 工作，要么把内核 bank 搬回 4 GiB 以下（改启动镜像，
  不是驱动），要么给 QUP SID 编 apps-SMMU——两者都远超一个触摸补丁，而且会危及 UFS/GPU。
  所以 0161 换了一条完全不同的路：**把 gpio16..19 重新 mux 成 GPIO，用软件 SPI 直接
  驱动这四根焊盘**（mode 0、MSB first、CS 是 gpio19，靠 MMIO 访问本身限速）。没有 DMA、
  没有 FIFO、没有 SMMU，写错时序最多读错数据，不会再把总线挂死。DT 没说 gpio16/17/18
  哪个是 clk/mosi/miso，驱动**把六种分配都试一遍**，谁读出 `… 65 03` 的 Novatek trim id
  就用谁，六次都打日志。`liuqintouchdma=1` 仍可回到 SE-DMA 路径（等内存/SMMU 问题解决）。
  镜像 `shim/out/shim-boot-touch10.img`（sha256 `9ab92b4b…`，内核 `68cb4fb9…`，
  payload `00f57f62…`）——**已编译，未上机**。

- **2026-10-24 触摸第 7 步：卡点在第一次 SE-DMA 传输；打印 DMA 地址（0160）**：
  touch8 上机后屏幕同时出现 `touch spi cfg at 418.428 s` 和 `touch bus probe at 418.429 s`
  两个路标，中间没有任何 `LIUQIN TOUCH:` 行 —— 这是 0159 的 bug：`liuqin_boot_mark()`
  会把 `liuqin_console_force` 置 1 打完再清 0，把我们的「解除静音」冲掉了（行其实进了 log，
  只是没画）。但两个路标已经把范围定死：`touch bus probe` 在 `tch_dma_alloc()` 和 after-init
  dump **之后**，所以暂存页和 `kmem_alloc_contig()` 都成功了，**卡点在 `tch_se_start()` 里，
  也就是第一次 SE-DMA 传输**（或 CS 命令）。整机冻住而不是只卡线程，说明那笔 DMA 事务让
  互联（NOC）无法解码而停摆，所有 CPU 的内存访问一起挂住。最大嫌疑是地址：本 ADT 的 DRAM
  在 `0x840000000`（33 GiB），我们把物理地址直接写进 `SE_DMA_TX/RX_PTR_L/H`，而 Linux 是
  `dma_map_single()` 经 QUP 的 SMMU 上下文拿到 IOVA 再写进去的。0160 修好静音（
  `tch_visible_mark()` 每次打点后重新置位），并在 `tch_dma_xfer()` 里把前四次传输的地址
  打出来（`DMA #1 tx+rx len 1 tx 0x… rx 0x… mode 0x…`），屏幕最后一行就是卡点。
  镜像 `shim/out/shim-boot-touch9.img`（sha256 `6afa204c…`，内核 `1d1e00a6…`，
  payload `4dc1b607…`）——**已编译，未上机**。若最后一行停在 `DMA #1`，下一步按
  `docs/TOUCH_NEXT.md` 里的顺序试：低地址分配 / apps-SMMU 的 S2CR bypass / GPI DMA。

- **2026-10-24 触摸第 6 步：路标证明卡在 liuqin_touch_spi_setup 内部；让 bring-up 自己上屏（0159）**：
  touch7 上机后屏幕出现 `LIUQIN BOOT: touch spi start at 61.932 s (+138 ms)`，之后**再没有任何
  行**（既没有 `touch id read` 也没有 `touch spi failed`，也没有更晚的 boot mark），说明：
  延迟线程生效了、boot 早已正常跑起来（61.4 s 还在画 exec 行），**卡点在
  `liuqin_touch_spi_setup()` 里面**。但里面的 `LIUQIN TOUCH:` 行被 `liuqinmute=1` 压掉了，
  整机卡死时 log 线程也来不及刷盘，所以还是看不出最后一行是哪条。0159 只把这段「解除静音」：
  setup 线程执行 `liuqin_touch_spi_setup()` 和 Novatek 读取期间置 `liuqin_console_force = 1`，
  结束后清 0，于是这些行（本来就进了持久 log）也画到屏幕上；再加两个路标
  `touch spi cfg`（FIFO/DMA 判断与暂存页之前）和 `touch bus probe`（`tch_se_start()` 之前）。
  镜像 `shim/out/shim-boot-touch8.img`（sha256 `f19657da…`，内核 `39951582…`，
  payload `d37c6b92…`）——**已编译，未上机**。屏幕最后一行就是卡住的那条：
  停在 `SPI master mode 0` 与 `FIFO interface` 之间 → `kmem_alloc_contig`；停在
  `DMA staging page` 附近 → `tch_se_start()` 的寄存器/第一条 DMA 命令。详见
  `docs/TOUCH_NEXT.md`。

- **2026-10-24 触摸第 5 步：SE-DMA 版把机器卡死，改成延迟线程 + 屏幕路标（0158）**：
  `shim-boot-touch6.img` 在触摸阶段**整机卡死**（要手动重启）：屏幕停在持久 log 尾段 dump
  的中间，大约 "log thread started at 48.521 s" 之后 0.1 s。注意 `liuqinmute=1` 时屏幕只画
  强制的 `LIUQIN BOOT:` 行，`LIUQIN TOUCH:` 只进持久 log，所以屏幕上没有 touch 行**不代表**
  没跑到；而 mount 线程的打印又被 log 线程那 60 行尾段 dump 挡在后面（控制台串行），因此
  卡死点无法从照片判断。log 线程也一起停了，那 2 s 一次的刷盘没赶上，这次启动的 log 丢了。
  0158 把 SPI 这段从 mount 路径挪到**启动后 10 s 的独立线程**（`liuqintouchdelay`，0-120 s），
  每一步都调 `liuqin_boot_mark()`（**这些行即使 liuqinmute=1 也会画在屏幕上**）：
  `touch clocks+pins ready` → `touch spi start` → `touch id read` / `touch spi failed` →
  `touch reports up`，屏幕上最后一个路标就是卡住的位置；同时 init 时复位 SE DMA 的 FSM
  （ABL 是用 GPI DMA 驱动这个 SE 的），拒绝 phys=0，并把启动参数改成 `ufslogsecs=1`
  （每秒刷盘，卡死前最多丢 1 s）。镜像 `shim/out/shim-boot-touch7.img`
  （sha256 `c8f362f8…`，内核 `4d9ae9ad…`，payload `1f5e7eb8…`）——**已编译，未上机**。
  如果卡在 `touch spi start` 之后、`DMA staging page` 之前，是 `kmem_alloc_contig` 的问题；
  卡在 `CS assert`/`TX probe` 之后，就是第一次 DMA 传输把互联总线挂死（下一步查 apps-SMMU
  的 S2CR 或改 GPI DMA）。详见 `docs/TOUCH_NEXT.md`。

- **2026-10-24 触摸第 4 步：这个 SE 根本没有 FIFO，改走 SE-DMA（0157）**：
  touch5 上机后 mux 修好了（`gpio16..19 ctl 0x84`、gpio21 `0x3`），**CS_ASSERT 成功**
  （`after-cmd 0x41`、`irq 0x1`），命令通道没问题；dump 给出了真正的答案：
  **`ifdis 0x00000001`，`GENI_IF_DISABLE_RO.FIFO_IF_DISABLE` 置位 —— 这个 SE 没有 CPU
  FIFO 接口**，所以带数据的 FIFO 命令（page 写）一直 `M_CMD_OVERRUN`，而不带数据的 CS
  命令能过。`before init` 里 `gsi 0xf` 说明之前用这个 SE 的驱动走的是 **GPI DMA**
  （Linux 的 `spi_geni_init()` 读到 FIFO 关闭也会直接选 GPI DMA）。因此 0157 改用
  **SE-DMA**（Qualcomm 的 U-Boot GENI SPI 驱动明确说 SE-DMA 总是可用）：
  `SE_GENI_DMA_MODE_EN`、`SE_DMA_{TX,RX}_{PTR_L,PTR_H,LEN,ATTR}`、轮询
  `SE_DMA_{TX,RX}_IRQ_STAT`，一页 `kmem_alloc_contig` 的物理连续暂存区（TX/RX 相隔
  256 字节），传输前后 flush/clean cache（SE 不 cache-coherent）；FIFO 路径保留给有
  FIFO 的引擎。镜像 `shim/out/shim-boot-touch6.img`（sha256 `66007235…`，内核
  `52888bf0…`，payload `8f7508a7…`）——**已编译，未上机**。
  若 DMA 也不动（`TX probe` 的 `dma tx` 和 `irq` 一直 0），下一步是 apps-SMMU 的
  S2CR（QUP SID 可能还在翻译，因为之前的 GPI DMA 走同一个 SID）或 GPI DMA 本身。

- **2026-10-24 触摸第 3 步首次上机：SE 拒收命令，引脚 mux 写错（0156）**：
  `shim-boot-touch4.img` 在真机上启动正常（没 panic，USB 照常起来），touch 段日志：
  `QUP hw 0x30080000 (v3.8) TX fifo 16 x 32 bit`、SPI 主控配置成功、`gpio21 in/out 0x0`
  （INT 被触摸核拉低 → 核是活的、有电），但 `Novatek trim id 00 00 00 00 00 00`，
  `irq 0x00000002`，之后每次 event 读也都是 `0x2`。`0x2 = M_CMD_OVERRUN`，说明 **SE 根本
  没执行命令**，6 个 0 是总线侧失败，不是触摸核不回话。目前有一个已确认的 bug 和一个待查点：
  1) **引脚 mux 值写错（已确认）**：TLMM 的 mux 值是「该 pin 函数表里的下标」，`pinctrl-sm8450.c`
     里 `gpio16..19` 是 `PINGROUP(n, qup4, phase_flag, ...)`，所以 **qup4 = 1**，
     2 是 phase_flag；0153 一直写 2，SPI 信号根本没接到焊盘（stage-1 的审计行反而按
     `mux == 1` 判 "(qup4)"，自相矛盾）。0156 改成 `ctl 0x84`（mux 1、无偏置、6 mA）。
  2) gpio21 的偏置 2 在 SM8450 是 **keeper 不是 pull-up**（0=无、1=下拉、2=keeper、
     3=上拉），0156 改成 3。
  为什么 mux 写错会让 SE 报 overrun，目前**还解释不了**（mux 只影响信号出不出焊盘，
  不该影响命令是否被接受），所以 overrun 的原因仍待 0156 的 dump 判定。
  另外 0156 加了诊断：init 前/后 dump SE 状态（status/irq/irqen/ctrl/cmd0/gp_len、
  clk/sel/out/cgc/gsi/seirq/**ifdis**、SPI 各寄存器），先试 CS_ASSERT，再试 1 字节
  TX_ONLY，都失败就按 cancel → abort → clear-disable → start-trigger 依次尝试并逐条打印，
  哪个生效日志里会写 "SE4 accepts commands after …"；全失败就 dump `dead` 状态并且不启动
  报点线程。重点看 `ifdis` 的 bit0（为 1 表示该 SE 的 FIFO 接口被关掉、只能走 GPI DMA）
  和 `ctrl` 的 bit0（`M_GENI_DISABLE`，ABL 把主序列器关了的话正是这个）。
  0156 还按 Qualcomm 自己的 U-Boot GENI SPI 驱动改了两处：全双工 FIFO 的 opcode 是
  **7**（`SPI_TX_RX`；Linux 用方向位 OR 出来的 3 不在命令表里），以及引擎可能根本不接受
  `CS_ASSERT/CS_DEASSERT`——那种情况下驱动会记 `CS per transfer` 并让引擎自己按传输拉 CS。
  镜像 `shim/out/shim-boot-touch5.img`（sha256 `ec707038…`，内核 `3d21d10d…`，
  payload `d328928b…`）——**已编译，未上机**。详见 `docs/TOUCH_NEXT.md`。

- **2026-10-24 触摸第 3 步：SPI 主控 + chip id + 10 指报点（0155，未上真机）**：
  `liuqintouch>=3` 在时钟/引脚之后按 `spi_geni_init()` / `setup_fifo_params()` 的顺序把
  SE4 配成 SPI 主控：`GENI_OUTPUT_CTRL 0x7f`、`GENI_FORCE_DEFAULT_REG 1`、
  `SE_GENI_M_IRQ_EN` 加 done/tx-wm/rx-wm/rx-last、`SE_GENI_DMA_MODE_EN` 清 bit0、
  `SE_SPI_TRANS_CFG` 清 `CS_TOGGLE`、8 位字（`WORD_LEN 4`）+ 4 合 1 packing、mode 0、
  CS0（`DEMUX_SEL 0`）、`GENI_SER_M_CLK_CFG = 16 分频 | SER_CLK_EN`（19.2 MHz → 1.2 MHz）。
  传输是 FIFO 轮询：`TX/RX_TRANS_LEN`、`M_CMD0 = opcode<<27 | FRAGMENTATION`（TX+RX 用
  **3**，不是 7；7 是 GPI DMA 的）、按 `TX_FIFO_STATUS` 喂 TX、边跑边排空 RX，CS 用
  `CS_ASSERT/CS_DEASSERT` 手动开合，每个 Novatek 阶段一个 CS 窗口。之后按页/地址协议
  （`0xff, addr>>15, addr>>7`，再 `addr & 0x7f` + 1 个 dummy）读 `0x3f004` 的 6 字节 trim id
  和 `0x2fe78` 的 16 字节 FWINFO，然后起内核线程：gpio21 触发时读 `0x2fe00` 的 65 字节报点帧，
  解 10 条 6 字节记录（event 1/2 = 按下/移动，3 = 抬起），缩放 1800x2880 后调
  `liuqin_hid_touch()`，帧里消失的手指补发抬起；空帧/全 0 帧不解码，避免把按着的手指抬掉。
  两个硬件事实：**SM8450 的 TLMM `GPIO_IN_OUT` 里 in 是 bit0、out 是 bit1**
  （`pinctrl-sm8450.c` `.in_bit = 0`，和老 SoC 相反）；SE 时钟是 19.2 MHz 只因为第 2 步把
  RCG 设成源 0 + M/N/D=0。可选参数 `liuqintouchdiv`（分频，默认 16）、
  `liuqintouchpoll=1`（不等 INT，每 tick 读）、`liuqintouchswap`、`liuqintouchflip`。
  镜像 `shim/out/shim-boot-touch4.img`（sha256 `17cfd104…`，内核 `c9e9e166…`，
  payload `7c2f44c0…`）——**只在主机上编译通过，还没有在平板上启动过**。
  下一步：`fastboot boot` 这张图，看 trim id 那行的 `irq 0x…` 和 6 字节 id。`irq` 是最后
  一条命令的 `M_IRQ_STATUS`：**bit0=1 就表示命令完成**（`0x08000001` 这种是正常的读，
  bit27 是 RX FIFO last），bit1/2/3 和 bit24/25/28/29 才是错误；0x0 表示 50 ms 超时；若 id 是 00/ff，说明触摸核还没在跑，需要补 vendor 的
  复位序列（bootloader reset 0x69 / sw reset idle 0xA5 / unlock 0x35）。详见
  `docs/TOUCH_NEXT.md`。

- **2026-10-24 GENI SE4 活了**：`SE4 status 0x0 fwrev 0x00000102`（GENI 1.2.0）。时钟、
  引脚、SE 访问全部验证通过。第 3 步（SPI 主控）需要的寄存器已查清：
  `SE_SPI_CPHA 0x224 / LOOPBACK 0x22c / CPOL 0x230 / TRANS_CFG 0x25c / WORD_LEN 0x268 /
  TX_TRANS_LEN 0x26c / RX_TRANS_LEN 0x270 / SLAVE_EN 0x2bc`；GENI 侧
  `SER_M_CLK_CFG 0x48`（m/n 分频，决定 SPI 时钟）、`M_CMD0 0x600`（opcode 在 bit31:27、
  params 在 26:0）、`M_CMD_CTRL 0x604`、`M_IRQ_STATUS 0x610 / EN 0x614 / CLEAR 0x618`、
  `TX_FIFO 0x700 / RX_FIFO 0x780 / FIFO_STATUS 0x800,0x804`；
  opcode：`SPI_TX_ONLY 1 / SPI_RX_ONLY 2 / SPI_TX_RX 7 / CS_ASSERT 8 / CS_DEASSERT 9`。
  下一步：按 `spi_geni_setup` 的顺序配好这些，发 Novatek 的读 chip-id 命令。

- **2026-10-24 触摸第 2 步成功、修 stale 判断（0154）**：touch2 的日志证明时钟和引脚
  都配好了（`CBCR 0x40028bc0 -> 0x4002cbc0`，`s4 RCG cmd 0x2 (root-on update-done)
  halt 0x4220 (running)`，`gpio16–19 ctl 0x88`，`gpio21 ctl 0x2`），但随后打印
  `SE4 not clocked, registers not read` —— 判断用了写之前读到的 `cbcr` 旧值。0154 改成
  写完后再读一次。镜像 `shim/out/shim-boot-touch3.img`（sha256 `b0cf9301…`，内核
  `72733ff7…`）。另外 touch2 那次启动本身是正常的（日志到 500 s 仍在跑 daemon），
  只是首次 LaunchServices seeding 很慢，锁屏要等。

- **2026-10-24 触摸第 2 步：开时钟 + 配引脚（0153）**。审计结果：SE4 时钟关
  （CBCR 0x40028bc0，bit14=0；halt 0x80004220；RCG cmd 0x80000000 = ROOT_OFF），
  五个引脚全是 `ctl 0x1`（mux 0 = GPIO、pull-down），ABL 什么都没准备。0153 加
  `liuqintouch>=2`：置 0x62008 bit14、按 clk-rcg2 写 M/N/D=0 与 CFG=0（BI_TCXO、
  MND）、CMD = UPDATE|ROOT_EN 并轮询 UPDATE 清位，再把 gpio16–19 设成 mux 2
  （qup4）+6 mA、gpio21 设成输入上拉，然后读 SE 寄存器。
  镜像 `shim/out/shim-boot-touch2.img`（sha256 `fcc2b43c…`，内核 `b6a9575d…`）。

- **2026-10-24 触摸第 1 步：只读审计（0152）**：新增 `bsd/dev/liuqin_touch.c`
  （`liuqintouch=1`，仅读不写），从 `liuqin_ufs.c` 的挂载路径调用。读：GCC
  `QUPV3_WRAP0_CBCR`（0x62008，m_ahb/s_ahb/core/core2x/s4 五位）、SE4 的分支
  halt/RCG（0x274dc/0x274e4/e8/f0/f4/f8）、TLMM gpio16–19/21 的 mux/pull/drive
  （ctl = 0x1000*pin，mux 在 bit2–4，pull 在 bit0–1，drive 在 bit6–8），**只有在
  SE4 时钟确认在跑时**才读 SE 寄存器（0x40 status / 0x68 fwrev / 0x630 / 0x640），
  避免访问被门控的 QUP 块挂总线。镜像 `shim/out/shim-boot-touch1.img`
  （sha256 `c9a53ea0…`，内核 `1edf23a8…`）。

- **2026-10-24 USB 枚举成功**：`shim-boot-usb8.img`（`liuqinusb=2 liuqinusbdwc=1`）
  起机后主机侧看到 USB 设备——DWC3 软复位、事件缓冲、ep0、gadget 这条链路首次真正跑通。
  （此前 USB 从未在 iOS 里工作过：probe 被注释、GCC 偏移是 sweet 的、GCC 映射又太小。）
  设备描述符是 CDC-ECM，VID 0x1209 / PID 0x0001；macOS 是否绑定 ECM 驱动待确认，Linux
  主机可直接用 `tools/liuqin-usbnet.sh`（192.168.7.1/24，live log udp/5140）。

- **2026-10-24 USB 上电不再伤 GPU，锁屏正常**：`shim-boot-usb7.img`（0151，GCC 1 MiB
  映射 + `liuqinusb=2`）正常启动到锁屏，说明之前四次 panic 全是 `liuqin_usb_probe()` 读
  0x9c010（映射外）造成的。下一步镜像 `shim/out/shim-boot-usb8.img`
  （sha256 `c73b6afc…`，加 `liuqinusbdwc=1`）：读 DWC3、软复位、建事件缓冲、ep0 并启动
  gadget（`ud_thread` 轮询），目标是在主机侧看到 1209:0001 的 CDC-ECM 设备。

- **2026-10-24 panic 真正原因：GCC 映射太小（0151）**。`liuqin_usb_probe()` 在
  `liuqin_log_start()`/`liuqin_sample_start()` 之后被调用（正好是日志里
  `log thread started at 63.015 s` 那一刻）。0150 把 clkref 分支改成
  **0x9c010/0x9c00c**，但 `GCC_SIZE` 仍是 **0x90000**（映射 0x100000–0x18FFFF），
  `0x100000+0x9c010 = 0x19c010` 在映射外 → 一读就 data abort。报告循环不看
  `liuqinusb` 级别，所以 usb5/6/diag 全中；usb3/4 则是 sweet 偏移写坏 GPU 时钟。
  0151 把 `GCC_SIZE` 改成 **0x100000**（覆盖到 0x200000）。镜像
  `shim/out/shim-boot-usb7.img`（sha256 `b57987e5…`，内核 `303bd170…`，`liuqinusb=2`
  + `ufslogsecs=2`，DWC3 仍 gate）。

- **2026-10-24 panic 诊断镜像**：usb6（`liuqinusb=1`，只读）也 panic，USB 排除。核对
  working log 发现 ladder 只跑到 `done (asked 5, reached 5)`，**第 20 步从未执行**，所以
  panic 位置符号化到 `step_core_start` 是假象（栈已坏，pc 是野指针）。持久 log 每
  `ufslogsecs` 秒刷盘（默认 10），panic 会丢最后一分钟；新镜像
  `shim/out/shim-boot-diag.img`（sha256 `11bab27c…`）用 `ufslogsecs=2`，panic 前的行
  基本都能留下来。下一步靠这张的 log 定位。

- **2026-10-24 改完偏移仍 panic（隔离中）**：usb5（0150，SM8450 偏移）仍 panic 在 GPU
  ladder 第 20 步（`printf` 附近，esr 0x96…）。为隔离变量出新镜像
  `shim/out/shim-boot-usb6.img`（sha256 `75cb6f5b…`，`liuqinusb=1`：只读 GCC 报告，
  不写任何寄存器、不碰 DWC3）。若这张也 panic，则与 USB 无关（GPU step 20 自身不稳，
  可能与上次 panic 遗留的 GPU/GMU 状态有关，建议先整机断电再起）。

- **2026-10-24 panic 根因：USB 用了 sweet 的 GCC 偏移（0150）**。usb3/usb4 的 panic 不是
  USB 探测本身，而是 GPU ladder 第 20 步（CP 启动）在访问 CP 寄存器时外部 abort
  （esr 0x96000010）。原因是 `liuqin_usb.c` 的 `usb_cbcr[]`/GDSCR 偏移抄自
  **sweet（SM7150, gcc-sdmmagpie）**：GDSCR 0x0f004、master 0x0f010…；这些地址在
  SM8450 上是**别的时钟**，USB 上电把 GPU/显示要用的时钟关掉了。按
  `gcc-sm8450.c` 改正：`usb30_prim_gdsc` = **0x49004**，分支 = 0x49018(master) /
  0x49020(sleep) / 0x49024(mock_utmi) / 0x49084(cfg_noc) / 0x49088(aggre) /
  0x4905c/0x49060/0x49064(phy aux/com/pipe)，clkref 0x9c010/0x9c00c 不动。镜像
  `shim/out/shim-boot-usb5.img`（sha256 `66762d14…`，内核 `4dc16a59…`）。

- **2026-10-24 USB3 镜像 panic**：把 probe 打开后的 `shim-boot-usb3.img` 起机后 panic。
  静音模式下普通 IOLog 不画屏，照片上只有强制行，看不到停在哪一步，等这次的持久 log。
  同时加 0149：DWC3 访问也变成 opt-in（`liuqinusbdwc=1` 才读），默认只报告 GCC 状态，
  这样 USB 探测再出错也不会把启动打死。安全镜像
  `shim/out/shim-boot-usb4.img`（sha256 `28da9f41…`，内核 `cd645334…`）。

- **2026-10-24 USB probe 真正打开（0148）**：上一张 `liuqinusb=2` 毫无反应，因为
  `liuqin_usb_probe()` 在 `liuqin_ufs.c` 里是**注释掉的**（0080 起就是：
  `-- faults on this unit (data abort)`）。那次 abort 的来源是 `0x088e2000` 的 QUSB2
  PHY ——那是 sweet（SM7150）的地址，SM8475 用 eUSB2 repeater，读它就是踩空洞。
  0148 恢复调用，并把 PHY 段改成 opt-in（`liuqinusbphy=1` 才读），其余由已有的
  `liuqinusb` 级别控制（0 关 / 1 只报告 / 2 启动 gadget）。镜像
  `shim/out/shim-boot-usb3.img`（sha256 `7e7e8716…`，内核 `ed23d806…`）。

- **2026-10-24 USB 上电（0147）**：`liuqinusb=2` 那张没被主机识别。原因大概率是
  `fastboot boot` 交接后 ABL 把 USB30_PRIM 的 GDSC 折叠、分支时钟关掉，而 probe 只
  读不写，读到 off 就直接返回（`controller unpowered or unclocked; DWC3 not read`），
  gadget 从未启动。0147 在 `enabled>=2` 时自己打开 GDSC（清 SW_COLLAPSE、等 PWR_ON）
  并逐路打开 CBCR（写 CLK_ENABLE、等 CLK_OFF 消失，clkref 不动），每步打印结果，然后
  继续原来的 DWC3 读取与 gadget 启动。镜像 `shim/out/shim-boot-usb2.img`
  （sha256 `4b5b744c…`，内核 `fae1a62d…`）。

- **2026-10-24 USB 现状**：驱动早已在（补丁 0070-0074/0077/0078：DWC3 gadget、ep0、VBUS override、CDC-ECM、live log），但 `liuqinusb` 默认是 1（只报告状态），我们所有镜像都没带 `liuqinusb=2`，所以手机从来没在 iOS 里枚举过。新镜像
  `shim/out/shim-boot-usb.img`（sha256 `d42659d4…`，boot line 加 `liuqinusb=2`）用来验证枚举：设备应为 VID 0x1209 / PID 0x0001、CDC-ECM 两个接口。主机侧
  `tools/liuqin-usbnet.sh` 只支持 Linux（ip/nmcli），macOS 没有 CDC-ECM 驱动；要在 macOS 上用就得把 gadget 换成 NCM。

- **2026-10-24 桌面达成 + 输入计划**：静音启动等待 LaunchServices 首次 seeding 完成后，
  **锁屏界面出现**（CPU 软件渲染 + IOSurface + IOMFB，整条图形链路首次真正跑起来）。
  但触摸/按键都没有：HID 服务（0058/0068）已编入内核，`liuqin_hid_touch` /
  `liuqin_hid_key` 没人调用——缺驱动。硬件事实（来自 `yzddmr6/linux-sm8450-liuqin`
  分支 `liuqin-6.17`，`sm8475-xiaomi-liuqin.dts` + `sm8450.dtsi`，节点又引自 vendor
  ABL 的 FDT）：NT36532 走 **GENI SPI4 `@0x990000`**（QUPv3 wrapper 0 SE4，CS0，
  9.6 MHz 上限），IRQ = **TLMM gpio21**（上升沿），GIC SPI 605，软件复位寄存器
  `0x1fb43e`，固件 `novatek_nt36532_m81_fw_csot.bin` 由驱动 RAM 载入，引脚
  gpio16/17/18 (data/clk) + gpio19 (cs)，function `qup4`；GCC 侧
  `gcc_qupv3_wrap0_s4_clk` 使能 `0x62008` BIT(14)、halt `0x274dc`、RCG
  `0x274e4`。完整计划写在 `docs/TOUCH.md`（只读审计 → chip-id → 报点 →
  按键 SPMI）。

- **2026-10-24 桌面卡点定位到 LaunchServices/lsd**：静音启动的 log 证明软件显示
  链路是通的（`AppleCLCD 1800x2880`、`display on`、backboardd 建 IOSurface、
  `swap 1..3`），但之后十分钟只重复 exec `fairplayd.A2`/`bluetoothd`。用共享缓存
  （`ipsw dyld a2s`，slide 0x37150000）符号化这次的采样栈：SpringBoard t0 在
  `-[LSApplicationRecord initWithBundleIdentifier:…]` → `_LSContextInitCommon`
  （LaunchServices 数据库初始化），backboardd t8 在
  `_xpc_connection_send_message_with_reply_sync` 等同步 XPC 回复——两边都卡在
  LaunchServices/`lsd`（83.8 s exec，仍活着）。没有任何 crash 报告。新镜像
  `shim/out/shim-boot-sample-lsd.img`（sha256 `26ca9cad…`）静音并加
  `liuqinsample=SpringBoard,backboardd,lsd`，下次 log 带 lsd 的栈。

- **2026-10-24 “没有第二遍进度条”其实是控制台没静音**：GPU 这批镜像为了在屏幕上看
  IOLog 故意去掉了 `liuqinmute`，而 iOS 的第二遍进度条是用户态画在同一块屏上的，
  控制台文字会把 logo/进度条顶掉——看起来像“起不来”，其实上一份正常启动的 log 里
  SpringBoard/backboardd 都 exec 了、SMP 也在采样。`shim/out/shim-boot-mute.img`
  （sha256 `2809d735…`）是同一套参数加 `liuqinmute=1`：屏幕保留 logo/进度条，持久
  log 仍然收全部行，所以 GPU 进度和 `gputest:` 结果照样能拉到。

- **2026-10-24 整包 opt-in 镜像**：`~/liuqin-root/ipad71-root-optin.img`（16 GiB
  完整镜像，sha256 `8a4f3da9…`）＝ nohid root + 新版 opt-in 工具
  （`/usr/libexec/liuqin-gpu-test`，sha256 `a3a46027…`）+ 同一个 LaunchDaemon。
  稀疏镜像那次没起来，改用整包刷入。

- **2026-10-24 用户态测试改为 boot-arg 开关**：上一份 log（其实是上一轮启动）显示
  这次 `GPU ready` 在 **51 s**——GPU 服务在 launchd 还在起系统时就发布了，daemon 立刻
  提交，很可能把 GPU/显示一起带走（第二遍进度条画不出来、控制台文字顶掉 logo）。工具
  现在读 `kern.bootargs`，只有 boot line 里有 `sheng-gpu-gputest=1` 才跑，否则打印
  `gputest: skipped (…)` 退出；启用时也会在服务出现后再等 20 s。
  产物：`~/liuqin-root/ipad71-root-optin.simg`（sha256 `999968f3…`，272 块，以当前
  设备镜像为基准的稀疏 diff）与 `shim/out/shim-boot-gputest.img`
  （sha256 `43a6485e…`，boot line 带 `sheng-gpu-gputest=1`）。

- **2026-10-24 整包 userdata（含用户态 GPU 测试）**：
  `~/liuqin-root/ipad71-root-gputest.img`（16 GiB 完整镜像，sha256
  `a0377120…`），内容 = 之前刷过的 nohid root + `/usr/libexec/liuqin-gpu-test`
  + `/System/Library/LaunchDaemons/com.liuqin.gputest.plist`（RunAtLoad，
  stdout/stderr → `/dev/console`）。直接 `fastboot flash userdata` 整包刷入即可，
  比稀疏镜像慢但省事。

- **2026-10-24 用户态 ABI 测试工具（未上机）**：新增 `tools/liuqin-gpu-test.cpp`
  + `tools/build-gpu-test.sh`（iOS 17.5 SDK，iOS arm64，输出
  `shim/out/liuqin-gpu-test`），`tools/liuqin-root-tweaks.py` 会把它装成
  `/usr/libexec/liuqin-gpu-test` 并挂一个 RunAtLoad 的 LaunchDaemon
  （stdout/stderr 到 `/dev/console`，结果进持久 log）；tweaks 里加了 `safe_chown`
  以便在忽略属主的 macOS 挂载上跑。userdata 稀疏镜像
  `~/liuqin-root/ipad71-root-gputest.simg`（sha256 `926e59b0…`，1034 块，4 MiB，
  以“已刷入的镜像”为基准做 diff；工具最多等 10 分钟，因为这块板子的 GPU 大约在启动 5 分钟后才发布）。它走真实 ABI 做与内核自检相同的验证：
  Query/GetDeviceInfo → BoAlloc/BoMap/BoCpuMap → Submit(CP_MEM_WRITE) → WaitFence
  → 读回 0x600dc0de。**尚未上机**。

- **2026-10-24 第二十六次上机（hfi26 那张）——CP 执行通用命令流已验证**：
  `CP self-test submit 0x0 fence=3 result=0 observed=3 wrote=0x600dc0de (want
  0x600dc0de)`。内核侧自此完整且端到端验证：GMU 冷启动 → OOB GPU_SET → GPU SMMU
  context（bank2/asid2/streams 0/1 + 带 mask 的 SMR）→ ring@0x200000 + fence 在
  memptrs@0x300000 → CP_SQE_CNTL → CP_NOP 探针 → a7xx_cp_init → 时间戳 fence →
  CP_MEM_WRITE 自检 → 用户客户端发布。剩下的是**用户态**那一半：
  `NativeAdrenoABI.h`（v1，16 个方法）目前没有任何消费者（没有库、没有测试工具、
  没有 Turnip 后端）。下一步计划写一个最小用户态客户端：开节点 → 分配/映射 BO →
  提交 PM4 → 等 fence → 校验 GPU 写入的值（同一份自检，但走真实 ABI）。

- **2026-10-24 第二十五次上机（hfi25 那张）——GPU READY（整条链路首次完整跑通）**：
  `CP started fence=2`、`CP interrupt mask 0x3b283c2 (want 0x33b283c2) refused …;
  polling fences`、`client=ready`、`GPU ready, publishing services`、`GPU started
  on gpu`。中断只丢了 bit28，fence 轮询不受影响。0146 加内核态自检提交：
  `CP_MEM_WRITE` 0x600dc0de 到 scratch 0x302040，用时间戳 fence 收尾再读回，验证
  CP 对任意 PM4 命令流的执行。镜像 `shim/out/shim-boot-hfi26.img`
  （sha256 `4831bf03…`，内核 `4b077679…`）。

- **2026-10-24 第二十四次上机（hfi24 那张）**：CP 起来了，但尾段停在
  `cp interrupt setup stopped at 0xe00002ca`——`NativeAdrenoIrq::programMask` 写
  `RBBM_INT_0_MASK` 后读回不一致就 poison（这块 RBBM 不保留我们请求的全部位）。
  ring 的 fence 本来是轮询 memptrs 的，中断只是优化，不该挡住客户端。0145 改成
  best-effort，并打印 mask/status 读回。镜像 `shim/out/shim-boot-hfi25.img`
  （sha256 `f028ce9c…`，内核 `f545de0c…`）。

- **2026-10-24 第二十三次上机（hfi23 那张）——CP 跑起来了**：
  `CP probe fence=1 result=0 observed=1`（环/事件写/时间戳全有效），紧接着
  `cp started fence=2`（a7xx_cp_init 被执行并确认）。至此整条链路在真机上首次打通：
  GMU 冷启动 → OOB GPU_SET → 带 mask 的 GPU SMMU context → ring@0x200000 →
  `CP_SQE_CNTL` → ME_INIT → 时间戳 fence。之前 ME_INIT 卡住是因为 **SQE 启动后的
  第一次提交不会被执行**，用一个两字的 CP_NOP 提交“点着”之后 ME_INIT 就通了。
  0144 在运行中的 CP 上恢复提交路径：中断 attach+mask、`NativeAdrenoSubmit` 和
  `NativeAdrenoUserClient` 绑到 **GPU context 的 VM**（GMU 的 VM 没有 GPU stream
  可达的 iova），并把节点标记为 ready。镜像 `shim/out/shim-boot-hfi24.img`
  （sha256 `6ae4cd32…`，内核 `50adafc7…`）。下一步是用户态提交（Mesa/Turnip）。

- **2026-10-24 第二十二次上机（hfi22 那张）**：改成阶梯的零表 ME_INIT 后结果不变
  ——`rptr=17=wptr`、`hw_fault=0 cp_int=0 rbbm_int0=0`、memptrs 全 0。也就是没有任何
  fault，CP 把整条提交都取了，但时间戳没写。0143 在 ME_INIT 前加一个只含
  `CP_NOP` 的探针提交（其唯一效果就是环自己的时间戳事件写）：探针 fence 到达 →
  问题在 ME_INIT；不到 → 事件写包本身没执行。镜像
  `shim/out/shim-boot-hfi23.img`（sha256 `2b087769…`，内核 `d5b12ab0…`）。

- **2026-10-24 第二十一次上机（hfi21 那张）**：`rptr` 从 0 走到 17 并与 `wptr` 相等
  → **CP 确实取了环并执行到 ME_INIT**，ring/CP/SMMU 链路都通；但时间戳写回没发生
  （memptrs 全 0），说明 CP 停在 ME_INIT 包里。阶梯的 a7xx_cp_init **不带 power-up
  寄存器表**（最后三个字为 0），而 kit 的版本会把 CP 指向 0x301000 的表并置读表位。
  0142 改成按阶梯的十个字直接 `ring_.submit`，并在超时后补读 `CP_HW_FAULT`/
  `CP_INTERRUPT_STATUS`/`RBBM_INT_0_STATUS`。镜像 `shim/out/shim-boot-hfi22.img`
  （sha256 `dcaac7eb…`，内核 `8945cd74…`），待上机。

- **2026-10-24 第二十次上机（hfi19/20 那张）**：`CP_SQE_CNTL=1 written` 不再挂死
  ——SMR mask 就是根因；CP 起来了（`rbbm=0x1 hw_fault=0`），ME_INIT 提交成功
  （`fence=1`），但 2 s 内 fence 没到（`observed 0`），超时 `0xe00002d6`。0141 在
  提交后和超时后各读一次 `rptr/wptr`，并把 memptrs 前 4 个字打出来，用来区分“CP
  没取环”和“取了但时间戳没写回”。镜像 `shim/out/shim-boot-hfi21.img`
  （sha256 `0da50dcb…`，内核 `f81cbbb4…`）。

- **2026-10-24 第十九次上机（hfi19 那张）**：`GPU route smr1=0x84000000 s2cr1=0x2
  smr2=0x84000001 s2cr2=0x2`——两条 GPU route 都按阶梯的带 mask 形式落进去了。
  0140 在 `CP_SQE_CNTL` 之前加只读 SMMU preflight（IDR1 推导 CB 几何，读 SGFSR、
  bank2 的 SCTLR/TCR/TTBR0/FSR），用来确认 CP 取指路径是否已经 fault。镜像
  `shim/out/shim-boot-hfi20.img`（sha256 `9b5d7108…`，内核 `46d73522…`）。

- **2026-10-24 第十八次上机（hfi18 那张）**：顺序按阶梯改对后仍在 `writing
  CP_SQE_CNTL=1` 挂死，所以不是顺序。剩下的嫌疑是 **CP 自己的取指**：它经 GPU 的
  SMMU stream 读 SQE。kit 的 stream attach 写 `SMR = VALID|sid`（0x80000000|sid），
  而阶梯（以及 0102 给 GMU route 修的写法）是 `VALID|0x400<<16|sid`；qsmmu 会丢掉
  没有 mask 的 SMR，于是 GPU 的 stream 0/1 从未匹配，CP 取指走默认路径 fault，CP
  卡死并不再应答寄存器写。0139 在 context 建好后按阶梯重写两条 GPU route 并打印读回
  `GPU route smr1=0x84000000 s2cr1=0x2 …`。镜像 `shim/out/shim-boot-hfi19.img`
  （sha256 `8b9fbb3c…`，内核 `d2c2f257…`），待上机。

- **2026-10-24 第十七次上机（hfi17 那张）**：补了 BV rptr 和 RB_CNTL bit27 后仍在
  `CP_SQE_CNTL` 挂死。与阶梯 step 20 的剩余差异是**顺序**：阶梯先写
  `CP_AHB_CNTL=1`（第 12 条）再写 `CP_RB_*`（第 28–31 条），而我们的 ring attach
  在寄存器表之前就写了 `CP_RB_*`（此时 CP 的 AHB 还没开）。0138 把 `startCp` 拆成
  两相：相 0 写寄存器表 + 发布 power-up 表 → ring attach → 相 1 启 SQE + ME_INIT +
  fence。镜像 `shim/out/shim-boot-hfi18.img`（sha256 `65a2f2a6…`，内核
  `cd15ab84…`），待上机。

- **2026-10-24 第十六次上机（ladder20 那张）——阶梯证明这套 CP 序列在本机可行**：
  `CP_SQE_CNTL 0 -> 0x1 (reads 0x1)`、`done (rptr 10)`、`THE 3D CORE TAKES
  COMMANDS`。与我们的 `startCp` 逐条对比只剩两处差异，且都能解释“CP 卡住→总线
  不应答”：①**`CP_BV_RB_RPTR_ADDR` 我们没写**（复位 0，SQE 启动后 BV 环的 rptr 会
  往地址 0 写，翻译失败把 CP 卡死）；②**`CP_RB_CNTL` bit27**（阶梯 0x800020c，我们
  0x20c）。0137 补上这两条并把 SQE 启动恢复为默认开（`sheng-gpu-sqe-start=0` 可关）。
  镜像 `shim/out/shim-boot-hfi17.img`（sha256 `23396c41…`，内核 `4ee2b1b2…`）。

- **2026-10-24 第十五次上机（hfi16 那张）**：不启 SQE 时系统正常，数据齐全：
  `CP_SQE_CNTL reads 0x0`、`rbbm=0x1`、`hw_fault=0`、`sqe`/`rb` 读回与写入一致
  （0x100000 / 0x200000）→ CP 块活着且编程正确；ME_INIT 提交成功（fence=1）但
  fence 不到，说明 CP 没执行；写 `CP_SQE_CNTL=1` 才会挂总线。下一张
  `shim/out/shim-boot-ladder20.img`（sha256 `53f1eca3…`）用 `liuqinadreno=20`、
  不带 `sheng-gpu-bringup` 让阶梯自己跑到 step 20，逐条打印寄存器写入/读回，
  作为“这套 CP 启动序列在本机到底能否成功”的判据。

- **2026-10-24 第十四次上机（hfi15 那张）**：补了 GBIF_HALT 后仍旧停在同一行
  （`CP registers written, starting the SQE` 之后重启）——`CP_SQE_CNTL(0x0808)` 的写
  本身就是总线停住，别的 CP 寄存器都正常。0136 把这次写改成默认不发
  （`sheng-gpu-sqe-start=1` 才发），让系统能起完，同时先读一次 `CP_SQE_CNTL`、保留
  `sqe=`/`rb=` 读回、ME_INIT 提交和 fence 等待，用这一轮把 SQE 取指路径的情况摸清。
  镜像 `shim/out/shim-boot-hfi16.img`（sha256 `932480cf…`，内核 `aa7fa5a6…`）。

- **2026-10-24 第十三次上机（hfi13 那张）——GX 上电，GPU 窗口确认为 0x3d00000**：
  `OOB GPU_SET acked (intr=0x80000000)` 之后 `GPU window ADT 0x2d00000 id=0x0 |
  DT 0x3d00000 id=0x70040000 status=0x1`——ADT 的 gpu 节点地址是错的，vendor DT 的
  0x3d00000 才是活的；探测自动切换后 `CP ring attached`、寄存器表全部写完。随后挂死
  并重启，最后一行是 `CP registers written, starting the SQE`，即卡在
  `CP_SQE_CNTL=1`。0134 把这条写前后加面包屑，0135 补上阶梯 step 20 的第一条
  `GBIF_HALT(0x3c45)=0`（我们漏了，GBIF 停着 SQE 首次取指就完不成）。镜像
  `shim/out/shim-boot-hfi15.img`（sha256 `59b283e5…`，内核 `fe5a86f9…`），待上机。

- **2026-10-24 第十二次上机（hfi12 那张）**：HFI 30 确实被确认了（`elapsed` 1.4 ms →
  19 ms、`q1 rd/wr 0x1c`、`polls=87`），0x3d00000 的映射也成功
  （`GPU window DT map 0xfffffe503f9ec000`），但两个窗口仍读 0——地址和 vote 都没
  问题，是 GX 域压根没被抬起来。阶梯 step 18 还有一步 kit 没有的
  `a6xx_gmu_set_oob(GMU_OOB_GPU_SET)`：写 `HOST2GMU_INTR_SET` bit30 → 等
  `GMU2HOST_INTR_INFO` bit31 → 清除，之后 GMU 自己打开 GX GDSC。0133 加了
  `powerGx()` 并在冷启动后立刻执行。镜像 `shim/out/shim-boot-hfi13.img`
  （sha256 `cfc6ada5…`，内核 `db9c28ae…`），待上机。

- **2026-10-24 第十一次上机（hfi11 那张）**：两个 GPU 窗口都读 0（`ADT id=0x0 |
  DT id=0x0`），地址不是问题。解析 vendor DTB 确认：`kgsl-3d0@3d00000` 的 reg 是
  0x3d00000/0x40000 等，ADT 的 0x2d... 就是同一批块整体低 0x1000000，而 GMU 正是
  在 ADT 的 0x2d68000 上跑起来的——所以 GPU 块是**没上电**：GMU 在跑（always-on
  域），但从没抬 GX 域。缺的是阶梯 resume 的最后一条 **HFI 30**（初始 GX perf/bw
  vote，{0, 1, 1, 3}）；kit 的启动序列止于 START。0132 在六步之后补发这条，并在
  探窗口前留 20 ms 给域上电。镜像 `shim/out/shim-boot-hfi12.img`
  （sha256 `e6ab8bf7…`，内核 `60b8324e…`），待上机。

- **2026-10-24 第十次上机（hfi10 那张）**：`sqe=0x00000000 (want 0x100000)`——写进
  `CP_SQE_INSTR_BASE` 读回 0，整个 GPU 窗口（ADT 的 0x2d00000）不接受写，CP 失败是
  它的下游。地址对不上：vendor DTB 是 `kgsl-3d0@3d00000`、`gmu@3d69000`，阶梯也用
  0x3d00000 真读到过 RBBM_STATUS；而 ADT 是 0x2d00000（整体低 0x1000000），可 GMU
  又确实在 ADT 的 0x2d68000 上跑起来了。0131 双地址探测：读两边 dword0/0x210 并
  打印，选能应答的那个窗口给 ring/CP 用。镜像 `shim/out/shim-boot-hfi11.img`
  （sha256 `7841d743…`，内核 `1ebf742c…`），待上机。

- **2026-10-24 第九次上机（hfi9 那张）**：`CP SQE alive rbbm=0x0 hw_fault=0x0 rptr=0`
  之后 `CP start stopped at 0xe00002ca`（`kIOReturnIOError`，没有 fence 等待日志，
  说明错在 startCp 自己的步骤里）。查到两处：power-up 表被写进了 **fence 页**
  （传参错），而且失败路径不出声。0130 传对 pwrup buffer，并把每一步的状态打出来
  （`sqe=0x..%08x (want 0x100000)` 的读回还能判断 0x2d00000 这个 GPU 窗口到底能不能
  写——stock DT 里 GPU 在 0x3d00000）。镜像 `shim/out/shim-boot-hfi10.img`
  （sha256 `91a18643…`，内核 `8223c6ab…`），待上机。

- **2026-10-24 第八次上机（hfi8 那张）**：`CP ring attached ring 0x200000/0x8000
  fence 0x300000/0x1000`——ring 寄存器在 GPU context 自己的 buffer 上编程成功，
  rptr 也能读回；系统照常起完。0129 接上 CP 启动：按阶梯 step 20 的寄存器表
  （RBBM/UCHE/CP 配置 + CP_SQE_INSTR_BASE）→ `CP_SQE_CNTL=1` + 20 ms → A740
  power-up list + a7xx_cp_init 经 ring 下发 → 等时间戳 fence（2 s）。镜像
  `shim/out/shim-boot-hfi9.img`（sha256 `fd96301c…`，内核 `7097b58d…`），待上机。

- **2026-10-24 第七次上机（hfi7 那张）——GPU context 全部建成，不再 panic**：
  HFI 六步全过（`ack=0xde`），SMMU 空槽 1/2，GPU context `status=0 step 7`
  （prepare/program phase 5 13 写/attach 2 路校验/vm attach/5 个 buffer 全部映射），
  `CP: sqe 74672 bytes at 0x100000`。随后 `ring/SQE refused` 是因为
  `gpu_context_ready_` 从未置位。0128 置位它，并把旧的 ring 路径（在 GMU VM 的
  iova 0 上自建 buffer）换成 GPU context 自己的 buffer：ring 0x200000/32KiB、
  fence 在 memptrs 0x300000，cache 维护走 buffer 自己的物理页；CP init 留到下一步。
  镜像 `shim/out/shim-boot-hfi8.img`（sha256 `93de7d13…`，内核 `2f3272b5…`）。

- **2026-10-24 第六次上机（hfi6 那张）**：面包屑最后一行 `A3 i=16 smr=00000000
  s2cr=00020000`，之后立刻 data abort——故障在槽位 17 的 SMR/S2CR 读。两条事实：
  `IDR0=0x4c017e18` 的低 8 位 = 24 个 SMR（阶梯就是按这个上界扫的），我们硬扫 32；
  而且找到两个空槽后没有提前退出。0127 把扫描限制在前 16 项、逐项打印
  （`A3 ... free=`）、找齐就 break，并允许 `sheng-gpu-gpuslot0/1` 直接指定跳过扫描。
  镜像 `shim/out/shim-boot-hfi7.img`（sha256 `80724699…`，内核 `4820cf85…`）。

- **2026-10-24 第五次上机（hfi5 那张）**：`ColdStart status=0 ack=0xde elapsed=1408 us`
  全过；随后仍在同一个相对位置 `NativeAdrenoGpuService::start+0x14d4` data abort，
  而且 `GPU ctx call` 没打印出来——崩溃点在 HFI dump 之后、第一条 GPU context 日志
  之前，也就是四条 `PE_parse_boot_argn` 或 32 槽 SMR/S2CR 扫描里。0126 加面包屑
  `A1/A2/A3/A4`，并给 `sheng-gpu-ctx=0` 开关（跳过 GPU context，保证能起完）。
  镜像 `shim/out/shim-boot-hfi6.img`（sha256 `d76b184c…`，内核 `6cd9b68b…`）。

- **2026-10-24 第四次上机（hfi4 那张）——启动序列全部通过**：`ColdStart status=0
  ack=0xde stage=8 transport=0`，六条消息（性能表、带宽表、ACD 使能、ACD 表、
  CORE_FW_START、START）全部被确认，`q0 0x18/0x18`、`q1 0x18/0x20`。紧接着在 GPU
  context 建立阶段内核 data abort（还在 `GPU context status` 那行之前），照片太糊，
  pc/lr 读不准。0125 给这条路径加了逐步打印（call/allocate/enter/tables/buffers/
  prepare/program/attach/vm attach/map N + Step 回调），下次上机最后一行就能定位。
  镜像 `shim/out/shim-boot-hfi5.img`（sha256 `8ea9a0ec…`，内核 `2e0ad0e4…`）。

- **2026-10-24 第三次上机（hfi3 那张）**：`ack=0x5e`——性能表、带宽表、ACD 使能、
  ACD 表、CORE_FW_START 五步全部被固件确认，索引驱动读包完全正确。最后一步 START
  报 `status=1`（`ShengGmuArgument`）：kit 的编码器给 START 的是**零字**负载（只有
  首部），而我的交换函数拒绝 `words==0`。0124 允许零字命令（响应仍限 19 字）。
  镜像 `shim/out/shim-boot-hfi4.img`（sha256 `7fd64f41…`，内核 `b5b2d443…`），待
  上机；全过应为 `status=0 ack=0xde`，随后是 `GPU context status ...`、
  `CP: sqe ...`、ring attach。

- **2026-10-24 第二次上机（hfi2 那张）——HFI 真的通了**：`ColdStart status=10
  stage=6 ack=0x1e`，即性能表/带宽表/ACD 使能/ACD 表四步都收到了带正确序号的
  ACK；`q0 rd/wr 4/4`（GMU 消费了每条命令）、`q1 rd/wr 0/0x14`（写回五个 4 字
  ACK）。`transport=6` 失败在第五步 CORE_FW_START，原因是我的交换函数只看固定
  槽位 0/4/8/12，而第五个 ACK 在字偏移 16。之前"这颗 GMU 不推进索引"的结论作废
  ——那是 kit 路径下命令从未被消费时的读数。0123 改成阶梯的索引驱动：按 `write`
  发布、等中断、按 `read` 读包并推进，不再扫描、不再清零窗口，跳过 F2H 错误和别
  的序号。镜像 `shim/out/shim-boot-hfi3.img`（sha256 `7339aa23…`，内核
  `c1ec0d3d…`），待上机；成功时应是 `status=0 ack=0xde`，随后 GPU context 和
  `CP: sqe ...`。

- **2026-10-24 第一次上机（hfi=1 那张）**：`ColdStart status=1`（`ShengGmuArgument`），
  `proto stage=1 polls=0 transport=4 elapsed=4 us`——是我的新传输把**命令**包也卡在
  19 字（那是 ACK 的上限），性能表 ~150 字，第一次交换就被参数检查拒了，GMU 一个
  寄存器都没碰（失败路径随后把 GMU 复位，dump 里 `sysreset=0x1`）。0122 拆成
  `SHENG_HFI_COMMAND_WORDS=254` / `SHENG_HFI_PACKET_WORDS=19`，总预算 200 ms → 2 s
  （六条消息各 100 ms 放不下），ColdStart 行加 `ack=0x%x`。镜像
  `shim/out/shim-boot-hfi2.img`（sha256 `2e8b83c8…`，内核 `af323341…`），待上机。

### 2026-10-24（HFI 改用阶梯的中断传输，待上机）

- 事实：这颗 Gen7 GMU 会把 ACK 包写进响应窗口并抬起 `GMU2HOST_INTR`，但
  **两个队列的 read/write 索引都不推进**（还顺手改写了表头 `size` 字段）。
  kit 的 `AdrenoHfiPoll` 是索引驱动的，所以真实 ACK 在内存里也只会报
  `Empty`；0116/0119 的扫描能过 stage 1，但把环弄错位，最后 panic
  （`_kalloc_type_view_parse`，0120 已撤）。
- 0121 新增 `ShengGmuColdStartInterrupt()`：按阶梯 `hfi_send` 的顺序，清零两个
  窗口 → 在窗口基址发布命令并把命令侧索引写成一致状态 → clean + 清旧中断 +
  脉冲 `HOST2GMU_INTR` → 限时等 `GMU2HOST_INTR_INFO` bit0 并 CLR → invalidate
  后只接受"长度 1..19 + id 相同 + 序号相同"的包（4 个按字对齐的基址槽）。
  不推进任何固件自己的索引，stale 包因为窗口已清零且序号在涨，不可能误判。
  用 `sheng-gpu-hfi=1` 选择这条路径，`0`（默认）仍是 kit 的索引传输。
- 同一补丁加了 `mode=intr` 的对照打印（table 6 字、两组索引、rsp/cmd 前 4 字、
  `intr`）。判读：`transport=4` = 门铃没抬起来（通知路径问题）；`transport=6`
  且 `intr=0x1` = 答了但包不在模型位置（看 rsp/cmd dump）；`transport=7` =
  固件拒绝，`fwerr` 是它的错误码。
- 镜像 `shim/out/shim-boot-hfi.img`（sha256 `bba49838…`，内核 `3d6336e7…`），
  boot line = cp15 那行再加 `sheng-gpu-hfi=1`。**尚未上机。**
- 本轮增量构建用 `WORK=/tmp/liuqin-build3 tools/build-local.sh`（干净，0 error），
  `tools/validate-repo.py` 通过，`tools/verify-patches.sh` 现在**全部 120 个补丁
  干净应用**。顺带修了两个一直存在的补丁问题：0112 加了三个 dump 参数却没改
  `q1 %08x %08x %08x` 格式串（导致 0113 的上下文对不上），0120 的上下文早于
  0119 加的那句注释。修完后从零重建的树与构建树在 0121 涉及的每个文件上逐字节
  相同。


### 2026-10-24 上午（桌面卡点：BKS check-in，不是 GPU）

- 把 root 里的 `HIDPlugins/SessionFilters` 和 `ServicePlugins` 改名后重新刷
  userdata：backboardd 的 `BKHIDSystem init` 过了（sampler 能跑满 40 次采样，
  之前卡在 sample 8 的插件枚举）。
- 但 SpringBoard 只是往前走了一步：现在停在
  `BKSSystemShellService _checkIn`（同步 XPC 等 backboardd 回复），backboardd
  主线程在 run loop 空闲、一个队列线程在 `dispatch_group_wait`；backboardd 在
  整个 boot 里 533 次探测 `IOAcceleratorES` 都是 none。
- ios-sweet 的 NOTES 记录过同一个墙（"no GPU, so no CoreAnimation renderer
  yet"），他们最后是靠 CPU 渲染 + 显示模式/scale/root 状态修过去的，不是 GPU。
  liuqin 现在在同一位置。
- 下一步候选：对比 sweet 的 display-scale（liuqin 2 / 1800x2880，sweet 3 /
  1080x2400）和 IOMFB frame-info/refresh 返回值；再决定是否要给 backboardd
  一个 stub accelerator 或改渲染路径。

### 2026-10-24 凌晨（GPU：接上 GPU context）

- 冷启动成功后服务现在会建 GPU 自己的 SMMU context：bank/asid 默认 2/2
  （`sheng-gpu-gpubank`/`gpuasid` 可覆盖），stream 0/1 都路由到 bank 2；
  两个 SMR 槽自动取 GMU 之后的空闲项（SMR 无 VALID 且 S2CR type 2），
  也可用 `sheng-gpu-gpuslot0/1` 指定。
- 成功时打印 `GPU context status 0x0 step 7` 和
  `GPU buffers sqe 0x100000 ring 0x200000 memptrs 0x300000 pwrup 0x301000
  scratch 0x302000`。
- 旧的 ring/SQE 块仍会拒绝（`gpu_context_ready_` 还没置位）——下一步是 CP 提交
  （SQE 微码、pwrup 列表、ring 门铃）。
- 镜像 `shim-boot-gpuctx.img`（内核 `76ddbef1…`，sha `069fe391…`）。

### 2026-10-24 凌晨（GPU：Sheng 冷启动真机跑通）

- `ColdStart status=0 fw_ready=1 hfi_ready=1 stage=1 stopped_at=0
  fw_init=0x100 hfi=0x1 polls=20`：CM3 释放、固件 `FW_INIT_RESULT=0x100`、
  HFI ready、`AdrenoHfiStartupRun()` 走完（stage 1）——A730 命令 DB 生成的
  vote/ARC/BCM 配置被 GMU 接受了。**Sheng 冷启动整条链在真机上通了。**
- 关键修复序列：0094 scattered、0095 16K 页、0096 带宽 0 档、0097 io/services、
  0102 SMR/S2CR（VALID+mask）、0103 100ms 轮询、0104 ICACHE/HFI cache clean。
- 下一步：GPU context（bank 2/asid 2，stream 0/1）+ SQE/pwrup + CP 提交，
  然后接 user client。服务现在会打 `ring/SQE refused: the GPU context ... is
  not built`，这是预期行为。

### 2026-10-24 凌晨（GPU：固件没跑，因为 ICACHE 还在 CPU cache 里）

- 100 ms 预算下 `fw_init=0x0 polls=783`：CM3 从没写 `FW_INIT_RESULT`。
- 固件和 HFI 队列表是普通 cacheable CPU 写，GMU 通过 SMMU 读 ICACHE；
  阶梯每次加载完 ICACHE 和初始化 HFI 后都会调 `to_memory()`（clean 到 PoC），
  而 Sheng 框架只做了 `dsb`，GMU 可能取到 cache 里的旧/零数据。
- 补丁 `0104-liuqin-gmu-cache-clean.patch`：`ShengGmuServices` 加 `clean`
  回调，固件加载后、CM3 释放前对 icache/dummy/debug/log/hfi 做 clean；
  `NativeAdrenoGmuPlatform::services()` 指向
  `NativeAdrenoMaintainOwnedPages(..., NativeAdrenoClean)`。
- 新镜像 `shim-boot-route3.img`（内核 `26e4407b…`，sha `53869602…`）。

### 2026-10-24 凌晨（GPU：CM3 起来了，FW_INIT_RESULT 轮询超时）

- 按阶梯值重写 SMR/S2CR 后，完整 ColdStart **不再复位**：
  `route smr0 0x84000005 s2cr0 0x1`、`ColdStart status=6 fw_ready=0 hfi_ready=0
  stage=0`。status 6 = `ShengGmuTimeout`，卡在 `FW_INIT_RESULT` 轮询。
- 移植的 `poll()` 只给固件 10 ms（A740 的预算）；A730 的 CM3 要通过 SMMU 取
  ICACHE，10 ms 不够。改成 100 ms，并在 ColdStart 行里加打
  `fw_init=<最后读到的值> hfi=<值> polls=<次数>`，用来区分“还没 ready”（0）
  和“掩码不对”（例如固件写 0xbabeface 而代码等 0x100）。
- 新镜像 `shim-boot-route2.img`（内核 `8faeca0e…`，sha `67efd4cb…`）。

### 2026-10-23 深夜（GPU：GMU 的 stream match 没有 VALID）

- probe3 打出：`scr0 0x2e0406 smr0 0x00000005 s2cr0 0x1 smr1 0x0 s2cr1 0x20000`。
  SCR0 已使能、S2CR0 是 context 1，但 SMR0 的 VALID 位是 0（阶梯写的是
  `0x84000005` = VALID | mask 0x400 | sid 5）。
- 阶梯的写法：`SMR = VALID | (0x400 << 16) | sid`；kit 的
  `ArmSmmu4KAttachStreams()` 只写 `VALID | sid`，没设 mask，qsmmu 最后把
  VALID 位留成了 0。这样 stream 5 不匹配，CM3 第一次取指（IOVA 0x4000）
  落到默认 fault/bypass，正好对应复位。
- `shim-boot-route.img`（内核 `beed603b…`，sha `d4762dcc…`）：带
  `sheng-gpu-route=1` 时在 GMU context 建好后按阶梯的值重写 S2CR/SMR 并打印
  回读 `route smr0 … s2cr0 …`，然后跑完整 ColdStart（去 mute）。
- 判读：回读保持 0x84000005 且 CM3 还复位 → 路由不是全部原因；回读还是 0x5
  → qsmmu 拒绝这个写，得换方式建 context。

### 2026-10-23 深夜（GPU：qsmmu 的 context bank 读会 panic）

- probe2 在打印 `readonly` 之后 panic（Kernel data abort）：多读的 context
  bank SCTLR（偏移 0x8000）在这颗 `qcom,qsmmu-v500` 上会 trap——hypervisor 只
  模拟全局寄存器（SCR0/SMR/S2CR），阶梯也只碰这些。
- probe3（内核 `5ca024b5…`，sha `3b40709a…`）去掉了 CB 读，保留
  `scr0/smr0/s2cr0/smr1/s2cr1`。对照阶梯：SCR0 0x002e0406、SMR0
  0x84000005（stream 5）、S2CR0 context 1/type 0。
- 另外：probe2 的 panic 有完整文本（去 mute 后），所以之前 gmu 镜像的“无文本
  复位”也可能是一次被 mute 掉的 panic——下一步若 probe3 的 SMR/S2CR 不对，
  就能解释 CM3 释放后的取指失败。

### 2026-10-23 深夜（GPU：stop1–4 全过，复位在 CM3 释放/固件取指）

- stop1/2/3/4 四张镜像**都能读条**：19 条 RSC/PDC、FAL/TCM retention、
  固件加载+TCM 回读、配置回读（含 SYSRESET 保持 1）都安全。
- 所以复位就发生在 `CM3_SYSRESET = 0` 之后、CM3 第一次取指时。首要怀疑
  GMU 的 SMMU context：CM3 从 IOVA 0x4000 取 ICACHE，走 stream 5；如果
  这个 stream 没有像阶梯那样路由到 context 1（SMR0 = 0x84000005），取指就会
  fault 或 bypass 到 DRAM。
- 新探针 `shim-boot-probe2.img`（内核 `ae071c60…`，sha `f583807e…`）在 GMU
  context 建好后只读地打印 `scr0 smr0 s2cr0 smr1 s2cr1 cb1_sctlr`，用来和
  阶梯的值对比（SMR0 0x84000005、S2CR0 context1/type0、SCR0 0x002e0406）。
- 如果 SMR0/S2CR0 是 0 或 type 2（bypass），就是 context 路由的问题。

### 2026-10-23 深夜（GPU：探针只读安全；复位在写路径，开始二分）

- 探针镜像上机：**没有复位**，打印
  `readonly sysreset 0x1 hfi 0x0 cx 0x0 rscc 0x0 ao 0x0 cm3 0x0`——六个状态
  寄存器都读得到，而且正好是 `ShengGmuLoadAndStartHfi()` 写之前要求的状态
  （CM3 在复位、HFI 未 ready、retention 清、RSC idle、无 fault）。
- 所以 0097 的复位发生在**写**路径，不是读。
- 探针那次 boot 的 HFS+ root 连续 37 次挂载失败（`unknown Volume Signature :
  666b`），但下一次 boot 正常挂载并跑到 1305 s，**userdata 没坏**，是那次
  boot 的偶发问题（那张镜像去掉了 mute）。
- 新工具：`sheng-gpu-stop=N`，在 LoadAndStartHfi 的第 N 个阶段后直接返回，
  不做任何清理写：1=19 条 RSC/PDC 写后，2=FAL/TCM retention 后，3=固件加载+
  TCM 回读后，4=配置寄存器回读后（CM3 释放前）。
- 四个镜像 `shim-boot-stop1..4.img` 都是**去 mute** 的，屏幕能看到
  `ColdStart status=... stopped_at=N`。按 1→2→3→4 刷，第一次复位的那个 N 就是
  挂住的位置。

### 2026-10-23 深夜（GPU：ColdStart 开始碰 GMU 后整机复位）

- 0097 的镜像：`gpu ladder done at 47.024 s` 之后整机重启，没等到任何
  `ColdStart status` 行。这是第一次 ColdStart 真正访问 GMU（io/services 已接），
  复位点可能在：第一次 GMU 寄存器读、19 条 RSC/PDC 写、FAL/TCM retention、
  或 CM3 释放后的固件启动。
- 诊断镜像 `shim-boot-probe.img`（内核 `541d1cf9…`，sha `58958a3d…`）：
  去掉 `liuqinmute`（屏幕显示所有 IOLog），加 `sheng-gpu-readonly=1`——只读
  六个 GMU 寄存器然后停，不写任何寄存器。
- 判读：出现 `NativeAdreno: readonly sysreset ...` = 读没问题，复位在写/固件
  路径；在那行之前复位 = `liuqinadreno=5` 停下的时钟/电源状态下 GMU 窗口不可
  安全访问，需要让阶梯多跑几步或显式给 GMU 上电。
- 另：用户没说是 panic 屏还是直接重启；下次留意有没有 panic 文本。

### 2026-10-23 深夜（GPU：startup config 过了，ColdStart 拿到全零 device）

- 0096 上机：`startup config refused` 消失，说明 0 带宽档的修复生效；ColdStart
  真的被调用了：`ColdStart status=1 fw_ready=0 hfi_ready=0 stage=0`。
- status 1 = `ShengGmuArgument`：`ShengGmuColdStart()` → `LoadAndStartHfi()` 的
  第一个 validate 检查 `d->io` / `d->services`，而服务只建了 `gmu_io_` 和
  `services`，从没写进 `gmu_device_`，所以 device 全零、在任何寄存器访问前被拒。
- 补丁 `0097-liuqin-gmu-device-io.patch`：调用前赋值
  `gmu_device_.io=gmu_io_; gmu_device_.services=services;`。
- 新镜像：内核 `1acebdee…`，`shim-boot-gmu.img` `bd7450b9…`，默认
  `shim-boot.img` `a340e62f…`。**未上机**。下一轮进入真正的 GMU load：
  RSC/PDC 19 写、FAL、TCM retention、HFI 队列、15 个固件块、TCM 回读、
  FW_INIT_RESULT 轮询、HFI start；任何非 0 status 都直接发我。

### 2026-10-23 深夜（GPU 自举：GMU context 过了，startup config 被拒）

- 0095 上机：`GMU payloads mapped … ttbr0=0x100084711c000`、`GMU firmware
  validated (57272 bytes)` 都出来了——16K 页这条通了。停在
  `startup config refused`。
- 已知 bug：`AdrenoHfiBuildBandwidth()` 要求带宽表第一项是 0 且严格递增，
  生成器把 0 过滤掉了（表从 1720 开始），已修（7 档，从 0 起）。
- 另外加了诊断：`ShengGmuBuildStartup()` 记录 `sheng_gmu_vote_stage`
  （1 参数/2 GPU ARC/3 GMU ARC/4 BCM aux/5 带宽/6 最终校验）和各 aux 的字节数，
  失败时服务打印一行 `startup config refused stage N (...)`。
- 新镜像：内核 `51ad7a6b…`，`shim-boot-gmu.img` `d2e9d6df…`，默认
  `shim-boot.img` `1fa8458c…`。**未上机**。下一轮看 `ColdStart status` 或
  stage 号：2/3 = 命令 DB 的 ARC 表和这颗 SoC 不匹配，4 = BCM aux 不是 8 字节
  记录，5 = BCM 地址/尺寸（SH0/MC0/ACV 是猜的那组）。

### 2026-10-23 深夜（GPU 自举卡点：16K 页被 4K 检查挡住）

- 0094 的镜像上机：`16760832 bytes not contiguous, retrying scattered` 之后
  仍然 `allocation failed 0xe00002c7`。两次都在描述符之前失败，根因不是内存，
  而是 `NativeAdrenoMemory::allocate()` 开头的 `NativeAdrenoQueryCpu4K()`：
  它只接受 4 KiB 粒度内核，而这个移植是 **16 KiB 页**。
- 补丁 `0095-liuqin-gmu-16k-pages.patch`：接受 16 KiB（TG0=2/TG1=1），并在
  16 KiB 页上关掉 write-combine（`pmap_set_cache_attributes()` 按 4 KiB 页号
  编程，在 16K 内核上会改到无关页）；显式 cache 维护不变。
- 新镜像：内核 `5e32e4b1…`，`shim-boot-gmu.img` `267582ea…`，默认
  `shim-boot.img` `a5c19869…`。**未上机**。下一轮预期能看到 GMU context 的
  Prepare/Program/Attach/Map 走完，进入固件加载和 ColdStart。

### 2026-10-23 深夜（GPU 自举第一次真机：卡在 16 MiB ICACHE 分配）

- `shim-boot-gmu.img` 上机（`liuqinadreno=5` + sheng 参数）：CX 窗口
  `0x2d9e000/0x1000` 对了，SMMU 读到 `id2=0x5511`、`scr0=0x2e0406`，
  GMU 五窗口绑定成功，ColdStart 路径真的跑到了。停在：
  `GMU context stopped at step 1 status 0xe00002c7`（kIOReturnUnsupported）。
- 原因：GMU allocator 第一块是 16 MiB 的 ICACHE（0xffc000），要求物理连续；
  拿不到连续段时 `NativeAdrenoMemory::allocate()` 以 Unsupported 失败。
  SMMU 是逐页映射的，不需要物理连续（Linux 那边也是 IOMMU 映射）。
- 补丁 `0094-liuqin-gmu-scattered.patch`：连续分配失败时退回 scattered，
  并打印大小和回退；这样下一轮要么过 Allocate，要么报出具体是哪个 buffer。
- 新镜像：内核 `2787b458…`，`shim-boot-gmu.img` `6d5b2dfb…`，默认
  `shim-boot.img` `f7cb672a…`。**未上机**。下一步看
  `GMU payloads mapped` / `firmware validated` / `ColdStart status`。

### 2026-10-23 深夜（GPU 路线 A：Sheng 表和服务自举已编进内核）

- 移植了 `ShengGmu.c` / `ShengGmuVotes.c` / `ShengGmuCommandDb.c`（之前只有头，
  所以服务里的 ColdStart 整段被编掉），并加进 `iokit/conf/files.arm64`。
- 新工具 `tools/prepare-liuqin-gmu.py` 生成三张表：寄存器/19 条 RSC-PDC/
  窗口/固件 sha、GPU OPP（vendor DTB 的 13 档 + DDR 带宽表）、命令 DB
  （0x80860000/0x20000）。偏移用 `CORE_GPU_DELTA=0`、窗口内 dword，和阶梯的
  `GMU_REG()` 等价。
- 服务改了两处：GMU 固件改从 fwarea 读（`liuqin_fw_get("gmu_gen70000.bin")`，
  弱符号里没有固件）；CX 窗口按 `reg-names = "cx_misc"` 找（原来是 index 1，
  实际是 `cx_dbgc`）。
- 内核里 “not generated” 字符串消失，`ColdStart status=...` 在。
- 新实验镜像 `shim/out/shim-boot-gmu.img`（`d4c48f2e…`，内核 `db5577b1…`），
  cmdline 加 `liuqinadreno=5 liuqinsvc=1 sheng-gpu-bringup=1
  sheng-gpu-bank=1 sheng-gpu-asid=1 sheng-gpu-sid=5 sheng-gpu-slot=0
  sheng-gpu-cmddb-version=1`。**没上机**。默认镜像 `shim-boot.img` 仍是
  `56dc6823…`（同样换了新内核）。
- 下一步：先上机看 `ColdStart status`；如果 status=0 / fw_ready=1 /
  hfi_ready=1，再接 `NativeAdrenoGpuContext`（bank=2 asid=2，slots/sids 0/1）
  和 ring/SQE。BCM 的 SH0/MC0/ACV 是唯一没把握的数据。

### 2026-10-23 深夜（GPU：GMU 修复真机确认，选路线 A）

- 新镜像（`b0a98ee5`）上机确认了 cell 顺序修复：日志有
  `NativeAdreno: GMU five-window I/O adapter bound`，三个服务照常
  attach/start/register，GPU 节点仍是 `discovered`。
- 服务起不了 CP 的原因已定位：构建里没有 `ShengGmuSource.h` /
  `ShengGmuVotesSource.h` / `ShengGmuCommandDbSource.h`，服务的 GMU
  ColdStart 整段被 `__has_include` 编掉（内核里只有 “not generated” 字符串）。
  路线定为 **A：为 liuqin 生成这三张表**，让服务自己起 GMU/GPU/CP。
  完整的数据清单、19 条 RSC/PDC 写、窗口/偏移公式（`(word<<2)-0x69000`）、
  固件与 command DB、以及缺的 OPP 带宽/BCM 数据都写在 `docs/GPU_NEXT.md`。
- 已知的坑：`ShengGmuBindMmio()` 会逐窗口比较 physical/bytes，而服务目前
  混用了 ADT 相对地址（0x2d…) 和映射后的 VA；生成表之前必须先定死用哪一套，
  否则 bind 返回 `ShengGmuArgument`。
- 下一步（按 GPU_NEXT.md 的 5 个检查点）：先写
  `tools/prepare-liuqin-gmu.py` 生成三张表并让 “not generated” 消失，再接
  `NativeAdrenoGpuContext`。

### 2026-10-23 深夜（UFS HS 真机验证通过；下一步是用户态）

- 0092 的时钟层镜像已上机（`shim-boot-hs.img`，内核 `2d33f4bc…`，
  `ufsphyhs=1`）：**HS 生效**。首个 1024 条命令窗口 74 MiB / 4.663 s =
  ~16 MiB/s（之前 0.5 MB/s），延迟 min/avg/max 15/176/2898 us（之前
  730/112050/843414 us），>1 ms 的命令 4/1024（之前 1022/1024）。
  完整日志与解读在 `docs/BOOT_PERFORMANCE.md` 的 “What it printed” 一节。
- 启动里程碑（内核时钟）：`ufs init start` 43.9 s、`root device ready`
  47.08 s、`exec launchd` 47.14 s、`exec SpringBoard` **49.65 s**、
  `exec backboardd` 49.85 s。20 分钟的 rootfs 读取没有了。
- 三个日志问题已在同一轮修掉：`host restart` 之前错把 HCS 0xf 当成功
  （HCS 0x8 = UIC ready 就够了，当时其实重启成功）；DME_POWERON 在这颗
  控制器上返回 1（invalid opcode），Linux 也不发，已删；吞吐行的单位写成
  `KiB/s`，实际打印的是 MiB/s，已改。
- `hs after … pm 0x55 tx 1 rx 1` 是**过时的回读**（还是 ABL 的 SLOWAUTO
  值），不能拿它判断 HS；16 MiB/s 和 176 us 延迟才是证据。
- 新镜像（HS 变默认，`ufsphypwm=1` 保留旧 PWM 路径）：内核 16,833,296 B
  sha256 `b0a98ee5…`；`shim-boot.img` `788e43e4…`，`shim-boot-pwm.img`
  `b265f8fe…`。**这两个新镜像还没有上机**（上机的是 `2d33f4bc` 那版）。
- 现在停在用户态，和 UFS 无关：SpringBoard（pid 26）活着但卡在同一 pc
  （SMP stackshot 反复打印），CommCenter 崩了 428 次（sig 11 / reason 0xb），
  fairplayd.A2、bluetoothd 每 ~10 秒被 launchd 重新拉起；全程没有一条
  `LIUQIN exec failed`。下一步是 GPU/Metal 那条线（见
  `docs/GPU_MILESTONE.md`），不是继续动 UFS。

### 2026-10-23 夜（UFS 时钟层，已编译、未上机）

- 新补丁 `xnu/patches/0092-liuqin-ufs-clock-layer.patch`，加在 0091 之上
  （全系列 91 个补丁）：GCC 时钟层、主机侧时钟寄存器、Linux 的 PHY 上电顺序，
  以及 PA_PWRMODE 的修正。源码是 `bsd/dev/liuqin_ufs.c`，细节和下一次启动该
  看的日志行都在 `docs/BOOT_PERFORMANCE.md` 最后一节。
- 重新解读第三次上机的数字：`uic reset 60` 是 ETIMEDOUT（Darwin errno 60），
  `enable/poweron/link 16` 是 EBUSY（`ufs_uic()` 的 “UIC not ready” 早退，
  HCS bit 3 为 0），都不是 UIC 结果；`pm probe fast=10` 才是 UIC 结果
  = invalid value。**PA_PWRMODE 是两 nibble（RX 7..4 / TX 3..0），FAST/FAST
  应为 0x11，之前写的是 1**，所以“设备拒绝 HS”这个结论要撤回。
- 这一版补上的东西：GCC 的 GDSC/BCR/全部分支（含 `gcc_ufs_0_clkref_en`
  0x9c000 和 tx/rx symbol clock）、RCG 频率（axi/unipro/ice 300 MHz、
  aux 19.2 MHz，寄存器值由日志反解）、`SYS1CLK_1US` (0xC0)、CFG1 的
  `QUNIPRO_SEL` 和 device ref clock (bit 26)、`DME_VS_CORE_CLK_CTRL`
  (0xD002) / `PA_VS_CORE_CLK_40NS_CYCLES` (0x9007)；PHY 改成 `SW_PWRUP`
  先置位、表在后、`SW_RESET` 只清不置、只开 `SERDES_START`。
- 本机编译（macOS / Xcode 15.4）：`kernel.development.liuqin` 16,833,296 B，
  sha256 `2d33f4bc4c576c5fbfd07eb08a2c2f72d5ccf4e6608a93b232678986dfcec964`；
  默认镜像 `shim/out/shim-boot.img` 31,809,536 B `c12bc453…`，HS 实验镜像
  `shim/out/shim-boot-hs.img` `6ce15fed…`（cmdline 只多 `ufsphyhs=1`）。
  **两个镜像都没有上机。**
- 主机侧验证：`tools/verify-patches.sh` 91/91 干净应用（新建的干净树，
  `xnu-8796.141.3`），`tests/test_tools.py` 全过，`validate-repo.py`
  91 个补丁/19 个工具/11 个脚本全过。这些都不是启动结果。
- 下次上机：先跑默认镜像确认 PWM 路径没被破坏；再跑 `shim-boot-hs.img`，
  依次看 `gcc rates …` / `phy post … ready 0x43` / `dme … -> 0` /
  `hs … pwr 0x11 -> 0` / `hs after … pm 0x11`。若 `phy post` 还是 ready 0x4，
  下一步把 block reset 提到 `ufs_take_over_host()` 之前；若 UIC 仍 `EBUSY`，
  看 `gcc rates` 里的 unipro 是否是 300 MHz。

### 2026-10-23 晚（启动速度：UFS 停在 PWM）

- 真机上量了启动：内核只用 3.4 秒，20 分钟几乎全在用户态读 rootfs。
- UFS 逐条延迟：最小 730 us、平均 112 ms、最大 843 ms（1022/1024 > 1 ms），
  有效吞吐 ~0.5 MB/s，正好是 PWM-G1 的量级。
- 链路属性：设备支持 HS-G4，TX/RX 档位能写进去，但 `PA_PWRMODE` 停在
  SLOWAUTO，写 FAST/FASTAUTO 被拒（UIC 返回 10/2）。
- 已排除（都在真机上试过，见 `docs/BOOT_PERFORMANCE.md`）：控制台绘制、
  exec 路径、读预取、直接继承 ABL 的链路、绕过 PHY 的轻唤醒。
- 结论：驱动里的 QMP PHY 只做了"上电+启动"三行寄存器，没有 HS 配置，
  所以 M-PHY 起不来 HS。唯一的修法是移植 Linux
  `phy-qcom-qmp-ufs.c` 里 SM8450 的 SerDes/TX/RX/PCS 表（0091 已移植；
  0092 补上外面的时钟层，见本节最上面的新条目）。
- 测量工具都在 `0090-liuqin-boot-timing.patch`（boot 标记、延迟直方图、
  强制显示的关键行、`liuqinmute` 提前到平台专家、exec 时间戳）。

### 2026-10-23（NativeAdreno 接线，待上机）

- 镜像已打好，**没有上机**：`shim/out/shim-boot.img`（sha256 `693b0227…`），
  boot line 在 `docs/GPU_MILESTONE.md` 的 “Image” 一节，新增参数 `liuqinsvc=1`。
  上机后先看持久 log 里 `NativeAdreno:` 开头的行，再看 `ioreg -l` 有没有
  `NativeAdrenoGpuService`（应带 `native-adreno-state = discovered`）。
- 本轮修了三件“以前一直没生效”的事：`0082` 与 `0042` 重复的 hunk（旧树是在
  重复 hunk 出现之前建的，所以本机一直没发现，重新 `verify-patches.sh` 会停在
  0082）；NativeAdreno 服务向 `this` 而不是 provider 要 device memory；解析
  reg/iommus 时按大端 FDT 读，而本移植的 ADT 是主机序。
- NativeAdreno 的 62 个文件和三个 kit 现在**在仓库里**（`0089` 补丁），不再只
  存在于 `/tmp/liuqin-build2`。`/tmp` 会被系统清理，别再把唯一副本留在那儿。
- 构建目录是 `/tmp/liuqin-build2`（`WORK=/tmp/liuqin-build2 tools/build-local.sh`，
  增量约 1 分钟）；`SEED_XNU` 可以复用已经打好的树。
- **2026-10-23 第一轮上机结果**：服务全起来了（`nubs ... = 1 1 1`、三个服务
  attach/start/register、GPU 节点已发布），系统照旧走到 `exec launchd`。我自己的
  `parseReg` 把 ADT 的两 cell 值拼反了（低字在前，被当成高字），所以 GMU 绑定
  失败；映射不受影响（走 XNU 自己解析的 `reg`）。已修，日志细节在
  `docs/GPU_MILESTONE.md` 的 “First on-device round”。
- 下一次上机的顺序：先确认多出 `GMU five-window I/O adapter bound` 那行，再让
  服务接管 `liuqinadreno=N` 阶梯已经建立的状态，最后才是 Mesa/Turnip 用户态。

- 本仓库里**没有任何**来自真机的 dump；`dtb/`、`backup/`、`research/` 都在
  `.gitignore` 里，保持这样（含序列号与校准数据）。
- `shim/out/`、`xnu/out/` 是构建产物，不入 git；本机内核的构建目录是
  `/tmp/liuqin-build`（`tools/build-local.sh`，默认 `WORK`）。
- 内核、payload、boot.img 都已就绪：boot.img 里嵌的是 vendor_boot 的
  14 个基础 DTB（cape 优先），dtb 加载地址 0x3000000（避开 17 MB 的 payload
  ramdisk）。内存 bank 要等第一次上机看 shim 的打印（见 §3、§7）。
- 不要改 `boot`/`recovery`/`modem`/`persist` 分区；只覆盖 `userdata`。
- 改补丁后先跑 `tools/verify-patches.sh`，再花 CI 时间。

- **2026-10-27 触摸第 188 步：/tmp 被反复清空 → 构建树搬到 `~/liuqin-build3`；加"强制保活"开关（touch142）**：
  15:55 与 16:17 两次 `/tmp` 清理把构建树还原成"仓库旧补丁序列"，丢掉了当天的全部改动。
  现在树在 **`~/liuqin-build3`**（`WORK=~/liuqin-build3 tools/build-local.sh`，链接器
  `LLD=~/liuqin-build3/ld.lld`，即 wip 里那份 zig 包装脚本），makedefs 里的 fakeroot 路径
  已从 `/tmp/...` 改指 `~/liuqin-build3/fakeroot`，快照仍在 `~/liuqin-root/wip/2026-10-27/`。
  touch142 新增启动参数 **`liuqinkeepalive=<ms>`**：
  - 0 = 旧行为（静默 5 s 才 INT poke，20 s 才重下）；
  - >0 = 恢复线程每 `ms` 主动拉低 INT 200 us（只在 INT 当前为低时做，避免和面板驱动争线），
    从根上不让它进入低功耗态。
  代价：触摸控制器不再省电（多几毫安），INT 线被周期性驱动；对 iOS/显示/CPU 没有影响。
  产物：内核 `d71e8fe3…`、`shim/out/shim-boot-touch142-keepalive.img`（sha256 `f8794522…`，
  启动行带 `liuqinkeepalive=2000`）。

- **2026-10-27 触摸第 189 步：touch142 的看门狗一次都没救——因为 revive 分支写在 `if (running) continue;` 后面（touch143）**：
  touch142 日志（sha256 `613f0c0f…`）：
  - keep-alive 生效（`keep-alive: poking INT every 2000 ms`），触摸能用（15 → 36 → 51 个触点帧）；
  - 之后固件再次停摆，`three interrupts with 0xFD …` 反复打印 **几十次**（说明报告线程一直在发现挂死），
    但 **一条 `revive` 都没有**。
  原因：`liuqin_touch_recover_thread()` 里 `if (running) { … continue; }` 排在两个 revive 分支**之前**，
  而这颗芯片停摆后状态寄存器仍然读 `0xa3`（running），所以每轮都从这里 continue 掉，恢复逻辑永远够不到。
  touch143 把顺序改成：帧计数 → `tch_fw_dead`（中断无帧）→ 静默 poke → 静默 4 轮重下 → 再判 running。
  另外 `fw verify` 仍然一直报 51 个块不符：`tch_nvt_read()` 的缓冲虽然提到 320，
  但位敲的 `tch_xfer()` 上限是 256 B，而校验按 256 B 数据 + 2 B 开销读 → 每次仍失败（51 又是块数）。
  已把校验块降到 **128 B**。
  产物：内核 `587f32d3…`、`shim/out/shim-boot-touch143-revivefix.img`（sha256 `72ec8bdb…`，带 `liuqinkeepalive=2000`）。

- **2026-10-27 触摸第 191 步（交接）：触摸本体可用但会停摆；下一轮按 `docs/DMA_HANDOFF.md` 死磕 DMA**：
  - 今天的最终结论：**镜像已证明完整**（`fw verify: image identical to the file (201410 bytes)`），
    位敲在正确 pace 下能出帧、能派发（135/139/142 都出现过 `event 0..300 dispatch ok`）；
  - 仍未解决的是**固件会在一段时间后停摆**（state 仍 0xa3、INT 消失、缓冲静止），
    "静默重下"不能当判据（143 因此变成 0 触摸），只有"中断无帧"是可靠证据；
  - 用户决定：**下一轮专攻 DMA**。所有相关事实（SE4→apps_smmu stream 0x5A3、S2CR 偏移 0x1500968C、
    四个窗口的可读值、低 4 GiB 的两个空闲窗、touch129 的死点、以及"先读 SMMU 再碰 DMA 页"的顺序）
    都整理进了 **`docs/DMA_HANDOFF.md`**，包含具体改动顺序和不要踩的坑；
  - 构建树在 `~/liuqin-build3`（`/tmp` 今天被清空两次，丢过全部改动），快照在
    `~/liuqin-root/wip/2026-10-27/`；最近一版镜像 `shim-boot-touch144-quiet30.img`（`5fee6ef2…`）。

- **2026-10-27 触摸第 190 步：镜像终于被证明是好的；但"静默 8 秒就重下"把 143 拖成了 0 触摸（touch144 放宽到 30 s）**：
  touch143 日志（sha256 `3f7aa6f2…`）关键两行：
  ```
  LIUQIN TOUCH: fw verify: image identical to the file (201410 bytes)   ← 镜像完全正确
  LIUQIN BOOT: touch fw verify ok at 61.818 s (+148 ms)
  ```
  说明**下载/校验这条线彻底闭环**（128 B 块 + `NVT_BUF_MAX 320` 之后才真正读到数据）。
  但这一轮 **0 个触点帧**，而且 `revive` 从 1 喷到 10：
  恢复线程把"8 秒没有触点帧"当成死机（`tch_quiet_checks >= 4` × 2 s 周期），
  于是每 12 秒就把面板复位重下一次，任何触摸都落在下载窗口里——比不救更糟。
  结论：**静默本身不能当死机判据**（空闲与死机在总线上没有区别），证据必须是"中断来了却没帧"。
  touch144：
  - 静默 5 s 只 poke INT（保留）；
  - 静默重下从 8 s 放宽到 **30 s**（且最多 10 次）；
  - "中断无帧"三次仍然**立即**重下（这条是证据驱动的，142 里它确实反复命中）。
  产物：内核 `de089ca0…`、`shim/out/shim-boot-touch144-quiet30.img`（sha256 `5fee6ef2…`，带 `liuqinkeepalive=2000`）。

- **2026-10-27 触摸第 192 步（主机侧）：按 `docs/DMA_HANDOFF.md` 的顺序改完并出了镜像，但没有上机**：
  这台 mac 上没有 adb/fastboot，USB 上也没有设备，所以"第一条 DMA 之后冻机"还在不在**不知道**。
  - 代码（`bsd/dev/liuqin_touch.c`，对应文档第 1–4 步）：
    - `tch_spi_setup()` 的 `liuqintouchdma=1` 分支顺序改成
      **`tch_smmu_fix()` → `tch_dma_alloc()` → `tch_se_start()`**（原来是 `tch_dma_alloc()` 在前，
      也就是 touch132 冻机那次的顺序）。
    - `tch_smmu_fix()` 从"整个块一块 64 KiB 映射"改成**两个 16 KiB 窗口**：page 0 读
      `SCR0/IDR0`，0x9000 那页读 `S2CR[0x5a3]`（0x1500968c，页内 +0x68c）。**推断**：
      文档写的"16 KiB 映射"如果照字面只映 0x15000000 起的 16 KiB，够不到 0x968c，
      所以按实际要碰的偏移拆成两页；写 bypass 后立即回读并打印。
    - 第一条 DMA 之前重新读一次 `S2CR[0x5a3]` 并打印（`SMMU S2CR[0x5a3] before DMA #1 is …`），
      同时打印 `dma_mode_en` / `general_cfg` / `status`；DMA 失败行补上
      `dma_mode_en` 与 `general_cfg`。
    - 文档第 4 步的备用窗**不用改代码**：`liuqintouchdmaphys=0x85e00000` 本来就支持；
      另加 `liuqintouchloopback=N`（默认 0），把 N 原样写进 `SE_SPI_LOOPBACK`(0x22c)，
      用来做"回环能过、对面板不能过"的区分。
  - 构建（macOS / Xcode 15.4，`WORK=~/liuqin-build3 tools/build-local.sh`）通过：内核
    17,019,552 B，sha256 `e54d22d7…`。26 条警告全是这个文件既有的
    `-Wdeclaration-after-statement` / `-Wmissing-prototypes` 一类，没有新增诊断。
  - 镜像（都是 31,997,952 B，payload 25,423,056 B）：固件区直接从 touch144 的镜像里取出
    （9 个文件的 sha256 全对，含 `novatek_nt36532_m81_fw_csot.bin`），所以 A/B 只差内核和启动参数：
    - `shim/out/shim-boot-touch145-dma-smmu.img`，sha256 `ff386b32…`（默认窗 0x85200000）；
    - `shim/out/shim-boot-touch145-dma-altpage.img`，sha256 `582d9e3e…`
      （`liuqintouchdmaphys=0x85e00000`）；
    - `shim/out/shim-boot.img` 与第一张相同。
  - 启动行 = touch144 那条**去掉 `liuqinmarks=0`**（`liuqinmute=1` 时 marks 默认就是关的，
    已核对 `liuqin_ufs.c` 的默认分支）+ `liuqintouchdma=1 liuqintouchsmmu=2`；
    altpage 再加 `liuqintouchdmaphys=0x85e00000`。长度 567 / 597 B（上限 607）。
  - 主机侧验证：`tools/verify-patches.sh` 219/219 干净；`tests/test_tools.py` 全过；
    内核二进制里能找到新字符串；两张镜像里的 cmdline 与固件区都逐项核对过。**这些都不是启动结果。**
  - 上机就按文档第 4/5 步走：
    1. 启动 `shim-boot-touch145-dma-smmu.img`，抓 log，依次看
       `SMMU S2CR[0x5a3] @0x1500968c … type ?`、`liuqintouchdma=1`、`DMA #1 config`、
       `DMA #1 issuing opcode`。
    2. 若 `type 1` 且随后有 `DMA #1 came back … tx …`（不再冻机）→ 走第 5 步，把固件下载切到 DMA
       （`tch_xfer()` 上限已是 0x3f00，`tch_fw_write_sram` 的 256 B 调用会自动落到 DMA）。
    3. 若仍冻在 `DMA #1 issuing` → 直接换 `shim-boot-touch145-dma-altpage.img`（不用重编），
       看 0x85E00000 窗。
    4. 若两个窗都冻 → 启动行加 `liuqintouchloopback=1` 再试，区分"存储/DMA 路径"与"SPI 从机/面板"。
  - **仓库状态（重要）**：动手前 `xnu/patches/` 系列已经落后构建树——`liuqin_touch.c` 差
    1674 行 / 51 个 hunk（touch129–144 的改动只在 `~/liuqin-build3` 里）。当时
    `verify-patches.sh` 照样能过，因为它只验证"能干净应用"，不验证"能复现当天内核"。
    本轮补了两个补丁，再跑 219/219 干净，且应用后的 `bsd/dev/liuqin_touch.c` 与
    `~/liuqin-build3` **逐字节相同**：
    - `0219-liuqin-touch-record-build-tree-work.patch`（51 hunk，把那份工作记进系列）；
    - `0220-liuqin-touch-dma-smmu-order.patch`（9 hunk，本轮的 DMA 改动）。
    **其余仍有 55 个文件与构建树不同**（`bsd/conf/files`、`liuqin_input.c`、`liuqin_ufs.c`、
    `kern_exec.c`、`kern_exit.c`、`liuqin_amfi.c`、构建树里整套 `iokit/.../hid` 的 IOHID 等），
    所以**从仓库干净重建仍然复现不出当天内核**；这是下一件该补的事，本轮没做。
  - `tools/mkpayload.py`：cmdline 超过 607 B 时现在会告警并打印被丢掉的尾部。这轮踩到过：
    第一版 altpage 镜像的 `liuqintouchdmaphys` 被悄悄截成 `0x85e0`。

- **2026-10-27 触摸第 193 步（上机 + 修正）：`S2CR = 0x8000 + 4*sid` 被硬件证伪；第 192 步的镜像会 panic，改用 touch146**：
  - 上机用的是第 192 步的 `shim-boot-touch145-dma-smmu.img`（`ff386b32…`），panic：
    ```
    LIUQIN TOUCH: liuqintouchdma=1, trying SE DMA again
    Kernel data abort … esr 0x96000010
    ```
    寄存器（完整文本在屏上）：`pc 0xfffffe00075141a8`、`far 0xffffffe7a8a71d68c`、
    `x8 0x00200406`（SCR0）、`x22 0x4c017e71`（IDR0）、`x19 0xffffffe7a8a71d000`（SMMU 窗口 VA）。
    反汇编对得上：`0x…7514198 ldr w8,[x20]`（SCR0，成功）→ `0x…75141a0 ldr w22,[x20,#32]`
    （IDR0，成功）→ **`0x…75141a8 ldr w20,[x19,#1676]`**（= 窗口 + 0x68c，外部中止）。
    所以映射和 SCR0/IDR0 都没问题，**是 0x1500968c 这个地址不存在**——0x8000+ 是
    global space 1，这颗片上读它就 trap；`GpuPowerBoard.h` 里早有同一句结论
    （"the apps_smmu abort happened on the first gr1 read"）。另外 S2CR 本来就是按
    **stream-match 条目**编号，不是按 stream id，所以"改 0x1500968C"这条路线本身不成立。
    顺带：这次日志里**没有** `DMA: mapping phys 0x85200000` 那一行，说明第 192 步改的
    "先 SMMU、后 DMA 页"顺序确实生效了（旧顺序会先打印 DMA 页映射）。
  - 修正（`liuqin_touch.c`，取代第 192 步的实现）：`tch_smmu_fix()` 只碰 global space 0，
    映射 **一个** 16 KiB 窗口，扫 `SMR[0..127]`@0x800 / `S2CR[0..127]`@0xc00，打印至多 16 条非零项，
    再按 arm-smmu 语义（VALID、id/mask）找匹配 stream 0x5a3 的 SMR；找到就把那条 S2CR 的
    TYPE 改成 bypass（只改 [1:0]，保留 CBNDX 等）。表里没有匹配项就**原样不动**并打印出来，
    不再往任何 0x8000+ 地址写。`S2CR.TYPE` 仍是 0=translate / 1=bypass / 2=fault。
  - 产物（启动行与第 192 步相同：`liuqintouchdma=1 liuqintouchsmmu=2`）：
    内核 17,019,552 B，sha256 `163ebb4b…`；
    `shim/out/shim-boot-touch146-dma-gr0.img`（sha256 `2505a2c3…`，默认窗 0x85200000）、
    `shim/out/shim-boot-touch146-dma-altpage.img`（sha256 `4147033a…`，0x85E00000）；
    `shim/out/shim-boot.img` = gr0 那张。**touch145 的两张作废**（SMMU 那一步必 panic）。
  - 本轮**只证明了 SMMU 的读法**，DMA 本体还没跑过：touch145 在 `tch_smmu_fix()` 里就死了，
    第 192 步改的"先 SMMU 后 DMA 页"顺序还没被执行到。下次启动 touch146 时才会第一次走到
    `tch_dma_alloc()` → `tch_se_start()`，判据仍是 `DMA #1 issuing` 之后有没有 `DMA #1 came back`。
  - `xnu/patches/0220-liuqin-touch-dma-smmu-order.patch` 已按修正后的实现重生成（8 个 hunk，
    反向校验与构建树一致）；`0219` 不变。
  - **镜像辨识（一次踩坑）**：有人刷了旧的 `shim-boot-touch145-dma-altpage.img`，日志与第 192 步
    touch145 那次一字不差（`pc 0x…75141a8`、`lr 0x…7514180`、`x22 = 0x4c017e71`、
    `x19/x20` 是两个 SMMU 窗口 VA）——那是旧内核的双窗口实现，`lr` 正是第二次
    `ml_io_map` 的返回地址。新内核（touch146）在同一个地址是 `str x8, [sp]`（栈写，
    不可能产生设备地址的 FAR），而且**打 SMR/S2CR 之前一定会先打印**
    `LIUQIN TOUCH: SMMU gr0 scr0 0x… idr0 0x… (NUMSMRG field 0x…)`。
    所以：日志里没有这行 = 跑的不是 touch146；刷机前先核对
    `shasum -a 256`（gr0 `2505a2c3…`、altpage `4147033a…`）。

- **2026-10-27 触摸第 194 步（上机 touch146-gr0）：SMMU 表第一次读出来；条目只有 113 个，越界又 panic（touch147）**：
  - touch146-gr0（`2505a2c3…`）真的跑到了新代码，屏上先打印：
    ```
    LIUQIN TOUCH: SMMU gr0 scr0 0x00200406 idr0 0x4c017e71 (NUMSMRG field 0x71), SMR@0x800 S2CR@0xc00
    LIUQIN TOUCH: SMMU [0] … [15] …
    ```
    然后 panic。寄存器：`pc 0xfffffe0007514270`（`ldr w20,[x8]`，即 SMR 的读）、
    `esr 0x96000010`、`far 0xffffffe4f910f89c4`、`x28 0xffffffe4f910f8dc4`、`lr 0x75142d0`。
    `x8 = x28 - 0x400`，所以窗口基址是 `0xffffffe4f9108000`，出问题的偏移是
    `0x800 + 4*113 = 0x9c4`——**读第 113 项**。而 `IDR0[7:0] = 0x71 = 113` 正是条目数，
    也就是说数组是 **0…112**，读 113（越界）就外部中止。上一轮用 128 作上界，踩中了这个。
  - 同一次日志还证明了两件事（都跟上一轮写的相反/补充）：
    - **S2CR 的字段不是 [1:0]/[15:8]**：ARM SMMU-500/QSMMU 是 `TYPE=[17:16]`、
      `CBNDX=[7:0]`（Linux `arm-smmu.h`，以及 LKML 上给 QSMMU 补的
      `ARM_SMMU_S2CR_TYPE`/`CBNDX` 定义）。按正确字段读，`S2CR[i] = i` 的意思是
      **CBNDX=i、type=translate**，不是“type=i”。
    - 表是真的：`SMR[0] = 0x80000000`（valid、id 0）、`SMR[3] = 0x84022800`
      （id 0x2800、mask 0x402）——正好是 **显示** 的那条流：DT 里
      `qcom,smmu_sde_unsec_cb` 是 `iommus = <0x5b 0x2800 0x402>`，也就是
      `qcom,handoff-smrs = <0x2800 0x402>`。前 16 项里**没有**匹配 0x5A3 的 SMR。
  - touch147 的改法（`tch_smmu_fix()`）：
    1. `nsmr = IDR0[7:0]`，扫描严格 `i < nsmr`（不再用 128 硬上界）；
    2. 用正确的 TYPE/CBNDX 字段解码，打印所有 id≠0 的 valid SMR（至多 12 条）和计数；
    3. 分流：有匹配 0x5A3 的 SMR → 只改它的 `S2CR TYPE=bypass`；没有 → 取一条
       **all-zero**（SMR/S2CR 都是 0）的条目，写 `S2CR=bypass` 再写
       `SMR=VALID|0x5A3`；再没有 → 复用一条“valid/id 0/mask 0”的冗余条目
       （只匹配 stream 0，最低那条保留，路由不变）。写入后都回读打印。
  - 产物：内核 17,019,552 B sha256 `65d94d9a…`；
    `shim-boot-touch147-smmu2.img`（`ba35b070…`，表 + 上面的 1/2/3）、
    `shim-boot-touch147-smmu0.img`（`9275fda8…`，**对照**：`liuqintouchsmmu=0` 完全跳过 SMMU）；
    `shim/out/shim-boot.img` = smmu2。**touch146 两张也没用了**（扫描越界必 panic）。
  - 下一次上机先 smmu2：看它选了哪条路（matches / programming / 表原样不动）和回读值，
    再看 `DMA #1 issuing` 之后有没有 `DMA #1 came back`。若还冻，用 smmu0 对照：
    它也冻 → 不是 SMMU 的锅（查 SE DMA 描述符/时钟）；它不冻 → 就是流匹配的问题。

- **2026-10-27 触摸第 195 步：touch147 两种 SMMU 模式都硬挂重启（无 panic）；下一轮查 QUP 的 AHB 主时钟与 DMA 页（touch148）**：
  - 现场：`shim-boot-touch147-smmu2.img`（`ba35b070…`）和 `shim-boot-touch147-smmu0.img`
    （`9275fda8…`）都**没有 panic，直接卡死后重启**（用户观察）。**没有取到当次日志尾部**，
    所以"死在哪一行"还不知道。
  - 先纠正我上一条的推断：`liuqintouchsmmu=0` 只是**不碰** SMMU 寄存器，SE 的 DMA 仍然带
    SID 0x5A3 过 SMMU，所以它挂**不能**证明"SMMU 无关"；它只证明"挂点在我们自己的 SMMU 探测
    之后"（那次根本没执行 SMMU 代码）。
  - 由此把怀疑对象换成两个，都能解释"无 panic 的硬挂"：
    1. **QUP wrapper 的 AHB 主时钟没开**。位敲只用 TLMM + SE 从端口，寄存器读写全都正常，
       但 DMA 要 QUP 的 AHB master 去读写内存；m_ahb/s_ahb/core/core_2x 是
       `GCC_QUPV3_WRAP0_CBCR`(0x62008) 的 bit 6/7/8/9（clk-qcom 里各是一个
       BRANCH_HALT_VOTED 分支），驱动从来没开过，只开过 S4。主端口被 gate 时，第一条
       DMA 事务不完成、互联挂死、无 panic——和现象一致。
    2. **DMA 页 0x85200000 未必是真实内存**。它是按 vendor DTB 的 reserved-memory 空窗推出来的；
       本移植的 ADT 里内核管理的 DRAM 是从 0x100000000 开始的，低 4 GiB 那块是固件/显示
       切走的，空窗只是"两个 carveout 之间"，不等于被 DRAM 覆盖。若 PA 没有内存响应，
       QUP 的第一条事务同样会挂死。
  - touch148 的改动（都在 `bsd/dev/liuqin_touch.c`）：
    - 新增 `tch_qup_dma_clocks()`：进 DMA 分支先读 `0x62008` 并打印
      `[m_ahb s_ahb core core2x s4]`，缺的位补上再回读打印（`liuqintouchqupclk=0` 可关）。
    - `tch_dma_alloc()` 里加**页面自检**：向 0 和 0x4000 各写一个 pattern、`dsb`、读回，
      打印 `DMA page self-test ok/MISMATCH`。PA 不是内存的话这一步就会暴露（或直接挂在这里，
      最后一行就是自检前的那行）。
    - 两张镜像：`shim-boot-touch148-smmu2.img`（`218f8008…`，SMMU 照旧写 bypass/补条目）和
      `shim-boot-touch148-smmu1.img`（`28ca5eeb…`，**只读 SMMU**，保证一定能走到时钟/自检/DMA，
      用来排除"SMMU 写入本身把总线写死"这个可能）；内核 `4414408f…`。
  - 判读顺序：`QUP CBCR …`（哪些位原本就是 0）→ `forced … -> …`（回读是否生效）→
    `DMA page self-test` → `DMA #1 issuing`。若 CBCR 本来就是全 1，时钟假设直接排除；
    若 forced 后回读没变，说明这些位不是普通 enable（或 hypervisor 不认），也要换路。

- **2026-10-27 触摸第 196 步：touch147-smmu2 的日志拿到了——SMMU 扫全了、表没写成（判据写错），DMA #1 一发就挂（touch149）**：
  - 当次日志（`touch147-smmu2`，没有 transfer 回来）关键部分：
    ```
    SMMU gr0 scr0 0x00200406 idr0 0x4c017e71, 113 entries
    SMMU [  0] SMR 0x800000e0 id 0xe0 mask 0x0,  S2CR 0x00000000 type 0 cb 0
    SMMU [  1] SMR 0x800004a0 id 0x4a0 mask 0x0, S2CR 0x00000001 type 0 cb 1
    SMMU [  2] SMR 0x800000c0 id 0xc0 mask 0x0,  S2CR 0x00000002 type 0 cb 2
    SMMU [  3] SMR 0x84022800 id 0x2800 mask 0x402, S2CR 0x00000003 type 0 cb 3   ← 显示
    SMMU [  5] SMR 0x80110584 id 0x584 mask 0x11,    S2CR 0x00000005 type 0 cb 5
    SMMU 113 entries: 6 valid (1 match only stream 0), 0 all-zero
    SMMU no valid SMR matches stream 0x5a3
    SMMU no entry to program; leaving the table alone
    DMA page fixed at phys 0x85200000 / DMA staging page at 0x…… (32 KiB)
    CS assert as configured: after-cmd 0x41 irq 0x1
    DMA #1 tx+rx len 1 tx 0x85200000 rx 0x85204000 mode 0x00000001
    DMA #1 issuing opcode 1 (the transfer starts here)        ← 到这里硬挂，之后重启
    ```
  - 事实：**有界扫描成功了**（113 项读完，不再 panic）；表里确实**没有**匹配 0x5A3 的 SMR；
    但我的“空槽”判据要求 `SMR==0 && S2CR==0`，而这张表的无效条目 S2CR 都是 `i`（非 0），
    于是 107 个可用槽全被拒掉，**根本没用上 bypass**，DMA 还是带着未匹配的 0x5A3 发出去。
  - 因此上一轮“smmu0 也冻”依旧不能说明 SMMU 无关——**bypass 从未真正生效过**。
    挂点精确落在 `DMA #1 issuing` 之后、没有 `came back`，而且 DMA 页的映射/清零都正常打印过
    （CPU 对 0x85200000 的写没有挂），所以下一个该排除的就是“流未被 bypass”和“AHB 主时钟”。
  - touch149 的改动：
    1. **只要 SMR 的 VALID 位是 0 就算空槽**（S2CR 是不是 0 无所谓——SMR 无效时那条 S2CR
       本来就没被任何流使用），用它写 `S2CR=bypass`、再写 `SMR=VALID|0x5A3`，回读打印。
       这张表有 107 个这样的槽，肯定写得成（除非 hypervisor 拒绝）。
    2. 保留 touch148 的 `tch_qup_dma_clocks()`（`QUP CBCR` 打印 + 补 m_ahb/s_ahb/core/core_2x）
       和 `DMA page self-test`。所以这次日志会同时回答三个问题：时钟位、页是否真内存、bypass 回读。
  - 产物：内核 `061829e0…`；`shim-boot-touch149-smmu2.img`（`bbed2f69…`，会写 bypass）、
    `shim-boot-touch149-smmu1.img`（`bc6bd7ac…`，只读 SMMU，用来分离“时钟修好了”还是“bypass 修好了”）；
    `shim/out/shim-boot.img` = smmu2。**touch147/148 都作废**（148 的槽判据同样是错的）。

- **2026-10-27 触摸第 197 步：时钟和 DMA 页都排除；SMMU 的 BYPASS 被硬件写成 FAULT（qsmmu 的已知怪癖）→ 改用"保留 context bank 模拟 bypass"（touch150）**：
  - touch149-smmu2 的日志（能读到，说明这次也是硬挂重启）：
    ```
    QUP CBCR 0x4002cbc0 [m_ahb 1 s_ahb 1 core 1 core2x 1 s4 1]    ← 四个 wrapper 时钟本来就是开的
    SMMU 113 entries: 6 valid (1 match only stream 0), 107 free
    SMMU programming free entry 6 (SMR invalid)
    SMMU writing S2CR[6] = 0x00010000, SMR[6] = 0x800005a3 (bypass for stream 0x5a3)
    SMMU readback SMR 0x800005a3 S2CR 0x00020000                   ← BYPASS 变 FAULT
    DMA page self-test ok (0xa5a5a5a5 0x5a5a5a5a)                  ← 0x85200000 是真内存
    DMA #1 issuing opcode 7                                       ← 仍然硬挂
    ```
  - 结论（都有出处）：
    - **时钟排除**：`0x4002cbc0` 的 bit6/7/8/9/14 全为 1，驱动没有写任何位（没有 `forced` 行）。
    - **DMA 页排除**：pattern 回读一致。
    - **BYPASS 不可用**：写 `S2CR.TYPE=1` 回读成 `2`（FAULT）。这不是我算错——Linux
      `arm-smmu-qcom.c` 的 `qcom_smmu_cfg_probe()` 专门检测这一点：
      "writing BYPASS will end up written as FAULT"，然后保留最后一个 context bank，
      把 `CBAR` 设成 `CBAR_TYPE_S1_TRANS_S2_BYPASS`、`SCTLR=0`，再把要 bypass 的流用
      **TYPE=TRANS + 那个 CB** 指过去（`qcom_smmu_write_s2cr()`）。也就是说这颗 SMMU
      **根本不能真 bypass**，只能"借一个翻译被关掉的 CB"。
    - 于是 touch149 把流指到 FAULT，第一次 DMA 事务必然被 SMMU 拒绝，QUP 就挂死——和现象一致。
  - touch150 的实现（`tch_smmu_bypass_cb()`）：
    1. 从 IDR1 读几何（`PAGESIZE` bit31、`NUMPAGENDXB[30:28]`、`NUMCB[7:0]`），
       `pgshift = 16 或 12`、`numpage = 1 << (NUMPAGENDXB+1)`、
       `CBAR(n) = (1<<pgshift) + 4n`、`CB(n) = (numpage+n) << pgshift`；
       选 `CB = NUMCB-1`（DT 的 78 → 77），并检查不超出 1 MiB 寄存器块；
    2. 写 `CBAR = 0x00010000`（S1_TRANS_S2_BYPASS）、`CB.SCTLR = 0`，前/后都回读打印；
    3. 再写 `S2CR[slot] = CB`（TYPE=TRANS）、`SMR[slot] = VALID|0x5A3`，回读打印。
    `liuqintouchbypass=0` 可以关掉这一段。
  - 产物：内核 `af3ca3e7…`；`shim-boot-touch150-bypasscb.img`（`934e5b7e…`）；
    `shim/out/shim-boot.img` = 这张。**touch149 作废**。
  - 风险：CB 寄存器（CBAR/CB.SCTLR）是 gr1 里的**已实现**寄存器，理论上可访问；
    但如果 hypervisor 把 gr1 也拦了，会在 `CBAR … (before)` 那一行之前/处 panic——
    那也说明这条路只能换 GPI DMA。
- **2026-10-27 触摸第 198 步：SMMU 通了（不再 panic、bypass CB 写入生效），DMA 的锅落在 SE 引擎侧；补上 QUPv3 的 SE 控制块（touch151）**：
  - touch150 的日志（**这次没有 panic**，1000 次轮询后正常返回）：
    ```
    SMMU quirk: idr1 0x6000004b (pgshift 12, numpage 128, numcb 75) -> bypass CB 74, CBAR at +0x1128, CB at +0xca000
    SMMU CBAR 0x00000000 SCTLR 0x0080270e (before)
    SMMU CBAR 0x00010000 SCTLR 0x00000000 (after)          ← gr1 可访问，写入生效
    SMMU readback SMR 0x800005a3 S2CR 0x0000004a           ← TYPE=TRANS / CBNDX=74
    DMA #1 came back: status 0x00000041 irq 0x00000000 tx 0x00000000 rx 0x00000000 after 1000 polls
    ```
  - 三个新事实：
    1. **SMMU 退休**：touch149（TYPE 被硬件改成 FAULT）是硬挂，touch150（借 CB 74）是
       "等满 1000 次、零中断、零 DMA 状态位"——既没有 DMA_DONE 也没有 AHB 错误。
       流已经不再被拒，SMMU 从嫌疑名单里出去。
    2. `status 0x41` 与一次成功的 `CS_ASSERT` 之后的稳态一模一样（`M_GENI_CMD_ACTIVE|bit6`）：
       **序列器把数据命令一直握在手里**，五次恢复尝试（cancel/abort/clear-disable/start-trigger）
       数字完全相同，之后 SE 拒绝一切命令。
    3. **CB 号以 IDR1 为准**（`NUMCB=0x4b=75` → CB 74），不是 DT 的 `num-context-banks-override=0x4e`。
  - 对照上游源码后，前两轮怀疑的点都不是问题：
    - `M_CMD0` 里**没有 M_CMD_DMA 这一位**（`spi-geni-qcom.c` 只有 `FRAGMENTATION = BIT(2)` 等五个参数），
      DMA 与否只由 `SE_GENI_DMA_MODE_EN` 决定；我们的 `(opcode<<27)|FRAGMENTATION` 与上游等价。
    - 描述符写法（`IRQ_EN_SET` → `PTR_L/H` → `ATTR`=1/0 → `LEN`）与 `geni_se_tx/rx_init_dma()` 逐条一致。
  - 真正的缺口：**QUPv3 的 SE 控制块（0x2000 段）从来没读过**。`geni_load_se_fw()` 在设置传输模式时写
    `SE_DMA_IF_EN@0x2004 |= DMA_IF_EN`（"Serial engine DMA interface is enabled"）和
    `SE_FIFO_IF_DISABLE@0x2008`；本机 `GENI_IF_DISABLE_RO@0x64` 读 **1**（FIFO 接口关），
    bootloader 留下的 `SE_GSI_EVENT_EN@0xe18 = 0xf` 也说明这颗 SE 是按 **DMA/GSI** 装载的。
    FIFO 关、DMA 接口没开 → 数据命令取不到 TX、存不下 RX，正是"永远 active、零中断"。
    另一个与上游的差别是顺序：2024 年重构后上游是**先 `M_CMD0`、再写描述符**，我们一直反过来。
  - touch151（`xnu/patches/0221-liuqin-touch-dma-interface.patch`，13 hunk）：
    1. `tch_se_dma_if()`：打印 `clk_ctrl/dma_if_en/ro/fifo_if_disable/gsi_ev`，
       `DMA_IF_EN` 为 0 就置 1 并回读；`liuqintouchdmaif=0` 只观察。
    2. `liuqintouchdmaorder=1` + 探测链新增 `cmd-first`（先命令后描述符）。
    3. 失败取证：`SE_DMA_DEBUG_REG0`（TX/RX active）、`SE_DMA_TX/RX_LEN_IN`（实际搬了多少字节）、
       `SE_DMA_TX/RX_IRQ_EN`、TX/RX 暂存区前 4 字节（RX 命令前清零，变一个字节就证明 DMA 跑过）、
       bypass CB 的 `FSR/FAR/FSYNR0`（编程 CB 时先清陈旧 fault）。
  - 产物：内核 `bcd67a63…`、payload `a5b31e4c…`、`shim-boot-touch151-dmaif.img`（`e338028c…`，
    `shim/out/shim-boot.img` = 这张）；启动行与 touch150 相同（567 B，结尾
    `liuqintouchdma=1 liuqintouchsmmu=2`）。主机侧：编译通过、220/220 补丁干净、0221 逆向校验、
    `cmp` 与构建树逐字节一致、`validate-repo` 与 `tests/test_tools.py` 全过。
  - 判据写在 `docs/DMA_HANDOFF.md` 末尾的表里；最关键的两条：
    `dma_if_en 0x0 -> 0x1` 之后就成了 → 直接上固件下载；
    本来就 `0x1` 且 `len_in tx 0 rx 0` → 说明 SE 的原语表是按 GSI 装的，下一条路是 GPI DMA。

- **2026-10-27 触摸第 199 步：touch151 实机崩在 SE+0x2004（QUP v3.0 没有 0x2000 段）→ `DMA_IF_EN` 路线作废；重读上游后真正缺的是"退出 GSI 事件模式"（touch152）**：
  - touch151-dmaif 的日志（照片，`LIUQIN SCREEN LOG` / `LIUQIN LOG TAIL`）：
    ```
    QUP hw 0x30000000 (v3.0) TX fifo 16 words x 32 bit, RX fifo 16 words
    ...
    liuqintouchdma=1, trying SE DMA again
    QUP CBCR 0x4002cbc0 [m_ahb 1 s_ahb 1 core 1 core2x 1 s4 1]
    Kernel data abort. at pc 0xfffffe00075142ac, lr 0xfffffe00075142a8
    esr: 0x96000010   far: 0xfffffe3a2cd36004
    ```
    注意这轮**没有** `SE4 clk_ctrl …` 那行：`tch_se_dma_if()` 一次都没打印，说明崩在它的读里。
  - 符号表 + 反汇编定位（内核链接基址 `0xfffffe0007000000`，与 panic 的 pc 同段）：
    ```
    fffffe00075142a8: ldr x8, [x20, #1456]   ; x8 = tch.se
    fffffe00075142ac: ldr w21, [x8, #8196]   ; 0x2004 —— 崩在这条
    ```
    `esr 0x96000010` = synchronous external abort（DFSC=0x10），不是 translation fault：
    VA 映射成功，是总线目标不回话；`far - x8 = 0x2004`。
  - 结论：**这颗 QUP 是 v3.0，没有 0x2000 那一段**。`SE_GENI_CLK_CTRL/SE_DMA_IF_EN/
    SE_FIFO_IF_DISABLE` 属于后来的 "loadable SE"（上游 `geni_load_se_firmware()` 只在
    `proto == GENI_SE_INVALID_PROTO` 时用），所以 touch151 的 `DMA_IF_EN` 理论在硬件上
    无从验证，**直接作废**；能读的只有头文件里明确标 RO 的 `DMA_IF_EN_RO@0xe20`。
  - 重读上游 `qcom-geni-se.c`/`spi-geni-qcom.c` 后，真正漏掉的是**模式切换**：
    - `geni_configure_xfer_mode(GENI_SE_DMA)`：`SE_GENI_DMA_MODE_EN |= 1`、
      **`SE_IRQ_EN = 0xf`**、**`SE_GSI_EVENT_EN = 0`**；
    - `geni_se_select_dma_mode()`：先 `geni_se_irq_clear()`（其中就写 `SE_GSI_EVENT_EN = 0`），
      把 FIFO 水线中断位从 `SE_GENI_M_IRQ_EN` 清掉，最后才置 DMA 位；
    - 反过来 GPI 模式是 `SE_IRQ_EN = 0`、`SE_GSI_EVENT_EN = 0xf`。
    我们的引擎从 bootloader 起就是 **0xf（GPI 配置）**，而 touch129..150 每次都只置 DMA 位、
    没关 GSI 事件 → 命令被交给没有 TRE ring 的 GSI 块，正是"永远 active、零字节、零中断"。
  - 另一条更硬的线索：`tch_dump_state("after init")` 的 `gsi 0x0000000f` 是在
    `tch_irq_clear()`（写 0）之后读到的——**这个寄存器写不进去**。如果 0xe18 真不可写，
    SE 的所有权就在 GSI 手上，SE DMA 再配也不会动，下一步只能是 GPI DMA。
  - touch152（`xnu/patches/0222-liuqin-touch-dma-gsi-mode.patch`，11 hunk）：
    1. 彻底不碰 0x2000/0x2004/0x2008；`tch_se_dma_if()` 改读 `DMA_IF_EN_RO@0xe20`，
       并对 `SE_GSI_EVENT_EN` 做**写-回读**，打印 `gsi 0x… -> 0x… (write took|IGNORED)`；
    2. `tch_dma_mode()`：`tch_irq_clear()` → `SE_IRQ_EN = 0`（本移植没有 QUP 中断处理程序，
       全程轮询，免得真通了以后变成没人数中断的风暴）→ 清 FIFO 水线中断位 → 置 DMA 位；
    3. 默认顺序改成上游的 **cmd-first**；`liuqintouchdmaorder=0` 和探测链的 `desc-first`
       仍可切回旧顺序；
    4. 完成判据加上 `SE_DMA_TX/RX_LEN_IN`（`SE_IRQ_EN=0` 时状态位可能不来），1 ms 处抓一次
       `SE_DMA_DEBUG_REG0/LEN_IN` 快照；失败且快照全 0 时直接打印 "the sequencer is not
       taking SE DMA"；
    5. DMA 路径失败后、退回 bit-bang 之前**再试一次 FIFO 引擎**（Linux 在没有 GPI 通道时
       正是这样 fallback）——若 FIFO 其实能用，立刻就有硬件定时的 SPI，不必等 GPI。
  - 产物：内核 `8d4a15c8…`、payload `d9d41857…`、`shim/out/shim-boot-touch152-gsi-mode.img`
    （`f2f1ac5f…`，`shim/out/shim-boot.img` = 这张）；启动行与 touch150/151 完全相同（567 B，
    结尾 `liuqintouchdma=1 liuqintouchsmmu=2`）。主机侧：编译通过（只有既有 warning）、
    221/221 补丁干净、0222 逆向校验（`patch -R` 后与 0221 状态 `cmp` 一致）、
    `validate-repo` 与 `tests/test_tools.py` 全过、payload 里的新串逐一核对。
  - 判据见 `docs/DMA_HANDOFF.md` 新加的一节（上一节以 `dma_if_en` 为核心的表已作废）。

- **2026-10-27 触摸第 200 步：touch152 实测 SE DMA 真的跑起来了（DMA_DONE/EOT、len_in 正确、TX probe ok）；坏在完成判据用了 `LEN_IN` → 命令还没移完就发下一条，读到垃圾、`M_CMD_OVERRUN`（touch153）**：
  - touch152 的日志（`152.txt`，6673 行）。先纠正第 199 步里两处从照片上读错的地方：
    - **QUP 是 v3.8**：`QUP hw 0x30080000 (v3.8)`，不是 v3.0。`SE+0x2004` 读就 panic 这件事仍然成立，
      但"v3.0 没有 0x2000 段"这个解释不成立（v3.8 应该有）；原因未知，可能和这颗 QUP 实际
      实现的窗口只有 0x1000 有关。**结论不变：不要碰 0x2000/0x2004。**
    - **`SE_GSI_EVENT_EN` 是可写的**：本次 `after init … gsi 0x00000000 seirq 0x0000000f`
      （第 199 步看到的 0xf 是照片误读），并且 `dma_if_en_ro 0x1`、`gsi 0x00000000 -> 0x00000000
      (write took)`。所谓"被 GSI 锁死"作废。
  - **关键成功证据**（第一次真正看到 DMA 工作）：
    ```
    DMA #1 tx+rx len 1 … dma_mode 0x00000001 gsi 0x00000000 seirq 0x00000000
    DMA #1 came back: status 0x00000000 irq 0x08000081 tx 0x00000003 rx 0x00000003
                     len_in tx 1 rx 1 after 2 polls
    TX probe as configured: ok
    SE4 running, chip select by hand
    ```
    `tx/rx 0x3` = DONE|EOT，`status 0x0`，`len_in` 等于长度。**上游那套"清 GSI 事件 → 置 DMA 位"
    就是之前缺的那一步**，SMMU bypass CB 也没问题。
  - 但读回的数据是垃圾：`Novatek trim id f6 a8 62 d6 09 40 (unknown id)`、
    `report space 63644x9018, 24 fingers, chip 0xf7`。原因找到了，是**我自己引入的完成判据**：
    - `SE_DMA_TX_LEN_IN` 表示**DMA 已经搬进 SE 的字节数**，不是 SPI 已经移出去的字节数。
      256 B 的写命令在 **35 次轮询（1.75 ms）** 时 `len_in` 就到 256，而 600 kHz 下 256 B 需要
      4.3 ms；那一行同时是 `status 0x00000041`（`M_GENI_CMD_ACTIVE` 还在）。
    - 于是下一条命令在引擎还忙的时候就写了 `M_CMD0` → 6 次
      `DMA failed: status 0x0 irq 0x00000002`（`M_CMD_OVERRUN`），
      `fw dl part 0 @0x00000 write failed after 0 chunks`，面板自然收到半条事务。
    - 附带把日志刷爆了：计数器只在 `< 8` 时自增，之后每条传输都打印
      `DMA #8 issuing …`，6673 行里绝大部分是这个。
  - 结论：**SE DMA 数据通路已经通了，坏的只是"什么时候算一条命令做完"**。
  - touch153（`xnu/patches/0223-liuqin-touch-dma-cmd-done.patch`，4 hunk）：
    1. 完成判据改成 `M_IRQ_STATUS & M_CMD_DONE_EN`（bit0），`LEN_IN` 只作诊断；
    2. `ok` 要求 `M_CMD_DONE` + 无错误位 + 已武装方向的 `TX/RX_DMA_DONE`；
    3. 计数器每条都自增，只有前 8 条打印；
    4. 失败诊断换成 `no M_CMD_DONE: …` / `command done, DMA status short: …`。
  - 顺手修了构建链的一个坑：`shim/out/fwarea-touch144.bin` 不知何时从 `shim/out` 消失了，
    用 `python3 tools/mkfwarea.py ~/liuqin-root/fw shim/out/fwarea.bin` 重建，
    sha256 `bdcb253bacd99b4a…` 与 NOTES 第 122 步记录的一致（8 MiB，`novatek…bin` 在 0x2b000）。
  - 产物：内核 `581853df…`、payload `c1acf8e7…`、`shim/out/shim-boot-touch153-cmd-done.img`
    （`110f7259…`，`shim/out/shim-boot.img` = 这张）；启动行不变（567 B）。主机侧：
    编译通过、222/222 补丁干净、`validate-repo` 通过、新串核对。
  - 下一次该看到：`Novatek trim id 20 00 00 32 65 03 (NT36532)`（和 bit-bang 那次一样），
    然后 `fw dl part …` 逐段写下去；256 B 一条大约 86 次轮询（4.3 ms），整包 ~4-5 s。
    如果某条仍打印 `no M_CMD_DONE`，看它同一行的 `status/irq`：`0x41` 说明引擎还在移，
    `irq 0x2` 说明上一条没等完又发了下一条。

- **2026-10-27 触摸第 201 步：touch153 把"命令完成"修对了（每条都是 `M_CMD_DONE`，不再 overrun），但面板读回仍不稳定；同时发现 SE 路径的 `tch.dummy` 一直是 0（每次读少一个字节）→ touch154 先自检再自校准**：
  - touch153 的日志（`153.txt`）。传输层已经完全正常，这是第一次没有任何 `DMA failed`：
    ```
    DMA #2 tx len 3 … came back: status 0x00000000 irq 0x00000001 tx 0x00000003 rx 0x00000000 len_in 3 after 2 polls
    DMA #3 tx+rx len 7 … came back: status 0x00000000 irq 0x08000081 tx 0x00000003 rx 0x00000003 len_in 7/7 after 4 polls
    ```
    即 TX-only 与 duplex 都按 `M_CMD_DONE` 收尾，`M_CMD_OVERRUN` 消失。
  - 但面板数据仍是垃圾，而且**两次启动不一样**：
    `trim id f6 88 62 d6 21 40`（本次）vs `f6 a8 62 d6 09 40`（touch152）；
    `@0x11a00 state 51 d4 ab f9`、`fw state 0x91`、`@0x2fe00` 全 ff。
    共同点是 `f6 ?? 62 d6 ?? 40`，且**与 MOSI 上的波形相关**——更像"面板根本没在驱动 MISO"
    或时钟/CS 时序处在边缘（和 NOTES 第 96 步在 bit-bang 早期看到的
    "83 80 3a 4f … / 83 81 3a 47 …，marginal read" 同一类现象）。
  - 读日志时发现一个**确定的 bug**：`tch.dummy` 在 SE 路径里从没被赋值，结构体默认 0；
    而 pad 路径明确设 `tch.dummy = 1`（第 2669-2676 行的注释：dummy=0 时 trim id 会整体
    移一位，"3f 20 00 00 32 65" 而不是 "20 00 00 32 65 03"）。日志完全对上：
    `DMA #3 … len 7` = 6 + 1 + **0**，而 pad 路径是 `len 8`。**SE 路径每一次读都少一个字节、
    而且错位。**
  - 另一处差异：SE 路径从不复位面板（gpio20），pad 路径会。但 pad 路径在复位前的
    "probing before any reset" 就能读到正确的 trim id，所以暂时不把复位塞进来（一次只动一个变量）。
  - touch154（`xnu/patches/0224-liuqin-touch-dma-diagnose.patch`，2 hunk）：
    1. `tch.dummy = 1; tch.page8 = false;` 放进 SE 分支（修上面那个 bug）；
    2. 新增 `tch_dma_diag()`，在 `tch_se_start()` 之前跑四件事：
       - 打印 `SE_SPI_LOOPBACK / DEMUX_OUTPUT_INV / TRANS_CFG`；
       - 读 gpio16..19 的 pad 电平（mux 到 qup4 后 `GPIO_IN_OUT` bit0 仍反映引脚），
         在 CS 命令前后各读一次——**引擎的 CS 到底有没有到 gpio19**；
       - loopback（`SE_SPI_LOOPBACK=1`）发 `55 aa 0f f0` 再收，验证 SE 的 DMA/packing
         是否逐字节正确（把"引擎内部"和"面板/引脚"分开）；
       - 用 ROM trim id 做**自校准**：`(顺序 desc/cmd) × (div 32/128/8)` 六种组合各读一次，
         第一个返回 `.. 65 03` 的组合被保留；每个组合都打印结果。
  - 产物：内核 `b8f91bba…`、payload `23a1777c…`、`shim/out/shim-boot-touch154-diag.img`
    （`1636e600…`，`shim/out/shim-boot.img` = 这张）；启动行不变（567 B）。
    主机侧：编译通过、223/223 补丁干净、`validate-repo` 通过、新串核对。
  - 下一次判读：
    - `pads … assert 0x?` 的 bit3 从 1 变 0 → CS 到了引脚；一直不变 → SE 的 CS 没接到 gpio19，
      那就要么改成 GPIO 手动 CS，要么查 SE4 的 CS 映射。
    - `SE loopback … byte exact` → SE 数据通路没问题，问题在面板侧；`MISMATCH` → 还是
      packing/顺序问题。
    - 六个 `trim id …` 里任意一个 `MATCH` → 后续整场都用它，固件下载应该立刻正常；
      全 `no match` 且 loopback 正常 → 基本可以确定是引脚/CS/电气层面。

- **2026-10-27 触摸第 202 步：touch154 三个结论——CS 到引脚、loopback 回全零、面板开始"确定性"地答错；touch155 加 pad 路径对照、复位与 CS 全程跟踪**：
  - touch154 的日志（`154.txt`）关键行：
    ```
    pads 16..19 idle 0x8 assert 0x0 release 0x8 (bit3 = gpio19 = CS)
    SE loopback tx 55 aa 0f f0 rx 00 00 00 00 (MISMATCH)
    trim id desc-first div 32:  ff ff ff ff ff ff   (no match)
    trim id cmd-first  div 32:  00 00 02 01 aa aa   (no match)
    trim id … div 128/8:        00 00 02 01 aa aa   (后四次完全一样)
    ```
  - 结论 1：**CS 正确到达 gpio19 并翻转**（bit3：1→0→1）。SE 的 CS 映射、pad 复用都没问题，
    "改成 GPIO 手动 CS"这条排除了。
  - 结论 2：**loopback 收全零**。这颗 SE 的 `GENI_IF_DISABLE_RO=1`（FIFO 接口被禁用），
    loopback 很可能走 FIFO 接口，所以这个结果本身不可全信；但它留下头号嫌疑：
    **TX 数据到底有没有进移位寄存器**（TX 空 FIFO 移位 → loopback 全零 + 面板收到全零命令，
    全都对得上）。
  - 结论 3：dummy=1 修复后，面板的回答不再每次都不一样：第一次 `ff ff …`，之后五次全是
    同一个确定值 `00 00 02 01 aa aa`——面板在回答，但答错，即命令内容不对或面板状态不对。
    整场 probe 的 id 是 `66 96 09 40 dd bf`（随时间又变了），面板状态在漂移。
  - 承认 touch154 的一个实现错误：**扫描里 "desc-first" 标签是假的**——`tch_dma_order` 只在
    命中时才赋值，六次读实际全跑了 cmd-first。touch155 改成每次读之前赋值。
  - touch155（`xnu/patches/0225-liuqin-touch-dma-diag2.patch`，6 hunk）：
    1. loopback 写 1 后回读打印；
    2. **CS 全程跟踪**：一次完整读的五个 pad 采样（idle/assert/写完 page/读完/释放）+ rx 6 字节，
       看 FRAGMENTATION 有没有把 CS 保持到读结束；
    3. 前 3 条传输打印 TX 暂存页前 8 字节（CPU 视角，确认送出去的内容没错）；
    4. 扫描顺序修复：先 `tch_dma_order = order` 再读；
    5. 六种组合全不中后**切回 bit-bang 在 pad 上读一次 trim id**（mux 回 GPIO、校准、读、
       再 mux 回 qup4）——面板到底还活着没有，把"SE 侧"与"面板侧"彻底分开；
    6. 然后 **gpio20 复位脉冲**，再用 SE 读两次（两种顺序）。
  - 产物：内核 `aeffad95…`、payload `7b78161f…`、`shim/out/shim-boot-touch155-diag2.img`
    （`e15ca8c4…`，`shim/out/shim-boot.img` = 这张）；启动行不变（567 B）。
    主机侧：编译通过、224/224 补丁干净、`validate-repo` 通过、新串核对。
  - 下一次判读：
    - `CS trace … after-page 0x?` 的 bit3 为 0 → CS 保持住了；为 1 → FRAGMENTATION 没生效，
      CS 在 page 写与读之间掉了，命令因此到不了面板。
    - `pad-path trim id … (panel alive)` → 面板没问题，问题全在 SE 数据路径；
      `(no answer)` → 我们的 SE 传输把面板弄挂了，复位之后看 `reset retry`。
    - `DMA #N TX page 55 aa 0f f0 …` → CPU 侧数据没错；SE 移出去的是不是这些字节仍未知。
    - `reset retry … MATCH` → 缺的就是复位；`no match` → 回头修 SE 数据路径。

- **2026-10-27 触摸第 203 步：touch155 判定——面板活着、CS 全程保持、desc-first 的 TX-only 必挂；问题缩小到"cmd-first 下发命令时第一个字节"；touch156 直接测移位机制**：
  - touch155 的日志（`155.txt`）关键行：
    ```
    pads 16..19 idle 0x8 assert 0x0 release 0x8            ← CS 翻转正常
    CS trace idle 0x1 assert 0x1 after-page 0x1 after-read 0x1 release 0x9
    DMA #4 issuing opcode 1, descriptor before command → status 0x41 after 1000 polls（挂死）
    DMA #7 同上（desc-first TX-only 第二次挂死）
    trim id cmd-first div 128:  00 00 02 01 aa aa (no match)
    pad-path trim id 20 00 00 32 65 03 (panel alive)      ← 决定性
    reset retry trim id …:      00 00 02 01 aa aa (no match)
    ```
  - 结论（都在这张日志里）：
    1. **面板没坏**：我们一堆 SE 传输之后立刻切回 bit-bang，trim id 正确。
       问题 100% 在 SE 数据路径，不在面板、不在 CS、不在复位。
    2. **CS 全程保持**：`CS trace` 的 bit3 在 assert/after-page/after-read 都是 0，
       释放后回 1——FRAGMENTATION 保住了 CS，协议分帧和 bit-bang 一致。
       （注意 idle=0x1 说明上一条数据命令自己就把 CS 拉低了，符合 CS_TOGGLE=0 的行为。）
    3. **desc-first 的 TX-only 必然挂死**（`status 0x41`、`tx 0x0`，但 `len_in 3`）：
       描述符先于命令武装时，DMA 的 EOT 在命令激活前就发完了，序列器永远等不到完成。
       → 顺序只能 cmd-first（上游今天的顺序，符合）；desc-first 从候选里去掉。
    4. **复位无用**：`reset retry` 与复位前一样。面板状态不是原因。
    5. cmd-first 的读稳定返回 `00 00 02 01 aa aa`（确定但错误）；
       不同地址读回不同内容，说明命令**部分**到达面板。
  - 现在的头号假设：**cmd-first 时命令发出的一瞬间 FIFO 还是空的，第一个字节被移成
    未知值/丢掉**，面板按移位后的命令解析 → 确定性错答。loopback 全零也可由
    "移位通路没吃到 DMA 的数据"解释，但 loopback 本身可能走被禁用的 FIFO 接口，不可全信。
  - touch156（`xnu/patches/0226-liuqin-touch-dma-txprobe.patch`，3 hunk），专门测移位机制：
    1. **8 字节 DMA loopback**（tx 01..08，打印全部 8 个 RX）——如果有移位/丢字节，
       移位模式直接现形；
    2. **FIFO loopback**（tx 11 22 33 44，走 FIFO 路径）——移位寄存器本身好不好；
       FIFO 能回读、DMA 不能 → 问题在 DMA→FIFO 的投递时序；
    3. **raw read8**：完整 8 字节 RX 不跳 offset 原样打印；
    4. **raw double-addr**：把地址字节连发两次（9 字节），看面板是否在第二个字节上应答
       ——若是，则证明第一个字节被吃/被替，接下来就能用"TX 前插一个字节"补偿。
  - 产物：内核 `3a4f33ff…`、payload `bcc96930…`、`shim/out/shim-boot-touch156-txprobe.img`
    （`3288c293…`，`shim/out/shim-boot.img` = 这张）；启动行不变（567 B）。
    主机侧：编译通过、225/225 补丁干净、`validate-repo` 通过、新串核对。
  - 下一次判读：
    - `SE loopback8 … rx 00 01 02 03 04 05 06 07`（右移一位）→ 就是丢首字节，
      改 `cmd-first` + TX 前置一个占位字节补偿；
    - `FIFO loopback ok rx 11 22 33 44` + DMA 不对 → DMA 投递时序问题，试
      "先武装 TX、命令、再武装 RX"（首字节就有了，EOT 在命令激活后才发，可能绕开
      desc-first 的挂死）；
    - `FIFO loopback` 也不对 → 移位/打包本身坏，跟 DMA 无关，查 packing 配置；
    - `raw double-addr` 的第二个 0x04 之后出现 `20 00 00 32 65 03` → 首字节被替换
      成未知值 → 直接上补偿法。

- **2026-10-27 触摸第 204 步：touch156 判定——loopback 在这颗 SE 上彻底死了（DMA 和 FIFO 都是全零），不能用它测 TX；面板 raw 流是确定性的错误；touch157 改问"FIFO 路径读面板"和"page 写有没有丢"**：
  - touch156 的日志（`156.txt`）关键行：
    ```
    loopback reg written 1, reads 0x00000001        ← 寄存器可写
    SE loopback8 … rx 00 00 00 00 (transfer failed) ← RX 状态位 0、页全零
    FIFO loopback failed rx 00 00 00 00             ← 走 FIFO 也一样全零
    raw read8 rx 10 00 00 00 02 01 aa aa
    raw double-addr rx 00 00 00 02 01 aa aa 55 55
    pad-path trim id 20 00 00 32 65 03 (panel alive)
    ```
  - 结论：
    1. **loopback 通路在这颗 SE 上是死的**：寄存器能写（读回 1），但 DMA 路径和 FIFO 路径的
       loopback 都回全零。`GENI_IF_DISABLE_RO=1`（FIFO 接口被禁用）能解释——loopback 大概
       走 FIFO 接口。**所以 loopback 不能用来量 TX，这条路划掉。**
    2. 面板的 raw 流 `10 00 00 00 02 01 aa aa`（`aa aa`、`55 55` 像状态/测试码），
       double-address 也没把 id 救回来 → 首字节不是简单"被顶掉"。
    3. `pad-path` 再次证明面板好、SE 侧错。
    4. 一个日志噪声：touch156 的 CS trace 里 DMA #2 是 `M_CMD_OVERRUN`——因为 FIFO loopback
       在 trace 之前跑、把引擎留在了坏状态。diag 里测试的顺序本身有副作用。
  - touch157（`xnu/patches/0227-liuqin-touch-dma-fifotest.patch`，2 hunk）：
    1. **删掉** loopback8 + FIFO-loopback 两条传输（又死又污染引擎状态），只留 loopback
       寄存器写回读；
    2. 六种组合全不中后，**切 FIFO 路径读面板 trim id**（`tch.fifo_capable=true` 读一次）——
       FIFO 接口和 DMA 引擎共用同一个移位寄存器：FIFO 能读对 → 移位/时钟/CS 都好，问题在
       DMA→FIFO 的投递；FIFO 也读不对 → 移位通路本身坏；
    3. **page-0 读**：故意把 page 写成 0 再读 trim 地址，若结果和正常读一样，说明
       TX-only 的 page 写根本没生效，所有读都落在默认页。
  - 产物：内核 `22136133…`、payload `3135e5e4…`、`shim/out/shim-boot-touch157-fifotest.img`
    （`96a0e219…`，`shim/out/shim-boot.img` = 这张）；启动行不变（567 B）。
    主机侧：编译通过、226/226 补丁干净、`validate-repo` 通过、新串核对。
  - 下一次判读：
    - `FIFO-path trim id … MATCH` → 移位通路好，问题在 DMA 投递 → 改 "先武装 TX→命令→再武装 RX"
      或给 TX 加前置占位字节；
    - `FIFO-path … no match/read failed` → 移位/时钟/CS 本身坏（与 DMA 无关）→ 查 packing、
      `SE_GENI_BYTE_GRAN`、SPI 模式/相位；
    - `page-0 read` 与 `raw read8` 一样 → page 写没生效（TX-only 传输丢字节），读数全在默认页；
      `page-0 read` 与 `raw read8` 不同 → page 写生效了，问题在地址字节或读的某一步。

- **2026-10-27 触摸第 205 步：touch157 定案——page 写完全无效（page-0 读与正常读一模一样），FIFO 路径也读不了，desc-first 挂死时 TX FSM 停在 state 11；touch158 上"GPIO 逻辑分析仪"直接看线**：
  - touch157 的日志（`157.txt`）关键行：
    ```
    CS trace idle 0x8 assert 0x0 after-page 0x0 after-read 0x0 release 0x8   ← 干净的一次
    FIFO-path trim id 00 00 00 00 00 00 (read failed)
    page-0 read  rx 10 00 00 00 02 01 aa aa
    raw read8    rx 10 00 00 00 02 01 aa aa        ← 与 page-0 完全相同
    DMA #3 (desc-first) state: debug 0x000000b1 (tx_active 1) …
    pad-path trim id 20 00 00 32 65 03 (panel alive)
    ```
  - 结论：
    1. **page 写完全无效**：故意写 page=0 与写真实 page 0x3f62，读回**逐字节相同**。
       面板根本没收到那 3 个字节（或者收到的是同一个错值），所有读都落在默认页上。
       这就解释了为什么"每次读结果一样、换地址却偶尔不同"——页面机制从没生效。
    2. **FIFO 路径也读不了**（`read failed`）：`GENI_IF_DISABLE_RO=1` 是真的，FIFO 接口
       在这颗 SE 上不可用，不能当参照。
    3. **desc-first 挂死时 `debug 0x000000b1`：`DMA_TX_ACTIVE=1`、`DMA_TX_STATE=0xb`**——
       第一次看到挂死那一刻的 TX DMA 状态机停在 state 11；cmd-first 则干净完成（tx 0x3）。
    4. CS 行为这次是干净的（assert/after-page/after-read 全 0，release 回 1）。
    5. 面板依然活着（pad 路径 id 正确）。
  - 所以现在的结论只剩一句话：**SE 的 TX 字节没有以正确内容出现在 MOSI 上**，
    CS/时钟/完成信号/RX 捕获全都是好的。最后一个证人就是线本身。
  - touch158（`xnu/patches/0228-liuqin-touch-dma-wirecapture.patch`，2 hunk）：
    新增 `tch_wire_capture()`——把 SPI 时钟降到最慢（div 4095 = 4.7 kHz，213 µs/bit），
    发 8 字节已知图样 `a5 5a 3c c3 0f f0 11 88`，在引擎移位的同时用 CPU 紧循环采样
    gpio16/17/18（MISO/MOSI/CLK）的电平，按 CLK 上升沿重建 MOSI 和 MISO 的每一个字节，
    打印 `wire tx …` / `wire rx …` 和采到的位数。放在 !found 分支里、page-0 读之后、
    切回 bit-bang 之前。
  - 产物：内核 `35581035…`、payload `b1ec60ef…`、`shim/out/shim-boot-touch158-wirecapture.img`
    （`495ce582…`，`shim/out/shim-boot.img` = 这张）；启动行不变（567 B）。
    主机侧：编译通过、227/227 补丁干净、`validate-repo` 通过、新串核对。
  - 下一次判读：
    - `wire tx a5 5a 3c c3 0f f0 11 88` → TX 线是对的，问题在面板侧对时序的容忍
      （CS 建立/保持、字节间空隙）→ 加 CS/命令间延时；
    - `wire tx 00 00 …` 或 `ff ff …` → TX 数据根本没出去 → 查 DMA→FIFO→移位 的投递；
    - `wire tx 52 ad 1e …`（整体右移一位）→ 就是首比特相位问题 → 换 CPHA 或前置补偿；
    - `wire tx` 与图样完全不同但 `(N bits)` 正常 → 记录实际值再定位；
    - `0 bits` → 移位根本没跑（时钟没动），回到时钟/命令层。

- **2026-10-27 触摸第 206 步：touch158 的实测翻转结论——duplex 的 TX 在线上逐字节正确（`wire tx a5 5a 3c c3 0f f0 11 88`）；剩下的唯一嫌疑是 TX-only（page 写）到底有没有移位；touch159 把三种捕获都做一遍**：
  - touch158 的日志（`158.txt`）关键行：
    ```
    wire capture at 4688 Hz, tx a5 5a 3c c3 0f f0 11 88
    wire tx a5 5a 3c c3 0f f0 11 88 (64 bits)     ← 逐字节完全正确
    wire rx 00 00 00 00 00 00 00 00               ← 图样不是合法命令，面板不应答（预期）
    page-0 read rx 10 00 00 00 02 01 aa aa        ← 与 touch157 一致：page 写无效
    pad-path trim id 20 00 00 32 65 03 (panel alive)
    ```
  - 结论（重要，方向翻转）：
    1. **duplex 的 TX 通路完全正确**：DMA 投递、打包、移位寄存器、时钟、CS 全部好。
       之前所有"命令被移位/丢字节"的猜测都排除。
    2. `wire rx` 全零是预期的——`a5 5a …` 不是合法命令，面板自然不说话。
       这也说明 **RX 采样点/引脚没问题**（面板不驱动时稳定读 0）。
    3. 仍然成立的是：**page 写无效**（page-0 读与正常读逐字节相同）。
  - 于是唯一没被捕获过的路径就是 **TX-only（opcode 1）**——page 写正是它。两种可能：
    (a) TX-only 命令实际上没有把字节移出去（duplex 好、TX-only 坏）；
    (b) TX-only 移出去了，但 page 值被写错。
  - touch159（`xnu/patches/0229-liuqin-touch-dma-txonly-capture.patch`，3 hunk）：
    `tch_wire_capture()` 改成带参数 `(tx_only, 名字, 图样, 长度)`，一次跑三种捕获：
    1. `duplex`：8 字节已知图样 `a5 5a 3c c3 0f f0 11 88`（对照，应逐字节命中）；
    2. `txonly`：**真实的 page 写 `ff 3f 62`**（关键——它在不在线上）；
    3. `readcmd`：**真实的读命令 `04 00×7`**（顺便看面板在这个慢速时钟下到底回什么，
       与驱动读到的 `10 00 00 00 02 01 aa aa` 对照）。
  - 产物：内核 `e9f8fbf2…`、payload `81177649…`、`shim/out/shim-boot-touch159-txonly.img`
    （`9e112669…`，`shim/out/shim-boot.img` = 这张）；启动行不变（567 B）。
    主机侧：编译通过、228/228 补丁干净、`validate-repo` 通过、新串核对。
  - 下一次判读：
    - `wire txonly tx ff 3f 62 (24 bits)` → page 写在线上是对的 → 问题在面板对这个
      "慢速/快速"时序的接受度或 page 语义（比如需要更多 CS 间隔），改加延时；
    - `wire txonly tx 00 00 00` 或位数不对 → **TX-only 根本没移位** → 这是最终病根，
      改成"duplex 发 page 写"（opcode 7，RX 丢弃）即可绕过；
    - `wire readcmd rx …` 与驱动的 `10 00 00 00 02 01 aa aa` 一致 → RX 通路与面板应答
      都对，问题只在 page 写；
    - `wire readcmd rx` 与驱动读到的不同 → RX 在快速时钟下采样有问题，也要一起修。

- **2026-10-27 触摸第 207 步：touch159 的线探针给出铁证——`page 写 ff 3f 62` 正确在线、读命令正确、**面板在 4.7 kHz 下回出正确的 `20 00 00 32 65 03`**；所以数据通路全对，病根是 page 写与读之间的间隔（touch160 加快关可调延时并自校准）**：
  - touch159 的日志（`159.txt`）关键行：
    ```
    wire duplex  tx a5 5a 3c c3 0f f0 11 88 (64 bits)
    wire txonly  tx ff 3f 62 (24 bits)              ← TX-only 也正确！
    wire readcmd tx 04 00 00 00 00 00 00 00 (64 bits)
    wire readcmd rx 00 3f 20 00 00 32 65 03         ← 面板回出正确 trim id
    ```
  - 解读：`rx` 的第 2…7 字节正好是 `20 00 00 32 65 03`（前面 `00 3f` 是地址/哑字节）。
    也就是说：**DMA 投递、打包、移位、TX-only、RX 采样、CS、时钟——整条 SE 通路在 4.7 kHz
    下逐字节正确，面板也完全正常应答。**
  - 那 600 kHz 下为什么 `00 00 02 01 aa aa`？两个差异：
    1. 时钟（4.7 kHz vs 600 kHz）；
    2. **page 写与读之间隔了多久**——线探针里中间夹了 `printf`（毫秒级），驱动的
       `tch_nvt_read_locked` 中间只有几条寄存器写（微秒级）。
    日志里 div 128（150 kHz）也一样错，说明**更可能是间隔**而不是时钟：150 kHz 的位宽
    完全够用，但间隔依然只有几微秒。
  - touch160（`xnu/patches/0230-liuqin-touch-dma-page-delay.patch`，6 hunk）：
    1. 新增两个全局 `tch_page_dly`（page 写→读的间隔，µs）和 `tch_cs_dly`
       （CS 拉低→首个时钟的间隔，µs），默认都是 0，行为不变；
    2. 在**三处 page 写之后**都插入 `tch_page_dly` 延时（读、SRAM 块写、单字节写），
       在 `tch_cs(true)` 之后插入 `tch_cs_dly`；
    3. diag 里在线探针之后再跑一个 **4 组的时序矩阵**
       `(page,cs) = (0,0)/(1000,0)/(1000,50)/(5000,50)`，第一组返回正确 id 的就被保留，
       后续整场都用它；全不中则恢复 (0,0)。
  - 产物：内核 `1615f085…`、payload `54f3005b…`、`shim/out/shim-boot-touch160-timing.img`
    （`1344daae…`，`shim/out/shim-boot.img` = 这张）；启动行不变（567 B）。
    主机侧：编译通过、229/229 补丁干净、`validate-repo` 通过、新串核对。
  - 下一次判读：
    - 任意一组 `MATCH` + `keeping page delay …` → **根因就是间隔**，而且整场已经自动用上了；
      接着看 `Novatek trim id 20 00 00 32 65 03`、`fw dl part …` 是否顺利写下去；
    - 四组全 `no match` → 间隔不是全部原因；下一步把**时钟**也加进矩阵
      （div 128/32 与延时组合），或者直接怀疑 600 kHz 下 RX 采样点；
    - `wire readcmd rx` 一旦不再等于 `00 3f 20 00 00 32 65 03`，说明面板状态变了，先复位再测。

- **2026-10-27 触摸第 208 步：touch160 的延时矩阵四组全灭——间隔不是原因；线探针在 4.7 kHz 仍然正确。剩下只有"时钟本身"和"高速下首字节投递"两个变量，touch161 各测一个**：
  - touch160 的日志（`160.txt`）关键行：
    ```
    page delay 0    us, cs delay 0  us: 00 00 02 01 aa aa (no match)
    page delay 1000 us, cs delay 0  us: 00 00 02 01 aa aa (no match)
    page delay 1000 us, cs delay 50 us: 00 00 02 01 aa aa (no match)
    page delay 5000 us, cs delay 50 us: 00 00 02 01 aa aa (no match)
    wire readcmd rx 00 3f 20 00 00 32 65 03        ← 4.7 kHz 下仍然正确
    pad-path trim id 20 00 00 32 65 03 (panel alive)
    ```
  - 结论：**page 写→读的间隔不是原因**（连 5 ms 都不行），CS 拉低后的静默也不是。
    而 4.7 kHz 下同样的引擎、同样的命令，面板回出正确 id。
  - 剩下两个变量：
    1. **时钟本身**：慢到 4.7 kHz 才对——需要知道 SE 自己的读在慢速下是否也正确，
       以及阈值在哪；
    2. **高速下首字节的投递**：cmd-first 是"先命令、后武装描述符"，600 kHz 下一个字节
       13.3 µs，武装描述符约 10 µs——第一个字节可能在 DMA 还没就绪时就开始移位了。
       4.7 kHz 下一个字节 1.7 ms，DMA 早就就绪，所以线探针看不出来。
  - touch161（`xnu/patches/0231-liuqin-touch-dma-speed-sweep.patch`，1 hunk）：
    (a) **速度扫描**：cmd-first 下用 div 4095 / 512 / 128 / 32 各读一次 trim id，
        第一个 MATCH 的 div 被保留给整场；
    (b) **desc-first duplex**：在 600 kHz 下把描述符先武装再发命令，同时把 page 写
        改成 **duplex（opcode 7，RX 丢弃）**——这样既让首字节提前进 FIFO，又绕开
        TX-only 的 desc-first 挂死（state 11）。
  - 产物：内核 `13e94465…`、payload `bec403fe…`、`shim/out/shim-boot-touch161-speed.img`
    （`1fd932c6…`，`shim/out/shim-boot.img` = 这张）；启动行不变（567 B）。
    主机侧：编译通过、230/230 补丁干净、`validate-repo` 通过、新串核对。
  - 下一次判读：
    - `se read div 4095 (4688 Hz): … MATCH` → SE 读在慢速下正确 → 纯速率问题；
      再看 512/128 是否也 MATCH，阈值决定可用速度；
    - `desc-first duplex: … MATCH at rx[6..7]` → **首字节投递就是根因**，修法明确：
      page 写改 duplex + 全程 desc-first；
    - 两者都不中 → 慢速也错说明 RX 采样点/相位在高速下不对，下一步在 4.7 kHz 下打印
      SE 自己读到的 8 字节原始数据，与 CPU 线探针逐字节对照。

- **2026-10-27 触摸第 209 步：touch161 把病根钉在 RX——不是时钟、不是顺序，而是**SE 的 RX 捕获**：面板在线上给的是正确数据，SE 的 DMA 收进来的是 `10 00 00 00 02 01 aa aa`，4.7 kHz 也一样错**：
  - touch161 的日志（`161.txt`）关键行：
    ```
    se read div 4095 (4688 Hz): 00 00 02 01 aa aa (no match)   ← 慢速也错！
    se read div 512  (37500 Hz): 00 00 02 01 aa aa (no match)
    se read div 128  (150000 Hz): 00 00 02 01 aa aa (no match)
    se read div 32   (600000 Hz): 00 00 02 01 aa aa (no match)
    desc-first duplex:           10 00 00 00 02 01 aa aa (no match)
    wire readcmd rx 00 3f 20 00 00 32 65 03                    ← CPU 采样正确
    ```
  - 结论（重大修正）：
    1. **SE 自己的读在任何时钟下都错**（连 4.7 kHz 都错），而**同一时钟、同一条线**上
       CPU 采样能看到正确的 `20 00 00 32 65 03`。→ **TX/MOSI、面板、时钟都没问题，
       坏的是 SE 的 RX 捕获通路**（移位寄存器 → RX FIFO → DMA → 内存）。
    2. 顺序（desc-first duplex）也无关：结果一模一样。
    3. 之前"page 写无效"的判据（page-0 读 == 正常读）**不再成立**——既然 RX 本身是坏的，
       两次读当然得到同样的垃圾。page 写的证据要重新解读（线探针已证明它在线正确）。
    4. 之前"4.7 kHz 能行"的说法也不对：线探针只证明了**线上**对，SE 收到的仍然是错的。
  - touch162（`xnu/patches/0232-liuqin-touch-dma-rxcompare.patch`，3 hunk）：
    `tch_wire_capture()` 在**同一次传输**上同时给出两个视角：
    1. `wire <name> rx`（CPU 逐边沿采样）；
    2. `dma <name> rx`（SE 的 DMA 写进 RX 页的 24 字节，**RX 页预先填 0xee**，
       这样写偏了/没写都会现形），并打印 `SE_DMA_RX_LEN_IN` 与 `RX_IRQ_STAT`。
  - 产物：内核 `65c1cabd…`、payload `0d47e20d…`、`shim/out/shim-boot-touch162-rxcompare.img`
    （`1e7342a6…`，`shim/out/shim-boot.img` = 这张）；启动行不变（567 B）。
    主机侧：编译通过、231/231 补丁干净、`validate-repo` 通过、新串核对。
  - 下一次判读（看 `readcmd` 那组）：
    - `dma readcmd rx` 前 8 字节仍是 `00 00 00 00 02 01 aa aa`，后面是 `ee ee…`
      → SE 确实写了 8 字节，但内容是"自己算出来的"，与真实 MISO 无关 → RX FIFO 数据源
      或 packing 错，下一步查 `SE_GENI_RX_PACKING_CFG0/1`、`SE_GENI_BYTE_GRAN`、
      `SE_DMA_RX_ATTR`；
    - `dma … rx` 全是 `ee` → RX DMA 根本没写（尽管报 DONE）→ 目标地址/描述符问题；
    - `dma … rx` 从第 4/8 字节才开始变 → 写偏了，按偏移修正；
    - `dma … rx` 与 `wire … rx` 一致 → RX 其实是对的，那矛盾就出在其他地方（极不可能）。

- **2026-10-27 触摸第 210 步：touch162 对同一笔传输的两个视角——CPU 看到 `00 3f 20 00 00 32 65 03`，SE 的 RX DMA 在**同一个偏移**写进 `10 00 00 00 02 01 aa aa`（后面还是 `ee`）。RX 地址和长度都对，**内容不是面板的数据**；touch163 开始查 RX 的配置与触发条件**：
  - touch162 的日志（`162.txt`）：
    ```
    wire readcmd rx 00 3f 20 00 00 32 65 03
    dma  readcmd rx 10 00 00 00 02 01 aa aa ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee
                    (len_in 8 irq 0x00000000)
    wire duplex rx 00 00 00 00 00 00 00 00
    dma  duplex rx 00 00 00 00 00 00 00 00 ee …
    ```
  - 读法：
    1. **偏移正确**：RX 页预填 `ee`，前 8 字节被覆盖、后面仍是 `ee` → DMA 确实写在
       `TCH_DMA_RX_OFF+0`，长度也对。所以不是"写偏了"。
    2. **内容错**：SE 收到 `10 00 00 00 02 01 aa aa`，而线上是 `00 3f 20 00 00 32 65 03`。
       注意 `10 00 00 00` 恰好是 TX 首字节 `04` **左移 2 位**的结果 —— 像是 RX 侧
       采到的是 MOSI（或移位相位偏了），而不是面板的 MISO；末尾 `aa aa` 像电平/时钟串扰。
    3. duplex 那组线上全零、SE 也收全零 —— 与"空 FIFO 返回 0"一致，不能证明 RX 工作。
    4. `irq 0x00000000` 是假象：`tch_cs(false)` 里的 `tch_irq_clear()` 会清 DMA 状态，
       读数在它之后。下一版把状态读在清之前。
  - touch163（`xnu/patches/0233-liuqin-touch-dma-rxvariants.patch`，4 hunk）：
    1. 基线捕获后打印 RX 配置：`SE_DMA_RX_LEN/ATTR`、`SE_GENI_BYTE_GRAN`、
       `SE_GENI_RX_PACKING_CFG0/1`、`SE_GENI_RX_FIFO_STATUS`；
    2. **variant 1**：把 `RX_WATERMARK/RFR` 从 FIFO 模式的 13/14 改成 **1/1** 再捕获
       —— 验证"水线挡住 RX DMA 触发"的猜想（跑完恢复 13/14）；
    3. **variant 2**：`SE_DMA_RX_ATTR = EOT_BUF`（Linux 写 0）再捕获。
  - 产物：内核 `1a484878…`、payload `f5c4d0b7…`、`shim/out/shim-boot-touch163-rxvariants.img`
    （`473ec262…`，`shim/out/shim-boot.img` = 这张）；启动行不变（567 B）。
    主机侧：编译通过、232/232 补丁干净、`validate-repo` 通过、新串核对。
  - 下一次判读（对比同一行的 `wire readcmd* rx` 与 `dma readcmd* rx`）：
    - `dma readcmd-wm1 rx` 等于线上值 → **水线就是原因**，把 1/1 写进 DMA 路径；
    - `dma readcmd-attr1 rx` 等于线上值 → RX 需要 EOT 属性；
    - 两个都还是 `10 00 00 00 02 01 aa aa` → RX 数据源本身没接通，按 `rx cfg` 那行
      检查 packing/gran，并把 FIFO 状态与捕获时对照；
    - 若 `fifo_stat` 显示 RX FIFO 里确实有字，而 DMA 拿到的不是它 → DMA 读 FIFO 的端口/
      偏移问题，下一步改用 FIFO 直接读（不走 DMA）做对照。

- **2026-10-27 触摸第 211 步：touch163 三个 RX 变体全部无效（水线 1/1、EOT 属性都一样），但 `rx cfg` 证明 packing/gran 与 Linux 完全一致；touch164 改为"不武装 RX DMA"直接问 FIFO 有没有收到数据**：
  - touch163 的日志（`162.txt`，sha256 `6403f22f…`）：
    ```
    rx cfg len 8 attr 0x00000000 gran 0 pack0 0x0007f8fe pack1 0x000ffefe fifo_stat 0x00000000
    wire readcmd           rx 00 3f 20 00 00 32 65 03
    dma  readcmd           rx 10 00 00 00 02 01 aa aa …
    capture variant 1: wm 1/1 attr 0x00000000
    dma  readcmd-wm1       rx 10 00 00 00 02 01 aa aa …    ← 水线无效
    capture variant 2: wm 13/14 attr 0x00000000
    dma  readcmd-attr1     rx 10 00 00 00 02 01 aa aa …    ← EOT 属性无效
    ```
  - 读法：
    1. **水线不是原因**（1/1 与 13/14 结果完全相同）；**RX EOT 属性也不是**。
       顺带发现 `SE_DMA_RX_ATTR` 写 1 后读回仍是 0 —— 这颗 SE 上 RX 的 ATTR 位写不进去。
    2. **packing 与 Linux 完全一致**：`pack0 0x0007f8fe / pack1 0x000ffefe` 正是
       `geni_se_config_packing(8, 4, msb_first=1)` 笔算出来的值；`BYTE_GRAN=0` 也对。
       → 打包配置可以排除。
    3. `fifo_stat 0x00000000` 是 DMA 把 FIFO 抽干之后的读数，**不能**说明 FIFO 没进过数据。
  - 所以现在只剩一个问题：**RX FIFO 到底有没有被写入过？**
  - touch164（`xnu/patches/0234-liuqin-touch-dma-rxfifotest.patch`，2 hunk）：
    新增 `tch_rx_fifo_test()`——把真实读命令按 4.7 kHz 打出去，但**只武装 TX DMA、
    不武装 RX DMA**，让 FIFO 不被抽干；然后读 `SE_GENI_RX_FIFO_STATUS` 的字数，
    并**直接把 FIFO 里的字读出来**打印。
    - FIFO 里有正确的字 → RX 数据路径是好的，坏的只是 "FIFO→DMA→内存"，
      而且**可以绕开 RX DMA，用 CPU 直接读 FIFO**（读只有 8 字节，FIFO 深 16 字，够用）；
    - FIFO 是空的 → RX 数据压根没进 FIFO，这颗 SE 的接收通路只能在 GSI/GPI 模式下工作。
  - 产物：内核 `5513f95e…`、payload `aeb2c2eb…`、`shim/out/shim-boot-touch164-rxfifo.img`
    （`1a62b399…`，`shim/out/shim-boot.img` = 这张）；启动行不变（567 B）。
    主机侧：编译通过、233/233 补丁干净、`validate-repo` 通过、新串核对。
  - 下一次判读：
    - `rx fifo after clocking … wc N`（N>0）且 `rx fifo word i` 里能拼出 `20 00 00 32 65 03`
      → **改用 CPU 读 FIFO**，RX DMA 不要了；这也是最快能出可用驱动的路子；
    - `wc 0` → RX 从未进 FIFO，SE-DMA 的接收在这颗引擎上不可用 → 要么写 GPI/GSI DMA，
      要么让 SE 只负责 TX/CLK/CS、MISO 用 CPU 在慢速下采（只适合调试）；
    - `wc N` 但字里的数据是错的 → FIFO 收到的是别的东西，按字模板对照 packing 再定位。

- **2026-10-27 触摸第 212 步：touch164 回答"RX FIFO 有没有收到数据"——**收到 2 个字（8 字节），而且 CPU 能直接读出来**。所以坏的是 FIFO→DMA→内存 这一段，TX/移位/FIFO 都对；touch165 直接把 RX 改成 CPU 读 FIFO，等于给 SE-DMA 加了一条能用的接收通路**：
  - touch164 的日志（`164.txt`）关键行：
    ```
    rx fifo after clocking (no rx dma): stat 0x40000002 wc 2 (64 bits on the wire)
    rx fifo word 0 = 0x00000000
    rx fifo word 1 = 0x00000000
    wire readcmd rx 00 3f 20 00 00 32 65 03
    dma  readcmd rx 10 00 00 00 02 01 aa aa ee ee …
    ```
  - 读法：
    1. **RX FIFO 确实被写入**（`wc 2` = 8 字节 = 64 bit，与线上位数一致），
       而且**CPU 直接读 `SE_GENI_RX_FIFOn` 是通的**（读到两个字）。
    2. 这两个字是 `0x00000000`：这一次测试前面没有 page 写，面板对 addr 0x04 的
       默认页读很可能就是 0——**重要的是 `wc 2`，不是值**。
    3. 于是定位到：**移位→打包→RX FIFO 这一段是好的；坏的是 FIFO→DMA→内存**。
       `GENI_IF_DISABLE_RO=1` 禁的是 CPU 的 FIFO 通道？不——CPU 读 FIFO 明明可以。
       真正坏的是 RX DMA 引擎读 FIFO 的那一侧（TX DMA 读内存/写 FIFO 是好的）。
  - touch165（`xnu/patches/0235-liuqin-touch-dma-rx-via-fifo.patch`，3 hunk）：
    1. 新增 `tch_dma_fifo_xfer()`：**TX 仍走 SE 的 DMA**（线探针证明逐字节正确），
       **RX 不武装 DMA**，在命令运行期间用 `tch_drain_rx()` 从 RX FIFO 直接搬字节；
    2. 新全局 `tch_rx_via_fifo`；`tch_dma_xfer()` 在有 rx 且该标志为 1 时转走新函数；
    3. diag 里紧接 FIFO 测试做一次完整 trim id 读（TX-DMA + CPU RX FIFO），
       命中 `.. 65 03` 就把 `tch_rx_via_fifo` 打开并保留整场；不命中则关掉。
  - 产物：内核 `6eeedfd8…`、payload `5e023d96…`、`shim/out/shim-boot-touch165-rxviadio.img`
    （`e05062b4…`，`shim/out/shim-boot.img` = 这张）；启动行不变（567 B）。
    主机侧：编译通过、234/234 补丁干净、`validate-repo` 通过、新串核对。
  - 下一次判读：
    - `tx-dma + cpu rx fifo: 20 00 00 32 65 03 (MATCH - keeping it)` →
      **RX 修好了**。接着应该看到 `Novatek trim id 20 00 00 32 65 03`、
      `fw dl part 0 …` 正常写下去（写是 TX-only，本来就走 DMA）；
    - 不中但 `wc` 非零 → FIFO 数据在，但 CPU 读的时序/打包要和线上再对一次
      （下一步在同一个传输里同时打印 `wire rx` 与 FIFO 读出的字）；
    - `wc 0` → 与本次相反，说明 FIFO 写入依赖命令形式，需重测。

- **2026-10-27 触摸第 213 步：touch165 的 RX-FIFO 通路也返回全零——RX 从此不再是"配置问题"，touch166 一次把两种可能判死，并保证这一轮无论如何都能把固件下载和触摸帧跑起来**：
  - touch165 的日志（`165.txt`）关键行：
    ```
    rx fifo after clocking (no rx dma): stat 0x40000002 wc 2 (64 bits on the wire)
    rx fifo word 0 = 0x00000000
    rx fifo word 1 = 0x00000000
    tx-dma + cpu rx fifo: 00 00 00 00 00 00 (read failed)
    wire readcmd  rx 00 3f 20 00 00 32 65 03
    dma  readcmd  rx 10 00 00 00 02 01 aa aa ee ee …
    se read div 4095/512/128/32: 00 00 00 00 00 00 (read failed)
    desc-first duplex: 10 00 00 00 02 01 aa aa (no match)
    pad-path trim id 20 00 00 32 65 03 (panel alive)
    ```
  - 读法：
    1. **CPU 读 FIFO 也全是零**，和 RX DMA 的 `10 00 00 00 02 01 aa aa` 是两套
       不同的垃圾 → 不是"FIFO→DMA"单点故障，而是**引擎报告的接收字节本身就是零**。
    2. 同一时刻线探针在 gpio16 上明明有面板的 `00 3f 20 00 00 32 65 03`，
       而 SE 在 gpio17/18/19 上的 TX/CLK/CS 逐位正确 → **问题只在"哪根线是
       SE 的串行输入"这一侧**，或者是这颗 SE 的接收块没有开。
    3. 因而剩下两个可测的分支：(a) 双工 opcode 用错——Linux `spi-geni-qcom`
       构造的是 `SPI_TX_ONLY|SPI_RX_ONLY = 3`，而本驱动一直用 7；(b) 接收块被
       `SE_FIFO_IF_DISABLE`/`SE_IRQ_EN` 一类使能关掉（`SE_DMA_RX_IRQ_STAT` 一直
       是 0，而 `LEN_IN` 已经走了 8 字节，正好像 DMA_RX_IRQ_EN=0）。
  - touch166（`xnu/patches/0236-liuqin-touch-rx-route-test.patch`，7 hunk）：
    1. `tch_dma_mode()` 把 `SE_IRQ_EN` 从 0 改成 `0xffffffff` —— 与 Linux
       `geni_se_irq_clear()`/`geni_se_select_dma_mode()` 完全一致（它们只清
       `SE_GSI_EVENT_EN`，从不把 `SE_IRQ_EN` 清零）。这一位组里含
       `DMA_RX_IRQ_EN`，也就是引擎自己的接收事件使能。
    2. 新增 `tch_rx_route_test()`：A 用 opcode 7 与 3 各做一次真实 trim id 读；
       B 打开 `SE_SPI_LOOPBACK` 发 `de ad be ef` 收回环数据——**回环绕开 pad**，
       若回环能拿回自己的字节，就证明接收通路是好的、只是 pad 接错线；同时读
       `SE_DMA_RX_IRQ_STAT`/`LEN_IN`/FIFO 状态。
    3. `tch_rx_fifo_test()` 增加"读走前后字数"的对照：**字数不降 = CPU 根本
       没有 FIFO 通道**（此时上面那些 0 不是数据而是"No data"），字数降到 0 =
       那些 0 就是 FIFO 里的真实内容。
    4. **兜底**：三条路由只要有一条拿回 `.. 65 03` 就保留它；一条都不行，
       就在 `tch_dma_diag()` 之后把整条栈切到 pads（gpio16..19 回 GPIO，
       `tch.bitbang=true`、`tch.dma=0`）并**立即返回继续 bring-up**。
       读（trim id / 事件页 / 帧）走已验证的 bit-bang，写（固件下载）也走
       bit-bang —— 慢（240 KiB 约 8 s）但这一轮能真正走到触摸帧，而不再卡在
       一条收不到数据的接收通道上。
  - 产物：内核 `fd288980…`（17,036,304 B）、payload `0460e34b…`、
    `shim/out/shim-boot-touch166-rxconform.img`（`ad03ee98…`，32,014,336 B，
    `shim/out/shim-boot.img` = 这张）；启动行不变（567 B）。
    主机侧：编译通过（只有既有 warning）、235/235 补丁干净、`validate-repo` 通过、
    新串核对（`rx route A/B`、`rx fifo wc after draining`、`no engine receive route works`）。
  - 下一次判读：
    - `rx route A1 opcode 3: 20 00 00 32 65 03 (MATCH)` → **opcode 就是病根**，
      后面 bring-up 全走 SE，速度恢复正常；
    - `rx route B loopback … rx de ad be ef` 但 A 两条都不中 → 接收块是好的、
      **pad 接错/信号不是 gpio16**，下一步查 TLMM 里 SE4 的输入映射（而不是再改
      SE 寄存器）；
    - 两条 A 都不中且 B 也回零 → 这颗 SE 的接收块确实只能在 GSI/GPI 下工作，
      本轮已自动落到 pads 上继续跑；
    - `rx fifo wc after draining: 2` → CPU 没有 FIFO 通道，`wc` 再也不能当作
      "FIFO 收到数据"的证据（之前两步的推断要按这一条修正）；
    - 之后看 `whole stack on the pads`（若出现）→ `Novatek trim id` → `fw dl part 0/1/2/3`
      → `attempt N state now 0xa? running` → `fwinfo`；任一步出现 `MATCH`/`running`
      就说明触摸链路已经通了。

- **2026-10-27 触摸第 214 步：病根就是双工 opcode——3 收得到，7 收不到。touch167 把它定为默认，并把诊断走查从默认启动里摘掉，改成一条干净的 bring-up**：
  - touch166 的日志（`166.txt`）同一台机器、同一个时钟、同一块缓冲区，只换 opcode：
    ```
    rx route A0 opcode 7: 00 00 02 01 aa aa (no match)
    rx route A1 opcode 3: 20 00 00 32 65 03 (MATCH)
    rx route B loopback tx de ad be ef -> rx 00 00 00 00 (… opcode 7 跑的，等于没测)
    ```
    → **`SPI_TX_ONLY|SPI_RX_ONLY = 3`（Linux `spi-geni-qcom` 的写法）才是这颗 SE
    固件认可的双工命令**。7 能把字节逐位发出去（所以线探针一直是对的），但接收侧
    报垃圾——第二十轮之前所有"RX DMA / packing / pad"的怀疑全部由这一个字节解释：
    `M_CMD0 = (7 << 27) | FRAGMENTATION`。
    `SE_IRQ_EN=0xffffffff` 那一条（touch166 同轮）与结果无关，但保留（与 Linux 一致）。
  - 同轮还暴露了第二个问题：**走查本身会毁掉总线**。`trim id cmd-first div 32` 明明
    MATCH，紧接着 `tch_se_start()` 的 CS/TX 探针（`after-cmd 0x41`）之后，下一次读
    就变成 `Novatek trim id ff ff ff ff ff ff`，后面的 `fw state 0xfd`、
    `@0x11a00 fw 08 98 02 ce…`、`fw chunk @0x00000 write failed` 全是这一下的余波。
    走查还让启动多花约 78 s（`touch bus probe at 143.153 s`，引擎 65 s 就起步了）。
  - touch167（`xnu/patches/0237-liuqin-touch-duplex-opcode3.patch`，7 hunk）：
    1. **默认双工 opcode 改成 3**（`SPI_DUPLEX`）；新增 `liuqintouchopcode=7`
       供对照；把当初误导的注释（"3 不是合法 opcode"）就地改正。
    2. **`tch_dma_diag()` 默认不跑**，`liuqintouchdiag=1` 才跑；正常启动省掉 78 s，
       也不再让走查污染引擎/面板状态。
    3. 默认路径改成：`tch_dma_order=1`、`div 32`、`tch_irq_clear()`、`tch_nvt_bringup()`，
       然后**直接读 ROM trim id**，最多 3 次（每次中间再 bring-up 一次 + 20 ms）；
       命中的那一次打印 `SE4 receive is live (duplex opcode 3, div 32, 600 kHz)`。
       不再调用 CS/TX 探针（`tch_se_start()`）——它们就是 0x41 的来源。
    4. 三次都不中才落到 pads 兜底（`no engine receive route works …`），
       逻辑同 touch166，仍保证这一轮能继续 bring-up。
  - 产物：内核 `0746e1f6…`（17,036,304 B）、payload `cef9af6d…`、
    `shim/out/shim-boot-touch167-opcode3.img`（`9a53ecc0…`，32,014,336 B，
    `shim/out/shim-boot.img` = 这张）；启动行不变（567 B）。
    主机侧：编译通过（只有既有 warning）、236/236 补丁干净、`validate-repo` 通过、
    新串核对（`SE4 receive is live`、`engine bring-up`、`liuqintouchdiag`）。
  - 下一次判读：
    - `engine bring-up 1: 20 00 00 32 65 03 (panel answered, opcode 3)` →
      **SE 接收通了**。接着看 `fw dl bld: ilm des …`（这次应该是可读的固件描述符，
      不是 touch166 那种 `0x000000 / 0x01d1bb` 垃圾）、`fw dl part 0 … 4`、
      `attempt 1 state now 0xa? running`、`fwinfo … ver ok`；
    - `engine bring-up 1..3: … (no answer)` → 走查之外的东西也在破坏总线
      （`tch_nvt_bringup()` 的复位序列或 CS 命令），下一步把这三次的
      `irq/status` 一起打出来；
    - 出现 `no engine receive route works` → 说明干净状态下 3 也收不到，
      那才是"pad 接错线"，按 pads 路线先跑触摸。

- **2026-10-28 触摸第 215 步：opcode 3 在干净启动里第一次就拿到 `20 00 00 32 65 03`——接收侧确认修好；卡点变成"第一次写"，而且是被判据冤杀的：命令完成、`LEN_IN` 两个方向都走满，只有 DMA 状态位迟到**：
  - touch167 的日志（`167.txt`）关键行：
    ```
    DMA #8 issuing opcode 3, command before descriptor (the transfer starts here)
    DMA #8 came back: status 0x00000000 irq 0x08000081 tx 0x00000000 rx 0x00000000 len_in tx 8 rx 8 after 2 polls
    DMA failed: status 0x00000000 irq 0x08000081 tx 0x00000000 rx 0x00000000 …
    engine bring-up 1: 00 00 00 00 00 00 (no answer, opcode 3)
    engine bring-up 2: 20 00 00 32 65 03 (panel answered, opcode 3)
    SE4 receive is live (duplex opcode 3, div 32, 600 kHz)
    fw state 0xfc @0x125860 (not ready)
    @0x11a00 state d6 dd 77 a8 | fw 0c 98 02 cc 18 75 4c 96 7f
    Novatek trim id 20 00 00 32 65 03 (NT36532) irq 0x00000001
    fw dl: size 245760 end 0x200 cascade 1 info 14 parts 16 spidma 0
    fw dl bld: ilm des 0x000000 len 0x01d1bb | dlm des 0x100000 len 0x014107
    fw chunk @0x00000 write failed (try 1)
    fw chunk @0x00000 write failed (try 2)
    fw dl part 0 @0x00000 write failed after 0 chunks
    LIUQIN BOOT: touch fw dl start at 193.184 s (+150 ms) … touch fw FAIL at 193.392 s (+208 ms)
    ```
  - 读法：
    1. **接收侧确认修好**：opcode 3 下第一次读（`#8`）的 `LEN_IN` 已经是 `tx 8 rx 8`，
       第二次直接 `20 00 00 32 65 03`；随后 `fw state`、`@0x11a00`、trim id
       连读四次都正常。二十轮里对 RX DMA / packing / pad 的怀疑到此全部作废。
    2. `#8` 被判失败的原因不是数据没到，而是 **`TX/RX_DMA_DONE` 比 `M_CMD_DONE` 晚**：
       `irq 0x08000081`（`M_CMD_DONE` | `0x80` | `RX_FIFO_LAST`）无错误位、`LEN_IN` 满，
       而两个状态寄存器读 0。下一次命令立刻成功，说明字节确实走完了。
    3. 同一个判据把下载也判死了：`fw dl start` → `fw FAIL` 整轮 **208 ms**，
       扣掉 35 ms 的 SW 复位等待还剩约 170 ms——而一个 `0xFC00` chunk 在 600 kHz 下
       要走 **860 ms**，所以 chunk 0 两次重试都在前几 KB 就断了。
       **不是"写不进去"，是中途某一笔被判据不接受**：252 条命令里只要有一条碰上
       迟到的状态位，整块就废。
    4. `fw dl bit-bang pace 4 us` 是误导行：引擎是活的，下载走 DMA；这句无条件打印，
       让一轮 DMA 失败看起来像 pads 问题。
  - touch168（`xnu/patches/0238-liuqin-touch-dma-status-late.patch`，7 hunk）：
    1. `tch_dma_xfer()`：`M_CMD_DONE` 且无错误位、但 `TX/RX_DMA_DONE` 为 0 时，
       **再轮询 2 ms**；仍不出现就接受 **`M_CMD_DONE` + 两个 `LEN_IN` 都到长度**
       （TX 字节全部从内存取走、RX 字节全部写回内存），并打印用的是哪条判据。
       调用方本来就有回读与 CRC 兜底，误接受不会悄悄放过一张坏镜像。
    2. 失败打印不再被 `tch_dma_count <= 8` 挡住：最多 24 条，带
       `cmd/len/status/irq/tx/rx/len_in`；CS assert/deassert 失败也打 `irq/status`。
    3. 每次下载尝试把 `tch_dma_count` 归零——touch167 恰好死在下载头几次传输上，
       而计数早在 bring-up 时就超过 8，最该有日志的地方是空的。
    4. `fw chunk … write failed` 补上最后一次传输的 `cmd/len/irq`；新增 `fw dl path`
       明确写 `serial engine, DMA` / `FIFO` / `pads (bit-bang)`。
    5. 引擎路径一次发 `TCH_DMA_MAX`（16128 B）而不是 256 B：一个 0xFC00 chunk
       从 252 条命令降到 4 条，暴露面小六十倍。pads/FIFO 仍按 256。
  - 产物：内核 `98a60e2a…`（17,036,504 B）、payload `38477d88…`、
    `shim/out/shim-boot-touch168-dmastatus.img`（`1f5b7036…`，32,014,336 B，
    `shim/out/shim-boot.img` = 这张）；启动行不变（567 B）。
    主机侧：编译通过（只有既有 warning）、237/237 补丁干净（应用后
    `bsd/dev/liuqin_touch.c` 与构建树逐字节相同）、`validate-repo` 通过、新串核对。
  - 下一次判读：
    - `DMA #N accepted on command done + len_in …` 或 `status latched … after the command`
      → 判据成立，接着看 `fw dl part 0 … 4` 是否写下去、`attempt 1 state now 0xa? running`、
      `fwinfo … ver ok`；
    - `DMA #N failed: cmd … len … irq 0x…` → 第一次能看到下载内部真正的失败原因：
      `0x2` = M_CMD_OVERRUN（上一条没结束）、`0x4` = ILLEGAL_CMD、`0x8` = CMD_FAILURE、
      `0x08000000` = RX_FIFO_LAST（不是错误位）；
    - `CS assert failed irq 0x…` → CS 命令本身在双工之后失效，下一步给 CS 单独做复位序列；
    - 整轮仍停在 `write failed` 但 `last dma cmd 1 len 3` → 失败就在页写那 3 个字节，
      下一步直接对同一笔做两次连续重试并打印前后 `SE_GENI_STATUS`。

- **2026-10-28 触摸第 216 步：`168.txt` 与 `167.txt` 是同一份文件，这一轮没有新的硬件数据；给驱动加上开机自报 build stamp，以后一眼就能认出日志属于哪张镜像**：
  - 事实：用户附的 `168.txt`（381,522 B）与上一轮 `167.txt` 的 sha256 完全相同
    （`58af40c6e9b888505691121739886fe73fbe0d19e3d33fc20bde3947a4d46750`），
    内容与 touch167 那次启动逐字一致（`touch fw dl start at 193.184 s`、
    `attempt 1 state now 0xfc`、`fwinfo 6a 18 …` 全部相同）。
  - 四条互相独立的判据说明它只能由 **touch167 镜像**打印：
    1. `fw dl bit-bang pace 4 us (2 us was the corrupt one)`——touch168 把这句话
       放进了 `if (tch.bitbang)`，而该轮引擎 DMA 是活的，168 永远不会打；
    2. `fw chunk @0x00000 write failed (try 1)`——168 的格式是
       `write failed (try 1, last dma cmd N len N irq 0x…)`；
    3. 整轮没有 `fw dl path …`——168 每次下载尝试必打一行；
    4. `DMA #8 came back: … len_in tx 8 rx 8 after 2 polls`——同一情形 168 打的是
       `accepted on command done + len_in …`。
    镜像侧也核对过：`shim-boot-touch167-opcode3.img` 里 `fw dl path` /
    `accepted on command done` / `status latched` / `last dma cmd` 四个串一个都没有，
    `shim-boot-touch168-dmastatus.img` 四个全有。
  - 因此 touch168 的判据改动**仍未在真机上跑过**，结论保持待验证，不据此再改代码。
  - touch169（`xnu/patches/0239-liuqin-touch-build-stamp.patch`，2 hunk）=
    touch168 + 自报家门，功能零改动：
    `#define TCH_BUILD "touch169 dma-late-status+16k-chunks"`，
    在 probe 线程最前面、`SPI bring-up in %d s` 之前打印
    `LIUQIN TOUCH: driver touch169 …`。日志再被截断也知道是哪一次构建。
  - 产物：内核 `c62df06f1ac4c3135bcbd2e616a353746e47207ceba5be58b6b2baed5b41f2dd`
    （17,036,504 B）、payload `bfe93ed6a67bdf3af1c475a432c78fcbc480dd658d37335646ad4d9c8747ded1`、
    `shim/out/shim-boot-touch169-dmastatus.img`
    （`21e02090e68bb91d646419bc54b9806abbaef7196bcf1277e69df7300654c6cf`，
    32,014,336 B；`shim/out/shim-boot.img` = 这张）。启动行不变（567 B）。
    主机侧：编译通过、238/238 补丁干净（应用后 `bsd/dev/liuqin_touch.c`
    sha256 `611dcaa82723a21d5f4c73f80f7342828ff9ecfc84ce15272aa8f6b0f91012bc`
    与构建树逐字节相同）、`validate-repo` 通过、镜像里新串齐全。
  - 下一次判读（先看第一行）：
    - 没有 `LIUQIN TOUCH: driver touch169 …` → 又是旧镜像/旧日志，不看后面；
    - `DMA #N accepted on command done + len_in …` 或 `status latched … after the command`
      → 判据成立，接着看 `fw dl path serial engine, DMA`、`fw dl part 0 … 4`、
      `attempt 1 state now 0xa? running`、`fwinfo … ver ok`；
    - `DMA #N failed: cmd … len … irq 0x…` → 第一次能看到下载内部真正的失败原因：
      `0x2` = M_CMD_OVERRUN、`0x4` = ILLEGAL_CMD、`0x8` = CMD_FAILURE、
      `0x08000000` = RX_FIFO_LAST（不是错误位）；
    - 整轮仍停在 `write failed` 但 `last dma cmd 1 len 3` → 失败就在页写那 3 个字节，
      下一步对同一笔做两次连续重试并打印前后 `SE_GENI_STATUS`。

- **2026-10-28 触摸第 217 步：touch169 上机——DMA 路径、迟状态判据、16 KiB 分片全部生效；下载死在"等待时间没跟着长度走"：一个 16128 B 的命令在 600 kHz 下要 215 ms，而轮询只给 50 ms，引擎还在移数据就被判超时**：
  - `169.txt`（403,262 B，sha256 `dd420779…`）第一行 `LIUQIN TOUCH: driver touch169 …`
    确认跑的是新镜像。
  - 一次就过的部分：
    ```
    DMA #8 came back: status 0x0 irq 0x08000081 tx 0x00000003 rx 0x00000003 len_in tx 8 rx 8 after 3 polls
    engine bring-up 1: 20 00 00 32 65 03 (panel answered, opcode 3)
    SE4 receive is live (duplex opcode 3, div 32, 600 kHz)
    fw dl path serial engine, DMA
    DMA #29 status latched 1 polls after the command (tx 0x00000003 rx 0x0)
    ```
    这次 `TX/RX_DMA_DONE` 两个位都在（`tx 0x3 rx 0x3`），bring-up **第一次**就拿到 trim id；
    迟状态判据也在下载中命中过一次（`#29`）。
  - 真正的失败，三次尝试一模一样：
    ```
    DMA #27 failed: cmd 1 len 16128 status 0x00000041 irq 0x00000000 tx 0x0 rx 0x0
        len_in tx 5504 rx 0 after 1000 polls, dma_mode_en 0x1 general_cfg 0xf
    fw chunk @0x00000 write failed (try 1, last dma cmd 1 len 16128 irq 0x0)
    ```
    三次六个超时点的 `len_in tx`：5504 / 5792 / 5632 / 6208 / 5696 / 5792。
  - 算术（这就是全部原因）：
    - 16128 B × 8 bit ÷ 600 kHz = **215 ms**；
    - 轮询 `1000 × delay(50 µs)`，加上每轮 7 次寄存器读，实测 **≈70 µs/轮 ≈ 70–75 ms**；
    - 观测 `len_in` 5504–6208 B，正好是 600 kHz 下的 **73–83 ms**——**同一个截止时间**，
      抖动量与时钟一致。
    - `status 0x41` = bit0 `M_GENI_CMD_ACTIVE` + bit6：命令**还在跑**；`irq 0`：没有完成、
      也没有错误位。不是总线错，不是面板，是等得太短。
    - 整轮 211/219/218 ms ≈ 两次超时（2×73 ms）+ 页写与描述符的开销。
    - 反证：若真是"卡住"，六个点会停在同一个字节数；若是 DMA 长度被截断（如 13 位 = 7936），
      也会停在同一值。都不是。
  - touch170（`xnu/patches/0240-liuqin-touch-dma-wait-scaled.patch`，5 hunk）：
    1. **等待时间跟着长度走**：`budget = (len*8/spi_hz 的微秒数)/50 µs + 400`（20 ms 余量），
       下限 1000（小传输不变），上限 100000。16128 B 现在给 4700 轮 ≈ 235 ms 名义值
       （实测每轮 ~70 µs，约 330 ms）。
    2. **超时后取消命令**：`M_GENI_CMD_ACTIVE` 还立着时写 `SE_GENI_M_CMD_CTRL_REG` =
       `M_GENI_CMD_CANCEL`（bit2，Linux `geni_se_cancel_m_cmd` 的同一条路），轮询到 ACTIVE
       落下，再清 IRQ。以前超时后直接发下一笔，等于把重试叠在一条还活着的命令上。
    3. **进度三点采样**：在 25% / 50% / 75% 处记一次 `SE_DMA_TX_LEN_IN`，失败时打印
       `tx progress at 25/50/75% of the wait: a b c of len`——下一次如果还失败，
       一眼分开"太慢"（三点递增）与"卡死"（三点相同）。
    4. 失败行改成 `after %u of %u polls`，直接看出预算。
  - 产物：内核 `49274a29…`（17,036,536 B）、payload `e1418ccf…`、
    `shim/out/shim-boot-touch170-dmawait.img`（`233c68e4…`，32,014,336 B，
    `shim/out/shim-boot.img` = 这张；启动行不变，567 B）。主机侧：编译通过、
    239/239 补丁干净（应用后 `liuqin_touch.c` `f962727c…` 与构建树逐字节相同）、
    `validate-repo` 通过、镜像里新串齐全。
  - 下一次判读：
    - `fw dl part 0 … 15` 逐段出现、`crc …` 有值 → 下载真的通了，接着看
      `attempt 1 state now 0xa? running`、`fwinfo … ver ok`；
    - 若仍 `failed … after %u of %u polls` 但 `tx progress` 三点递增 → 等待还是不够或
      SPI 时钟比 600 kHz 慢，下一次按实测速率反推 budget；
    - 三点相同 → 真的卡住，那时才回到总线/SMMU（此时有 `status`/`len_in` 实证，不再靠猜）；
    - `cancel did not clear` → 取消序列无效，下一步改走 abort 并把 `SE_GENI_M_CMD_CTRL_REG`
      前后值打出来。

- **2026-10-28 触摸第 218 步：touch170 上机——固件下载真的通了，201410 B 与文件逐字节相同、`BOOTED`、`state 0xa3`、fwinfo 就是厂商验收值；但一帧接触都没有，报告区是 `fd`——IC 在 CRC-fail 重启循环里，而我们的"清 CRC 标志"一直写在 `0x00000`（ILM 第一个字节），标志根本没清**：
  - `170.txt`（382,967 B，sha256 `d3e7bbaa…`）第一行 `driver touch170 length-scaled-wait+cancel`，新镜像确认。
  - **DMA 侧全部达标**：
    ```
    fw dl part 0 @0x00000 len 0x1d1bc bin 0x200: 2 chunk 1615 ms
    fw dl part 1 @0x100000 len 0x14108 bin 0x22000: 2 chunk 1113 ms
    fw verify: image identical to the file (201410 bytes)
    fw dl BOOTED: 201924 B 6 chunks 3 s, state 0xa0@125860 1 polls
    attempt 1 state now 0xa3 (running)
    fwinfo 19 e6 26 3c 46 50 70 80 … (ver 0x19 ok, 38x60 sensors)
    ```
    `fw_ver 0x19 / ~0xe6 / x_num 38 / y_num 60 / 18000x28800` 与
    `docs/TOUCH_FW_RE.md` 里厂商验收值逐项相同；下载 201410 B 只花 3 s。
  - **新问题**：`0 contact frames`；`dump 125800: fd fd …`（三次 INT 都是）；
    `beat 12/24/36s: running, 937/1878/2814 event reads (0 failed), 0 contact frames`。
    这正是 NOTES 第 183 步记录过的死法：**IC 进入 CRC-fail 重启循环**，报告区恒为 `fd`，
    INT 变成 ~13 ms 的周期脉冲。
  - **找到的代码错误**：`revive` 与 `tch_nvt_bringup()` 里清 CRC 标志写的是
    `tch_nvt_write8(0x00000, 0xa5)`——`0x00000` 是 **ILM 的第一个字节**，不是标志。
    厂商 dmesg 明确 `crc_err_flag_addr=0x1fb535`（NT36523 的 `0x3f135` + `0x1c4400`），
    而且必须先把 MCU 打回 idle（`0xaa` 两次写 `swrst 0x1fb43e`）再写 `0xa5` 才生效，
    写后回读 `0xa5` 才算清掉。也就是说：**这个标志从某次失败的下载之后一直是置位的**，
    之后每次启动固件都掉回重启循环——和"183 步能摸、之后再也摸不到"完全吻合。
  - touch171（`xnu/patches/0241-liuqin-touch-crc-flag.patch`，7 hunk）：
    1. `tch_crc_flag(why, fix)`：读 `0x1fb535` 与 trim id；`fix` 时按厂商
       `nvt_stop_crc_reboot()` 的顺序（`0xaa`×2 → 1 ms → 写 `0xa5` → 回读，最多 5 次），
       打印 `crc flag 0x%02x -> 0x%02x in %u tries, trim …`；
    2. 调用点：每次下载尝试开始（`before download`）、下载/引导结束
       （`after boot`，只读不改，避免把刚起来的固件又打回 idle）、两条 revive 路径（`revive`）；
    3. 删掉两处写 `0x00000` 的假清标志。
  - 产物：内核 `542a9210…`（17,036,576 B）、payload `d0a4c95b…`、
    `shim/out/shim-boot-touch171-crcflag.img`（`84c03151…`，32,014,336 B，
    启动行不变 567 B）。
  - 下一次判读：
    - `before download: crc flag 0x?? -> 0xa5 in N tries` → 说明标志之前是置位的，
      这就是 0xFD 的根因；接着看 `after boot: crc flag 0xa5` 与
      `first frame 0a …`、`finger 1 ev …`、`touch frame 1`；
    - 若 `before download` 本来就是 `0xa5`（标志没置位），那 0xFD 另有原因，
      下一步做"INT 期间整块 0x125800–0x125880 转储 + 0x51 握手字节读写"；
    - `-> 0x00` 或非 `0xa5` → 写不进去，下一步改成先 `0xaa` 再 `0x35`（unlock）再 `0xa5`。

- **2026-10-28 触摸第 219 步：CRC 标志假设被证伪（下载前后都是 0x00）；但 171 的日志给出了新的确定事实——固件在开机后约 18 s 是健康空转的（`ff 00 00 00 00 00` 保持不变），第 ~81 s 才翻成 `fd`，也就是"停止扫描"发生在固定时刻；厂商 probe 在读完 fwinfo 后会清 `0x51` 握手字节，我们从来没清**：
  - `171.txt`（404,357 B，sha256 `cf2677e6…`）：
    ```
    driver touch171 crc-flag-cleared
    before download: crc flag 0x00 -> 0xa5 in 0 tries, trim 20 00 00 32
    after boot: crc flag 0x00, trim 20 00 00 32
    fw verify: image identical to the file (201410 bytes)
    fw dl BOOTED: 201924 B 6 chunks 3 s, state 0xa0@125860 1 polls
    attempt 1 state now 0xa3 (running)
    fwinfo 19 e6 26 3c … (ver 0x19 ok, 38x60 sensors)
    ```
    即"CRC 标志被锁存"这条**不成立**：下载前是 0x00（写 0xa5 能立住），引导后又是 0x00。
    touch171 的清标志代码没有害处，但不是根因。
  - **新的时间线**（这是本轮真正的收获）：
    - 63.2 s：`evt data ff 00 00 00 00 00 ff 00 (no record)`——这是"全部抬起"的空记录，
      固件**活着且在扫**；
    - 63.2 s → 81.1 s 共 18 s，报告区**一个字节都没变**（驱动只在变化时打印），
      期间 938→0 帧；
    - 81.1 s：`evt data fd fd …`，此后一直是 `fd`，INT 变成 ~13 ms 周期脉冲。
    - 170 那轮同样的翻转发生在 79.8 s，也是在引导后约 18 s。**固定延迟，说明是固件内部的
      看门狗，而不是总线**。
  - 对照厂商 `nt36xxx_ts_probe`：`nvt_get_fw_info()` 之后紧接着 `nvt_clear_fw_status()`——
    把 `EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE`（event+0x51 = `0x125851`）写 0 并回读确认。
    我们从来没做过这一步。固件设了握手字节、没人清，约 18 s 后自己进恢复循环，
    和"183 步能摸、之后每次都在十几秒内死掉"完全吻合。
  - touch172（`xnu/patches/0242-liuqin-touch-fw-status-clear.patch`，5 hunk）：
    1. `tch_clear_fw_status(why)`：读 `0x125851` → 写 0 → 回读（最多 20 次 × 10 ms），
       打印 `fw status 0x51 0x%02x -> 0x%02x in %u clears`；
    2. 引导成功（`ready`）后立刻清一次；
    3. 心跳行加上 `fw 0x51=0x%02x`，并且每次心跳发现非 0 就再清（最多 8 次）——
       这样既保持固件"有人应答"，又能从日志看出它什么时候又开始置位。
  - 产物：内核 `1bf5423c…`（17,036,648 B）、payload `5a4308e3…`、
    `shim/out/shim-boot-touch172-fwstatus.img`（`a2a848fa…`，32,014,336 B，
    启动行不变 567 B）。
  - 下一次判读：
    - `after boot: fw status 0x51 0x00 -> 0x00`，且心跳里 `fw 0x51=0x00` 一直保持、
      81 s 不再翻 `fd` → 固件被救活，此时**去摸屏幕**应出现 `first frame`、
      `finger 1 ev …`、`touch frame 1`；
    - `0x51` 一开始就是非 0（比如 0x01/0xA5）→ 说明固件在等握手，清除后应立刻开始上报；
    - 心跳里 0x51 又变非 0 且清不掉 → 固件在报错，下一步把 `0x51` 的值和
      `0x125800–0x125880` 整块一起打在心跳里。

- **2026-10-28 触摸第 220 步：触摸通了，多指也正常——根因是"读完 fwinfo 必须写一次 `0x125851`"，写什么不重要，那次写本身就是固件等的应答**：
  - `172.txt`（674,423 B，sha256 `c05c5ff1…`）关键行：
    ```
    driver touch172 fw-status-clear
    after boot: crc flag 0x00, trim 20 00 00 32
    after boot: fw status 0x51 0x00 -> 0x00 in 1 clears
    first frame 0a 2b 20 70 7f 08 ff ff ff ff
    finger 1 ev 2 raw 11040,28799 of 18000x28800
    HID: touch 0 down at 1104,2879 edge
    |backboardd| event 0: type 1 finger 0 0.614,1.000 -> dispatch ok
    ```
  - 稳定性：`beat 216s: running, 16842 event reads (0 failed), 3380 contact frames, fw 0x51=0x00`，
    整轮 **0 次 `fd`**、**0 次 revive**、backboardd `3464 samples, 182 ups, 0 dispatch failures`；
    多指 `finger 2` / `finger 3` 都有派发记录。
  - **机制要点（必须记住，别再"优化"掉）**：清除前后读回都是 `0x00`，
    也就是说固件不是等某个值被写成 0，而是**等这次写事务本身**。
    所以引导后那次 `tch_clear_fw_status()` 必须**无条件写一次**，
    不能因为"读出来已经是 0"就跳过——这正是 touch171 及之前一直缺的一步：
    固件在开机约 18 s 后没人应答，就自行进恢复循环（报告区整块变成 `fd`）。
  - 对照 171：171 的报告区在 63.2 s 是 `ff 00 00 00 00 00`（健康空转），81.1 s 变 `fd`；
    172 在 61 s 写了一次 `0x125851`，之后 **216 s 再没掉过**。
  - 已知好镜像（就是这一版，不必再刷新的）：
    内核 `1bf5423c…`（17,036,648 B）、payload `5a4308e3…`、
    `shim/out/shim-boot-touch172-fwstatus.img` = `a2a848fa…`（32,014,336 B）。
  - 仍然成立的历史结论：opcode 3 修好了接收；DMA 判据要接受"命令完成 + LEN_IN 走满"；
    等待时间必须随长度放大；引擎一次发 16112 B 时超时按 `len×8/spi_hz` 算。
  - 下一步（非阻塞）：SPI 仍是 600 kHz（固件下载 3 s、每帧 ~12 ms 轮询）；
    若要更快可以试 div 16，但**先别动**——现在的组合是已验证可用的。

- **2026-10-28 按键第 221 步：内核里第一次读 SPMI——电源键、音量键的读数通路写好了（touch173）**：
  - 硬件位置用 mainline DTS 逐条核对（`yzddmr6/linux-sm8450-liuqin`，`liuqin-6.17` 的
    `sm8475-xiaomi-liuqin.dts`，不是猜的）：
    - 电源 = PMK8350 PON（SID 0）`RT_STS` `0x1310` **bit 7**（GEN3 布局，
      `PON_GEN3_KPDPWR_N_SET`）；
    - 音量下 = 同一字节 **bit 6**（PON RESIN，DTS 里 `linux,code = <KEY_VOLUMEDOWN>`）；
    - 音量上 = PM8350 **GPIO6**（SID 1，`gpio-keys/key-volume-up`，ACTIVE_LOW），
      引脚基址 `0x8800 + 0x100*(n-1)` → **`0x8d00`**，`RT_STS` 在 +`0x10` → **`0x8d10`** bit0。
  - SPMI 通路（从本机 base DTB 解出的 `reg`/`reg-names`，与 Linux `spmi-pmic-arb.c` 对齐）：
    ```
    cfg 0x0c42d000/0x4000   core 0x0c400000/0x3000   chnls 0x0c500000/0x400000
    obsrvr 0x0c440000/0x80000 (读通道 = 0x8000*ee + 0x20*apid)   intr 0x0c4c0000/0x10000
    ```
    - APID 不假设等于 PPID：扫 `core + 0x0b00 + 4*apid`，`ppid = (reg >> 8) & 0x1fff`，
      `features(0x04) & 0x7ff` 是 APID 个数；`ppid = (sid << 8) | (addr >> 8)`。
    - 读命令 `CMD = (15 << 27) | ((addr & 0xff) << 4) | (len - 1)`，轮询 `STATUS` bit0=DONE、
      bit1..6=错；数据 `RDATA0/RDATA1`。ee=0 时 v7 与 v8 偏移相同，两种 arbiter 都适用。
    - **只读**：没有 PMIC 驱动就写 PMIC 寄存器有可能改到电源轨，按键不需要写。
  - 实现放在 `bsd/dev/liuqin_touch.c` 末尾（补丁序列里唯一完全同步的文件）：
    `liuqin_keys_thread` 20 ms 采一次、两次一致才认边沿（40 ms 去抖），边沿调
    `liuqin_touch_post(LIUQIN_BUTTON_*)`；`liuqinkeys=0` 可关。日志前缀 `LIUQIN KEYS:`。
  - 产物：内核 `9b00817e…`（17,036,784 B）、payload `f87e9222…`、
    `shim/out/shim-boot-touch173-keys.img`（`de164b8c…`，32,018,432 B，
    启动行不变 567 B）。补丁 `0243-liuqin-pmic-keys.patch`（4 hunk）。
  - 下一次判读：
    - `LIUQIN KEYS: spmi arbiter version 0x70000000, 512 apids` + 三条 `apid N`
      → arbiter 通了，接着看 `power reads 0x00`（松手时 bit7=0）；
    - 按电源键应出现 `LIUQIN KEYS: power down/up`，音量键同理；
    - `spmi arbiter version 0x00000000` → core 区映射/地址不对（先查 0xc400000 是否可达）；
    - 三条 `read failed` 且 status 有错误位 → APID 所有权不在 EE0，下一步读
      `intr` 区的 owner 表确认，而不是直接改写成 v8 偏移。

- **2026-10-28 按键第 222 步：touch173 把触摸弄没了，根因是按键线程的 SPMI 轮询/全表扫描——撤回成默认关闭（touch174）**：
  - `173.txt`（402,801 B，sha256 `ef1a3432…`）里的硬事实：
    ```
    LIUQIN KEYS: spmi arbiter version 0x70000000, 768 apids
    LIUQIN KEYS: power sid 0 ppid 0x013 apid -1
    LIUQIN KEYS: volume up sid 1 ppid 0x18d apid 651
    LIUQIN KEYS: volume up reads 0x00
    touch clocks+pins ready at 130.143 s        ← 172 是 47.353 s，慢了 83 s
    touch fw BOOTED at 143.628 s                ← 172 是 62.7 s
    after boot: fw status 0x51 0x00 -> 0x00 in 1 clears   ← 握手写了
    evt data fd fd … at 154.116 s               ← 但固件 10 s 后就死了，整轮 0 帧
    ```
  - 结论（观察，不是猜测）：
    1. **启动被拖慢 83 s**。172 与 173 唯一差别是按键代码，而按键代码在 probe 里做了
       **3 × 768 次 arbiter 映射表读**（`pmic_apid()` 每个键扫一遍）——平均每读 ~27 ms，
       合计约 80 s。也就是说这块 arbiter 的 MMIO 读非常慢，不能那样扫表。
    2. **触摸固件在报告线程起来 10 s 后进 `fd`**，整轮 0 帧；172（除按键外完全相同）
       216 s 一直正常。`0x51` 握手这次也写了，所以不是那个机制。
       现在把"轮询/扫表会打死触摸"记为**已观察到的相关性**，机制未查明。
    3. 顺带：`power`/`volume down` 的 PPID 0x013 在映射表里**找不到**（apid -1），
       而 `volume up` 的 0x18d 找到 apid 651——说明 PMK8350 那些外设要么不在 bus0 的
       映射表里、要么 PPID 不是 DTS 推出来的 0x13；这条**还没验证**。
  - touch174（`0243-liuqin-pmic-keys.patch` 重写）：
    - 按键功能**默认关闭**，`liuqinkeys=1` 才映射 arbiter、扫表、起轮询线程；
    - 默认启动路径与 touch172 **逐字节等价**（`liuqin_keys_start()` 里只有一次启动参数判断）；
    - 触摸逻辑一行没改。
  - 产物：内核 `eb544351…`（17,036,784 B）、payload `7ff71abe…`、
    `shim/out/shim-boot-touch174-keys-off.img`（`489ac590…`，32,018,432 B）。
  - 下一次判读：
    - **先确认触摸回来**（`first frame` / `finger 1` / `touch 0 down`）；
    - 触摸正常后，下一轮再单独开 `liuqinkeys=1` 测按键（那时才付 80 s 扫表的代价），
      并把扫表改成"只读前 64 项 + 打印命中"以定位 PMK8350 的真实 PPID；
    - 若想同时要按键，就得先把扫表代价降下来（分段读、缓存、或直接按 APID 命中判断）。

- **2026-10-28 触摸第 223 步：按键关掉后触摸仍然不好使——说明 172 的成功不是"清 0x51 就够"，而是另有条件；touch175 加"收到两次 fd 就先补一次 0x51 再决定要不要重下"**：
  - `keyoff.txt`（456,852 B，sha256 `d6ab68f0…`）：`driver touch174 keys-off`，
    启动恢复 47.0 s、`0x51` 握手照写、firmware `BOOTED` 60.5 s，
    60.997 s 首次读到 `ff 00 00 00 00 00 ff 00`（**健康空转**），
    **75.310 s 变 `fd`**（引导后 14 s），整轮 0 帧，revive 4 次也没救回来。
  - 与能用的 172 逐行对照：
    | | 172（能用） | 174（不能用） |
    | --- | --- | --- |
    | 首次读到健康空转 | 61.3 s | 61.0 s |
    | 变 `fd` | 从未（216 s 全程正常） | 75.3 s |
    | 首帧 | 83.472 s | 无 |
    | 0x51 握手 | 写了 | 写了 |
    两者内核里与触摸相关的代码**完全相同**（174 只多了一段不执行的按键代码）。
  - 因此记录的结论：
    1. `0x51` 一次写**不是充分条件**；172 的成功目前无法用代码差异解释。
    2. 观察到的差别只有用户行为：172 那次是"一上电就持续划屏"，173/174 是"等系统起来再看"。
       **这只是相关性，机制未证实**，下一轮要用同一条路径验证（同镜像 + 一开始就划）。
  - touch175（`0243` 再改写）：收到 3 次带 `fd` 的中断后，**先写一次 `0x125851` + 5 ms，
    再读一次报告区**；若恢复有效帧就跳过重下（打印 `firmware answered the 0x51 ack`），
    否则才走原来的 re-download。按键保持默认关闭。
  - 产物：内核 `db0f5e10…`（17,036,784 B）、payload `e4f11b8a…`、
    `shim/out/shim-boot-touch175-ackretry.img`（`e9c7ce1f…`，32,018,432 B）。
  - 下一次判读：
    - `firmware answered the 0x51 ack` → `0x51` 确实是它等的东西，只是要**周期性**给，
      下一步改成每 2 s 补一次；
    - 仍然 `fd` → 补 0x51 无效，那 172 的成功只与"上电即触摸"有关，下一步把
      **上电后前 20 s 一定有手指**作为唯一变量重跑；
    - 无论哪种，**先把触摸恢复到 172 的 216 s 稳定**再谈按键。

- **2026-10-28 触摸第 224 步：touch175 上机，用户确认"触摸好使"——当前已知好镜像是 touch175；但"到底是哪一项让它活的"还没定论，日志未拿到**：
  - 现象：用户口头确认可用，本轮没有附日志（因此**没有** `first frame`、帧数、
    是否出现 `firmware answered the 0x51 ack` 的记录，别把这些当成已验证）。
  - 与 172/174/175 的差异对照（用于下一轮定位，仍未证实）：
    | 镜像 | 按键 | fd 补 0x51 | 用户行为 | 结果 |
    | --- | --- | --- | --- | --- |
    | 172 | 无 | 无（只有引导后一次） | 上电即划屏 | 216 s 正常 |
    | 174 | 有但关闭 | 无 | 等系统起来 | 14 s 后 `fd`，0 帧 |
    | 175 | 有但关闭 | **有** | 上电即划屏（已要求） | **可用** |
    两个变量同时变了（补 0x51 + 上电即划屏），所以还不能归因。
  - 下一步优先级：
    1. **拿到 175 的日志**：看有没有 `firmware answered the 0x51 ack`、
       有没有出现过 `fd`、`beat` 里的帧数；这一条决定是"周期性补 0x51"
       还是"上电即触摸"才是关键。
    2. 若确认是 0x51：把它做成每 2 s 补一次，触摸就不再依赖用户何时上手。
    3. 若确认是"上电即触摸"：说明固件的扫描/基线要一次真实接触才启动，
       那就得在驱动里找等价的启动手段（vendor 的 resume 序列），而不是靠用户。
  - 按键（本会话最初的请求）仍未工作，且现在知道**这块 arbiter 的 MMIO 读极慢**：
    touch173 里 3 × 768 次映射表读让"touch clocks+pins ready"从 47 s 推迟到 130 s，
    合到**约 26 ms/次**。这个数字决定了按键不能按 20 ms 轮询（3 次读就是 78 ms/轮），
    下一步要么找到让 arbiter 读变快的前置条件（时钟/电源域），
    要么改成"一次读一个字（PON RT_STS 两个键共用）"，把每轮压到 1 次读。

- **2026-10-28 触摸第 225 步：touch175 上机——能用，但闲置约一分钟后又死；死的时间点全部落在"没人碰"的时段，而唯一只在闲置时做的动作是我们每 2 s 去拉 INT 引脚（touch176 关掉）**：
  - 用户口头确认：touch175 "触摸好使"，随后"闲置一分钟又死了"。没有附日志，
    所以**帧数、是否出现 `firmware answered the 0x51 ack` 都没有记录**，不当成已验证。
  - 现有全部"死亡时刻"都对应闲置：
    | 镜像 | 最后接触 | 变 `fd` | 闲置多久 |
    | --- | --- | --- | --- |
    | touch173 | 无（起机就闲置） | 引导后 10 s | 10 s |
    | touch174 | 无 | 引导后 14 s | 14 s |
    | touch175 | 用户划了约一分钟 | 停手后约 1 min | ~60 s |
    | touch172 | 全程在划 | 从未 | — |
  - 闲置时本驱动额外做的只有两件事，都是我们自己发明的、厂商驱动没有的：
    1. `liuqinkeepalive=2000`（启动行里一直有）→ 每 2 s 把 gpio21 拉低 200 µs 再放回输入；
    2. 心跳里"静默 5 s 就 poke 一次 INT"（`tch_int_wake()`，打印
       `6 s without a frame, poking INT to wake the panel`）。
    两者都会去驱动控制器的 **INT 输出脚**。触摸时 gpio21 常为高，poke 被跳过——
    这与"一直划就活着、一停就死"的现象一致。
  - touch176（`0243` 再改写，9 hunk）：两条 poke 路径默认**全部关闭**，
    需要 `liuqintouchwake=1` 才启用；日志会打印
    `N s without a frame, INT poke disabled (liuqintouchwake=1 enables it)` 说明它在静默。
    另外 fd 判定行加上"到目前为止多少有效帧"。
  - 产物：内核 `a3be433f…`（17,036,816 B）、payload `a72a27c0…`、
    `shim/out/shim-boot-touch176-nopoke.img`（`f27e5d01…`，32,018,432 B）。
  - 下一次判读：
    - 闲置 5 分钟不再变 `fd` → poke 就是杀手，问题结束（`liuqintouchwake` 保持默认关）；
    - 仍然 `fd`，但日志里能看到 `INT poke disabled` → poke 无罪，下一个嫌疑是
      13 ms 轮询本身（届时把闲置读降到 100 ms 再试）；
    - 只要不再死，就可以在这个基线上重新做按键（按键必须改成每次 1 次 SPMI 读，
      而且要避开 26 ms/次的读代价）。

- **2026-10-28 触摸第 226 步：touch176 上机——这次是"正在划"的时候死的，死前最后一帧停在按下状态；INT poke 的假设作废；touch177 至少保证不再留一个卡住的手指**：
  - 用户描述（无日志）：touch176 正常使用中突然失效，失效前最后状态是"保持按下"。
    这直接否定了上一条的 INT poke 假设（touch176 默认已经完全不 poke）。
  - 由此更新事实表：
    | 镜像 | 是否 poke INT | 死亡时在做什么 | 死亡 |
    | --- | --- | --- | --- |
    | 173/174 | 是 | 闲置 | 10–14 s |
    | 175 | 是 | 先是划动，停手后约 1 min | 停手后 |
    | 176 | **否** | **正在划动** | 划动中 |
    | 172 | 是 | 全程划动 | 216 s 未死 |
    结论：**poke 不是必要条件**；死亡与"闲置/活动"也没有稳定关系。
    唯一稳定的是：固件会停，而且停了以后不会自己回来。
  - touch177（`0243` 再改写，11 hunk）：`tch_fw_dead` 置位时**立刻**给所有按下的槽位
    发 `LIUQIN_TOUCH_UP` 并打印 `firmware stopped mid-gesture, releasing every held
    finger`，不再等 6 个空帧。触摸路径其余不变；poke 仍默认关。
  - 产物：内核 `347e569d…`（17,036,816 B）、payload `d08f7e0a…`、
    `shim/out/shim-boot-touch177-release.img`（`77e67a41…`，32,018,432 B）。
  - 下一步：**要日志**（用户问要不要抓，答：要）。需要看到的是
    fd 出现前后 60 s 的完整记录：`beat` 帧数、`evt data`、有没有
    `firmware answered the 0x51 ack`、`releasing every held finger`、以及 revive 之后
    是否恢复出帧。没有这些就只能继续猜。

- **2026-10-28 触摸第 227 步：176 的日志到手——这次不是固件崩，而是**扫描停了**（没有 `fd`、state 一直是 0xa3、0x51 一直是 0）；更要命的是**两条 revive 路径根本没重下固件**，只写了个 0x69 就读回信息，日志里 reset 之后立刻还是 `state 0xa3 (running)`**：
  - `176.txt`（531,622 B，sha256 `1f60ef07…`），`driver touch176 no-int-poke`：
    ```
    first frame 0a 24 e0 49 48 05 12 15 7c 4c
    beat 30s: 2332 reads,  5031 contact frames
    beat 60s: 4666 reads,  5699 contact frames
    beat 90s: 6993 reads,  5967 contact frames     ← 到此为止，之后再也不涨
    5 s without a frame, INT poke disabled (liuqintouchwake=1 enables it)
    revive 1: no contact frame for 30 s, re-downloading the firmware
    revive: crc flag 0x00 -> 0xa5 in 0 tries, trim 20 00 00 32
    fw state 0xa3 @0x125860 (running)              ← "重下"之后仍然 running，说明什么都没做
    … revive 2/3/4/5 同样
    beat 210s: 16338 reads, 5967 contact frames    ← 一整轮 0 新帧
    ```
  - 这次**没有** `evt data fd …`：报告区不是 `fd`，`0x51` 全程 0x00。
    所以 failure mode 是"控制器活着但不再扫描/上报"，不是 CRC 重启循环。
    touch176 已经完全不 poke INT，所以 INT poke 也不是它。
  - **代码 bug（本轮重点）**：`tch_fw_dead` 分支和"30 s 没帧"分支里的恢复动作都是
    `crc_flag → delay → 写 0x69 → delay → tch_novatek_info()`，**从未调用
    `tch_fw_download()`**。日志正好证明：0x69 写完，下一次读状态仍是 `0xa3 running`，
    也就是既没复位也没重下，面板当然继续不报点。也就是说从 touch142 起，
    所有"revive"都是空转。
  - touch178（`xnu/patches/0243-liuqin-touch-recovery-and-keys.patch`，13 hunk，
    已把原 `0243-liuqin-pmic-keys.patch` 改名合并）：
    1. 两条 revive 路径改成真正调用 `tch_fw_download()`（复位 + 写描述符 + 全量下载 +
       引导 + `0x51` 应答），再读 fwinfo；
    2. 保留 touch177 的"固件停摆立刻释放所有手指"；
    3. 保留 touch176 的 INT poke 默认关闭。
  - 产物：内核 `97f52fd8…`（17,036,848 B）、payload `f3479321…`、
    `shim/out/shim-boot-touch178-revive.img`（`1e93591b…`，32,018,432 B）。
  - 下一次判读：
    - `revive 1: … re-downloading` 之后出现 `fw dl part 0 …` / `BOOTED` → 恢复真的在做了；
    - 若重下之后帧恢复 → 问题只剩"为什么扫描会停"，下一步盯 `beat` 里 0x51/state 的变化；
    - 若重下之后仍 0 帧 → 说明是面板/模拟前端侧的问题（fw 本身能跑），
      下一步在同一时刻读 vendor 的 FW history（`MMAP_HISTORY_EVENT0/1`）定位停因。

- **2026-10-28 触摸第 228 步：touch178 上机——用户确认"这次触摸一直好用"；当前已知好镜像换成 touch178，但"是没停过还是停了又被救回来"还没有日志佐证**：
  - 用户口头确认（无日志）。按流程记录为**观察**，不是已验证的指标：
    没有帧数、没有 revive 记录、没有 `first frame`，所以不能据此说"恢复链路已验证"。
  - touch178 相对 touch177 只有一处功能改动：两条 revive 真正调用 `tch_fw_download()`
    （touch177 之前它们是空转，176 的日志已证）。若这一轮确实发生过停摆，
    那么恢复第一次真正跑起来就是这一版；若根本没停，这版与 177 的差别没被触发。
    两种情况都符合"好用"，需要下一份日志才能分清。
  - 已知好镜像（当前）：
    ```
    shim/out/shim-boot-touch178-revive.img   1e93591ba280142b19112addf5ba77046fc77f87bb472eebb77bb3daa4e87a0a
    payload                                  f34793212d07b1682470b9be02463e8f8826aa81e821cf8a90609ead1b441e9c
    kernel (17,036,848 B)                    97f52fd8b90294154af9e9907d2120265b9fc71731f62647e9e02ba2cd5ad31a
    ```
  - 待办（按顺序）：
    1. 拿到一份"好用"的日志：看 `beat` 的帧数增长、有没有 `revive`、
       以及 revive 后有没有 `fw dl part 0 …` —— 这决定"恢复链路"是否真的算验证过；
    2. 按键（本会话最初的请求）：必须在**不碰触摸启动路径**的前提下做——
       线程延后到引导后 ≥120 s、映射表扫描设上限、每次只读 1 字节（PON 两个键共用 0x1310），
       并且先把"arbiter 一次读到底多慢"量出来（touch173 里推测约 26 ms/次）。

- **2026-10-28 触摸第 229 步：按用户要求先不动按键，去查"扫描为什么会停"；touch179 只加观测、不改行为**：
  - 现有证据（176.txt）能排除的：
    - 不是 CRC 重启循环（全程没有 `evt data fd …`，`0x51` 一直 0x00）；
    - 不是 state 掉（`fw state 0xa3` 一直 running）；
    - 不是 INT poke（touch176 已经默认完全不 poke，照样停）；
    - 不是"没人碰"（这次是划动中停的）；
    - INT 引脚本来就几乎不动作（整轮只有 5 次边沿，靠轮询出帧）。
  - 停之前那几帧很像**读撞上写**：`touch 1 down at 1793,318`、`touch 2/3` 同时出现，
    坐标乱跳，然后四指一起 `up at 0,0`，`backboardd released stuck fingers`，
    之后彻底没有新帧。这条只是**观察**，尚未证实。
  - 无法用日志区分的两个可能：
    1. 固件停止写报告区（buffer 冻结）；
    2. 我们每 10 ms 读一次、撞上固件写（buffer 一直在变但永远解不出有效帧）。
  - touch179 的观测（行为零改动，触摸路径与 178 相同）：
    - 每个 tick 比较报告区前 65 字节与上一次，统计**内容变化次数** `changes`；
      心跳行变成
      `beat Ns: … , R event reads (F failed, C changes), V contact frames, fw 0x51=0xNN, buf xx xx xx xx`；
    - 心跳里的 `buf` 是当前报告区头 4 字节，能直接看出"冻结在 idle 模式"还是"在乱跳"；
    - 这两项就能把上面两个可能分开：停止后 `changes` 不再增长 = 固件不写了；
      继续增长但 frames 不涨 = 我们在撞它的写。
  - 产物：内核 `5b51c783…`（17,036,912 B）、payload `1200168a…`、
    `shim/out/shim-boot-touch179-bufwatch.img`（`11c91001…`，32,018,432 B）。
  - 下一次判读：只要再停一次，看心跳行里的 `changes` 与 `buf`：
    - `changes` 停住、`buf` = `ff 00 00 00` → 固件停止写（往固件内部状态查，考虑加大两次读之间的间隔）；
    - `changes` 继续涨、`buf` 乱跳 → 读撞写（改成只在 INT 有效或低频读）。

- **2026-10-28 触摸第 230 步：178 的日志（1,016,547 B）——恢复确实在跑（每次都真的重下并 `BOOTED`），但**停摆会反复发生，而且重下之后常常不再出帧**；固件在持续使用中每 10–60 s 崩一次**：
  - `178.txt`（sha256 `69fe1a87…`）时间线：
    ```
    beat  30s: 2338 reads,  446 frames     ← 正常使用
    beat 180s: 14033 reads, 1985 frames
    firmware stopped mid-gesture, releasing every held finger
    revive 1: … → fw dl part 0 … → fw dl BOOTED (真重下了)
    beat 210s: 2033 frames     ← 恢复了 48 帧
    beat 240s: 2069 frames     ← 之后不再增长
    beat 270s: 2069 frames
    firmware stopped mid-gesture, releasing every held finger
    revive 2 → 下载成功 → 270s 起 0 新帧
    … revive 3/4/5 同样是"下载成功但不出帧"，其中两次在 10 s 内又"stopped mid-gesture"
    ```
  - 结论（观察）：
    1. **恢复链路本身是通的**：`fw dl part 0 … BOOTED` 每次都完成，`state 0xa3`；
    2. 但**重下不等于恢复上报**：多数 revive 之后帧计数不再增长；
    3. 首次停摆发生在约 200 s、1985 帧之后，是"用着用着崩"，与闲置无关，
       与 INT poke 无关（这版默认不 poke）。
  - 推测（未证实）：0x69 复位对**正在运行**的固件并不能把它留在 bootloader
    （176 的日志里复位后立刻又是 `state 0xa3`），于是下载写进了还在运行的固件，
    下一次崩溃就会提前到来。这条要靠 touch180 的"重下后立刻读报告区"来分辨。
  - touch180（在 179 的观测之上再加一条）：每次 revive 的下载完成后立刻打印
    `after the reload the report area reads xx xx xx xx, 0x51 0xNN, state running/not ready`，
    并保留 `beat` 里的 `changes`/`buf`。行为零改动。
  - 产物：内核 `78d16adb…`（17,036,912 B）、payload `cbdf4dca…`、
    `shim/out/shim-boot-touch180-watch.img`（`ab05857d…`，32,018,432 B）。
    242/242 补丁干净、应用后 `liuqin_touch.c` `3beffe8b…` 与构建树逐字节相同、`validate-repo` 通过。
  - 下一次判读：
    - 重下后 `report area` = `ff 00 00 00`（空转）→ 固件在跑但不扫描 → 查固件自身状态；
    - 重下后 = `fd fd fd fd` → 重下没把固件真正重启 → 复位序列要改（先 idle 0xaa 再 0x69）；
    - 停机后 `changes` 冻结/继续增长 → 分别对应"固件不写"和"我们撞它的写"。

- **2026-10-28 触摸第 231 步：180 的日志（642,290 B）定位了"7–8 分钟必死"的真正机制——**不是固件撑不住 8 分钟，而是 revive 预算只有 10 次，用完以后就再也不救了**；touch181 把预算改成"只限制连续失败"，出过一帧就恢复预算**：
  - `180.txt`（sha256 `391a60ae…`）关键序列：
    ```
    beat  30s: … 67 frames, buf ff ff ff ff
    beat  60s: … 276 frames, buf fd fd fd fd      ← 第一次死
    revive 1 → after the reload … ff 00 00 00, state running   ← 重下成功、固件在跑
    beat  90s: 310 frames
    revive 2 → ff 00 00 00; beat 120s: 310 frames（没涨）
    … revive 3..7 每次都是 ff 00 00 00 / running
    beat 180s: 335   beat 210s: 362   beat 240s: 362
    beat 270s: 395   beat 300s: 395   beat 330s: 461
    beat 360/390/420s: 461, buf fd fd fd fd     ← 之后 revive 预算耗尽，永久死
    ```
  - 三条硬结论：
    1. **重下后的固件是"活着但空转"**：`after the reload … ff 00 00 00, state running`，
       没有 `fd`、状态正常——也就是说重下之后它并不进入坏状态，只是**不扫描**；
    2. **重下有时能恢复上报**：帧数在多次 revive 之间从 310 涨到 461，
       说明恢复不是完全无效，只是大多数时候不奏效；
    3. **"7–8 分钟炸掉"= revive 用完了**：10 次 × (3 s 下载 + 30 s 等待) ≈ 5–7 分钟，
       之后 `tch_fw_revive < 10` 一直为假，面板整轮不再被救。
  - touch181（`0243-liuqin-touch-recovery-and-keys.patch` 扩到 20 hunk）：
    1. `tch_fw_revive` 上限从 10 改成 `TCH_REVIVE_MAX`（100000）；
    2. **只要解出一帧有效接触就把计数清零**，所以它只限制"连续失败"，
       不会再出现"用过 10 次就永久放弃"；
    3. 保留 touch180 的 `after the reload` 探针和心跳里的 `changes`/`buf`。
  - 产物：内核 `30fb9db7…`（17,036,912 B）、payload `2d0d369b…`、
    `shim/out/shim-boot-touch181-endless.img`（`5a27d487…`，32,018,432 B）。
    242/242 干净、应用后 `liuqin_touch.c` `feef8ab6…` 与构建树逐字节相同。
  - 下一次判读：
    - 8–10 分钟后触摸还能用（哪怕中间有几次 3 s 的重下停顿）→ "永久死"这一条修掉了；
    - `after the reload` 一直是 `ff 00 00 00` 却长期不出帧 → 重下后固件不扫描，
      下一步要么在重下后补一次 `tch_nvt_bringup()`（含 0x35 unlock），
      要么把 SPI 时钟从 600 kHz 提到 2.4 MHz（每次读从 0.87 ms 降到 0.22 ms，
      更接近厂商 9.6 MHz 的行为）。

- **2026-10-28 触摸第 232 步：181 上机——触摸侧比 180 好（3 次 revive 全部成功、290 帧、预算不再用光），但整机在约 443 s（≈7 分钟）黑屏，日志里没有 panic**：
  - `181.txt`（590,040 B，sha256 `8b94db6b…`）事实：
    ```
    beat 150s: … 72 frames    beat 180s: … 98    beat 210s: 135
    beat 240s: 135            beat 270s: 197 (buf fd)   beat 300s: 227 (buf fd)
    beat 330s: 290            beat 360s: 290
    revive 1 → after the reload … ff 00 00 00, state running   （3 次都是这样）
    ```
    即 revive 一直在工作（`after the reload` 三次全是 `ff 00 00 00` + running），
    帧数也还能涨；**revive 预算这条修好了**。
  - 但整机在 443 s 之后没有新日志：
    - **没有 panic**、没有 KASAN、没有 `SMP` 死锁采样（3 次 SMP 采样都是 backboardd
      的正常等待栈，是周期采样器，不是 panic）；
    - 结束前 30 s 有明显的整机病态：`exec xpcproxy / fairplayd.A2 / bluetoothd`
      每约 10 s 循环一次（说明有进程在崩了又被拉起），
      还有 `exec xpcproxy at 432.321 s (+6314 ms)`、`442.368 s (+9241 ms)` 这种
      6–9 秒的整机停顿，且都紧跟在一次触摸重下之后。
  - 推断（未证实）：重下固件期间（约 3 s 全量下载 + 0.5 s 整幅回读，期间 `delay()`
    忙等、SPI 引擎被占）会让整机停顿数秒，进程看门狗因此杀进程；
    连续几次之后系统进入黑屏/挂死。touch181 把 revive 改成无限次，恰好把这个
    停顿放大了。
  - 待确认（阻塞下一个方向）：黑屏之后设备是**还能被电源键唤醒**（那就是显示/休眠问题），
    还是必须长按强制重启（那就是整机挂死）。这两条要走的修法完全不同。

- **2026-10-28 触摸第 233 步：黑屏无法用电源键验证（按键还没修好），改为把触摸驱动的稳态与重下负载都压下来（touch182）**：
  - 用户反馈：黑屏后**无法判断**是挂死还是显示休眠，因为电源键没有驱动。
  - 从日志能确定的只有一点：**整份日志在 443 s 处彻底停止**（没有任何线程再打印，
    包括每 5 s 一次的心跳），这与"单是显示休眠"不符，更像整机停住；
    但**没有 panic**，所以不是内核主动崩。
  - 黑屏前 30 s 的系统病态（观察）：
    - `exec xpcproxy / fairplayd.A2 / bluetoothd` 每约 10 s 循环一轮（进程反复重启）；
    - 两次 6.3 s / 9.2 s 的整机停顿，都紧跟在一次触摸固件重下之后。
  - touch182 把触摸侧负载降下来（逻辑不变，只降频率/省步骤）：
    1. 轮询周期 **10 ms → 20 ms**（SPI 流量与 CPU 减半，仍 50 Hz 采样）；
    2. revive 上限 100000 → **30 次连续失败**（不再是无限，避免一直重下）；
    3. **重下时跳过整幅回读验证**（那一步要读 200 KB，约 0.5 s SPI，
       只在首次下载有意义），重下总时长明显变短。
  - 产物：内核 `b2ede353…`（17,036,944 B）、payload `4416f84a…`、
    `shim/out/shim-boot-touch182-light.img`（`1420cb34…`，32,018,432 B）。
    242/242 干净、应用后 `liuqin_touch.c` `21c5b7ae…` 与构建树逐字节相同。
  - 下一步：
    - 若这一版不再黑屏且触摸可用 → 说明是重下/轮询负载把整机拖垮的，问题收敛；
    - 若仍黑屏 → 需要外部手段判断存活（例如 USB 日志/串口），
      或先做一个不依赖电源键的存活指示（屏幕内容/心跳写盘）。

- **2026-10-28 触摸第 234 步：按用户要求停在 touch181、把 182 回退掉，并为"下一步修按键"写好交接（`docs/KEYS_HANDOFF.md`）**：
  - 回退内容（touch182 → 181）：
    1. 轮询周期 `hz/50`（20 ms）→ 回到 `hz/100`（10 ms）；
    2. `TCH_REVIVE_MAX` 30 → 100000；
    3. 去掉 `tch_fw_reloading` 标志与"重下时跳过整幅回读"的分支。
  - **回退后重新构建的内核与 payload 与 181 镜像逐字节相同**：
    内核 `30fb9db7e7e2bbeb08b6e4f61cb1114408143554d523f15d1ad650b6104d0ca7`（17,036,912 B）、
    payload `2d0d369bb99cf41bbb555a35d48e6a340def084a8e81ce0de8d3f0ddb1637d84`、
    镜像 `shim/out/shim-boot-touch181-endless.img`
    `5a27d4879860c794130906a817bc00e4e04f439103956dd4f83fe3ce2fd66312`。
    补丁 `0243-liuqin-touch-recovery-and-keys.patch`（20 hunk，
    sha256 `5ce8a9c39dd1262f485e73858825824352a2bde20e2d2acf79a974947d1a2d97`），
    242/242 干净，应用后 `liuqin_touch.c` `feef8ab6…` 与构建树逐字节相同，`validate-repo` 通过。
  - **交接文档 `docs/KEYS_HANDOFF.md`** 覆盖：当前镜像与哈希、按键硬件地址（PON `0x1310`
    bit7/bit6、PM8350 GPIO6 `0x8d10`）、SPMI arbiter v7 的 reg 区与读命令/APID 规则、
    touch173 里 `ppid 0x013 → apid -1` 与 26 ms/次读的实测、代码位置、
    下一步的五步方案（先量读耗时 → 假设 APID=ppid → 只在 120 s 后扫表 → 查第二条总线 →
    50 ms 轮询 2 字节）、六个坑、构建/打包/验证命令、上机判读表。
  - 本会话到此收尾：触摸停在"能用但不稳、崩了自动重下"的 181；按键未开始（默认关）。

- **2026-10-28 按键第 235 步：找到 `ppid 0x013` 找不到的根因（v7 映射表偏移与 PPID 掩码都错了）；做 touch183 诊断镜像，等 183.txt**：
  - 对照 Linux `drivers/spmi/spmi-pmic-arb.c`（master）逐行核对 v7 分支，发现 touch173 的
    `pmic_apid()` 有两处硬错误（**源码事实，不是上机观察**）：
    1. v7 的 APID→PPID 表在 `core + 0x2000 + 4*apid`
       （`pmic_arb_apid_map_offset_v7()`），不是 `0x0b00`；`0x0b00` 是未实现窗口，
       这也很可能是"一次表读约 26 ms"的来源（**待 183 的计时证实**）；
    2. 表项 PPID 只有 12 位（`PMIC_ARB_PPID_MASK = 0xfff`），旧代码用 `0x1fff`，
       把保留的 bit 20 折进了比较，即使偏移对了也可能匹配不上。
  - 另外旧 `pmic_read()` 的 4000 次循环没有墙钟上限：如果状态寄存器本身要 26 ms，
    一次坏 APID 的读会卡 100 s 以上。新代码用 20 ms 墙钟超时。
  - touch183（`0243` 重写，20 hunk）只改按键段和构建标记，触摸的初始化/下载/轮询一行没动：
    - 线程启动后 **先睡 120 s** 再碰 SPMI，避免重演 touch173 把触摸 bring-up 推迟 83 s；
    - 先按 handoff 的猜测直接读 `APID 19 / 0x1310`，并打印耗时；
    - 再读 `features[0]/[1]`、计时第一次表读，然后**扫完整的 bus0 表**
      （慢的话每 64 项让出 CPU 并打印进度；找到两个 PPID 就提前停）；
    - bus0 没有就扫 bus1（v7 APID 空间从 bus0 的 count 开始）；
    - 找到后打印 owner EE，读一次 PON/音量上原始字节，然后 **50 ms 轮询**，
      两次一致去抖；PON bit7=电源、bit6=音量下，PM8350 GPIO6 `0x8d10` bit0
      按 DTS 的 `GPIO_ACTIVE_LOW` 解释（0=按下）；
    - **这个诊断镜像默认开按键**（`liuqinkeys=0` 可关），因为 567 B 启动行不能改；
      按键验证完后要把默认改回关闭。
  - 产物（都已验证）：
    内核 `a291772a118aaba57f3c082848cfb355cd985889ceee1733ebe6cc8b4b7f8e05`（17,037,056 B）；
    payload `7aacb1ff8ad862d733eb1f54bc12048a190fc4e0031ae32fde100f3a5b99444e`（25,440,560 B）；
    镜像 `shim/out/shim-boot-touch183-keys-spmi.img`
    `e6d31a50b33d065f06fd2f1a08e96fd30e9ca7b1ee3f6794228c7508b59f4c03`（32,018,432 B）。
    补丁 `0243` 20 hunk，sha256 `d6223841f464c1fbf857a77ef320d5a1f0c3ed3c0d470d615c0d6221252541a2`；
    `verify-patches` all 242 clean，应用后 `liuqin_touch.c`
    `640c69e5b8847d24284f757b7e3379dabed9d847c30587a530517c5b537efab2` 与构建树逐字节相同；
    `validate-repo` PASSED；镜像里的启动行 567 B 与 touch167 抓回的逐字节相同。
  - 下一次判读（183.txt）：
    - `first map read ... ns`：如果是几十/几百 ns → 26 ms 是旧偏移造成的；如果仍 ~26 ms，
      → 这块 core 的 MMIO 就是慢，但线程已让出 CPU，扫描仍会完成；
    - `bus0 scan ...` + `power/volume down ppid 0x013 -> apid N` → 映射表就是答案；
    - `direct apid 19 ... -> 0x??` 如果 APID 19 正好是 PON，连扫描都不用；
    - `pon reads 0x00`（松手）→ 按电源键应出现 `power down/up`；
      音量下是同一字节 bit6；音量上 `0x01`=松手、`0x00`=按下；
    - 如果仍是 `ppid 0x013 -> -1`，看 `features ... bus1 N` 与 `bus1 scan` 的结果，
      必要时下一步改查 `intr`/owner 或确认 PON 的 PPID 不是 0x13。
  - 注意：这一版是**按键诊断镜像**，不是最终默认配置；确认按键通路后再把默认关闭改回来。

- **2026-10-28 GPU 第 236 步：按键确认可用；GPU 一直没起来是启动行少了一个 `d`**：
  - 用户反馈按键已好（真机观察；这次没有新的日志文件入库）。
  - 从 `~/Downloads/181.txt`（touch181，和 touch183 同一启动行）看，GPU 服务每次停在：
    `NativeAdreno: GMU firmware validated (57272 bytes)` →
    `NativeAdreno: GMU boot needs sheng-gpu-cmddb-version` →
    `NativeAdreno: publishing the GPU node without a started CP`。
    也就是 **ColdStart 根本没开始**，不是 GMU/CP 本身坏了。
  - 根因（源码 + 日志对照）：启动行里写的是 `sheng-gpu-cmdb-version=1`（少一个 `d`），
    而 `NativeAdrenoService.cpp` 解析的是 `sheng-gpu-cmddb-version`（command DB）。
    hfi25/26 当时用的是正确的 `cmddb`；后来拼 567 B 触摸启动行时拼错了。
    这条从 touch167 起就一直存在，所以整个触摸阶段 GPU 都是停的。
  - 修法：只改启动行一个字符（`cmdb` → `cmddb`），**内核不动**（仍是 touch183 的
    `a291772a…`，按键默认开、触摸 181 行为）。新启动行 568 B；
    payload 的 `cmdline[608]` / XNU arm64 `BOOT_LINE_LENGTH=608` 都放得下（上限 607）。
  - 主机侧核对：新旧 payload 的 `0x000–0x047` 与 `0x2a8` 之后逐字节相同，
    只有 `0x048` 起的 cmdline 字段变化；镜像大小仍 32,018,432 B。
  - 产物：
    内核 `a291772a118aaba57f3c082848cfb355cd985889ceee1733ebe6cc8b4b7f8e05`（未重建，17,037,056 B）；
    payload `shim/out/payload-gpu1-cmddb.bin`
    `3e4c79f632e96ca4ebc9e2c6e70fc9b7490504752dfded842e7d76d81fab1a06`（25,440,560 B）；
    ADT `shim/out/liuqin.adt` `044ba3a32f52813b085cac794ca397e8946c622f0f34e724ba83f85e5ee91cc9`（13,796 B）；
    镜像 `shim/out/shim-boot-gpu1-cmddb.img`
    `bd44bcf351dd6cae4ba04809bd89ec9f9517c01db36815abf740b29da35cfb09`（32,018,432 B）。
    `validate-repo` PASSED。没有重编内核，所以 `verify-patches` 不需要重跑。
  - 下一次上机先看（`184.txt`）：
    - `NativeAdreno: GMU boot needs ...` 不再出现；
    - `NativeAdreno: ColdStart status=0 fw_ready=1 hfi_ready=1 ...`；
    - `NativeAdreno: GPU ready, publishing services` / `GPU started on gpu`；
    - `NativeAdreno: CP self-test ... wrote=0x600dc0de`。
    若停在 `ColdStart status=...` 或 `cp ...`，按 `docs/GPU_NEXT.md` 的对应 checkpoint 查。
  - 说明：这只是把 GPU 内核 bring-up 恢复；UI 仍走软件渲染，Metal 需要用户态
    Mesa/Turnip/NativeMetal（`docs/GPU_MILESTONE.md` 的 “Still open”）。

- **2026-10-28 GPU 第 237 步：`gpu1.txt` 确认内核 GPU 全链路通过；UI 仍无 Metal，下一步是用户态**：
  - 用户刷 `shim-boot-gpu1-cmddb.img`，日志 `gpu1.txt`（539,251 B，sha256
    `b0b3ba3d53ebe9e62dcf4791e10b802b00d3902e8129aaab6daf5fa9b2d4e3cc`）：
    `ColdStart status=0 mode=intr fw_ready=1 hfi_ready=1 stage=8 ... ack=0xde elapsed=18993 us`、
    `OOB GPU_SET acked`、`GX power up (0x0)`、`GPU context status 0x0 step 7`、
    `CP SQE alive rbbm=0x1 hw_fault=0x0`、`CP started fence=2`、
    `CP self-test ... wrote=0x600dc0de (want 0x600dc0de)`、
    `GPU ready, publishing service`、`LIUQIN BOOT: adreno services started at 76.336 s`。
    中断 mask 仍只丢 bit28（`0x13d283c2` vs `0x33d283c2`），fence 轮询，和 hfi26 一致，
    不是新问题；日志里没有 GPU fault/panic（`DumpPanic` 是用户态 crash 汇报的常驻进程，
    后面还有几百秒正常日志）。
  - **但 UI 还没有 Metal**：backboardd/SpringBoard 的
    `IOServiceMatching("IOAcceleratorES")` / `IOAcceleratorESLegacy` 在日志里重复
    358 次 `-> 0 found`，`AGXAccelerator -> none`。内核发布的是
    `NativeAdrenoGpuService`（自定义 ABI 的 user client），不是 iOS Metal 发现链要的
    `IOAcceleratorES` + `MetalPluginName` + `NativeAdrenoMetal` bundle。
  - 当前内核里 `AGXAccelerator`/`NativeAdrenoPublish.cpp` 是 sheng 新栈的代码，
    但没有被 board glue 实例化/发布：`native_adreno_publish()` 没有调用者，
    `AGXAccelerator` 没有实例；用户态也没有 `NativeAdrenoMetal.bundle`。
    所以 CoreAnimation 仍走软件渲染。
  - 下一步两个层次（需要用户选方向）：
    1. **用户态 ABI 自检**：`/usr/libexec/liuqin-gpu-test` 已经在当前 root 里
       （sha256 `a3a46027…`），但没有 LaunchDaemon；`shim/out/liuqin-gpu-test`
       已重编。把它排进 launchd + 启动行加 `sheng-gpu-gputest=1`，目标
       `gputest: target[0x40]=0x600dc0de want 0x600dc0de -> PASS`。
       这一步只验证现有 `NativeAdrenoGpuService` user client，不改内核。
    2. **原生 Metal**：按 sheng 的 `GpuPowerProbe.cpp` / `NativeAdrenoAccelerator.cpp`
       / `drivers/NativeMetal` 把 `AGXAccelerator` 发布出来，再装
       `NativeAdrenoMetal` bundle + `NativeBridgeMetalFrontend` + Turnip。
       目标：`IOAcceleratorES` 匹配到 1、backboardd 打开插件、Metal 设备出现。
       这是多步工程（内核 accelerator 发布 + 用户态插件/信任/注入），不是一张镜像能完成。
  - 本步没有重编内核、没有改 root；等用户选 1 还是 2。

- **2026-10-28 GPU 第 238 步：用户选原生 Metal；内核先发布 AGXAccelerator（`sheng-gpu-metal=1`），出 metal1 镜像**：
  - 方向：用户明确选“直接开始原生 Metal 移植”。第一步只做**内核 accelerator 发布**，
    用户态插件/Turnip 下一步再做；完整计划写在 `docs/METAL_PORT.md`。
  - 代码（`xnu/patches/0244-liuqin-native-metal-publish.patch`，5 hunk）：
    1. `iokit/conf/files.arm64` 把 `LiuqinStubs.cpp` 移到 `NativeAdrenoAccelerator.cpp`
       之前，保证 stub `IOAcceleratorES` 的 metaclass 先注册；
    2. `NativeAdrenoService.cpp` 在 CP 起来后、仅当 `sheng-gpu-metal=1` 时
       调 `native_adreno_publish(vm, execbuf_, &ring_, ...)`，用 A730 的
       `NativeAdrenoAbiDeviceInfo`（chip id `0x07030001`、3 MiB GMEM、UBWC 4.0，
       va `0x20000000/0x60000000`），然后分配 `AGXAccelerator`，设
       `MetalPluginName=NativeAdrenoMetal`、`MetalPluginClassName=NativeAdrenoMetalDevice`、
       `NativeSandboxGrantProcess=backboardd,...`，attach/register；
    3. 发布前检查 `IOAcceleratorES`/`AGXAccelerator` 的 metaclass 和 136 字节基类，
       缺失就只打印 `accelerator class missing`，回退到旧的 `NativeAdrenoGpuClient`，
       不让新路径把旧 GPU 服务带走。
  - 启动行 586 B：在 gpu1 那行基础上加 `sheng-gpu-metal=1`（`sheng-gpu-cmddb-version=1`
    保持不变，仍在 607 B 上限内）。
  - 产物：
    内核 `7cb67731eceaf1914baa220cf7ade2ef01b71523eda2f1037d9734da4c6410a6`（17,037,096 B）；
    payload `13c0131b96cb76c6f885d9d61b1e3f718256f4916aee7f2399fb46ce1bd7a80c`（25,440,608 B）；
    镜像 `shim/out/shim-boot-metal1-accel.img`
    `08515212e90622a91084ca1cbca2c98e9c5d8dc3c38683da1e24b360fb9be427`（32,018,432 B）。
  - 主机侧验证：`verify-patches` **243/243** 干净应用；应用后的
    `iokit/conf/files.arm64` 与 `NativeAdrenoService.cpp` 与构建树逐字节相同；
    `validate-repo` PASSED（243 patches）。
  - 上机预期（`metal1.txt`）：
    - `NativeAdreno: metal publish 0x0`；
    - `NativeAdreno: accelerator published (IOAcceleratorES)`；
    - backboardd/SpringBoard 的 `IOServiceMatching("IOAcceleratorES")` 从
      `0 found` 变成 `1 found`，并读取 `MetalPluginName`；
    - 因为插件 bundle 还不存在，Metal 设备仍不会出现，UI 仍是软件渲染；
      但这一步能证明 accelerator 的发现链已经接上。
    如果看到 `accelerator class missing`、`metal publish 0x...` 非 0、
    或 `accelerator attach/allocate failed`，把整份日志发回。
  - 未做：用户态 `NativeAdrenoMetal.bundle`/Turnip/Turnip 的 iOS 16.6.1 适配；
    `zzw-native-adreno-sandbox-grant.patch` 也还没进 liuqin 序列（插件 open 需要它）。
  - 注意：这仍是**诊断镜像**，不是最终 UI 配置；`sheng-gpu-metal=1` 在启动行里显式开启。

- **2026-10-28 GPU 第 239 步：metal1 真机确认 accelerator 发布成功；open 被 sandbox 拒绝（0xe00002c7），黑屏是显示休眠/按键线程未开始**：
  - 用户刷 `shim-boot-metal1-accel.img`，日志 `metal1.txt`（411,036 B，sha256
    `8f3fb8664421609692633f11b603833795e77f7f2d7a21811258cd2e57ff49f2`）。
  - **Stage 1 成功**（观察）：
    - `NativeAdreno: metal publish 0x0`（line 283）；
    - `NativeAdreno: accelerator published (IOAcceleratorES)`（line 284）；
    - backboardd `IOServiceMatching("IOAcceleratorES") -> 1 found`（line 2116）；
    - `AGXAccelerator` 出现在匹配结果里（line 2145 起）。
  - **插件 open 被 sandbox 拒绝**（观察）：
    `IOK backboardd: open AGXAccelerator type 1 -> 0xe00002c7`（line 2268），
    `0xe00002c7` 是 `kIOReturnNotPermitted`。这是原始 IOGPU 库用 type 1 打开，
    `NativeSandboxGrantProcess` 还没有被内核的 open 路径识别——需要
    `zzw-native-adreno-sandbox-grant.patch`。**`0245-liuqin-native-adreno-sandbox-grant.patch`
    已写好（4 hunk），`shim-boot-metal2-sandbox.img`（`d8b470db…`）已构建但没让用户刷**，
    因为插件 bundle 还不存在，open 仍会被驱动的 `type != 0x4147` 拒绝。
  - **黑屏判读**（观察 + 推断）：
    - 日志里 `LIUQIN FB: display off` 在 101 s（line 6042），之前有 swap 30；
      日志继续到 160 s，没有 panic、没有 GPU fault，进程还在 exec。
    - 同一镜像的 `LIUQIN KEYS: thread up, waiting 120 s before the first SPMI read`
      在 line 374；到日志结束都没有 `LIUQIN KEYS: arbiter version`，说明按键线程
      还在 120 s 等待里，**电源键没有被轮询**。锁屏后显示休眠，按电源键无法唤醒，
      看起来就是“黑屏”。
    - 之前的 `gpu1.txt` 也有 `display off/on`；这次不是 GPU panic。建议：等
      `LIUQIN KEYS: polling ...` 出现后再按电源键；日常使用先回
      `shim-boot-gpu1-cmddb.img`（`bd44bcf3…`，无 accelerator，软件渲染但按键/触摸可用）。
  - **用户态工具链 bootstrap（进行中）**：
    - Rust 1.99.1 已装（rsproxy 镜像）；Python 3.11.18（uv）已装；
      `glslangValidator` 16.6.0 已从源码构建；
    - Mesa 固定 commit `85fda3b3…` 已从 GitLab 拉到 `refs/UniXNU/.cache/mesa-work`，
      9 个 `patches/mesa/*.patch` 已按序应用；
    - Turnip 配置/构建发现 Xcode 自带 bison 2.3 不支持 Mesa 的 `-Werror
      -Wcounterexamples`，正在构建 GNU bison 3.8.2；之后重跑
      `scripts/build-turnip-darwin.py --target ios`。
    - 之后还要：`build-native-metal-bridge.py`（Rust + metal2vulkan + llvm-as）、
      `build-adreno-metal-plugin.py`（iOS 17 target，需适配当前 **iOS 16.6.1** root）。
  - 未做：0245 的完整 `verify-patches` 因网络中断过一次，已重跑；`metal2` 未上机；
    插件/Turnip 尚未产出。

- **2026-10-28 GPU 第 240 步：Turnip + metal2vulkan bridge + `NativeAdrenoMetal.bundle` 全部按 iOS 16.0 构建；出 metal3 诊断镜像和 root sparse diff（插件 test 模式，无 `.publish`）**：
  - 用户态工具链（这次都落在主机上，未进仓库）：
    - Python 3.11.18（uv）、Rust 1.99.1（rsproxy）、GNU bison 3.8.2、glslang 16.6.0；
    - Mesa 固定 commit `85fda3b3…` + 9 个 `patches/mesa/*.patch`；
    - **Turnip** `libvulkan_freedreno.dylib` 17,720,928 B，`LC_BUILD_VERSION minos 16.0`；
    - **metal2vulkan bridge** `libnative_metal_bridge.a` 308,599,096 B，minos 16.0；
    - **插件 bundle** `NativeAdrenoMetal.bundle`（`NativeAdrenoMetal` 22,090,992 B +
      `libvulkan_freedreno.dylib` 17,193,520 B），`MinimumOSVersion=16.0`，
      CDHash `cad1f66a33b91cd493c6ff3a284fe611fe29e465`；4 KiB 页签名。
  - 内核侧新增 `xnu/patches/0246-liuqin-native-metal-diag-log.patch`（2 hunk）：
    - `SetMetadata` 的插件结果行打到 `IOLog("NativeAdrenoMetal: ...")`；
    - `AGXDeviceUserClient` 的 open 成功/拒绝也打进程名和 type。
    这样不用 boot page 就能从日志看用户态 GPU 测试结果。
  - 产物：
    内核 `44c81d522aee82c4df61033f8673a7565c9ab0ead7e6d78414d995785e30fbbb`（17,037,152 B）；
    payload `771d6700a7a1b0f3a0f6b5b4dcc82528fc61f74d79f3a5e6f2cdd2e328407825`（25,440,608 B）；
    镜像 `shim/out/shim-boot-metal3-diag.img`
    `329fc13f24465ea3169ddf2a545396b6215c38d1f69369f2e87187c9c67dddd9`（32,018,432 B）；
    root sparse diff `~/liuqin-root/ipad71-root-metal-nopublish.simg`
    `52e4bcee3cdfea4d66d99d0b8b23fce3658fe392159ea90a14d52b564f73bdd8`（39,126,964 B，
    基准 `ipad71-root-wrap18.img`，sha256 `bf7ecd5a…`）。
    完整改好的 root `~/liuqin-root/ipad71-root-metal-nopublish.img`（16 GiB）也在。
  - 插件是 **test 模式**：root 里**故意不放** `NativeAdrenoMetal.publish`，所以插件跑
    `NativeAdrenoUserTest` 后返回 nil，UI 仍走软件渲染；`AGXAccelerator` 的 open
    由 0245 的 sandbox grant 放行，日志应出现：
    - `NativeAdreno: metal client open type 0x4147 proc backboardd`
    - `NativeAdrenoMetal: backboardd s<step> e<error> v<verified>/<verifiedWc> ...`
    - 之后 Turnip 阶段 `NativeAdrenoMetal: T ...`
  - 上机步骤：先刷 root sparse diff（**前提是设备 userdata 就是 wrap18**；不确定就刷
    完整 16 GiB 镜像），再 `fastboot boot shim-boot-metal3-diag.img`，等 5–6 分钟，
    日志存 `metal3.txt`。黑屏仍可能是显示休眠 + 按键线程 120 s 未开始，不影响日志。
  - 已验证：**245/245** 补丁干净应用，`NativeAdrenoPublish.cpp` 与构建树逐字节相同，
    `validate-repo` PASSED；待上机：插件 test 是否通过、Turnip 是否进入 `T` 阶段。

- **2026-10-28 GPU 第 241 步：metal3 日志定位——触摸/按键是 HID 晚发布竞态（backboardd 50 s 早于 HID 服务），不是 root/GPU；HID 注入库加 90 次重试，出 metal4**：
  - `metal3.txt`（632,684 B，sha256 `df51b70c…`）观察：
    - 内核触摸/按键都在工作：`LIUQIN HID: touch 0 down ...`、`LIUQIN KEYS: arbiter version`
      和末尾 `volume up down/up`；`LIUQIN HID` 服务正常。
    - 但 backboardd 在 50.383 s 启动，`create Liuqin Touch Screen/Buttons failed`（57 s），
      而 HID 服务要晚些才发布；旧 root 的 backboardd 在 79 s 启动，82.9 s
      `Liuqin Touch Screen ready`。所以是**启动时序**，不是 root 文件丢失，也不是 0245/0246。
    - 插件没有加载：日志只有 `IOAcceleratorES -> AGXAccelerator` 和
      `open AGXAccelerator type 1 -> 0xe00002c7`，**没有** `metal client open type 0x4147`
      或 `NativeAdrenoMetal:`；0245 的 sandbox grant 还没生效（或属性/进程名比较失败），
      需要下一步加日志。
  - 修复（`host/liuqin_touch_inject.c`）：`make_service()` 在
    `virtualServiceClientCreateWithCallbacks()` 返回 NULL 时原来直接 `break`；
    现在最多 90 次、每次 1 s 重试（每 10 次打一行），覆盖 HID 服务发布晚于
    backboardd 的情况。重编 `shim/out/libliuqintouch.dylib`
    `aaef27a0947abefdc6bfe35f3c9af2885dd5ac6fc0d97c61018f1e4afdb5ef26`（53,792 B），
    重新生成 `bsd/dev/liuqin_dylib.h`（`LIUQIN_DYLIB_BYTES 53792`）。
  - 产物：
    内核 `8af2e95dd2bce8d34c3c2df05d5a437a021600831d5c24c71137804656479803`（17,037,152 B）；
    payload `b232056e5bb549b453dfaf63c364ce82d02f78b422fd5cd7aef881f02be67c88`（25,440,608 B）；
    镜像 `shim/out/shim-boot-metal4-hidretry.img`
    `c55955449d44dcaa27020ca15c06842d6065bf9785f45d631bc0194ee70a9072`（32,018,432 B）。
  - 上机：**只刷 boot 镜像 metal4**（root 已经是我们刷的 `ipad71-root-metal-nopublish.img`，
    不用再动 userdata）；等 HID 服务发布 + 重试，触摸/按键应恢复；插件 test 仍可能不跑
    （grant 待修）。日志存 `metal4.txt`。
  - 下一步：给 `native_sandbox_grant()` 加 `IOLog`（属性类型、进程名、匹配结果），
    查 0xe00002c7 为什么没被 grant 跳过；然后插件 test 的 `NativeAdrenoMetal:` 行。

- **2026-10-28 GPU 第 242 步：metal4 真机确认触摸/按键恢复（HID 重试有效）；插件仍未 open，给 sandbox grant 加日志出 metal5**：
  - 用户用 Android 内脚本拉的日志 `liuqin-log-20261028-172812.txt`（638,006 B，
    sha256 `ee171bbc…`）：
    - `|backboardd| Liuqin Touch Screen ready`（line 3419）、`Liuqin Buttons ready`
      （line 3478），触摸/按键都恢复；`LIUQIN KEYS: power/volume up/down` 有事件。
      HID 的 90 次重试修复有效，root 不需要再动。
    - 插件仍未加载：只有 `IOAcceleratorES -> AGXAccelerator` 和
      `open AGXAccelerator type 1 -> 0xe00002c7`（line 2341），没有
      `metal client open type 0x4147` / `NativeAdrenoMetal:`。
    - 日志里 `LIUQIN HID: IOHIDDevice property get NativeSandboxGrantProcess by
      gamecontrollerd` 是 `native_sandbox_grant()` 对每个 IOServiceOpen 查属性时
      触发 HID 代理打印，正常，不是新问题。
  - `0247-liuqin-native-sandbox-grant-log.patch`（2 hunk）给 `native_sandbox_grant()` 加：
    - 属性 class（OSString/OSData）、service class、进程名、list；
    - `grants <proc>` / `does not list <proc>` / `has no NativeSandboxGrantProcess`。
  - 产物：
    内核 `cefc87d44598c572e20a02286847a8d679aaa026ff6a6e06382449e823643d7f`（17,037,152 B，
    含 HID 重试 + grant 日志）；
    payload `49a44db458786d0a3e98f745bfee31e05af53bc24d144529fa30f3179dd23cb5`（25,440,608 B）；
    镜像 `shim/out/shim-boot-metal5-grantlog.img`
    `7e3013235a2c24eae57adc954ffe3e81dc1bc239e80e05d0c4344c2715c2bc60`（32,018,432 B）。
  - 上机：**只刷 metal5 boot**，root 不动。日志里找 `NativeSandboxGrant:`：
    - `grants backboardd` → sandbox 过了，接着应看到
      `NativeAdreno: metal client refused 0xe00002c6 type 0x1`（原始 IOGPU）和/或
      `metal client open type 0x4147`；
    - `does not list backboardd` → 进程名比较问题；
    - `property is OSData/...` → `setProperty` 类型问题；
    - `has no NativeSandboxGrantProcess` → 属性没设到 accelerator 上。
  - `validate-repo`：**246 patches PASSED**；完整 `verify-patches` 这次因 GitHub SSL 失败，
    但 0247 已在本地 0246 树上应用并与构建树逐字节相同；待上机看 metal5 的 grant 日志。

- **2026-10-28 GPU 第 243 步：metal5 证明 sandbox grant 已生效；0xe00002c7 其实是 kIOReturnUnsupported（驱动拒绝原始 type 1），插件仍没有 type 0x4147 open；给 MetalPluginName/ClassName 读取加日志出 metal6**：
  - 用户日志 `liuqin-log-20261028-195419.txt`（521,899 B，sha256 `8ebe07b7…`）：
    - `NativeSandboxGrant: AGXAccelerator proc backboardd list backboardd,...`
    - `NativeSandboxGrant: AGXAccelerator grants backboardd`
    - 紧接着 `IOK backboardd: open AGXAccelerator type 1 -> 0xe00002c7`。
  - **修正上一轮的误判**：`0xe00002c7` 不是 `kIOReturnNotPermitted`，而是
    **`kIOReturnUnsupported`**（`iokit/IOKit/IOReturn.h`: `BadMessageID=0x2c6`,
    `Unsupported=0x2c7`）。grant 已经放行，0xc7 是 `AGXAccelerator` 对原始
    **IOGPU type 1** 的拒绝，符合驱动设计。
  - 插件仍没有 `metal client open type 0x4147`。问题已不在 sandbox，而在
    **Metal 发现链没有走到插件**：可能 framework 先做原始 IOGPU type 1 open，
    失败后不再读 `MetalPluginName`/加载 bundle；本 port 的 `IOAcceleratorES`
    只是 `LiuqinStubs.cpp` 的 stub，没有 sheng 的原始
    `com.apple.iokit.IOGPUFamily`。
  - `0248-liuqin-native-metal-property-log.patch`（1 hunk）在
    `native_adreno_note_property_read()` 里给 `MetalPluginName`/
    `MetalPluginClassName` 的读取打日志（reader 进程名）。
  - 产物：
    内核 `f6d5ddafa21c158affdec2ce22138596945176a965222a075663c14f030edd1c`（17,037,152 B）；
    payload `b024aa7dae385ab44c8f8caa29c8ca87155ee4d72fbcaf11c763dc83af9119cc`（25,440,608 B）；
    镜像 `shim/out/shim-boot-metal6-proplog.img`
    `b06cfab6756411122062c014e723d848c625f15a7ba19ed9b19ab970b68777ae`（32,018,432 B）。
  - 上机：只刷 metal6 boot。日志里找 `NativeAdreno: MetalPluginName read by ...`：
    - 有 → framework 读到了属性，问题在 bundle 加载/类实例化/type 0x4147 open；
    - 没有 → 发现链在更前面就停了（原始 IOGPU type 1 / stub 基类），下一步
      不是插件，而是把原始 `IOGPUFamily` 或等效的 type 1 用户客户端接进来。
  - 待验证：247 patches、metal6 的 `MetalPluginName` 读取日志。

- **2026-10-28 GPU 第 244 步：做跳过 Setup 的 root（iOS 16 的 ForceNoBuddy/SetupState + 关自动熄屏）**：
  - `ipad71-root-metal-nopublish.img` 里 `com.apple.purplebuddy` 只有
    `SetupDone/SetupFinishedAllSteps/SetupVersion/daemonizeNeedsToRun` 和
    mini-buddy false；iOS 16 的 HackiOS root 还要求
    `ForceNoBuddy=true`、`SetupState="SetupUsingAssistant"`，否则 Hello/Setup
    仍会起来。另外 `com.apple.springboard` 没有 `SBDisableAutoDim` 等键，锁屏后会
    自动熄屏。
  - 在 clone 上合并（owner 501/20，0600）：
    - `com.apple.purplebuddy`: 上述四键 + `ForceNoBuddy` + `SetupState`；
    - `com.apple.springboard`: `SBDisableAutoDim`、`SBDontDimOrLockOnAC`、
      `SBDisableIdleSleep`；
    - `com.apple.migration` build `20G81`、`com.apple.managedconfiguration.notbackedup`；
    - `private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}` 已存在。
  - 产物：
    完整 `~/liuqin-root/ipad71-root-metal-skipsetup.img`（16 GiB，sha256
    `f10e65901a1ca8273c85e775a26f3557ca99da15d305d215e920d80de375bf65`）；
    稀疏 `~/liuqin-root/ipad71-root-metal-skipsetup.simg`
    `27fa66ab4a2f9967960862f10bb70a7736bc6ed031499ae92fa88a942f450c41`
    （364 KiB，91/4194304 块，基准是刚刷的 `ipad71-root-metal-nopublish.img`）。
  - 刷：sparse 直接 `fastboot -S 512M flash userdata ...simg`，再 boot metal6。
  - 如果仍然进 Setup：下一步 seed `mobileactivationd` 的
    `Data/System/<uuid>/Library/internal/data_ark.plist`（当前 Data/System 为空）
    或把 `/Applications/Setup.app` 禁用；先看 sparse 这版。
  - **更新（第 245 步）**：sparse 版已把 `/Applications/Setup.app` 改名为
    `Setup.app.disabled`，新 sparse `bc3786c9…`（808 KiB）、新 full `bf6c24d4…`；
    上一条的 `27fa66ab…`/`f10e6590…` 作废。

- **2026-10-28 GPU 第 245 步：20:16 日志仍是旧 root（用户还没刷 skipsetup）；MetalPluginName/ClassName 已被 backboardd/SpringBoard/chronod/Setup 读取——发现链是通的，插件没 open 是因为 grant 名单缺 UI 进程；0249 加 SpringBoard/Setup/chronod，出 metal7**：
  - `liuqin-log-20261028-201620.txt`（661,243 B，sha256 `67f63d69…`）：
    - `NativeAdreno: MetalPluginName read by backboardd`（2101）、`... by SpringBoard`
      （2787）、`... by chronod`（4926）、`... by Setup`（6372），
      `MetalPluginClassName` 同样；说明 framework 已读到名字，没有停在 type 1。
    - 仍没有 `NativeAdreno: metal client open type 0x4147`；type 1 的
      `0xe00002c7` 是 `kIOReturnUnsupported`，不是 sandbox。
    - `Setup` 进程仍在，是因为用户还没刷 skipsetup root（当时 root 仍是
      `ipad71-root-metal-nopublish.img`），不是 sparse 失败。
  - 插件 test 线程在 **SpringBoard/Setup/chronod** 里 open type 0x4147，但这些进程
    不在 `NativeSandboxGrantProcess` 名单（只有 backboardd/iconservicesagent/
    WidgetRenderer_Default/PosterBoard），所以 open 被 sandbox 拒绝在驱动之前。
  - `0249-liuqin-native-sandbox-grant-ui.patch`（1 hunk）把名单扩为
    `backboardd,iconservicesagent,WidgetRenderer_Default,PosterBoard,SpringBoard,Setup,chronod`。
  - 产物：
    内核 `c43800ac0c8be177e771cbbaf8b02a6827ed7c52ac64598b1d261126228a7134`
    （17,037,152 B）；payload `1b437c6c3d59cb110ace7706f0e42306f6f9889e552440168ff78a5b161d1bc5`；
    镜像 `shim/out/shim-boot-metal7-grantui.img`
    `d69a2d4ded78eeba91ae6aaaeef414b281b2461cfeeb11eaaca3f7855a0f8040`（32,018,432 B）；
    skipsetup root full `bf6c24d4abf8183d8db06921968d6d0a72eec25f5c948f000d0f1415ea0d5db5`、
    sparse `bc3786c9f55a07b348b9167308bc91e2fcfd1602faea9fe1478ac0b24f9cb68c`（808 KiB，
    基准 `ipad71-root-metal-nopublish.img`，含 Setup.app.disabled）。
  - 上机：刷 skipsetup sparse + boot metal7；预期 Setup 不再起来，日志出现
    `NativeSandboxGrant: ... grants SpringBoard` 和 `metal client open type 0x4147`。

- **2026-10-28 GPU 第 246 步：metal7 日志——grant 名单已生效（backboardd 过），但 SpringBoard/Setup/chronod 只读 MetalPluginName、从不 open，插件 initWithAcceleratorPort: 没被调用；在注入库里直接 dlopen 插件并调用它，出 metal8（只 boot）**：
  - `liuqin-log-20261028-203436.txt`（649,200 B，sha256 `9cf8b16a…`）：
    - `NativeSandboxGrant: AGXAccelerator proc backboardd list
      backboardd,...,SpringBoard,Setup,chronod` / `grants backboardd`（2278-2279）；
    - SpringBoard（2886）、chronod（4914）、Setup（6154）都只读
      `MetalPluginName`/`MetalPluginClassName`，没有任何后续 open；仍无
      `metal client open type 0x4147`。
    - 说明问题不是 grant 名单，而是 **插件没有被实例化**（framework 读了名字但没走到
      `initWithAcceleratorPort:`，或 bundle 在设备上加载失败）。
  - 主机侧检查：bundle `codesign --verify --deep --strict` 通过（CDHash
    `cad1f66a…`），`_OBJC_CLASS_$_NativeAdrenoMetalDevice` 导出，依赖
    Metal/Foundation/IOSurface；所以不是明显的签名/符号缺失。
  - 诊断：在 `libliuqintouch`（注入 backboardd）里加 `metal_probe_thread`：
    `dlopen("/System/Library/Extensions/NativeAdrenoMetal.bundle/NativeAdrenoMetal")`，
    `objc_getClass("NativeAdrenoMetalDevice")`，`IOServiceMatching("IOAcceleratorES")`，
    再 `objc_msgSend` 调 `initWithAcceleratorPort:`；日志用 `|backboardd|` 的 stderr。
    - `dlopen` 失败 → bundle/AMFI/`_MTLDevice` 符号问题（会打印 `dlerror`）；
    - 成功且 init 返回 → 插件 test 线程会 open type 0x4147，驱动打
      `metal client open`，证明插件和测试本身是好的，剩下的是 framework/原始 IOGPU
      发现链。
  - 注入库 `shim/out/libliuqintouch.dylib` 54,160 B
    `b454a8c690e6cccc70e8345cf0f41daa55a0ee50c71218bed1b27fc041c662cb`；
    `tools/embed-dylib.sh` 现在写到 `$WORK/xnu/bsd/dev/liuqin_dylib.h`（原来硬编码
    `/tmp/liuqin-build3`，已修）。
  - 产物：内核 `cc335c4b3cbcc7c89f5db064c79a970256f49ecc7737065bfd9ef8d73d5be829`
    （17,037,152 B）；payload `8dc1c9fa4a810a695868cf0af9bb4645375abcdb7d8a8c567b547bf433e58804`；
    镜像 `shim/out/shim-boot-metal8-probe.img`
    `b342a0175766a670449fd25fb3ba8ae0aec8d9d0e001411b498ee6f2df888b16`（32,018,432 B）。
  - 上机：**只 boot metal8**，root 不动。日志找：
    `|backboardd| metal probe: start`、`metal probe: plugin dlopen failed: ...`
    或 `metal probe: initWithAcceleratorPort -> ...`、`NativeAdreno: metal client open type 0x4147`。
  - 这一步只验证插件 bundle/类/方法；如果 probe 成功但 framework 仍不调用，下一步是
    原始 `IOGPUFamily`/type 1 用户客户端（sheng prelink），不是继续改插件。

- **2026-10-28 GPU 第 247 步：metal8 probe 给出硬错误——设备上 `dlopen` 插件报 “not a mach-o file”；主机镜像里的文件是好的；metal10 加文件头/size/mode 和 `/var/tmp` 拷贝 probe**：
  - `liuqin-log-20261028-204747.txt`（560,491 B，sha256 `2328b8d3…`）：
    ```
    |backboardd| metal probe: start
    |backboardd| metal probe: plugin dlopen failed: dlopen(/System/Library/Extensions/NativeAdrenoMetal.bundle/NativeAdrenoMetal, 0x0006): tried: '...' (not a mach-o file)
    ```
    这是 dyld 对文件 magic 的判断，不是 sandbox/签名；Setup/SpringBoard 仍只读属性。
  - 主机镜像 `ipad71-root-metal-nopublish.img` 里的 `NativeAdrenoMetal`：
    size 22,090,992，sha256 与 build bundle 完全相同
    `21b7d540…`，`ls -lO` 无 compressed 标志；所以镜像里的文件是好的。
    设备上 dyld 看到的内容不是 Mach-O → HFS 文件读取/extent/mmap 或路径问题。
  - `libliuqintouch` 再加两个探针（metal10）：
    1. `open/read` 前 16 字节 + `lstat` size/mode，直接打印；
    2. 把文件按 `read/write` 复制到 `/var/tmp/NativeAdrenoMetal.probe`，再 `dlopen`
       这个拷贝（touch dylib 就是从 `/var/tmp` 成功加载的）。
    这能区分：文件内容坏（hdr 不是 `cffaedfe`）还是 HFS 路径/mmap 坏（原路径失败、
    `/var/tmp` 拷贝成功）。
  - 产物：内核 `8dab7539d8fb7615eafdc92481e1a217cafb7b25ac74bfb1a177d42139c8eed9`
    （17,037,152 B）；payload `a5390d60a0666dc9cfd15516ed4c33f3fde9c76f0a95369ffea589e3aad50fc2`；
    镜像 `shim/out/shim-boot-metal10-copyprobe.img`
    `54d64d83473adfafe16103e13072ef31caa1687346ace7f1ebe77aa878f01958`（32,018,432 B）。
  - 上机：只 boot metal10，root 不动。日志找：
    `metal probe: size ... mode ...`、`hdr ...`、`plugin dlopen failed`、
    `copied to /var/tmp/...`、`copy dlopen failed/succeeded`、`initWithAcceleratorPort`。
  - 如果 hdr 就是 Mach-O 而原路径 dlopen 失败、拷贝成功 → HFS 文件/mmap 问题；如果
    hdr 不是 Mach-O → 文件在设备上被读坏/截断，优先查 HFS 驱动/根镜像布局。

- **2026-10-28 GPU 第 248 步：metal10 找到真正原因——设备上插件文件的内容是一段 HFS catalog 文本（hdr `fari.SafariLinkE`），不是 Mach-O；主机镜像同一文件是 `cffaedfe`；说明 root 只写进了目录/大小，没写进插件的 22 MiB 数据块**：
  - `liuqin-log-20261028-210024.txt`（523,554 B，sha256 `3c8c2564…`）：
    ```
    |backboardd| metal probe: size 22090992 mode 0100755
    |backboardd| metal probe: hdr 666172692e5361666172694c696e6b45 n=16
    |backboardd| metal probe: plugin dlopen failed: ... (not a mach-o file)
    |backboardd| metal probe: copy dlopen failed: ... (not a mach-o file)
    ```
    `66 61 72 69 2e 53 61 66 61 72 69 4c 69 6e 6b 45` = `fari.SafariLinkE`，
    是 **ASCII 文件名片段**（HFS catalog B-tree 的内容），不是 Mach-O；size 却是
    插件正确的 22,090,992，说明**目录记录在、数据块不在**。
  - 主机镜像验证：`F_LOG2PHYS` 显示插件第一个逻辑块在 raw offset
    **13,824,487,424**（块 3,375,119），raw 读出来是 `cffaedfe`；主机文件
    sha256 `21b7d540…` 与 build 相同。也就是说，**设备上的同一块仍是旧的
    catalog 文本**——root 刷入时只写了 HFS 元数据（路径/大小），没有写插件的
    22 MiB 数据。
  - 原因：当前 root 是 **sparse/partial 刷入**，`ipad71-root-metal-nopublish.simg`
    只包含部分块；插件的 fb 数据块没有被写进设备。sparse 版的 skipsetup
    `bc3786c9…` 基准是当前 root，**也修不了这个数据块**。
  - 修复：必须刷 **完整** 的 skipsetup root（16 GiB）
    `~/liuqin-root/ipad71-root-metal-skipsetup.img`
    sha256 `bf6c24d4abf8183d8db06921968d6d0a72eec25f5c948f000d0f1415ea0d5db5`；
    它里面有正确的插件 22 MiB 数据，同时 `Setup.app.disabled`/`ForceNoBuddy` 跳过 Setup。
    刷完整镜像后再 boot metal7（grant 名单）或 metal10（probe）。
  - 这解释了为什么 `MetalPluginName` 属性读取正常、type 1 也正常，但插件永远不 open：
    **设备上的插件文件根本不是 Mach-O**。


- **2026-10-28 GPU 第 249 步：用户刷完整 skipsetup root + boot 后“进不去桌面”；把 `Setup.app` 恢复，保留 prefs/auto-dim/plugin，出 setupapp sparse/full；先 boot metal7（无 probe）恢复 UI**：
  - 推测（待日志确认）：`Setup.app.disabled` 让 SpringBoard 没有可交接的 Setup
    UI，启动后黑屏/无桌面；另外 metal10 的 probe 在 backboardd 里跑插件 test，
    也可能让 UI 卡住。两者都该先排除。
  - 新镜像 `~/liuqin-root/ipad71-root-metal-setupapp.img`：从
    `ipad71-root-metal-skipsetup.img` 克隆，只把 `Setup.app.disabled` 改回
    `Setup.app`；`com.apple.purplebuddy` 的 `SetupDone/ForceNoBuddy/SetupState`、
    springboard 的 `SBDisableAutoDim` 等、正确的 plugin 数据都保留。
    完整 sha256 `95a604b58225d66745435f6dd81a1988b4ed06ab862cf9051d7fed64b2ae2a9a`。
  - 稀疏 `~/liuqin-root/ipad71-root-metal-setupapp.simg`
    `c7f97332e01a47b46689498c5b24d8ad37f432a3b63a97661cac794fc8107a3f`
    （296 KiB，74/4194304 块，基准是 `ipad71-root-metal-skipsetup.img`，即刚才刷的
    完整 skipsetup root）。
  - 上机：先刷 setupapp sparse（若当前 root 就是刚才的完整 skipsetup），再 boot
    **metal7**（`d69a2d4d…`，无 probe，不会跑插件 test），确认 UI/桌面回来。
    如果 UI 回来但仍是 Setup 页，说明 prefs 不跳过，下一步做 SpringBoard injection
    （HackiOS 的 SBSetupManager 办法），不再删 Setup.app。
  - 如果还是黑屏，拉日志看 SpringBoard/backboardd 是否 crash/loop；metal10 的
    probe/test 先不要用。


- **2026-10-28 GPU 第 250 步：`2248.txt` 显示不是内核卡死，而是 Setup 在等激活（mobileactivationd/budd 在，FB/touch 都在）；ios-sweet 的 prefs/data ark 依赖 Data seed，而我们的 Data 是空的；改做 HackiOS 式 SpringBoard 注入，出 inject root**：
  - `2248.txt`（1,213,003 B，sha256 `fece2b92…`）：`exec mobileactivationd` 111.844 s、
    `budd`（`com.apple.purplebuddy.budd`）、`exec SpringBoard` 104.133 s、
    `LIUQIN FB` 80,000 swaps、touch 一直有事件；**没有 panic**。所以 40 分钟是
    Setup 在等激活/进度，不是 kernel hang；等下去也不会完成。
  - ios-sweet 的 `sweet-root-tweaks.py` 里 `activation()` 需要
    `private/var/containers/Data/System/.../com.apple.mobileactivationd` 容器；
    我们这份 root 的 `Data/System` 是空的（旧 root 是跑过一次 setup 的 Data，
    被完整镜像重置了），所以 `data_ark` 没 seed，Setup 卡住。
  - 采用 HackiOS `inject_sb.c` 的办法（同一套 `SBLockdownManager`/`SBSetupManager`
    注入），但按 16.6.1 用类名/selector 替换，不依赖 16.5 地址：
    - `-[SBLockdownManager _setupActivationState]` → int@+8 = 2（activated）；
    - `-[SBSetupManager _setSetupRequiredReason:]` → 强制 0；
    - `_dyld_register_func_for_add_image` 等类晚加载时补 patch。
  - 产物：
    `shim/out/libliuqinsetup.dylib` 50,608 B
    `5fade1632ac4a4bc42f4c7aa4be49e2f714dcd1fc6ef066a426529c7bbf29ac6`；
    `shim/out/liuqin-springboard` 99,904 B
    `7c829db1343a5ee80fe04bf5801850e63e18a1a56ed049faf153cc17a4863ea1`；
    安装到 root 的 `/usr/libexec/liuqin-springboard`，并把
    `/System/Library/LaunchDaemons/com.apple.SpringBoard.plist` 的
    `ProgramArguments` 改成它；launcher 把 dylib 写到 mobile 的 Caches，
    设 `DYLD_INSERT_LIBRARIES` 后 exec 真正的 SpringBoard。`Setup.app` 保留。
  - 镜像：
    `~/liuqin-root/ipad71-root-metal-inject.img`（16 GiB）
    `6e0b4f63a369daeba66244616eaec46aab693664aa95945c3f7e0fd4fac7057c`；
    稀疏 `~/liuqin-root/ipad71-root-metal-inject.simg`
    `5c06949cfe3712e2a83aebee246c65ce5a83ff50a45ed1425353b10ba012e143`
    （253 KiB，基准是 setupapp 镜像，即刚才刷的 root）。
    压缩包 `~/liuqin-root/ipad71-root-metal-inject.img.zip`
    `e44604386b26e0c56c9982087759e6d49c40f393852d894045803f6fe14a4f7a`（5.1 GiB）。
  - 上机：刷 inject sparse（或完整 img），boot **metal4**（`c5595544…`）；日志找
    `|SpringBoard| liuqin setup: ... activation patched ... reason patched`，
    然后应该跳过 Setup 到锁屏/桌面。如果 `SBLockdownManager`/`SBSetupManager`
    没找到，日志会打印 `not yet`，再按 16.6.1 的类/ivar 调整。


- **2026-10-28 GPU 第 251 步：用户刷的是 inject 整包，但 `2330.txt` 里注入完全没跑——根因是 launchd 读的是单文件 `/System/Library/xpc/launchd.plist`，不是 `/System/Library/LaunchDaemons/com.apple.SpringBoard.plist`；改单文件无效。已改单文件里的 job 并出 inject2**：
  - `2330.txt`（1,094,565 B，sha256 `5024f4b6…`）：
    `exec SpringBoard at 50.659 s`；没有 `LIUQIN LAUNCHER`、没有
    `|SpringBoard| liuqin setup:`；SpringBoard 还是直接起真身。`budd`（59.2 s）、
    `mobileactivationd`（58.5 s）、`lsd`（58.1 s）都在，`LIUQIN FB` 68,000 swaps；
    20 分钟进度条是 LaunchServices 建库，不是注入。
  - 单文件 `launchd.plist` 的 `LaunchDaemons` key
    `/System/Library/LaunchDaemons/com.apple.SpringBoard.plist`（Label
    `com.apple.SpringBoard`）`ProgramArguments` 原来是
    `/System/Library/CoreServices/SpringBoard.app/SpringBoard`；已改成
    `/usr/libexec/liuqin-springboard`。individual plist 也保持改成 launcher。
  - inject2 产物：
    完整 `~/liuqin-root/ipad71-root-metal-inject2.img`（16 GiB）
    `ecfe8d611890b346556756b12e3291a3b190434f6a24ac916f349f07ff77d115`；
    稀疏 `~/liuqin-root/ipad71-root-metal-inject2.simg`
    `a553e6b3cef3e290b014f2cb744895e90a62c0c4810fdc56edddd43cb25cac8b`
    （816 KiB，基准是 inject1 `6e0b4f63…`，即用户刚刷的整包）；
    压缩包 `...inject2.img.zip`
    `a7a4f9541858a58fd12feb42bce51dbe6fa5b45c5c41b9e599ca626bf74e52a9`（5.1 GiB）。
  - 上机：刷 inject2 sparse/完整，boot metal4；日志应出现
    `|SpringBoard| liuqin setup: ... patched`；Setup 跳过。LaunchServices 建库仍要
    几十分钟，这次不要中途重启。


- **2026-10-31 推送 + 248 补丁本地校验**：把这一批（102 个新补丁、touch/keys/GPU/HID
  文档、`host/` 注入工具、`refs/` 与 `linux-ref/` 参考源码）推到
  https://github.com/QPRS-6/iPadOS-liuqin。推送前在本机（Linux）跑
  `tools/verify-patches.sh`：三棵 Apple 源树 staging 完成后 **248/248 补丁干净应用**。
  唯一的偏差是 `0019-stage-libpthread.sh` 用 `xcrun` 拷 `TargetConditionals.h`，本机
  没有 macOS SDK，用一个 stub 文件补上——补丁应用不读这个头文件，CI 仍用真实 SDK。
  `tests/test_tools.py` 与 `tools/validate-repo.py` 同时全绿。
