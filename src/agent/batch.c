#include <agent/batch.h>

#include <mirror/protocol.h>

#include <mirror/net.h>
#include <stdlib.h>
#include <string.h>

size_t wire_packet_record_overhead(void) {
    return 4u + MIRROR_COMMON_HEADER_LEN + mirror_packet_meta_len();
}

int wire_batch_init(struct wire_batch *batch, size_t capacity) {
    if (!batch || !capacity) return -1;
    memset(batch, 0, sizeof(*batch));
    batch->data = malloc(capacity);
    if (!batch->data) return -1;
    batch->capacity = capacity;
    return 0;
}

void wire_batch_reset(struct wire_batch *batch) {
    if (!batch) return;
    batch->length = 0;
    batch->records = 0;
    batch->frame_bytes = 0;
}

void wire_batch_destroy(struct wire_batch *batch) {
    if (!batch) return;
    free(batch->data);
    memset(batch, 0, sizeof(*batch));
}

int wire_batch_append_packet(struct wire_batch *batch,
                             const struct agent_config *cfg, uint64_t epoch,
                             const struct packet_item *item) {
    if (!batch || !batch->data || !cfg || !item || !item->data || !item->len)
        return -1;
    if (batch->length > batch->capacity) return -1;
    size_t meta_len = mirror_packet_meta_len();
    uint64_t payload_len64 = (uint64_t)meta_len + item->len;
    uint64_t record_len64 = MIRROR_COMMON_HEADER_LEN + payload_len64;
    uint64_t wire_len64 = 4u + record_len64;
    if (payload_len64 > UINT32_MAX || record_len64 > UINT32_MAX ||
        wire_len64 > SIZE_MAX)
        return -1;
    if (wire_len64 > batch->capacity - batch->length)
        return wire_len64 <= batch->capacity ? 1 : -1;

    uint8_t *cursor = batch->data + batch->length;
    uint32_t record_len = (uint32_t)record_len64;
    uint32_t network_record_len = htonl(record_len);
    memcpy(cursor, &network_record_len, sizeof(network_record_len));
    cursor += sizeof(network_record_len);

    struct mirror_record_header header = {
        .magic = MIRROR_MAGIC,
        .version = MIRROR_VERSION,
        .record_type = MIRROR_RECORD_PACKET,
        .header_length = MIRROR_COMMON_HEADER_LEN,
        .payload_length = (uint32_t)payload_len64
    };
    mirror_record_header_to_wire(&header, cursor);
    cursor += MIRROR_COMMON_HEADER_LEN;

    struct mirror_packet_meta meta = {0};
    memcpy(meta.agent_uuid, cfg->agent_uuid, sizeof(meta.agent_uuid));
    meta.interface_id = cfg->interface_id;
    meta.flags = item->flags;
    meta.connection_epoch = epoch;
    meta.sequence_number = item->sequence_number;
    meta.timestamp_ns = item->timestamp_ns;
    meta.captured_length = item->len;
    meta.original_length = item->original_len;
    meta.vlan_tci = item->vlan_tci;
    meta.vlan_tpid = item->vlan_tpid;
    meta.vlan_tag_count = item->vlan_tag_count;
    meta.direction = item->direction;
    meta.packet_type = item->packet_type;
    mirror_packet_meta_to_wire(&meta, cursor);
    cursor += meta_len;
    memcpy(cursor, item->data, item->len);

    batch->length += (size_t)wire_len64;
    batch->records++;
    batch->frame_bytes += item->len;
    return 0;
}
