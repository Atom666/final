#include <agent/batch.h>
#include <agent/capture.h>
#include <agent/platform.h>
#include <agent/queue.h>
#include <agent/self_filter.h>
#include <agent/sync.h>
#include <mirror/net.h>
#include <mirror/protocol.h>
#include <mirror/tls.h>
#include <mirror/util.h>

#include <errno.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Atomic int g_stop = 0;

#define RESIDENCE_BUCKETS 64u

struct sender_metrics {
    uint64_t batches_sent;
    uint64_t batch_wire_bytes;
    uint64_t batch_max_records;
    uint64_t batch_max_wire_bytes;
    uint64_t failed_batches;
    uint64_t failed_batch_records;
    uint64_t tls_write_operations;
    uint64_t ssl_write_calls;
    uint64_t ssl_write_duration_ns;
    uint64_t ssl_write_duration_max_ns;
    uint64_t residence_samples;
    uint64_t residence_sum_ns;
    uint64_t residence_max_ns;
    uint64_t residence_buckets[RESIDENCE_BUCKETS];
};

struct log_sample {
    uint64_t monotonic_ns;
    uint64_t captured_packets;
    uint64_t captured_bytes;
    uint64_t sent_packets;
    uint64_t sent_bytes;
    uint64_t cpu_us;
};

static uint64_t agent_monotonic_ns(void) {
    return monotonic_ns();
}

static uint64_t process_cpu_us(void) {
    return agent_process_cpu_us();
}

static uint64_t current_rss_bytes(void) {
    return agent_current_rss_bytes();
}

static unsigned residence_bucket(uint64_t ns) {
    uint64_t us = (ns + 999ULL) / 1000ULL;
    uint64_t upper_bound = 1;
    unsigned bucket = 0;
    while (upper_bound < us && bucket + 1 < RESIDENCE_BUCKETS) {
        upper_bound <<= 1;
        bucket++;
    }
    return bucket;
}

static void observe_residence(struct sender_metrics *metrics, uint64_t ns) {
    metrics->residence_samples++;
    metrics->residence_sum_ns += ns;
    if (ns > metrics->residence_max_ns) metrics->residence_max_ns = ns;
    metrics->residence_buckets[residence_bucket(ns)]++;
}

static uint64_t residence_percentile_us(const struct sender_metrics *metrics,
                                        uint32_t percentile) {
    if (!metrics->residence_samples) return 0;
    uint64_t target = (metrics->residence_samples * percentile + 99) / 100;
    uint64_t cumulative = 0;
    for (unsigned i = 0; i < RESIDENCE_BUCKETS; i++) {
        cumulative += metrics->residence_buckets[i];
        if (cumulative >= target) return i >= 63 ? UINT64_MAX : 1ULL << i;
    }
    return metrics->residence_max_ns / 1000ULL;
}

static void init_log_sample(struct log_sample *sample,
                            const struct agent_stats *stats) {
    sample->monotonic_ns = agent_monotonic_ns();
    sample->captured_packets = atomic_load(&stats->capture_packets);
    sample->captured_bytes = atomic_load(&stats->capture_bytes);
    sample->sent_packets = atomic_load(&stats->records_sent);
    sample->sent_bytes = atomic_load(&stats->bytes_sent);
    sample->cpu_us = process_cpu_us();
}

static void log_tls_failure(const char *operation, SSL *ssl, int rc) {
    int ssl_error = SSL_get_error(ssl, rc);
    long verify_result = SSL_get_verify_result(ssl);
    unsigned long openssl_error = ERR_peek_last_error();
    char openssl_text[256] = "none";
    if (openssl_error) ERR_error_string_n(openssl_error, openssl_text, sizeof(openssl_text));
    log_msg("agent", "warn",
            "%s failed ssl_error=%d verify_result=%ld verify_error=%s openssl_error=%s",
            operation, ssl_error, verify_result,
            X509_verify_cert_error_string(verify_result), openssl_text);
}

static void reconnect_sleep(uint32_t seconds, unsigned *seed) {
    uint64_t base_ms = (uint64_t)seconds * 1000;
    uint64_t jitter_ms = base_ms ? agent_random_next(seed) % (base_ms / 4 + 1) : 0;
    uint64_t total = base_ms + jitter_ms;
    agent_sleep_interruptible(total > UINT32_MAX ? UINT32_MAX : (uint32_t)total,
                              &g_stop);
}

static int send_record(SSL *ssl, uint16_t type, const uint8_t *payload, uint32_t payload_len) {
    struct mirror_record_header hdr = {
        .magic = MIRROR_MAGIC,
        .version = MIRROR_VERSION,
        .record_type = type,
        .header_length = MIRROR_COMMON_HEADER_LEN,
        .payload_length = payload_len
    };
    uint32_t len = htonl(hdr.header_length + hdr.payload_length);
    uint8_t wh[MIRROR_COMMON_HEADER_LEN];
    mirror_record_header_to_wire(&hdr, wh);
    if (tls_write_all(ssl, &len, sizeof(len)) != 0) return -1;
    if (tls_write_all(ssl, wh, sizeof(wh)) != 0) return -1;
    return payload_len ? tls_write_all(ssl, payload, payload_len) : 0;
}

static int send_register(SSL *ssl, const struct agent_config *cfg, uint64_t epoch) {
    uint8_t buf[512];
    struct mirror_register_payload reg = {0};
    memcpy(reg.agent_uuid, cfg->agent_uuid, 16);
    reg.interface_id = cfg->interface_id;
    reg.interface_index = get_iface_index(cfg->capture_iface);
    get_iface_mac(cfg->capture_iface, reg.interface_mac);
    reg.interface_mtu = (uint32_t)get_iface_mtu(cfg->capture_iface);
    reg.max_capture_frame_size = cfg->max_capture_frame_size;
    reg.connection_epoch = epoch;
    agent_get_hostname(reg.hostname, sizeof(reg.hostname) - 1);
    snprintf(reg.interface_name, sizeof(reg.interface_name), "%s", cfg->capture_iface);
    snprintf(reg.agent_version, sizeof(reg.agent_version), "mirror-poc/1");
    agent_get_os_release(reg.kernel_release, sizeof(reg.kernel_release));
    mirror_register_to_wire(&reg, buf);
    return send_record(ssl, MIRROR_RECORD_REGISTER, buf, mirror_register_payload_len());
}

static int recv_register_ack(SSL *ssl, uint32_t max_record_size) {
    uint32_t len_n = 0;
    uint8_t hdr_buf[MIRROR_COMMON_HEADER_LEN];
    char err[128];
    if (tls_read_exact(ssl, &len_n, 4) != 0) return -1;
    uint32_t len = ntohl(len_n);
    if (len < MIRROR_COMMON_HEADER_LEN || len > max_record_size) return -1;
    if (tls_read_exact(ssl, hdr_buf, sizeof(hdr_buf)) != 0) return -1;
    struct mirror_record_header hdr;
    mirror_record_header_from_wire(hdr_buf, &hdr);
    if (mirror_validate_envelope(&hdr, len, max_record_size, err, sizeof(err)) != 0) return -1;
    if (hdr.record_type != MIRROR_RECORD_REGISTER_ACK || hdr.header_length != MIRROR_COMMON_HEADER_LEN ||
        hdr.payload_length != 0) return -1;
    uint32_t rest = len - MIRROR_COMMON_HEADER_LEN;
    uint8_t tmp[256];
    while (rest) {
        uint32_t n = rest > sizeof(tmp) ? sizeof(tmp) : rest;
        if (tls_read_exact(ssl, tmp, n) != 0) return -1;
        rest -= n;
    }
    return 0;
}

static int load_or_make_uuid(struct agent_config *cfg) {
    bool zero = true;
    for (int i = 0; i < 16; i++) if (cfg->agent_uuid[i]) zero = false;
    if (!zero) return 0;
    char s[64] = {0};
    FILE *f = cfg->uuid_file[0] ? fopen(cfg->uuid_file, "r") : NULL;
    if (f && fgets(s, sizeof(s), f) && parse_uuid(s, cfg->agent_uuid) == 0) { fclose(f); return 0; }
    if (f) fclose(f);
    if (agent_generate_uuid(cfg->agent_uuid) != 0) {
        log_msg("agent", "error", "could not generate agent UUID");
        return -1;
    }
    if (cfg->uuid_file[0]) {
        f = fopen(cfg->uuid_file, "w");
        if (f) { char uuid[40]; uuid_to_string(cfg->agent_uuid, uuid, sizeof(uuid)); fprintf(f, "%s\n", uuid); fclose(f); }
        else log_msg("agent", "warn", "cannot persist agent UUID to %s: %s", cfg->uuid_file, strerror(errno));
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--list-interfaces")) {
        if (network_init() != 0) return 1;
        int result = capture_list_interfaces() == 0 ? 0 : 1;
        network_cleanup();
        return result;
    }
    const char *config_path = argc > 1 ? argv[1] :
#ifdef _WIN32
        "mirror-agent.conf";
#else
        "examples/agent.conf";
#endif
    struct agent_config cfg;
    char err[256];
    if (load_agent_config(config_path, &cfg, err, sizeof(err)) != 0) {
        fprintf(stderr, "config error: %s\n", err);
        return 2;
    }
    if (load_or_make_uuid(&cfg) != 0) return 1;
    if (network_init() != 0) {
        fprintf(stderr, "network initialization failed\n");
        return 1;
    }
    agent_platform_install_stop_handler(&g_stop);
    tls_global_init();

    struct packet_queue queue;
    if (packet_queue_init(&queue, cfg.queue_max_packets, cfg.queue_max_bytes) != 0) {
        log_msg("agent", "error", "queue init failed");
        return 1;
    }
    struct packet_item *batch_items = calloc(cfg.batch_max_records, sizeof(*batch_items));
    struct wire_batch batch;
    if (!batch_items || wire_batch_init(&batch, cfg.batch_max_bytes) != 0) {
        log_msg("agent", "error", "batch buffer init failed");
        free(batch_items);
        packet_queue_destroy(&queue);
        return 1;
    }
    struct self_filter sf;
    if (self_filter_init(&sf) != 0) {
        wire_batch_destroy(&batch);
        free(batch_items);
        packet_queue_destroy(&queue);
        return 1;
    }
    struct agent_stats stats = {0};
    struct sender_metrics sender_metrics = {0};
    struct capture_context cap = { .cfg = &cfg, .queue = &queue, .self_filter = &sf, .stats = &stats };
    log_offload_state(cfg.capture_iface);
    agent_thread_t tid;
    bool capture_started = agent_thread_create(&tid, capture_thread_main, &cap) == 0;
    if (!capture_started) {
        log_msg("agent", "error", "capture thread creation failed");
        atomic_store(&g_stop, 1);
    }

    struct tls_client_config tc = {
        .ca_file = cfg.ca_file,
        .cert_file = cfg.client_cert_file,
        .key_file = cfg.client_key_file,
        .server_name = cfg.tls_server_name,
        .verify_peer = cfg.tls_verify_peer,
        .insecure = cfg.insecure_tls
    };
    SSL_CTX *ssl_ctx = tls_create_client_ctx(&tc, err, sizeof(err));
    if (!ssl_ctx) {
        log_msg("agent", "error", "%s", err);
        atomic_store(&g_stop, 1);
    }

    uint64_t epoch = now_ns();
    uint32_t backoff = cfg.reconnect_initial_sec ? cfg.reconnect_initial_sec : 1;
    unsigned jitter_seed = (unsigned)(epoch ^ (uint64_t)agent_process_id());
    uint64_t downtime_start = now_ns();
    while (!atomic_load(&g_stop) && ssl_ctx) {
        if (atomic_load(&cap.failed)) { log_msg("agent", "error", "capture thread stopped unexpectedly"); break; }
        atomic_fetch_add(&stats.connection_reconnects, 1);
        char local_ip[64] = "";
        uint16_t local_port = 0;
        mirror_socket_t fd = connect_tcp(cfg.receiver_host, cfg.receiver_port,
                                         local_ip, sizeof(local_ip), &local_port);
        if (fd == MIRROR_INVALID_SOCKET) {
            char socket_error[256];
            log_msg("agent", "warn", "connect failed: %s",
                    socket_error_string(socket_last_error(), socket_error,
                                        sizeof(socket_error)));
            reconnect_sleep(backoff, &jitter_seed);
            if (backoff < cfg.reconnect_max_sec) backoff = backoff > cfg.reconnect_max_sec / 2 ? cfg.reconnect_max_sec : backoff * 2;
            continue;
        }
        if (agent_set_socket_send_timeout(fd, cfg.transport_write_timeout_sec) != 0) {
            char socket_error[256];
            log_msg("agent", "warn", "could not set transport write timeout: %s",
                    socket_error_string(socket_last_error(), socket_error,
                                        sizeof(socket_error)));
        }
        char peer_ip[64] = "";
        agent_peer_ipv4(fd, peer_ip, sizeof(peer_ip));
        self_filter_update_ipv4(&sf, local_ip, local_port, peer_ip[0] ? peer_ip : cfg.receiver_host, cfg.receiver_port);
        SSL *ssl = SSL_new(ssl_ctx);
        if (!ssl) {
            log_msg("agent", "error", "SSL allocation failed");
            close_socket(fd);
            break;
        }
        SSL_set_fd(ssl, (int)(uintptr_t)fd);
        if (cfg.tls_server_name[0]) SSL_set_tlsext_host_name(ssl, cfg.tls_server_name);
        if (cfg.tls_verify_peer && !cfg.insecure_tls && cfg.tls_server_name[0] && SSL_set1_host(ssl, cfg.tls_server_name) != 1) {
            log_msg("agent", "error", "could not enable TLS hostname verification");
            SSL_free(ssl); close_socket(fd); break;
        }
        int connect_rc = SSL_connect(ssl);
        int register_rc = 0;
        int ack_rc = 0;
        if (connect_rc == 1) register_rc = send_register(ssl, &cfg, epoch);
        if (connect_rc == 1 && register_rc == 0) ack_rc = recv_register_ack(ssl, 65536);
        if (connect_rc != 1 || register_rc != 0 || ack_rc != 0) {
            if (connect_rc != 1) log_tls_failure("TLS handshake", ssl, connect_rc);
            else if (register_rc != 0) log_msg("agent", "warn", "REGISTER write failed");
            else log_msg("agent", "warn", "REGISTER_ACK read or validation failed");
            SSL_free(ssl); close_socket(fd); reconnect_sleep(backoff, &jitter_seed);
            if (backoff < cfg.reconnect_max_sec) backoff = backoff > cfg.reconnect_max_sec / 2 ? cfg.reconnect_max_sec : backoff * 2;
            continue;
        }
        atomic_fetch_add(&stats.connection_downtime_ms, (now_ns() - downtime_start) / 1000000ULL);
        backoff = cfg.reconnect_initial_sec ? cfg.reconnect_initial_sec : 1;
        log_msg("agent", "info", "connected receiver=%s:%u local=%s:%u", cfg.receiver_host, cfg.receiver_port, local_ip, local_port);
        uint64_t next_heartbeat = now_ns() + (uint64_t)cfg.heartbeat_interval_sec * 1000000000ULL;
        uint64_t next_stats = now_ns() + (uint64_t)cfg.stats_interval_sec * 1000000000ULL;
        uint64_t next_log = now_ns() + (uint64_t)cfg.log_interval_sec * 1000000000ULL;
        struct log_sample previous_log;
        init_log_sample(&previous_log, &stats);
        while (!atomic_load(&g_stop)) {
            if (atomic_load(&cap.failed)) { log_msg("agent", "error", "capture thread stopped unexpectedly"); atomic_store(&g_stop, 1); break; }
            size_t batch_count = 0;
            uint64_t batch_wire_bytes = 0;
            int pop = packet_queue_pop_batch_timed(
                &queue, batch_items, cfg.batch_max_records,
                cfg.batch_max_records, cfg.batch_max_bytes,
                (uint32_t)wire_packet_record_overhead(), 500,
                cfg.batch_linger_us, &batch_count, &batch_wire_bytes);
            if (pop < 0) break;
            if (pop == 0) {
                wire_batch_reset(&batch);
                uint64_t dequeue_ns = agent_monotonic_ns();
                int serialize_error = 0;
                for (size_t i = 0; i < batch_count; i++) {
                    uint64_t residence_ns = dequeue_ns >= batch_items[i].enqueue_monotonic_ns ?
                        dequeue_ns - batch_items[i].enqueue_monotonic_ns : 0;
                    observe_residence(&sender_metrics, residence_ns);
                    if (wire_batch_append_packet(&batch, &cfg, epoch,
                                                 &batch_items[i]) != 0) {
                        serialize_error = 1;
                        break;
                    }
                }
                if (!serialize_error && batch.length != batch_wire_bytes)
                    serialize_error = 1;
                uint64_t write_calls = 0;
                int write_error = serialize_error;
                if (!serialize_error) {
                    uint64_t write_started = agent_monotonic_ns();
                    sender_metrics.tls_write_operations++;
                    write_error = tls_write_all_counted(ssl, batch.data,
                                                        batch.length,
                                                        &write_calls) != 0;
                    uint64_t write_finished = agent_monotonic_ns();
                    uint64_t write_duration = write_finished >= write_started ?
                        write_finished - write_started : 0;
                    sender_metrics.ssl_write_calls += write_calls;
                    sender_metrics.ssl_write_duration_ns += write_duration;
                    if (write_duration > sender_metrics.ssl_write_duration_max_ns)
                        sender_metrics.ssl_write_duration_max_ns = write_duration;
                }
                if (write_error) {
                    atomic_fetch_add(&stats.transport_send_errors, 1);
                    sender_metrics.failed_batches++;
                    sender_metrics.failed_batch_records += batch_count;
                } else {
                    sender_metrics.batches_sent++;
                    sender_metrics.batch_wire_bytes += batch.length;
                    if (batch.records > sender_metrics.batch_max_records)
                        sender_metrics.batch_max_records = batch.records;
                    if (batch.length > sender_metrics.batch_max_wire_bytes)
                        sender_metrics.batch_max_wire_bytes = batch.length;
                    atomic_fetch_add(&stats.records_sent, batch.records);
                    atomic_fetch_add(&stats.bytes_sent, batch.frame_bytes);
                }
                for (size_t i = 0; i < batch_count; i++)
                    packet_item_free(&batch_items[i]);
                if (write_error) break;
            }
            uint64_t now = now_ns();
            if (now >= next_heartbeat) {
                if (send_record(ssl, MIRROR_RECORD_HEARTBEAT, NULL, 0) != 0) break;
                next_heartbeat = now + (uint64_t)cfg.heartbeat_interval_sec * 1000000000ULL;
            }
            if (now >= next_stats) {
                if (send_record(ssl, MIRROR_RECORD_STATS, NULL, 0) != 0) break;
                next_stats = now + (uint64_t)cfg.stats_interval_sec * 1000000000ULL;
            }
            if (now >= next_log) {
                struct packet_queue_snapshot queue_snapshot;
                packet_queue_get_snapshot(&queue, &queue_snapshot);
                struct log_sample current_log;
                init_log_sample(&current_log, &stats);
                uint64_t elapsed_ns = current_log.monotonic_ns > previous_log.monotonic_ns ?
                    current_log.monotonic_ns - previous_log.monotonic_ns : 1;
                double elapsed_sec = (double)elapsed_ns / 1000000000.0;
                double ingress_pps = (double)(current_log.captured_packets - previous_log.captured_packets) / elapsed_sec;
                double ingress_mbps = (double)(current_log.captured_bytes - previous_log.captured_bytes) * 8.0 / elapsed_sec / 1000000.0;
                double egress_pps = (double)(current_log.sent_packets - previous_log.sent_packets) / elapsed_sec;
                double egress_mbps = (double)(current_log.sent_bytes - previous_log.sent_bytes) * 8.0 / elapsed_sec / 1000000.0;
                double cpu_percent = (double)(current_log.cpu_us - previous_log.cpu_us) * 100000.0 / (double)elapsed_ns;
                double batch_avg_records = sender_metrics.batches_sent ?
                    (double)current_log.sent_packets / sender_metrics.batches_sent : 0.0;
                double batch_avg_wire_bytes = sender_metrics.batches_sent ?
                    (double)sender_metrics.batch_wire_bytes / sender_metrics.batches_sent : 0.0;
                double ssl_write_avg_us = sender_metrics.tls_write_operations ?
                    (double)sender_metrics.ssl_write_duration_ns /
                    sender_metrics.tls_write_operations / 1000.0 : 0.0;
                double residence_avg_us = sender_metrics.residence_samples ?
                    (double)sender_metrics.residence_sum_ns /
                    sender_metrics.residence_samples / 1000.0 : 0.0;
                log_msg("agent", "info", "connection_state=connected epoch=%llu sequence=%llu reconnects=%llu downtime_ms=%llu captured=%llu sent=%llu ingress_pps=%.2f ingress_mbps=%.2f egress_pps=%.2f egress_mbps=%.2f queue_packets=%zu queue_bytes=%llu queue_peak_packets=%zu queue_peak_bytes=%llu queue_dropped=%llu queue_dropped_bytes=%llu batches_sent=%llu batch_failed=%llu batch_failed_records=%llu batch_avg_records=%.2f batch_avg_wire_bytes=%.2f batch_max_records=%llu batch_max_wire_bytes=%llu ssl_write_operations=%llu ssl_write_calls=%llu ssl_write_avg_us=%.2f ssl_write_max_us=%llu queue_residence_avg_us=%.2f queue_residence_p50_us=%llu queue_residence_p95_us=%llu queue_residence_p99_us=%llu queue_residence_max_us=%llu cpu_percent=%.2f rss_bytes=%llu truncated=%llu kernel_truncated=%llu oversized_dropped=%llu segmented_aggregates=%llu generated_segments=%llu forwarded_aggregates=%llu segmentation_failures=%llu ring_dropped=%llu checksum_not_ready=%llu self_filtered=%llu self_filtered_bytes=%llu send_errors=%llu alloc_failures=%llu",
                        (unsigned long long)epoch, (unsigned long long)atomic_load(&stats.sequence_number_current),
                        (unsigned long long)atomic_load(&stats.connection_reconnects),
                        (unsigned long long)atomic_load(&stats.connection_downtime_ms),
                        (unsigned long long)atomic_load(&stats.capture_packets), (unsigned long long)atomic_load(&stats.records_sent),
                        ingress_pps, ingress_mbps, egress_pps, egress_mbps,
                        queue_snapshot.packets, (unsigned long long)queue_snapshot.bytes,
                        queue_snapshot.peak_packets, (unsigned long long)queue_snapshot.peak_bytes,
                        (unsigned long long)queue_snapshot.dropped_packets,
                        (unsigned long long)queue_snapshot.dropped_bytes,
                        (unsigned long long)sender_metrics.batches_sent,
                        (unsigned long long)sender_metrics.failed_batches,
                        (unsigned long long)sender_metrics.failed_batch_records,
                        batch_avg_records, batch_avg_wire_bytes,
                        (unsigned long long)sender_metrics.batch_max_records,
                        (unsigned long long)sender_metrics.batch_max_wire_bytes,
                        (unsigned long long)sender_metrics.tls_write_operations,
                        (unsigned long long)sender_metrics.ssl_write_calls,
                        ssl_write_avg_us,
                        (unsigned long long)(sender_metrics.ssl_write_duration_max_ns / 1000ULL),
                        residence_avg_us,
                        (unsigned long long)residence_percentile_us(&sender_metrics, 50),
                        (unsigned long long)residence_percentile_us(&sender_metrics, 95),
                        (unsigned long long)residence_percentile_us(&sender_metrics, 99),
                        (unsigned long long)(sender_metrics.residence_max_ns / 1000ULL),
                        cpu_percent, (unsigned long long)current_rss_bytes(),
                        (unsigned long long)atomic_load(&stats.capture_truncated_packets),
                        (unsigned long long)atomic_load(&stats.capture_kernel_truncated_packets),
                        (unsigned long long)atomic_load(&stats.capture_oversized_dropped_packets),
                        (unsigned long long)atomic_load(&stats.capture_segmented_aggregates),
                        (unsigned long long)atomic_load(&stats.capture_generated_segments),
                        (unsigned long long)atomic_load(&stats.capture_forwarded_aggregates),
                        (unsigned long long)atomic_load(&stats.capture_segmentation_failures),
                        (unsigned long long)atomic_load(&stats.kernel_ring_dropped_packets),
                        (unsigned long long)atomic_load(&stats.checksum_not_ready_packets),
                        (unsigned long long)atomic_load(&stats.self_transport_packets_filtered),
                        (unsigned long long)atomic_load(&stats.self_transport_bytes_filtered),
                        (unsigned long long)atomic_load(&stats.transport_send_errors),
                        (unsigned long long)atomic_load(&stats.capture_alloc_failures));
                previous_log = current_log;
                next_log = now + (uint64_t)cfg.log_interval_sec * 1000000000ULL;
            }
        }
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close_socket(fd);
        downtime_start = now_ns();
        if (!atomic_load(&g_stop)) reconnect_sleep(backoff, &jitter_seed);
    }
    atomic_store(&cap.stop, 1);
    packet_queue_close(&queue);
    if (capture_started) agent_thread_join(tid);
    if (ssl_ctx) SSL_CTX_free(ssl_ctx);
    wire_batch_destroy(&batch);
    free(batch_items);
    packet_queue_destroy(&queue);
    self_filter_destroy(&sf);
    network_cleanup();
    return 0;
}
