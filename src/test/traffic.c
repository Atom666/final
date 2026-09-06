#include <mirror/test_traffic.h>

#include <arpa/inet.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xxhash.h>

static void put16(uint8_t *p, uint16_t value) { uint16_t n = htons(value); memcpy(p, &n, 2); }
static void put32(uint8_t *p, uint32_t value) { uint32_t n = htonl(value); memcpy(p, &n, 4); }
static void put64(uint8_t *p, uint64_t value) { uint64_t n = mirror_test_htonll(value); memcpy(p, &n, 8); }
static uint16_t get16(const uint8_t *p) { uint16_t n; memcpy(&n, p, 2); return ntohs(n); }
static uint32_t get32(const uint8_t *p) { uint32_t n; memcpy(&n, p, 4); return ntohl(n); }
static uint64_t get64(const uint8_t *p) { uint64_t n; memcpy(&n, p, 8); return mirror_test_ntohll(n); }

uint64_t mirror_test_htonll(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)value) << 32) | htonl((uint32_t)(value >> 32));
#else
    return value;
#endif
}

uint64_t mirror_test_ntohll(uint64_t value) { return mirror_test_htonll(value); }

static uint8_t body_byte(uint64_t run_id, uint64_t sequence, size_t offset) {
    uint64_t x = run_id ^ (sequence * 0x9e3779b97f4a7c15ULL) ^ (uint64_t)offset;
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return (uint8_t)x;
}

int mirror_test_build_packet(uint8_t *buffer, size_t capacity, uint32_t packet_length,
                             uint64_t run_id, uint64_t sequence) {
    if (!buffer || packet_length < MIRROR_TEST_MIN_PACKET_SIZE ||
        packet_length > MIRROR_TEST_MAX_PACKET_SIZE || packet_length > capacity || !sequence)
        return -1;
    memset(buffer, 0, MIRROR_TEST_HEADER_LEN);
    for (size_t i = MIRROR_TEST_HEADER_LEN; i < packet_length; i++)
        buffer[i] = body_byte(run_id, sequence, i - MIRROR_TEST_HEADER_LEN);
    put32(buffer, MIRROR_TEST_MAGIC);
    put16(buffer + 4, MIRROR_TEST_VERSION);
    put16(buffer + 6, MIRROR_TEST_HEADER_LEN);
    put64(buffer + 8, run_id);
    put64(buffer + 16, sequence);
    put32(buffer + 24, packet_length);
    put32(buffer + 28, 0);
    put64(buffer + 32, XXH3_64bits(buffer + MIRROR_TEST_HEADER_LEN,
                                   packet_length - MIRROR_TEST_HEADER_LEN));
    return 0;
}

int mirror_test_parse_packet(const uint8_t *buffer, size_t length,
                             struct mirror_test_packet *packet) {
    if (!buffer || !packet || length < MIRROR_TEST_HEADER_LEN ||
        get32(buffer) != MIRROR_TEST_MAGIC || get16(buffer + 4) != MIRROR_TEST_VERSION ||
        get16(buffer + 6) != MIRROR_TEST_HEADER_LEN)
        return -1;
    packet->run_id = get64(buffer + 8);
    packet->sequence = get64(buffer + 16);
    packet->packet_length = get32(buffer + 24);
    packet->body_hash = get64(buffer + 32);
    if (packet->packet_length != length || packet->packet_length < MIRROR_TEST_HEADER_LEN ||
        packet->packet_length > MIRROR_TEST_MAX_PACKET_SIZE || !packet->sequence)
        return -2;
    uint64_t actual_hash = XXH3_64bits(buffer + MIRROR_TEST_HEADER_LEN,
                                      length - MIRROR_TEST_HEADER_LEN);
    return actual_hash == packet->body_hash ? 0 : -2;
}

int mirror_test_tracker_init(struct mirror_test_tracker *tracker, uint64_t expected) {
    if (!tracker || !expected || expected > MIRROR_TEST_MAX_EXPECTED || expected > SIZE_MAX - 7)
        return -1;
    memset(tracker, 0, sizeof(*tracker));
    size_t bytes = (size_t)((expected + 7) / 8);
    tracker->seen = calloc(bytes, 1);
    if (!tracker->seen) return -1;
    tracker->expected = expected;
    return 0;
}

void mirror_test_tracker_destroy(struct mirror_test_tracker *tracker) {
    if (!tracker) return;
    free(tracker->seen);
    memset(tracker, 0, sizeof(*tracker));
}

void mirror_test_tracker_record(struct mirror_test_tracker *tracker,
                                const uint8_t *payload, size_t payload_length,
                                uint64_t expected_run_id) {
    struct mirror_test_packet packet;
    int parsed = mirror_test_parse_packet(payload, payload_length, &packet);
    if (parsed == -1 || packet.run_id != expected_run_id) return;
    tracker->matching_packets++;
    tracker->matching_bytes += payload_length;
    if (parsed != 0) { tracker->corrupted_packets++; return; }
    if (packet.sequence > tracker->expected) { tracker->out_of_range_packets++; return; }
    uint64_t index = packet.sequence - 1;
    uint8_t mask = (uint8_t)(1u << (index & 7));
    if (tracker->seen[index >> 3] & mask) tracker->duplicate_packets++;
    else { tracker->seen[index >> 3] |= mask; tracker->unique_packets++; }
}

uint64_t mirror_test_tracker_missing(const struct mirror_test_tracker *tracker) {
    return tracker->expected - tracker->unique_packets;
}

bool mirror_test_tracker_success(const struct mirror_test_tracker *tracker) {
    return tracker->unique_packets == tracker->expected && !tracker->duplicate_packets &&
           !tracker->corrupted_packets && !tracker->out_of_range_packets;
}

void mirror_test_tracker_print(const char *component, const struct mirror_test_tracker *tracker,
                               uint64_t run_id) {
    printf("component=%s run_id=%" PRIu64 " expected=%" PRIu64
           " received=%" PRIu64 " unique=%" PRIu64 " missing=%" PRIu64
           " duplicates=%" PRIu64 " corrupted=%" PRIu64
           " out_of_range=%" PRIu64 " bytes=%" PRIu64 " result=%s\n",
           component, run_id, tracker->expected, tracker->matching_packets,
           tracker->unique_packets, mirror_test_tracker_missing(tracker),
           tracker->duplicate_packets, tracker->corrupted_packets,
           tracker->out_of_range_packets, tracker->matching_bytes,
           mirror_test_tracker_success(tracker) ? "PASS" : "FAIL");
}
