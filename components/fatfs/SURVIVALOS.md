# fatfs (project override)

Verbatim copy of ESP-IDF v5.5.5 `components/fatfs` (tests left out), overriding the
IDF component of the same name so that two FatFs options can be changed:

- `FF_FS_EXFAT 1` — SDXC cards (64 GB and up) are sold formatted as exFAT, which
  stock ESP-IDF cannot mount at all.
- `FF_LBA64 1` — needed by FatFs to read GPT partition tables, which is what
  macOS Disk Utility writes unless told otherwise.

Follow-on changes: the SD, raw-flash and wear-levelling disk drivers return the
sector count as `LBA_t` (it is 64-bit now), and `ff.c` zeroes the variable first
for drivers outside this component that still write 32 bits.

Files larger than 2 GiB still cannot be read through the C library, because
`off_t`/`long` are 32-bit on this target; exFAT only removes the need to reformat.

FatFs is (C) ChaN under its BSD-style licence (`src/00readme.txt`); the ESP-IDF
glue is Apache-2.0. Some exFAT implementations are subject to Microsoft patents.
