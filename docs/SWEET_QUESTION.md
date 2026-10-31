# Question for ios-sweet (draft, ready to paste into an issue)

**Title:** How does your port get HID input delivered — does backboardd open your
`SweetHIDTouch` user client, and what publishes `IOHIDEventSystem` on your kernel?

Body:

> Hi — first of all thank you for publishing ios-sweet, patch 0058 (the
> IOHIDEventService/queue/user-client trio compiled unchanged from
> IOHIDFamily-1915.140.3) was the only usable reference for my port.
>
> I am bringing up an SM8450 Xiaomi (liuqin) with xnu-8796.141.3 and an arm64
> iPadOS 16.6.1 (20G81) userdata root. Touch driver, firmware and coordinate
> decoding all work; the HID events reach `_clients`-style dispatch, and my
> `IOHIDEventService` services are matched by backboardd's own dictionary
> (`IOProviderClass = IOService`, `IOPropertyMatch = {HIDServiceSupport = true}`),
> pinged (`kIOServiceMatchedNotificationType`) and *drained by backboardd itself*
> — but backboardd never calls `newUserClient` on them, so nothing is delivered.
>
> What I verified on the device:
>
> - the kernel-side match with backboardd's exact dictionary finds both of my
>   services (`selftest: HIDServiceSupport match found 2 services, 2 ours`);
> - `IOUserNotification` pings for them are sent and `copyNextObject()` is called
>   by `backboardd`;
> - a stub `IOHIDDevice`/`AppleMultitouchDevice` (`PrimaryUsagePage 0xff00`,
>   `PrimaryUsage 0xf`) is opened by `gamecontrollerd` with connect type
>   `0x484944` (`kIOHIDLibUserClientConnectManager`) — so the 'HID' user-client
>   path works, backboardd just never uses it on my objects;
> - backboardd reads no HID property from those objects (only `IOCFPlugInTypes`
>   / `IOUserServiceProperties`), and neither class name
>   (`IOHIDDevice`/`AppleMultitouchDevice`), provider chain
>   (`IOResources` vs. device-owned), late publication nor a `/product` node
>   changes that;
> - the 16.7.16 (20H392) userland has the identical HID surface as 16.6.1
>   (`IOHIDEventServiceOpenService` absent in both, `IOHIDEventSystem*` present),
>   so the userland version is not the difference.
>
> For reference: your notes say the GT9896 comes in over I2C/GENI and is
> delivered as HID digitizer events - that is exactly the shape my driver uses
> (SPI, IOHIDEvent digitizer collections through an IOHIDEventService), so the
> transport is not the difference I am chasing.
>
> So on your port: (a) does `backboardd` actually open your `SweetHIDTouch`
> service (i.e. does `SweetHID.cpp`'s `open()` logging show a client)? (b) If
> yes, what else does your kernel publish that mine does not — is there an
> `IOHIDEventSystem` object, an `IOHIDDevice` nub, an `IOHIDEventDriver`, or a
> personality/property on the service (e.g. `PrimaryUsagePage/Usage`,
> `Transport`, `ReportDescriptor`, `DisplayIntegrated`) that makes backboardd
> accept it? (c) Any boot-arg or ADT node you consider relevant?
>
> A one-line answer ("we never saw an open either, input comes from X") would
> save me days of disassembly. Thanks!
