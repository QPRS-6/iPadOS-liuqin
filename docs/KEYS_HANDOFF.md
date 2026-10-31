# 按键（电源 / 音量）交接 —— 2026-10-28 收尾，下一次会话从这里开始

一句话：**按键已在真机确认可用（2026-10-28 用户反馈）；触摸能用但不稳（固件会崩，
崩了靠重下救）。按键这一节可以收尾，下一步转到 GPU（见 `docs/GPU_NEXT.md` 文末）。**

---

## 0. 当前状态

| 项 | 值 |
| --- | --- |
| 正在用的镜像 | `shim/out/shim-boot-gpu1-cmddb.img`，sha256 `bd44bcf351dd6cae4ba04809bd89ec9f9517c01db36815abf740b29da35cfb09`（GPU 修复版，内核仍是 touch183） |
| 同镜像的 payload | `shim/out/payload-gpu1-cmddb.bin`，`3e4c79f632e96ca4ebc9e2c6e70fc9b7490504752dfded842e7d76d81fab1a06` |
| 内核（17,037,056 B） | `a291772a118aaba57f3c082848cfb355cd985889ceee1733ebe6cc8b4b7f8e05` |
| 启动行 | 568 B；只把 `sheng-gpu-cmdb-version` 改成 `sheng-gpu-cmddb-version` |
| 补丁序列 | 242 个，最后一个 `0243-liuqin-touch-recovery-and-keys.patch`（20 hunk） |
| 触摸 | 可用，但固件在使用中每 30–90 s 崩一次，每次靠重下恢复（约 3 s 停顿） |
| 按键 | **真机确认可用**；touch183 起默认开（`liuqinkeys=0` 可关），新 GPU 镜像沿用 |
| 未解释 | 181 上出现过一次约 7 分钟黑屏，日志整份停住、无 panic |

`touch182`（20 ms 轮询、revive 限 30、跳过整幅回读）已经**回退**：
当前源码/补丁序列重新构建出的内核与 payload 与上面的 181 镜像逐字节相同
（已验证：`30fb9db7…` / `2d0d369b…`）。按键在 `touch183` 上验证通过后，
没有再做源码改动；GPU 修复只改了 payload 的启动行，内核仍是 `a291772a…`。

---

## 1. 按键硬件（已核对，不是推测）

来源：本机 base DTB（`dtb/boot_unpacked/dtb`）+ mainline
`yzddmr6/linux-sm8450-liuqin`（分支 `liuqin-6.17`）的 `sm8475-xiaomi-liuqin.dts`
+ 厂商模块 dmesg（见 `docs/KEYS.md`）。

| 键 | 位置 | 读的地址 | 判读 |
| --- | --- | --- | --- |
| 电源 | PMK8350 PON，SPMI **SID 0** | `0x1310`（PON `RT_STS` + 0x10） | bit 7 = 按下（GEN3 布局 `PON_GEN3_KPDPWR_N_SET`） |
| 音量下 | 同一字节（PON RESIN） | `0x1310` | bit 6 |
| 音量上 | PM8350 GPIO6，**SID 1** | `0x8d10`（引脚基址 `0x8800+0x100*5` + 0x10） | bit 0（ACTIVE_LOW，按下为 0 或 1 需实测确认） |

PMIC GPIO 基址规律：`pinctrl@8800` 一个节点管 12 个 pin，第 n 个引脚 = `0x8800 + 0x100*(n-1)`，
`RT_STS` 在 +0x10，bit0 是引脚电平（`pad->base = reg + i * 0x100`，见 Linux `pinctrl-spmi-gpio.c`）。

---

## 2. SPMI 通路（arbiter v7：`0x70000000`，768 个 APID）

DT 里的 `reg` / `reg-names` 顺序（从本机 DTB 解出）：

```
0x0c42d000 0x04000   cfg
0x0c400000 0x03000   core      ← version(0x00)、features(0x04)、APID→PPID 表(0x0b00+4*i)
0x0c500000 0x400000  chnls     ← 写通道：0x1000*apid（本驱动不用，只读）
0x0c440000 0x80000   obsrvr    ← 读通道：0x8000*ee + 0x20*apid（ee=0）
0x0c4c0000 0x10000   intr
```

规则（与 Linux `drivers/spmi/spmi-pmic-arb.c` 的 v7 分支一致）：

- `ppid = (sid << 8) | (addr >> 8)`；
- APID 从映射表读出来，**不能假设等于 PPID**：v7 的表在 `core + 0x2000 + 4*apid`
  （**不是 `0x0b00`**；`0x0b00` 是未实现窗口，touch173 的 26 ms/次读和 `apid -1` 都来自这里），
  `ppid_field = (reg >> 8) & 0xfff`（PPID 是 12 位，不是 13 位），`reg == 0` 表示该 APID 未用；
  APID 个数 = `core + 0x04` 的 `& 0x7ff`（本机读出 768）；
- 读命令：`CMD = (15 << 27) | ((addr & 0xff) << 4) | (len - 1)`（15 = `PMIC_ARB_OP_READ`）；
- 轮询 `STATUS`（通道 + 0x08）bit0 = DONE，bit1..6 = 失败；
- 数据：`RDATA0`（+0x18，前 4 字节）、`RDATA1`（+0x1c，后 4 字节）；
- ee=0 时 v7 与 v8 的读通道偏移相同（v8 是 `0x40000*ee`），两种 arbiter 都适用。

**只读。** 这个移植没有 PMIC 驱动，写 PMIC 寄存器可能动到电源轨；按键不需要写。

---

## 3. 已经验证过的观察（touch173 的日志；根因已在第 235 步找到）

```
LIUQIN KEYS: spmi arbiter version 0x70000000, 768 apids
LIUQIN KEYS: power sid 0 ppid 0x013 apid -1          ← 映射表里没找到
LIUQIN KEYS: volume up sid 1 ppid 0x18d apid 651     ← 找到了，读回 0x00
LIUQIN KEYS: power read failed (apid -1)
```

两个硬事实（后经源码核对，第 1 条其实是**表偏移读错了**，不是 PPID 不存在）：

1. **`ppid 0x013` 不在 bus 0 的映射表里**（当时读的是 `core+0x0b00`，v7 的真表在 `core+0x2000`；
   且旧掩码 `0x1fff` 把保留位折了进来）；
2. 那次启动为了找 APID 扫了 **3 × 768 次表读**，把 `touch clocks+pins ready`
   从 47 s 推迟到 130 s —— **约 26 ms 一次 MMIO 读**。这个数字决定了：
   按键**不能**按 20 ms 轮询（3 次读就是 78 ms/轮），也不能在启动路径里扫全表。

---

## 4. 代码在哪

全部在 `~/liuqin-build3/xnu/bsd/dev/liuqin_touch.c`（**补丁序列里唯一完全同步的文件**，
所以按键代码也塞在这里，不要新建文件——`bsd/conf/files` 与另外 ~55 个文件不在补丁里）：

| 符号 | 作用 |
| --- | --- |
| `SPMI_*` 宏（文件末尾“PMIC keys”一节） | 上面那套寄存器/命令格式 |
| `pmic_read(apid, addr, buf, len)` | 一次 SPMI 读（含 DONE/错误位轮询） |
| `pmic_apid(ppid, count)` | 扫映射表找 APID（**很慢，见上**） |
| `liuqin_keys_thread()` | 20 ms 轮询、两次一致去抖、边沿调 `liuqin_touch_post()` |
| `liuqin_keys_start()` | 由 `liuqin_touch_probe()` 调用；**只有 `liuqinkeys=1` 才做事** |
| `tch_crc_flag()` | 清 PMK8350 的 CRC 错误标志（`0x1fb535`），与按键无关但同段代码 |

默认关闭的开关就在 `liuqin_keys_start()` 的第一行（`PE_parse_boot_argn("liuqinkeys", …)`）。

---

## 5. 下一步（touch183 已按 1–5 做完，等日志）

1. ~~先量一次 SPMI 读到底多慢~~：touch183 会打印 `arbiter version ... read in N ns`、
   `first map read ... ns` 和 `direct apid 19 ... us`。
2. ~~不要扫全表，先假设 APID≈PPID~~：touch183 先读 `APID 19 / 0x1310`，再扫表。
3. ~~表读放在 ≥120 s 的独立线程~~：touch183 的 keys 线程先 `tsleep(120 s)`，
   再在独立线程里扫；慢的话每 64 项让出 CPU。
4. 第二条总线：touch183 在 bus0 找不到时自动试 bus1（`features[1]` 的 count，
   APID 从 bus0 的 count 开始；表项和 obs 偏移都用全局 APID）。
5. 轮询：每 **50 ms** 读 2 个字节（`0x1310` 一次给电源+音量下，`0x8d10` 给音量上），
   两次一致去抖；音量上按 DTS 的 ACTIVE_LOW 解释（0=按下）。
6. 每加一步都**单独出一个镜像**验证，别和触摸的改动混在一起。

**现在只等 `183.txt`**：先看 `first map read`、`bus0 scan`、两条 `ppid ... -> apid N`，
再看按键的 `power down/up`。

---

## 6. 坑（都是踩过的）

- **`0x51` 写**：固件引导后必须无条件写一次 `0x125851`（`tch_clear_fw_status`），
  否则固件约 18 s 后自行进恢复循环、报告区变 `fd`。这一条是触摸能用的前提。
- **revive 必须真的重下**：早期 revive 只写了 `0x69` 就读信息（日志里复位后仍是
  `state 0xa3`），等于空转。现在两条 revive 都调用 `tch_fw_download()`。
- **INT poke 默认关闭**：`liuqintouchwake=1` 才开。开着的时候怀疑过它把面板弄死。
- **重下有 3 s 停顿**：期间占着 SPI 引擎；181 上出现过一次 7 分钟黑屏（无 panic），
  疑似与此有关，尚未证实。
- **不要改启动行**（567 B），除非确实要加参数；改了要重新 `mkpayload`。
- 每次产出内核都要把 sha256 写进 `NOTES.md`；改完补丁跑 `tools/verify-patches.sh`
  和 `python3 tools/validate-repo.py`。

---

## 7. 构建 / 打包 / 验证（照抄）

```bash
cd /Users/qprs/Documents/deepseek-harness/default-workspace/iPadOS-liuqin

# 1) 内核
WORK=/Users/qprs/liuqin-build3 LLD=/Users/qprs/liuqin-build3/ld.lld ./tools/build-local.sh

# 2) 从旧镜像里取回启动行（567 B，不能手写）
BOOT=$(strings -a shim/out/shim-boot-touch167-opcode3.img | grep -o "debug=0xc.*liuqintouchsmmu=2")

# 3) payload + 引导镜像
K=~/liuqin-build3/fakeroot/System/Library/Kernels/kernel.development.liuqin
python3 tools/mkpayload.py "$K" shim/out/liuqin.adt shim/out/payload-touchXXX.bin \
    --cmdline "$BOOT" --fwarea shim/out/fwarea.bin
LLD=/Users/qprs/liuqin-build3/ld.lld shim/build.sh "$PWD/shim/out/payload-touchXXX.bin"
cp shim/out/shim-boot.img shim/out/shim-boot-touchXXX-名字.img

# 4) 验证
./tools/verify-patches.sh          # 应输出 all 242 patches applied cleanly
python3 tools/validate-repo.py     # 应输出 PASSED
```

改驱动后重新生成补丁：先保存改动前的文件，`diff -u 旧 新`，去掉前两行换成
`--- a/bsd/dev/liuqin_touch.c` / `+++ b/bsd/dev/liuqin_touch.c`，
覆盖 `xnu/patches/0243-liuqin-touch-recovery-and-keys.patch`。

---

## 8. 上机后看什么

```
LIUQIN KEYS: thread up, waiting 120 s before the first SPMI read
LIUQIN KEYS: arbiter version 0x70000000 read in 123 ns
LIUQIN KEYS: features 0x00000300/0x00000000 -> bus0 768 apids, bus1 0 apids
LIUQIN KEYS: direct apid 19 addr 0x1310 -> 0x00 (status 0x00000001, 87 us)
LIUQIN KEYS: first map read at 0x2000 took 74 ns
LIUQIN KEYS: bus0 scan 768 apids in 120 us (156 ns/read), 143 mapped
LIUQIN KEYS: power/volume down ppid 0x013 -> apid N (owner EE 0)
LIUQIN KEYS: volume up ppid 0x18d -> apid N (owner EE 0)
LIUQIN KEYS: pon reads 0x00 (power released, volume down released)
LIUQIN KEYS: volume up reads 0x01 (active low: released)
LIUQIN KEYS: polling power/volume down every 50 ms; press a key (volume up is active low)
LIUQIN KEYS: raw pon 0x80 volup 0x01
LIUQIN KEYS: power down        ← 按电源键
LIUQIN KEYS: raw pon 0x00 volup 0x01
LIUQIN KEYS: power up
```

判定：

| 看到 | 含义 | 下一步 |
| --- | --- | --- |
| `apid -1` | 表里没有这个 PPID | 走 §5.4（第二条总线）或改 PPID |
| 读回错误位 | 该 APID 不归 EE0 | 读 `intr` 区的 owner 表确认 |
| `power reads 0x00` 但按下不变 | bit 位/极性判断错 | 把整字节打印出来看哪一位动 |
| 出现 down/up | 内核通了 | 看 iOS 是否响应（锁屏/音量） |

---

## 9. 相关文档

- `NOTES.md` 第 218–233 步：触摸从“打通”到“会崩”的全部证据链；
- `docs/DMA_HANDOFF.md`：SPI/DMA 那条线怎么通的、哪些假设作废；
- `docs/KEYS.md`：按键硬件与 SPMI 协议的最初记录；
- `refs/novatek/`：厂商驱动源码（`nvt_nt36xxx.c/h`、mem map）与 NT36532 地址表。

---

## 10. 新会话开场白（直接复制粘贴）

```
这个仓库继续做 Xiaomi Pad 6 Pro（liuqin，SM8475）iPadOS/XNU 移植的按键驱动。
仓库：/Users/qprs/Documents/deepseek-harness/default-workspace/iPadOS-liuqin
先读 docs/KEYS_HANDOFF.md（按键交接，含硬件地址、SPMI 协议、touch183 诊断镜像、坑），
再读 NOTES.md 的 §8 交接 和第 218–235 步，以及 docs/KEYS.md。

目标：把 电源键 / 音量上 / 音量下 在内核里读出来，经 /dev/liuqin-touch 和已有的 HID
映射生效（HID 侧已就绪，不用改用户态）。

现状（不要重新验证，先当真）：
- 触摸可用但不稳：固件每 30–90 s 崩一次，崩了自动重下恢复；稳定基线仍是
  shim/out/shim-boot-touch181-endless.img（sha256 5a27d487…，内核 30fb9db7…）。
- 按键：v7 映射表偏移/掩码的根因已找到（表在 core+0x2000，PPID 12 位）；
  shim/out/shim-boot-touch183-keys-spmi.img（sha256 e6d31a50…，内核 a291772a…）
  是按键诊断镜像，默认开按键（liuqinkeys=0 可关），线程先睡 120 s 再读 SPMI。
  下一步的日志是 183.txt；拿到后先解析它，不要重新猜协议。
- 已知阻塞（touch173 的旧结论，已被第 235 步修正）：ppid 0x013 在 bus0 表里找不到，
  旧代码读的是 0x0b00；真表在 0x2000。26 ms/次读很可能也是旧偏移造成的，touch183 实测。

硬约束：
1) SPMI 只读，绝不写 PMIC 寄存器（没有 PMIC 驱动，写可能动电源轨）；
2) 最终镜像按键默认关闭；诊断镜像可以默认开，但要在 NOTES 里写清楚；
3) 不要动触摸的初始化/下载/轮询路径，不要开 INT poke；
4) 启动行 567 B 不变（用 shim/out/shim-boot-touch167-opcode3.img 里 strings 抓回来）；
5) 每次产出内核要跑 tools/verify-patches.sh 和 python3 tools/validate-repo.py，
   把内核/payload/镜像的 sha256 记进 NOTES.md。

工作方式：我在 macOS 上手动刷机（没有 adb），你给出镜像路径 + sha256，我刷完把日志
存成 NNN.txt 发你，你解析后继续。

下一步：解析 183.txt 里 `first map read`、`bus0 scan`、`ppid 0x013 -> apid N`、
`ppid 0x18d -> apid N`、`pon reads`、`volume up reads` 和按键边沿；
若 APID 找到了但边沿不对，先改极性/位；若仍是 -1，看 bus1 的 features/scan。
```
