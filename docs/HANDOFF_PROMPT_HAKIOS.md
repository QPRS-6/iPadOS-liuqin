# 交接提示词：Xiaomi Pad 6 Pro（liuqin / SM8475 / Adreno 730）iPadOS/XNU —— HakiOS 链路

把下面整段贴给下一个会话的 agent 作为第一条消息。

---

你是接手 Xiaomi Pad 6 Pro（liuqin / SM8475 / Adreno 730）iPadOS/XNU port 的 agent。

**先读**（不要跳过）：
- `iPadOS-liuqin/NOTES.md` 第 370 步（当前）
- `iPadOS-liuqin/docs/HANDOFF_2026-10-31_GPU.md`

机主不读代码。所有汇报必须说清：**改了什么 / 为什么 / 看哪行日志**，附 sha256，并说明 root 没动。

## 当前目标

把 GPU 修到能用（能渲染、能接进 UI）。已决定**换成 HakiOS 链路**：

```
Android fastboot
  → HakiOS UEFI（Mu-Silicium + HakiOS 改动，或 HakiOS U-Boot）
  → xnuloader
  → XNU（port 的 XNU/ADT/fwarea）
  → launchd
```

GPU 的 secure 初始化放进 HakiOS UEFI 的 QcomPkg/SCM。kexec（native Linux 先启动）保留为 fallback。

## 已经确定的事实（不要再重试）

GPU 引擎在 secure 模式，事务带 **SID 27**（`sid=0x1b08`）。所有非安全出口都试完了：

| 尝试 | 结果 |
| --- | --- |
| 写 SMMU SMR 槽 | 只有 slot1/2 可写；写 SID 8/27/0x1b08 都 data abort |
| `qcom_scm_kgsl_set_smmu_aperture` | 返回 0，但引擎仍 SID 27 |
| zap（PAS init/auth，`a730_zap`） | 成功（`TRUSTZONE RUNS THE ZAP SHADER`），但 secure SMR slot3..5 仍空 |
| `CP_SET_SECURE_MODE 0` | CP 回 `CP_OPCODE_ERROR`（rptr 停住，cp_int 0x1） |
| zap 之后写 `RBBM_SECVID_TRUST_CNTL (0xf400)=0` | 仍 data abort（PC 653，ESR 0x96000010） |
| `qcom_scm_restore_sec_cfg(PAS_GPU,0)` | QSEE 返回 `0xfffffffffffffffc`（-4） |

XNU 的 NativeAdreno：GMU/CP/fence/A0/A11 都健康（A0/A11 `result=0`），但 `filled 0/512`；客户端 M/T 全错，`smmu fault sid=0x1b08`。

**结论**：GPU 的 secure 配置只能由 first-stage（stock kgsl / freedreno / UEFI+QSEE）做，XNU 自己做不了。

其他事实：
- 稳定回退：`shim-boot-metal96-post-zap-smr.img`（只读）、`metal95-zap-only`、`metal79-ccu`。
- root 一直是 `f7043229b7a628ea69aa171a606fad6d01120a21821652b32814a4b6213e565f`，**不要动**。
- 6 Pro 的原生 Linux（Ubuntu）**能 boot、几乎全部硬件可用**；可作为 kexec fallback 的 host。
- custom Android 17 的内核 `5.10.237-Fuutao-Qn_miao` **闭源**，`CONFIG_KEXEC` 未开。
- HakiOS 只支持 **redwood（POCO X5 Pro 5G，SM7325，Adreno 642L）**；6 Pro 的 SM8475/a730 要自己 port。
- HakiOS 的 XNU/driver 可参考，但 port 的 XNU（liuqin、NativeAdreno、display/touch）更靠前；H2 优先加载 port 的 XNU。

## 迁移计划（H0–H4）

### H0：建 `devices/liuqin`（参考 redwood）

- clone `hakios-xnu-kernel`，`tools/hakios-device.py new liuqin`；
- 填 SM8475 SoC 层（`soc/`）、`liuqin.config`、`pull-firmware.sh`、UEFI platform、status；
- 用 `tools/hakios-device.py sync` 拿 `hakios-uefi` / redwood 作参考。

### H1：6 Pro 最小 HakiOS UEFI

- `hakios-uefi`（Mu-Silicium + HakiOS）加 SM8475/liuqin platform；
- `fastboot boot` 起来，GOP/串口、UFS 可读；
- 这一步不碰 XNU。

### H2：xnuloader → port 的 XNU

- xnuloader 加载 port 的 XNU（Mach-O `kernel.development.liuqin`）+ `liuqin.adt` + `fwarea.bin`；
- cmdline 用 `tools/boot-cmdline-gpu.txt`，`sheng-gpu-bringup=0`；
- 目标：XNU 起来，串口/屏幕有日志；A11 仍 0 没关系；
- 如果 xnuloader 的 payload 格式不兼容，先 chainload port 的 `shim`（`shim/Image` + payload）。

### H3：HakiOS UEFI 里做 GPU secure init

- QcomPkg/SCM：PAS init/auth（`a730_zap`）、`restore_sec_cfg`、`set_smmu_aperture`、trust/SMR；
- 再启动 XNU，`sheng-gpu-bringup=0`，看 A11 是否开始填、`sid` 是否还是 `0x1b08`；
- 这是 HakiOS 路线成败关键；QSEE 可能同样 -4。

### H4：客户端 / Metal / UI

- A11 通了之后，回到 turnip/Metal、display、touch、UI。

## 关键文件 / 产物

- XNU 源码：`~/liuqin-build3/xnu`（`iokit/NativeAdreno`、`bsd/dev/liuqin_adreno.c`）
- shim：`iPadOS-liuqin/shim`；payload：`shim/out/payload-metal79-ccu.bin`；ADT：`shim/out/liuqin.adt`；cmdline：`tools/boot-cmdline-gpu.txt`（H2/kexec 时把 `sheng-gpu-bringup=1` 改成 `0`）
- 稳定 boot 镜像：`shim/out/shim-boot-metal96-post-zap-smr.img`（只读）、`metal95-zap-only`、`metal79-ccu`
- root：`/Users/qprs/liuqin-root/ipad71-root-wrap16-gpu11-wr.img`，17,179,869,184 B，`f7043229…`，不动
- HakiOS：`/tmp/hackios-xnu`（clone）；`hakios-uefi` / `hakios-sm7325` 通过 `tools/hakios-device.py sync` 拉

## 工作规则

- **只有 agent 构建/算哈希 boot 镜像**；机主只刷 boot 镜像；root 不动。
- 日志只认机主回传的 `.txt`；只有照片要明说“这是照片”。
- 事实与推断分开；**不要重复上面“已确定的事实”里的失败尝试**。
- 每次改完更新 `NOTES.md` 当前步和 `HANDOFF` 的 §2/§5/§6；kernel sha256 记进 NOTES。
- 汇报格式：改了什么 / 为什么 / 看哪行日志；附 sha256；root 不变。

## 下一步

从 **H0** 开始：

1. `tools/hakios-device.py new liuqin`，参考 `devices/redwood`；
2. 确认 SM8475 的 SoC 层、`liuqin.config`、`pull-firmware.sh`、UEFI platform 要填什么；
3. 确认 HakiOS UEFI 能否在 6 Pro 上 `fastboot boot` 起来（GOP/串口/UFS）。
