# Hardware acceptance record

Status: **not run**. Complete this before enabling PC-generated control.
Use a bench setup with propulsion disabled during channel/failsafe checks.

Record radio hardware revision, bootloader, receiver Web UI version and
target, regulatory domain, Betaflight target/version, firmware SHA-256,
binding method and configured rate/telemetry ratio.

## Baselines

- [ ] Official BETAFPV ELRS3 radio firmware binds to the Matrix receiver.
- [ ] CH1..4 and AUX1..4 move correctly in Betaflight Receiver tab.
- [ ] Baseline open-source BIN builds, flashes and can be recovered.
- [ ] USB joystick and radio calibration work with the baseline.

## V3 firmware

- [ ] Custom BIN flashes; bootloader recovery remains available.
- [ ] Start with 150 Hz, 1:8 telemetry; verify actual persisted settings.
- [ ] BIND press results in a solid receiver LED and working channels.
- [ ] CH1..4 endpoints, center, direction and AETR mapping are correct.
- [ ] AUX1 arm/enable is low/high only; AUX2..4 positions are correct.
- [ ] Link remains stable for at least ten minutes while moving controls.
- [ ] `linkStatistics` updates RSSI, LQ and SNR plausibly.
- [ ] `elrsTelemetry.frames` advances with Betaflight telemetry enabled.
- [ ] Inspect battery frame type 0x08 against Betaflight voltage/current/mAh.
- [ ] Receiver-off and transmitter-off each produce safe vehicle outputs.
- [ ] Restore each side; reconnect without rebinding.
- [ ] Repeat at least ten power cycles on each side, including RX-first/TX-first.
- [ ] Repeat binding/reconnect at 50/250/500 Hz; record any unsupported rate.
- [ ] Check radio configuration changes and binding while telemetry is active.
- [ ] Verify flash usage, free heap and each task's stack watermark on hardware.
- [ ] Scope TIM1/DIO1/SPI timing at 150 and 500 Hz: packet interval, TX completion,
  hop timing, telemetry slot, interrupt latency and absence of overruns.

Native tests cannot establish RF range, on-device interrupt timing, PA behavior,
or actual receiver binding. Attach results and firmware checksum here before
starting USB control/watchdog/telemetry-forwarding work.
