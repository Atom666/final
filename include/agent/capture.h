#ifndef AGENT_CAPTURE_H
#define AGENT_CAPTURE_H

#include <stdatomic.h>
#include <stdint.h>
#include <mirror/config.h>
#include <agent/queue.h>

struct self_filter;

struct agent_stats {
    _Atomic uint64_t capture_packets;
    _Atomic uint64_t capture_bytes;
    _Atomic uint64_t capture_truncated_packets;
    _Atomic uint64_t capture_kernel_truncated_packets;
    _Atomic uint64_t capture_oversized_dropped_packets;
    _Atomic uint64_t capture_segmented_aggregates;
    _Atomic uint64_t capture_generated_segments;
    _Atomic uint64_t capture_forwarded_aggregates;
    _Atomic uint64_t capture_segmentation_failures;
    _Atomic uint64_t kernel_ring_dropped_packets;
    _Atomic uint64_t self_transport_packets_filtered;
    _Atomic uint64_t self_transport_bytes_filtered;
    _Atomic uint64_t records_sent;
    _Atomic uint64_t bytes_sent;
    _Atomic uint64_t transport_send_errors;
    _Atomic uint64_t connection_reconnects;
    _Atomic uint64_t connection_downtime_ms;
    _Atomic uint64_t capture_alloc_failures;
    _Atomic uint64_t checksum_not_ready_packets;
    _Atomic uint64_t sequence_number_current;
};

struct capture_context {
    const struct agent_config *cfg;
    struct packet_queue *queue;
    struct self_filter *self_filter;
    struct agent_stats *stats;
    _Atomic int stop;
    _Atomic int failed;
};

void *capture_thread_main(void *arg);
void log_offload_state(const char *ifname);
int capture_list_interfaces(void);

#endif
