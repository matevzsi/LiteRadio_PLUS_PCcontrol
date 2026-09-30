#include "common.h"
#include "radiolink.h"
#include "function.h"
#include "crsf.h"
#include "tim.h"
#include "stmflash.h"
#include "stdbool.h"
#include "elrs_v3.h"
#if defined(Regulatory_Domain_ISM_2400)  
#include "sx1280.h"
#include "fhss.h"
#include "mixes.h"
#define Radio SX1280
#define Radio_Config SX1280_Config
#define RXnb SX1280_RXnb
#define TXnb SX1280_TXnb
#define SetFrequencyReg SX1280_SetFrequencyReg
#elif defined(Regulatory_Domain_EU_868) || defined(Regulatory_Domain_FCC_915)
#include "sx1276.h"
#include "fhss.h"
#define Radio SX1276
#define Radio_Config SX1276_Config
#define RXnb SX1276_RXnb
#define TXnb SX1276_TXnb
#define SetFrequencyReg SX1276_SetFrequencyReg
#endif

#define RX_CONNECTION_LOST_TIMEOUT 3000LU // After 3000ms of no TLM response consider that slave has lost connection

volatile uint8_t NonceTX;
crsfLinkStatistics_t linkStatistics;

//// MSP Data Handling ///////
uint8_t NextPacketIsMspData = 0;  // if true the next packet will contain the msp data

////////////SYNC PACKET/////////
/// sync packet spamming on mode change vars ///
#define syncSpamAResidualTimeMS 500 // we spam some more after rate change to help link get up to speed
#define syncSpamAmount 3
volatile uint8_t syncSpamCounter = 0;
uint32_t rfModeLastChangedMS = 0;
////////////////////////////////////////////////

uint32_t SyncPacketLastSent = 0;

volatile uint32_t LastTLMpacketRecvMillis = 0;
uint32_t TLMpacketReported = 0;

//LQCALC<10> LQCalc;
//LPF LPD_DownlinkLQ(1);

volatile uint8_t busyTransmitting;
volatile uint8_t UpdateParamReq = 0;
uint32_t HWtimerPauseDuration = 0;

volatile uint8_t WaitRXresponse = 0; /* 0=TX, 1=pre-RX, 2=telemetry slot */
static uint32_t telemetryWindow;
static uint8_t telemetrySlots;
uint8_t WaitEepromCommit = 0;

volatile uint8_t InBindingMode = 0;
uint8_t BindingPackage[5];
volatile uint8_t BindingSendCount = 0;

uint16_t firmwareRev;

uint8_t baseMac[6];
uint16_t inCRC;
bool MasterUidUseChipIDFlag = true;
void ProcessTLMpacket()
{
    uint8_t *packet = Radio.radioRXdataBuffer;
    uint16_t received = ((uint16_t)(packet[0] & 0xFC) << 6) | packet[7];
    packet[0] &= 3;
    if (InBindingMode || !WaitRXresponse || (telemetryWindow & 1U) || packet[0] != TLM_PACKET ||
        received != calcCrc14(packet, 7, CRCInitializer)) return;
    connectionState = connected;
    LastTLMpacketRecvMillis = HAL_GetTick();
    telemetryWindow |= 1U;
    linkStatistics.downlink_SNR = Radio.LastPacketSNR;
    /* CRSF wire RSSI fields are positive magnitudes of negative dBm. */
    linkStatistics.downlink_RSSI = (uint8_t)(-Radio.LastPacketRSSI);
    if ((packet[1] & 3) == ELRS_TELEMETRY_TYPE_LINK) {
        linkStatistics.uplink_RSSI_1 = packet[2] & 0x7F;
        linkStatistics.uplink_RSSI_2 = packet[3] & 0x7F;
        linkStatistics.active_antenna = packet[2] >> 7;
        linkStatistics.uplink_Link_quality = packet[4] & 0x7F;
        linkStatistics.uplink_SNR = (int8_t)packet[5] / 4;
    } else if ((packet[1] & 3) == ELRS_TELEMETRY_TYPE_DATA) {
        elrs_v3_telemetry_receive(packet[1] >> 2, &packet[2]);
    }
}

void GenerateSyncPacketData()
{
#if !defined(Regulatory_Domain_ISM_2400)
    const uint8_t SwitchEncMode = 0x01;
#endif
    uint8_t Index;
    if (syncSpamCounter)
    {
        Index = (tx_config.rate & 0x03);
//        TLMrate = (tx_config.tlm & 0x07);
    }
    else
    {
        Index = (ExpressLRS_currAirRate_Modparams->index & 0x03);
//        TLMrate = (ExpressLRS_currAirRate_Modparams->TLMinterval & 0x07);
    }
    expresslrs_tlm_ratio_e newRatio = (StubbornSender_IsActive()) ? TLM_RATIO_1_2 : (expresslrs_tlm_ratio_e)tx_config.tlm;

    if (connectionState == connected && ExpressLRS_currAirRate_Modparams->TLMinterval < newRatio)
    LastTLMpacketRecvMillis = SyncPacketLastSent;
    ExpressLRS_currAirRate_Modparams->TLMinterval = newRatio;
    
    Radio.radioTXdataBuffer[0] = SYNC_PACKET & 0x03;
    Radio.radioTXdataBuffer[1] = FHSSgetCurrIndex();
    Radio.radioTXdataBuffer[2] = NonceTX;
#if defined(Regulatory_Domain_ISM_2400)
    Radio.radioTXdataBuffer[3] = elrs_v3_sync_config(Index, newRatio);
#else
    Radio.radioTXdataBuffer[3] = (Index << 6) + (newRatio << 3) + (SwitchEncMode << 1);
#endif
    Radio.radioTXdataBuffer[4] = UID[3];
    Radio.radioTXdataBuffer[5] = UID[4];
    Radio.radioTXdataBuffer[6] = UID[5];
  
    SyncPacketLastSent = HAL_GetTick();
    if (syncSpamCounter)
        --syncSpamCounter;
}

void SetRFLinkRate(uint8_t index) // Set speed of RF link (hz)
{
    expresslrs_mod_settings_s * ModParams = get_elrs_airRateConfig(index);
    expresslrs_rf_pref_params_s * RFperf = get_elrs_RFperfParams(index);
    uint8_t invertIQ = InBindingMode || (UID[5] & 0x01);
    if ((ModParams == ExpressLRS_currAirRate_Modparams)
        && (RFperf == ExpressLRS_currAirRate_RFperfParams)
        && (invertIQ == Radio.IQinverted))
    return;
    TIM1->ARR = ModParams->interval - 1;
    TIM1->EGR = TIM_EGR_UG;
    __HAL_TIM_CLEAR_FLAG(&htim1, TIM_FLAG_UPDATE);
    Radio_Config(ModParams->bw, ModParams->sf, ModParams->cr, GetInitialFreq(), ModParams->PreambleLen, invertIQ);

    ExpressLRS_currAirRate_Modparams = ModParams;
    ExpressLRS_currAirRate_RFperfParams = RFperf;

    FHSSsetCurrIndex(0);
    NonceTX = 0;
    WaitRXresponse = 0;
    telemetryWindow = 0;
    telemetrySlots = 0;
    linkStatistics.downlink_Link_quality = 0;
    /* CRSF RF-mode enum, distinct from the sync packet rate index. */
    { static const uint8_t modes[4] = {9, 7, 5, 2};
      linkStatistics.rf_Mode = modes[index < RATE_MAX ? index : RATE_DEFAULT]; }
    connectionState = disconnected;
    rfModeLastChangedMS = HAL_GetTick();
}

void HandleFHSS()
{
//    if (InBindingMode)
//    {
//        return;
//    }

//    uint8_t modresult = (NonceTX+1) % ExpressLRS_currAirRate_Modparams->FHSShopInterval;

//    if (modresult == 0) // if it time to hop, do so.
//    {
//        SetFrequencyReg(FHSSgetNextFreq());
//    }
      uint8_t modresult = (NonceTX + 1) % ExpressLRS_currAirRate_Modparams->FHSShopInterval;
      // If the next packet should be on the next FHSS frequency, do the hop
      if ((InBindingMode == 0) && (modresult == 0))
      {
        SetFrequencyReg(FHSSgetNextFreq());
      }
}

void HandleTLM()
{
    uint8_t denom = TLMratioEnumToValue(ExpressLRS_currAirRate_Modparams->TLMinterval);
    if (!InBindingMode && denom > 1 && ((uint8_t)(NonceTX + 1) % denom) == 0) {
        RXnb();
        WaitRXresponse = 1;
        telemetryWindow <<= 1;
        if (telemetrySlots < 32) ++telemetrySlots;
    }
}

void Rate_Modify(uint8_t index)
{
    tx_config.rate = index;
    syncSpamCounter = syncSpamAmount;
    tx_config.modify = 1;
}


uint16_t SendRCdataToRF(uint16_t* crsfcontrol_data)
{

    if (InBindingMode && BindingSendCount >= 7) return 0;
    if (!InBindingMode) NonceTX++;
    if (WaitRXresponse == 1) {
        WaitRXresponse = 2;
        return 0; /* Reserve a full slot, even if RX already completed. */
    }
    if (WaitRXresponse == 2) {
        uint32_t window = telemetryWindow;
        uint8_t received = 0;
        while (window) { received += window & 1U; window >>= 1; }
        linkStatistics.downlink_Link_quality = telemetrySlots ? received * 100 / telemetrySlots : 0;
        WaitRXresponse = 0;
    }
    busyTransmitting = 1;
    uint32_t now = HAL_GetTick();
    static uint8_t syncSlot;
    uint32_t SyncInterval;

    SyncInterval = (connectionState == connected) ? ExpressLRS_currAirRate_RFperfParams->SyncPktIntervalConnected : ExpressLRS_currAirRate_RFperfParams->SyncPktIntervalDisconnected;

    bool skipSync = InBindingMode;

    uint8_t NonceFHSSresult = NonceTX % ExpressLRS_currAirRate_Modparams->FHSShopInterval;
//    uint8_t NonceFHSSresultWindow = (NonceFHSSresult == 1 || NonceFHSSresult == 2) ? 1 : 0; // restrict to the middle nonce ticks (not before or after freq chance)
    uint8_t WithinSyncSpamResidualWindow = (HAL_GetTick() - rfModeLastChangedMS < syncSpamAResidualTimeMS) ? 1 : 0;
    if(!skipSync && (syncSpamCounter || WithinSyncSpamResidualWindow) &&  (NonceFHSSresult == 1 || NonceFHSSresult == 2))
    {  
        GenerateSyncPacketData();
        syncSlot = 0;
    }
    else if((!skipSync) && ((syncSlot / 2) <= NonceFHSSresult) && (now - SyncPacketLastSent > SyncInterval) && (Radio.currFreq == GetInitialFreq())) // don't sync just after we changed freqs (helps with hwTimer.init() being in sync from the get go)
    {
//		 //在发送补偿包的时候，不知道为什么，定时器中断时间会提前中断？需要加延时校准，否者在连接betaflight SPI接收机时 接收端会跳频混乱，后续应该会改善。
//		if(tx_config.rate == 0x01)  //250Hz
//		{
//			delay(2000);     
//		}
//		if(tx_config.rate == 0x02)  //150Hz
//		{
//			delay(1000);     
//		}
//		if(tx_config.rate == 0x00)  //500Hz
//		{
//			delay(3000);     
//		}
        GenerateSyncPacketData();
        syncSlot = (syncSlot + 1) % (ExpressLRS_currAirRate_Modparams->FHSShopInterval * 2);
    }
    else
    {
        if(NextPacketIsMspData && StubbornSender_IsActive())
        {
            uint8_t *data;
            uint8_t maxLength;
            uint8_t packageIndex;
            StubbornSender_GetCurrentPayload(&packageIndex, &maxLength, &data);
            
            Radio.radioTXdataBuffer[0] = MSP_DATA_PACKET & 0x03;
            Radio.radioTXdataBuffer[1] = packageIndex | (elrsTelemetry.ack << 7);
            Radio.radioTXdataBuffer[2] = maxLength > 0 ? *data : 0;
            Radio.radioTXdataBuffer[3] = maxLength > 1 ? *(data + 1) : 0;
            Radio.radioTXdataBuffer[4] = maxLength > 2 ? *(data + 2) : 0;
            Radio.radioTXdataBuffer[5] = maxLength > 3 ? *(data + 3) : 0;
            Radio.radioTXdataBuffer[6] = maxLength > 4 ? *(data + 4) : 0;
            // send channel data next so the channel messages also get sent during msp transmissions
            NextPacketIsMspData = 0;
            // counter can be increased even for normal msp messages since it's reset if a real bind message should be sent
            BindingSendCount++;
            if (ExpressLRS_currAirRate_Modparams->TLMinterval != TLM_RATIO_1_2)
                syncSpamCounter = 1;
            /* The task exits binding only after the final TX completion. */
        }
        else
        {
            // always enable msp after a channel package since the slot is only used if MspSender has data to send
            NextPacketIsMspData = 1;
            GenerateChannelDataHybridSwitch8(Radio.radioTXdataBuffer, crsfcontrol_data);
        }
    }

    ///// Next, Calculate the CRC and put it into the buffer /////

    uint16_t crc = calcCrc14(Radio.radioTXdataBuffer, 7, CRCInitializer);
    Radio.radioTXdataBuffer[0] =(Radio.radioTXdataBuffer[0] & 0x03) | ((crc >> 6) & 0xFC);
    Radio.radioTXdataBuffer[7] = crc & 0xFF;

    TXnb(Radio.radioTXdataBuffer, 8);
    
    return 0;
}

/*
 * Called as the timer ISR when transmitting
 */
void timerCallbackNormal()
{
// Do not send a stale channels packet to the RX if one has not been received from the handset
// *Do* send data if a packet has never been received from handset and the timer is running
//     this is the case when bench testing and TXing without a handset
//  uint32_t lastRcData = crsf.GetRCdataLastRecv();
//  if (!lastRcData || (micros() - lastRcData < 1000000))
//  {
//    busyTransmitting = 1;
//    SendRCdataToRF();
//  }
}

/*
 * Called as the timer ISR while waiting for eeprom flush
 */
void timerCallbackIdle()
{
    NonceTX++;
    if (NonceTX % ExpressLRS_currAirRate_Modparams->FHSShopInterval == 0)
    {
        FHSSptr++;
    }
}


void ParamUpdateReq()
{
    UpdateParamReq = 1;
}



void RXdoneISR()
{
    ProcessTLMpacket();
    busyTransmitting = 0;
}

void TXdoneISR()
{
    if (!busyTransmitting) return;
    HandleFHSS();
    HandleTLM();
    busyTransmitting = 0;
}

void ExpressLRS_Init(uint8_t protocolIndex)
{
    HAL_Delay(1);
}


void setup(void)
{
    uint16_t readTemp[1];
    STMFLASH_Read(MasterUidUseChipIDFlag_ADDR,&readTemp[0],1);
    MasterUidUseChipIDFlag = readTemp[0];
    if(MasterUidUseChipIDFlag)
    {
        /*获取芯片ID作为连接ID*/
        Get_CRSFUniqueID(MasterUID);
    }
    else
    {
        /*获取绑定短语生成的ID作为连接ID*/
        uint16_t readTemp[6];
        STMFLASH_Read(MasterID1FromBindPhrase_ADDR,&readTemp[0],1);
        STMFLASH_Read(MasterID2FromBindPhrase_ADDR,&readTemp[1],1);
        STMFLASH_Read(MasterID3FromBindPhrase_ADDR,&readTemp[2],1);
        STMFLASH_Read(MasterID4FromBindPhrase_ADDR,&readTemp[3],1);
        STMFLASH_Read(MasterID5FromBindPhrase_ADDR,&readTemp[4],1);
        STMFLASH_Read(MasterID6FromBindPhrase_ADDR,&readTemp[5],1);
        MasterUID[0] = readTemp[0]&0xff;
        MasterUID[1] = readTemp[1]&0xff;
        MasterUID[2] = readTemp[2]&0xff;
        MasterUID[3] = readTemp[3]&0xff;
        MasterUID[4] = readTemp[4]&0xff;
        MasterUID[5] = readTemp[5]&0xff;
    }
    
    UID[0] = MasterUID[0];
    UID[1] = MasterUID[1];
    UID[2] = MasterUID[2];
    UID[3] = MasterUID[3];
    UID[4] = MasterUID[4];
    UID[5] = MasterUID[5];
    
    CRCInitializer = elrs_v3_crc_init(UID);
    FHSSrandomiseFHSSsequence(uidMacSeedGet()); 
#if !defined(Regulatory_Domain_ISM_2400)
    Radio.currSyncWord = UID[3];
#endif    
    
#if defined(Regulatory_Domain_ISM_2400)       
    SX1280_Reset();
    while(firmwareRev == 0|| (firmwareRev == 65535))
    {
        firmwareRev= SX1280_GetFirmwareVersion();
    }

    SX1280_Init();
    SX1280_SetPower((PowerLevels_e)DefaultPowerEnum);
    tx_config.lastPower = 2;
    SetRFLinkRate(RATE_DEFAULT);
    generateCrc14Table();
    tx_config.rate = RATE_DEFAULT;
    tx_config.lastRate = RATE_DEFAULT;
#elif defined(Regulatory_Domain_EU_868) || defined(Regulatory_Domain_FCC_915)
    SX1276_Init();
    SX1276_Reset();
    if (SX1276_DetectChip())
    {
        SX1276_ConfigLoraDefaults();
    }
    else
    {
        while(1);
    }
    SX1276_SetOutputPower((PowerLevels_e)DefaultPowerEnum);
    SetRFLinkRate(RATE_DEFAULT);
    generateCrc14Table();
    tx_config.rate = 1;
    tx_config.lastRate = 1;
    TIM1->ARR = 5000;
#endif    
    uint16_t txConfigInit[3]; 
    STMFLASH_Read(INTERNAL_ELRS_CONFIGER_INFO_ADDR,txConfigInit,3);
    
    /*内置高频头参数自检*/
    if(2 < txConfigInit[0])
    {
        txConfigInit[0] = 2;
        STMFLASH_Write(INTERNAL_ELRS_CONFIGER_INFO_POWER_ADDR,&txConfigInit[0],1);
        
    }
    if(RATE_MAX <= txConfigInit[1])
    {
        txConfigInit[1] = 2;
        STMFLASH_Write(INTERNAL_ELRS_CONFIGER_INFO_Rate_ADDR,&txConfigInit[1],1);
    }
    if(7 < txConfigInit[2])
    {
        txConfigInit[2] = TLM_RATIO_1_8;
        STMFLASH_Write(INTERNAL_ELRS_CONFIGER_INFO_TLM_ADDR,&txConfigInit[2],1);
    }
    tx_config.power = (uint32_t)txConfigInit[0];

    tx_config.rate = (uint32_t)txConfigInit[1];
    
    tx_config.tlm = (uint32_t)txConfigInit[2];
    
    tx_config.lastTLM = tx_config.tlm;
    tx_config.lastRate = tx_config.rate;
    SetRFLinkRate(tx_config.rate);
    ExpressLRS_currAirRate_Modparams->TLMinterval = (expresslrs_tlm_ratio_e)tx_config.tlm;
    elrs_v3_telemetry_reset();
}

#if defined(Regulatory_Domain_ISM_2400)  

/* Stop the scheduler before task-side SPI/configuration or flash writes.
 * RF IRQs remain enabled while waiting; a missing DIO cannot deadlock the task.
 * Caller holds the FreeRTOS critical section until ResumeRF(). */
static void PauseRF(void)
{
    uint32_t started = HAL_GetTick();
    HAL_TIM_Base_Stop_IT(&htim1);
    while (busyTransmitting && HAL_GetTick() - started < 50) vTaskDelay(1);
    taskENTER_CRITICAL();
    SX1280_SetMode(SX1280_MODE_FS);
    SX1280_ClearIrqStatus(SX1280_IRQ_RADIO_ALL);
    busyTransmitting = 0;
    WaitRXresponse = 0;
}

static void ResumeRF(void)
{
    TIM1->CNT = 0;
    TIM1->EGR = TIM_EGR_UG;
    __HAL_TIM_CLEAR_FLAG(&htim1, TIM_FLAG_UPDATE);
    HAL_NVIC_ClearPendingIRQ(TIM1_UP_IRQn);
    HAL_NVIC_ClearPendingIRQ(EXTI15_10_IRQn);
    HAL_TIM_Base_Start_IT(&htim1);
    taskEXIT_CRITICAL();
}

uint16_t SX1280_Process(uint16_t* controlDataBuff)
{
    uint8_t i;
    /* Publish all channels together; the timer must not see a mixed sample. */
    taskENTER_CRITICAL();
    for (i = 0; i < 8; ++i) channelData[i] = controlDataBuff[i];
    taskEXIT_CRITICAL();
    if (InBindingMode) {
        if (BindingSendCount >= 7 && !busyTransmitting) ExitBindingMode();
        return 0;
    }
    if (connectionState == connected && HAL_GetTick() - LastTLMpacketRecvMillis > RX_CONNECTION_LOST_TIMEOUT) {
        connectionState = disconnected;
        linkStatistics.uplink_Link_quality = 0;
        linkStatistics.downlink_Link_quality = 0;
    }
    /* USB configurator settings are untrusted array indices. */
    if (tx_config.rate >= RATE_MAX) tx_config.rate = RATE_DEFAULT;
    if (tx_config.tlm > TLM_RATIO_1_2) tx_config.tlm = TLM_RATIO_1_8;
    if (tx_config.power > 2) tx_config.power = 2;
    if (tx_config.rate != tx_config.lastRate && !tx_config.modify) Rate_Modify(tx_config.rate);
    if (tx_config.modify && syncSpamCounter) return 0;
    if (tx_config.modify || tx_config.tlm != tx_config.lastTLM || tx_config.power != tx_config.lastPower) {
        PauseRF();
        /* Configuration changes can interrupt RF while STM32 flash is erased.
         * Configure on the bench; re-start a coherent nonce/FHSS epoch. */
        if (tx_config.rate != tx_config.lastRate) {
            tx_config.lastRate = tx_config.rate;
            STMFLASH_Write(INTERNAL_ELRS_CONFIGER_INFO_Rate_ADDR, (uint16_t *)&tx_config.rate, 1);
        }
        if (tx_config.tlm != tx_config.lastTLM) {
            tx_config.lastTLM = tx_config.tlm;
            STMFLASH_Write(INTERNAL_ELRS_CONFIGER_INFO_TLM_ADDR, (uint16_t *)&tx_config.tlm, 1);
        }
        if (tx_config.power != tx_config.lastPower) {
            tx_config.lastPower = tx_config.power;
            SX1280_SetPower((PowerLevels_e)(tx_config.power + PWR_25mW));
            STMFLASH_Write(INTERNAL_ELRS_CONFIGER_INFO_POWER_ADDR, (uint16_t *)&tx_config.power, 1);
        }
        ExpressLRS_currAirRate_Modparams = 0; /* Force reset after flash pause. */
        SetRFLinkRate(tx_config.rate);
        ExpressLRS_currAirRate_Modparams->TLMinterval = (expresslrs_tlm_ratio_e)tx_config.tlm;
        tx_config.modify = 0;
        syncSpamCounter = syncSpamAmount;
        ResumeRF();
    }
    /* CRSF power enum: 25/50/100 mW -> 2/8/3. */
    { static const uint8_t powers[3] = {2, 8, 3};
      linkStatistics.uplink_TX_Power = powers[tx_config.power]; }
    return 0;
}
#elif defined(Regulatory_Domain_EU_868) || defined(Regulatory_Domain_FCC_915)

uint16_t SX1276_Process(uint16_t* controlDataBuff)
{
    channelData[0] = controlDataBuff[0];
    channelData[1] = controlDataBuff[1];
    channelData[2] = controlDataBuff[2];
    channelData[3] = controlDataBuff[3];
    channelData[4] = controlDataBuff[4];
    channelData[5] = controlDataBuff[5];
    channelData[6] = controlDataBuff[6];
    channelData[7] = controlDataBuff[7];
    if(tx_config.modify && (syncSpamCounter == 0) )
    {    
        switch(tx_config.rate)
        {
            case FREQ_900_RATE_200HZ:
                SetRFLinkRate(FREQ_900_RATE_200HZ);
                TIM1->ARR = 5000;
                break;
            case FREQ_900_RATE_100HZ:
                SetRFLinkRate(FREQ_900_RATE_100HZ);
                TIM1->ARR = 10000;
                break;
            case FREQ_900_RATE_50HZ: 
                SetRFLinkRate(FREQ_900_RATE_50HZ);
                TIM1->ARR = 20000;
                break;                    
            case FREQ_900_RATE_25HZ:
                SetRFLinkRate(FREQ_900_RATE_25HZ);
                TIM1->ARR = 40000;
                break;
            default:
                break;
        }
        tx_config.modify = 0;
    }

    if(tx_config.rate != tx_config.lastRate)
    {
        tx_config.lastRate = tx_config.rate;
        Rate_Modify(tx_config.rate);
        STMFLASH_Write(INTERNAL_ELRS_CONFIGER_INFO_Rate_ADDR,(uint16_t *)&tx_config.rate,1);
    }
    if(tx_config.tlm != tx_config.lastTLM)
    {
        tx_config.lastTLM = tx_config.tlm;
        ExpressLRS_currAirRate_Modparams->TLMinterval = (expresslrs_tlm_ratio_e)tx_config.tlm;
        STMFLASH_Write(INTERNAL_ELRS_CONFIGER_INFO_TLM_ADDR,(uint16_t *)&tx_config.tlm,1);
    }
    if(tx_config.power != tx_config.lastPower)
    {
        tx_config.lastPower = tx_config.power;
        switch (tx_config.power)
        {
            case 0:
                SX1276_SetOutputPower((PowerLevels_e)PWR_25mW);
                break;       
            case 1:
                SX1276_SetOutputPower((PowerLevels_e)PWR_50mW);
                break;            
            case 2:
                SX1276_SetOutputPower((PowerLevels_e)PWR_100mW);
                break;            
            default:
                break;
        }
        STMFLASH_Write(INTERNAL_ELRS_CONFIGER_INFO_POWER_ADDR,(uint16_t *)&tx_config.power,1);
    }   
    
    return 0;
}

#endif

//void loop()
//{
//    uint32_t now = HAL_GetTick();
//    
//    if (now > (RX_CONNECTION_LOST_TIMEOUT + LastTLMpacketRecvMillis))
//    {
//        connectionState = disconnected;
//    }
//    else
//    {
//        connectionState = connected;
//    }

//    // only send msp data when binding is not active
//    if (InBindingMode)
//    {
//        // exit bind mode if package after some repeats
//        if (BindingSendCount > 6) 
//        {
//            ExitBindingMode();
//        }
//    }
//}


void EnterBindingMode()
{
    if (InBindingMode) return;
    PauseRF();
    SendUIDOverMSP();
    /* Keep the normal UID/FHSS seed, as upstream does. Binding overrides
     * CRC and IQ, and transmits on the UID-independent sync frequency. */
    CRCInitializer = 0;
    InBindingMode = 1;
    NonceTX = 0;
    NextPacketIsMspData = 1;
    elrs_v3_telemetry_reset();
    ExpressLRS_currAirRate_Modparams = 0;
    SetRFLinkRate(RATE_BINDING);
    ResumeRF();
}

void ExitBindingMode()
{
    if (!InBindingMode) return;
    PauseRF();
    CRCInitializer = elrs_v3_crc_init(UID);
    InBindingMode = 0;
    StubbornSender_ResetState();
    NextPacketIsMspData = 0;
    ExpressLRS_currAirRate_Modparams = 0;
    SetRFLinkRate(tx_config.rate < RATE_MAX ? tx_config.rate : RATE_DEFAULT);
    ExpressLRS_currAirRate_Modparams->TLMinterval = (expresslrs_tlm_ratio_e)tx_config.tlm;
    syncSpamCounter = syncSpamAmount;
    ResumeRF();
}

void SendUIDOverMSP()
{
    BindingPackage[0] = MSP_ELRS_BIND;
    BindingPackage[1] = MasterUID[2];
    BindingPackage[2] = MasterUID[3];
    BindingPackage[3] = MasterUID[4];
    BindingPackage[4] = MasterUID[5];
    StubbornSender_ResetState();
    BindingSendCount = 0;
    StubbornSender_SetDataToTransmit(5, BindingPackage, ELRS_MSP_BYTES_PER_CALL);
    InBindingMode = 1;
}
