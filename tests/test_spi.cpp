/* Exercise the real read-command implementation, with only the STM32 SPI
 * peripheral mocked. The old transmit-only implementation fails this test. */
#include <cassert>
#include <cstdint>
#include <cstring>
#include "sx1280reg.h"
namespace spi_test {
static unsigned hspi2, SPI2_NSS_GPIO_Port, SPI2_NSS_Pin;
enum {GPIO_PIN_RESET, GPIO_PIN_SET, HAL_OK, HAL_ERROR};
static bool selected, failTransfer;
static unsigned readCalls;
static void SX1280_HalWaitOnBusy() {}
static void HAL_GPIO_WritePin(unsigned,unsigned,unsigned level){selected=level==GPIO_PIN_RESET;}
static int HAL_SPI_Transmit(unsigned *,uint8_t *,unsigned,unsigned){return HAL_OK;}
static int HAL_SPI_TransmitReceive(unsigned *,uint8_t *tx,uint8_t *rx,unsigned n,unsigned){
    assert(selected); ++readCalls;
    if(failTransfer) return HAL_ERROR;
    assert(n>=3 && tx[1]==0);
    memset(rx,0,n);
    if(tx[0]==SX1280_RADIO_GET_IRQSTATUS){assert(n==4);rx[2]=0;rx[3]=1;}
    else if(tx[0]==SX1280_RADIO_GET_RXBUFFERSTATUS){assert(n==4);rx[2]=8;rx[3]=0x42;}
    else if(tx[0]==SX1280_RADIO_GET_PACKETSTATUS){assert(n==4);rx[2]=160;rx[3]=0xf4;}
    else if(tx[0]==SX1280_RADIO_GET_STATUS){assert(n==3);rx[0]=0xA2;}
    else assert(false);
    return HAL_OK;
}
#include "spi.inc"
}
void test_spi(){
    using namespace spi_test;
    uint8_t response[2]={0xa5,0x5a};
    SX1280_HalReadCommand(SX1280_RADIO_GET_IRQSTATUS,response,2);
    assert(response[0]==0 && response[1]==1 && readCalls==1 && !selected);
    SX1280_HalReadCommand(SX1280_RADIO_GET_RXBUFFERSTATUS,response,2);
    assert(response[0]==8 && response[1]==0x42 && !selected);
    SX1280_HalReadCommand(SX1280_RADIO_GET_PACKETSTATUS,response,2);
    assert(response[0]==160 && response[1]==0xf4 && !selected);
    SX1280_HalReadCommand(SX1280_RADIO_GET_STATUS,response,1);
    assert(response[0]==0xA2 && !selected);
    failTransfer=true;
    SX1280_HalReadCommand(SX1280_RADIO_GET_IRQSTATUS,response,2);
    assert(response[0]==0 && response[1]==0 && !selected);
}
