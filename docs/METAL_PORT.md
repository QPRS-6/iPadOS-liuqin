# Native Metal on liuqin (Adreno 730): port plan and state

Date: 2026-10-28.  This is the working plan for the sheng native-Metal stack on
the Xiaomi Pad 6 Pro.  The kernel GPU bring-up itself is already proven
(`gpu1.txt`); this file is about the gap between "the GPU executes CP work"
and "SpringBoard/backboardd composite the UI through it".

## What is already true

- `shim-boot-gpu1-cmddb.img` boots with the corrected
  `sheng-gpu-cmddb-version=1` and the whole kernel chain succeeds:
  `ColdStart ack=0xde`, `OOB GPU_SET`, `GPU context status 0x0`,
  `CP started fence=2`, `CP self-test wrote=0x600dc0de`, `GPU ready` at 76.3 s.
- The kernel publishes `NativeAdrenoGpuService`, an `IOService` with the
  custom `NativeAdrenoABI.h` user client.  `tools/liuqin-gpu-test.cpp` targets
  this service.
- **The UI is still software-rendered.**  `gpu1.txt` shows backboardd and
  SpringBoard running `IOServiceMatching("IOAcceleratorES")` 358 times, every
  one returning `-> 0 found`; `AGXAccelerator -> none`.

## What the original Metal discovery needs

On iOS the Metal framework does not know the native driver.  It looks for a
service whose class is `IOAcceleratorES` (or `IOAcceleratorESLegacy`), reads
`MetalPluginName` from it, loads
`/System/Library/Extensions/<MetalPluginName>.bundle`, and opens the user
client.  sheng's working shape is therefore:

```
kernel: NativeAdreno -> native_adreno_publish() -> AGXAccelerator
        (IOAcceleratorES, MetalPluginName=NativeAdrenoMetal,
         MetalPluginClassName=NativeAdrenoMetalDevice,
         NativeSandboxGrantProcess=backboardd,...)
userland: NativeAdrenoMetal.bundle -> NativeBridgeMetalFrontend
          -> metal2vulkan -> Turnip -> AGXDeviceUserClient
```

The kernel tree on this port already contains the sheng sources for the second
half (`NativeAdrenoPublish.cpp`, `NativeAdrenoAccelerator.cpp`,
`iokit/NativeAdreno/NativeMetal/*.h`), but until now nothing instantiated them:
`native_adreno_publish()` had no caller and `AGXAccelerator` was never
allocated.  `LiuqinStubs.cpp` supplies an `IOAcceleratorES` stub, so this port
does not load the original `com.apple.iokit.IOGPUFamily` kext the way sheng
does.

## Stage 1: publish the accelerator (kernel, this round)

`xnu/patches/0244-liuqin-native-metal-publish.patch`:

- moves `LiuqinStubs.cpp` before `NativeAdrenoAccelerator.cpp` in
  `iokit/conf/files.arm64`, so the stub `IOAcceleratorES` metaclass exists
  before `AGXAccelerator`'s static initializer runs;
- after the CP is up and only with `sheng-gpu-metal=1`, calls
  `native_adreno_publish(vm, execbuf_, &ring_, ...)` with an Adreno 730
  `NativeAdrenoAbiDeviceInfo` (chip id `0x07030001`, 3 MiB GMEM, UBWC 4.0),
  allocates `AGXAccelerator`, sets `MetalPluginName`/`MetalPluginClassName`/
  `NativeSandboxGrantProcess`, attaches it to the platform and registers it;
- falls back to the old `NativeAdrenoGpuClient` path when the publish or the
  class lookup fails, so a failed metal path does not break the existing GPU
  service.

Boot argument: `sheng-gpu-metal=1` (the boot line is 586 bytes, still under
the 607-byte payload limit).

Expected log on success:

```
NativeAdreno: metal publish 0x0
NativeAdreno: accelerator published (IOAcceleratorES)
```

and, later, the `IOAcceleratorES -> 1 found` lines from backboardd.  If the
class order is wrong the guard logs

### metal1 on hardware (2026-10-28)

Confirmed on `metal1.txt`: `metal publish 0x0`, `accelerator published
(IOAcceleratorES)`, `IOAcceleratorES -> 1 found`, `AGXAccelerator`.  Then
backboardd's original IOGPU open (type 1) is refused with `0xe00002c7`
(`kIOReturnNotPermitted`): the sandbox patch is the next requirement.
`0245-liuqin-native-adreno-sandbox-grant.patch` is written and verified,
and `shim-boot-metal2-sandbox.img` (`d8b470db…`) is built but deliberately
not shipped until the plugin bundle exists (the driver would still refuse the
original type-1 open).

The black screen in `metal1` is `LIUQIN FB: display off` at 101 s; the log
continues to 160 s with no panic or GPU fault.  The keys thread is still in
its `waiting 120 s` window (no `LIUQIN KEYS: arbiter version` yet), so the
power key is not polled and cannot wake the panel.  Use
`shim-boot-gpu1-cmddb.img` (`bd44bcf3…`, no accelerator) for daily use until
the plugin image exists.
`accelerator class missing (parent 0/1 own 0/1)` and the old path stays.

## Stage 2: let the plugin open the accelerator

Even with the accelerator published, the plugin's `IOServiceOpen` will be
refused by the platform sandbox.  sheng solves this with
`patches/xnu/zzw-native-adreno-sandbox-grant.patch`, which makes
`NativeSandboxGrantProcess` stand in for the MACF/filter verdict.  This port
does not have that patch yet; it is the next kernel change after the Stage 1
log.

## Stage 3: the userspace plugin

Reference: `~/liuqin-root/refs/UniXNU` (`drivers/NativeMetal`,
`scripts/build-turnip-darwin.py`, `scripts/build-adreno-metal-plugin.py`) and
`~/liuqin-root/refs/UniXNU-sheng` (the sheng device package).

Pieces:

1. Turnip (Mesa freedreno Vulkan) with the Darwin KMD backend, pinned Mesa
   revision + `patches/mesa/turnip-darwin-kmd.patch`;
2. `libnative_metal_bridge.a` (metal2vulkan behind a C ABI);
3. `NativeAdrenoMetal.bundle` (`NativeAdrenoMetalPlugin.mm` +
   `NativeBridgeMetalFrontend.mm` + `AdrenoPM4.o`), ad-hoc signed with 4 KiB
   pages;
4. installation into the iPadOS root and (if AMFI still checks) a trust
   entry.  This root already has `amfi_get_out_of_my_way=1` and
   `cs_enforcement_disable=1`, but the original Metal bundle is loaded by
   `NSBundle` as the render server, so the signature/trust path must be
   tested, not assumed.

The sheng plugin is built for `arm64-apple-ios17.0` with
`MinimumOSVersion=17.0`.  **This root is iOS 16.6.1 (20G81).**  The plugin and
the Metal SPI must be rebuilt/tested for 16.6.1 (or the root must move to a
17.x IPSW); that is the largest compatibility risk in the port.

## Stage 4: Metal device and compositing

Once the plugin opens `AGXDeviceUserClient`:

- the plugin must return a device through `MTLAddDevice` for backboardd;
- the `NativeAdrenoMetal.publish` / `.clients` allow-list decides which
  processes get a device;
- the sheng scanout path (`NativeMobileDisplay`, `GpuScanoutPresenter`) may be
  needed for GPU composition; the current liuqin display is a simple
  framebuffer (`LIUQIN FB`), not the vendor swap chain.

This stage is where "GPU works in the kernel" becomes "the UI is drawn by the
GPU".  It is not a single patch.

## Risks / open questions

- iOS 16.6.1 vs the sheng iOS 17 plugin ABI and Metal SPI.
- The original `IOGPUFamily` is stubbed; the `AGXAccelerator` base is 136 bytes
  and has no operational overrides, which the plugin only uses for
  `newUserClient` + properties.  This must be proven on hardware.
- The A730 `NativeAdrenoAbiDeviceInfo` values (chip id/gmem/UBWC) are from the
  pinned A730 catalog and ladder; Turnip may reject them if a field is wrong.
- The current `NativeAdrenoService` and the sheng `NativeAdrenoPublish` are two
  generations of the driver; Stage 1 deliberately keeps both paths and uses
  the publish path only when `sheng-gpu-metal=1`.
- The userland build needs Mesa/Turnip/metal2vulkan inputs that are not in
  this repo; they must be fetched at the pinned revisions.

## Artifacts (Stage 1)

| artifact | bytes | sha256 |
| --- | --- | --- |
| kernel | 17,037,096 | `7cb67731eceaf1914baa220cf7ade2ef01b71523eda2f1037d9734da4c6410a6` |
| payload | 25,440,608 | `13c0131b96cb76c6f885d9d61b1e3f718256f4916aee7f2399fb46ce1bd7a80c` |
| boot image | 32,018,432 | `08515212e90622a91084ca1cbca2c98e9c5d8dc3c38683da1e24b360fb9be427` |

The Stage 1 image is a **diagnostic**: the accelerator is published but the
plugin does not exist yet, so the expected user-facing result is still the
software-rendered UI; the log is what proves the kernel half.

## Stage 3: the plugin test mode (metal3, 2026-10-28)

Turnip, the metal2vulkan bridge and the plugin bundle are now built for
**iOS 16.0** (the root is 16.6.1, not sheng's 17.x):

| piece | bytes | notes |
| --- | --- | --- |
| `libvulkan_freedreno.dylib` | 17,720,928 | `LC_BUILD_VERSION minos 16.0` |
| `libnative_metal_bridge.a` | 308,599,096 | Rust metal2vulkan bridge, minos 16.0 |
| `NativeAdrenoMetal` (bundle exe) | 22,090,992 | 4 KiB-page ad-hoc signature |
| bundle Turnip | 17,193,520 | `dlopen`ed next to the plugin |

The first hardware step deliberately installs the bundle **without**
`NativeAdrenoMetal.publish`. The plugin's test mode then runs
`NativeAdrenoUserTest` (BO alloc/map, userspace IB, submit, WaitFence, read
back) and returns nil, so the system keeps the software renderer. The kernel
also logs the plugin's result (`0246-liuqin-native-metal-diag-log.patch`):

```
NativeAdreno: metal client open type 0x4147 proc backboardd
NativeAdrenoMetal: backboardd s<step> e<error> v<verified>/<verifiedWc> ...
NativeAdrenoMetal: T ...          # Turnip phase, if reached
```

Artifacts:

| artifact | bytes | sha256 |
| --- | --- | --- |
| kernel | 17,037,152 | `44c81d522aee82c4df61033f8673a7565c9ab0ead7e6d78414d995785e30fbbb` |
| payload | 25,440,608 | `771d6700a7a1b0f3a0f6b5b4dcc82528fc61f74d79f3a5e6f2cdd2e328407825` |
| `shim-boot-metal3-diag.img` | 32,018,432 | `329fc13f24465ea3169ddf2a545396b6215c38d1f69369f2e87187c9c67dddd9` |
| root sparse diff | 39,126,964 | `52e4bcee3cdfea4d66d99d0b8b23fce3658fe392159ea90a14d52b564f73bdd8` |

The sparse diff is based on `ipad71-root-wrap18.img`
(`bf7ecd5a…`); the matching full 16 GiB image is
`~/liuqin-root/ipad71-root-metal-nopublish.img`.  If the device root is not
wrap18, flash the full image instead.  Only after the test passes should
`NativeAdrenoMetal.publish` (and optionally `.clients`) be added beside the
bundle for GPU composition.

## Pulling the log from Android (no recovery boot)

`tools/liuqin-log-android.sh` reads the persistent LIUQIN log ring from a
rooted Android boot.  The ring is the last 1025 4 KiB blocks of the `hfs`
partition (the iPadOS root); Android does not touch that partition, so the
log from the last iPadOS boot is still there.

```sh
adb push tools/liuqin-log-android.sh /data/local/tmp/
adb shell su -c 'sh /data/local/tmp/liuqin-log-android.sh'
adb pull /sdcard/Download/liuqin-log-<timestamp>.txt
```

The script decodes the header/ring on the device with toybox `od`/`dd`/
`head`/`tail` (no Python).  If the `hfs` by-name link is missing:

```sh
adb shell su -c 'DEV=/dev/block/sda36 sh /data/local/tmp/liuqin-log-android.sh'
```

`tools/liuqin-log-android-pull.sh` does push/run/pull in one step and writes
the host-side file.  Both need root; without root the block device cannot be
opened.  The old recovery path (`tools/liuqin-log-pull.sh`) remains the
fallback.
