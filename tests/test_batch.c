#include <agent/batch.h>
#include <mirror/protocol.h>

#include <arpa/inet.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read_u32(const uint8_t *data) {
    uint32_t value;
    memcpy(&value, data, sizeof(value));
    return ntohl(value);
}

static size_t check_record(const uint8_t *data, size_t available,
                           const struct packet_item *expected,
                           const struct agent_config *cfg, uint64_t epoch) {
    assert(available >= 4 + MIRROR_COMMON_HEADER_LEN);
    uint32_t record_len = read_u32(data);
    assert((uint64_t)record_len + 4 <= available);
    struct mirror_record_header header;
    assert(mirror_record_header_from_wire(data + 4, &header) == 0);
    assert(header.magic == MIRROR_MAGIC);
    assert(header.version == MIRROR_VERSION);
    assert(header.record_type == MIRROR_RECORD_PACKET);
    assert(header.header_length == MIRROR_COMMON_HEADER_LEN);
    assert(header.payload_length == mirror_packet_meta_len() + expected->len);
    struct mirror_packet_meta meta;
    const uint8_t *payload = data + 4 + MIRROR_COMMON_HEADER_LEN;
    assert(mirror_packet_meta_from_wire(payload, header.payload_length, &meta) == 0);
    assert(memcmp(meta.agent_uuid, cfg->agent_uuid, sizeof(meta.agent_uuid)) == 0);
    assert(meta.interface_id == cfg->interface_id);
    assert(meta.connection_epoch == epoch);
    assert(meta.sequence_number == expected->sequence_number);
    assert(meta.timestamp_ns == expected->timestamp_ns);
    assert(meta.captured_length == expected->len);
    assert(meta.original_length == expected->original_len);
    assert(meta.flags == expected->flags);
    assert(memcmp(payload + mirror_packet_meta_len(), expected->data,
                  expected->len) == 0);
    return 4u + record_len;
}

int main(void) {
    struct agent_config cfg = { .interface_id = 7 };
    for (size_t i = 0; i < sizeof(cfg.agent_uuid); i++)
        cfg.agent_uuid[i] = (uint8_t)(i + 1);
    uint8_t frame_a[64], frame_b[128];
    memset(frame_a, 0xa5, sizeof(frame_a));
    memset(frame_b, 0x5a, sizeof(frame_b));
    struct packet_item a = {
        .data = frame_a, .len = sizeof(frame_a), .original_len = sizeof(frame_a),
        .flags = 0x12, .sequence_number = 100, .timestamp_ns = 200
    };
    struct packet_item b = {
        .data = frame_b, .len = sizeof(frame_b), .original_len = sizeof(frame_b),
        .flags = 0x34, .sequence_number = 101, .timestamp_ns = 201
    };
    size_t capacity = wire_packet_record_overhead() * 2 + sizeof(frame_a) + sizeof(frame_b);
    struct wire_batch batch;
    assert(wire_batch_init(&batch, capacity) == 0);
    assert(wire_batch_append_packet(&batch, &cfg, 999, &a) == 0);
    assert(wire_batch_append_packet(&batch, &cfg, 999, &b) == 0);
    assert(batch.records == 2);
    assert(batch.frame_bytes == sizeof(frame_a) + sizeof(frame_b));
    assert(batch.length == capacity);
    size_t first = check_record(batch.data, batch.length, &a, &cfg, 999);
    size_t second = check_record(batch.data + first, batch.length - first,
                                 &b, &cfg, 999);
    assert(first + second == batch.length);
    assert(wire_batch_append_packet(&batch, &cfg, 999, &a) == 1);
    wire_batch_reset(&batch);
    assert(batch.length == 0 && batch.records == 0 && batch.frame_bytes == 0);
    wire_batch_destroy(&batch);
    puts("test_batch: PASS");
    return 0;
}
