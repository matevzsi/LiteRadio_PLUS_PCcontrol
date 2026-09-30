"""Host protocol tests without HID hardware or the hidapi dependency."""
import importlib.util
from pathlib import Path
import struct

spec = importlib.util.spec_from_file_location('pc_hid', Path(__file__).resolve().parents[1] / 'tools/pc_hid.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)
packet = host.command(65536, [1500]*8)
assert len(packet) == 65 and packet[:5] == b'\0PC\x01\x01'
assert packet[5:7] == b'\0\0' and packet[7] == 0x2C
assert struct.unpack_from('<8H', packet, 11) == (1500,)*8
for channels,mask,watchdog in [([0]*8,0x2c,100),([1500]*8,0xff,100),([1500]*8,0x2c,251)]:
    try:
        host.command(0,channels,mask,watchdog)
        raise AssertionError('malformed command accepted')
    except ValueError:
        pass
frame = bytearray([0xc8,10,8,0,42,0,7,0,0,23,90])
frame.append(host.crc8(frame[2:]))
decoder = host.Decoder()
def fragment(seq, frame, offset, count):
    return (b'PC\x01\x20'+struct.pack('<I',seq)+bytes([len(frame),offset,count])+frame[offset:offset+count]).ljust(64,b'\0')
result = decoder.receive(fragment(1,frame,0,len(frame)))
assert result['voltage_v'] == 4.2 and result['consumed_mah'] == 23
assert result['current_a'] == .7 and result['remaining_percent'] == 90
large = bytearray([0xc8,62,0x02])+bytearray(range(60))
large.append(host.crc8(large[2:]))
assert len(large)==64
assert decoder.receive(fragment(2,large,0,53)) is None
assert decoder.receive(fragment(3,large,53,11)) is None
assert decoder.receive(fragment(2,large,53,11))['frame'] == large.hex()
frame[-1] ^= 1
assert decoder.receive(fragment(3,frame,0,12))['type'] == 'invalid_crsf'
print('PASS: HID host encoding, validation, CRSF fragmentation and battery decoding')
