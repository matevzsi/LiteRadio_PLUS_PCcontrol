# Validation record (2026-10-01)

Target: LiteRadio_Plus_SX1280. Compiler: ARMCC 5.06 update 6 (750).
Device pack: Keil.STM32F1xx_DFP 2.4.1.

* Original source full rebuild: 0 errors, 4 warnings.
* Modified source full rebuild: **0 errors, 2 warnings**.
* Remaining warnings predate this work: unused `Tim1IsOpen` in `rgb.c`,
  unused `switchDelay` in `sx1280.c`.
* Linker: code 47,856 B; RO data 1,644 B; RW data 1,460 B; ZI data 18,108 B.
  RW+ZI = 19,568 B of 20,480 B (912 B unallocated by the linker).
  Runtime task stack/heap headroom still needs hardware measurement.
* Application region: 0x08008000..0x0801FFFF, 96 KiB.
* BIN length: 50036 B; initial SP 0x20004C70; reset vector 0x08008199.
* Intel HEX checksums, address bounds and every data byte checked against BIN.
* Firmware SHA-256: `9f64b8923979690d6795ababd0901c977114ccfccfcd9f036bafdff4b1607617`.
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
* Actual composite HID class tested with mocked USB hardware: interface routing,
  configuration/report descriptors, interrupt OUT and EP0 SET_REPORT, malformed
  and short transfers, endpoint busy state and two-fragment 64-byte telemetry.
* PC controls: manual throttle full-travel mapping (-100..100% to 0..100%),
  unchanged PC lift outputs, nominal output limits, timeout boundary, sequence and
  tick wrap, malformed commands, replay rejection, physical override, STOP,
  disconnect/reconnect and required switch cycle.
* Original 108-byte joystick/configurator report descriptor unchanged byte for
  byte. Its SHA-256 is
  `5cc9a061ba6bda5e2b7478fee4a4e9dc57d3d3cd8f1dcf66cbc4178fbeed07e2`.
  The supplied bootloader HEX is unchanged.
* Python host tests: HID packet layout, named lift/thrust/steer percentages,
  inverse thrust mapping across -100..100%, range/NaN rejection, client STOP,
  CRSF fragments and battery decoding.
* Keyboard tests execute the actual event handlers with fake Tk/time/client:
  targets, slew rates, opposing A/D, Space auto-repeat, focus loss and close.
* Python scripts compile; CLI help exposes lift/thrust/steer. No hardware
  dependency is needed to run the host tests.
* `git diff --check` passes.

Current firmware: `artifacts/LiteRadio_2_SE_V2_ELRS3_USB.bin` (matching HEX,
AXF, map and SHA256SUMS.txt alongside it). Flash only the application at
0x08008000; the bootloader region is outside the emitted HEX addresses.

Manual ELRS binding and control work after the SPI fix. This USB/keyboard build
has not been flashed by this session. Physical enumeration, bootloader COM
access, concurrent USB/RF timing, runtime stack/heap headroom, PC watchdog at
the receiver and telemetry readings remain hardware checks in
[USB-control.md](USB-control.md). CH6 retains seven-position Hybrid resolution.

Run the added Python tests with:

```powershell
python tests/test_pc_hid.py
python tests/test_keyboard.py
```

Arm display extension: USB status byte 36 tested for arm/disarm/unavailable;
Python decoding handles older firmware as unknown. GUI tests cover live/stale
arm indication. Four real Tk bars fit the window without label overlap.
