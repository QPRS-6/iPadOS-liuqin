#!/usr/bin/env python3
"""Embed libliuqintouch.dylib into the launcher, as a C byte array.

The library used to be a separate file in the root image; when a flash leaves
parts of the image stale, the image's copy silently stays an older build and the
window server dies on the stale dependency.  Carrying it inside the launcher
means only that one binary has to arrive.
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
with open(dst, 'w') as f:
    f.write('/* generated from %s - do not edit */\n' % src)
    f.write('#define LIUQIN_DYLIB_SIZE %d\n' % len(data))
    f.write('#define LIUQIN_DYLIB_FNV %du\n' % (lambda h: h)(0))
    f.write('static const unsigned char liuqin_dylib[] = {\n')
    for i in range(0, len(data), 16):
        f.write('\t' + ','.join('0x%02x' % b for b in data[i:i+16]) + ',\n')
    f.write('};\n')
# FNV-1a of the first 4096 bytes, the same check the launcher logs
h = 2166136261
for b in data[:4096]:
    h = ((h ^ b) * 16777619) & 0xffffffff
print('embedded %d bytes, first-4096 FNV %08x' % (len(data), h))
