# USB control and telemetry

Manual ELRS operation works after the SPI fix. This build adds
PC control without changing the working OTA packet format or hopping code.

## USB compatibility

* VID/PID remain **0483:5750**.
* Interface **0** remains the original joystick/configurator HID, with the
  original unnumbered 16-byte input and 64-byte output report descriptor.
  The joystick continues reporting physical controls while RF and PC control run.
* Interface **1** is a new vendor HID (usage page **FF00**, usage **1**), with
  separate 64-byte interrupt IN/OUT endpoints **82/02**. Reports are unnumbered;
  message type is a payload byte, not a HID report ID. Using another interface
  avoids changing the existing joystick report format.
* The application source contains no CDC virtual COM implementation. The
  supplied bootloader HEX and the flash region below **08008000** are untouched,
  preserving the existing bootloader/virtual COM path. This does not add a
  simultaneous application COM port.
* Legacy configuration writes remain available with the transmitter powered
  off in USB joystick/configurator mode. Writes are deferred out of USB IRQs
  and rejected while RF is powered; read requests remain available.
* Endpoint 1 maximum packet size is corrected from 2 to 64 bytes so its existing
  reports fit. STM32 PMA buffers occupy 0018..0197, without overlaps.

Implementation follows the [USB HID specification](https://www.usb.org/sites/default/files/hid1_11.pdf)
and [HIDAPI's unnumbered-report convention](https://github.com/libusb/hidapi/blob/master/hidapi/hidapi.h).
HIDAPI writes include a leading **00** report-number byte: **65 host bytes**,
of which **64** are USB payload. Reads return 64 payload bytes.

## Mapping and source selection

Inputs are calibrated stick values after Mode 1/2 selection. The hovercraft
profile is isolated in `pc_control.c`, before RF serialization. It applies in
manual and PC modes without overwriting saved calibration or mixer settings.
The legacy joystick still uses its original saved mixer configuration.

| RF output | Manual input | PC input | Manual weight | Manual offset |
|---|---|---|---|---|
| CH3 forward thrust | I1 / Ele | thrust, command slot CH3 | 200% | -100% |
| CH4 differential thrust | I0 / Ail | steer, command slot CH4 | 100% | 0% |
| CH5 | SB | Always SB | 100% | 0% |
| CH6 lift | I2 / Thr | lift, command slot CH6 | 50% | +50% |

The Python API uses named **lift, thrust and steer**, each describing the
requested FC output in **-100..100%**. -100% thrust/lift means off, not reverse.
Steer is centered at 0%.

The manual throttle stick maps its full **-100..100%** travel to **0..100% lift**:
bottom = 0% (1500), center = 50% (1750), top = 100% (2000).
This 50% weight/+50% offset applies only to the physical throttle source.
PC lift commands already specify final FC percentages and bypass this manual mix;
the PC watchdog still sets lift to -100% (1000).

The manual elevator is self-centered: its useful forward travel is **0..100%**
(1500..2000), mapped to FC thrust **-100..100%** (1000..2000). The mix is
`1500 + (input-1500)*weight/100 + offset*5`, clamped to **1000..2000**.
Ele = 1500 produces 1000, 1750 produces 1500, and 2000 produces 2000.
Negative elevator travel also produces 1000; there is no reverse thrust.

The HID payload contains pre-mix inputs. Python converts a desired FC thrust
percentage `t` to `1500 + 2.5*(t+100)` before sending it. This undoes the
200%/-100% mixer so scaling is applied once. Steer and lift use `1500 + 5*p`.
All final profile outputs clamp to 1000..2000, including calibrated stick overtravel.

CH1/2/7/8 retain the saved physical mixer outputs unless selected in the PC
command mask. CH5 cannot be selected by the PC. The default mask is **2C**
(CH3, CH4 and CH6).

**SC high** (value >1750) permits PC control; SC middle/low immediately selects
manual controls on the next radio task iteration. Change `PC_ENABLE_INPUT` in
`pc_control.h` to select another switch. RF still requires the power button;
USB alone does not start RF. The original startup throttle check remains.

The working ELRS **Hybrid** mode is preserved: CH1..4 have 10-bit resolution;
CH5 is two-position (SB middle behaves as low); **CH6 has seven switch positions**.
Continuous high-resolution lift on CH6 would require a separate OTA-mode change.

### Remote LED

During normal RF operation, active PC control alternates the remote LED between
blue and red every **100 ms** (five full cycles per second). SC high without
valid commands does not trigger this indication. Leaving PC mode or entering
PC failsafe returns to the normal LED behavior. Startup throttle warnings,
binding, calibration, charging and other existing LED indications retain their
own behavior. The animation uses the system tick and adds no blocking delay.

## Watchdog

Default **100 ms**, configurable per command from **50 to 250 ms**. Send at 50 Hz.
Only valid, forward-moving 16-bit sequence numbers refresh it. Duplicate,
backward, malformed and out-of-range packets are rejected. Sequence wrap works;
an advance of 32768 or more is rejected.

With SC high but no valid command, or on timeout/disconnect/suspend/STOP:

* CH3 = **1000**, CH4 = **1500**, CH6 = **1000**.
* Other PC-selected channels center; CH5 remains on **physical SB**.
* Timeout/disconnect/STOP locks PC control until SC is moved out of high and
  back again. Reopening the host program alone cannot restart thrust.

Commands received while manual are rejected. After enabling SC, the first fresh
command establishes the sequence. Arbitration runs in the radio task, nominally
every 2 ms, using current snapshots rather than queued old samples. Switch
sampling runs every 10 ms. ELRS receiver/Betaflight RF-loss failsafe remains
separate. A stalled MCU or RF task is not covered by the USB-command watchdog.

## Command report: host to interface 1

All multibyte integers are little-endian. Send exactly 64 payload bytes.

| Offset | Bytes | Meaning |
|---|---|---|
| 0 | 2 | ASCII `PC` |
| 2 | 1 | Version 1 |
| 3 | 1 | 1 = command; 0 = STOP |
| 4 | 2 | Sequence number |
| 6 | 1 | Channel selection mask, bit 0 = CH1; bit 4 forbidden |
| 7 | 1 | Reserved, zero |
| 8 | 2 | Watchdog milliseconds, 50..250 |
| 10 | 16 | Eight uint16 input values, each 988..2012 |
| 26 | 38 | Reserved, zero |

Slot CH3 holds the thrust input before its 200%/-100% mix. Slots CH4/6 hold
final steer/lift values; the manual throttle's 50%/+50% mix is bypassed for PC lift.
Other selected slots hold
final channel values. All eight values must be in range even if not selected.
STOP disables PC control without requiring a valid sequence or enabled switch.
Both interrupt OUT and HID `SET_REPORT(Output, ID 0)` are supported. The command
path has bounded memory use and performs no flash writes, waits or RF SPI calls.

## Reports: interface 1 to host

Every report begins with ASCII `PC`, version 1, and a type byte at offset 3.
Unused bytes are zero.

**20: raw CRSF telemetry fragment**

* 4..7: completed CRSF frame sequence (uint32).
* 8: total frame length (4..64); 9: byte offset; 10: fragment length (1..53).
* 11..63: fragment data. A 64-byte frame takes two reports.

Frames include CRSF address, length, type and CRC. Only validated complete OTA
reassemblies are forwarded. A frame is copied before fragmenting, so fragments
cannot mix two frames. If the host stops reading, newer complete frames can
replace the latest available frame; skipped frames are counted in status.
Telemetry backpressure does not block RF or command reception.

**21: status, nominally 10 Hz**

| Offset | Meaning |
|---|---|
| 4 | State: 0 manual, 1 PC, 2 failsafe |
| 5 | PC command permission |
| 6..7 | Last accepted sequence |
| 8..9 | Watchdog ms |
| 10..13 | Accepted command count |
| 14..17 | Rejected report count |
| 18..21 | Skipped complete telemetry frames |
| 22 | ELRS connection enum; 2 = connected |
| 23..32 | Ten CRSF link-stat bytes: uplink RSSI1/RSSI2/LQ/signed SNR, antenna, RF mode, TX power code, downlink RSSI/LQ/signed SNR |
| 33 | PC control locked |
| 34 | Current channel mask |
| 35 | Radio power enabled |
| 36 | CH5 arm command: 0 unavailable, 1 disarm, 2 arm (not FC confirmation) |

RSSI bytes are positive magnitudes of negative dBm; SNR is signed whole dB.
Connection status relies on downlink telemetry and does not prove motor outputs.

## Python utility and bench check

Install `hidapi` in your Python environment, then:

```powershell
python tools/pc_hid.py --list
python tools/pc_hid.py --joystick
# Explicit 50 Hz command stream: forward off, differential centered, lift off
python tools/pc_hid.py --send --lift -100 --thrust -100 --steer 0
```

Use `--serial` if multiple radios are attached. `--channels` supplies eight raw
input values; `--mask` selects outputs. Ctrl+C sends STOP. Monitoring alone does
not send commands. Battery CRSF frames decode voltage, current, mAh and percent;
other frames print raw hex. Status reports print link and command counters.

Bench acceptance, with motor outputs made safe:

1. Confirm the legacy joystick and bootloader connection still enumerate.
2. Power the radio on, put SC low, and verify the manual mapping on the receiver.
3. Start the safe command stream, then move SC high; verify state PC and counters.
4. Test lift/thrust/steer separately. SB alone must control CH5 in every state.
5. Stop the process, unplug USB, suspend USB and send malformed/replayed commands;
   verify safe outputs and required switch cycle after timeout/disconnect.
6. Confirm joystick changes still track physical controls during PC operation.
7. Compare battery telemetry with Betaflight and test sustained USB/RF operation.

Host tests and Keil build are automated; physical USB enumeration, simultaneous
RF timing and receiver behavior require hardware verification of this build.

## Keyboard controller

```powershell
python tools/keyboard_control.py
# Optional when multiple radios are connected:
python tools/keyboard_control.py --serial YOUR_USB_SERIAL
```

This adapts `betahovercontrol/src/zorro_control/keyboard.py` to `LiteRadioClient`
over HID. Tkinter is included in standard Windows Python installations; the
additional dependency is `hidapi`. No COM-port argument is needed.

* **W:** target thrust -40%, released target -100%.
* **A/D:** steer -50%/+50%; both pressed cancel to 0%.
* **Space:** toggle lift between -100% and -30%; auto-repeat does not retoggle.
* **Esc/window close:** safe command, then STOP and close.
* **Focus loss:** clear held keys and lift toggle and send safe outputs immediately.

The original code's actual targets are retained (its old comments described
-60% thrust and 0% lift, but the code used -40% and -30%). Update interval is
40 ms. Slew limits are thrust 120 percentage points/s, steer 225/s and lift 150/s.
Status shows PC permission, lock state, LQ/RSSI and battery telemetry when available.
CH6 still has the seven-position Hybrid resolution described above.

For another controller:

```python
from pc_hid import LiteRadioClient
radio = LiteRadioClient()
try:
    # Repeat at 25-50 Hz while PC control is enabled.
    radio.send_control(lift=-30, thrust=-40, steer=0)
    print(radio.read_telemetry())
finally:
    radio.close()  # Sends STOP; SC must be cycled before PC control resumes.
```

### Keyboard telemetry display

The battery bar is **Vehicle battery (RX/FC)**: CRSF battery telemetry generated
by the vehicle flight controller and forwarded through the ELRS receiver.
It is not the transmitter handset battery; handset battery data is not reported.

Colored bars show RX uplink RSSI, vehicle battery voltage, and RF connection/LQ.
The RSSI display scale is -120..-40 dBm. The voltage display defaults to
3.0..4.35 V for the 1S vehicle; use `--battery-min` and `--battery-max` to change
that scale. Bar fill represents voltage on this scale, not estimated charge.
Colors on these two bars indicate position on their display scales, not calibrated
battery alarms or an RF sensitivity threshold. LQ is the reported percentage;
its bar is green at 90%+, amber at 50..89%, and red below 50%.

Missing telemetry is gray. USB status older than one second and battery readings
older than five seconds are marked stale. An RF disconnect grays out the signal
and battery readings and marks the link red. The last battery voltage remains
labeled stale with its age instead of appearing live.

The **Arm command (SB / CH5)** bar shows red **ARM requested** or green
**DISARM requested**, from the actual CH5 value published to the RF packet path.
This indicates the radio's command, not confirmation that the FC has armed.
Stale status, powered-off RF, or older firmware without this status field shows
**Unknown / unavailable** in gray. The new arm indicator requires the matching
updated transmitter firmware. Physical SB remains the only arming control.
