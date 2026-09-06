#include <mirror/config.h>
#include <receiver/output.h>
#include <receiver/pipeline.h>
#include <receiver/client.h>

#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct collected_packet {
    uint32_t sequence;
    uint8_t flags;
    uint16_t source_port;
};

struct collector {
    pthread_mutex_t mutex;
    struct collected_packet packets[32];
    size_t count;
};

static uint16_t read16(const uint8_t *p) {
    uint16_t value;
    memcpy(&value, p, sizeof(value));
    return ntohs(value);
}

static uint32_t read32(const uint8_t *p) {
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return ntohl(value);
}

static void write16(uint8_t *p, uint16_t value) {
    value = htons(value);
    memcpy(p, &value, sizeof(value));
}

static void write32(uint8_t *p, uint32_t value) {
    value = htonl(value);
    memcpy(p, &value, sizeof(value));
}

static uint8_t *make_tcp_frame(bool reverse, uint32_t sequence,
                               uint8_t flags, size_t payload_len,
                               size_t *frame_len) {
    *frame_len = 14 + 20 + 20 + payload_len;
    uint8_t *frame = calloc(1, *frame_len);
    assert(frame);
    for (size_t i = 0; i < 12; i++) frame[i] = (uint8_t)(i + 1);
    write16(frame + 12, 0x0800);
    uint8_t *ip = frame + 14;
    ip[0] = 0x45;
    write16(ip + 2, (uint16_t)(20 + 20 + payload_len));
    ip[8] = 64;
    ip[9] = 6;
    const uint8_t a[4] = {192, 0, 2, 1};
    const uint8_t b[4] = {192, 0, 2, 2};
    memcpy(ip + 12, reverse ? b : a, 4);
    memcpy(ip + 16, reverse ? a : b, 4);
    uint8_t *tcp = ip + 20;
    write16(tcp, reverse ? 8000 : 40000);
    write16(tcp + 2, reverse ? 40000 : 8000);
    write32(tcp + 4, sequence);
    tcp[12] = 5u << 4;
    tcp[13] = flags;
    write16(tcp + 14, 65535);
    for (size_t i = 0; i < payload_len; i++) tcp[20 + i] = (uint8_t)(i + sequence);
    return frame;
}

static int collect_frame(void *arg, const uint8_t *frame, size_t frame_len) {
    (void)frame_len;
    struct collector *collector = arg;
    const uint8_t *tcp = frame + 14 + 20;
    pthread_mutex_lock(&collector->mutex);
    if (collector->count < 32) {
        collector->packets[collector->count].source_port = read16(tcp);
        collector->packets[collector->count].sequence = read32(tcp + 4);
        collector->packets[collector->count].flags = tcp[13];
    }
    collector->count++;
    pthread_mutex_unlock(&collector->mutex);
    return OUTPUT_OK;
}

static void submit(struct output_pipeline *pipeline, bool reverse,
                   uint32_t sequence, uint8_t flags, size_t payload_len,
                   uint64_t timestamp) {
    size_t frame_len;
    uint8_t *frame = make_tcp_frame(reverse, sequence, flags, payload_len, &frame_len);
    assert(output_pipeline_submit(pipeline, frame, (uint32_t)frame_len,
                                  timestamp, true, NULL) == 0);
    free(frame);
}

static void sleep_ms(long milliseconds) {
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L
    };
    nanosleep(&delay, NULL);
}

struct producer_arg {
    struct output_pipeline *pipeline;
    bool reverse;
};

static void *producer_main(void *opaque) {
    struct producer_arg *arg = opaque;
    if (arg->reverse) {
        submit(arg->pipeline, true, 500, 0x12, 0, 200);
    } else {
        submit(arg->pipeline, false, 101, 0x18, 100, 300);
        submit(arg->pipeline, false, 201, 0x18, 100, 400);
        submit(arg->pipeline, false, 101, 0x18, 100, 500);
    }
    return NULL;
}

int main(void) {
    struct receiver_config cfg;
    receiver_config_defaults(&cfg);
    cfg.reorder_window_ms = 50;
    cfg.reorder_max_flows = 16;
    cfg.reorder_flow_timeout_sec = 1;
    cfg.output_queue_max_packets = 64;
    cfg.output_queue_max_bytes = 1024 * 1024;

    struct collector collector = {0};
    assert(pthread_mutex_init(&collector.mutex, NULL) == 0);
    struct output_pipeline pipeline = {0};
    char err[128];
    struct receiver_stats observed = {0};
    assert(output_pipeline_init_custom_observed(&pipeline, &cfg, collect_frame,
                                                &collector, &observed,
                                                err, sizeof(err)) == 0);

    struct producer_arg forward = { .pipeline = &pipeline, .reverse = false };
    struct producer_arg reverse = { .pipeline = &pipeline, .reverse = true };
    pthread_t forward_thread, reverse_thread;
    assert(pthread_create(&forward_thread, NULL, producer_main, &forward) == 0);
    assert(pthread_create(&reverse_thread, NULL, producer_main, &reverse) == 0);
    pthread_join(forward_thread, NULL);
    pthread_join(reverse_thread, NULL);
    sleep_ms(5);
    pthread_mutex_lock(&collector.mutex);
    assert(collector.count == 0);
    pthread_mutex_unlock(&collector.mutex);
    submit(&pipeline, false, 100, 0x02, 0, 100);

    output_pipeline_close(&pipeline);
    pthread_mutex_lock(&collector.mutex);
    assert(collector.count == 5);
    assert(collector.packets[0].source_port == 40000);
    assert(collector.packets[0].sequence == 100);
    assert((collector.packets[0].flags & 0x02) != 0);
    assert(collector.packets[1].source_port == 8000);
    assert(collector.packets[1].sequence == 500);
    assert(collector.packets[2].sequence == 101);
    assert(collector.packets[3].sequence == 201);
    assert(collector.packets[4].sequence == 101);
    pthread_mutex_unlock(&collector.mutex);

    collector.count = 0;
    memset(&observed, 0, sizeof(observed));
    cfg.reorder_window_ms = 1000;
    cfg.output_queue_max_packets = 1;
    assert(output_pipeline_init_custom_observed(&pipeline, &cfg, collect_frame,
                                                &collector, &observed,
                                                err, sizeof(err)) == 0);
    /* A mid-stream packet without SYN remains buffered until timeout/close. */
    submit(&pipeline, false, 1000, 0x18, 100, 1000);
    sleep_ms(10);
    struct output_pipeline_snapshot snapshot;
    output_pipeline_get_snapshot(&pipeline, &snapshot);
    assert(snapshot.queued_packets == 1);
    size_t frame_len;
    uint8_t *frame = make_tcp_frame(false, 1001, 0x18, 100, &frame_len);
    assert(output_pipeline_submit(&pipeline, frame, (uint32_t)frame_len,
                                  1100, true, NULL) != 0);
    assert(observed.output_queue_drops == 1);
    assert(observed.output_queue_drop_packet_limit == 1);
    assert(observed.output_queue_drop_byte_limit == 0);
    free(frame);
    output_pipeline_close(&pipeline);
    assert(collector.count == 1);

    collector.count = 0;
    memset(&observed, 0, sizeof(observed));
    cfg.output_queue_max_packets = 10;
    cfg.output_queue_max_bytes = 200;
    assert(output_pipeline_init_custom_observed(&pipeline, &cfg, collect_frame,
                                                &collector, &observed,
                                                err, sizeof(err)) == 0);
    submit(&pipeline, false, 2000, 0x18, 100, 2000);
    frame = make_tcp_frame(false, 2100, 0x18, 100, &frame_len);
    assert(output_pipeline_submit(&pipeline, frame, (uint32_t)frame_len,
                                  2100, true, NULL) != 0);
    free(frame);
    assert(observed.output_queue_drop_byte_limit == 1);
    output_pipeline_close(&pipeline);
    assert(collector.count == 1);

    collector.count = 0;
    cfg.output_queue_max_packets = 20000;
    cfg.output_queue_max_bytes = 32 * 1024 * 1024;
    cfg.reorder_window_ms = 20;
    assert(output_pipeline_init_custom(&pipeline, &cfg, collect_frame,
                                       &collector, err, sizeof(err)) == 0);
    submit(&pipeline, false, 3000, 0x02, 0, 3000);
    for (uint32_t i = 0; i < 10000; i++)
        submit(&pipeline, false, 3001 + i * 100, 0x18, 100, 3001 + i);
    output_pipeline_close(&pipeline);
    assert(collector.count == 10001);

    pthread_mutex_destroy(&collector.mutex);
    puts("test_pipeline: PASS");
    return 0;
}
