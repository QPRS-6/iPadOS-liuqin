# Touch: resume here (2026-10-25, firmware boots with the NT36532 map; reports are the remaining step)

## State in one screen

- **Chip**: Novatek NT36532, "Cascade" family. Trim id `20 00 00 32 65 03`, read at `0x1FB104`.
  The id is **not in Google's trim table** (`refs/novatek/nvt_nt36xxx_mem_map.h`: no entry has
  `0x32` at byte 3), so every per-chip branch below is the *cascade family* one - the map is
  `NT36523_memory_map` (`EVENT_BUF 0x2FE00`, `BOOT_RDY 0x3F10D`, `TX_AUTO_COPY_EN 0x3F7E8`,
  `SPI_DMA_TX_INFO 0x3F7F1`) plus the vendor module's own dmesg, not a trim-table entry.
- **Bus**: bit-banged SPI on TLMM pads - **CS 19, CLK 18, MOSI 17, MISO 16** (the device
  tree's own grouping). Mode 0, page split `addr >> 15 / addr >> 7`, a page write is its own
  chip-select transaction, and there is **one dummy byte between address and data**
  (`tch.dummy = 1`, transfer `len + 2`).
- **Firmware**: `novatek_nt36532_m81_fw_csot.bin` (245760 bytes) rides in the firmware area
  (`~/liuqin-root/fw` -> `tools/mkfwarea.py` -> `--fwarea`), offset `0x2B000`, fetched with
  `liuqin_fw_get()`.
- **Download**: `bsd/dev/liuqin_touch.c` (`tch_fw_download`, `tch_fw_write_sram`,
  `tch_fw_bld_bank`) runs the whole official sequence - bootloader reset, TX auto-copy mode,
  ILM/DLM descriptors, 0xFC00 chunks at 2 us pacing, DMA-done poll, BLD/FW CRC enable,
  boot_ready, reset-state poll - and now prints the CRC verdict as well.
- **Blocker (touch38) and its cause (found in touch39, from the reference source, not from the
  tablet)**: `SPI_DMA_TX_INFO` never cleared and the part never reported ready because every
  bulk SRAM chunk went out as `addr & 0x7f`, i.e. **without bit 7 - as a read**.  The vendor's
  `CTP_SPI_WRITE()` always applies `SPI_WRITE_MASK()` (`| 0x80`, `nvt_nt36xxx.h:444`,
  `nvt_nt36xxx.c:392`) and `nvt_write_sram()` pushes the whole payload through it
  (`nt36xxx_fw_update.c:687`).  So the download *did* execute to completion and nothing ever
  landed in ILM/DLM, which is exactly what a "never ready, copy engine never finishes" chip
  looks like.  **touch39 confirms the fix on the wire**: the SRAM read-back after section 0 is
  byte-exact (see "First boot of touch39" below).

## What touch39 changes

Patch `0187-liuqin-touch-fw-write-and-crc.patch`, four things:

1. **The write mask** (`tch_fw_write_sram`): `hdr[0] = (addr & 0x7f) | 0x80`.
2. **The missing last info section**: `nvt_bin_header_parser()` reads the *last* info record of
   a cascade image from `end - 0x10` once the header record (source offset below `end`) has
   been seen - not from `0x30 + 0x10 * n`.  At `0x30 + 0x10 * 13` our firmware has a decoy
   record (src `0x200`, len `0`), so the second half of the header (src `0x100` ->
   SRAM `0x114208`, len `0xff`) was never written.  The download is now 4 sections /
   6 chunks instead of 3 / 5.
3. **`nvt_boot_ready()` for a hardware-CRC part**: BOOT_RDY is written `1` and **left** `1`.
   Clearing it and writing POR_CD `0xA0` is the `hw_crc == 0` path (`nvt_nt36xxx.c:536`); this
   family reports `hw_crc = 2`, so our clear was cancelling the boot request we just made.
4. **Compact single-line logging plus the CRC verdict** (below).

## The three diagnostics, and their answers

1. **Compact logging**: one line per stage, every value on that line - one start line, one line
   per section, one descriptor-readback line, one CRC line, one verdict line.  The five
   `fw chunk k/n` lines printed inside one section are gone.
2. **CRC verdict**, straight from `nvt_read_bld_hw_crc()` (`nt36xxx_fw_update.c:884`):

   ```
   LIUQIN TOUCH: fw dl crc: txinfo 0x00@3 done 1 ilm 1 dlm 1 | ilm exp e3173a51 got e3173a51 | dlm exp 667268cf got 667268cf
   ```

   `txinfo` = `SPI_DMA_TX_INFO (0x3F7F1)`, `@n` = the poll it cleared at (200 = never);
   `done/ilm/dlm` = bits 2/0/1 of `BLD_ILM_DLM_CRC_ADDR (0x3F133)`; `got` =
   `R_ILM_CHECKSUM (0x3F120)` / `R_DLM_CHECKSUM (0x3F124)`, compared with the container's
   `0x18`/`0x1c` values.  The verdict line repeats the same numbers, so one surviving line is
   enough.  A straight SRAM read-back right after section 0 says whether the payload landed
   before the CRC engine has an opinion at all.
3. **`nvt_spi_dma_setup()`**: **not needed for this image.**  The official flow calls it only
   when `nvt_ts_check_tid(tid_nt36523n) && spi_dma_div_cnt_val`, and
   `spi_dma_div_cnt_val = fwdata[0x29] & 0x01` (`nt36xxx_fw_update.c:386`).  This container has
   `fw[0x29] == 0x00`, so the official driver writes no SPI-DMA divider either; the start line
   prints `spidma 0` so an image that flips the bit is visible.  The poll is byte-for-byte the
   vendor's `nvt_check_spi_dma_tx_info()`: 200 tries, 1 ms apart, done when the byte is `0x00`.

## What the next boot should show

Same procedure as below.  Wait for the download to finish (~1.5-2 minutes at 2 us pacing; the
bit-banged bus, not the wait states, sets that), do not power off, then pull the log.  Expected
sequence, in order (numbers from this container):

```
LIUQIN TOUCH: fw dl: size 245760 end 0x200 cascade 1 info 14 parts 16 spidma 0
LIUQIN TOUCH: fw dl bld: ilm des 0x000000 len 0x01d1bb | dlm des 0x100000 len 0x014107
LIUQIN TOUCH: fw dl part 0 @0x00000 len 0x1d1bc bin 0x200: 2 chunk N ms
LIUQIN TOUCH: fw dl readback @0x00000: 48 00 01 a2 eb 02 00 00 | file 48 00 01 a2
LIUQIN TOUCH: fw dl part 1 @0x100000 len 0x14108 bin 0x22000: 2 chunk N ms
LIUQIN TOUCH: fw dl part 14 @0x114108 len 0x100 bin 0x0: 1 chunk N ms
LIUQIN TOUCH: fw dl part 15 @0x114208 len 0x100 bin 0x100: 1 chunk N ms
LIUQIN TOUCH: fw dl crc: txinfo 0x00@N done 1 ilm 1 dlm 1 | ilm exp e3173a51 got X | dlm exp 667268cf got Y
LIUQIN TOUCH: fw dl BOOTED: 201924 B 6 chunks N s, txinfo 0x00@N, crc 0x07 ilm X dlm Y, state 0xa0@2fe60 0xff@2fd60 N polls
```

Reading it:

| what shows | what it means |
| --- | --- |
| `bld` line reads `ffffff`/`000000` for all four | either the register writes do not land (mask, one-way bus) **or these registers are write-only** - the payload readback and the CRC line decide, this one can only accuse |
| `readback` differs from the file | the SRAM write still does not land (and the CRC will agree) |
| `readback` matches, `txinfo` never clears | payload lands, copy engine never finishes: descriptor address/length, or the engine wants the SPI-DMA value |
| `crc done 0` | the bootloader never ran its CRC: BLD_CRC_EN / boot_ready ordering |
| `crc done 1, ilm 1, dlm 1, got == exp` | data and CRC are good; if it still does not boot the remaining suspects are the CRC-enable write sizes and the event-buffer base |
| `crc ilm 0` or `dlm 0` | that section's data is wrong (DLM first: it starts at `0x100000`, two chunks) |
| verdict `BOOTED` | the fwinfo lines below should turn real (`fw_ver 0x19`, `38x60`) and the report thread takes over |

If the CRC line itself is missing from the log, the verdict line still carries `txinfo`, the
CRC flags and both checksums - that is the line not to lose.

**How long to wait** (measured, not estimated): the bit-banged bus moves **0.33 ms per byte**
- touch39: 119228 bytes in 39609 ms; touch40: the whole 201924-byte download finished 53 s
after `touch fw dl start` (~80 s -> 133.4 s), i.e. ~25 kHz, 40 us per bit (two `delay(2)`
calls plus three MMIO accesses).  Section 0 alone is ~40 s.  Power off only after the last
`touch fw ...` mark; the pull itself is a recovery ADB read (`tools/liuqin-log-pull.sh`), so
the log has to be on disk before the power cycle.

## First boot of touch39 (partial log): the payload lands

The pulled log stops at 133.6 s of boot - 14 s after section 0 finished, 13 s before the
verdict - so it has no `part 1`, no `fw dl crc` and no verdict line.  What it does have:

```
LIUQIN TOUCH: fw dl: size 245760 end 0x200 cascade 1 info 14 parts 16 spidma 0
LIUQIN TOUCH: fw dl bld: ilm des 0xffffff len 0xffffff | dlm des 0xffffff len 0xffffff
LIUQIN TOUCH: fw dl part 0 @0x00000 len 0x1d1bc bin 0x200: 2 chunk 39609 ms
LIUQIN TOUCH: fw dl readback @0x00000: 48 00 01 a2 eb 02 00 00 | file 48 00 01 a2
```

- **The write-mask fix works.**  The SRAM read-back is byte-exact - the first firmware bytes
  ever observed inside this part.  Nothing else in the log was needed to prove it.
- The old path was also faster (the whole 245 KB in ~46 s); with real writes the part now
  holds the transfer back, which is what a chip consuming the data looks like.
- The descriptor read-back is `0xffffff` for all four registers.  Either they are write-only
  or the register writes do not land; the payload read-back says the register path works
  (same framing, same mask), so **write-only is the likely reading**.
- `irq 0x00000000` on the trim-id line: the bit-banged path reports no SE errors, as it has
  since touch25.

So the remaining question for the next boot - touch40, whose download code is byte-identical -
is only the one the CRC line answers: did the bootloader copy ILM and DLM with matching
checksums, and does the part leave the reset state.

## Second boot (touch40): the whole download runs, then a panic of ours

The screen (photo of the panic) showed the download end to end:

```
LIUQIN BOOT: touch fw sec 2 at 133.213 s
LIUQIN TOUCH: fw dl part 14 @0x114108 len 0x100 bin 0x0: 1 chunk 88 ms
LIUQIN BOOT: touch fw sec 3 at 133.302 s (+88 ms)
LIUQIN TOUCH: fw dl part 15 @0x114208 len 0x100 bin 0x100: 1 chunk 89 ms
LIUQIN BOOT: touch fw sec 4 at 133.391 s (+89 ms)
LIUQIN TOUCH: fw dl crc pre: txinfo 0xff@200 done 1 ilm 1 dlm 1 | ilm exp e3173a51 got ffffffff | dlm exp 667268cf got ffffffff
LIUQIN BOOT: touch fw crc done 1 ilm 1 dlm 1 at 134.147 s (+755 ms)
panic(cpu 1 caller 0xfffffe0008745d7c): __memcpy_chk object size check failed:
      dst 0xfffffffe0f617f3cc8, src 0xfffffffe00780cf96, (5 < 6) @ubsan.c:716
```

- **The panic is ours, not the chip's.**  `tch_fw_reg_write()` declared `uint8_t buf[6]` and
  copied up to six data bytes to `buf + 1`; the cascade reset-status clear is a seven-byte
  write, so `__memcpy_chk` compared a five-byte destination with a six-byte copy and the
  DEVELOPMENT kernel's UBSan panicked.  touch41 (`0189`) widens the buffer to eight bytes.
- **The download itself finished**: all four sections, including the two info sections touch39
  never reached (88 and 89 ms each).  This is the first boot that got past the transfer and
  into the CRC-enable / boot_ready sequence - and it died *before* boot_ready, so the part was
  never asked to start.
- `txinfo 0xff@200`: `SPI_DMA_TX_INFO (0x3F7F1)` **never** clears, not even with data that is
  provably in SRAM, and it reads all ones like the descriptor registers do.  The likely
  reading is that this part does not implement that register; the poll is harmless (200 ms)
  but is not evidence of anything on NT36532.
- `done 1 ilm 1 dlm 1` was read **before** boot_ready.  The vendor only reads these flags
  after a failed boot attempt (`nvt_download_firmware_hw_crc` -> fail -> `nvt_read_bld_hw_crc`)
  because the bootloader fills them - and the `R_` checksums - when it tries to start.  That
  also explains `got ffffffff`.  So these numbers settle nothing yet; touch41 reads the same
  registers again after the boot attempt and prints them as `crc post`.

## What touch41 fixes

`shim-boot-touch41.img` is touch40 plus two things (patch `0189-liuqin-touch-reg-write-buf.patch`):

1. `tch_fw_reg_write()`'s buffer is `uint8_t buf[8]` - the panic is gone, so the cascade
   seven-byte clear and then `nvt_boot_ready()` actually run.
2. The BLD CRC flags and `R_ILM`/`R_DLM` are read a second time, **after** the boot attempt,
   and printed in the verdict as `crc post`.  That is the vendor's own reading point
   (`nvt_download_firmware_hw_crc` -> fail -> `nvt_read_bld_hw_crc`), because the bootloader
   fills them when it tries to start.  The source does not say which polarity is "pass" for
   this register (the neighbouring `DMA_CRC_FLAG` uses bit 0 = 1 for *error*), so the boot
   itself is the verdict: `touch fw BOOTED` means they passed.

The screen then ends with `touch fw crc done <d> ilm <i> dlm <m>` and
`touch fw BOOTED` / `touch fw did not boot`.  Everything else - pins, protocol, download,
pacing - is unchanged from touch40.

## Third boot (touch41): the whole sequence runs, the part still does not boot

touch41 got past the panic, so for the first time the driver ran
`boot_ready` and the reset-state poll:

```
LIUQIN TOUCH: fw dl part 1 @0x100000 len 0x14108 bin 0x22000: 2 chunk 27473 ms
LIUQIN TOUCH: fw dl crc pre: txinfo 0xff@200 done 1 ilm 1 dlm 1 | ilm exp e3173a51 got ffffffff | dlm exp 667268cf got ffffffff
LIUQIN BOOT: touch fw did not boot at 214.619 s
LIUQIN TOUCH: fw dl did not boot: 201924 B 6 chunks 68 s, txinfo 0xff@200, crc post 0xff ilm ffffffff dlm ffffffff, state 0xff@2fe60 0xff@2fd60 20 polls
```

No panic, every section written (ILM 39.4 s, DLM 27.5 s, the two info
sections 91/88 ms), and then nothing: `SPI_DMA_TX_INFO` stuck at `0xff`, the
CRC flags unreadable after the attempt, the reset status `0xff` at both
candidate bases.

## The reason: every address was from the wrong chip family

The `0x2FE00`/`0x3F10D`/`0x3F7F1`/`0x3F128` set is the **NT36523** map.  This
part is NT36532 and has its own map, which is in the vendor module
(`nt36532_touch.ko`) that TWRP ships - it is unstripped, so the map struct and
the offsets the code reads can both be extracted.  See
`refs/novatek/nt36532_vendor_map.md` for the full table and the derivation;
the short version:

| what | old (NT36523) | NT36532 |
| --- | --- | --- |
| event buffer | `0x2FE00` | **`0x125800`** (reset state `0x125860`, host cmd `0x1258D0`) |
| boot ready | `0x3F10D` | **`0x1FB50D`** |
| tx auto copy | `0x3F7E8`, value `0x69` | **`0x1FC925`, value `0x56`** (`hw_crc = 2`) |
| spi dma tx info | `0x3F7F1` | **`0x1FC914`** |
| ILM des/len/crc | `0x3F128`/`0x3F118`/`0x3F100` | **`0x1FB528`/`0x1FB518`/`0x1FB500`** |
| DLM des/len/crc | `0x3F12C`/`0x3F130`/`0x3F104` | **`0x1FB52C`/`0x1FB530`/`0x1FB504`** |
| R_ILM / R_DLM / flags | `0x3F120`/`0x3F124`/`0x3F133` | **`0x1FB520`/`0x1FB524`/`0x1FB533`** |
| bootloader reset | `0x00000` | **`0x1FB43E`** (`swrst_sif`) |

That explains every "reads `0xffffff` / never clears / nothing happens" in
touch39-41: the driver was writing the NT36523 register block, which does not
exist on this part.  It also explains why the payload read-back *did* work -
that path writes SRAM, not registers.

## What touch42 changes

`shim-boot-touch42.img`, patch `0190-liuqin-touch-nt36532-map.patch`:

1. every download address above is the NT36532 one;
2. the descriptors are written **before** `nvt_tx_auto_copy_mode()`, and the
   auto-copy value is `0x56`;
3. `nvt_bld_crc_enable()` is dropped - the vendor's own NT36532 flow has no
   such write;
4. `nvt_fw_crc_enable()` uses the vendor's sizes (7 bytes at `event+0x60`,
   3 bytes `{host_cmd, 0xAE, 0x00}`) with `EVENT_MAP_HOST_CMD = 0xD0`;
5. the reset-state poll and the report thread both use `0x125800`/`0x125860`,
   and FWINFO is read at `0x125878`;
6. the boot mark still reports the verdict on screen.

## Fourth boot (touch42): the firmware boots

```
LIUQIN TOUCH: fw dl crc pre: txinfo 0x03@200 done 0 ilm 0 dlm 0 | ilm exp e3173a51 got 00000000 | dlm exp 667268cf got 00000000
LIUQIN BOOT: touch fw crc done 0 ilm 0 dlm 0 at 125.506 s (+753 ms)
LIUQIN BOOT: touch fw BOOTED at 125.550 s (+43 ms)
LIUQIN TOUCH: fw dl BOOTED: 201924 B 6 chunks 68 s, txinfo 0x03@200, crc post 0x04 ilm e3173a51 dlm cd21b90f, state 0xa0@125860 1 polls
LIUQIN TOUCH: fwinfo 19 e6 26 3c 46 50 78 00 00 00 0a 00 01 f1 01 00 26 (broken, using the panel size)
LIUQIN TOUCH: report space 1800x2880, 10 fingers, 0 buttons, chip 0x00
```

**The part boots.**  `state 0xa0@125860` on the first poll of the new event
buffer - the whole "download, CRC, boot_ready" chapter closes with the NT36532
map (touch42/`0190`) after touch39-41 failed on the NT36523 one.

- `txinfo 0x03@200`: `0x1FC914` is a real register (it reads `0x03`, not the
  `0xff` the NT36523 address returned) and it does not go to `0x00` in 200 ms.
  Whatever the bits mean, the copy happened: the part booted.
- `crc post 0x04`: `done = 1`, `ilm = 0`, `dlm = 0`.  `R_ILM` reads
  `0xe3173a51`, exactly the container's ILM checksum.  `R_DLM` reads
  `0xcd21b90f`, which is the container's *header* record checksum (the last
  section written), not the DLM one - the two `R_` registers look like "last
  section copied" rather than a per-section pair with our write order.  It does
  not block the boot because no BLD CRC enable is set for this part.
- `fwinfo 19 e6 26 3c ...`: `0x19 + 0xe6 = 0xff` (version and its complement)
  and `0x26 0x3c` = **38 x 60** - the acceptance values from
  `docs/TOUCH_FW_RE.md`.  The "broken" verdict is our own decode: this map puts
  `max_x`/`max_y` somewhere else in the block, so the panel-size fallback is
  used.  Cosmetic, worth fixing for the finger decode.

So the firmware path is done.  A second boot of the same image reproduced it
(`touch fw BOOTED at 131.365 s`, 67 s of transfer): the boot prints
`report thread up, INT on gpio21` and `gpio21 in/out 0x00000000 (in 0 out 0)`,
and nothing follows - the event-buffer diagnostics only go to the log, so the
screen cannot say whether anything arrived.

## What touch43 changes: the report path answers on screen

`shim-boot-touch43.img`, patch `0191-liuqin-touch-report-probe.patch`:

1. **fwinfo decode fixed**: validity is `fw[0] + fw[1] == 0xff` (this map's
   `fw[4..7]` are not the geometry), and the line now prints `ver 0x19 ok,
   38x60 sensors`; the report space stays the panel's 1800x2880.
2. **Report-path marks**, rate-limited, drawn over the UI:
   - `touch int <level>` on the first 6 `gpio21` level changes,
   - `touch evt <b0 b1 b2 b3>` on the first 4 event-buffer changes that do not
     decode,
   - `touch frame <n>` on the first 6 frames that do decode.
3. The stale candidate-address hunt (`0x11a00`, `0x2fe00`, `0x3f004`, ...) is
   gone from the report loop - it was NT36523 leftovers and cost five extra
   reads every 100 ms.

So after `touch fw BOOTED`, slide a finger on the lock screen and read the
screen: `touch frame` means the report path works (then it is coordinates and
HID delivery), `touch evt` without a frame means the buffer moves but our
decode rejects it (the mark carries the bytes), `touch int` alone means the
interrupt works and the buffer address is still off, and no mark at all means
the controller is not scanning and the vendor's post-boot mode/handshake
sequence is the next thing to transcribe.

## Fifth boot (touch43): reports arrive, and the decode was wrong

The screen showed the whole report path waking up:

```
LIUQIN TOUCH: block 0a 28 05 5a c2 65 ff ff ff ff ff ff
LIUQIN BOOT: touch evt 0a28055a at 136.768 s
LIUQIN TOUCH: first frame 0a 22 4c 78 12 88 09 ff ff ...
LIUQIN HID: touch 3 down at 1217,1799
LIUQIN BOOT: touch frame 1 at 138.567 s (+1281 ms)
LIUQIN TOUCH: gpio21 did not assert for a touch, polling the event buffer
```

- **The controller scans and reports**: frames land at `0x125800` and reach
  `liuqin_hid_touch()`.  This is the first time any touch event has been
  dispatched on this port.
- `gpio21` never asserts, so the thread falls back to polling - that is the
  existing safety net working (and it means the INT pin/polarity still needs a
  look, but it no longer blocks anything).
- The finger ids and coordinates were garbage.  Two reasons: the test was
  several fingers sliding randomly (so several records per frame is expected),
  and - the real bug - this part does **not** use the old 12-bit x/y packing.

## What touch44 changes: the vendor's record layout

The vendor module's `nvt_ts_work_func` decodes a report as: records start at
the event buffer itself with a stride of 6, byte 0 is `(id << 3) | event`,
**x is a big-endian 16-bit value at +1 and y at +3**, and a record is only
accepted for event 1 or 2 (3 is a release, tracked through the slot).  The
older nt36xxx parts pack 12-bit x/y into three bytes - that is what this driver
did, so every coordinate was noise.

`shim-boot-touch44.img`, patch `0192-liuqin-touch-frame-layout.patch`:

1. record accessors `tch_rec_id/event/x/y` implementing the layout above;
2. `fwinfo`'s `0x4650`/`0x7800` are the **sensor space** (18000 x 30720, ten
   times the panel), not nonsense - they are now accepted and used for scaling
   instead of being thrown away by a `<= 4096` test;
3. the first six accepted records print `finger <id> ev <e> raw <x>,<y> of
   <max_x>x<max_y>`, which is the calibration line: one finger, slow slide
   corner to corner, and the raw values should walk monotonically.

## Sixth boot (touch44): single-finger tracking is correct

The log from a slide test:

```
LIUQIN TOUCH: fwinfo 19 e6 26 3c 46 50 70 80 00 0a 00 01 f1 01 00 26 (ver 0x19 ok, 38x60 sensors, sensor space geometry)
LIUQIN TOUCH: report space 18000x28800, 10 fingers, chip 0x00 (sensor counts match the panel)
LIUQIN TOUCH: finger 1 ev 2 raw 5120,12360 of 18000x28800
LIUQIN HID: touch 0 down at 512,1236
LIUQIN TOUCH: finger 1 ev 2 raw 3298,9016 ...
LIUQIN TOUCH: finger 1 ev 2 raw 827,5013 ...
LIUQIN TOUCH: finger 1 ev 2 raw 36,3325 ...
LIUQIN TOUCH: gpio21 asserted (level 1)      <- the INT line works too
```

- **id stays 1, event stays 2, and the raw values walk smoothly** - the record
  layout is right (`raw / 10` = panel pixels, 18000x28800 = exactly 10 x
  1800x2880).
- `gpio21 asserted (level 1)` shows the interrupt line does fire; the thread
  had already switched to polling before the first edge, which is why it polls.
- **No `HID: touch ... up` in the whole log.**  A lift is reported as an empty
  frame (`ff ff ff...`, `fd fd fd...` at the end of a gesture) and the driver
  only released slots from *valid* frames, so the last finger stayed pressed.
  backboardd would ignore every input after that.

## touch45: release on an empty frame

`shim-boot-touch45.img`, patch `0193-liuqin-touch-release-on-empty.patch`: when
a frame carries no valid record at all, every held slot gets an up.  A spurious
release costs one frame; a stuck finger costs the session.

## Seventh boot (touch45): the drag becomes a stream of taps, and the HID last mile

```
HID: touch 0 down at 1003,2879 edge
HID: touch 0 up at 0,0 edge
HID: touch 0 down at 970,2455 edge
HID: touch 0 up at 0,0 edge
... 55 ups in one gesture
```

Two separate things:

1. **The lift fix was too eager.**  A read that lands between two controller
   reports returns an empty frame, exactly like a lift; releasing on the first
   empty frame turned the drag into 55 taps.  touch46 keeps an empty-frame
   counter and only releases after a run of them (~120 ms), and it drops the
   second (109-byte) block read from the loop - two reads of the same
   single-entry buffer per tick were racing the controller.
2. **The HID events reach nothing.**  `LIUQIN HID: <service> open by <client>`
   is logged by `IOHIDEventService::open()`, and the log has **zero** such
   lines: no process ever opened the touch service.  `dispatchEvent()` copies
   the event to every client in `_clients`, and that list is empty.  The whole
   pipeline - chip -> frame -> `liuqin_hid_touch()` - works; the events are
   dropped in the kernel because backboardd never became a client.

So "touch does nothing" is not a touch-driver problem any more.  What is left
is the HID plumbing between our `IOHIDEventService` and backboardd, which is
also why the buttons path was never verified.

## touch46: drag hysteresis (and a way to test the HID plumbing)

`shim-boot-touch46.img`, patch `0194-liuqin-touch-drag-hysteresis.patch`:
release only after ~6 empty frames, one buffer read per tick.

`shim-boot-touch46-hidtest.img` is the same kernel with `liuqinhidtest=6` in the
boot line: the buttons service presses Home every 6 s and logs
`LIUQIN HID: key 0x.. down (N clients)`.  That `N` is the answer to "does
anything consume our HID services": `0` means the service is published but
unconnected (backboardd never opened it), `>0` means the plumbing works and
only the touch event format is in question.

## The HID last mile: published, but never opened

`liuqinhidtest=6` printed `key ... (0 clients)` - and `IOHIDEventService::open()`
never logged a single `open by`.  So backboardd matches our services and never
opens them:

- the kernel only has our stub `IOHIDEventService`; there is no
  `IOHIDEventSystem`, `IOHIDEventDriver` or `IOHIDDevice` in the image;
- backboardd does arm the right notification -
  `IOK backboardd: IOServiceFirstMatch <IOProviderClass IOService>
  <IOPropertyMatch HIDServiceSupport true> -> notify` - at ~50 s of boot;
- both HID services publish during kernel init, well before that;
- `handleOpen`/`newUserClient` log every open they see, and backboardd's IOKit
  trace shows opens for AppleCLCD, IOPMRootRegion etc. but none for HID.

The implementation is byte-for-byte the ios-sweet one (which delivers touch and
buttons on the sweet port: their `0058` notes say
"SweetHIDTouch -> backboardd"), so the difference is *when* the service
appears, not what it contains.

## touch47: publish the HID services late

`shim-boot-touch47.img`, patch `0195-liuqin-hid-publish-late.patch`: the
services still start at kernel init and still receive events, but
`registerService()` is deferred to `liuqinhidpublish` seconds after boot
(default 120, `0` = old behaviour).  The boot line also keeps
`liuqinhidtest=45`, so the buttons press at ~45/90/135 s and log their client
count.

What the log will say:

- `LIUQIN HID: touch publishes in 120 s` at init, then
  `LIUQIN HID: touch published late` at ~120 s;
- `LIUQIN HID: <service> open by <client> -> 1` if backboardd picks it up -
  that is the line the whole last mile hangs on;
- `key ... (N clients)` with `N > 0` confirms it from the other side.

**Result: late publish is not the answer.**  The services published at 220 s
(log `touch published late`), well after backboardd armed its notification at
~139 s, and the buttons still logged `key 0x40 down (0 clients)` afterwards - no
`open by`, no HID user-client log at all.  So backboardd matches the property
and still does not open the service.

One difference to the working sweet port is now known: this root is **iOS
16.6.1 (20G81)** (`SystemVersion.plist` in `ipad71-root-nodaemon.img`) while
ios-sweet runs 16.7 (20H392) and compiled its HID user client / queue from
IOHIDFamily-1915.140.3 (16.7).  A user client ABI mismatch would explain a
silent refusal, but it would not explain the missing `open`.

## touch48: does the match even reach us?

`shim-boot-touch48.img`, patch `0196-liuqin-hid-match-probe.patch`, overrides
`IOHIDEventService::matchPropertyTable()` and logs every evaluation:

```
LIUQIN HID: LiuqinHIDTouch matchPropertyTable -> 1 (score 10000)
```

- a line for our service means backboardd's match *reaches* it and then
  declines to open - the property table (class, usage, transport, version) is
  then the suspect;
- no line at all means the service is not in the plane backboardd searches, and
  publishing/registration is the suspect.

**Result (touch48 boot): zero `matchPropertyTable` lines.**  The boot had
`liuqinhidpublish=120`, and the services only published at 220 s - *after*
backboardd armed its `HIDServiceSupport` match (~139 s).  So that boot tested
the "publish after the notification" order and the notification did not produce
a match.

## touch48b: publish immediately, with the probe in

`shim-boot-touch48b.img` is the same kernel with `liuqinhidpublish=0`, i.e. the
services are back in the registry from kernel init (long before backboardd's
match).  That splits the two remaining cases cleanly:

- `matchPropertyTable -> 1` lines at ~139 s: matching works, backboardd sees the
  service and declines to open it -> the property table (class name, usage,
  transport, user-client version) is the suspect;
- still zero lines: our service never reaches the matcher at all -> the
  registration/publish path (or the IOService plane) is the suspect, and the
  fix is kernel-side, e.g. publishing under a provider backboardd walks.

**Result (touch48b boot): still zero `matchPropertyTable` lines**, with both
services published at kernel init (`buttons published now`, `touch published
now`).  So even a service that has been in the registry for ~100 s before
backboardd's match never reaches the matcher.

## touch49: the match runs from inside the kernel

`shim-boot-touch49.img`, patch `0197-liuqin-hid-registry-selftest.patch`: right
after publishing, the driver runs backboardd's own match from kernel space -
`IOService::getMatchingServices()` with
`IOProviderClass = IOService` and `IOPropertyMatch HIDServiceSupport = true` -
and logs how many services it finds and how many are ours:

```
LIUQIN HID: touch published now (prop 1, id 0x...)
LIUQIN HID: selftest: HIDServiceSupport match found N services, M ours
```

**Result (touch49 boot): `selftest: HIDServiceSupport match found 2 services, 2
ours`.**  The exact dictionary backboardd uses finds both of our services from
inside the kernel, so the registry, the properties and the match itself are
all correct.  (`matchPropertyTable` logged nothing even then - IOKit's matching
does not call that virtual, it matches passively; that probe can be ignored.)

So the failure is one layer further out: the kernel finds the services, but
backboardd never opens one.  IOKit's `addMatchingNotification()` does replay
existing matches for a fresh notification (`installNotification()` builds the
`existing` iterator and calls `invokeNotifier()` for each), and the user-space
delivery then goes through `IOServiceUserNotification::handler()` -> a mach
"ping".  That ping is the next thing to observe.

## touch50: does the notification ping ever reach user space?

`shim-boot-touch50.img`, patch `0198-liuqin-iokit-notification-ping.patch`, logs
every first-match ping IOKit sends to a user-space notifier:

```
LIUQIN IOK: notification ping type 0x... for LiuqinHIDTouch id 0x...
```

**Result (touch50 boot): zero pings** - and the IOKit source says why:
`IOServiceUserNotification::handler()` only pings when `armed` is set, and
`armed` becomes true in `IOUserNotification::next()` only once the client has
drained the queue.  So a **service that already exists when the notifier is
registered is queued silently and never pinged**; only a service that
first-matches *after* the client drained the initial batch wakes it.  Our HID
services start during kernel init, i.e. always in the silent initial batch -
which also means "publish later" (touch47/48) could not help: re-registering is
not a new first-match.

## touch51: create the HID services *after* backboardd is listening

`shim-boot-touch51.img`, patch `0199-liuqin-hid-relaunch.patch`: a timer
(`liuqinhidrelaunch`, default 150 s) allocates and starts **fresh instances** of
`LiuqinHIDButtons` and `LiuqinHIDTouch` under IOResources.  New instances means
new first-match events, after backboardd has drained its queue, which is
exactly the case the ping exists for.

What to look for:

```
LIUQIN HID: relaunch in 150 s
LIUQIN HID: buttons relaunched
LIUQIN HID: touch relaunched
LIUQIN IOK: notification ping type 0x... for LiuqinHIDTouch id 0x...
LIUQIN HID: LiuqinHIDTouch open by <client> -> 1
```

The last two lines are the last mile: the ping proves backboardd was woken,
the open proves it accepted the service.  The relaunched touch instance is the
one `liuqin_hid_touch()` dispatches to, so a finger on the lock screen after
150 s is the real test.

**Result (touch51 boot): the pings arrive, the open does not.**

```
LIUQIN HID: buttons relaunched
LIUQIN IOK: notification ping type 0x65 for LiuqinHIDButtons id 0x1000001bd
LIUQIN HID: touch relaunched
LIUQIN IOK: notification ping type 0x65 for LiuqinHIDTouch id 0x1000001be
```

`0x65` = 101 = `kIOServiceMatchedNotificationType`, so IOKit did wake backboardd
for both new services; backboardd still never opens one, and the key presses
after the relaunch are still `(0 clients)`.

## touch52: does user space drain the notification?

After the ping, backboardd's IOKit library must call `IOIteratorNext` on the
notification, which lands in `IOServiceUserNotification::copyNextObject()` - the
function that both hands out the matched service and sets `armed` again.
`shim-boot-touch52.img` (patch `0200-liuqin-iokit-drain-probe.patch`) logs every
drain:

```
LIUQIN IOK: notification drain -> LiuqinHIDTouchLiuqinHIDTouch (armed 0)
```

**Result (touch52 boot): `LIUQIN IOK: notification drain -> LiuqinHIDTouch...`
appears.**  So the full notification chain works: kernel matches -> ping ->
user space drains and receives the service object.  What does not happen is the
`open`.  Since `newUserClient()` only logged *failures*, a successful open with
a different connect type would have been invisible - which is exactly what the
next probe checks.

## touch53: log every open attempt and its type

`shim-boot-touch53.img`, patch `0201-liuqin-hid-open-type-probe.patch`:

- `LIUQIN HID: <service> newUserClient type 0x...` on every open attempt,
- `LIUQIN HID: <service> user client created type 0x...` on success,
- type 0 and 1 are now accepted as well, because the only hard requirement is
  that the kernel's `kIOHIDEventServiceUserClientType` ('esuc') matches what
  *this root's* libIOHID sends - and this root is 16.6.1 while the kernel HID
  code was copied from 16.7.

- `newUserClient type 0x...` lines: backboardd does open, and the type tells us
  whether the ABI matches ('esuc') or not;
- none: backboardd drains the service and then decides not to open it, which
  moves the question into its callback logic (what it expects from an
  IOHIDEventService before opening it).

**Result (touch53 boot): no `newUserClient` line at all** - backboardd drains
the service object (and drains AppleCLCD the same way, so that is its generic
IOKit monitor) and never opens it.

## Why: this iOS version wants the kernel `IOHIDEventSystem`

Strings in the root's dyld cache (`ipad71-root-nodaemon.img`, iOS **16.6.1**,
20G81):

```
"HIDServiceSupport"                       4
"IOHIDEventSystemClientCreateWithType"    3
"IOHIDEventSystem"                       74
"IOHIDEventServiceOpenService"            0
```

There is **no direct "open one event service" API** in this user space: its
`libIOHID` talks to the kernel `IOHIDEventSystem` object, which opens the event
services and forwards their events through the shared queue.  This port has
only a stub `IOHIDEventService` (patch 0058) and no `IOHIDEventSystem`
(checked: the kernel image has no such class), so no HID input of any kind -
touch or buttons - can reach backboardd, however correct the driver is.

ios-sweet delivers input with the same HID patch on **iOS 16.7** (20H392), so
their user space attaches through a path this 16.6.1 root does not have.

The touch driver itself is finished: firmware boots, frames decode (`finger 1 ev
2 raw ...`), coordinates scale correctly, `liuqin_hid_touch()` builds digitizer
collections, and the service is matched, pinged and drained by backboardd.

## The 16.7 userland is not the answer either

Pre-check on the iPad6,7 **16.7.16 (20H392)** IPSW - the very build ios-sweet
runs - gives exactly the same HID surface as 16.6.1:

| string | 16.6.1 (20G81) | 16.7.16 (20H392) |
| --- | --- | --- |
| `IOHIDEventServiceOpenService` | 0 | 0 |
| `IOHIDEventSystemClientCreateWithType` | 3 | 3 |
| `IOHIDEventSystem` | 74 | 74 |
| `HIDServiceSupport` | 4 | 4 |

So replacing the root cannot change the HID attach path - the userland library
is the same in both.  (The DeviceTree payload of both IPSWs is an iBoot
`bvx2`/LZFSE container, so the `ipsw` CLI is needed to read it at all.)

## touch54: add the missing `/product` node

`IOK backboardd: path IODeviceTree:/product -> none` in every boot - our ADT has
no `/product` node, while ios-sweet's does.  `tools/mkadt.py` has always been
able to copy it (`product_props()`), it just needs the device tree at
`research/ipad-d321ap/d321ap.adt`:

```sh
# once, from the iPad8,1 (D321AP, j317ap) IPSW:
ipsw img4 im4p extract Firmware/all_flash/DeviceTree.j317ap.im4p -o d321ap.adt
mkdir -p research/ipad-d321ap && mv d321ap.adt research/ipad-d321ap/
python3 tools/mkadt.py shim/out/liuqin.adt --dtb dtb/boot_unpacked/dtb
```

`ipsw` is blacktop's tool; the prebuilt macOS x86_64 tarball from its releases
works without Homebrew/Go (`ipsw_<ver>_macOS_x86_64.tar.gz`).

The resulting ADT carries the real D321AP `/product` (61 properties: `iPad Pro
(11-inch)`, `artwork-device-idiom pad`, `graphics-featureset-class APPLE5`,
`artwork-scale-factor 2`, `thin-bezel 1`, ...).

**First attempt (touch54) panicked in the shim's own loader**: `STAGE ldr-copy`,
`ESR 0x96000010`, `FAR 0x101004000`.  Cause: regenerating the ADT without
`--dram-base/--dram-size` reset the DRAM bank to the build default
(`0x100000000+0x17ff00000`), and `mkpayload` takes the kernel's load address
from the ADT - so the shim copied segments to 4 GiB, which is a hole on this
board.  The last good payloads all carry `phys 0x840000000, mem 0x13ff00000`.
Rebuilt with the right bank as `shim-boot-touch54b.img`; the payload header now
matches touch53 again.

**Result (touch54b boot): `/product` is there, the open is still missing.**

```
IOK backboardd: path IODeviceTree:/product -> IOPlatformDevice   (was "none")
```

`/product` is now read correctly by backboardd, and it changes nothing about
HID: still zero `newUserClient` lines, still `(0 clients)` on the keys.  So the
device tree was not the gate either; the missing piece is the kernel HID event
system itself.

Keep the `/product` node anyway - it makes the presented device tree faithful
(and is what `mkadt.py` was written for).

Two ways forward, both bigger than a patch:

1. **Port `IOHIDEventSystem`** (Apple open-source IOHIDFamily) into the kernel and
   hook the stub services into it - days of work, and the stub `IOHIDEventService`
   would have to grow into the real one;
2. **Rebuild the root on iOS 16.7** so it matches the ios-sweet reference where
   this HID design is known to deliver touch and buttons.









## What the screen marks mean (touch40 onwards)

`shim-boot-touch40.img` is touch39 plus boot marks (patch
`0188-liuqin-touch-fw-progress-marks.patch`), and touch41 keeps them.  The console is muted
(`liuqinmute=1`), so touch39's screen said nothing for the whole download and that boot was
powered off 13 s before the verdict.  Now, on the screen, over the UI (real touch40 numbers):

```
LIUQIN BOOT: touch fw dl start at ~80 s
LIUQIN BOOT: touch fw sec 1          <- ILM written (the long one, ~40 s)
LIUQIN BOOT: touch fw sec 2 at 133.213 s (+1416 ms)
LIUQIN TOUCH: fw dl part 14 @0x114108 len 0x100 bin 0x0: 1 chunk 88 ms
LIUQIN BOOT: touch fw sec 3 at 133.302 s (+88 ms)
LIUQIN TOUCH: fw dl part 15 @0x114208 len 0x100 bin 0x100: 1 chunk 89 ms
LIUQIN BOOT: touch fw sec 4 at 133.391 s (+89 ms)
LIUQIN BOOT: touch fw crc done 1 ilm 1 dlm 1 at 134.147 s (+755 ms)
LIUQIN BOOT: touch fw BOOTED          <- or "touch fw did not boot", or "touch fw FAIL"
```

`+N ms` is the time since the previous boot mark of **any** component, so a delta can include
an `exec` mark - the section marks and their order are what matters.  The last mark is the
answer; the log then carries the numbers.

## Reference material already in the repo

- `refs/novatek/nt36xxx.c`, `nt36xxx.h`, `nt36xxx_mem_map.h`, `nt36xxx_fw_update.c` -
  the official Google novatek_touch sources (android-15 branch), authoritative for every
  register and sequence.
- `docs/TOUCH_FW_RE.md` - the container format, the write recipe and the acceptance values
  (`fw_ver = 0x19`, `fw_type = 0x01`, `PID = 0x5973`, `x_num = 38`, `y_num = 60`).
- `NOTES.md` section 8 - the whole history, including the pitfalls (log ring shows the previous
  boot, `tch_xfer` caps at 256 bytes, the device tree's cs group is gpio19, etc.).

## Build, boot and log

```
cd iPadOS-liuqin && WORK=/tmp/liuqin-build3 ./tools/build-local.sh
python3 tools/mkpayload.py /tmp/liuqin-build3/fakeroot/System/Library/Kernels/kernel.development.liuqin \
    shim/out/liuqin.adt /tmp/payload-touchNN.bin --cmdline "$BOOT" --fwarea ~/liuqin-root/fwarea.bin
LLD=/tmp/ld.lld shim/build.sh /tmp/payload-touchNN.bin && cp shim/out/shim-boot.img shim/out/shim-boot-touchNN.img
```

`shim-boot-touch54b.img` (the one to boot: D321AP `/product` + correct DRAM) -
sha256 `cc04b22d0ab38b4dde942233b5c32566bbadd4a7ae97648c1efd430ce55d61df`,
payload `d3a6043d91caf2f322f788f07922e132b573917a7428266ee7545f8c39045364`.

`shim-boot-touch54.img` (DO NOT BOOT: wrong DRAM bank, panics in ldr-copy) -
sha256 `80965ba8125ca1a558836a65332443bef821b1ff76bb53631176b1e546343d48`.

`shim-boot-touch53.img` (booted: no open attempt at all) - sha256
`d1412f0ce7b8a737746a392d50232cf8ec7af27fd0ec51298ec5511b55182ff1`, kernel
`34bc4ba16118f40de46fddf19209eb9c2feec5e59df614dc9d13190f43f51fc3`, payload
`91eadec26a60ca60a890ea61d0252c4cbabd9b7e7c74744c8a357bd16085e752`.

`shim-boot-touch52.img` (booted: user space drains the notification) - sha256
`ea5e0a98454315ac34a0c80e713928bf9f701b029734d9d1d2f3a2f4ba74b264`, kernel
`3aa40312e116a5477ee56568a03a1e2fb7c7b4c8d4a2c88a4c6d716fe18e9ce0`, payload
`972a241e3f5aa3c3cde4d1cb042cc600815aa2d6b86706103c90cdee0b7b62a0`.

`shim-boot-touch51.img` (booted: pings arrive, no open) - sha256
`330397a7ffc9a75f8855d4744fd84a0cad40295d6d672d590881cd0f1e414921`, kernel
`9aa5be0f4bf57075f8c8f29fe147b8fe81b484de4bec96703bb629209452ec2f`, payload
`d0734690ab4c95e41d1c1b818b30da0a933b55f5519ad3fbc3705ed47c567c0c`.

`shim-boot-touch50.img` (booted: no ping, as the arming order predicts) - sha256
`ba24c872594fe1b36ea393293a05df866f4986ae8e43941767ea513715c2f803`, kernel
`62aa2f255bb1e6693b9731c8eb75e66c7c4f4830b7380452ad88305dd233d558`, payload
`7657d15fd09aeb6ce706c6950388934c4029bb7487e1ceabe4ca1854d3dade84`.

`shim-boot-touch49.img` (booted: kernel-side match finds 2 of 2) - sha256
`2fdb5971033377177f3f7e6c9c895700ff40986730284c037be7238f2599ab92`, kernel
`6922633926f384fcc98c11ffaf2c4c38ebf818b0a8a16d62a2fc1d00ddf871f9`, payload
`a388254f3985e8241f3197978c04d391e3ee68f1727a34a2af7f4cf4c0838ec3`.

`shim-boot-touch48b.img` (booted: immediate publish, still no match) - sha256
`861b5102287627f83cf2ed0fc04e675ddcf093b1b1f97f02b2dcaf94167aba4c`, payload
`8cd268156725da8c82386b2acbe121672100920b55ddfc6ee84faf853e277dc4`, kernel
`9c3b7eb1423ef649bacd4bae0b821c6d56bc77191ed0716eaa7128dca1c1fd8a`.

`shim-boot-touch48.img` (booted: no matchPropertyTable, published at 220 s) - sha256
`018d3a1cadfd13b14feee0fec763fd4f61db074d8d7acc2010c37caca00a5b3c`, kernel
`9c3b7eb1423ef649bacd4bae0b821c6d56bc77191ed0716eaa7128dca1c1fd8a`, payload
`47e656598e6ea7d7ba01f49f5e4d2c0e9950c05633545802200255de1fa6cf71`.

`shim-boot-touch47.img` (booted: late publish, still 0 clients) - sha256
`a560d5eb347cd64bd43b2f818af11037df1430216f8b8d6d9eb51a81fc3fa5e9`, kernel
`bf5fc67a953a96beb3687411cb8cb32efced3b02aedea5d9d253e0be93f093ad`, payload
`693fb1ef0c8781420a90e07ab98bff25accc083a8f0d629930a64e7bda7a005a`.

`shim-boot-touch46-hidtest.img` (booted: `0 clients`) - sha256
`8b5d786b59951dced02fb760028ecf6edb3b748f474cba51fbd2e619aef267b4`, payload
`da8b1525d0951120ca8abc976acbd3dadbf3e75b64e8e5fec9f68f3e6f9f222e`.

`shim-boot-touch46.img` (same kernel, normal boot line) - sha256
`f602297762ca8a6d21ad2fb995ef170e663099fdc55d21c11390b65aca3f846c`, kernel
`db8cfc85c65c4edbd0929c513aee09643e34f2548bbab377b8860f01cb1c6a0f`, payload
`2bf47180f27adfeac07deddd42cea2ceb0147adac504de6b1b21a5b436831a25`.

`shim-boot-touch45.img` (booted: taps instead of a drag) - sha256
`0c607b5b0a8a89b25fe7157e447fe3e0d2d38271de07d4c02b0eee3d7a2dd652`, kernel
`296b9634a248f0477baa1b2cedaef32f3941a115fadf0d076e9c81898fa8d140`, payload
`14e9150f07f33619bc54fb71a76ced401bf2a85924b72938df38029ff8da30b9`.

`shim-boot-touch44.img` (booted: single-finger tracking correct) - sha256
`cdaca6316a28a0e59a5c9f9cc7184f190045384d582fde358a2b14eab55f7d2f`, kernel
`95f97a58cf6a5ff092bd08270f494d8ffd293ba61986f661c5df3e4060db2f7f`, payload
`931c90ef394169c93f1c35b75515419a8b5dea39d2eb3ed95eb20db41d0355d7`.

`shim-boot-touch43.img` (booted: reports arrive, HID dispatched) - sha256
`c0298530cb56132dcb2c475996866268b3b2d0eaa45d13f06570ac8782bb08c1`, kernel
`ee178fa5cf5581c9e0658bc18e22fb3e4f76274d5793fb8fc40d01a1c2c957ad`, payload
`0a8e70f321e53812ef0ce25a69dc169d8fe8e2e653c6e7de0d0039dccc484104`.

`shim-boot-touch42.img` (booted twice: firmware boots, no reports yet) - sha256
`4fa9606c9d91540d45adc5b9f2a4d4a48726fc2b134279b4b5f5991fbf127388`, kernel
`77ceb6795080b8cc8ef27fe8ada128a98e00309ac4b2d27ac9ec346eb63f71cc`, payload
`b1b69aa3ef4e43773954ed366a1481601f0cc766bbf871f495610181c4d52a09`.

`shim-boot-touch41.img` (booted once: whole sequence ran, part did not boot) - sha256
`f10ea7b372ad19a54b4d786d654a3189aff35d850a11f9a8ad8f5861d2873fff`, kernel
`d0eaad011de0b80e91aed1b5e14985a5bbc28d161e4c8dc1ecd5f9597d94d8b3`, payload
`cd900db60b010b0ad701b1d585ed09699623dd64376a8dcc71665842bd929ea1`.

`shim-boot-touch40.img` (booted once, panicked after the CRC marks) - sha256
`bd5ac9b43696e585eafed4c482c3c3447ed4a39fab2df1213c79ebee0bb33c6b`, kernel
`74dcd5531f6cc7cb2a85236fb06434098471d286f63adc4ff17f884fc77965f5`, payload
`b4724b6b8aebe6287bd576e4d39dfa2753610a50668cae3b856e7b65852f4fc2`.

`shim-boot-touch39.img` (booted once, partial log; the download logic is identical) - sha256
`5d557c11e24ebfb89fd0e6f41b8ce2034c9252eaccff5014e69386a452b558dc`, kernel
`a02455c574c10a60f5759fe27e064e22c6dab2be3d9c21329d28a78ad08d9db2`, payload
`67748796fece02401f96298f9ac15f2ef7e6c0e4cb79ef0a42605decc9b43e6d`.

Boot: power off completely, hold **volume down + power** to reach fastboot, `fastboot boot <img>`.
**Never let Android boot** - it reformats the `userdata` partition that holds the iPadOS root
(restore image: `~/liuqin-root/ipad71-root-nodaemon-restore.simg`). The root image that reaches
the lock screen is `~/liuqin-root/ipad71-root-nodaemon.img` (the `optin` one has a RunAtLoad GPU
test daemon that stops the UI).

After booting, wait for the touch window (the bring-up starts ~10 s after the boot settles,
`touch spi start` around 80 s in), then **wait for the last `touch fw ...` mark on the
screen** - the whole download is ~67 s from `touch fw dl start`.  Then power off, boot
recovery and pull the log with `tools/liuqin-log-pull.sh` (the pull is a recovery ADB read;
the log has to be on disk before the power cycle).

## Verified on the tablet

`shim/out/shim-boot-touch3.img` (kernel `72733ff7…`) boots with
`liuqintouch=2` and prints:

```
LIUQIN TOUCH: GCC QUPV3_WRAP0_CBCR 0x40028bc0 -> 0x4002cbc0
LIUQIN TOUCH: s4 RCG cmd 0x00000002 (root-on update-done) halt 0x00004220 (running)
LIUQIN TOUCH: gpio16 ctl 0x88   (17, 18, 19 likewise: mux 2 = qup4, 6 mA)
LIUQIN TOUCH: gpio21 ctl 0x2 (input, pull-up)
LIUQIN TOUCH: SE4 status 0x00000000 fwrev 0x00000102
```

So the wrapper clock, the s4 RCG, the pin mux and SE register access all work.
ABL had prepared none of it: the audit (`liuqintouch=1`) showed the s4 branch
halted, the RCG at `ROOT_OFF`, and all five pins at mux 0 / pull-down.

## First boot of touch4: the SPI command is rejected

`shim-boot-touch4.img` booted (`liuqintouch=3`, log pulled from the tablet),
reached the same place as before, and printed:

```
LIUQIN TOUCH: SE4 GENI protocol 1 (SPI) fwrev 0x00000102
LIUQIN TOUCH: QUP hw 0x30080000 (v3.8) TX fifo 16 words x 32 bit, RX fifo 16 words
LIUQIN TOUCH: SPI master mode 0, 8-bit words, CS0, 1200 kHz (19.2 MHz / 16), fifo 4 byte/word, trans_cfg 0x00000000 slave_en 0x00000000
LIUQIN TOUCH: Novatek trim id 00 00 00 00 00 00 (read failed) irq 0x00000002
LIUQIN TOUCH: fwinfo read failed, report space 1800x2880
LIUQIN TOUCH: report thread up, INT on gpio21
LIUQIN TOUCH: gpio21 in/out 0x00000000 (in 0 out 0) mux 0 pull 2
LIUQIN TOUCH: event read failed (irq 0x00000002)      (four times)
```

What this says:

- The engine geometry is what the driver assumed (QUP v3.8, 16x32-bit FIFO),
  the SPI configuration registers took the writes, and the boot itself was
  unaffected.
- **`irq 0x00000002` is `M_CMD_OVERRUN`** and it comes back for every command
  (the trim id, the FWINFO and all event reads).  The SE is not executing the
  commands at all, so the six zero bytes are a bus-side failure, not a silent
  touch chip.
- `gpio21 in/out 0x0` means the INT line is being held low, i.e. the touch
  controller is powered and driving it — the die is alive.
- **The pins were muxed to the wrong function.**  Stage 2 wrote mux 2, but the
  TLMM mux value is the index of the function in the pin's own list, and
  `pinctrl-sm8450.c` lists `gpio16..19` as `PINGROUP(n, qup4, phase_flag, ...)`
  — qup4 is **1**, phase_flag is 2.  The old audit line even checked
  `mux == 1` for "(qup4)", so the code contradicted itself.  With mux 2 the SPI
  signals never reach the pads.

With the pin mux wrong, no SPI traffic could work regardless of the engine, so
0156 fixes that.  It does *not* explain the overrun — the mux only decides
whether the signals reach the pads, not whether the SE accepts a command — so
0156 also makes the driver find out why, instead of guessing (next section).
One useful data point was gained: the touch chip is alive, powered, and its INT
line reads correctly.

## Second boot (touch5): this serial engine has no FIFO interface

The mux fix and the diagnostics turned the picture around.  The same boot now
says:

```
LIUQIN TOUCH: gpio16 ctl 0x00000084 -> 0x00000084     (mux 1 = qup4, 6 mA)
LIUQIN TOUCH: gpio21 ctl 0x00000003 (input, pull-up)
LIUQIN TOUCH: before init SE4 status 0x00000000 irq 0x00000000 irqen 0x33c0007e ctrl 0x00000000 cmd0 0x00000000 gp_len 0x00000000
LIUQIN TOUCH: before init SE4 clk 0x00000011 sel 0x00000000 out 0x0000007f cgc 0x0000007f gsi 0x0000000f seirq 0x00000000 ifdis 0x00000001
LIUQIN TOUCH: after init SE4 clk 0x00000101 ... gsi 0x00000000 seirq 0x0000000f ifdis 0x00000001
LIUQIN TOUCH: CS assert as configured: after-cmd 0x00000041 first-irq 0x00000000 irq 0x00000001
LIUQIN TOUCH: SE4 running, chip select by hand
LIUQIN TOUCH: Novatek trim id 00 00 00 00 00 00 (read failed) irq 0x00000002
```

Reading it:

- The pin mux is right now (`0x84`), and the CS command **works**:
  `after-cmd 0x41` shows the SE latched it (`M_GENI_CMD_ACTIVE`), `irq 0x1`
  is `M_CMD_DONE`.  The command interface is fine.
- **`ifdis 0x00000001`**: `GENI_IF_DISABLE_RO.FIFO_IF_DISABLE` is set, so this
  serial engine **has no CPU FIFO interface at all**.  The CS command carries no
  data and completes; the page write is a FIFO transfer and comes back as
  `M_CMD_OVERRUN`.  That is the whole story of touch4/5.
- `before init` also shows `gsi 0x0f` (GSI event enables): whatever drove this
  touch controller before us — the bootloader / the Android driver — used **GPI
  DMA**, and so does Linux here (`spi_geni_init()` reads `GENI_IF_DISABLE_RO`
  and picks GPI DMA when the FIFO is absent).
- `clk 0x11` before our write and `0x101` after: the divider write landed.

So the data path has to be DMA.  Qualcomm's own GENI SPI driver ([U-Boot v3,
Aug 2026](https://patchwork.ozlabs.org/project/uboot/patch/20260828-geni-spi-v2-review-v3-1-228729b62381@oss.qualcomm.com/))
states that SE-DMA is always available and falls back to it when the FIFO is
missing, which is what 0157 implements: `SE_GENI_DMA_MODE_EN`, the
`SE_DMA_{TX,RX}_PTR_{L,H}` / `_LEN` / `_ATTR` registers, the DMA IRQ statuses,
one physically contiguous staging page (`kmem_alloc_contig`, TX and RX halves
256 bytes apart, cache flushed around every transfer).  The FIFO path is kept
for engines that do have one.

## Third boot (touch6): the machine wedges during the touch bring-up

`shim-boot-touch6.img` (the SE-DMA build) **hangs** right at the touch window
and has to be rebooted.  The screen stops in the middle of the persistent-log
tail dump, about 0.1 s after "log thread started at 48.521 s".

What the screen can and cannot say:

- With `liuqinmute=1` the console draws only the forced `LIUQIN BOOT:` marks;
  every `LIUQIN TOUCH:` line goes to the persistent log only.  So the absence
  of touch lines on the screen means nothing.
- The mount thread prints those lines *behind* the log thread's 60-line tail
  dump (the console is serialised), which is why a hang inside the touch code
  can still leave the screen at an early dump line.

So the hang is somewhere in `liuqin_touch_spi_setup()` — the only new code in
touch6 is the SE-DMA path — but the screen cannot narrow it down, and the log
thread froze before its 2 s flush, so that boot's log is gone.  The prime
suspects are `kmem_alloc_contig()` in that context and the first DMA
transaction (a bad bus transaction from the SE can wedge the interconnect,
which freezes every CPU, not just the touch thread).

0158 makes the next attempt survive long enough to tell us where:

- the SPI bring-up runs from its own thread **10 s after the boot has settled**
  (`liuqintouchdelay`, 0-120 s) instead of from the mount path, so a wedge no
  longer takes the boot's own log dump down with it and the log has flushed
  everything that led to it;
- `liuqin_boot_mark()` is called at every step, and those marks *are* drawn on
  screen even with `liuqinmute=1`: `touch clocks+pins ready`, `touch spi start`,
  `touch id read`, `touch spi failed`, `touch reports up`.  **The last of those
  on screen is exactly how far it got.**
- the DMA FSMs are reset once at init (the bootloader drove this SE over GPI
  DMA), and a zero physical address is rejected;
- the boot line uses `ufslogsecs=1` so the persistent log flushes every second.

## Fourth boot (touch7): the mark appears, then nothing

The deferral worked exactly as intended: the screen shows

```
LIUQIN BOOT: touch spi start at 61.932 s (+138 ms)
```

and **no line after it** — neither `touch id read`, nor `touch spi failed`, nor
any later boot mark.  The boot itself had long since settled (exec marks were
still being drawn at 61.4 s), so the wedge is inside
`liuqin_touch_spi_setup()`, after the mark and before the function returns.
The `LIUQIN TOUCH:` lines inside it were muted, so the screen cannot say which
of them was the last one, and a full wedge freezes the log thread before it can
flush that second.

0159 therefore un-mutes just the bring-up: the setup thread sets
`liuqin_console_force = 1` for the duration of `liuqin_touch_spi_setup()` and
the Novatek reads, and back to 0 afterwards.  Those are the lines the
persistent log already got; now the screen gets them too.  Two extra boot marks
split the function: `touch spi cfg` (before the FIFO/DMA decision and the
staging page) and `touch bus probe` (before `tch_se_start()`).

## Fifth boot (touch8): the wedge is the first DMA transfer

The screen (read back from the photo) shows both marks, one millisecond apart:

```
LIUQIN BOOT: touch spi cfg at 418.428 s (+3 ms)
LIUQIN BOOT: touch bus probe at 418.429 s (+0 ms)
```

with none of the `LIUQIN TOUCH:` lines between them.  That is a bug in 0159,
not a missing step: `liuqin_boot_mark()` sets `liuqin_console_force = 1` for its
own line and then **back to 0**, so the un-mute was cancelled by the very mark
that followed it.  The lines were printed (into the log) but not drawn.

The two marks still settle the question: `touch bus probe` is printed *after*
`tch_dma_alloc()` and the after-init dump, so those succeeded — the staging
page exists and `kmem_alloc_contig()` is fine.  The wedge is inside
`tch_se_start()`, i.e. either the `CS_ASSERT` probe or **the first SE-DMA
transfer**, which is the only new thing on that path.

Why a DMA transfer can take the whole machine down, not just the thread: a
transaction the interconnect cannot decode stalls the fabric, and every CPU's
memory traffic with it — which is exactly what "the screen stops and nothing
ever prints again" looks like.  The address is the prime suspect: this port's
ADT puts DRAM at `0x840000000` (33 GiB), and the SE is handed that physical
address in `SE_DMA_TX/RX_PTR_{L,H}`.  If the engine expects an IOVA (as Linux's
`dma_map_single()` would provide through the QUP's SMMU context) or issues a
narrower address than the fabric can decode, the transaction goes nowhere good.

0159's un-mute is fixed (`tch_visible_mark()` re-asserts the flag after every
boot mark), and `tch_dma_xfer()` prints the first four transfers before arming
them:

```
LIUQIN TOUCH: DMA #1 tx+rx len 1 tx 0x8400xxxxxx rx 0x8400xxxxxx mode 0x…1
```

That line, or the one before it, will be the last thing on the screen.

## Sixth boot (touch9): the addresses explain the wedge

With the console un-muted properly, the whole bring-up came out on screen:

```
LIUQIN BOOT: touch spi start at 512.129 s (+13 ms)
LIUQIN TOUCH: before init SE4 … gsi 0x0000000f seirq 0x00000000 ifdis 0x00000001
LIUQIN TOUCH: SE4 GENI protocol 1 (SPI) fwrev 0x00000102
LIUQIN TOUCH: QUP hw 0x30080000 (v3.8) TX fifo 16 words x 32 bit, RX fifo 16 words
LIUQIN TOUCH: SPI master mode 0, 8-bit words, CS0, 1200 kHz (19.2 MHz / 16), …
LIUQIN BOOT: touch spi cfg at 512.132 s (+2 ms)
LIUQIN TOUCH: DMA staging page at 0xfffffe85eff94000 (phys 0x8a0150000)
LIUQIN TOUCH: FIFO interface disabled (if_disable 0x00000001)
LIUQIN TOUCH: after init SE4 … gsi 0x00000000 seirq 0x0000000f ifdis 0x00000001
LIUQIN BOOT: touch bus probe at 512.134 s (+2 ms)
LIUQIN TOUCH: CS assert as configured: after-cmd 0x00000041 first-irq 0x00000000 irq 0x00000001
LIUQIN TOUCH: DMA #1 tx+rx len 1 tx 0x8a0150000 rx 0x8a0150100 mode 0x00000000
```

and then the machine is gone.  So: the staging page is at **0x8a0150000
(34.5 GiB)**, the `CS_ASSERT` command still completes, and the **first SE-DMA
transfer** is what wedges the interconnect.

That is the answer to the DMA question, and it is not a software detail of our
driver: this port's ADT only describes a kernel bank above 32 GiB
(`--dram-base 0x840000000`), and the QUP's DMA engine is handed that address.
Every reference implementation (Qualcomm's U-Boot GENI SPI driver, Linux's
`geni_se_tx_dma_prep`) uses the address space of the wrapper's DMA context —
on this board Linux feeds the QUP IOVAs through the apps SMMU, never a 34 GiB
physical address.  Without that translation the engine's transaction cannot
complete, and a transaction the fabric cannot decode stalls every CPU with it.
Making SE-DMA work therefore means either moving the kernel bank below 4 GiB
(a boot-image change, not a driver one) or programming the apps-SMMU for the
QUP SID — both far beyond a touch driver patch, and both risky for UFS/GPU.

## What touch10 does instead: software SPI on the same pads

The four pads are ordinary TLMM pins.  When the engine is unusable, 0161 muxes
gpio16..19 back to GPIO and drives them directly — mode 0, MSB first, chip
select on gpio19, a few hundred kHz to a couple of MHz depending on how fast
the MMIO accesses retire.  No DMA, no fifo, no SMMU, and nothing that can wedge
the fabric: a wrong waveform produces wrong bits, not a dead machine.

Nothing in the device tree says which of gpio16/17/18 is the clock, which is
MOSI and which is MISO, so the driver **tries all six assignments** and keeps
the one that answers with a Novatek trim id (`… 65 03`).  Every attempt is
logged, so a failure is informative too.

## Seventh boot (touch10): the software SPI runs, but MISO is dead

The bit-banged build did exactly what it was supposed to: **no freeze**, the
boot carried on (exec marks continued), and all six pad assignments were tried:

```
LIUQIN TOUCH: SE DMA not usable here, using bit-banged SPI
LIUQIN TOUCH: bitbang 0: clk gpio16 mosi gpio17 miso gpio18 -> 00 00 00 00 00 00
LIUQIN TOUCH: bitbang 1: clk gpio16 mosi gpio18 miso gpio17 -> 00 00 00 00 00 00
LIUQIN TOUCH: bitbang 2: clk gpio17 mosi gpio16 miso gpio18 -> 00 00 00 00 00 00
LIUQIN TOUCH: bitbang 3: clk gpio17 mosi gpio18 miso gpio16 -> 00 00 00 00 00 00
LIUQIN TOUCH: bitbang 4: clk gpio18 mosi gpio16 miso gpio17 -> 00 00 00 00 00 00
LIUQIN TOUCH: bitbang 5: clk gpio18 mosi gpio17 miso gpio16 -> 00 00 00 00 00 00
LIUQIN TOUCH: no pad assignment answered with a Novatek id
```

All zeros from every assignment.  Two very different explanations remain, and
they need different fixes, so the next build measures instead of guessing:

1. **our side**: the pads are not actually being driven or read the way we
   think (wrong OE bit, wrong output bit, wrong mux), so the controller never
   sees a clock and never drives MISO;
2. **the controller's side**: the chip is powered and pulling INT low (so the
   die is alive), but it does not drive MISO — it may be held in reset, or its
   firmware may not be running, in which case it needs the vendor's bring-up
   writes (`0x69` bootloader reset, `0xA5` software reset idle, `0x35` unlock)
   before it answers.

## What touch11 measures

Before any permutation, 0162 runs an electrical self-test on gpio16..19:

- drive each pad 0 then 1 as a GPIO output and **read the pad back** — this
  proves the mux, the output-enable bit and the output-value bit are right;
- bias each pad pull-down then pull-up as an input — a pad the chip drives
  does not follow the bias, a floating one does.  This is how we find out
  which pad (if any) the controller is driving, without assuming the DT order;
- finally it reads gpio21, the INT line that is known to sit at 0, as a
  reference that the input path works at all.

## Eighth boot (touch11): what the pad test actually said

```
pad gpio16 out: drive0 reads 0, drive1 reads 0  <-- does not follow (ctl 0x00000280)
pad gpio17 out: drive0 reads 0, drive1 reads 1
pad gpio18 out: drive0 reads 0, drive1 reads 1
pad gpio19 out: drive0 reads 0, drive1 reads 1
pad gpio16 in: pull-down 0, pull-up 0 (driven)
pad gpio17 in: pull-down 1, pull-up 1 (driven)
pad gpio18 in: pull-down 1, pull-up 1 (driven)
pad gpio19 in: pull-down 1, pull-up 1 (driven)
pad gpio21 in: 0 (INT, expect 0) io 0x0 ctl 0x3
bitbang 0: clk gpio16 mosi gpio17 miso gpio18 -> ff ff ff ff ff ff
bitbang 1..5 -> 00 00 00 00 00 00
```

Two flaws in the test itself, both now fixed:

- the read-back happened a few nanoseconds after driving, and the bias was
  read `bb_pace()` (a handful of `nop`s) after changing it.  Pads have
  capacitance and the pulls are weak, so those reads reflect the *previous*
  level rather than the new one.  gpio16's "drive1 reads 0" may be exactly
  that; the `ctl 0x280` readback proves the register side is right.
- the bit-bang clock ran at MMIO speed (three accesses per bit, ~300 ns), and
  the port only reported the trim id.

The one genuinely interesting datum: **bitbang 0 (clock on gpio16, which may
not rise) returned `ff`, while bitbang 2 (clock on gpio17, MISO on gpio18)
returned `00`** — the MISO line changes when a working clock is applied, which
is what a live controller looks like.  So the chip is probably answering; the
`00`s are more likely a command/address that the controller does not find
meaningful than a dead bus.

## What touch12 changes

- bit-bang half-period is now a real `delay(5)` (~100 kHz) and tunable with
  `liuqintouchpace=<us>`; slow enough that rise time, capacitance and any board
  pull-down cannot matter.
- the pad test settles: 10 us after a drive, 2 ms after a bias change, plus a
  rise probe (`immediately` vs `after 50 us`) so a slow line is not mistaken
  for a dead one.
- each pad assignment now reads **both** the trim id and the FWINFO block, and
  accepts either a Novatek id (`… 65 03`) or a self-consistent FWINFO
  (`fw_ver + ~fw_ver == 0xff`).  If the chip is alive but 0x3f004 is the wrong
  address for it, FWINFO at 0x2fe78 still identifies it.

## Ninth boot (touch12): one pad is held low, and nothing answers

With the slow clock and settled reads the picture is much cleaner:

```
pad gpio16 out: drive0 reads 0, drive1 reads 0  <-- does not follow (ctl 0x00000280)
pad gpio16 rise: immediately 0, after 50 us 0
pad gpio17/18/19 out: drive0 reads 0, drive1 reads 1
pad gpio17/18/19 rise: immediately 1, after 50 us 1
pad gpio17/18/19 in: pull-down reads 0, pull-up reads 1 (floating)
pad gpio21 in: 0 (INT, expect 0)
bitbang 0 (clk16) -> ff … | fw ff ff ff ff
bitbang 1..5 -> 00 … | fw 00 00 00 00
```

- gpio17/18/19 behave like normal pads: they drive, they rise immediately, and
  with the bias applied they float.  So our GPIO path is correct.
- **gpio16 cannot be driven high at all**, not even with 50 us to settle: the
  board (or the controller) holds it low through something stronger than a
  6 mA pad driver.
- bitbang 0's `ff` is now explained: its clock is gpio16, which never rises, so
  the controller is never clocked and the floating MISO reads high.  With a
  working clock (bitbang 1..5) MISO reads low — the chip-select may simply not
  be asserted, or MISO is the pad that is held low.

So no assignment answers, and one of the four pads is electrically held down.
Two things follow: the chip select may not be the pad the device tree names,
and the controller may not be in a state that answers at all.

## What touch13 tries

1. **All 24 role assignments**, not 6: each of gpio16..19 can be the chip
   select, the other three take the six clock/MOSI/MISO orders.  Each one is a
   trim-id and FWINFO read at ~100 kHz.
2. If none answers, **the vendor bring-up sequence** on the most likely pads —
   `cs 19, clk 17, mosi 18, miso 16` (gpio16 is the externally-held-low line,
   so it is the most likely MISO): write `0x69` (bootloader reset), wait 40 ms,
   `0xA5` (software reset idle), wait 20 ms, `0x35` (unlock), wait 20 ms, then
   read the id and FWINFO again.  This is exactly what the vendor driver does
   before trusting the bus, and the NT36532 answers it from ROM even when its
   RAM firmware is not loaded.

## Tenth boot (touch13): every role fails, the bring-up does not wake it

The 24 assignment sweep came back empty:

```
bitbang 1..24 -> 00 00 00 00 00 00 | fw 00 00 00 00
                (bitbang 1, 10 and 12 print ff instead)
none of the 24 pad assignments answered with a Novatek id
trying the vendor bring-up on cs19 clk17 mosi18 miso16
after bring-up: 00 00 00 00 00 00 | fw 00 00 00 00
controller still silent after the bring-up
```

So on this tablet the controller does not answer SPI on any combination of
gpio16..19, with or without the vendor's `0x69 / 0xA5 / 0x35` bring-up.  The
electrical oddity from touch12 stands: gpio16 cannot be driven high, the other
three behave like normal pads, and the `ff` readings are the ones where MISO is
left floating with no valid clock or chip select.

That leaves two very different worlds, and they need a test that does not use
SPI at all: **is the touch MCU running?**

## What touch14 does: watch the INT line

gpio21 is the controller's interrupt output.  A running touch MCU raises it when
a finger lands.  After the bit-bang probe gives up, the driver now starts a
thread that samples gpio21 every millisecond for **60 seconds**, with the
console unmuted, and logs every transition:

```
LIUQIN BOOT: touch int watch at …
LIUQIN TOUCH: watching gpio21 (INT) for 60 s, start is 0 - touch the panel now
LIUQIN TOUCH: gpio21 -> 1 after 1834 ms (1 transitions)
LIUQIN TOUCH: gpio21 -> 0 after 1849 ms (2 transitions)
…
LIUQIN TOUCH: gpio21 watch finished, N transitions
```

This is the decisive split:

- **transitions while a finger is on the panel** → the MCU is alive and
  scanning, so the firmware is there and only our bus is wrong.  The next work
  is on the electrical/protocol side (and the pin roles are still open).
- **no transition in 60 s** with the panel touched → the touch MCU is not
  running.  Then this is not a driver or wiring problem any more: the die is
  either not powered the way the vendor kernel powers it, or it has no
  RAM-loaded firmware (`novatek_nt36532_m81_fw_csot.bin`).  The next step would
  be the firmware download, which is a much larger piece of work and should be
  agreed before starting.

## Eleventh boot (touch14) and the real cause

The INT watch ran its full 60 s with **0 transitions**.  A controller that
neither answers SPI nor raises its interrupt is not scanning, and the reason
turned out not to be in the mainline device tree at all.

`backup/dtbo.img` — the vendor overlay the tablet actually boots — contains the
touch node (`novatek,NVT-ts-spi`, gpio21 IRQ, lcd-id gpio75) and, next to it,
the pin states the vendor kernel uses:

```
pmx_ts_reset_suspend/ts_reset_suspend:
    pins = gpio20; function = gpio; bias-pull-down      <- reset asserted
mi_ts_active:
    pins = gpio20, gpio21; function = gpio; drive 8 mA; bias-pull-up
```

**gpio20 is the touch controller's reset line, active low, and the mainline
DTS this port was built from does not mention it at all.**  Nothing in our boot
path has ever driven it, so it sits at its pull-down: the controller has been
held in reset the whole time.  That single fact explains every measurement in
this session:

- no SPI answer on any of the 24 pin assignments (the die was in reset);
- INT never moving (the MCU was not running);
- gpio16 unable to be driven high and the other three floating (the die's pads
  were powered down or in reset, so only board bias was visible);
- the vendor bring-up writes doing nothing (they went to a chip in reset).

## What touch15 does

Before the pad self-test, the bit-bang bring-up now drives the reset line the
way the vendor's active state does: **gpio20 low for 5 ms, then high, then 30 ms
for the MCU to come up**, and logs the pad state and the INT level right after:

```
LIUQIN TOUCH: gpio20 reset released (ctl 0x00000280 io 0x…), INT now …
```

Everything else is unchanged: the pad self-test, the 24 role assignments, and
the vendor bring-up if they all fail.

## Twelfth boot (touch15): the reset was real, the silence remains

The reset release did what it should:

```
LIUQIN TOUCH: gpio20 reset released (ctl 0x00000280 io 0x00000003), INT now 0
```

`io 0x3` means the pad is driven high and reads high, so the controller's reset
is deasserted - and yet:

- all 24 role assignments still read `00` (or `ff` where MISO floats),
- the vendor `0x69 / 0xA5 / 0x35` bring-up still changed nothing,
- the INT watch ran another 60 s with **0 transitions**.

So gpio20 was a real find and a real fix to keep, but it was not the whole
story: with the die out of reset, nothing on the bus or the interrupt line
moves.  Either the die is not powered the way the vendor powers it, or its
ROM/firmware never gets to the point of serving SPI.

## What touch16 tries (software-only, per the chosen path)

1. **Reset shapes**, each followed by a trim-id + FWINFO probe on the most
   likely role assignment (cs19 clk17 mosi18 miso16, gpio16 being the pad the
   board holds low):
   - `100 ms low -> high, 200 ms settle` (the normal active-low reset, longer
     than the 5 ms touch15 used);
   - `100 ms high -> low, 200 ms settle` (in case the polarity is inverted);
   - `100 ms low, pads muxed back to qup4, then high, 200 ms` (in case the die
     latches an interface strap from the bus state at reset).
2. **The 24-role sweep twice**: once with chip select active low, once active
   high (a board-level inverter would make our "asserted = low" never select
   the part).
3. The vendor bring-up on the best guess, then the INT watch as before.

## Thirteenth boot (touch16): every software variant is silent

```
gpio20 100 ms low -> high (io 0x3), INT 0
reset low->high:  cs19 clk17 mosi18 miso16 -> 00 … | fw 00 …
gpio20 100 ms high -> low (io 0x0), INT 0
reset high->low:  … -> 00 … | fw 00 …
gpio20 released with the pads on qup4, INT 0
reset released on qup4: … -> 00 … | fw 00 …
sweeping again with chip select active high
none of the 48 pad assignments answered with a Novatek id
controller still silent after the bring-up
```

So: both reset polarities, releasing the reset with the pads on the QUP, all
24 roles with chip select active low **and** active high (48 assignments), and
the vendor `0x69 / 0xA5 / 0x35` bring-up — none of it produces a single bit from
the controller, and its INT line never moves in 60 s.

At this point the software side has been taken as far as it can go without new
information.  Everything a driver can get wrong on this path has been tried or
measured:

- bus setup (SE clocks, pin mux, GENI config, FIFO/DMA, 100 kHz bit-bang),
- pin roles (all 24, both chip-select polarities),
- the controller's reset line (gpio20, found in the vendor overlay),
- the vendor's own ROM bring-up sequence.

## Where to go next: measure, do not guess

The remaining possibilities are electrical, and each has a distinct measurement:

| Measurement | If it shows | Meaning |
| --- | --- | --- |
| logic analyser on gpio16..19 while the sweep runs | our clock/data waveforms present at the SoC pads | the signals reach the pads; the die is not answering |
| same, probing at the touch connector if reachable | same waveforms at the chip | wiring is fine |
| multimeter on the touch's VDD/VDDIO | rail at 0 V / low | **the die is unpowered** — this is the vendor-power-sequence path (an RPMh/PMIC vote, like the nabu port's `ldoc11`), and it would explain every silent measurement exactly |
| INT line (gpio21) with the reset released | driven low by the chip vs. only board bias | whether the die is alive at all |
| vendor stock Android on the same unit | touch works there | confirms the hardware and gives the vendor's init sequence to copy |

If the rail turns out to be off, the work is a small RPMh/PMIC vote path — a
different subsystem from the touch driver, and the honest next milestone.
If the rail is on and the waveforms reach the chip, the next step is the
vendor driver source (Xiaomi's `liuqin` branch) for its exact bring-up.

## Where we are (2026-10-24, after touch16)

The tablet's touch **works under TWRP**, so the die is powered and reachable
after ABL's `fastboot boot`; what is missing is our bring-up, not the hardware.
The same ABL handover feeds TWRP's kernel, whose vendor driver initialises the
part successfully, so the rail and the pins are fine.

Our own attempts are all silent: SE FIFO (the engine has none), SE DMA (wedges
the fabric at a 34 GiB address), bit-banged SPI at 100 kHz across all 24 pin
roles and both chip-select polarities, three reset shapes on gpio20, and the
vendor `0x69 / 0xA5 / 0x35` sequence on one guessed pinout.

The merged NT36xxx driver (AngeloGioacchino Del Regno, 2020; downstream-derived,
the same protocol as the vendor's) shows what the bring-up looks like and it
differs from ours in one important way: **the reset into the bootloader happens
before the first read**, and the read then uses the chip-info page:

```
reset gpio requested as GPIOD_OUT_HIGH (asserted high = out of reset)
usleep(10 ms)
write {0x00, 0x69}  /* bootloader reset */   msleep(35)
write {0x00, 0xA5}  /* software reset idle */ usleep(15 ms)
write {0x00, 0x35}  /* unlock */              usleep(10 ms)
set_page(0x0001f64e); read 7 bytes starting with event 0x4e (chip id)
```

We only ever sent that sequence on one guessed pin assignment.  If the roles
are wrong, the reset bytes go nowhere and every later read is silent - which is
exactly what we have been seeing.

## What touch17 does

- The 48-assignment sweep now sends the **vendor bring-up before every
  assignment** (so each pinout gets its own `0x69 / 0xA5 / 0x35`, with the
  vendor's delays), then reads:
  - the trim id at `0x3f004`,
  - the FWINFO block at `0x2fe78`,
  - the chip-info page at `0x1f64e` (the older/alternate id location).
- Any byte that is neither `00` nor `ff`, or a `65 03` id, or a self-consistent
  FWINFO, counts as an answer and stops the sweep.

## Fourteenth boot (touch17): the controller answers

Sending the vendor bring-up before every assignment did it.  Out of the 48
tried, exactly one answers:

```
LIUQIN TOUCH: bitbang 10: cs gpio17 clk gpio18 mosi gpio19 miso gpio16
    -> 83 80 3a 4f bb 9d | page b7 c0 1d 27   <-- ANSWER
```

So the roles on this board are **MISO = gpio16, CS = gpio17, CLK = gpio18,
MOSI = gpio19** - not the grouping the device tree suggests (its "cs" group is
gpio19, which is actually MOSI here).  That also explains the old gpio16
mystery: the pad could not be driven high because the controller drives it.

The bytes are deterministic (the driver's follow-up read returned the same
`83 80 3a 4f bb 9d`) and change with the address, so this is a real chip, not
crosstalk.  But they are not a Novatek id, and the FWINFO does not self-check -
the prime suspect is the **page addressing**: the harness this port used splits
the address as `addr >> 15 / addr >> 7` (its NT36523 origin), while the vendor
NT36xxx driver sets the page register from bits 23:16 and 15:8.

## What touch18 does

- Starts from the discovered assignment (cs17 clk18 mosi19 miso16), sends the
  vendor bring-up once, and reads the trim id, FWINFO and chip-info page with
  **both page splits** (`>>15/>>7` then `>>16/>>8`).
- Only if neither yields a valid id (`65 03`) or a self-consistent FWINFO does
  it fall back to the 48-assignment sweep.
- The HID report thread is now started **only on a trustworthy answer**; with a
  garbage id the driver watches the INT line instead of feeding random
  coordinates to the system.

## Fifteenth boot (touch18): the answer is real but unstable

```
reading with the addr>>15/>>7 page split
discovered pins: cs17 clk18 mosi19 miso16 -> ff ff ff ff ff ff | page ff ff ff ff
reading with the addr>>16/>>8 page split
discovered pins: ... -> ff ff ff ff ff ff | page ff ff ff ff
...
bitbang 10: cs gpio17 clk gpio18 mosi gpio19 miso gpio16
    -> 83 81 3a 47 bb 9c | page b7 c0 9d 23   <-- ANSWER
touch no valid id
```

Two things are now clear:

- The `>>16/>>8` split reads `ff` (nothing at those pages), while `>>15/>>7`
  returns data - so this part uses the 15/7 page split, and the earlier
  "wrong split" theory is out.
- The data **changes between boots and between accesses** (`83 80 3a 4f bb 9d`
  in touch17, `83 81 3a 47 bb 9c` here, and `ff` on the direct attempt a few
  milliseconds earlier).  That is not a protocol mismatch: it is a **marginal
  read** - the line, the sampling, or the framing is not reliable enough to
  trust a byte.

The HID gate did its job: with no valid id the driver watched INT instead of
feeding garbage coordinates to the system.

## What touch19 measures

On the known pins (cs17 clk18 mosi19 miso16, 15/7 split), a reset and then one
read per variant:

| variant | what it separates |
| --- | --- |
| baseline (5 us half period, 1 dummy) | reference |
| 25 us half period | sampling/rise time too fast |
| MISO with a pull-up | the line is driven (chip) vs floating (board bias) |
| 0 dummy bytes / 2 dummy bytes | the data is misaligned in the frame |

Those five lines decide the next move: a variant that yields `65 03` is the
answer; a pull-up that changes the bytes means the line is floating; a
pull-up that does not means the controller really drives it and the problem is
timing/framing.

## Sixteenth boot (touch19): a third, different "answer"

```
discovered pins: cs gpio17 clk gpio18 mosi gpio19 miso gpio16
    -> 03 ff ff ff ff ff | page 01 ff ff ff   <-- ANSWER
```

That is a third distinct pattern (`83 80 3a 4f bb 9d` in touch17, `83 81 3a 47
bb 9c` in touch18, `03 ff ff ff ff ff` here, with `ff` on some direct
attempts).  The loose rule from 0168 ("any byte that is not 00/ff counts")
stopped the sweep here, so the diagnostic matrix never ran - the driver went
straight to `touch no valid id`, which at least kept garbage out of HID.

Three different results for the same read means one of the read parameters is
wrong in a way that changes with timing.  The one knob never varied is the
**SPI mode**: a wrong sampling edge turns every read into a mixture of the
real bits and the previous bit, which is exactly the kind of plausible but
boot-dependent data we are seeing.

## What touch20 does

- A stricter answer rule: only a real Novatek id (`65 03`) or a
  self-consistent FWINFO stops the sweep, so the diagnostics always run.
- The bit-bang transfer now honours CPOL and CPHA, and the matrix walks **all
  four SPI modes at 5 us and 25 us half-period** on the known pins, with a
  vendor bring-up before each and both the id and the FWINFO read:

```
LIUQIN TOUCH: mode 0 at 5 us: xx xx xx xx xx xx | fw xx xx
LIUQIN TOUCH: mode 1 at 5 us: …
…
LIUQIN TOUCH: mode 3 at 25 us: …
LIUQIN TOUCH: MISO with pull-up: …
```

One of those eight lines should carry a recognisable id if the bus is sound and
only the mode was wrong.

## Seventeenth boot (touch20): the answer was an artifact

```
discovered pins direct read: 03 ff ff ff ff ff | page 01 ff ff ff
mode 0 at 5 us:  ff ff ff ff ff ff | fw ff ff
mode 1 at 5 us:  ff ff ff ff ff ff | fw ff ff
mode 2 at 5 us:  ff ff ff ff ff ff | fw ff ff
mode 3 at 5 us:  ff ff ff ff ff ff | fw ff ff
mode 0..3 at 25 us: ff ff ff ff ff ff | fw ff ff
MISO with pull-up: ff ff ff ff ff ff | fw ff ff
```

In every mode and at both clock rates the MISO line reads all ones, with and
without our pull-up, so the controller is **not driving it at all**.  The
occasional non-zero bytes (combo 10 in the sweep) are a pad artifact: its
neighbours were driving that pad microseconds earlier, and once the matrix runs
on clean pins the line is simply idle high.  The `03`/`01` first bytes are the
tail of our own address phase.

So the controller has never answered this session.  It does answer TWRP's
kernel, and that gives one concrete hypothesis left that is cheap to test:
**the part may still be running from a previous boot** (ABL, or a preceding
TWRP session, loaded its RAM firmware), and every image so far has started by
pulsing gpio20 - which discards that firmware and leaves the part silent.

## What touch21 does

- **Probes before any reset**: drives gpio20 high (deassert only, no pulse),
  waits 20 ms, and reads the id + FWINFO on the discovered pins in all four SPI
  modes.  A live controller is caught here, without its firmware being wiped.
- Only if that fails does it do the vendor bring-up and the rest of the
  diagnostics (page splits, the mode matrix, the 48-assignment sweep).

### How to run it (this matters)

1. Boot TWRP and confirm touch works there (that leaves a **running**
   controller with its firmware in RAM).
2. From TWRP, reboot to the bootloader **without a power cycle**
   (`adb reboot bootloader`, or the key combination).
3. `fastboot boot shim/out/shim-boot-touch21.img`.

If the controller's firmware survives the fastboot handover, touch21 sees it
before touching the reset line and reports `live controller answered in SPI
mode N` with a real id.

## The vendor driver, at last (2026-10-24, from TWRP's prebuilt modules)

TWRP for this tablet ships the working vendor modules in its device tree
(`ymdzq/device_xiaomi_liuqin-TWRP`, `prebuilt/liuqin/vendor/lib/modules/1.1/`),
including **`nt36532_touch.ko`** with full symbols and the firmware blobs
(`novatek_nt36532_m81_fw_csot.bin`, `_fw_tm.bin`, `_mp_csot.bin`, `_mp_tm.bin`,
`novatek_spi_fw.bin`).  Disassembling it settles every protocol question:

- `nvt_set_page(addr)` writes `[0xff, addr >> 15, addr >> 7]` - our 15/7 split
  is right.
- `nvt_write_addr(addr, val)` writes `[(addr & 0x7f) | 0x80, val]` after the
  page; `CTP_SPI_WRITE` sets the high bit, which we already matched.
- **`CTP_SPI_READ(buf, len)` transfers `len + 1` bytes and the driver takes the
  data from `buf[1]`: there is no dummy byte between the address and the
  data.**  We have been reading `rx[2]`, so every byte we ever fetched was
  shifted by one - which is exactly why ids never matched and why the values
  looked "plausible but wrong".
- `spi_setup` is called with `mode = 0` (also ours) and `bits_per_word = 8`.
- The probe reads `novatek,irq-gpio` and `novatek,lcd_id-gpio`, applies the
  DT pinctrl states (`pinctrl_select_state`), and **does not touch a reset
  GPIO or any regulator** - consistent with the part being powered by the
  panel rails.
- It *does* call `nvt_download_firmware`, so the RAM firmware is re-downloaded
  by the driver after a reset.

## What touch22 does

- `tch.dummy = 0`: the read is now `[addr]` followed immediately by `len`
  bytes of data and `len + 1` total bytes on the wire, exactly like
  `CTP_SPI_READ`.
- The bring-up delays follow the vendor (`18 ms` after `0x69`, `15 ms` after
  `0xA5`, `10 ms` after `0x35`).
- Ids are accepted with `65 03` at either `[0..1]` (the vendor's chip-info
  convention) or `[3..4]` (the older trim layout).
- Probes before the reset pulse, and the matrix keeps a `one dummy byte`
  control line so the difference is visible in one boot.

## Eighteenth boot (touch22): correct framing, still no answer

```
probing before any reset (gpio20 high, INT 0)
bitbang 900..903: cs17 clk18 mosi19 miso16 -> 03 ff ff ff ff ff | page 01 ff ff ff
reading with the addr>>15/>>7 page split  -> 03 ff ff ff ff ff | page 01 ff ff ff
MISO with pull-up: 03 ff ff ff ff ff | fw 07 ff
one dummy byte (old framing): ff ff ff ff ff ff | fw ff ff
```

With the vendor's own framing the bytes are `03` followed by all ones, and
the first byte tracks our address phase (`03` for the trim address, `01` for
the chip-info page).  That is an idle-high MISO line plus coupling from our
own signals - the controller is not driving the bus in our boot, whatever
pins or mode we use.

So the remaining difference between our boot and TWRP's is not the SPI
protocol any more.  TWRP is a **working reference**; the next step is to take
the ground truth from it instead of guessing:

```sh
adb shell "tar -C /proc/device-tree -cf /tmp/dt.tar ." && adb pull /tmp/dt.tar
adb shell "dmesg | grep -iE 'nvt|novatek|nt36|touch' > /tmp/nvt.log"; adb pull /tmp/nvt.log
adb shell "cat /sys/kernel/debug/regulator/regulator_summary > /tmp/reg.log 2>/dev/null"; adb pull /tmp/reg.log
```

- the runtime device tree gives the touch node's real parent bus and its
  supplies (the base DTB we never had),
- the nvt lines in dmesg give the vendor probe's order and the id it reads,
- the regulator summary says which rail feeds the touch and whether it is on.

Two zero-build experiments go with it: the TWRP-first reboot into
`shim-boot-touch22.img` (it probes before resetting), and
`ls /vendor/lib/modules/` under TWRP to see how the module is loaded.

## The TWRP dumps (2026-10-25): bus, power and the real trim address

`/proc/device-tree` from a running TWRP confirms the touch node and its power:

- `/soc/spi@990000/novatek@0`, `compatible = "novatek,NVT-ts-spi"`, `status ok`,
  `pinctrl-names = pmx_ts_active/pmx_ts_suspend`, **no supply properties**;
- `regulator_summary` shows the panel rails the die feeds from all **enabled**:
  `pm8350c_l2` (1.8 V, the DSI iovdd), `display_panel_dvdd_buck`,
  `display_panel_dvdd_ldo`, `display_panel_vsp`, `display_panel_vsn`.
  Power is not our problem.

`dmesg` from the same boot has the vendor driver's own log, and it is decisive:

```
[NVT-ts] nvt_ts_probe 3075: mode=0, max_speed_hz=9600000
[NVT-ts] nvt_parse_dt 1213: novatek,irq-gpio=322        (tlmm 21)
[NVT-ts] nvt_parse_dt 1222: novatek,lcd_id-gpio=376     (tlmm 75)
[NVT-ts] nvt_parse_dt 1227: This panel is CSOT
[NVT-ts] nvt_ts_check_chip_ver_trim 1875: chip_ver_trim_addr=0x1fb104,
          swrst_sif_addr=0x1fb43e, crc_err_flag_addr=0x1fb535
[NVT-ts] nvt_ts_check_chip_ver_trim 1902:
          buf[1]=0x20, buf[2]=0x00, buf[3]=0x00, buf[4]=0x32, buf[5]=0x65, buf[6]=0x03
```

**The trim id lives at `0x1FB104` and reads `20 00 00 32 65 03`.**  We have
been reading `0x3f004` (the nabu harness address), which on this part answers
`ff` - and that is exactly the all-ones data every probe has returned.

## What touch23 does

- `NVT_CHIP_VER_TRIM_ADDR` is now `0x1fb104`; `0x3f004` stays as a secondary.
- An answer is any adjacent `65 03` in either id read, so the check does not
  depend on where in the six bytes the signature lands.
- The probe-before-reset, the vendor framing (`dummy = 0`), mode 0 and the
  48-assignment sweep are all unchanged, so the same image also re-finds the
  pin roles if they are not what the DT grouping suggests.

## Nineteenth boot (touch23): the right address, still `03 ff ff …`

With the vendor's trim address (`0x1fb104`) the reads print

```
id detail  03 ff ff ff ff ff (expect 20 00 00 32 65 03)
```

The interesting part is the `03`: the vendor's id ends in `03`, and the rest is
all ones.  That is what a read of the *wrong page* looks like - and the reason
is the transaction structure.

The vendor's `nvt_set_page()` is a `CTP_SPI_WRITE` of its own: chip select is
asserted and released around the three page bytes, and **only then** does
`CTP_SPI_READ` assert chip select again to send the address and clock out the
data.  We kept the page bytes and the read inside a **single** chip-select
window, so the page was never latched as a separate message.

## What touch24 does

`tch_nvt_read()` and `tch_nvt_write8()` now use **two chip-select windows**:
one for the page write, one for the address plus data - byte for byte the
message boundaries the vendor driver produces.

That also applies to the bring-up sequence (`0x69 / 0xA5 / 0x35`), which goes
through `tch_nvt_write8()`.

## Twentieth boot (touch24): the id shows up, shifted by one byte

The 48-assignment sweep with the corrected address and split chip-select
transactions produced one line that is unmistakably the chip:

```
bitbang 24: cs gpio19 clk gpio18 mosi gpio17 miso gpio16
    -> 3f 20 00 00 32 65 | page 07 ff ff ff
```

`3f 20 00 00 32 65` is the vendor id `20 00 00 32 65 03` seen one byte early:
there is a **dummy byte** between the address and the data.  The pins in that
line are exactly the device tree's own grouping - **CS = gpio19** (the
`qup_spi4_cs` group) and among gpio16..18: **CLK = 18, MOSI = 17, MISO = 16** -
so the earlier "cs17" answer was indeed an artefact.

## What touch25 does

- Roles fixed to the found assignment: `cs19 clk18 mosi17 miso16`.
- `dummy = 1`: the read transfers `len + 2` bytes and takes the data from
  `rx[2]`, which is what the hardware actually needs.
- Everything else stays: vendor trim address `0x1fb104`, page write as its own
  chip-select transaction, probe before any reset, then FWINFO and reports.

Expected line:

```
LIUQIN TOUCH: live probe: cs gpio19 clk gpio18 mosi gpio17 miso gpio16
    -> 20 00 00 32 65 03 | …
```

## Twenty-first boot (touch25): the controller is identified

```
gpio20 100 ms low -> high (io 0x3), INT 0
reset low->high: cs gpio19 clk gpio18 mosi gpio17 miso gpio16
    -> 20 00 00 32 65 03 | page ff ff ff ff   <-- ANSWER
bit-banged SPI: cs gpio19 clk gpio18 mosi gpio17 miso gpio16
Novatek trim id 20 00 00 32 65 03 (NT36532)
fwinfo ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff (broken, using the panel size)
report space 1800x2880, 255 fingers, 255 buttons, chip 0xff
reports up
```

**The trim id is exactly the vendor's: `20 00 00 32 65 03`, NT36532.**  The
address (`0x1fb104`), the page transaction boundaries, the pins
(cs19/clk18/mosi17/miso16) and the dummy byte are all right now.

But the FWINFO block (application memory at `0x1f64e`) reads `ff`, and the INT
line never moves, so the controller is **not scanning**: it is sitting in its
ROM bootloader, where only the trim id answers.  Note where the id appeared in
the log - right after the 100 ms reset pulse, which is what put the part into
that state, and our reset variants still run **before** the live probe.

## What touch26 does

- The live probe now runs **first, before any reset**: it reads the trim id,
  the chip-info page (`0x1f64e`) and the event buffer (`0x2fe00`) with the
  reset line merely deasserted, and reports whether the application firmware is
  running:

```
LIUQIN TOUCH: probing before any reset (gpio20 high, INT …)
LIUQIN TOUCH: before reset: chip info xx xx xx xx | event xx xx xx xx
LIUQIN TOUCH: application firmware running | not running - controller is in bootloader mode
```

- `NVT_FWINFO_ADDR` is now the chip-info page `0x1f64e` (the vendor trim table
  lists it for this part) instead of the nabu harness's `0x2fe78`.
- Only if that probe finds nothing are the reset pulse and the rest of the
  diagnostics run, so a running controller is never reset by us.

If the answer is "bootloader mode", the next piece of work is the firmware
download (`nvt_download_firmware`): the protocol is in the vendor module we
have, and the firmware blobs (`novatek_nt36532_m81_fw_csot.bin` and friends)
are in the TWRP tree.

## Twenty-second boot (touch26/27): chip alive, UI restored, no reports yet

The white Apple logo turned out not to be touch at all: a 16 GiB `userdata`
image with a RunAtLoad GPU-test daemon (`ipad71-root-optin.img`) replaced the
root, and the system never took the display over.  Flashing
`ipad71-root-nodaemon.img` (same root, no daemon) brought the lock screen back.

touch26 also corrected an earlier conclusion.  With the probe running **before**
any reset:

```
probing before any reset (gpio20 high, INT 0)
before reset: chip info 5a 76 83 00 | event ff ff ff ff
bitbang 900: … -> 20 00 00 32 65 03   <-- ANSWER
Novatek trim id 20 00 00 32 65 03 (NT36532)
fwinfo 52 37 03 10 27 b6 34 8d f1 5c e9 02 fa 18 93 98
```

The application firmware **is** running (the chip-info page carries real data);
the "bootloader mode" reading was created by our own reset pulse.  touch26
therefore does not reset a live controller.

What is still missing: with a finger sliding on the lock screen, the report
thread saw **no INT edge and no decodable frame** - the event buffer never
produced a finger record.

## What touch27 instruments

- Every `gpio21` level change is logged (up to 12), so a touch that raises the
  interrupt is visible even if the frame does not decode.
- Every change of the first 8 bytes of the event buffer is logged (up to 12),
  whether or not it decodes, which shows what the controller writes when a
  finger is present.
- The FWINFO block counts as real when it is not all `00`/`ff` (this part does
  not use the nabu `fw[0] + fw[1] == 0xff` convention), so the report thread no
  longer falls into the "broken" path and the 255-finger/255-button values are
  gone.

## Twenty-third boot (touch27): no interrupt, no event data

With a finger sliding on the lock screen the whole boot:

```
report thread up, INT on gpio21
gpio21 in/out 0x00000000 (in 0 out 0) mux 0 pull 3
evt data ff ff ff ff ff ff ff ff (no record)
```

- The event buffer reads all `ff` - on this part that is an unmapped/empty
  region, not "no fingers" (a quiet buffer reads `00`).
- `gpio21` never asserts; the 10 ms poll in the report thread would have
  printed every edge it saw.
- The trim id and the chip-info page still answer, so the SPI path is fine.

So the controller is **not producing reports at all**.  The vendor driver's own
resume path is the clue: `nvt_ts_resume` calls `nvt_update_firmware` and
`nvt_check_fw_reset_state` before `nvt_irq_enable` - the app firmware is
(re)loaded at resume, and until that happens the part answers register reads but
does not scan.

## What touch28 does

Before going for the firmware download, close the last cheap gap: with no INT,
the event buffer was only read once a second, which can miss a touch that the
controller reports between reads.  touch28 polls it **every 10 ms for the first
minute** after the report thread starts (then back to once a second) and keeps
logging every buffer change and every gpio21 edge.

Run it on the nodaemon root, and slide a finger on the lock screen during that
first minute: a `evt data` line that changes with the finger, or a
`gpio21 asserted` line, tells us the report path is alive and only the decoding
is left.  If both stay silent, the next step is the firmware download
(`nvt_update_firmware`): the protocol is in the module we have, the image is
`novatek_nt36532_m81_fw_csot.bin`, and the transport is proven.

## Twenty-fourth boot (touch28): fast polling, still nothing

Sliding a finger from power-on until the home screen appeared - so the whole
fast-poll window was covered - produced only:

```
report thread up, INT on gpio21
evt data ff ff ff ff ff ff ff ff (no record)
```

No `gpio21 asserted`, no buffer change beyond the initial `ff`.  The controller
answers register reads but never produces a report.

Two differences from the vendor's own read path remain, and both are cheap:

1. The vendor's work function reads the event block with **len = 109** (one
   count byte plus 18 six-byte records); we read 65.  Some controllers keep the
   event FIFO output disabled unless the full block is clocked.
2. The vendor reads that block at **address 0** after a page set, so the page
   register has to be the one the block lives at.

## What touch29 does

- Reads the vendor's full **109-byte block** at `0x2fe00` on every poll (in
  addition to the 65-byte frame) and logs the first 12 bytes whenever the block
  contains anything other than `00`/`ff`.
- Keeps the 10 ms polling for the first minute, the INT edge logging and the
  buffer-change logging.

If the block stays empty, the controller is not scanning, and the remaining work
is the vendor's post-probe sequence: `nvt_boot_ready` (writes 1, 0, 0xA0 to
per-chip addresses taken from the trim table) followed by
`nvt_check_fw_reset_state`, and only if that fails the firmware download
(`nvt_update_firmware` -> `request_firmware` -> `nvt_write_fw` -> `nvt_boot_ready`).

## What stage 3 does (0155)

Patch `xnu/patches/0155-liuqin-touch-spi.patch` adds, behind
`liuqintouch>=3`, everything from the SE programming to `liuqin_hid_touch()`.
(It booted once — see the touch4 section above — and got as far as a configured
SPI master that the engine then refused to run commands for.)

1. **SPI master**, in `spi_geni_init()` / `setup_fifo_params()` order:
   protocol check (`GENI_FW_REVISION_RO` must say SPI), `geni_se_init()`
   (irq clear, `GENI_CGC_CTRL`, `SE_DMA_GENERAL_CFG`, `GENI_OUTPUT_CTRL` =
   0x7f, `GENI_FORCE_DEFAULT_REG` = 1, `SE_IRQ_EN`, RX watermark/RFR),
   FIFO mode (`SE_GENI_M_IRQ_EN` |= done/tx-wm/rx-wm/rx-last,
   `SE_GENI_DMA_MODE_EN` &= ~1), `SE_SPI_TRANS_CFG` &= ~`CS_TOGGLE`,
   8-bit words (`SE_SPI_WORD_LEN` = 4) with 4-to-1 FIFO packing, mode 0
   (`SE_SPI_CPHA`/`CPOL` = 0), CS0 (`SE_SPI_DEMUX_SEL` = 0), and
   `GENI_SER_M_CLK_CFG` = divider | `SER_CLK_EN` with `SE_GENI_CLK_SEL` = 0
   (BI_TCXO 19.2 MHz, no prescaler).  Divider 16 → 1.2 MHz.
2. **Transfers**: polled FIFO transfers.  `SE_SPI_*_TRANS_LEN`, `M_CMD0` =
   opcode (1 TX, 2 RX, 3 TX|RX) << 27 | `FRAGMENTATION`, TX fed through
   `SE_GENI_TX_FIFOn` respecting `SE_GENI_TX_FIFO_STATUS`, RX drained through
   `SE_GENI_RX_FIFOn` while the command runs, done/error from
   `SE_GENI_M_IRQ_STATUS`.  Chip select is driven by hand with the
   `SPI_CS_ASSERT` (8) / `SPI_CS_DEASSERT` (9) opcodes, one CS window per
   Novatek phase.
3. **Novatek**: page/address framing (`0xff`, `addr >> 15`, `addr >> 7`, then
   `addr & 0x7f` with one dummy byte), trim id from `0x3f004`, FWINFO from
   `0x2fe78` (16 bytes, `fw_ver + ~fw_ver == 0xff`, big-endian x/y max).
4. **Reports**: a kernel thread reads the 65-byte point frame at `0x2fe00`
   when gpio21 asserts (TLMM `GPIO_IN_OUT` bit 0, SM8450 `in_bit = 0`),
   decodes ten 6-byte records (`id << 3 | event`, event 1/2 = down/move,
   3 = release), scales to the 1800x2880 framebuffer and calls
   `liuqin_hid_touch(finger, down, x, y)`.  Fingers missing from a frame are
   released.  A frame with no valid record is ignored, so a zeroed or stale
   event buffer never lifts a held finger.  If a touch only shows up in the
   once-a-second health read, the thread logs it and switches to continuous
   polling by itself.

Boot args (all optional, stage 3):

| arg | meaning |
| --- | --- |
| `liuqintouch=N` | 0 off, 1 audit, 2 clocks/pins, 3 SPI + chip id + reports |
| `liuqintouchdiv=N` | SE clock divider, SCLK = 19.2 MHz / N (default 16 = 1.2 MHz) |
| `liuqintouchpoll=1` | read the event buffer every tick instead of waiting for gpio21 |
| `liuqintouchswap=1` | exchange the controller's x and y |
| `liuqintouchflip=N` | bit 0 mirrors x, bit 1 mirrors y |

## What the next boot should show (touch29)

Image: `shim/out/shim-boot-touch29.img`
(sha256 `586213922215dce9d208c4e99c90e2e4990a50b3266fb9219b4232c78ed8b778`,
kernel `3113713031e35c2d6faaa0c463c28da3a65dfc0b8f1ff2ff009dd10f3ccb125e`,
payload `b1975a04a90ad653ee479b6d338fe7423b7d6b3bb134acb26a18b3a1ac4566fb`).

Same procedure: boot on the nodaemon root, slide a finger through the first
minute.  A `LIUQIN TOUCH: block ...` line means the controller does answer with
the full block and only the frame length was wrong; no `block` line at all means
it is not scanning and the boot/firmware path is the remaining work.

## Build and test loop

```sh
# after editing the kernel tree at /tmp/liuqin-build3
WORK=/tmp/liuqin-build3 ./tools/build-local.sh          # incremental, ~1-3 min
# new patch: diff the file against a .preNNNN backup in /tmp and write it as
# xnu/patches/015N-liuqin-touch-*.patch (see 0152/0153/0154 for the format)
python3 tools/mkpayload.py /tmp/liuqin-build3/fakeroot/System/Library/Kernels/kernel.development.liuqin \
    shim/out/liuqin.adt /tmp/payload.bin --cmdline "$BOOT" --fwarea ~/liuqin-root/fwarea.bin
LLD=/tmp/ld.lld shim/build.sh /tmp/payload.bin
cp shim/out/shim-boot.img shim/out/shim-boot-touchN.img
shasum -a 256 shim/out/shim-boot-touchN.img
```

Boot line used for touch work (add `liuqintouch=N`):

```
debug=0xc serial=3 cpus=8 keepsyms=1 rd=ufsroot0 ufsrw=1 rootrw=1
launchd_unsecure_cache=1 cs_enforcement_disable=1 amfi_get_out_of_my_way=1
unentitled_ios_sim_launch=1 liuqinfb_noconsole=1 liuqinmute=1 wdt=-1
ignition_level=1 ufslogsecs=2 liuqintouch=3 liuqinadreno=5 liuqinsvc=1
sheng-gpu-bringup=1 sheng-gpu-bank=1 sheng-gpu-asid=1 sheng-gpu-sid=5
sheng-gpu-slot=0 sheng-gpu-cmddb-version=1 sheng-gpu-route=1 sheng-gpu-cp=1
sheng-gpu-hfi=1
```

## Traps that cost time this session

- `liuqinmute=1` means only the forced `LIUQIN BOOT:` lines are drawn on the
  screen; every `IOLog` line - including `LIUQIN TOUCH:` - only exists in the
  persistent log.  Pull the log (or use `ufslogsecs=2` so a panic loses at most
  two seconds).
- `ml_io_map` does not bound-check: reading a register outside the mapped window
  is a data abort.  The USB probe took the boot down four times by reading
  `GCC + 0x9c010` with a 0x90000 window (fixed in 0151 by mapping 1 MiB).
- A wrong GCC offset (sweet's instead of SM8450's) silently gates other blocks:
  the GPU ladder then dies with an external abort.
- The first boot after a userdata flash spends tens of minutes in LaunchServices
  seeding; the lock screen only appears after that.  Later boots are fast.
- The GPU work is done and independent: keep `sheng-gpu-*` as above so the boot
  path stays the one that reaches the lock screen.
- **SM8450's TLMM puts the GPIO input value in `GPIO_IN_OUT` bit 0 and the
  output in bit 1** (`pinctrl-sm8450.c` sets `.in_bit = 0, .out_bit = 1`).
  Most notes on the register quote the older SoCs, where it is the other way
  round.  Reading bit 1 would have made the INT line look permanently low.
- The SE clock is BI_TCXO 19.2 MHz only because stage 2 programs the RCG with
  source 0 and M/N/D = 0.  If that ever changes, the SPI clock changes with it.
- The Novatek event buffer can read back as zeros between reports; the driver
  only decodes frames that contain a valid finger record, and lifts fingers
  from the explicit event-3 release records.
- **The TLMM mux value is the index of the function in that pin's own function
  list, not a global function id.**  For `gpio16..19` on SM8450 that makes
  `qup4` = 1 (`PINGROUP(16, qup4, phase_flag, ...)`); writing 2 selects
  `phase_flag` and silently disconnects the SPI.  Stage 2 shipped mux 2 for one
  boot before the touch4 log made it obvious.
- `bias` encoding on this SoC: 0 = no pull, 1 = pull-down, **2 = keeper**,
  3 = pull-up.  Stage 2 called 2 "pull-up" and wrote keeper.
- **A completed `CS_ASSERT` proves the command interface, not the data path.**
  `GENI_IF_DISABLE_RO` (0x64) bit 0 says whether this SE has a CPU FIFO at all;
  on this tablet it does not, so every FIFO opcode fails while CS commands
  succeed.  `SE_GSI_EVENT_EN` (0xe18) left at `0xf` is the hint that the
  previous owner used GPI DMA.
