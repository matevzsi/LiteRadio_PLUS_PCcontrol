#include "pc_control.h"
#include <string.h>

pc_control_t pcControl = {{0}, PC_WATCHDOG_DEFAULT_MS, 0, 0, 0, 0, 0, 0, PC_MANUAL, PC_DEFAULT_MASK, 1};
static uint16_t le16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }

void pc_control_disconnect(void)
{
    pcControl.valid = 0;
    pcControl.permit = 0;
    pcControl.locked = 1;
    pcControl.state = PC_FAILSAFE;
}

/* Call from USB IRQ, or with that IRQ masked. No allocation, flash or RF work. */
int pc_control_receive(const uint8_t *r, uint16_t size, uint32_t now)
{
    unsigned i;
    uint16_t seq, delta, watchdog;
    if (size != PC_REPORT_SIZE || r[0] != 'P' || r[1] != 'C' || r[2] != 1)
        goto reject;
    /* Reserved bytes must be zero. */
    for (i = 26; i < PC_REPORT_SIZE; ++i) if (r[i]) goto reject;
    if (r[3] == 0) { pc_control_disconnect(); return 1; }
    if (r[3] != 1 || !pcControl.permit || (r[6] & 0x10) || !r[6] || r[7]) goto reject;
    if (pcControl.valid && (uint32_t)(now-pcControl.received_ms) >= pcControl.watchdog_ms) {
        pcControl.locked=1;
        pcControl.permit=0;
        goto reject;
    }
    watchdog = le16(r + 8);
    if (watchdog < 50 || watchdog > 250) goto reject;
    for (i = 0; i < 8; ++i)
        if (le16(r + 10 + 2*i) < 988 || le16(r + 10 + 2*i) > 2012) goto reject;
    seq = le16(r + 4);
    delta = (uint16_t)(seq - pcControl.sequence);
    if (pcControl.valid && (!delta || delta >= 0x8000)) goto reject;
    for (i = 0; i < 8; ++i) pcControl.channel[i] = le16(r + 10 + 2*i);
    pcControl.sequence = seq;
    pcControl.watchdog_ms = watchdog;
    pcControl.mask = r[6];
    pcControl.received_ms = now;
    pcControl.valid = 1;
    ++pcControl.accepted;
    return 1;
reject:
    ++pcControl.rejected;
    return 0;
}

uint16_t pc_control_mix(uint16_t input, int16_t weight, int16_t offset)
{
    /* Weight first, then offset; +/-100% spans 1000..2000 us. */
    int32_t value = 1500 + ((int32_t)input - 1500) * weight / 100 + (int32_t)offset * 5;
    if (value < 1000) value = 1000;
    if (value > 2000) value = 2000;
    return (uint16_t)value;
}

void pc_control_step(const uint16_t physical[8], const uint16_t manual[8],
                     uint16_t output[8], uint32_t now, uint8_t usb_ready)
{
    unsigned i;
    uint16_t thrust = physical[1], steer = physical[0], lift = physical[2];
    memcpy(output, manual, 8 * sizeof(uint16_t));
    if (physical[PC_ENABLE_INPUT] <= 1750) {
        pc_control_disconnect();
        pcControl.locked = 0;
        pcControl.state = PC_MANUAL;
    } else {
        pcControl.permit = usb_ready && !pcControl.locked;
        if (!pcControl.permit || !pcControl.valid ||
            (uint32_t)(now - pcControl.received_ms) >= pcControl.watchdog_ms) {
            /* Require switch off/on after timeout, disconnect or explicit STOP.
             * Retain last sequence so delayed packets cannot restart control. */
            if (pcControl.valid || !usb_ready) {
                pcControl.permit = 0;
                pcControl.locked = 1;
            }
            pcControl.state = PC_FAILSAFE;
            thrust = 1500; steer = 1500; lift = 988;
            for (i = 0; i < 8; ++i)
                if (pcControl.mask & (1U << i)) output[i] = 1500;
        } else {
            pcControl.state = PC_ACTIVE;
            for (i = 0; i < 8; ++i)
                if (pcControl.mask & (1U << i)) output[i] = pcControl.channel[i];
            if (pcControl.mask & (1U << 2)) thrust = pcControl.channel[2];
            if (pcControl.mask & (1U << 3)) steer = pcControl.channel[3];
            if (pcControl.mask & (1U << 5)) lift = pcControl.channel[5];
        }
    }
    output[2] = pc_control_mix(thrust, 200, -100);
    output[3] = pc_control_mix(steer, 100, 0);
    output[4] = physical[5]; /* CH5 always SB, including failsafe. */
    output[5] = pc_control_mix(lift, 100, 0);
    /* FC inputs stay in the nominal -100..100% range, including stick overtravel. */
    for (i=0;i<8;++i) output[i]=pc_control_mix(output[i],100,0);
}
