/* Real composite class implementation with a fake USB peripheral and legacy class. */
#include <cassert>
#include <cstdint>
#include <cstring>
#include <algorithm>
extern "C" {
#include "pc_control.h"
#include "elrs_v3.h"
}
namespace usb_test {
enum { USBD_OK, USBD_BUSY, USBD_FAIL, USBD_STATE_CONFIGURED=3, USBD_EP_TYPE_INTR=3,
       USB_REQ_TYPE_MASK=0x60, USB_REQ_TYPE_CLASS=0x20, USB_REQ_TYPE_STANDARD=0,
       USB_REQ_GET_DESCRIPTOR=6, USB_REQ_GET_INTERFACE=10, USB_REQ_SET_INTERFACE=11,
       USB_REQ_GET_STATUS=0, CUSTOM_HID_REQ_SET_REPORT=9, CUSTOM_HID_REQ_SET_IDLE=10,
       CUSTOM_HID_REQ_GET_IDLE=2, CUSTOM_HID_REQ_SET_PROTOCOL=11, CUSTOM_HID_REQ_GET_PROTOCOL=3 };
struct USBD_SetupReqTypedef { uint8_t bmRequest,bRequest; uint16_t wValue,wIndex,wLength; };
struct Endpoint { uint8_t is_used; };
struct USBD_HandleTypeDef { uint8_t dev_state; Endpoint ep_in[3],ep_out[3]; void *pClassData; };
struct USBD_CUSTOM_HID_HandleTypeDef { uint32_t IsReportAvailable; };
typedef uint8_t (*InitFn)(USBD_HandleTypeDef*,uint8_t);
typedef uint8_t (*SimpleFn)(USBD_HandleTypeDef*);
struct USBD_ClassTypeDef {
    InitFn Init,DeInit;
    uint8_t (*Setup)(USBD_HandleTypeDef*,USBD_SetupReqTypedef*);
    SimpleFn EP0_TxSent,EP0_RxReady;
    InitFn DataIn,DataOut;
    SimpleFn SOF;
    InitFn IsoINIncomplete,IsoOUTIncomplete;
    uint8_t *(*GetHSConfigDescriptor)(uint16_t*), *(*GetFSConfigDescriptor)(uint16_t*),
            *(*GetOtherSpeedConfigDescriptor)(uint16_t*), *(*GetDeviceQualifierDescriptor)(uint16_t*);
};
USBD_HandleTypeDef hUsbDeviceFS={USBD_STATE_CONFIGURED};
static uint32_t ticks;
static uint8_t legacy_config[41]={9,2,41,0,1,1,0,0xc0,50};
static unsigned legacy_setup_calls,legacy_in_calls,legacy_out_calls,stalls,tx_calls;
static uint8_t *received_buffer,*control_buffer,*descriptor_data;
static uint16_t received_size,descriptor_size;
static uint8_t sent[64];
static uint8_t fake_init(USBD_HandleTypeDef*,uint8_t){return USBD_OK;}
static uint8_t fake_setup(USBD_HandleTypeDef*,USBD_SetupReqTypedef*){++legacy_setup_calls;return USBD_OK;}
static uint8_t fake_in(USBD_HandleTypeDef*,uint8_t){++legacy_in_calls;return USBD_OK;}
static uint8_t fake_out(USBD_HandleTypeDef*,uint8_t){++legacy_out_calls;return USBD_OK;}
static uint8_t fake_rx(USBD_HandleTypeDef*){return USBD_OK;}
static uint8_t *fake_desc(uint16_t *n){*n=41;return legacy_config;}
USBD_ClassTypeDef USBD_CUSTOM_HID={fake_init,fake_init,fake_setup,nullptr,fake_rx,fake_in,fake_out,
    nullptr,nullptr,nullptr,fake_desc,fake_desc,fake_desc,fake_desc};
static uint32_t HAL_GetTick(){return ticks;}
static uint8_t Status_RadioPowered(){return 1;}
static uint8_t connectionState=2;
static uint16_t channelData[16]={1500,1500,1000,1500,2000};
static uint8_t linkStatistics[10]={};
static uint8_t USBD_LL_OpenEP(USBD_HandleTypeDef*,uint8_t,uint8_t,uint16_t){return USBD_OK;}
static uint8_t USBD_LL_CloseEP(USBD_HandleTypeDef*,uint8_t){return USBD_OK;}
static uint8_t USBD_LL_PrepareReceive(USBD_HandleTypeDef*,uint8_t ep,uint8_t *p,uint16_t n){
    assert(ep==2 && n==64); received_buffer=p; return USBD_OK;
}
static uint16_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef*,uint8_t ep){assert(ep==0 || ep==2);return received_size;}
static uint8_t USBD_CtlPrepareRx(USBD_HandleTypeDef*,uint8_t *p,uint16_t n){assert(n==64);control_buffer=p;return USBD_OK;}
static uint8_t USBD_CtlSendData(USBD_HandleTypeDef*,uint8_t *p,uint16_t n){descriptor_data=p;descriptor_size=n;return USBD_OK;}
static void USBD_CtlError(USBD_HandleTypeDef*,USBD_SetupReqTypedef*){++stalls;}
static uint8_t USBD_LL_Transmit(USBD_HandleTypeDef*,uint8_t ep,uint8_t *p,uint16_t n){
    assert(ep==0x82 && n==64);memcpy(sent,p,64);++tx_calls;return USBD_OK;
}
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define MIN(a,b) ((a)<(b)?(a):(b))
#include "usb_pc.inc"
}
void test_usb() {
    using namespace usb_test;
    auto *dev=&hUsbDeviceFS;
    assert(init(dev,1)==USBD_OK);
    uint16_t size; auto *cfg=configuration(&size);
    assert(size==73 && cfg[2]==73 && cfg[4]==2 && cfg[43]==1);
    assert(!memcmp(cfg+9,legacy_config+9,32));
    assert(cfg[61]==0x82 && cfg[68]==2);
    USBD_SetupReqTypedef req={0x81,6,0x2200,1,255};
    assert(pc_setup(dev,&req)==USBD_OK && descriptor_size==25);
    assert(descriptor_data[0]==6 && descriptor_data[2]==0xff);
    req.wIndex=0;assert(pc_setup(dev,&req)==USBD_OK && legacy_setup_calls==1);
    req={0x21,9,0x0200,1,65};assert(pc_setup(dev,&req)==USBD_FAIL);
    req.wLength=63;assert(pc_setup(dev,&req)==USBD_FAIL);
    req.wLength=64;assert(pc_setup(dev,&req)==USBD_OK);
    uint16_t physical[8]={1500,1500,988,1500,988,988,988,988},out[8];
    pc_control_step(physical,physical,out,0,1);physical[6]=2012;
    pc_control_step(physical,physical,out,1,1);
    memset(control_buffer,0,64);control_buffer[0]='P';control_buffer[1]='C';control_buffer[2]=1;
    control_buffer[3]=1;control_buffer[6]=0x2c;control_buffer[8]=100;
    for(unsigned i=0;i<8;++i) put16(control_buffer+10+i*2,1500);
    auto accepted=pcControl.accepted;
    received_size=63;
    assert(rxready(dev)==USBD_OK && pcControl.accepted==accepted);
    assert(pc_setup(dev,&req)==USBD_OK);
    received_size=64;
    assert(rxready(dev)==USBD_OK && pcControl.accepted==accepted+1);
    memcpy(received_buffer,control_buffer,64);received_buffer[4]=1;received_size=64;
    assert(dataout(dev,2)==USBD_OK && pcControl.accepted==accepted+2);
    received_size=5;assert(dataout(dev,2)==USBD_OK && pcControl.accepted==accepted+2);
    dataout(dev,1);datain(dev,1);assert(legacy_out_calls==1 && legacy_in_calls==1);
    ticks=100;USB_PC_Poll();assert(sent[3]==0x21 && busy && sent[36]==2);
    auto oldcalls=tx_calls;USB_PC_Poll();assert(tx_calls==oldcalls);datain(dev,2);
    elrsTelemetry.frames=10;elrsTelemetry.length=64;
    for(unsigned i=0;i<64;++i) elrsTelemetry.frame[i]=i;
    ticks=102;USB_PC_Poll();assert(sent[3]==0x20 && sent[8]==64 && sent[9]==0 && sent[10]==53);
    assert(!memcmp(sent+11,elrsTelemetry.frame,53));datain(dev,2);
    ticks=104;USB_PC_Poll();assert(sent[9]==53 && sent[10]==11);
    assert(!memcmp(sent+11,elrsTelemetry.frame+53,11));datain(dev,2);
    channelData[4]=1500; ticks=300;USB_PC_Poll();assert(sent[36]==1);datain(dev,2);
    channelData[4]=0; ticks=400;USB_PC_Poll();assert(sent[36]==0);datain(dev,2);
    assert(deinit(dev,1)==USBD_OK && pcControl.locked && !pcControl.valid);
    assert(!dev->ep_in[2].is_used && !dev->ep_out[2].is_used);
}
