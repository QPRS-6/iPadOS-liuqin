# 按键（电源 / 音量）——硬件事实与实现计划

数据来源：设备自己的 DT（`dtbo.img` 的 overlay + `vendor_boot.img` 的 base DTB），
用 `ipsw`/Python 离线解析（脚本见 `tools/` 之外的分析记录：`/tmp/fdtnode.py`、`/tmp/fdtwalk.py`）。

## 按键接在哪

| 按键 | 位置 | 证据 |
| --- | --- | --- |
| **电源键** | **PMK8350（SPMI SID 0）PON 块，基址 `0x1300`** | overlay：`qcom,pmk8350@0/pon_hlos@1300/pwrkey`，`compatible = "qcom,pmk8350-pwrkey"`，`interrupts = <0 0x13 0x7 0x3>`，`linux,code = 0x74`(KEY_POWER) |
| 电源键（长按/bark） | 同一 PON：`pwrkey-bark`(irq 0x0)、`pwrkey-resin-bark`(irq 0x2)、`resin`(irq 0x6, code 0x72) | 同上 |
| **音量键** | **PM8350（SPMI SID 1）`pinctrl@8800` 的 GPIO**，经 `gpio_keys/vol_up` 使用 | overlay：`qcom,pm8350@1/pinctrl@8800/key_vol_up` 与 `fragment@19/gpio_keys/vol_up`；`pinctrl@8800` 即 PMIC GPIO 控制块 |

## SPMI 控制器（PMIC 访问入口）

base DTB：`/soc/qcom,spmi@c42d000`，`compatible = "qcom,spmi-pmic-arb"`，`reg`：

```
0xc42d000 0x4000     (chnl：通道命令/数据)
0xc400000 0x3000     (core：配置)
0xc500000 0x400000   (obs)
0xc440000 0x80000    (intr)
0xc4c0000 0x10000
```

PMIC 顺序（同一 base DTB 的 `-debug@N` 节点给出）：`pmk8350@0`(SID 0)、`pm8350@1`(SID 1)、`pm8350c@2`(SID 2)。

## 实现计划（三步）

1. **最小 SPMI 读**：按 `spmi-pmic-arb`（v7/v8 布局）实现 APID 访问，读 PMK8350（SID0）任意一个已知寄存器（如 revision），
   在日志里打印出来 —— 这一步验证 arbiter 通路，成功后再往下。
2. **电源键**：读 PMK8350 PON（0x1300）的 KPDPWR 状态位并轮询（或配置其 IRQ），
   边沿变化时调用 `liuqin_touch_post(LIUQIN_BUTTON_DOWN/UP, LIUQIN_BUTTON_LOCK, …)`。
3. **音量键**：读 PM8350（SID1）`pinctrl@8800` 对应 GPIO 的输入状态，
   映射到 `LIUQIN_BUTTON_VOLUME_UP/DOWN`。

库与 HID 侧已经就绪（`Liuqin Buttons ready`，`BUTTON_DOWN/UP` → Power/Menu/Volume 的 HID usage），
所以只要内核能读到按键，iOS 侧立刻生效。

## 已知风险

- arbiter 的通道/所有者映射由 bootloader 配置，需确认从 fastboot 冷启动后该配置仍有效；
- SM8450 的 arbiter 是较新的版本，寄存器细节需按 `spmi-pmic-arb` 的实现核对；
- 音量键若是「模拟按键」接到 PMIC ADC（KPD/RESIN），读法会不同（DT 里 `resin` 的 code 0x72 值得注意）。

## 2026-10-28 实现：内核里直接读 SPMI（touch173）

硬件位置与 mainline DTS 一致，已用 `yzddmr6/linux-sm8450-liuqin`（`liuqin-6.17`）的
`sm8475-xiaomi-liuqin.dts` 逐条核对：

| 键 | 位置 | 读取地址 |
| --- | --- | --- |
| 电源 | PMK8350 PON（SID 0）`RT_STS` | `0x1310` bit 7（GEN3 布局，`PON_GEN3_KPDPWR_N_SET`） |
| 音量下 | 同一个字节的 bit 6（PON RESIN，DTS 里 `linux,code = <KEY_VOLUMEDOWN>`） | `0x1310` bit 6 |
| 音量上 | PM8350 GPIO6（SID 1，`gpio-keys/key-volume-up`，ACTIVE_LOW） | `0x8d10` bit 0 |

PMIC GPIO 的引脚基址是 `0x8800 + 0x100 * (n-1)`（`pinctrl@8800` 一个节点覆盖全部 12 个 pin，
`pad->base = reg + i * 0x100`），`RT_STS` 在 pin 基址 + `0x10`，bit0 就是引脚电平。

## SPMI 通路（arbiter v7/v8，只有读）

DT 里的 `reg`/`reg-names` 顺序（从本机 base DTB 解出来，不是猜的）：

```
0x0c42d000 0x04000  cfg
0x0c400000 0x03000  core      ← version(0x00)、features(0x04)、APID→PPID 表(0x0b00+4*i)
0x0c500000 0x400000 chnls     ← 写通道（本驱动不用）
0x0c440000 0x80000  obsrvr    ← 读通道：0x8000*ee + 0x20*apid
0x0c4c0000 0x10000  intr
```

- APID 不假设等于 PPID：按 Linux `spmi-pmic-arb.c` 的 v7 分支扫
  `core+0x2000+4*apid`（**不是 `0x0b00`**），`ppid = (reg >> 8) & 0xfff`
  （PPID 是 12 位），`features(0x04) & 0x7ff` 是 bus0 的 APID 个数；
  `features(0x08) & 0x7ff` 是 bus1 的个数，bus1 的 APID 从 bus0 的 count 开始。
- `ppid = (sid << 8) | (addr >> 8)`。
- 读命令：`CMD = (15 << 27) | ((addr & 0xff) << 4) | (len-1)`，
  然后轮询 `STATUS` bit0=DONE，bit1..6 = 失败；数据在 `RDATA0`（前 4 字节）/`RDATA1`。
- ee=0 时 v7 与 v8 的读通道偏移相同（v8 是 `0x40000*ee`），所以两种 arbiter 都能用。
- **只读**：这个移植没有 PMIC 驱动，写 PMIC 寄存器有可能改到电源轨；按键只用读。

内核实现放在 `bsd/dev/liuqin_touch.c` 末尾（该文件是补丁序列里唯一完全同步的文件），
线程 `liuqin_keys_thread` 20 ms 采一次、两次一致才认边沿（40 ms 去抖），
边沿调 `liuqin_touch_post(LIUQIN_BUTTON_*)`；启动参数 `liuqinkeys=0` 可关。

日志形状：

```
LIUQIN KEYS: spmi arbiter version 0x70000000, 512 apids
LIUQIN KEYS: power sid 0 ppid 0x013 apid 12
LIUQIN KEYS: volume up sid 1 ppid 0x18d apid 30
LIUQIN KEYS: volume down sid 0 ppid 0x013 apid 12
LIUQIN KEYS: power reads 0x00
LIUQIN KEYS: power down
```

## 2026-10-28 按键第 235 步：修正 v7 表偏移

`touch173` 的 `apid -1` 根因已找到：v7 的 APID→PPID 表在 `core+0x2000`，
而不是 `0x0b00`；PPID 掩码是 `0xfff`，不是 `0x1fff`。`touch183` 诊断镜像用正确的表
重扫，并实测表读耗时、直接读 `APID 19 / 0x1310`、50 ms 轮询三个键。
详见 `docs/KEYS_HANDOFF.md` 与 `NOTES.md` 第 235 步。

## 交接状态（2026-10-28）

本文是**硬件事实与最初计划**。当前实现的进度、阻塞点和下一步方案已经单独写在
**`docs/KEYS_HANDOFF.md`**（新会话从这里开始）：

- 代码在 `bsd/dev/liuqin_touch.c` 末尾的 PMIC keys 段，**默认关闭**（`liuqinkeys=1`）；
- 已知阻塞：`ppid 0x013` 在 bus 0 的 APID 映射表里找不到（`apid -1`），
  以及这块 arbiter 的 MMIO 读约 **26 ms/次**（扫 3×768 项让启动慢了 83 秒）；
- 当前在用的镜像见 `NOTES.md` §8 与 `docs/KEYS_HANDOFF.md` §0。
