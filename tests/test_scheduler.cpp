/* Execute the firmware scheduler's actual function bodies with a radio mock.
 * Verify slots across nonce wrap, every rate/ratio and binding completion. */
#include <cassert>
#include <cstdint>
#include <cstring>
extern "C" {
#include "elrs_v3.h"
void FHSSrandomiseFHSSsequence(uint32_t);
uint32_t GetInitialFreq(void);
uint32_t FHSSgetNextFreq(void);
uint8_t FHSSgetCurrIndex(void);
extern volatile uint8_t FHSSptr;
}
#define Regulatory_Domain_ISM_2400 1
#define SYNC_PACKET 2
#define MSP_DATA_PACKET 1
#define TLM_PACKET 3
#define ELRS_TELEMETRY_TYPE_LINK 1
#define ELRS_TELEMETRY_TYPE_DATA 2
#define ELRS_CRC_LEN 256
#define ELRS_CRC14_POLY 0x2e57
static uint16_t crc14tab[ELRS_CRC_LEN];
#define syncSpamAResidualTimeMS 500
enum expresslrs_tlm_ratio_e { TLM_RATIO_NO_TLM=0,TLM_RATIO_1_128,TLM_RATIO_1_64,TLM_RATIO_1_32,TLM_RATIO_1_16,TLM_RATIO_1_8,TLM_RATIO_1_4,TLM_RATIO_1_2 };
enum { disconnected, connected };
static struct {uint8_t radioTXdataBuffer[8],radioRXdataBuffer[8]; uint32_t currFreq; int8_t LastPacketSNR,LastPacketRSSI;} Radio;
static struct {uint8_t index,FHSShopInterval; expresslrs_tlm_ratio_e TLMinterval;} mod, *ExpressLRS_currAirRate_Modparams=&mod;
static struct {uint32_t SyncPktIntervalConnected,SyncPktIntervalDisconnected;} perf={5000,10},*ExpressLRS_currAirRate_RFperfParams=&perf;
static struct {uint8_t rate,tlm;} tx_config;
static struct {uint8_t downlink_Link_quality,downlink_RSSI,uplink_RSSI_1,uplink_RSSI_2,active_antenna,uplink_Link_quality;int8_t downlink_SNR,uplink_SNR;} linkStatistics;
static uint8_t NonceTX,WaitRXresponse,busyTransmitting,InBindingMode,NextPacketIsMspData,BindingSendCount,syncSpamCounter,telemetrySlots;
static uint32_t telemetryWindow,rfModeLastChangedMS,SyncPacketLastSent,LastTLMpacketRecvMillis;
static uint8_t UID[6]={0,0,12,23,34,45};
static uint16_t CRCInitializer;
static unsigned connectionState,clockMs,txCount,rxCount;
static uint32_t HAL_GetTick(){return clockMs;}
static uint8_t StubbornSender_IsActive(){return InBindingMode;}
static void StubbornSender_GetCurrentPayload(uint8_t *index,uint8_t *length,uint8_t **data){
    static uint8_t bind[5]={9,12,23,34,45}; *index=1;*length=5;*data=bind;
}
static uint8_t TLMratioEnumToValue(expresslrs_tlm_ratio_e ratio){return ratio?1<<(8-ratio):1;}
static void SetFrequencyReg(uint32_t f){Radio.currFreq=f;}
static void RXnb(){++rxCount;}
static void GenerateChannelDataHybridSwitch8(volatile uint8_t *p,uint16_t *ch){elrs_v3_pack_channels(p,ch,0,elrsTelemetry.ack);}
static uint16_t reference_crc14(uint8_t *p,uint8_t n,uint16_t crc){
    while(n--) {crc^=(uint16_t)*p++<<6;for(unsigned b=0;b<8;++b)crc=(crc<<1)^((crc&0x2000)?0x2e57:0);}
    return crc&0x3fff;
}
static void TXnb(volatile uint8_t *p,uint8_t n){
    assert(n==8);++txCount;
    uint8_t data[7];for(unsigned i=0;i<7;++i)data[i]=p[i];data[0]&=3;
    assert(reference_crc14(data,7,CRCInitializer)==(((uint16_t)(p[0]&0xfc)<<6)|p[7]));
    if((p[0]&3)==2){
        assert(p[1]==FHSSgetCurrIndex() && p[2]==NonceTX);
        assert(p[3]==elrs_v3_sync_config(tx_config.rate,tx_config.tlm));
    }
}
#include "scheduler.inc"

void test_scheduler(){
    generateCrc14Table();
    uint16_t channels[8]={1500,1500,1000,1500,1000,1500,1500,1500};
    for(unsigned rate=0;rate<4;++rate)for(unsigned ratio=0;ratio<8;++ratio){
        mod.index=rate;mod.FHSShopInterval=rate==3?2:4;
        mod.TLMinterval=(expresslrs_tlm_ratio_e)ratio;
        tx_config.rate=rate;tx_config.tlm=ratio;
        FHSSrandomiseFHSSsequence(elrs_v3_fhss_seed(UID));Radio.currFreq=GetInitialFreq();
        CRCInitializer=elrs_v3_crc_init(UID);
        NonceTX=WaitRXresponse=busyTransmitting=InBindingMode=NextPacketIsMspData=BindingSendCount=0;
        telemetryWindow=telemetrySlots=0;syncSpamCounter=3;SyncPacketLastSent=rfModeLastChangedMS=0;
        txCount=rxCount=0;
        for(unsigned slot=1;slot<=1024;++slot){
            clockMs=slot*7;
            unsigned oldTx=txCount;
            SendRCdataToRF(channels);
            assert(NonceTX==(slot&255));
            unsigned denom=TLMratioEnumToValue(mod.TLMinterval);
            bool tlm=denom>1 && slot%denom==0;
            assert(txCount==oldTx+(tlm?0:1));
            if(!tlm){
                TXdoneISR();
                unsigned hop=FHSSptr,rx=rxCount;
                TXdoneISR(); // duplicate DIO must not hop/listen twice
                assert(hop==FHSSptr && rx==rxCount);
            }
            assert(FHSSptr==((slot+1)/mod.FHSShopInterval)%240);
        }
        unsigned denom=TLMratioEnumToValue(mod.TLMinterval);
        assert(txCount==1024-(denom>1?1024/denom:0));
    }
    InBindingMode=1;NonceTX=0;BindingSendCount=0;NextPacketIsMspData=1;WaitRXresponse=0;
    mod.index=3;mod.FHSShopInterval=2;mod.TLMinterval=TLM_RATIO_1_16;
    CRCInitializer=0;syncSpamCounter=3;txCount=rxCount=0;
    FHSSrandomiseFHSSsequence(elrs_v3_fhss_seed(UID));Radio.currFreq=GetInitialFreq();
    for(unsigned slot=0;slot<30;++slot){
        SendRCdataToRF(channels);
        assert((Radio.radioTXdataBuffer[0]&3)!=2);
        assert(NonceTX==0 && FHSSptr==0 && rxCount==0 && CRCInitializer==0);
        TXdoneISR();
    }
    assert(BindingSendCount==7 && txCount==13 && !busyTransmitting);
    // Real RX parser: bitfields, signed quarter-dB SNR, CRC and slot gates.
    InBindingMode=0;WaitRXresponse=2;telemetryWindow=0;
    CRCInitializer=elrs_v3_crc_init(UID);
    uint8_t tlm[8]={3,1,static_cast<uint8_t>(80|128),static_cast<uint8_t>(90|128),static_cast<uint8_t>(97|128),static_cast<uint8_t>(-12),0,0};
    uint16_t crc=reference_crc14(tlm,7,CRCInitializer);
    tlm[0]|=(crc>>6)&0xfc;tlm[7]=crc&255;
    Radio.LastPacketRSSI=-85;Radio.LastPacketSNR=-2;
    memcpy(Radio.radioRXdataBuffer,tlm,8);ProcessTLMpacket();
    assert(connectionState==connected && linkStatistics.uplink_Link_quality==97);
    assert(linkStatistics.uplink_RSSI_1==80 && linkStatistics.uplink_RSSI_2==90);
    assert(linkStatistics.active_antenna==1 && linkStatistics.uplink_SNR==-3);
    assert(linkStatistics.downlink_RSSI==85 && linkStatistics.downlink_SNR==-2);
    assert(telemetryWindow==1);
    clockMs++;
    memcpy(Radio.radioRXdataBuffer,tlm,8);ProcessTLMpacket();
    assert(LastTLMpacketRecvMillis!=clockMs); // duplicate within same slot
    telemetryWindow=0;tlm[7]^=1;
    memcpy(Radio.radioRXdataBuffer,tlm,8);ProcessTLMpacket();
    assert(!telemetryWindow && LastTLMpacketRecvMillis!=clockMs);
}
