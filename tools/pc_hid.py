"""LiteRadio vendor HID test client. Install the `hidapi` Python package.

Monitor only by default. --send explicitly enables a 50 Hz command stream.
Uses interface 1 / usage page FF00; never sends commands to the joystick.
"""
import argparse
import json
import struct
import time

VID, PID = 0x0483, 0x5750


def command(sequence, channels, mask=0x2C, watchdog=100, stop=False):
    if len(channels) != 8 or any(not 988 <= c <= 2012 for c in channels):
        raise ValueError('Eight channel input values must be in 988..2012')
    if not 0 < mask <= 255 or mask & 0x10 or not 50 <= watchdog <= 250:
        raise ValueError('Mask must exclude CH5; watchdog must be 50..250 ms')
    payload = struct.pack('<2sBBHBBH8H', b'PC', 1, 0 if stop else 1,
                          sequence & 65535, mask, 0, watchdog, *channels)
    # HIDAPI needs a leading zero for an unnumbered report; it is not on the wire.
    return b'\0' + payload.ljust(64, b'\0')


def crc8(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ (0xD5 if crc & 0x80 else 0)) & 255
    return crc


class Decoder:
    def __init__(self):
        self.frame = bytearray()
        self.sequence = None
        self.length = 0

    def receive(self, raw):
        raw = bytes(raw)
        if len(raw) != 64 or raw[:3] != b'PC\x01':
            return None
        if raw[3] == 0x21:
            states = ('MANUAL', 'PC', 'FAILSAFE')
            return dict(type='status', state=states[raw[4]] if raw[4] < 3 else 'UNKNOWN',
                        permitted=bool(raw[5]), sequence=struct.unpack_from('<H', raw, 6)[0],
                        watchdog_ms=struct.unpack_from('<H', raw, 8)[0],
                        accepted=struct.unpack_from('<I', raw, 10)[0],
                        rejected=struct.unpack_from('<I', raw, 14)[0],
                        telemetry_dropped=struct.unpack_from('<I', raw, 18)[0],
                        link_state=raw[22], rssi_dbm=-raw[23], lq=raw[25],
                        snr_db=struct.unpack_from('b', raw, 26)[0],
                        locked=bool(raw[33]), mask=raw[34], radio_powered=bool(raw[35]))
        if raw[3] != 0x20:
            return None
        seq = struct.unpack_from('<I', raw, 4)[0]
        length, offset, count = raw[8:11]
        if not 4 <= length <= 64 or not 0 < count <= 53 or offset + count > length:
            self.sequence = None
            return None
        if offset == 0:
            self.frame, self.sequence, self.length = bytearray(), seq, length
        if seq != self.sequence or length != self.length or offset != len(self.frame):
            return None
        self.frame += raw[11:11 + count]
        if len(self.frame) != length:
            return None
        frame = bytes(self.frame)
        self.sequence = None
        if frame[1] + 2 != length or crc8(frame[2:-1]) != frame[-1]:
            return dict(type='invalid_crsf', frame=frame.hex())
        result = dict(type='crsf', sequence=seq, frame_type=frame[2], frame=frame.hex())
        if frame[2] == 0x08 and length >= 12:
            result.update(voltage_v=int.from_bytes(frame[3:5], 'big') / 10,
                          current_a=int.from_bytes(frame[5:7], 'big') / 10,
                          consumed_mah=int.from_bytes(frame[7:10], 'big'), remaining_percent=frame[10])
        return result


def select_device(hid, interface, serial):
    devices = [d for d in hid.enumerate(VID, PID)
               if (d.get('interface_number') == interface or
                   (interface == 1 and d.get('usage_page') == 0xFF00))
               and (not serial or d.get('serial_number') == serial)]
    if len(devices) != 1:
        raise RuntimeError(f'Expected one interface {interface}, found {len(devices)}. Use --list / --serial.')
    dev = hid.device()
    dev.open_path(devices[0]['path'])
    dev.set_nonblocking(True)
    return dev


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--serial')
    parser.add_argument('--send', action='store_true')
    parser.add_argument('--joystick', action='store_true', help='also read physical joystick interface')
    parser.add_argument('--gv1', type=float, default=0, help='Ele input, -100..100 percent, default 0')
    parser.add_argument('--gv2', type=float, default=0, help='Ail input, -100..100 percent')
    parser.add_argument('--gv3', type=float, default=-100, help='Thr input, -100..100 percent')
    parser.add_argument('--channels', type=int, nargs=8, help='override all eight input slots, units 988..2012')
    parser.add_argument('--mask', type=lambda s: int(s, 0), default=0x2C)
    parser.add_argument('--watchdog', type=int, default=100)
    parser.add_argument('--seconds', type=float, default=0, help='0 runs until Ctrl+C')
    args = parser.parse_args()
    if any(not -100 <= v <= 100 for v in (args.gv1, args.gv2, args.gv3)):
        parser.error('GV values must be -100..100')
    channels = args.channels or [1500, 1500, round(1500+5*args.gv1),
                                round(1500+5*args.gv2), 988, round(1500+5*args.gv3), 1500, 1500]
    command(0, channels, args.mask, args.watchdog)  # validate before opening anything
    import hid
    if args.list:
        for device in hid.enumerate(VID, PID):
            print(device)
        return
    dev = select_device(hid, 1, args.serial)
    joystick = None
    decoder = Decoder()
    sequence = 0
    start = deadline = time.monotonic()
    last_joystick = 0
    try:
        if args.joystick:
            joystick = select_device(hid, 0, args.serial)
        while not args.seconds or time.monotonic() - start < args.seconds:
            now = time.monotonic()
            if args.send and now >= deadline:
                if dev.write(command(sequence, channels, args.mask, args.watchdog)) != 65:
                    raise IOError('Incomplete HID command write')
                sequence = (sequence + 1) & 65535
                deadline = now + .02
            for _ in range(32):
                report = dev.read(64)
                if not report:
                    break
                result = decoder.receive(report)
                if result:
                    print(json.dumps(result), flush=True)
            if joystick:
                report = joystick.read(16)
                if len(report) == 16 and now-last_joystick >= .1:
                    print(json.dumps(dict(type='joystick', axes=struct.unpack('<8H', bytes(report)))))
                    last_joystick = now
            time.sleep(.002)
    except KeyboardInterrupt:
        pass
    finally:
        try:
            if args.send:
                dev.write(command(sequence, channels, args.mask, args.watchdog, stop=True))
        finally:
            dev.close()
            if joystick:
                joystick.close()


if __name__ == '__main__':
    main()
