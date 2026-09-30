#include <cassert>
#include <cstdint>
#include <cstring>
#include "sx1280reg.h"
namespace radio_test {
static struct {SX1280_RadioOperatingModes_t currOpmode;uint8_t radioRXdataBuffer[8];} SX1280;
static unsigned txDone,rxDone,rxReads,txWrites;
static uint16_t pending;
#define TXRXBuffSize 8
static void SX1280_SetMode(SX1280_RadioOperatingModes_t mode){SX1280.currOpmode=mode;}
static void SX1280_ClearIrqStatus(uint16_t mask){pending&=~mask;}
static uint16_t SX1280_GetIrqStatus(){return pending;}
static void TXdoneISR(){++txDone;}
static void RXdoneISR(){++rxDone;}
static uint8_t SX1280_GetRxBufferAddr(){return 0;}
static void SX1280_HalReadBuffer(uint8_t,uint8_t *p,uint8_t n){++rxReads;memset(p,0,n);}
static void SX1280_GetLastPacketStats(){}
static void SX1280Hal_TXenable(){}
static void SX1280Hal_RXenable(){}
static void SX1280_HalWriteBuffer(uint8_t,volatile uint8_t *,uint8_t){++txWrites;assert(SX1280.currOpmode==SX1280_MODE_FS);assert(!pending);}
#include "radio.inc"
}
void test_radio(){
    using namespace radio_test;
    SX1280.currOpmode=SX1280_MODE_RX;pending=SX1280_IRQ_RX_DONE;
    SX1280_IsrCallback();assert(rxDone==1 && rxReads==1 && !txDone);
    assert(SX1280.currOpmode==SX1280_MODE_RX);
    pending=SX1280_IRQ_RX_DONE|SX1280_IRQ_CRC_ERROR;
    SX1280_IsrCallback();assert(rxDone==1);
    pending=SX1280_IRQ_RX_DONE|SX1280_IRQ_SYNCWORD_ERROR;
    SX1280_IsrCallback();assert(rxDone==1);
    pending=SX1280_IRQ_TX_DONE;SX1280_IsrCallback();assert(!txDone);
    SX1280.currOpmode=SX1280_MODE_TX;pending=SX1280_IRQ_TX_DONE;
    SX1280_IsrCallback();assert(txDone==1 && SX1280.currOpmode==SX1280_MODE_FS);
    pending=SX1280_IRQ_TX_DONE;SX1280_IsrCallback();assert(txDone==1);
    SX1280.currOpmode=SX1280_MODE_TX;pending=SX1280_IRQ_RX_DONE;
    SX1280_IsrCallback();assert(rxDone==1);
    uint8_t p[8]={};SX1280.currOpmode=SX1280_MODE_RX;pending=SX1280_IRQ_RX_DONE;
    SX1280_TXnb(p,8);assert(txWrites==1 && !pending && SX1280.currOpmode==SX1280_MODE_TX);
    SX1280_TXnb(p,8);assert(txWrites==1 && txDone==2); // stalled TX recovery
}
