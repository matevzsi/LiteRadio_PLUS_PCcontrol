#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
extern "C" {
#include "elrs_v3.h"
void FHSSrandomiseFHSSsequence(uint32_t);
uint32_t GetInitialFreq(void);
uint32_t FHSSgetNextFreq(void);
extern uint8_t FHSSsequence[240];
extern volatile uint8_t FHSSptr;
}
void oracle_pack(uint8_t *, const uint32_t *, uint8_t, bool);
bool oracle_unpack(const uint8_t *, uint32_t *);
void oracle_hops(uint32_t, uint8_t *);
uint16_t oracle_crc_init(const uint8_t *);
uint32_t oracle_seed(const uint8_t *);
void test_scheduler();
void test_radio();
void test_spi();
void test_pc();
void test_usb();

static uint8_t crc8(const uint8_t *p, unsigned n) {
    unsigned crc=0;
    while(n--) { crc ^= *p++; for(unsigned b=0;b<8;++b) crc=((crc<<1)^((crc&128)?0xd5:0))&255; }
    return (uint8_t)crc;
}

static void telemetry_tests() {
    uint8_t frame[65] = {0xC8, 10, 0x08, 0, 42, 0, 7, 0, 0, 23, 90};
    frame[11]=crc8(frame+2,9);
    elrs_v3_telemetry_reset();
    elrs_v3_telemetry_receive(2, frame); // out of order, no ACK
    assert(!elrsTelemetry.ack);
    elrs_v3_telemetry_receive(1, frame);
    assert(elrsTelemetry.ack);
    elrs_v3_telemetry_receive(2, frame+5);
    assert(!elrsTelemetry.ack);
    elrs_v3_telemetry_receive(2, frame+5); // duplicate must not toggle
    assert(!elrsTelemetry.ack);
    elrs_v3_telemetry_receive(0, frame+10);
    assert(elrsTelemetry.frames==1 && elrsTelemetry.length==12);
    assert(!memcmp(elrsTelemetry.frame,frame,12));
    elrs_v3_telemetry_receive(0, frame+10);
    assert(elrsTelemetry.frames==1);
    frame[11]^=1;
    for(unsigned i=0;i<3;++i) elrs_v3_telemetry_receive(i==2?0:i+1,frame+5*i);
    assert(elrsTelemetry.frames==1 && elrsTelemetry.rejected==1);
    elrs_v3_telemetry_receive(63, frame);
    assert(elrsTelemetry.next_package==1 && elrsTelemetry.used==0);
    // Largest CRSF frame: final chunk carries four bytes plus one padding byte.
    for(unsigned i=0;i<64;++i) frame[i]=(uint8_t)i;
    frame[1]=62; frame[63]=crc8(frame+2,61);
    for(unsigned i=0;i<13;++i) elrs_v3_telemetry_receive(i==12?0:i+1,frame+5*i);
    assert(elrsTelemetry.frames==2 && elrsTelemetry.length==64);
    assert(!memcmp(elrsTelemetry.frame,frame,64));
    // Oversized transfers are rejected rather than silently truncated.
    for(unsigned i=0;i<14;++i) elrs_v3_telemetry_receive(i==13?0:i+1,frame);
    assert(elrsTelemetry.frames==2 && elrsTelemetry.rejected==2);
    // Receiver restart midway through an old transfer.
    elrs_v3_telemetry_receive(1, frame);
    elrs_v3_telemetry_receive(2, frame+5);
    elrs_v3_telemetry_receive(1, frame);
    assert(elrsTelemetry.used==5 && elrsTelemetry.next_package==2);
}

int main() {
    test_spi();
    test_pc();
    test_usb();
    std::mt19937 gen(0x353);
    uint8_t packet[8], expected[8];
    for(unsigned sample=0;sample<10000;++sample) {
        uint16_t channels[8]; uint32_t crsf[16];
        for(unsigned ch=0;ch<8;++ch) {
            static const uint16_t edges[]={0,988,1000,1500,2000,2012,65535};
            channels[ch]=sample<7?edges[sample]:(sample&1)?(uint16_t)(988+gen()%1025):(uint16_t)(gen()%65536);
            int value=((int)channels[ch]-1000)*1600/1000+192;
            crsf[ch]=value<172?172:value>1811?1811:value;
        }
        for(unsigned ch=8;ch<16;++ch) crsf[ch]=992;
        for(unsigned index=0;index<7;++index) for(unsigned ack=0;ack<2;++ack) {
            elrs_v3_pack_channels(packet, channels, index, ack);
            oracle_pack(expected,crsf,index,ack!=0);
            assert(!memcmp(packet,expected,7));
            uint32_t decoded[16]={};
            assert(oracle_unpack(packet,decoded)==(ack!=0));
            for(unsigned ch=0;ch<4;++ch) {
                int error=(int)decoded[ch]-(int)crsf[ch];
                assert(error>=-1 && error<=1);
            }
            assert(decoded[4]==(crsf[4]>992?1792u:191u));
        }
    }
    for(unsigned sample=0;sample<1000;++sample) {
        uint8_t uid[6], hops[240];
        for(auto &b:uid) b=(uint8_t)gen();
        assert(elrs_v3_crc_init(uid)==oracle_crc_init(uid));
        assert(elrs_v3_fhss_seed(uid)==oracle_seed(uid));
        oracle_hops(oracle_seed(uid),hops);
        FHSSrandomiseFHSSsequence(elrs_v3_fhss_seed(uid));
        assert(!memcmp(FHSSsequence,hops,240));
        const uint32_t first=(uint32_t)(2400400000.0/(52000000.0/262144.0));
        const uint32_t last=(uint32_t)(2479400000.0/(52000000.0/262144.0));
        const uint32_t spread=(last-first)*256/79;
        assert(GetInitialFreq()==first+41*spread/256);
        for(unsigned i=1;i<=480;++i) {
            assert(FHSSgetNextFreq()==first+hops[i%240]*spread/256);
            assert(FHSSptr==i%240);
        }
    }
    const uint8_t rates[]={4,6,7,9};
    for(unsigned r=0;r<4;++r) for(unsigned tlm=0;tlm<8;++tlm)
        assert(elrs_v3_sync_config(r,tlm)==((rates[r]<<4)|(tlm<<1)|1));
    telemetry_tests();
    test_scheduler();
    test_radio();
    puts("PASS: 140000 upstream channel comparisons and RX round trips, 1000 complete FHSS/seed comparisons,");
    puts("      frequency wrap, sync fields, telemetry CRC/reassembly, RF scheduling and radio IRQs.");
    puts("      USB routing/descriptors/telemetry, PC mapping, watchdog, replay and disconnect tests.");
}
