# ELRS 3.x manual-radio milestone

This records the original manual-radio milestone. The user has confirmed it
works; the subsequent [USB extension](USB-control.md) is now implemented.

## References and scope

Compared against official ExpressLRS **3.5.3**, commit
`40555e141efb0c93ea8d075ec47a27592355f924` (GPL-3.0).
Base fork commit: `bdadac975e765e577c9864831ada0ed3b04ad222`.
The local reference checkout is `.reference/ExpressLRS` (not shipped).

BETAFPV's [Matrix product page](https://betafpv.com/products/matrix-1s-brushless-flight-controller-hd)
identifies an onboard **serial ELRS receiver on UART3 using CRSF**, with
independent Wi-Fi/passthrough updates, target `BETAFPV 2.4GHz AIO RX`.
Its specification says 3.4.3; the [4IN1 firmware notes](https://support.betafpv.com/hc/en-us/articles/48215862783769-Firmware-for-Matrix-1S-Brushless-Flight-Controller-4IN1)
list 3.5.3 for the Betaflight 4.5.2 release. The actual unit's receiver version
must be read from its Web UI; Betaflight's version does not identify it.

This milestone is 8-byte LoRa OTA, Hybrid switches, fixed TX power, manual
controls, binding and RF link statistics. USB command injection and telemetry
forwarding remain deferred until hardware binding/control is demonstrated.
FLRC, DVDA, FullRes, Gemini, model match and EU CE LBT are outside this port.
The existing ISM_2400 target is retained.

## Original architecture

* `Src/adc.c`, `USER/Hardware/gimbal.c`: ADC/DMA samples, stored calibration,
  stick orientation, normalized physical values, FreeRTOS gimbal queue.
* `USER/Hardware/switches.c`: GPIO switches and switch queue.
* `USER/Drivers/mixes.c`: source mapping, reversal, weight/offset, limits and
  deadband; eight values in microsecond-like units go to `mixesValQueue`.
* `USER/Drivers/radiolink.c`: consumes that queue and calls `SX1280_Process`.
* `USER/Drivers/tx_main.c`: channel snapshot, configuration, UID, binding,
  sync/MSP/RC packet selection, nonce, CRC, telemetry and hopping decisions.
* `Src/stm32f1xx_it.c`: TIM1 sends RF slots; EXTI10 dispatches SX1280 DIO1.
* `USER/Hardware/sx1280.c` and `sx1280hal.c`: modem settings, RF amplifier,
  SPI2 FIFO/commands. STM32 controls the radio directly.
* `USER/protocol/fhss.c`: 80 frequencies, 240-entry deterministic sequence.
* `Src/usbd_desc.c`, `Src/usbd_custom_hid_if.c`, middleware Custom HID and
  `USER/Drivers/joystick.c`: existing USB joystick/configuration reports.
  Power/status logic selects joystick or radio operation; concurrent USB/RF
  operation is not implemented by this milestone.

Data flow: ADC/GPIO -> queues -> mixer -> radio task -> channel snapshot ->
TIM1 packet serializer -> SX1280 FIFO -> RF. Reverse path: SX1280 DIO1 ->
FIFO -> OTA CRC validation -> link statistics. Originally RX handling was
disabled and the DIO1 handler unconditionally called the TX-complete path.

## Verified protocol differences / implementation checklist

Source paths below are relative to [upstream 3.5.3](https://github.com/ExpressLRS/ExpressLRS/tree/3.5.3/src).

| Area | Required behavior | Upstream source |
|---|---|---|
| OTA version/CRC | Normal seed `((UID[4]<<8)|UID[5]) ^ 3`; binding seed zero; CRC14 polynomial 0x2E57 and split placement unchanged | `include/common.h`, `lib/OTA/OTA.cpp` |
| FHSS seed | UID[2..5] big endian, last byte XOR 3 | `src/common.cpp` |
| Sync frequency | Index 41, not 40; same 80 channels and 240 hops | `lib/FHSS/FHSS.cpp` |
| Frequency arithmetic | Interpolate integer register endpoints using spread scale 256; old independently rounded frequency table can differ by one register step | `lib/FHSS/FHSS.h` |
| Shuffle | Same LCG and within-block swaps, excluding sync entry | `lib/FHSS/FHSS.cpp`, `lib/FHSS/random.cpp` |
| Sync byte 3 | rate index bits 7:4, telemetry bits 3:1, switch mode bit 0; Hybrid=1 | `lib/OTA/OTA.h`, `src/tx_main.cpp` |
| Rate IDs | Local settings 0/1/2/3 must advertise upstream indices 4/6/7/9 (500/250/150/50 Hz) | `src/common.cpp` |
| RF settings | Correct old LI 4/7 name to LI 4/8 (both encode 0x07, so 250/150 modulation is unchanged); 50 changes SF9 LI 4/6 to SF8 LI 4/8; 500 stays SF5 LI 4/6. BW 800 kHz, 8 bytes; preambles 12/14/12/12; hops 4/4/4/2 | `src/common.cpp`, `lib/SX1280Driver/SX1280_Regs.h` |
| CH1-4 | Clamp CRSF 172..1811, round to 0..1023; concatenate four 10-bit little-endian values in bytes 1..5 | `lib/OTA/OTA.cpp`, `lib/CrsfProtocol/crsf_protocol.h` |
| AUX | AUX1 in byte 6 bit 7; telemetry ACK bit 6; AUX2..8 round robin in remaining six bits; six-position plus center, AUX8 16-position | same |
| TLM ratio | Wire values 0..7 remain no-TLM, 1:128 .. 1:2; upstream's enum offset is removed on serialization | `src/tx_main.cpp` |
| RF timing | After TX completion hop for next nonce, open RX before telemetry slot; skip exactly that slot while advancing nonce | `src/tx_main.cpp` |
| Link stats | Bytes 2/3: RSSI in low seven bits, antenna/model-match in high bits; byte 4 LQ plus MSP ACK; byte 5 signed SNR in quarter-dB | `lib/OTA/OTA.h`, `src/tx_main.cpp` |
| Binding | 50 Hz V3 modulation, sync frequency, inverted IQ, zero CRC, frozen nonce, MSP bind opcode plus UID[2..5]; finish final packet before restoring rate/UID | `src/tx_main.cpp`, `src/rx_main.cpp` |
| Radio IRQ | Dispatch RX_DONE versus TX_DONE; reject radio errors; prevent duplicate TX completion from advancing FHSS | `lib/SX1280Driver/SX1280.cpp`, `src/tx_main.cpp` |

The low-level SPI command reader also required correction: it used transmit-only
SPI and returned its own outgoing bytes instead of the SX1280 response. See the
[follow-up audit](ELRS3-audit.md). Board pin assignments are unchanged.
Hardware RF validation remains mandatory; a successful build is not proof of
binding or receiver-channel correctness.
