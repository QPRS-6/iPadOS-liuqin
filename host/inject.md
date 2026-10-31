# Injecting libliuqintouch into backboardd

The kernel posts touch samples to `/dev/liuqin-touch`; nothing consumes them
until a library of ours runs inside backboardd.  This is the second half of the
hackios-style route (see `docs/HID_LAST_MILE.md`) and needs one root-image
change and one flash.

## What goes into the image

1. `shim/out/libliuqintouch.dylib` -> `/usr/lib/libliuqintouch.dylib`
   (built by `tools/build-inject.sh`).
2. `/System/Library/LaunchDaemons/com.apple.backboardd.plist` gains

```xml
    <key>EnvironmentVariables</key>
    <dict>
        <key>DYLD_INSERT_LIBRARIES</key>
        <string>/usr/lib/libliuqintouch.dylib</string>
    </dict>
```

(`plutil -insert EnvironmentVariables.DYLD_INSERT_LIBRARIES -string
/usr/lib/libliuqintouch.dylib <plist>`)

## Build the image and the flash payload

```sh
cp -c ~/liuqin-root/ipad71-root-nodaemon.img ~/liuqin-root/ipad71-root-touch.img  # APFS clone: seconds
hdiutil attach -readwrite -noowners ~/liuqin-root/ipad71-root-touch.img
cp shim/out/libliuqintouch.dylib /Volumes/<vol>/usr/lib/
# NOT plutil -insert: that corrupts this binary plist.  plistlib instead:
python3 -c 'import plistlib,sys; p=sys.argv[1]; d=plistlib.load(open(p,"rb")); d.setdefault("EnvironmentVariables",{})["DYLD_INSERT_LIBRARIES"]="/usr/lib/libliuqintouch.dylib"; plistlib.dump(d,open(p,"wb"),fmt=plistlib.FMT_BINARY)' \
    /Volumes/<vol>/System/Library/LaunchDaemons/com.apple.backboardd.plist
hdiutil detach /Volumes/<vol>
python3 tools/sparsediff.py ~/liuqin-root/ipad71-root-nodaemon.img \
    ~/liuqin-root/ipad71-root-touch.img ~/liuqin-root/ipad71-root-touch.simg
fastboot -S 512M flash userdata ~/liuqin-root/ipad71-root-touch.simg
```

The sparse image only carries the changed blocks (a few MB), so this is a
short flash instead of the 16 GiB restore.

## What the boot log should say

```
liuqin-touch: /dev/liuqin-touch ready          (kernel)
LIUQIN INPUT: service opened by the event server   (our library in backboardd)
LIUQIN INPUT: reading /dev/liuqin-touch
```

Pressing a finger on the lock screen then moves the UI.  If instead the log has
nothing from our library, dyld refused to insert it (the AMFI/dyld check), and
the next move is to see whether `cs_enforcement_disable=1` is enough or the
library has to arrive as a SpringBoard/backboardd dependency.
