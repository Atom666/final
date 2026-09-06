#ifndef MIRROR_TEST_TRAFFIC_H
#define MIRROR_TEST_TRAFFIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIRROR_TEST_MAGIC 0x4d545354u
#define MIRROR_TEST_VERSION 1u
#define MIRROR_TEST_HEADER_LEN 40u
#define MIRROR_TEST_MIN_PACKET_SIZE MIRROR_TEST_HEADER_LEN
#define MIRROR_TEST_MAX_PACKET_SIZE 1400u
#define MIRROR_TEST_MAX_EXPECTED 100000000ULL

struct mirror_test_packet {
    uint64_t run_id;
    uint64_t sequence;
    uint32_t packet_length;
    uint64_t body_hash;
};

struct mirror_test_tracker {
    uint8_t *seen;
    uint64_t expected;
    uint64_t matching_packets;
    uint64_t unique_packets;
    uint64_t duplicate_packets;
    uint64_t corrupted_packets;
    uint64_t out_of_range_packets;
    uint64_t matching_bytes;
};

int mirror_test_build_packet(uint8_t *buffer, size_t capacity, uint32_t packet_length,
                             uint64_t run_id, uint64_t sequence);
int mirror_test_parse_packet(const uint8_t *buffer, size_t length,
                             struct mirror_test_packet *packet);
int mirror_test_tracker_init(struct mirror_test_tracker *tracker, uint64_t expected);
void mirror_test_tracker_destroy(struct mirror_test_tracker *tracker);
void mirror_test_tracker_record(struct mirror_test_tracker *tracker,
                                const uint8_t *payload, size_t payload_length,
                                uint64_t expected_run_id);
uint64_t mirror_test_tracker_missing(const struct mirror_test_tracker *tracker);
bool mirror_test_tracker_success(const struct mirror_test_tracker *tracker);
void mirror_test_tracker_print(const char *component, const struct mirror_test_tracker *tracker,
                               uint64_t run_id);

uint64_t mirror_test_htonll(uint64_t value);
uint64_t mirror_test_ntohll(uint64_t value);

#endif
