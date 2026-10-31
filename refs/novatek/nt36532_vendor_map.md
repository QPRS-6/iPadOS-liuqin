# NT36532: the vendor's own address map

The Google `novatek_touch` sources in this directory do **not** contain NT36532
(no trim entry has `0x32` at byte 3), and every address this driver used until
touch41 came from the `NT36523` family map - which is why six descriptor
registers read back `0xffffff` and nothing the download wrote had any effect.

This part is **project-specific**: the panel, the firmware and the register map
all belong to this one machine, so a map taken from any other device (or from
another Novatek family, which is exactly the touch39-41 mistake) is wrong even
when the chip name matches.  The map below is from the **liuqin's own** vendor
module - the one the tablet actually runs under Android, repackaged into the
liuqin TWRP device tree.  It is that module, not a reference driver:

```
name=nt36532_touch  vermagic=5.10.160-gki-g13e2f28f61e9  description=Novatek Touchscreen Driver
strings: novatek_nt36532_m81_fw_csot.bin, _fw_tm.bin, _mp_csot.bin, _mp_tm.bin,
         "Xiaomi stylus Generation one ...", nvt_xiaomi_lockdown_info_show
```

The firmware names alone tie it to this panel (`novatek_nt36532_m81_fw_csot.bin`
is the blob this port ships in the firmware area), and `trim_id_table[0]` is the
`ff ff ff 32 65 03` entry that matches the id the tablet answers.

Location:

```
https://raw.githubusercontent.com/ymdzq/device_xiaomi_liuqin-TWRP/master/\
prebuilt/liuqin/vendor/lib/modules/1.1/nt36532_touch.ko
sha256 403d1fcc0961d234ddf71cb5cc12727a701a48ef6b22794899064e0553234870
591144 bytes, ELF64 aarch64 relocatable, not stripped
```

## Symbols

```
NT36532_cascade_memory_map   .rodata 0x74f0  size 0xc8 (200 bytes = 50 x u32)
NT36532_single_memory_map    .rodata 0x7428  size 0xc8
NT36532_hw_info              .data   0x280   size 0x10   [0] = hw_crc = 2
trim_id_table                .rodata 0x6e88  size 0x5a0  (24 entries)
```

`trim_id_table[0]` is `{ff ff ff 32 65 03}` with mask `{0,0,0,1,1,1}` - the
first entry matches this panel (`20 00 00 32 65 03`, only bytes 3..5 checked)
and is the NT36532 fallback.

The map values below were cross-checked against the offsets the module's own
code reads from that struct (llvm-objdump on the functions named in brackets),
so the field names do not depend on guessing the struct order:

| field | address | how it was confirmed |
| --- | --- | --- |
| `EVENT_BUF_ADDR` | `0x125800` | map[0]; `nvt_fw_crc_enable` sets the page from `map+0`, `nvt_get_fw_info` reads `map+0 \| 0x78` |
| reset state | `0x125860` | event buffer + `EVENT_MAP_RESET_COMPLETE` (0x60) |
| host command | `0x1258D0` | event buffer + `EVENT_MAP_HOST_CMD` (0xD0, **not** 0x50) |
| `BOOT_RDY_ADDR` | `0x1FB50D` | `nvt_boot_ready` reads `map+132` |
| `POR_CD_ADDR` | 0 (absent) | `map+136` is zero; `nvt_boot_ready` only uses it when `hw_crc == 0` |
| `TX_AUTO_COPY_EN` | `0x1FC925` | `nvt_tx_auto_copy_mode` reads `map+140` |
| `SPI_DMA_TX_INFO` | `0x1FC914` | `nvt_check_spi_dma_tx_info` reads `map+144` |
| `ILM_LENGTH_ADDR` | `0x1FB518` | descriptor writer, arg 3 = `map+152` |
| `DLM_LENGTH_ADDR` | `0x1FB530` | descriptor writer, arg 3 = `map+156` |
| `ILM_DES_ADDR` | `0x1FB528` | descriptor writer, arg 1 = `map+164` |
| `DLM_DES_ADDR` | `0x1FB52C` | descriptor writer, arg 1 = `map+168` |
| `G_ILM_CHECKSUM` | `0x1FB500` | descriptor writer, arg 5 = `map+172` |
| `G_DLM_CHECKSUM` | `0x1FB504` | descriptor writer, arg 5 = `map+176` |
| `R_ILM_CHECKSUM` | `0x1FB520` | crc reader, `map+180` |
| `R_DLM_CHECKSUM` | `0x1FB524` | crc reader, `map+184` |
| `BLD_ILM_DLM_CRC` | `0x1FB533` | crc reader, `map+192`, 2-byte read |
| `BLD_DES_ADDR` | `0x1FB514` | map+160 |
| `BLD_LENGTH_ADDR` | `0x1FB538` | map+148 |
| `DMA_CRC_EN` | `0x1FB536` | map+188 |
| `DMA_CRC_FLAG` | `0x1FB534` | map+196 |

These are the NT36523N addresses shifted by `+0x1C4400` (e.g. `0x3F10D` ->
`0x1FB50D`, `0x3F128` -> `0x1FB528`), which is why the two families look alike
but never overlap.

## Behaviour that differs from the Google cascade flow

- **Software reset**: `nvt_bootloader_reset()` writes `0x69` to the part's
  swrst address.  The vendor dmesg says `swrst_sif_addr=0x1fb43e`; the
  `0x00000` of the NT36523 flow is not it.
- **TX auto copy**: `hw_crc == 2` parts write **`0x56`** to
  `TX_AUTO_COPY_EN` (`0x69` is the `hw_crc == 1` value).  The write happens
  *after* the ILM/DLM descriptors.
- **No BLD CRC enable**: the module's `nvt_update_firmware` only touches the
  descriptor/length/checksum/flag registers; there is no write of a
  `BLD_CRC_EN` bit (the NT36523 family's `0x3F30E` does not exist here).
- **`nvt_fw_crc_enable()`**: 7-byte clear of the reset-status record at
  `event+0x60`, then a 3-byte write of `{event_host_cmd, 0xAE, 0x00}`.
- **`nvt_boot_ready()`**: `BOOT_RDY = 1`, 5 ms, and nothing else (the part is
  `hw_crc = 2`, so the clear and `POR_CD = 0xA0` branch is not taken).
- **`nvt_check_spi_dma_tx_info()`**: 200 tries, 1 ms apart, done at `0x00`.
  On NT36532 the register exists (`0x1FC914`), unlike the `0x3F7F1` this
  driver polled before.
