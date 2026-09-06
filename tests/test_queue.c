#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <agent/queue.h>

struct delayed_push {
    struct packet_queue *queue;
};

static void *push_after_delay(void *opaque) {
    struct delayed_push *arg = opaque;
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000L };
    nanosleep(&delay, NULL);
    struct packet_item item = { .len = 40, .data = malloc(40) };
    assert(item.data);
    memset(item.data, 0x5a, item.len);
    assert(packet_queue_push_drop_newest(arg->queue, &item) == 0);
    return NULL;
}

int main(void) {
    struct packet_queue q;
    assert(packet_queue_init(&q, 1, 100) == 0);
    struct packet_item a = { .len = 60, .data = (unsigned char *)strdup("a") };
    struct packet_item b = { .len = 60, .data = (unsigned char *)strdup("b") };
    assert(packet_queue_push_drop_newest(&q, &a) == 0);
    assert(packet_queue_push_drop_newest(&q, &b) != 0);
    packet_item_free(&b);
    assert(q.dropped_packets == 1);
    struct packet_item out;
    assert(packet_queue_pop(&q, &out) == 0);
    assert(out.len == 60);
    packet_item_free(&out);
    struct packet_item timed;
    assert(packet_queue_pop_timed(&q, &timed, 1) == 1);
    packet_queue_close(&q);
    packet_queue_destroy(&q);

    assert(packet_queue_init(&q, 4, 1000) == 0);
    struct packet_item input[3] = {0};
    for (size_t i = 0; i < 3; i++) {
        input[i].len = 50;
        input[i].data = malloc(input[i].len);
        assert(input[i].data);
        memset(input[i].data, (int)i, input[i].len);
        assert(packet_queue_push_drop_newest(&q, &input[i]) == 0);
    }
    struct packet_queue_snapshot snapshot;
    packet_queue_get_snapshot(&q, &snapshot);
    assert(snapshot.packets == 3 && snapshot.bytes == 150);
    assert(snapshot.peak_packets == 3 && snapshot.peak_bytes == 150);
    struct packet_item batch[3] = {0};
    size_t batch_count = 0;
    uint64_t wire_bytes = 0;
    assert(packet_queue_pop_batch_timed(&q, batch, 3, 3, 120, 10,
                                        1, 0, &batch_count, &wire_bytes) == 0);
    assert(batch_count == 2 && wire_bytes == 120);
    assert(batch[0].enqueue_monotonic_ns != 0);
    packet_item_free(&batch[0]);
    packet_item_free(&batch[1]);
    assert(packet_queue_pop_batch_timed(&q, batch, 3, 3, 1000, 10,
                                        1, 100, &batch_count, &wire_bytes) == 0);
    assert(batch_count == 1 && wire_bytes == 60);
    packet_item_free(&batch[0]);
    packet_queue_close(&q);
    packet_queue_destroy(&q);

    assert(packet_queue_init(&q, 4, 1000) == 0);
    struct packet_item first = { .len = 40, .data = malloc(40) };
    assert(first.data);
    memset(first.data, 0xa5, first.len);
    assert(packet_queue_push_drop_newest(&q, &first) == 0);
    struct delayed_push delayed = { .queue = &q };
    pthread_t producer;
    assert(pthread_create(&producer, NULL, push_after_delay, &delayed) == 0);
    assert(packet_queue_pop_batch_timed(&q, batch, 3, 3, 1000, 10,
                                        1, 100000, &batch_count,
                                        &wire_bytes) == 0);
    pthread_join(producer, NULL);
    assert(batch_count == 2 && wire_bytes == 100);
    packet_item_free(&batch[0]);
    packet_item_free(&batch[1]);
    packet_queue_close(&q);
    packet_queue_destroy(&q);
    return 0;
}
