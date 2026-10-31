# Novatek firmware download: what the vendor module says

Source: `nt36532_touch.ko` (TWRP device tree, unstripped), decompiled with
angr's decompiler (`pip install angr`, no Java needed).  Addresses are the
ELF virtual addresses (the `.text`-relative ones from `objdump` plus 0x41000).

## `nvt_write_firmware(img, size)` - 0x4eca4

```
for (i = 0; i < section_count; i++) {
        e = section_table + i * 28;             /* 28-byte entries */
        off  = *(u32 *)(e + 12);                /* offset into the image   */
        addr = *(u32 *)(e + 16);                /* target address          */
        len  = *(u32 *)(e + 20);                /* bytes to write          */
        if (off + len > size) return -EINVAL;
        if (len == 0) continue;
        chunks = round_up(len + 1, 0xFC00) / 0xFC00;
        do {
                if (nvt_set_page(addr) != 0) return error;
                buf[0] = addr & 0x7f;
                memcpy(buf + 1, img + off, chunk);
                if (CTP_SPI_WRITE(buf, chunk + 1) != 0) return error;
                addr += 0xFC00;                 /* and off, len -= 0xFC00 */
        } while (--chunks);
}
```

So: **one chip-select transaction per chunk**, `[addr & 0x7f]` followed by up
to `0xFC00` payload bytes, after a page write to the chunk's address.

**The trap in that line** (it cost a whole session, 2026-10-25): `CTP_SPI_WRITE()`
applies `SPI_WRITE_MASK()` - `buf[0] |= 0x80` - before it sends anything
(`nvt_nt36xxx.h:444`, `nvt_nt36xxx.c:392`), so the byte on the wire is
`(addr & 0x7f) | 0x80`.  The decompiled pseudo-code and the official
`nvt_write_sram()` both show only `addr & 0x7f`, and the mask lives inside the
helper.  A payload sent without bit 7 is a **read** command: the transfer
completes, the SRAM keeps whatever it had, and the part never boots.

## `nvt_boot_ready()` - 0x41e04

```
nvt_write_addr(addr1, 0x01);
__const_udelay(21475000);
if (!active_panel) {
        nvt_write_addr(addr1, 0x00);
        nvt_write_addr(addr2, 0xA0);
}
```

`addr1` / `addr2` are `*(u32 *)(hw_info + 132)` and `*(u32 *)(hw_info + 136)`:
fields of the per-chip hardware-info struct, which is filled from the
trim table.  Mapping those two offsets to absolute addresses is the last
unknown of the download path.

## The firmware container - `novatek_nt36532_m81_fw_csot.bin` (245760 bytes)

Verified byte by byte (2026-10-25); the earlier guess in this section was wrong:

```
0x000  u32 end = 0x200                      header size / info records end
0x000  ILM: bin 0x200  sram 0x000000  len 0x1D1BB   crc 0xE3173A51 (at 0x18)
0x00C  DLM: bin 0x22000 sram 0x100000 len 0x14107   crc 0x667268CF (at 0x1C)
0x020  0x03        bit 1 = cascade_2nd_header_info
0x028  0x00        no overlay section
0x029  0x00        spi_dma_div_cnt_val = fw[0x29] & 1  -> no nvt_spi_dma_setup()
0x030  info records, 16 bytes each, 14 of them, {sram, size, bin, crc}
          [0..11] zero
          [12] pos 0xF0: sram 0x114108  size 0xFF  bin 0x000  crc 0x7ADCFBAF
          [13] read from end - 0x10 = 0x1F0:
                        sram 0x114208  size 0xFF  bin 0x100  crc 0xCD21B90F
0x100  a second copy of the 0x200-byte header (this is what [12]/[13] write)
```

`nvt_update_firmware` (0x4d22c) parses this into the 28-byte section table the
writer consumes; its decompilation trips an angr bug (division by zero when
normalised), so the parser still has to be read from the disassembly or with a
different decompiler.  The official C parser (`nvt_bin_header_parser`) does
describe it completely - see the write recipe below.

## Acceptance values (from TWRP's own log and `/proc/nvt_fw_version`)

```
fw_ver = 0x19 (25), fw_type = 0x01, PID = 0x5973, x_num = 38, y_num = 60
```

## The official source settles it (Google `novatek_touch`, android-15 branch)

Fetched from `android.googlesource.com/kernel/google-modules/touch/novatek_touch`
(branch `android-gs-akita-6.1-android15-qpr2`), kept in `refs/novatek/`:

- `nt36xxx_fw_update.c` - parser, writer, checksum and the update flow,
- `nt36xxx.c`, `nt36xxx.h` - `nvt_boot_ready`, `nvt_write_addr`, transfer length,
- `nt36xxx_mem_map.h` - per-chip address tables.

### Addresses for this family

This part (`20 00 00 32 65 03`) is **not** in the official trim table - no entry
has `0x32` at byte 3 - so the per-chip branches that are gated on a tid do not
define its behaviour.  The map that matches everything the part answers is
`NT36523_memory_map` (the "cascade" one, `EVENT_BUF_ADDR = 0x2FE00`; the
`...N` variant uses `0x2FD00` and is polled as well):

```
EVENT_BUF_ADDR   = 0x2FE00
BOOT_RDY_ADDR    = 0x3F10D
TX_AUTO_COPY_EN  = 0x3F7E8
SPI_DMA_TX_INFO  = 0x3F7F1
ILM/DLM des+len  = 0x3F128/0x3F12C, 0x3F118/0x3F130
G_ILM/G_DLM crc  = 0x3F100/0x3F104, R_ILM/R_DLM = 0x3F120/0x3F124
BLD_CRC_EN       = 0x3F30E,  BLD_ILM_DLM_CRC = 0x3F133
```

There is **no POR_CD_ADDR** field in this map.  `NVT_TRANSFER_LEN = 63 * 1024 =
0xFC00`, and `nvt_boot_ready()` is:

```c
nvt_write_addr(ts->mmap->BOOT_RDY_ADDR, 1);
mdelay(5);
if (!ts->hw_crc) {
        nvt_write_addr(ts->mmap->BOOT_RDY_ADDR, 0);
        nvt_write_addr(ts->mmap->POR_CD_ADDR, 0xA0);
}
```

`hw_crc` comes from the trim table (`NT36523_hw_info.hw_crc = 2`), so on this
family **only the set happens** - clearing BOOT_RDY after the set cancels the
boot request.  The two unknown registers of the decompiled vendor module map to
`hw_info + 132 = BOOT_RDY_ADDR` and `hw_info + 136 = POR_CD_ADDR`.

### The firmware container (parser logic, `nt36xxx_fw_update.c`)

```
end      = le32(fwdata[0..3])                     /* 0x200 for our blob            */
cascade  = (fwdata[0x20] >> 1) & 1                 /* 1 for our blob                */
info_sec_num = ((end/2 - 0x30) / 0x10) + 1         /* 14                            */
ovly_info    = (fwdata[0x28] >> 4) & 1             /* 0                             */
partition    = 2 + ovly_sec_num + info_sec_num     /* 16                            */

/* ILM & DLM (12 bytes each from offset 0) */
BIN_addr = le32(fwdata[0 + i*12]); SRAM_addr = le32(fwdata[4 + i*12]);
size     = le32(fwdata[8 + i*12]); crc       = le32(fwdata[0x18 + i*4]);

/* info sections: {SRAM_addr, size, BIN_addr, crc} at 0x30 + 0x10*(i-2), except
 * that once a record's BIN_addr < end (the header record) has been seen, a
 * cascade image reads the remaining records from end - 0x10 */
```

For `novatek_nt36532_m81_fw_csot.bin` that yields: ILM = file 0x200/0x1D1BB to
SRAM 0x0, DLM = file 0x22000/0x14107 to SRAM 0x100000, and **two** info sections
(SRAM 0x114108 from file 0x000 and SRAM 0x114208 from file 0x100, 0xFF bytes
each).  Reading the second one from `0x30 + 0x10*13` instead of `end - 0x10`
gives a decoy record (bin 0x200, size 0) and silently drops it.

### The write recipe

```
nvt_bootloader_reset()                     /* 0x69, 35 ms                    */
nvt_set_bld_hw_crc()                       /* ILM/DLM descriptor regs        */
(cascade) nvt_tx_auto_copy_mode(); (fw[0x29] & 1) nvt_spi_dma_setup();
nvt_write_firmware()                       /* nvt_write_sram per partition   */
        per 0xFC00 chunk: nvt_set_page(SRAM_addr);
                          fwbuf[0] = (SRAM_addr & 0x7F) | 0x80;  /* SPI_WRITE_MASK */
                          CTP_SPI_WRITE(fwbuf, len + 1);
nvt_check_spi_dma_tx_info()                /* SPI_DMA_TX_INFO == 0, 200x1 ms */
nvt_bld_crc_enable(); nvt_fw_crc_enable(); /* cascade: 7- and 3-byte writes  */
nvt_boot_ready()                           /* 0x3F10D = 1, 5 ms, and done    */
nvt_check_fw_reset_state(RESET_STATE_INIT) /* poll event 0x60 for 0xA0..0xAF */
```

Acceptance: `fw_ver = 0x19`, `fw_type = 0x01`, `PID = 0x5973`, `x_num = 38`,
`y_num = 60`.
