#ifndef AGENT_QUEUE_H
#define AGENT_QUEUE_H

#include <agent/sync.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct packet_item {
    uint8_t *data;
    uint32_t len;
    uint32_t original_len;
    uint32_t flags;
    uint16_t vlan_tci;
    uint16_t vlan_tpid;
    uint8_t direction;
    uint8_t packet_type;
    uint8_t vlan_tag_count;
    uint64_t timestamp_ns;
    uint64_t sequence_number;
    uint64_t enqueue_monotonic_ns;
};

struct packet_queue_snapshot {
    size_t packets;
    uint64_t bytes;
    size_t peak_packets;
    uint64_t peak_bytes;
    uint64_t dropped_packets;
    uint64_t dropped_bytes;
};

struct packet_queue {
    agent_mutex_t mu;
    agent_cond_t cond;
    struct packet_item *items;
    size_t cap;
    size_t head;
    size_t tail;
    size_t count;
    uint64_t bytes;
    uint64_t max_bytes;
    uint64_t dropped_packets;
    uint64_t dropped_bytes;
    size_t peak_count;
    uint64_t peak_bytes;
    bool closed;
};

int packet_queue_init(struct packet_queue *q, size_t max_packets, uint64_t max_bytes);
void packet_queue_destroy(struct packet_queue *q);
int packet_queue_push_drop_newest(struct packet_queue *q, const struct packet_item *item);
int packet_queue_pop(struct packet_queue *q, struct packet_item *out);
/* 0: item, 1: timeout, -1: closed/error. */
int packet_queue_pop_timed(struct packet_queue *q, struct packet_item *out, uint32_t timeout_ms);
/* 0: batch, 1: first-item timeout, -1: closed/error. */
int packet_queue_pop_batch_timed(struct packet_queue *q,
                                 struct packet_item *out, size_t out_capacity,
                                 size_t max_items, uint64_t max_wire_bytes,
                                 uint32_t per_item_wire_overhead,
                                 uint32_t first_timeout_ms, uint32_t linger_us,
                                 size_t *out_count, uint64_t *out_wire_bytes);
void packet_queue_get_snapshot(struct packet_queue *q,
                               struct packet_queue_snapshot *snapshot);
void packet_queue_close(struct packet_queue *q);
void packet_item_free(struct packet_item *item);

#endif
