#ifndef RECEIVER_CLIENT_H
#define RECEIVER_CLIENT_H

#include <stdint.h>
#include <openssl/ssl.h>
#include <mirror/config.h>
#include <receiver/output.h>
#include <receiver/dedup.h>
#include <pthread.h>

struct output_pipeline;

struct receiver_stats {
    uint64_t active_connections;
    uint64_t active_agents;
    uint64_t accepted_connections;
    uint64_t rejected_connections;
    uint64_t tls_handshake_errors;
    uint64_t protocol_errors;
    uint64_t records_received;
    uint64_t bytes_received;
    uint64_t output_packets_sent;
    uint64_t output_bytes_sent;
    uint64_t output_send_errors;
    uint64_t output_short_writes;
    uint64_t output_oversized_packets;
    uint64_t output_interface_down;
    uint64_t output_mtu_mismatch;
    uint64_t output_queue_drops;
    uint64_t output_queue_drop_packet_limit;
    uint64_t output_queue_drop_byte_limit;
    uint64_t output_queue_drop_allocation;
    uint64_t output_queue_drop_stopping;
    uint64_t output_pipeline_admitted;
    uint64_t output_pipeline_parsed;
    uint64_t output_pipeline_send_batches;
    uint64_t output_pipeline_send_syscalls;
    uint64_t output_pipeline_parse_ns;
    uint64_t output_pipeline_reorder_ns;
    uint64_t output_pipeline_send_ns;
    uint64_t output_pipeline_residence_ns;
    uint64_t output_pipeline_residence_max_ns;
    uint64_t output_reorder_packets_examined;
    uint64_t output_reordered_packets;
    uint64_t output_reorder_timeouts;
    uint64_t output_reorder_bypassed;
    uint64_t agent_packets_received;
    uint64_t agent_bytes_received;
    uint64_t agent_sequence_gaps;
    uint64_t receiver_transport_duplicates_total;
    uint64_t receiver_transport_out_of_order_total;
    uint64_t receiver_cross_agent_duplicates_total;
    uint64_t receiver_dedup_fallback_total;
    uint64_t receiver_dedup_unhandled_offload_suspected_total;
    uint64_t receiver_checksum_repairs_total;
    uint64_t receiver_checksum_repair_failures_total;
    uint64_t receiver_segmented_aggregates_total;
    uint64_t receiver_generated_segments_total;
    uint64_t receiver_segmentation_failures_total;
    uint64_t agent_epoch_changes;
};

struct agent_metric {
    uint8_t uuid[16];
    uint32_t interface_id;
    uint64_t epoch;
    uint64_t last_sequence;
    uint64_t packets_received;
    uint64_t bytes_received;
    uint64_t transport_accepted;
    uint64_t gaps;
    uint64_t transport_duplicates;
    uint64_t out_of_order;
    uint64_t cross_agent_duplicates;
    uint64_t output_packets;
    uint64_t output_bytes;
    uint64_t output_errors;
    uint64_t oversized_packets;
    uint64_t processing_errors;
    uint64_t output_queue_drops;
    uint64_t checksum_repairs;
    uint64_t checksum_repair_failures;
    uint64_t segmented_aggregates;
    uint64_t generated_segments;
    uint64_t last_seen_ns;
    int used;
    int connected;
};

struct agent_registry {
    pthread_mutex_t mutex;
    struct agent_metric *items;
    size_t capacity;
};

struct client_thread_arg {
    int fd;
    SSL_CTX *ctx;
    const struct receiver_config *cfg;
    struct output_pipeline *pipeline;
    struct receiver_stats *stats;
    struct agent_registry *registry;
    struct dedup_cache *dedup;
    struct transport_registry *transport;
};

void *receiver_client_thread(void *arg);
void receiver_log_agent_metrics(struct agent_registry *registry);

#endif
