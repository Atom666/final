#ifndef AGENT_BATCH_H
#define AGENT_BATCH_H

#include <stddef.h>
#include <stdint.h>

#include <agent/queue.h>
#include <mirror/config.h>

struct wire_batch {
    uint8_t *data;
    size_t capacity;
    size_t length;
    size_t records;
    uint64_t frame_bytes;
};

size_t wire_packet_record_overhead(void);
int wire_batch_init(struct wire_batch *batch, size_t capacity);
void wire_batch_reset(struct wire_batch *batch);
void wire_batch_destroy(struct wire_batch *batch);
/* 0: appended, 1: no room, -1: invalid record. */
int wire_batch_append_packet(struct wire_batch *batch,
                             const struct agent_config *cfg, uint64_t epoch,
                             const struct packet_item *item);

#endif
