#ifndef RECEIVER_FINGERPRINT_H
#define RECEIVER_FINGERPRINT_H

#include <stddef.h>
#include <stdint.h>

#define DEDUP_FINGERPRINT_LEN 16
#define DEDUP_FINGERPRINT_VERSION 1

enum fingerprint_status {
    FINGERPRINT_OK = 0,
    FINGERPRINT_FALLBACK = 1,
    FINGERPRINT_FAIL_OPEN = 2
};

struct packet_fingerprint {
    uint8_t bytes[DEDUP_FINGERPRINT_LEN];
    uint8_t payload_hash[DEDUP_FINGERPRINT_LEN];
    uint32_t payload_length;
    uint32_t ip_packet_length;
    uint16_t ether_type;
    uint8_t ip_version;
    uint8_t ip_protocol;
    uint8_t format_version;
    uint8_t fallback;
};

enum fingerprint_status fingerprint_packet(const uint8_t *frame, size_t frame_len,
                                           struct packet_fingerprint *out);

#endif
