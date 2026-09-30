# Follow-up audit: unstable link after binding

## Confirmed error and correction

The port made IRQ dispatch depend on `SX1280_GetIrqStatus()`, but I missed a
defect in the inherited `SX1280_HalReadCommand()`: it called
`HAL_SPI_Transmit()` and copied the outgoing buffer back to the caller.
It never collected the radio response. In the IRQ path that meant interpreting
uninitialized stack bytes as interrupt flags. The original firmware ignored
those flags and unconditionally ran the TX-complete callback; this port made
the existing defect affect normal operation.

Official [SX1280Hal::ReadCommand](https://github.com/ExpressLRS/ExpressLRS/blob/3.5.3/src/lib/SX1280Driver/SX1280_hal.cpp)
reads SPI responses and copies command payload from offset two. The STM32
implementation now uses `HAL_SPI_TransmitReceive()`, zero dummy bytes, the same
response offset, and zeroed output if the transfer fails. This also repairs
reads of the RX FIFO offset and packet statistics.

Missed or false TX-complete handling can disrupt the hopping schedule. Binding
uses one fixed frequency, so accepting a bind packet does not establish that
normal hopping works. This is a plausible explanation for the reported RX LED
and intermittent data, but hardware testing must establish whether it explains
all observed symptoms.

The initial IRQ tests mocked status reads and therefore missed the HAL defect.
The new regression compiles the actual read-command function with only the SPI
peripheral mocked. It failed on the old implementation and passes after the
fix. It checks IRQ status, RX FIFO offset, packet statistics, the special status
command, failed transfers, and chip-select release.

## Comparison against official ExpressLRS

Reference: tag **3.5.3**, commit
`40555e141efb0c93ea8d075ec47a27592355f924`. The full comparison table is in
[ELRS3-port.md](ELRS3-port.md).

* Channel serialization: 140,000 packets match the official
  [Hybrid encoder](https://github.com/ExpressLRS/ExpressLRS/blob/3.5.3/src/lib/OTA/OTA.cpp).
  The same packets now also pass through the actual official RX decoder.
  All four primary controls round-trip within one CRSF unit; arm and telemetry
  ACK decode correctly. This rules out a primary-channel bit-packing mismatch
  for the tested inputs. It does not validate physical ADC/calibration data.
* UID/CRC seeds and hopping: 1,000 UIDs match official seed functions and all
  240 hopping entries, including frequency arithmetic and sequence wrap.
* Sync fields, rate IDs, LoRa settings, bind payload and telemetry layout were
  compared with `OTA.h`, `common.cpp`, `tx_main.cpp` and `rx_main.cpp` in that
  pinned source. No additional mismatch was identified in the supported modes.
* Scheduler tests cover four rates and eight telemetry ratios, 1,024 slots
  each, including nonce wrap, bind behavior, telemetry windows and radio IRQs.
  These are host simulations, not measurements of STM32 interrupt timing.

The [switch documentation](https://www.expresslrs.org/software/switch-config/)
also describes the selected Hybrid channel layout. Version-specific details
are taken from the pinned 3.5.3 code because the live documentation also covers
newer releases.

The corrected build still needs an on-device link/control test. No PC-control
features were added, and no hardware was flashed by this audit.
