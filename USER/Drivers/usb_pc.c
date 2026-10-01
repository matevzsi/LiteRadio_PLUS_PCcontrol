/* Additional HID interface. Interface 0 and its report descriptor remain legacy. */
#include "usb_pc.h"
#include "pc_control.h"
#include "usbd_customhid.h"
#include "usbd_ctlreq.h"
#include "stm32f1xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "elrs_v3.h"
#include "crsf.h"
#include "common.h"
#include "status.h"
#include "radiolink.h"
#include <string.h>

extern USBD_HandleTypeDef hUsbDeviceFS;
static uint8_t rx[64], control_rx[64], tx[64];
static volatile uint8_t busy, control_pending;
static uint8_t idle, protocol, alternate;
static uint8_t config[73];
static uint8_t frame[64], frame_length, frame_offset;
static uint32_t frame_number, seen_frame, dropped_frames, last_status;
/* Unnumbered reports, vendor page FF00 / usage 1, 64-byte IN and OUT. */
static uint8_t report_descriptor[] = {
    0x06,0x00,0xFF, 0x09,0x01, 0xA1,0x01,
    0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x40,
    0x09,0x01, 0x81,0x02, 0x09,0x02, 0x91,0x02, 0xC0
};
static uint8_t interface_descriptor[] = {
    9,4,1,0,2,3,0,0,0,
    9,0x21,0x11,0x01,0,1,0x22,sizeof(report_descriptor),0,
    7,5,0x82,3,64,0,2,
    7,5,0x02,3,64,0,2
};
static uint8_t *configuration(uint16_t *length)
{
    uint16_t legacy_length;
    uint8_t *legacy = USBD_CUSTOM_HID.GetFSConfigDescriptor(&legacy_length);
    memcpy(config, legacy, 41);
    memcpy(config + 41, interface_descriptor, sizeof(interface_descriptor));
    config[2] = sizeof(config); config[3] = 0; config[4] = 2;
    *length = sizeof(config);
    return config;
}
static uint8_t *qualifier(uint16_t *length)
{ return USBD_CUSTOM_HID.GetDeviceQualifierDescriptor(length); }
static uint8_t init(USBD_HandleTypeDef *dev, uint8_t cfg)
{
    uint8_t result = USBD_CUSTOM_HID.Init(dev,cfg);
    if (result != USBD_OK) return result;
    busy = control_pending = frame_length = 0;
    idle = alternate = 0; protocol = 1;
    seen_frame = elrsTelemetry.frames;
    pc_control_disconnect();
    USBD_LL_OpenEP(dev,0x82,USBD_EP_TYPE_INTR,64);
    USBD_LL_OpenEP(dev,0x02,USBD_EP_TYPE_INTR,64);
    dev->ep_in[2].is_used = dev->ep_out[2].is_used = 1;
    return USBD_LL_PrepareReceive(dev,0x02,rx,sizeof(rx));
}
static uint8_t deinit(USBD_HandleTypeDef *dev, uint8_t cfg)
{
    pc_control_disconnect();
    busy = control_pending = frame_length = 0;
    USBD_LL_CloseEP(dev,0x82); USBD_LL_CloseEP(dev,0x02);
    dev->ep_in[2].is_used = dev->ep_out[2].is_used = 0;
    return USBD_CUSTOM_HID.DeInit(dev,cfg);
}
static uint8_t pc_setup(USBD_HandleTypeDef *dev, USBD_SetupReqTypedef *req)
{
    static uint16_t status;
    control_pending = 0; /* A new setup aborts any previous control transfer. */
    if (dev->pClassData)
        ((USBD_CUSTOM_HID_HandleTypeDef *)dev->pClassData)->IsReportAvailable=0;
    if (req->wIndex == 0) {
        if (req->bRequest == CUSTOM_HID_REQ_SET_REPORT &&
            (req->bmRequest & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_CLASS &&
            (req->wLength > 64 || !req->wLength)) goto stall;
        return USBD_CUSTOM_HID.Setup(dev,req);
    }
    if (req->wIndex != 1) goto stall;
    if ((req->bmRequest & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_STANDARD) {
        switch (req->bRequest) {
        case USB_REQ_GET_DESCRIPTOR:
            if (req->wValue == (0x22U << 8))
                return USBD_CtlSendData(dev,report_descriptor,MIN(req->wLength,sizeof(report_descriptor)));
            if (req->wValue == (0x21U << 8))
                return USBD_CtlSendData(dev,interface_descriptor+9,MIN(req->wLength,9));
            break;
        case USB_REQ_GET_INTERFACE: return USBD_CtlSendData(dev,&alternate,MIN(req->wLength,1));
        case USB_REQ_SET_INTERFACE: if (!req->wValue) return USBD_OK; break;
        case USB_REQ_GET_STATUS: return USBD_CtlSendData(dev,(uint8_t *)&status,MIN(req->wLength,2));
        }
    } else if ((req->bmRequest & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_CLASS) {
        switch (req->bRequest) {
        case CUSTOM_HID_REQ_SET_REPORT:
            if (req->bmRequest != 0x21 || req->wValue != 0x0200 || req->wLength != 64) break;
            control_pending = 1;
            return USBD_CtlPrepareRx(dev,control_rx,sizeof(control_rx));
        case CUSTOM_HID_REQ_SET_IDLE: idle = req->wValue >> 8; return USBD_OK;
        case CUSTOM_HID_REQ_GET_IDLE: return USBD_CtlSendData(dev,&idle,MIN(req->wLength,1));
        case CUSTOM_HID_REQ_SET_PROTOCOL: if (req->wValue == 1) return USBD_OK; break;
        case CUSTOM_HID_REQ_GET_PROTOCOL: return USBD_CtlSendData(dev,&protocol,MIN(req->wLength,1));
        }
    }
stall:
    USBD_CtlError(dev,req);
    return USBD_FAIL;
}
static uint8_t rxready(USBD_HandleTypeDef *dev)
{
    if (control_pending) {
        control_pending = 0;
        if (USBD_LL_GetRxDataSize(dev,0) == sizeof(control_rx))
            pc_control_receive(control_rx,sizeof(control_rx),HAL_GetTick());
        return USBD_OK;
    }
    return USBD_CUSTOM_HID.EP0_RxReady(dev);
}
static uint8_t datain(USBD_HandleTypeDef *dev, uint8_t endpoint)
{
    if (endpoint == 2) { busy = 0; return USBD_OK; }
    return USBD_CUSTOM_HID.DataIn(dev,endpoint);
}
static uint8_t dataout(USBD_HandleTypeDef *dev, uint8_t endpoint)
{
    if (endpoint != 2) return USBD_CUSTOM_HID.DataOut(dev,endpoint);
    pc_control_receive(rx,USBD_LL_GetRxDataSize(dev,endpoint),HAL_GetTick());
    return USBD_LL_PrepareReceive(dev,0x02,rx,sizeof(rx));
}
USBD_ClassTypeDef USBD_PC_COMPOSITE = {
    init,deinit,pc_setup,NULL,rxready,datain,dataout,NULL,NULL,NULL,
    configuration,configuration,configuration,qualifier
};
static void put16(uint8_t *p,uint16_t value) { p[0]=value; p[1]=value>>8; }
static void put32(uint8_t *p,uint32_t value) { put16(p,value); put16(p+2,value>>16); }

/* Called by the USB/joystick task, never the RF interrupt. At most one transfer. */
void USB_PC_Poll(void)
{
    uint8_t chunk = 0;
    uint32_t now = HAL_GetTick();
    taskENTER_CRITICAL();
    if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED || busy) {
        taskEXIT_CRITICAL(); return;
    }
    if (!frame_length && elrsTelemetry.frames != seen_frame) {
        if (elrsTelemetry.frames > seen_frame + 1) dropped_frames += elrsTelemetry.frames - seen_frame - 1;
        seen_frame = frame_number = elrsTelemetry.frames;
        frame_length = elrsTelemetry.length;
        frame_offset = 0;
        memcpy(frame,elrsTelemetry.frame,frame_length);
    }
    memset(tx,0,sizeof(tx)); tx[0]='P'; tx[1]='C'; tx[2]=1;
    if (frame_length && now-last_status < 100) {
        tx[3]=0x20; put32(tx+4,frame_number); tx[8]=frame_length; tx[9]=frame_offset;
        chunk=frame_length-frame_offset; if (chunk>53) chunk=53;
        tx[10]=chunk; memcpy(tx+11,frame+frame_offset,chunk);
    } else if (now-last_status >= 100) {
        tx[3]=0x21; tx[4]=pcControl.state; tx[5]=pcControl.permit;
        put16(tx+6,pcControl.sequence); put16(tx+8,pcControl.watchdog_ms);
        put32(tx+10,pcControl.accepted); put32(tx+14,pcControl.rejected);
        put32(tx+18,dropped_frames); tx[22]=connectionState;
        memcpy(tx+23,&linkStatistics,sizeof(linkStatistics));
        tx[33]=pcControl.locked; tx[34]=pcControl.mask; tx[35]=Status_RadioPowered();
        /* 0 = unavailable, 1 = disarm command, 2 = arm command. Not FC confirmation. */
        if (tx[35] && channelData[4]>=1000 && channelData[4]<=2000)
            tx[36]=channelData[4]>1500 ? 2 : 1;
    } else { taskEXIT_CRITICAL(); return; }
    busy=1;
    if (USBD_LL_Transmit(&hUsbDeviceFS,0x82,tx,sizeof(tx)) != USBD_OK) busy=0;
    else if (chunk) {
        frame_offset+=chunk;
        if (frame_offset==frame_length) frame_length=0;
    } else last_status=now;
    taskEXIT_CRITICAL();
}
