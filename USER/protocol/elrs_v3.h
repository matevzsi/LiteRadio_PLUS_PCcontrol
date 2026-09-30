/* Minimal standard-resolution OTA adapter, based on ExpressLRS 3.5.3.
 * Upstream copyright/licensing: GPL-3.0, see LICENSE and docs/ELRS3-port.md. */
#ifndef ELRS_V3_H
#define ELRS_V3_H
#include <stdint.h>

#define ELRS_OTA_VERSION_ID 3U
#define ELRS_V3_PACKET_SIZE 8U
#define ELRS_V3_CRSF_MAX 64U

uint16_t elrs_v3_crc_init(const uint8_t uid[6]);
uint32_t elrs_v3_fhss_seed(const uint8_t uid[6]);
uint8_t elrs_v3_rate_index(uint8_t local_rate);
uint8_t elrs_v3_sync_config(uint8_t local_rate, uint8_t tlm_ratio);
void elrs_v3_pack_channels(volatile uint8_t packet[8], const uint16_t channels[8],
                           uint8_t switch_index, uint8_t telemetry_ack);

typedef struct {
    uint8_t ack;
    uint8_t next_package;
    uint8_t used;
    uint8_t overflow;
    uint8_t assembling[ELRS_V3_CRSF_MAX];
    /* Last complete, CRC-checked frame for debugger inspection. No USB yet. */
    uint8_t frame[ELRS_V3_CRSF_MAX];
    uint8_t length;
    uint32_t frames;
    uint32_t rejected;
} elrs_v3_telemetry_t;
extern elrs_v3_telemetry_t elrsTelemetry;
void elrs_v3_telemetry_reset(void);
void elrs_v3_telemetry_receive(uint8_t package_index, const uint8_t payload[5]);
#endif
