#include <agent/queue.h>

#include <stdlib.h>
#include <string.h>
#include <mirror/util.h>

static void pop_one_locked(struct packet_queue *q, struct packet_item *out) {
    *out = q->items[q->head];
    memset(&q->items[q->head], 0, sizeof(q->items[q->head]));
    q->head = (q->head + 1) % q->cap;
    q->count--;
    q->bytes -= out->len;
}

int packet_queue_init(struct packet_queue *q, size_t max_packets, uint64_t max_bytes) {
    memset(q, 0, sizeof(*q));
    q->items = calloc(max_packets, sizeof(q->items[0]));
    if (!q->items) return -1;
    q->cap = max_packets;
    q->max_bytes = max_bytes;
    if (agent_mutex_init(&q->mu) != 0) {
        free(q->items);
        q->items = NULL;
        return -1;
    }
    if (agent_cond_init(&q->cond) != 0) {
        agent_mutex_destroy(&q->mu);
        free(q->items);
        q->items = NULL;
        return -1;
    }
    return 0;
}

void packet_item_free(struct packet_item *item) {
    free(item->data);
    memset(item, 0, sizeof(*item));
}

void packet_queue_destroy(struct packet_queue *q) {
    if (q->items) {
        for (size_t i = 0; i < q->cap; i++) packet_item_free(&q->items[i]);
    }
    free(q->items);
    agent_mutex_destroy(&q->mu);
    agent_cond_destroy(&q->cond);
}

int packet_queue_push_drop_newest(struct packet_queue *q, const struct packet_item *item) {
    uint64_t enqueue_ns = monotonic_ns();
    agent_mutex_lock(&q->mu);
    if (q->closed || q->count == q->cap || q->bytes + item->len > q->max_bytes) {
        q->dropped_packets++;
        q->dropped_bytes += item->len;
        agent_mutex_unlock(&q->mu);
        return -1;
    }
    q->items[q->tail] = *item;
    q->items[q->tail].enqueue_monotonic_ns = enqueue_ns;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;
    q->bytes += item->len;
    if (q->count > q->peak_count) q->peak_count = q->count;
    if (q->bytes > q->peak_bytes) q->peak_bytes = q->bytes;
    agent_cond_signal(&q->cond);
    agent_mutex_unlock(&q->mu);
    return 0;
}

int packet_queue_pop(struct packet_queue *q, struct packet_item *out) {
    agent_mutex_lock(&q->mu);
    while (!q->closed && q->count == 0) agent_cond_wait(&q->cond, &q->mu);
    if (q->count == 0 && q->closed) {
        agent_mutex_unlock(&q->mu);
        return -1;
    }
    pop_one_locked(q, out);
    agent_mutex_unlock(&q->mu);
    return 0;
}

int packet_queue_pop_timed(struct packet_queue *q, struct packet_item *out, uint32_t timeout_ms) {
    agent_mutex_lock(&q->mu);
    int rc = 0;
    while (!q->closed && q->count == 0 && rc == 0)
        rc = agent_cond_timedwait_ns(&q->cond, &q->mu,
                                     (uint64_t)timeout_ms * 1000000ULL);
    if (q->count == 0) {
        int result = q->closed ? -1 : 1;
        agent_mutex_unlock(&q->mu);
        return result;
    }
    pop_one_locked(q, out);
    agent_mutex_unlock(&q->mu);
    return 0;
}

int packet_queue_pop_batch_timed(struct packet_queue *q,
                                 struct packet_item *out, size_t out_capacity,
                                 size_t max_items, uint64_t max_wire_bytes,
                                 uint32_t per_item_wire_overhead,
                                 uint32_t first_timeout_ms, uint32_t linger_us,
                                 size_t *out_count, uint64_t *out_wire_bytes) {
    if (!q || !out || !out_count || !out_wire_bytes || !out_capacity ||
        !max_items || max_items > out_capacity) return -1;
    *out_count = 0;
    *out_wire_bytes = 0;
    agent_mutex_lock(&q->mu);
    int rc = 0;
    while (!q->closed && q->count == 0 && rc == 0)
        rc = agent_cond_timedwait_ns(&q->cond, &q->mu,
                                     (uint64_t)first_timeout_ms * 1000000ULL);
    if (q->count == 0) {
        int result = q->closed ? -1 : 1;
        agent_mutex_unlock(&q->mu);
        return result;
    }

    uint64_t linger_deadline = monotonic_ns() + (uint64_t)linger_us * 1000ULL;
    for (;;) {
        while (q->count && *out_count < max_items) {
            struct packet_item *next = &q->items[q->head];
            uint64_t wire_size = (uint64_t)next->len + per_item_wire_overhead;
            if (*out_count && *out_wire_bytes + wire_size > max_wire_bytes) break;
            if (!*out_count && wire_size > max_wire_bytes) {
                agent_mutex_unlock(&q->mu);
                return -1;
            }
            pop_one_locked(q, &out[*out_count]);
            (*out_count)++;
            *out_wire_bytes += wire_size;
        }
        if (*out_count >= max_items || *out_wire_bytes >= max_wire_bytes ||
            (q->count && (uint64_t)q->items[q->head].len + per_item_wire_overhead +
                         *out_wire_bytes > max_wire_bytes) || !linger_us || q->closed)
            break;
        uint64_t now = monotonic_ns();
        if (now >= linger_deadline) break;
        rc = agent_cond_timedwait_ns(&q->cond, &q->mu, linger_deadline - now);
        if (rc != 0 || q->closed) break;
    }
    agent_mutex_unlock(&q->mu);
    return 0;
}

void packet_queue_get_snapshot(struct packet_queue *q,
                               struct packet_queue_snapshot *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    agent_mutex_lock(&q->mu);
    snapshot->packets = q->count;
    snapshot->bytes = q->bytes;
    snapshot->peak_packets = q->peak_count;
    snapshot->peak_bytes = q->peak_bytes;
    snapshot->dropped_packets = q->dropped_packets;
    snapshot->dropped_bytes = q->dropped_bytes;
    agent_mutex_unlock(&q->mu);
}

void packet_queue_close(struct packet_queue *q) {
    agent_mutex_lock(&q->mu);
    q->closed = true;
    agent_cond_broadcast(&q->cond);
    agent_mutex_unlock(&q->mu);
}
