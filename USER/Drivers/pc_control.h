#ifndef PC_CONTROL_H
#define PC_CONTROL_H
#include <stdint.h>

#define PC_REPORT_SIZE 64U
#define PC_WATCHDOG_DEFAULT_MS 100U
/* Logical input order: Ail, Ele, Thr, Rud, SA, SB, SC, SD. */
#define PC_ENABLE_INPUT 6U
#define PC_DEFAULT_MASK 0x2CU /* CH3, CH4, CH6 */
enum { PC_MANUAL, PC_ACTIVE, PC_FAILSAFE };
typedef struct {
    uint16_t channel[8], watchdog_ms, sequence;
    uint32_t received_ms, accepted, rejected;
    uint8_t valid, permit, state, mask, locked;
} pc_control_t;
extern pc_control_t pcControl;
void pc_control_disconnect(void);
int pc_control_receive(const uint8_t *report, uint16_t size, uint32_t now);
void pc_control_step(const uint16_t physical[8], const uint16_t manual[8],
                     uint16_t output[8], uint32_t now, uint8_t usb_ready);
uint16_t pc_control_mix(uint16_t input, int16_t weight, int16_t offset);
#endif
