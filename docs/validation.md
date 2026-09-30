# Validation record (2026-09-30)

Target: LiteRadio_Plus_SX1280. Compiler: ARMCC 5.06 update 6 (750).
Device pack: Keil.STM32F1xx_DFP 2.4.1.

* Original source full rebuild: 0 errors, 4 warnings.
* Modified source full rebuild: **0 errors, 2 warnings**.
* Remaining warnings predate this work: unused `Tim1IsOpen` in `rgb.c`,
  unused `switchDelay` in `sx1280.c`.
* Linker: code 45,600 B; RO data 1,628 B; RW data 1,244 B; ZI data 17,700 B.
  RW+ZI = 18,944 B of 20,480 B (1,536 B unallocated by the linker).
  Runtime task stack/heap headroom still needs hardware measurement.
* Application region: 0x08008000..0x0801FFFF, 96 KiB.
* BIN length: 47648 B; initial SP 0x20004A00; reset vector 0x08008199.
* Intel HEX checksums, address bounds and every data byte checked against BIN.
* Firmware SHA-256: `a940ca1cf0b0fbaff4de11355d7d4354340fe50b28c5152a1e59babb3c2ac552`.
* Build log, AXF, map, BIN, HEX and checksum list are in `artifacts/`.

Native tests compiled and ran with MSVC 14.44 and Python 3.11:

* 140,000 channel packets compared byte-for-byte with functions extracted from
  pinned official ExpressLRS 3.5.3, including every Hybrid switch index and ACK.
  All packets also decoded by the official RX function: primary channels within
  one CRSF unit, with exact arm and ACK recovery.
* 1,000 UIDs: normal CRC seed, FHSS seed, all 240 sequence entries, register
  frequencies and two full sequence wraps checked against upstream.
* Every local rate and telemetry wire code checked for sync serialization.
* Actual scheduler/CRC function bodies exercised through 1,024 slots for each
  of four rates and eight telemetry ratios, including nonce rollover.
* Binding packet count/frozen nonce/zero CRC and final-TX wait verified in mocks.
* Real telemetry parser checked for RSSI/antenna/LQ bitfields, signed SNR,
  wrong CRC, duplicate-slot reception and link updates.
* Raw CRSF reassembly checked for CRC rejection, out-of-order fragments,
  duplicate fragments, restart/resync, maximum-length frames and overflow.
* Actual radio IRQ function bodies tested with RX/TX, invalid RX, stale IRQs,
  continuous RX mode bookkeeping, duplicate TX completion and TX recovery.
* Actual SPI command reader tested for IRQ/FIFO/statistics/status responses and
  failed transfers. This regression failed before the HAL fix and passes after
  it. Earlier status mocks missed the defect; see [audit](ELRS3-audit.md).
* `git diff --check` passes for firmware/docs/project edits. Existing generated
  RTE headers from the user's Keil setup contain unrelated trailing whitespace.

The user's Keil setup edits were retained. The SX1280 project adds only the
new `elrs_v3.c` source and corrects the application flash limit.

The user reported partial binding and unstable, incorrect channel readings with
the previous build. The corrected SPI build above has not been hardware tested.
No transmitter or receiver was flashed by the agent.
Matrix binding, on-device timing, several-minute link stability, physical
channel behavior and power-cycle recovery remain open in `bench-test.md`.
USB autonomous control remains gated on that hardware milestone.
