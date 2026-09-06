#ifndef RECEIVER_PIPELINE_H
#define RECEIVER_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct agent_metric;
struct agent_registry;
struct output_sink;
struct receiver_config;
struct receiver_stats;

typedef int (*output_pipeline_send_fn)(void *arg, const uint8_t *frame, size_t frame_len);

struct output_pipeline {
    void *impl;
};

struct output_pipeline_snapshot {
    uint64_t queued_packets;
    uint64_t queued_bytes;
    uint64_t peak_queued_packets;
    uint64_t peak_queued_bytes;
    uint64_t input_pending_packets;
    uint64_t input_pending_bytes;
    uint64_t peak_input_pending_packets;
    uint64_t peak_input_pending_bytes;
    uint64_t reorder_pending_packets;
    uint64_t peak_reorder_pending_packets;
    uint64_t reserved_packets;
    uint64_t active_flows;
    uint64_t deadline_flows;
    uint64_t capacity_time_ns;
    uint64_t capacity_time_max_ns;
    uint64_t flow_count;
};

int output_pipeline_init(struct output_pipeline *pipeline,
                         const struct receiver_config *cfg,
                         struct output_sink *sink,
                         struct receiver_stats *stats,
                         struct agent_registry *registry,
                         char *err, size_t err_len);
int output_pipeline_init_custom(struct output_pipeline *pipeline,
                                const struct receiver_config *cfg,
                                output_pipeline_send_fn send_fn, void *send_arg,
                                char *err, size_t err_len);
int output_pipeline_init_custom_observed(struct output_pipeline *pipeline,
                                         const struct receiver_config *cfg,
                                         output_pipeline_send_fn send_fn, void *send_arg,
                                         struct receiver_stats *stats,
                                         char *err, size_t err_len);
int output_pipeline_submit(struct output_pipeline *pipeline,
                           const uint8_t *frame, uint32_t frame_len,
                           uint64_t adjusted_timestamp_ns,
                           bool timestamp_valid,
                           struct agent_metric *agent);
void output_pipeline_get_snapshot(struct output_pipeline *pipeline,
                                  struct output_pipeline_snapshot *snapshot);
void output_pipeline_close(struct output_pipeline *pipeline);

#endif
