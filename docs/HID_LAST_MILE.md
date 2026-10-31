# HID last mile: what the iPadOS 16 userland actually waits for

Assessment done against Apple's open-source **IOHIDFamily-1915.140.3** (the
version ios-sweet compiled, and the closest published source to this root's
16.6.1/16.7.16 userland) plus the kernel's own IOKit traces of backboardd.

## What the kernel sees backboardd ask for

`shim-boot-touch50..54b` logs every `IOServiceGetMatchingServices` /
`IOServiceAddMatchingNotification` the userland makes (patch 0049).  The HID ones
are exactly these:

```
IOProviderClass = IOHIDDevice, PrimaryUsagePage = 0xff00, PrimaryUsage = 0xf   (vendor multitouch)
IOProviderClass = IOHIDDevice, DeviceUsagePairs = <array ...>
IOProviderClass = IOHIDDevice, PrimaryUsage = <...>
IOProviderClass = IOService,  IOPropertyMatch = { HIDServiceSupport = true }
```

Nothing ever matches `IOHIDEventService` or `IOHIDEventSystem` by name, and
nothing ever calls `IOServiceOpen` on our `IOHIDEventService`s - although the
`HIDServiceSupport` notification does deliver them (kernel match 2/2, ping sent,
user space drains the service object, touch51/52/53).

## Why porting `IOHIDEventSystem` would not have helped

`IOHIDFamily/IOHIDEventSystem.cpp` (11 KB) is **not** an event router and has
**no user client at all** (zero `newUserClient`/`UserClient` references).  It is
a debugging broker: on start it registers notifications for the
`IOHIDEventService` class, opens every service instance (`service->open(this,
...)`) and `HIDLog`s the events it receives.  The real routing is in
`IOHIDEventService::dispatchEvent()`, which walks `_clientDict` - i.e. exactly
the clients that called `open()`, which is what our stub already implements.

The `IOHIDEventSystemClient` that user space creates is served, on macOS, by
`IOHIDSystem/IOHIDUserClient.cpp` (`IOHIDEventSystemUserClient`,
`kIOHIDEventSystemConnectType`) - a kext that does not exist on iOS.  So on iOS
the event delivery to backboardd must happen through per-service user clients
(`IOHIDEventServiceUserClient`, 'esuc'), which is what this port already has
(compiled unchanged from Apple's source), or through the `IOHIDDevice` path.

## The remaining hypothesis

Backboardd's HID stack arms three `IOHIDDevice` matches and one
`HIDServiceSupport` match.  It opens neither of our services.  The most likely
reading is that on iOS 16 the *digitizer* is an **IOHIDDevice** (vendor usage
page 0xff00 / usage 0xf, as apple's multitouch devices publish) and that the
`HIDServiceSupport` notification is bookkeeping for other service kinds.  Under
that reading the missing kernel piece is `IOHIDDevice` + its user client, not
`IOHIDEventSystem`.

## Next probe (cheap, decides between the two readings)

Publish a **stub class literally named `IOHIDDevice`** carrying
`PrimaryUsagePage 0xff00 / PrimaryUsage 0xf` and a user client that logs every
connect type, and start it late (after backboardd's notifier is armed) - the
same trick that made the `HIDServiceSupport` ping visible.

- user space connects with some type: the `IOHIDDevice` path is live, and the
  real work is porting `IOHIDDevice` + `IOHIDLibUserClient`;
- nothing connects: the userland wants something else entirely, and the next
  step is disassembling its HID stack (`ipsw dyld` + BackBoard.framework).

## Probe built: `shim-boot-touch55.img`

Patch `0202-liuqin-hid-device-probe.patch` adds a kernel class literally named
`IOHIDDevice` (subclass of `IOService`), carrying `PrimaryUsagePage = 0xff00`,
`PrimaryUsage = 0xf`, `Transport = SPI`, `Product = Liuqin Touch Device` and a
one-entry `DeviceUsagePairs` array, and started late by the same timer that
relaunches the HID event services.

What to look for in the log:

```
LIUQIN HID: IOHIDDevice stub relaunched
LIUQIN HID: IOHIDDevice stub newUserClient type 0x...      <- the answer
LIUQIN HID: IOHIDDevice stub started (0xff00/0xf)
```

- a `newUserClient type` line: the userland's HID stack does open `IOHIDDevice`
  services, so the real work is porting `IOHIDDevice` + its user client
  (`IOHIDLibUserClient`) - and the type tells us which one;
- none: the userland wants neither `IOHIDEventService` nor `IOHIDDevice`, and the
  remaining path is disassembling its HID stack (`ipsw dyld` on
  BackBoard.framework).

## Result (touch55 boot): the userland DOES open `IOHIDDevice`

```
LIUQIN HID: IOHIDDevice stub started (0xff00/0xf)
LIUQIN HID: IOHIDDevice stub relaunched
LIUQIN IOK: notification ping type 0x65 for IOHIDDevice id 0x1000001c1
LIUQIN HID: IOHIDDevice stub newUserClient type 0x484944
```

`0x484944` is `0x00484944` = `kIOHIDLibUserClientConnectManager` (`IOHIDLibUserClient.h`
even spells it `/* HID */`), and `IOHIDDevice::newUserClient()` accepts *only*
that type, handing it to `newUserClientInternal()` -> **`IOHIDLibUserClient`**.

So the remaining gap is now exactly bounded: the kernel needs a real
`IOHIDDevice` + `IOHIDLibUserClient`, and our touch has to appear as a HID
device publishing a **multitouch report descriptor** and input reports.  The
`IOHIDEventService` path is a dead end for the digitizer.

What the port already has (`iokit/Kernel/arm/hid/`): the IOHIDEvent stack
(`IOHIDEvent`, `IOHIDEventServiceQueue`, `IOHIDEventServiceUserClient`, all the
`IOHID*Types/Keys/UsageTables` headers, compiled unchanged from Apple) plus our
`LiuqinHID.cpp`.  What is missing is the *device* half.

From Apple's IOHIDFamily-1915.140.3:

| file | size | role |
| --- | --- | --- |
| `IOHIDDevice.cpp` | 95 KB | the device object: properties, queues, reports |
| `IOHIDLibUserClient.cpp` | 78 KB | the 'HID' user client the userland opened |
| `IOHIDElementPrivate.cpp` | 73 KB | element tree built from the report descriptor |
| `IOHIDDeviceElementContainer.cpp` | 2 KB | element lookup |
| `IOHIDDescriptorParser*`, `IOHIDReportDescriptorParser*` | - | descriptor parsing |
| `IOHIDFamilyPrivate.*`, `IOHIDWorkLoop`, `IOHIDInterface` | - | support |

Roughly 250 KB of Apple source plus headers, and `IOHIDDevice.cpp` includes
`<IOKit/hidsystem/IOHIDSystem.h>` (a macOS-flavoured header) which will need a
stub or a shim on this tree.

**Second half of the problem**: the report format.  The userland parses the
input reports of the vendor multitouch device (usage page 0xff00 / usage 0xf)
itself; whatever descriptor we publish has to match its expectations.  The
sequence that de-risks it: get `IOHIDLibUserClient` answering, log exactly what
the userland asks for (report descriptor read, queue setup, first reports), and
then shape the descriptor/reports from that traffic.

## touch56: log the userland's HID-device call sequence

Instead of porting 250 KB blind, `shim-boot-touch56.img` (patch
`0203-liuqin-hid-device-client-probe.patch`) gives the `'HID'` connect type a
logging `IOUserClient` that names every selector from
`kIOHIDLibUserClient*` (`deviceIsValid`, `open`, `createQueue`,
`addElementToQueue`, `startQueue`, `getReport`, `setReport`, `getElementCount`,
`getElements`, ...) and reports the argument shapes:

```
LIUQIN HID: IOHIDDevice stub gave out a logging client
LIUQIN HID: device client sel 0 (deviceIsValid) sc=0 sci=0 out=0 ...
LIUQIN HID: device client sel 3 (createQueue) sc=3 sci=0 ...
LIUQIN HID: device client sel 14 (getElementCount) ...
```

Every method returns `kIOReturnUnsupported`, so the sequence stops at the first
call the userland cannot live without - which is exactly the information wanted:
it names the API the real port must implement first, and the argument layout
(scalar counts / structure sizes) fixes the protocol.

**Result (touch56 boot): the client is handed out and then nothing happens** -
`IOHIDDevice stub gave out a logging client` with zero `device client sel`
lines.  So the userland opens the device and gives up *before* its first
external call, i.e. its pre-flight check lives elsewhere: most likely in the
device's registry properties (`ReportDescriptor`, usages, transport) or in the
returned client's early state.

## touch57: what does it read first?

`shim-boot-touch57.img` (patch `0204-liuqin-hid-device-property-probe.patch`)
adds, on both the device and the logging client:

- `LIUQIN HID: IOHIDDevice property get <key>` for every `getProperty()` the
  userland performs on the stub (registry reads land here),
- `LIUQIN HID: device client closed` / `died` so a give-up is visible,
- on top of the existing selector log.

**Result (touch57 boot): only generic IOKit keys are read.**  Right after the
client is handed out the stub sees `IOName`, `IOPlatform*Action`,
`IOBusyInterest`, `IORematchPersonality`, `IOCFPlugInTypes` and
`IOUserServiceProperties` - the property set that IOKit's own service/user-client
machinery (`IOService::open`, CFPlugIn lookup, DriverKit user-service checks)
reads for *any* service.  No HID-specific key (`ReportDescriptor`,
`PrimaryUsage*`, `Transport`) is read, and the client is never closed, so the
decision to do nothing happens elsewhere - most likely the opener is not the
process we assumed.

## touch58: who opens it?

`shim-boot-touch58.img` (patch `0205-liuqin-hid-device-open-pid.patch`) logs the
opening task's pid/name:

```
LIUQIN HID: IOHIDDevice stub newUserClient type 0x484944 by <procname>
```

That single word decides the next step:

**Result (touch58 boot): the opener is `gamecontrollerd`.**

```
LIUQIN HID: IOHIDDevice stub newUserClient type 0x484944 by gamecontrollerd
```

Backboardd did receive the device (nine `notification ping type 0x65 for
IOHIDDevice`, plus a `drain -> IOHIDDevice`), i.e. it sees the service and
declines to open it, while the game-controller daemon (which also matches
`IOHIDDevice` + `DeviceUsagePairs`) opens it and then does nothing.

## touch59: publish it as `AppleMultitouchDevice`

On iOS the digitizer is an `IOHIDDevice` **subclass** called
`AppleMultitouchDevice`.  `IOProviderClass = IOHIDDevice` matches subclasses
too, so the userland can (and likely does) filter on the concrete class name
after the match.  `shim-boot-touch59.img` (patch
`0206-liuqin-hid-multitouch-device.patch`) renames the stub:
`AppleMultitouchDevice : public IOHIDDevice`, keeping the same usages,
`DeviceUsagePairs`, the logging user client, the pid log and the property log.

**Result (touch59 boot): the class name is not the filter either.**
`AppleMultitouchDevice started` and notifications fire for that class, but the
only client is still `gamecontrollerd`, which now reads
`GameControllerSupportedHIDDevice` and `ProductID` before giving up - while
backboardd keeps draining and never opens.

## touch60: publish the missing `IOHIDEventSystem`

The userland's HID stack contains the class name `IOHIDEventSystem`, and this
kernel publishes nothing with that name.  `shim-boot-touch60.img` (patch
`0207-liuqin-hid-event-system-probe.patch`) publishes a stub
`IOHIDEventSystem : IOService` with the same logging user client, the same
property log and the pid log:

```
LIUQIN HID: IOHIDEventSystem stub started
LIUQIN HID: IOHIDEventSystem newUserClient type 0x... by <proc>
LIUQIN HID: IOHIDEventSystem property get <key>
```

**Result (touch60 boot): the stub is inspected, never opened.**  Its properties
are read (`IOName`, platform actions, `IOBusyInterest`, `BSD Name`) and no
`newUserClient` arrives, so `IOHIDEventSystem` is an internal user-space name,
not a kernel service.  The same boot also showed the device stub being asked for
`Transport`, `VendorID` and `ProductID` (gamecontrollerd's identification path),
and gamecontrollerd remained the only opener.

## touch61: who actually drains the HID notifications?

Every claim so far ("backboardd sees the service, then declines") rests on the
*assumption* that backboardd is the process draining backboardd's notifiers.
`shim-boot-touch61.img` (patch `0208-liuqin-iokit-drain-process.patch`) logs the
draining task:

```
LIUQIN IOK: notification drain by <proc> -> <class><name> (armed N)
```

**Result (touch61 boot): backboardd itself drains all three.**

```
LIUQIN IOK: notification drain by backboardd -> LiuqinHIDButtons...
LIUQIN IOK: notification drain by backboardd -> LiuqinHIDTouch...
LIUQIN IOK: notification drain by backboardd -> AppleMultitouchDevice...
LIUQIN IOK: notification drain by SpringBoard -> AppleMultitouchDevice...
```

So backboardd's HID stack is alive, receives both notifications, takes the
service objects - and opens neither.  Its decision therefore rests on what the
services look like, and the only remaining observable is which properties it
reads before deciding.

## touch62: which properties does *backboardd* read?

`shim-boot-touch62.img` (patch `0209-liuqin-hid-property-reader.patch`) adds the
reading process to the property log:

```
LIUQIN HID: IOHIDDevice property get Transport by <proc>
```

**Result (touch62 boot): nobody does a HID-specific check on our device.**

| reader | what it reads |
| --- | --- |
| `backboardd` | `IOCFPlugInTypes`, `IOUserServiceProperties` (generic only) |
| `SpringBoard` | `IOCFPlugInTypes`, `IOUserServiceProperties` |
| `StandaloneHIDAud` | `IOCFPlugInTypes`, `IOUserServiceProperties` |
| `gamecontrollerd` | `VendorID`, `ProductID`, `Transport` (controller identification) |

So the clients that drain our services take the object and stop; no process ever
asks a digitizer question.  That points at structure rather than properties: on
a real device the event service is the **driver attached to the IOHIDDevice**
(`IOHIDEventDriver` on the nub) - backboardd finds that service, whose provider
chain contains the device.

## touch63: publish the event service under the device

`shim-boot-touch63.img` (patch `0210-liuqin-hid-touch-under-device.patch`)
changes only the parent: the relaunched `AppleMultitouchDevice` now owns the
relaunched `LiuqinHIDTouch` (device -> event service), instead of both being
children of IOResources.

```
LIUQIN HID: multitouch device relaunched
LIUQIN HID: touch relaunched under the device
```

**Result (touch63 boot): the re-parented service is still only drained.**
`touch relaunched under the device` and backboardd drains
`LiuqinHIDTouch` twice, but the only `newUserClient` in the whole boot is
`0x484944 by gamecontrollerd`, once.  The provider chain is not the gate either.

## Where that leaves it

Proven, with on-device evidence:

| layer | state |
| --- | --- |
| NT36532 firmware download/boot | works (three consecutive boots, `state 0xa0`) |
| frame decode / coordinates | works (`finger 1 ev 2 raw ...`, scaled to the panel) |
| HID service publication | works, matched by backboardd's own dictionary (2/2) |
| notification delivery | works (ping 0x65, drained by `backboardd`) |
| `IOHIDDevice` path | works end to end for `gamecontrollerd` |
| class name / provider chain / properties / 16.7 userland | all ruled out |

## The answer: `MultitouchSupport` attaches through CFPlugIn, not through us

Disassembly of the userdata root's dyld cache pinned the string
`HIDServiceSupport` to **`/System/Library/PrivateFrameworks/MultitouchSupport.framework`**
(not BackBoard, not IOKit - IOKit only carries a second copy).  The strings
around it spell out the whole attach protocol:

```
AppleMultitouchDummyV2
IOCFPlugInTypes
LocationID
Event Driver Properties
IOHIDInterface
IOHIDEventService
HIDServiceSupport
0516B563-B15B-11DA-96EB-0014519758EF
AppleMultitouchDriver.kext/Contents/PlugIns/MultitouchHID.plugin
```

`MultitouchSupport` therefore does what macOS does: `IOCreatePlugInInterfaceForService()`
on the candidate service with that UUID, which loads the plugin bundle from the
kext and instantiates it against the service - and *that plugin* is what talks to
the device through the `'HID'` user client.  A service without `IOCFPlugInTypes`
cannot be attached at all, which is exactly what all our probes saw.

The root image ships the plugin at
`/System/Library/Extensions/AppleMultitouchSPI.kext/PlugIns/MultitouchHID.plugin`.

So `shim-boot-touch64.img` (patch `0211-liuqin-hid-cfplugin-types.patch`) makes
`AppleMultitouchDevice` publish

```
IOCFPlugInTypes = { "0516B563-B15B-11DA-96EB-0014519758EF" :
                    "AppleMultitouchSPI.kext/PlugIns/MultitouchHID.plugin" }
```

**Result (touch64 boot): the property is published and still nothing attaches.**
`StandaloneHIDAud` now reads `IOCFPlugInTypes` from the stub, but the only
`newUserClient` remains `0x484944 by gamecontrollerd` - `MultitouchSupport`
never calls in, so publishing the plugin mapping alone is not the trigger.

That leaves two readings, and the ios-sweet answer decides between them:

1. on the 16.6.1 userland, `MultitouchSupport` is only reached for devices the
   *kernel driver* (AppleMultitouchSPI) has already claimed, so a hand-made
   IOHIDDevice cannot enter that path no matter what it publishes;
2. the attach is driven from `backboardd`'s HID stack through an
   `IOHIDEventService` (what ios-sweet does), and 16.6.1's stack asks for
   something we still do not publish.

Not proven, and no longer reachable by black-box probing: **why backboardd's HID
stack declines to open any service we publish**.  Every observable is accounted
for - it drains the notification, reads no digitizer property, never calls
`newUserClient` - so the decision lives inside the user-space HID library, and
reading that binary (or asking the ios-sweet author what their port does
differently) is the only way forward.

## Two working ports, two different strategies

Both reference projects run the **unmodified iOS userland** and both have touch;
neither ports IOHIDFamily the way I was about to.

### hackios (iOS 16.5, XNU 8796) - inject from inside backboardd

- kernel: `bsd/dev/redwood_input.c`, **227 lines** - a `/dev/redwood-touch`
  character device with a ring of `struct redwood_touch {time, type, finger, x,
  y}`; producers call `redwood_touch_post()`, readers block in `read()`.
- userland: `libredwood.dylib` (MIT) injected into **backboardd** with
  `DYLD_INSERT_LIBRARIES`.  It reads the device and feeds iOS through the
  **virtual HID service API**:

```c
IOHIDEventSystemClientCreateWithType(0, HID_CLIENT_SIMPLE /*4*/, attrs);
IOHIDEventSystemClientScheduleWithDispatchQueue(client, queue);
IOHIDVirtualServiceClientCreateWithCallbacks(client, attrs, &callbacks_v2, ...);
IOHIDEventRef e = IOHIDEventCreateDigitizerEvent(...);
IOHIDEventAppendEvent(e, IOHIDEventCreateDigitizerFingerEvent(...), 0);
IOHIDVirtualServiceClientDispatchEvent(service, e);
```

All three symbols exist in this root (checked in the 16.6.1 dyld cache), and the
code also marks edge contacts (`kIOHIDDigitizerEventFromEdgeTip`) which is what
the home/back swipes need.

### UniXNU (sheng) - do it properly in the kernel

`ShengHidInput/NativeHidInput.cpp` publishes an **`IOHIDDevice` with a real
report descriptor** (report 3 = ten-finger touch screen digitizer) and lets
Apple's **`IOHIDEventDriver`** (IOHIDFamily 2031.120.4, prelinked into their
kernel) parse it and dispatch events.  This is the "real" architecture, and it
needs the whole IOHIDFamily device/driver/user-client stack ported - the 250 KB
job.

### Progress: the kernel half of the hackios route is in

`shim-boot-touch65.img` (patch `0212-liuqin-touch-device.patch`) adds:

- `bsd/sys/liuqin_input.h` - `struct liuqin_touch_sample {time, type, finger, x, y}`
  with positions normalised to 0..65535;
- `bsd/dev/liuqin_input.c` - `/dev/liuqin-touch`, a 512-record ring: `read()`
  blocks for samples, `write()` accepts records so touches can be replayed from
  user space without touching the panel;
- `bsd/dev/liuqin_touch.c` posts `DOWN`/`MOVE`/`UP` (with the last position) for
  every decoded contact, next to the existing `liuqin_hid_touch()` call;
- `liuqin_input_init()` runs from the BSD bring-up that already calls
  `liuqin_adreno_init()`.

The kernel log line to look for is `liuqin-touch: /dev/liuqin-touch ready`.

### The user-space half is built too

- `host/liuqin_touch_inject.c` (adapted from hackios' MIT `inject.c`): creates the
  virtual touch-screen service (digitizer page/usage, `DisplayIntegrated`) and a
  consumer-control service for the buttons, re-creating the service until the
  event server opens it, then reads `/dev/liuqin-touch` and dispatches
  `IOHIDEventCreateDigitizerEvent` collections with edge-tip marking.
- `tools/build-inject.sh`: cross-builds `shim/out/libliuqintouch.dylib`
  (arm64, iOS 16) against a generated stub `.tbd` for IOKit's private symbols.
- The root image `ipad71-root-touch.img` carries the dylib in `/usr/lib` and
  `DYLD_INSERT_LIBRARIES` in `com.apple.backboardd.plist`; the sparse flash image
  is `~/liuqin-root/ipad71-root-touch.simg` (8.0 GiB).
- `host/inject.md` has the recipe and the log lines to expect.

**First boot with the injected image: the kernel device came up, the library did
not run.**  Apple's open dyld source (`DyldDelegates.cpp`) shows why: dyld calls
`amfi_check_dyld_policy_self()` at startup, and *if that call fails it zeroes the
output flags*, after which `pruneEnvVars()` deletes every `DYLD_*` variable.  A
kernel with no mac policy named `AMFI` answers `ENOPOLICY`, so
`DYLD_INSERT_LIBRARIES` is pruned before it is ever read.

hackios' `bsd/kern/kern_minimac.c` had the answer: register the policy as
**`AMFI`** and implement `mpo_policy_syscall` for request **0x5a** (argument
`{flags in, pointer to flags out}` -> answer `0xff`, everything allowed) and
**0x60** (boot-arg state -> `0x86`).  Patch
`0213-liuqin-amfi-dyld-policy.patch` does exactly that in this port's
`bsd/kern/liuqin_amfi.c`, and logs the first such call.

### Consequence for this port

The hackios route needs no IOHIDFamily work at all: our driver already decodes
frames, so the remaining pieces are

1. a kernel character device exposing contact samples (adapt `redwood_input.c`),
2. a small dylib (adapt their MIT-licensed `inject.c` touch half) that turns the
   samples into digitizer events through the virtual service API,
3. injecting that dylib into backboardd.

## Cost picture

| option | work | risk |
| --- | --- | --- |
| port `IOHIDDevice` + user client | ~100 KB of Apple source + descriptor plumbing | medium; userland may also want report-descriptor parsing in kernel |
| port `IOHIDEventSystem` | 350 lines | **useless** - no user client, no routing |
| RE the userland attach path | hours with `ipsw`/`otool` on BackBoard.framework | low, but only produces knowledge |
| ask ios-sweet how theirs attaches | minutes (issue/PR) | unknown latency |

