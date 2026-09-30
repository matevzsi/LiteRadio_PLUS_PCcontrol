# LiteRadio PLUS: ELRS 3.x manual-radio port

Firmware for **BETAFPV LiteRadio 2 SE V2 ELRS 2.4 GHz**, based on this
LiteRadio_PLUS fork and the OTA protocol in **ExpressLRS 3.5.3**.

**Status:** software build and native protocol tests pass. Binding, physical
channel operation, RF timing and reconnection on Matrix hardware are **not yet
verified**. This is a bench-test firmware, not a completed PC-control release.

The reported unstable link after binding exposed a missed SPI command-read
defect, now corrected. See the [audit findings](docs/ELRS3-audit.md).

## Implemented

* V3 CRC/UID seed, FHSS, sync layout and Hybrid channel serialization.
* Manual sticks/switches, existing calibration and mixer.
* LoRa 50/150/250/500 Hz; V3 binding at 50 Hz.
* Telemetry receive slots, RX interrupt dispatch, link statistics and bounded
  raw CRSF frame reconstruction/acknowledgement.
* Coherent channel snapshots and rate/bind transitions; no USB work in RF ISRs.

See [architecture and exact protocol differences](docs/ELRS3-port.md) and
[hardware acceptance checklist](docs/bench-test.md). Build sizes, test coverage
and the firmware checksum are recorded in [validation results](docs/validation.md).

## Build

Use Keil uVision with ARM Compiler **5.06 update 6 (750)** and the prepared
STM32F1 device pack (2.4.1 here). Open `MDK-ARM/LiteRadio_Plus.uvprojx`, select
**LiteRadio_Plus_SX1280**, then Rebuild. The default project target is a
different radio; select SX1280 explicitly.

Or run from PowerShell:

```powershell
.\tools\build.ps1
# Optional: .\tools\build.ps1 -Keil 'D:\Keil_v5\UV4\UV4.exe'
# If local script execution is disabled, allow this invocation only:
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1
```

The script performs a full rebuild, rejects failures/stale outputs, and copies
BIN, HEX, debug AXF, map, build log and SHA-256 checksums to `artifacts/`.
Original Keil outputs remain under `MDK-ARM/`.

### Protocol tests

```text
git clone --depth 1 --branch 3.5.3 https://github.com/ExpressLRS/ExpressLRS.git .reference/ExpressLRS
python tests/run_native.py
```

Run from a Visual Studio developer command prompt with Python 3 available,
or use GCC/G++ on PATH. Tests require the exact pinned upstream commit and
compile its encoder/decoder/PRNG/shuffle as an independent reference. They test the
actual firmware packet module, FHSS source, CRC and scheduler function bodies
and SPI command reader with hardware mocked. They do not measure STM32 execution
time or RF signals.

## Flash and recover

The application is linked at **0x08008000**. Existing bootloader, hardware
identity and calibration/configuration data below that address must be kept.

1. First confirm stock BETAFPV ELRS3 firmware binds and controls your Matrix.
   Save the radio's settings/calibration and keep its official recovery BIN.
2. Use [BETAFPV Configurator](https://github.com/BETAFPV/BETAFPV_Configurator/releases)
   with the **LiteRadio 2 SE V2 / SX1280** model. Follow the vendor's
   [V2-to-V3 flashing procedure](https://support.betafpv.com/hc/en-us/articles/22404447195673-How-to-Update-ELRS-V2-to-ELRS-V3),
   load the local `artifacts/LiteRadio_2_SE_V2_ELRS3_manual.bin`, then flash.
   This is an STM32 application BIN; do not load it into ExpressLRS Configurator
   as an ESP receiver/module image. Custom firmware acceptance by the vendor
   configurator still requires a hardware test.
3. If using SWD, program the application HEX at its encoded addresses, or the
   BIN at **0x08008000**, using sector erase rather than full-chip erase.
4. If the application fails to start, enter the existing bootloader by holding
   SETUP while powering on, then restore the correct official V2 SX1280 BIN.
   See the vendor's [bootloader connection instructions](https://github.com/BETAFPV/BETAFPV_Configurator/blob/main/docs/UnableToFindSerialPort_EN.md).
   The [radio firmware page](https://support.betafpv.com/hc/en-us/articles/4414348908057-LiteRadio-2-SE)
   distinguishes hardware revisions and firmware files.

No hardware was flashed during development.

## Rates, channels and binding

| Stored/configurator rate index | RF rate | V3 OTA rate index |
|---|---|---|
| 0 | LoRa 500 Hz | 4 |
| 1 | LoRa 250 Hz | 6 |
| 2 | LoRa 150 Hz | 7 |
| 3 | LoRa 50 Hz | 9 |

Fresh/invalid settings default to **150 Hz**, telemetry **1:8**. Valid saved
settings are preserved, including telemetry OFF, so select 150 Hz / 1:8 in
the existing configurator before testing. At 150 Hz this reserves about 18.75
telemetry slots/s; CRSF frames take several slots, and link-stat packets also
use this bandwidth. RC slots are reduced accordingly. Change RF settings only
while disarmed: saving STM32 flash briefly interrupts the link.

Internal mixer values remain **988..2012**, nominal center 1500, converted to
CRSF 172..1811 before V3 packing. OTA CH1..4 are **aileron, elevator, throttle,
rudder** (AETR). AUX1/CH5 is a low-latency two-position arm/enable switch.
Physical AUX2..4 use Hybrid switch bins; AUX5..8 are centered. Four primary
controls have 10-bit OTA resolution. Configure Betaflight accordingly.

Use an ELRS **3.x** receiver with matching regulatory domain and model match
disabled. Matrix's receiver is serial CRSF on UART3; changing Betaflight
firmware is not an ELRS receiver update. Check the actual receiver firmware
in its Web UI (BETAFPV documentation lists both 3.4.3 and 3.5.3).

For button binding, put the receiver into bind mode (the vendor documents
three power cycles and a double-blinking RX LED), turn the radio on in normal
RF mode, then briefly press/release BIND. The radio sends seven bind packets
at V3 50 Hz and returns to the configured rate after TX completion. Existing
UID/binding-phrase storage is preserved. A receiver with an existing binding
phrase may need its phrase cleared or matched before button binding works.

## Telemetry and failsafe

`linkStatistics` holds uplink RSSI magnitudes, LQ, SNR in dB, antenna, RF mode,
TX power and local downlink statistics. Downlink LQ uses up to 32 received-slot
samples. After three seconds without telemetry the local connection status
becomes disconnected; this is a diagnostic, not the receiver's RF failsafe.

`elrsTelemetry.frame` and `.length` hold the most recent complete CRSF frame
(up to 64 bytes, including address, length, type and CRC). `.frames` and
`.rejected` count accepted/rejected transfers. The standard CRC8-D5 is checked;
oversized/invalid frames are discarded, never published as truncated data.
These are debugger-visible values for this milestone; frames are not queued
or forwarded over USB yet.

RF loss still relies on ELRS receiver/Betaflight failsafe configuration. Verify
that loss disarms and produces safe motor outputs. There is no PC-command
watchdog yet because this firmware accepts no autonomous channel commands.

## USB / next milestone

The existing joystick/configuration HID interface is retained. New report IDs
0x10/0x20/0x21, PC channel commands, MANUAL/PC/FAILSAFE arbitration, PC watchdog,
hovercraft mixing and the Python HID test utility are **not implemented**.
Those follow successful manual Matrix binding/control, as required by the
project milestone order. The existing status logic still controls when USB
joystick and RF tasks run; simultaneous bidirectional USB/RF is not claimed.
