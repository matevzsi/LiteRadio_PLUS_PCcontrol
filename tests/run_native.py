"""Run from a Visual Studio developer prompt, or with g++ on PATH.

Requires the pinned ExpressLRS source in .reference/ExpressLRS.
Extracts the actual upstream functions to avoid a hand-written protocol oracle.
Generated harness files live only in artifacts/native-tests.
"""
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REF = ROOT / '.reference/ExpressLRS/src'
OUT = ROOT / 'artifacts/native-tests'
OUT.mkdir(parents=True, exist_ok=True)
revision = subprocess.check_output(['git', '-C', str(REF), 'rev-parse', 'HEAD'], text=True).strip()
assert revision == '40555e141efb0c93ea8d075ec47a27592355f924', revision


def read(path):
    return path.read_text(encoding='utf-8')


def function(source, name):
    start = re.search(r'^.*\b' + name + r'\([^;]*?\)\s*\{', source, re.M).start()
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


ota = read(REF / 'lib/OTA/OTA.cpp')
protocol = read(REF / 'lib/CrsfProtocol/crsf_protocol.h')
header = read(REF / 'lib/OTA/OTA.h')
structs = header[header.index('typedef struct'):header.index('} PACKED OTA_Packet4_s;') + len('} PACKED OTA_Packet4_s;')]
oracle = '''#include <cstdint>
#include <cstring>
#include <algorithm>
#define ICACHE_RAM_ATTR
#define PACKED
#define PACKET_TYPE_RCDATA 0
#define CRSF_CHANNEL_VALUE_MIN 172
#define CRSF_CHANNEL_VALUE_MAX 1811
#define CRSF_CHANNEL_VALUE_1000 191
#define CRSF_CHANNEL_VALUE_2000 1792
#define CRSF_CHANNEL_VALUE_MID 992
#define constrain(v,l,h) std::min(std::max((uint32_t)(v),(uint32_t)(l)),(uint32_t)(h))
#define DBG(...)
#define DBGCR
#pragma pack(push,1)
'''
oracle += read(REF / 'lib/TelemetryProtocol/telemetry_protocol.h').replace('#pragma once', '')
oracle += structs + '\n#pragma pack(pop)\nstruct OTA_Packet_s { OTA_Packet4_s std; };\n'
for name in ['fmap', 'CRSF_to_UINT10', 'CRSF_to_N', 'CRSF_to_SWITCH3b', 'CRSF_to_BIT',
             'UINT10_to_CRSF', 'N_to_CRSF', 'SWITCH3b_to_CRSF', 'BIT_to_CRSF']:
    oracle += function(protocol, name)
oracle += 'typedef uint32_t (*Decimate11to10_fn)(uint32_t);\nstatic uint8_t Hybrid8NextSwitchIndex;\n'
for name in ['Decimate11to10_Limit', 'PackUInt11ToChannels4x10', 'PackChannelDataHybridCommon', 'GenerateChannelDataHybrid8',
             'UnpackChannels4x10ToUInt11', 'UnpackChannelDataHybridCommon', 'UnpackChannelDataHybridSwitch8']:
    oracle += function(ota, name)
oracle += '''void oracle_pack(uint8_t *out,const uint32_t *channels,uint8_t index,bool ack) {
    static_assert(sizeof(OTA_Packet4_s)==8,"packing");
    OTA_Packet_s p={}; Hybrid8NextSwitchIndex=index;
    GenerateChannelDataHybrid8(&p,channels,ack,8); memcpy(out,&p,8);
}
bool oracle_unpack(const uint8_t *packet,uint32_t *channels) {
    OTA_Packet_s p; memcpy(&p,packet,8);
    return UnpackChannelDataHybridSwitch8(&p,channels,8);
}
namespace upstream {
static uint8_t UID[6];
static uint16_t OtaCrcInitializer;
#define OTA_VERSION_ID 3
'''
oracle += function(ota, 'OtaUpdateCrcInitFromUid')
oracle += function(read(REF / 'src/common.cpp'), 'uidMacSeedGet')
oracle += read(REF / 'lib/FHSS/random.cpp').replace('#include "random.h"', '')
oracle += 'static uint8_t FHSSptr;\nunsigned FHSSgetSequenceCount(){return 240;}\n'
oracle += function(read(REF / 'lib/FHSS/FHSS.cpp'), 'FHSSrandomiseFHSSsequenceBuild')
oracle += '''}
uint16_t oracle_crc_init(const uint8_t *uid){memcpy(upstream::UID,uid,6);upstream::OtaUpdateCrcInitFromUid();return upstream::OtaCrcInitializer;}
uint32_t oracle_seed(const uint8_t *uid){memcpy(upstream::UID,uid,6);return upstream::uidMacSeedGet();}
void oracle_hops(uint32_t seed,uint8_t *out){upstream::FHSSrandomiseFHSSsequenceBuild(seed,80,41,out);}
'''
(OUT / 'oracle.cpp').write_text(oracle)
# Compile the actual firmware FHSS source with only its board include replaced.
fhss = read(ROOT / 'USER/protocol/fhss.c').replace('#include "fhss.h"', '''#include <stdint.h>
#define Regulatory_Domain_ISM_2400 1
#define FREQ_HZ_TO_REG_VAL_24(f) ((uint32_t)((double)(f)/(52000000.0/262144.0)))
#define NR_FHSS_ENTRIES (sizeof(FHSSfreqs)/sizeof(uint32_t))
''')
(OUT / 'fhss.c').write_text(fhss)

# The scheduler harness includes unmodified function bodies from the firmware.
tx = read(ROOT / 'USER/Drivers/tx_main.c')
names = ['ProcessTLMpacket', 'GenerateSyncPacketData', 'HandleFHSS', 'HandleTLM', 'SendRCdataToRF', 'TXdoneISR']
common = read(ROOT / 'USER/protocol/common.c')
crc = function(common, 'generateCrc14Table') + function(common, 'calcCrc14')
(OUT / 'scheduler.inc').write_text(crc + '\n'.join(function(tx, n) for n in names), encoding='utf-8')
spi = function(read(ROOT / 'USER/Hardware/sx1280hal.c'), 'SX1280_HalReadCommand')
# MSVC lacks VLAs: preserve the old function's transaction length while
# giving its scratch buffer fixed storage, so its read defect can be reproduced.
if 'halTxBuffer[size + 2]' in spi:
    spi = spi.replace('halTxBuffer[size + 2]', 'halTxBuffer[258]').replace('sizeof(halTxBuffer)', '(size + 2)')
(OUT / 'spi.inc').write_text(spi, encoding='utf-8')
usb = read(ROOT / 'USER/Drivers/usb_pc.c')
(OUT / 'usb_pc.inc').write_text(re.sub(r'^#include.*$', '', usb, flags=re.M), encoding='utf-8')
sources = [ROOT / 'tests/test_spi.cpp', ROOT / 'tests/test_elrs_v3.cpp', OUT / 'oracle.cpp', OUT / 'fhss.c',
           ROOT / 'tests/test_pc.cpp', ROOT / 'USER/Drivers/pc_control.c',
           ROOT / 'tests/test_usb.cpp',
           ROOT / 'USER/protocol/elrs_v3.c', ROOT / 'tests/test_scheduler.cpp', ROOT / 'tests/test_radio.cpp']
radio = read(ROOT / 'USER/Hardware/sx1280.c')
(OUT / 'radio.inc').write_text('\n'.join(function(radio, n) for n in
    ['SX1280_TXnbISR', 'SX1280_RXnbISR', 'SX1280_IsrCallback', 'SX1280_TXnb']), encoding='utf-8')
exe = OUT / ('tests.exe' if shutil.which('cl') else 'tests')
if shutil.which('cl'):
    command = ['cl', '/nologo', '/utf-8', '/EHsc', '/std:c++14', '/I' + str(ROOT / 'USER/protocol'),
               '/I' + str(OUT), '/I' + str(ROOT / 'USER/Hardware'), '/I' + str(ROOT / 'USER/Drivers'), *map(str, sources), '/Fe:' + str(exe)]
else:
    # Compile C units as C, preserving linkage.
    objects = []
    for src in sources:
        obj = OUT / (src.stem + '.o')
        compiler = 'gcc' if src.suffix == '.c' else 'g++'
        subprocess.run([compiler, '-I' + str(ROOT / 'USER/protocol'), '-I' + str(OUT),
                        '-I' + str(ROOT / 'USER/Hardware'), '-I' + str(ROOT / 'USER/Drivers'),
                        '-c', str(src), '-o', str(obj)], check=True, cwd=OUT)
        objects.append(str(obj))
    command = ['g++', *objects, '-o', str(exe)]
subprocess.run(command, check=True, cwd=OUT)
subprocess.run([str(exe)], check=True, cwd=OUT)
