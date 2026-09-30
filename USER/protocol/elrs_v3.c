#include "elrs_v3.h"
#include <string.h>

elrs_v3_telemetry_t elrsTelemetry;

uint16_t elrs_v3_crc_init(const uint8_t uid[6])
{
    return (((uint16_t)uid[4] << 8) | uid[5]) ^ ELRS_OTA_VERSION_ID;
}

uint32_t elrs_v3_fhss_seed(const uint8_t uid[6])
{
    return ((uint32_t)uid[2] << 24) | ((uint32_t)uid[3] << 16) |
           ((uint32_t)uid[4] << 8) | (uid[5] ^ ELRS_OTA_VERSION_ID);
}

uint8_t elrs_v3_rate_index(uint8_t local_rate)
{
    static const uint8_t indices[4] = {4, 6, 7, 9};
    return indices[local_rate < 4 ? local_rate : 2];
}

uint8_t elrs_v3_sync_config(uint8_t local_rate, uint8_t tlm_ratio)
{
    return (elrs_v3_rate_index(local_rate) << 4) | ((tlm_ratio & 7) << 1) | 1;
}

/* Preserve the handset's existing 1000..2000 -> 192..1792 calibration,
 * including its extended 988..2012 endpoints, then apply upstream rounding. */
static uint16_t channel_to_crsf(uint16_t us)
{
    int32_t value = ((int32_t)us - 1000) * 1600 / 1000 + 192;
    if (value < 172) value = 172;
    if (value > 1811) value = 1811;
    return (uint16_t)value;
}

static uint8_t crsf_to_n(uint16_t ch, uint8_t count)
{
    if (ch <= 191) return 0;
    if (ch >= 1792) return count - 1;
    return (ch - 191) * count / 1602;
}

void elrs_v3_pack_channels(volatile uint8_t packet[8], const uint16_t channels[8],
                           uint8_t switch_index, uint8_t telemetry_ack)
{
    uint32_t bits = 0;
    uint8_t available = 0, dest = 1, ch;
    uint16_t crsf;
    uint8_t value;
    packet[0] = 0; /* RC type and cleared CRC high bits */
    for (ch = 0; ch < 4; ++ch) {
        crsf = channel_to_crsf(channels[ch]);
        /* ExpressLRS fmap(), rounded rather than simply truncating. */
        uint16_t ten = (((uint32_t)(crsf - 172) * 1023 * 2 / 1639) + 1) / 2;
        bits |= (uint32_t)ten << available;
        available += 10;
        while (available >= 8) {
            packet[dest++] = (uint8_t)bits;
            bits >>= 8;
            available -= 8;
        }
    }
    switch_index %= 7; /* upstream AUX2..AUX8 round robin */
    crsf = switch_index < 3 ? channel_to_crsf(channels[5 + switch_index]) : 992;
    if (switch_index == 6) value = crsf_to_n(crsf, 16);
    else if (crsf >= 924 && crsf <= 1060) value = 7;
    else value = crsf_to_n(crsf, 6);
    packet[6] = ((channel_to_crsf(channels[4]) > 992) << 7) |
                ((telemetry_ack & 1) << 6) | (switch_index << 3) | value;
    packet[7] = 0;
}

static uint8_t crsf_crc(const uint8_t *data, uint8_t length)
{
    uint8_t crc = 0, bit;
    while (length--) {
        crc ^= *data++;
        for (bit = 0; bit < 8; ++bit)
            crc = (crc << 1) ^ ((crc & 0x80) ? 0xD5 : 0);
    }
    return crc;
}

void elrs_v3_telemetry_reset(void)
{
    memset(&elrsTelemetry, 0, sizeof(elrsTelemetry));
    elrsTelemetry.next_package = 1;
}

/* StubbornReceiver 3.5.3 handshake. ACK accepted fragments even when an
 * oversized transfer must be discarded, so the RF transport can recover.
 * The final fragment can contain data and padding. Never publish truncation. */
void elrs_v3_telemetry_receive(uint8_t index, const uint8_t payload[5])
{
    uint8_t i, length;
    if (index == 63) {
        elrsTelemetry.ack ^= 1;
        elrsTelemetry.next_package = 1;
        elrsTelemetry.used = elrsTelemetry.overflow = 0;
        return;
    }
    if (index == 1 && elrsTelemetry.next_package > 1) {
        elrsTelemetry.next_package = 1;
        elrsTelemetry.used = elrsTelemetry.overflow = 0;
    }
    if (index != elrsTelemetry.next_package &&
        !(index == 0 && elrsTelemetry.next_package > 1)) return;
    for (i = 0; i < 5; ++i) {
        if (elrsTelemetry.used < ELRS_V3_CRSF_MAX)
            elrsTelemetry.assembling[elrsTelemetry.used++] = payload[i];
        else if (index != 0) elrsTelemetry.overflow = 1;
        /* Only the final fragment may include padding beyond byte 64. */
    }
    elrsTelemetry.ack ^= 1;
    ++elrsTelemetry.next_package;
    if (index != 0) return;
    length = elrsTelemetry.assembling[1];
    if (!elrsTelemetry.overflow && length >= 2 && length <= ELRS_V3_CRSF_MAX - 2 &&
        (uint16_t)length + 2 <= elrsTelemetry.used &&
        crsf_crc(&elrsTelemetry.assembling[2], length - 1) == elrsTelemetry.assembling[length + 1]) {
        memcpy(elrsTelemetry.frame, elrsTelemetry.assembling, length + 2);
        elrsTelemetry.length = length + 2;
        ++elrsTelemetry.frames;
    } else ++elrsTelemetry.rejected;
    elrsTelemetry.next_package = 1;
    elrsTelemetry.used = elrsTelemetry.overflow = 0;
}
