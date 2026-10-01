"""Host protocol tests without HID hardware or the hidapi dependency."""
import importlib.util
from pathlib import Path
import struct
import math

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
assert host.control_channels(thrust=-100,steer=0,lift=-100)[2:6] == [1500,1500,988,1000]
assert host.control_channels(thrust=0,steer=-100,lift=0)[2:6] == [1750,1000,988,1500]
assert host.control_channels(thrust=100,steer=100,lift=100)[2:6] == [2000,2000,988,2000]
for value in range(-100,101):
    channels=host.control_channels(thrust=value,steer=value,lift=value)
    fc_thrust=1500+(channels[2]-1500)*2-500
    assert abs(fc_thrust-(1500+5*value))<=1
    assert channels[3]==channels[5]==1500+5*value
for value in [-101,101,math.nan,math.inf]:
    for name in ['lift','thrust','steer']:
        try:
            host.control_channels(**{name:value})
            raise AssertionError('bad named control accepted')
        except ValueError:
            pass
class FakeHid:
    def __init__(self): self.writes=[]; self.closed=False
    def write(self,p): self.writes.append(p); return len(p)
    def read(self,n): return []
    def close(self): self.closed=True
device=FakeHid()
client=host.LiteRadioClient(device=device)
client.send_control(thrust=-40,steer=50,lift=-30)
slots=struct.unpack_from('<8H',device.writes[0],11)
assert slots[2]==1650 and slots[3]==1750 and slots[5]==1350
client.close()
assert device.closed and device.writes[-1][4]==0
print('PASS: HID host encoding, validation, CRSF fragmentation and battery decoding')

status=bytearray(64);status[:4]=b'PC\x01\x21'
assert host.Decoder().receive(status)['arm_command'] is None
status[36]=1;assert host.Decoder().receive(status)['arm_command'] is False
status[36]=2;assert host.Decoder().receive(status)['arm_command'] is True
print('PASS: arm command decoding and compatibility with older firmware')
