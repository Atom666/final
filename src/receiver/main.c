#include <receiver/client.h>
#include <receiver/output.h>
#include <receiver/pipeline.h>
#include <mirror/config.h>
#include <mirror/tls.h>
#include <mirror/util.h>

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>

static _Atomic int g_stop = 0;
static void on_signal(int sig) { (void)sig; atomic_store(&g_stop, 1); }

static uint64_t receiver_main_monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv) {
    const char *config_path = argc > 1 ? argv[1] : "examples/receiver.conf";
    struct receiver_config cfg;
    char err[256];
    if (load_receiver_config(config_path, &cfg, err, sizeof(err)) != 0) {
        fprintf(stderr, "config error: %s\n", err);
        return 2;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    tls_global_init();

    struct output_sink output;
    if (output_open(&output, cfg.output_interface, cfg.required_mtu, cfg.max_frame_size, err, sizeof(err)) != 0) {
        log_msg("receiver", "error", "output_mtu_mismatch=%d error=%s", strstr(err, "MTU") != NULL, err);
        return 1;
    }
    struct tls_server_config sc = {
        .cert_file = cfg.server_cert_file,
        .key_file = cfg.server_key_file,
        .client_ca_file = cfg.client_ca_file,
        .require_client_cert = cfg.require_client_certificate,
        .insecure = cfg.insecure_tls
    };
    SSL_CTX *ssl_ctx = tls_create_server_ctx(&sc, err, sizeof(err));
    if (!ssl_ctx) {
        log_msg("receiver", "error", "%s", err);
        return 1;
    }
    int lfd = listen_tcp(cfg.listen_address, cfg.listen_port, 128);
    if (lfd < 0) {
        log_msg("receiver", "error", "listen failed: %s", strerror(errno));
        return 1;
    }
    struct timeval accept_tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &accept_tv, sizeof(accept_tv));
    log_msg("receiver", "info", "listening %s:%u output=%s", cfg.listen_address, cfg.listen_port, cfg.output_interface);
    struct receiver_stats stats = {0};
    struct agent_registry registry = { .items = calloc(cfg.max_clients, sizeof(*registry.items)), .capacity = cfg.max_clients };
    if (!registry.items || pthread_mutex_init(&registry.mutex, NULL) != 0) {
        log_msg("receiver", "error", "agent registry allocation failed"); return 1;
    }
    struct dedup_cache dedup;
    if (dedup_cache_init(&dedup, &cfg, err, sizeof(err)) != 0) {
        log_msg("receiver", "error", "%s", err); return 1;
    }
    struct transport_registry transport;
    if (transport_registry_init(&transport, cfg.transport_max_entries) != 0) {
        log_msg("receiver", "error", "transport registry allocation failed"); return 1;
    }
    struct output_pipeline pipeline = {0};
    if (output_pipeline_init(&pipeline, &cfg, &output, &stats, &registry,
                             err, sizeof(err)) != 0) {
        log_msg("receiver", "error", "%s", err);
        return 1;
    }
    uint64_t next_log = now_ns() + (uint64_t)cfg.log_interval_sec * 1000000000ULL;
    uint64_t pipeline_sample_ns = receiver_main_monotonic_ns();
    uint64_t previous_admitted = 0, previous_parsed = 0, previous_emitted = 0;
    while (!atomic_load(&g_stop)) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                if (now_ns() >= next_log) {
                    struct output_pipeline_snapshot pipeline_snapshot;
                    output_pipeline_get_snapshot(&pipeline, &pipeline_snapshot);
                    log_msg("receiver", "info", "active_agents=%llu active_connections=%llu accepted=%llu rejected=%llu records=%llu bytes=%llu output_packets=%llu output_bytes=%llu output_errors=%llu output_short=%llu output_oversized=%llu output_down=%llu output_queue_packets=%llu output_queue_bytes=%llu output_queue_drops=%llu reorder_flows=%llu reordered_packets=%llu reorder_timeouts=%llu reorder_bypassed=%llu agent_packets=%llu agent_bytes=%llu sequence_gaps=%llu receiver_transport_duplicates_total=%llu receiver_transport_out_of_order_total=%llu receiver_cross_agent_duplicates_total=%llu receiver_dedup_cache_entries=%llu receiver_dedup_cache_evictions_total=%llu receiver_dedup_cache_expired_total=%llu receiver_dedup_fallback_total=%llu receiver_dedup_unhandled_offload_suspected_total=%llu receiver_checksum_repairs_total=%llu receiver_checksum_repair_failures_total=%llu receiver_segmented_aggregates_total=%llu receiver_generated_segments_total=%llu receiver_segmentation_failures_total=%llu epoch_changes=%llu protocol_errors=%llu tls_errors=%llu",
                            (unsigned long long)__atomic_load_n(&stats.active_agents, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.active_connections, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.accepted_connections, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.rejected_connections, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.records_received, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.bytes_received, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.output_packets_sent, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.output_bytes_sent, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.output_send_errors, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.output_short_writes, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.output_oversized_packets, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.output_interface_down, __ATOMIC_RELAXED),
                            (unsigned long long)pipeline_snapshot.queued_packets,
                            (unsigned long long)pipeline_snapshot.queued_bytes,
                            (unsigned long long)__atomic_load_n(&stats.output_queue_drops, __ATOMIC_RELAXED),
                            (unsigned long long)pipeline_snapshot.flow_count,
                            (unsigned long long)__atomic_load_n(&stats.output_reordered_packets, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.output_reorder_timeouts, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.output_reorder_bypassed, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.agent_packets_received, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.agent_bytes_received, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.agent_sequence_gaps, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.receiver_transport_duplicates_total, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.receiver_transport_out_of_order_total, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.receiver_cross_agent_duplicates_total, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&dedup.entries, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&dedup.evictions, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&dedup.expired, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.receiver_dedup_fallback_total, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.receiver_dedup_unhandled_offload_suspected_total, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.receiver_checksum_repairs_total, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.receiver_checksum_repair_failures_total, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.receiver_segmented_aggregates_total, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.receiver_generated_segments_total, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.receiver_segmentation_failures_total, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.agent_epoch_changes, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.protocol_errors, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.tls_handshake_errors, __ATOMIC_RELAXED));
                    log_msg("receiver-summary", "info",
                            "packets_received_from_agents=%llu cross_agent_deduplicated=%llu final_output_packets=%llu output_queue_packets=%llu output_queue_drops=%llu peer_rescued_after_reject=%llu rejected_observation_pairs=%llu reordered_packets=%llu reorder_timeouts=%llu reorder_bypassed=%llu transport_duplicates=%llu transport_out_of_order=%llu sequence_gaps=%llu checksum_repairs=%llu checksum_repair_failures=%llu segmented_aggregates=%llu generated_segments=%llu segmentation_failures=%llu output_errors=%llu output_oversized=%llu",
                            (unsigned long long)__atomic_load_n(&stats.agent_packets_received, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.receiver_cross_agent_duplicates_total, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.output_packets_sent, __ATOMIC_RELAXED),
                            (unsigned long long)pipeline_snapshot.queued_packets,
                            (unsigned long long)__atomic_load_n(&stats.output_queue_drops, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&dedup.peer_rescued_after_reject, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&dedup.rejected_observation_pairs, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.output_reordered_packets, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.output_reorder_timeouts, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.output_reorder_bypassed, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.receiver_transport_duplicates_total, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.receiver_transport_out_of_order_total, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.agent_sequence_gaps, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.receiver_checksum_repairs_total, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.receiver_checksum_repair_failures_total, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.receiver_segmented_aggregates_total, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.receiver_generated_segments_total, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.receiver_segmentation_failures_total, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.output_send_errors, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.output_oversized_packets, __ATOMIC_RELAXED));
                    uint64_t sample_now = receiver_main_monotonic_ns();
                    uint64_t admitted = __atomic_load_n(&stats.output_pipeline_admitted, __ATOMIC_RELAXED);
                    uint64_t parsed = __atomic_load_n(&stats.output_pipeline_parsed, __ATOMIC_RELAXED);
                    uint64_t emitted = __atomic_load_n(&stats.output_packets_sent, __ATOMIC_RELAXED);
                    double interval_sec = sample_now > pipeline_sample_ns ?
                        (double)(sample_now - pipeline_sample_ns) / 1000000000.0 : 1.0;
                    uint64_t parse_ns = __atomic_load_n(&stats.output_pipeline_parse_ns, __ATOMIC_RELAXED);
                    uint64_t reorder_ns = __atomic_load_n(&stats.output_pipeline_reorder_ns, __ATOMIC_RELAXED);
                    uint64_t send_ns = __atomic_load_n(&stats.output_pipeline_send_ns, __ATOMIC_RELAXED);
                    uint64_t batches = __atomic_load_n(&stats.output_pipeline_send_batches, __ATOMIC_RELAXED);
                    uint64_t syscalls = __atomic_load_n(&stats.output_pipeline_send_syscalls, __ATOMIC_RELAXED);
                    uint64_t residence_ns = __atomic_load_n(&stats.output_pipeline_residence_ns, __ATOMIC_RELAXED);
                    uint64_t examined = __atomic_load_n(&stats.output_reorder_packets_examined, __ATOMIC_RELAXED);
                    log_msg("receiver-pipeline", "info",
                            "admitted_pps=%.2f parsed_pps=%.2f emitted_pps=%.2f owned_packets=%llu owned_bytes=%llu peak_owned_packets=%llu peak_owned_bytes=%llu input_pending=%llu input_pending_bytes=%llu peak_input_pending=%llu peak_input_pending_bytes=%llu reorder_pending=%llu peak_reorder_pending=%llu reserved=%llu active_flows=%llu deadline_flows=%llu flow_count=%llu drops_total=%llu drops_packet_limit=%llu drops_byte_limit=%llu drops_allocation=%llu drops_stopping=%llu capacity_time_ms=%llu capacity_max_ms=%llu send_batches=%llu send_syscalls=%llu batch_avg_packets=%.2f parse_avg_us=%.2f reorder_avg_us=%.2f send_avg_us=%.2f residence_avg_us=%.2f residence_max_us=%llu examined_per_emitted=%.2f rejected_cache_entries=%llu rejected_cache_expired=%llu",
                            (double)(admitted - previous_admitted) / interval_sec,
                            (double)(parsed - previous_parsed) / interval_sec,
                            (double)(emitted - previous_emitted) / interval_sec,
                            (unsigned long long)pipeline_snapshot.queued_packets,
                            (unsigned long long)pipeline_snapshot.queued_bytes,
                            (unsigned long long)pipeline_snapshot.peak_queued_packets,
                            (unsigned long long)pipeline_snapshot.peak_queued_bytes,
                            (unsigned long long)pipeline_snapshot.input_pending_packets,
                            (unsigned long long)pipeline_snapshot.input_pending_bytes,
                            (unsigned long long)pipeline_snapshot.peak_input_pending_packets,
                            (unsigned long long)pipeline_snapshot.peak_input_pending_bytes,
                            (unsigned long long)pipeline_snapshot.reorder_pending_packets,
                            (unsigned long long)pipeline_snapshot.peak_reorder_pending_packets,
                            (unsigned long long)pipeline_snapshot.reserved_packets,
                            (unsigned long long)pipeline_snapshot.active_flows,
                            (unsigned long long)pipeline_snapshot.deadline_flows,
                            (unsigned long long)pipeline_snapshot.flow_count,
                            (unsigned long long)__atomic_load_n(&stats.output_queue_drops, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.output_queue_drop_packet_limit, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.output_queue_drop_byte_limit, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.output_queue_drop_allocation, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&stats.output_queue_drop_stopping, __ATOMIC_RELAXED),
                            (unsigned long long)(pipeline_snapshot.capacity_time_ns / 1000000ULL),
                            (unsigned long long)(pipeline_snapshot.capacity_time_max_ns / 1000000ULL),
                            (unsigned long long)batches,
                            (unsigned long long)syscalls,
                            batches ? (double)emitted / (double)batches : 0.0,
                            parsed ? (double)parse_ns / (double)parsed / 1000.0 : 0.0,
                            emitted ? (double)reorder_ns / (double)emitted / 1000.0 : 0.0,
                            batches ? (double)send_ns / (double)batches / 1000.0 : 0.0,
                            emitted ? (double)residence_ns / (double)emitted / 1000.0 : 0.0,
                            (unsigned long long)(__atomic_load_n(&stats.output_pipeline_residence_max_ns, __ATOMIC_RELAXED) / 1000ULL),
                            emitted ? (double)examined / (double)emitted : 0.0,
                            (unsigned long long)__atomic_load_n(&dedup.rejected_entries, __ATOMIC_RELAXED),
                            (unsigned long long)__atomic_load_n(&dedup.rejected_expired, __ATOMIC_RELAXED));
                    pipeline_sample_ns = sample_now;
                    previous_admitted = admitted;
                    previous_parsed = parsed;
                    previous_emitted = emitted;
                    receiver_log_agent_metrics(&registry);
                    next_log = now_ns() + (uint64_t)cfg.log_interval_sec * 1000000000ULL;
                }
                continue;
            }
            log_msg("receiver", "warn", "accept failed: %s", strerror(errno));
            continue;
        }
        __atomic_add_fetch(&stats.accepted_connections, 1, __ATOMIC_RELAXED);
        if (__atomic_load_n(&stats.active_connections, __ATOMIC_RELAXED) >= cfg.max_clients) {
            __atomic_add_fetch(&stats.rejected_connections, 1, __ATOMIC_RELAXED);
            close(fd);
            continue;
        }
        struct client_thread_arg *arg = calloc(1, sizeof(*arg));
        if (!arg) { __atomic_add_fetch(&stats.rejected_connections, 1, __ATOMIC_RELAXED); close(fd); continue; }
        arg->fd = fd;
        arg->ctx = ssl_ctx;
        arg->cfg = &cfg;
        arg->pipeline = &pipeline;
        arg->stats = &stats;
        arg->registry = &registry;
        arg->dedup = &dedup;
        arg->transport = &transport;
        pthread_t tid;
        __atomic_add_fetch(&stats.active_connections, 1, __ATOMIC_RELAXED);
        if (pthread_create(&tid, NULL, receiver_client_thread, arg) != 0) {
            __atomic_add_fetch(&stats.active_connections, (uint64_t)-1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&stats.rejected_connections, 1, __ATOMIC_RELAXED);
            close(fd); free(arg); continue;
        }
        pthread_detach(tid);
    }
    close(lfd);
    /* Detached client threads may still be unwinding after SIGTERM. The OS owns
       final reclamation; do not free their shared objects underneath them. */
    if (__atomic_load_n(&stats.active_connections, __ATOMIC_RELAXED) == 0) {
        output_pipeline_close(&pipeline);
        output_close(&output);
        SSL_CTX_free(ssl_ctx);
        dedup_cache_destroy(&dedup);
        transport_registry_destroy(&transport);
        pthread_mutex_destroy(&registry.mutex); free(registry.items);
    }
    return 0;
}
