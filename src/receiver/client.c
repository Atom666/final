#include <receiver/client.h>
#include <receiver/checksum.h>
#include <mirror/protocol.h>
#include <mirror/tls.h>
#include <mirror/util.h>
#include <mirror/vlan.h>
#include <receiver/fingerprint.h>
#include <receiver/pipeline.h>
#include <agent/segment.h>

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>

static void stats_add(uint64_t *p, uint64_t v) {
    __atomic_add_fetch(p, v, __ATOMIC_RELAXED);
}

static void log_tls_failure(const char *operation, SSL *ssl, int rc) {
    int ssl_error = SSL_get_error(ssl, rc);
    long verify_result = SSL_get_verify_result(ssl);
    unsigned long openssl_error = ERR_peek_last_error();
    char openssl_text[256] = "none";
    if (openssl_error) ERR_error_string_n(openssl_error, openssl_text, sizeof(openssl_text));
    log_msg("receiver", "warn",
            "%s failed ssl_error=%d verify_result=%ld verify_error=%s openssl_error=%s",
            operation, ssl_error, verify_result,
            X509_verify_cert_error_string(verify_result), openssl_text);
}

static struct agent_metric *registry_register(struct client_thread_arg *arg,
                                               const struct mirror_register_payload *reg) {
    struct agent_metric *found = NULL, *free_slot = NULL;
    pthread_mutex_lock(&arg->registry->mutex);
    for (size_t i = 0; i < arg->registry->capacity; i++) {
        struct agent_metric *m = &arg->registry->items[i];
        if (!m->used && !free_slot) free_slot = m;
        if (m->used && m->interface_id == reg->interface_id && memcmp(m->uuid, reg->agent_uuid, 16) == 0) { found = m; break; }
    }
    if (!found && free_slot) { found = free_slot; memset(found, 0, sizeof(*found)); found->used = 1; memcpy(found->uuid, reg->agent_uuid, 16); found->interface_id = reg->interface_id; }
    if (found) {
        if (found->epoch && found->epoch != reg->connection_epoch) { stats_add(&arg->stats->agent_epoch_changes, 1); found->last_sequence = 0; }
        found->epoch = reg->connection_epoch; found->connected = 1; found->last_seen_ns = now_ns();
    }
    pthread_mutex_unlock(&arg->registry->mutex); return found;
}

static void registry_received(struct client_thread_arg *arg, struct agent_metric *m,
                              uint32_t bytes) {
    pthread_mutex_lock(&arg->registry->mutex);
    m->packets_received++;
    m->bytes_received += bytes;
    m->last_seen_ns = now_ns();
    pthread_mutex_unlock(&arg->registry->mutex);
}

static void registry_transport_accepted(struct client_thread_arg *arg,
                                        struct agent_metric *m,
                                        uint64_t sequence, uint64_t gap) {
    pthread_mutex_lock(&arg->registry->mutex);
    m->gaps += gap;
    m->last_sequence = sequence;
    m->transport_accepted++;
    pthread_mutex_unlock(&arg->registry->mutex);
}

static void registry_duplicate(struct client_thread_arg *arg, struct agent_metric *m,
                               enum transport_result result) {
    pthread_mutex_lock(&arg->registry->mutex);
    if (result == TRANSPORT_DUPLICATE) m->transport_duplicates++;
    else if (result == TRANSPORT_OUT_OF_ORDER) m->out_of_order++;
    pthread_mutex_unlock(&arg->registry->mutex);
}

static void registry_cross_duplicate(struct client_thread_arg *arg, struct agent_metric *m) {
    pthread_mutex_lock(&arg->registry->mutex);
    m->cross_agent_duplicates++;
    pthread_mutex_unlock(&arg->registry->mutex);
}

static void registry_checksum_repair(struct client_thread_arg *arg,
                                     struct agent_metric *m, bool success) {
    pthread_mutex_lock(&arg->registry->mutex);
    if (success) m->checksum_repairs++;
    else m->checksum_repair_failures++;
    pthread_mutex_unlock(&arg->registry->mutex);
}

static void registry_segmentation(struct client_thread_arg *arg,
                                  struct agent_metric *m, uint64_t segments) {
    pthread_mutex_lock(&arg->registry->mutex);
    m->segmented_aggregates++;
    m->generated_segments += segments;
    pthread_mutex_unlock(&arg->registry->mutex);
}

enum agent_packet_outcome {
    AGENT_OUTPUT_SUCCESS,
    AGENT_OUTPUT_ERROR,
    AGENT_OUTPUT_OVERSIZED,
    AGENT_PROCESSING_ERROR
};

static void registry_outcome(struct client_thread_arg *arg, struct agent_metric *m,
                             enum agent_packet_outcome outcome, uint32_t bytes) {
    pthread_mutex_lock(&arg->registry->mutex);
    if (outcome == AGENT_OUTPUT_SUCCESS) {
        m->output_packets++;
        m->output_bytes += bytes;
    } else if (outcome == AGENT_OUTPUT_ERROR) {
        m->output_errors++;
    } else if (outcome == AGENT_OUTPUT_OVERSIZED) {
        m->oversized_packets++;
    } else {
        m->processing_errors++;
    }
    pthread_mutex_unlock(&arg->registry->mutex);
}

void receiver_log_agent_metrics(struct agent_registry *registry) {
    struct agent_metric *snapshot = calloc(registry->capacity, sizeof(*snapshot));
    if (!snapshot) {
        log_msg("receiver", "warn", "per-agent metrics snapshot allocation failed");
        return;
    }
    pthread_mutex_lock(&registry->mutex);
    memcpy(snapshot, registry->items, registry->capacity * sizeof(*snapshot));
    pthread_mutex_unlock(&registry->mutex);

    for (size_t i = 0; i < registry->capacity; i++) {
        const struct agent_metric *m = &snapshot[i];
        if (!m->used || !m->connected) continue;
        uint64_t transport_accounted = m->transport_accepted +
            m->transport_duplicates + m->out_of_order;
        uint64_t transport_unaccounted = m->packets_received > transport_accounted ?
            m->packets_received - transport_accounted : 0;
        uint64_t processing_expected = m->transport_accepted +
            m->generated_segments - m->segmented_aggregates;
        uint64_t processing_accounted = m->cross_agent_duplicates +
            m->output_packets + m->output_errors + m->oversized_packets +
            m->processing_errors + m->output_queue_drops;
        uint64_t processing_in_flight = processing_expected > processing_accounted ?
            processing_expected - processing_accounted : 0;
        char uuid[40];
        uuid_to_string(m->uuid, uuid, sizeof(uuid));
        log_msg("receiver-agent", "info",
                "agent_uuid=%s interface_id=%u epoch=%llu connected=%d packets_received=%llu bytes_received=%llu transport_accepted=%llu transport_duplicates=%llu transport_out_of_order=%llu sequence_gaps=%llu segmented_aggregates=%llu generated_segments=%llu cross_agent_duplicates=%llu output_packets=%llu output_bytes=%llu output_errors=%llu output_queue_drops=%llu oversized=%llu processing_errors=%llu checksum_repairs=%llu checksum_repair_failures=%llu transport_unaccounted=%llu processing_in_flight=%llu",
                uuid, m->interface_id, (unsigned long long)m->epoch, m->connected,
                (unsigned long long)m->packets_received,
                (unsigned long long)m->bytes_received,
                (unsigned long long)m->transport_accepted,
                (unsigned long long)m->transport_duplicates,
                (unsigned long long)m->out_of_order,
                (unsigned long long)m->gaps,
                (unsigned long long)m->segmented_aggregates,
                (unsigned long long)m->generated_segments,
                (unsigned long long)m->cross_agent_duplicates,
                (unsigned long long)m->output_packets,
                (unsigned long long)m->output_bytes,
                (unsigned long long)m->output_errors,
                (unsigned long long)m->output_queue_drops,
                (unsigned long long)m->oversized_packets,
                (unsigned long long)m->processing_errors,
                (unsigned long long)m->checksum_repairs,
                (unsigned long long)m->checksum_repair_failures,
                (unsigned long long)transport_unaccounted,
                (unsigned long long)processing_in_flight);
    }
    free(snapshot);
}

static int send_ack(SSL *ssl) {
    struct mirror_record_header hdr = {
        .magic = MIRROR_MAGIC,
        .version = MIRROR_VERSION,
        .record_type = MIRROR_RECORD_REGISTER_ACK,
        .header_length = MIRROR_COMMON_HEADER_LEN,
        .payload_length = 0
    };
    uint32_t len = htonl(MIRROR_COMMON_HEADER_LEN);
    uint8_t wh[MIRROR_COMMON_HEADER_LEN];
    mirror_record_header_to_wire(&hdr, wh);
    return tls_write_all(ssl, &len, 4) || tls_write_all(ssl, wh, sizeof(wh)) ? -1 : 0;
}

static int read_record(SSL *ssl, const struct receiver_config *cfg, struct mirror_record_header *hdr, uint8_t **payload) {
    uint32_t len_n = 0;
    uint8_t wh[MIRROR_COMMON_HEADER_LEN];
    char err[160];
    *payload = NULL;
    if (tls_read_exact(ssl, &len_n, 4) != 0) return -1;
    uint32_t len = ntohl(len_n);
    if (len < MIRROR_COMMON_HEADER_LEN || len > cfg->max_record_size) return -2;
    if (tls_read_exact(ssl, wh, sizeof(wh)) != 0) return -1;
    mirror_record_header_from_wire(wh, hdr);
    if (mirror_validate_envelope(hdr, len, cfg->max_record_size, err, sizeof(err)) != 0) {
        log_msg("receiver", "warn", "protocol_error=%s", err);
        return -2;
    }
    if (hdr->header_length != MIRROR_COMMON_HEADER_LEN) return -2;
    if (hdr->payload_length) {
        *payload = malloc(hdr->payload_length);
        if (!*payload) return -2;
        if (tls_read_exact(ssl, *payload, hdr->payload_length) != 0) {
            free(*payload); *payload = NULL; return -1;
        }
    }
    return 0;
}

struct pipeline_submit_context {
    struct output_pipeline *pipeline;
    const uint8_t *frame;
    uint32_t frame_len;
    uint64_t adjusted_timestamp_ns;
    bool timestamp_valid;
    struct agent_metric *agent;
};

static int submit_pipeline_packet(void *context) {
    struct pipeline_submit_context *submit = context;
    return output_pipeline_submit(submit->pipeline, submit->frame, submit->frame_len,
                                  submit->adjusted_timestamp_ns,
                                  submit->timestamp_valid, submit->agent);
}

static int process_output_frame(struct client_thread_arg *arg,
                                const struct mirror_register_payload *reg,
                                struct agent_metric *agent,
                                const struct mirror_packet_meta *meta,
                                const uint8_t *frame, size_t frame_len,
                                uint32_t flags, uint64_t *cross_duplicates) {
    if (flags & MIRROR_FLAG_CHECKSUM_NOT_READY) {
        enum checksum_repair_result repair =
            checksum_repair_offload_frame((uint8_t *)frame, frame_len);
        if (repair == CHECKSUM_REPAIR_OK) {
            stats_add(&arg->stats->receiver_checksum_repairs_total, 1);
            registry_checksum_repair(arg, agent, true);
        } else {
            stats_add(&arg->stats->receiver_checksum_repair_failures_total, 1);
            registry_checksum_repair(arg, agent, false);
        }
    }
    struct packet_fingerprint fingerprint;
    enum fingerprint_status fp_status = fingerprint_packet(frame, frame_len, &fingerprint);
    if ((flags & MIRROR_FLAG_GSO_OR_GRO_SUSPECTED) ||
        (fp_status != FINGERPRINT_FAIL_OPEN && reg->interface_mtu &&
         fingerprint.ip_packet_length > reg->interface_mtu))
        stats_add(&arg->stats->receiver_dedup_unhandled_offload_suspected_total, 1);
    uint64_t adjusted = 0;
    bool timestamp_valid = dedup_adjust_timestamp(arg->cfg, meta->agent_uuid,
                                                   meta->timestamp_ns, &adjusted);
    struct pipeline_submit_context submit = {
        .pipeline = arg->pipeline,
        .frame = frame,
        .frame_len = (uint32_t)frame_len,
        .adjusted_timestamp_ns = adjusted,
        .timestamp_valid = timestamp_valid,
        .agent = agent
    };
    if (arg->cfg->dedup_enabled && fp_status != FINGERPRINT_FAIL_OPEN) {
        if (fp_status == FINGERPRINT_FALLBACK)
            stats_add(&arg->stats->receiver_dedup_fallback_total, 1);
        enum dedup_submit_result result = dedup_cache_submit(arg->dedup, &fingerprint,
                meta->agent_uuid, meta->timestamp_ns, timestamp_valid, adjusted,
                receiver_monotonic_ns(), submit_pipeline_packet, &submit);
        if (result == DEDUP_SUBMIT_CROSS_AGENT_DUPLICATE) {
            stats_add(&arg->stats->receiver_cross_agent_duplicates_total, 1);
            registry_cross_duplicate(arg, agent);
            (*cross_duplicates)++;
        }
        return 0;
    }
    (void)submit_pipeline_packet(&submit);
    return 0;
}

static int handle_packet(struct client_thread_arg *arg, const struct mirror_register_payload *reg,
                         struct agent_metric *agent,
                         const uint8_t *payload, uint32_t payload_len,
                         uint64_t *last_seq, uint64_t *seq_gaps,
                         uint64_t *transport_duplicates, uint64_t *out_of_order,
                         uint64_t *cross_duplicates) {
    size_t meta_len = mirror_packet_meta_len();
    if (payload_len < meta_len) return -1;
    struct mirror_packet_meta meta;
    if (mirror_packet_meta_from_wire(payload, payload_len, &meta) != 0) return -1;
    if (meta.captured_length != payload_len - meta_len) return -1;
    if (meta.original_length < meta.captured_length ||
        (meta.flags & MIRROR_FLAG_FRAME_TRUNCATED) ||
        memcmp(meta.agent_uuid, reg->agent_uuid, 16) != 0 ||
        meta.interface_id != reg->interface_id || meta.connection_epoch != reg->connection_epoch)
        return -1;
    if ((meta.flags & MIRROR_FLAG_VLAN_METADATA_PRESENT) &&
        (meta.vlan_tag_count == 0 || meta.vlan_tag_count > MIRROR_MAX_VLAN_TAGS)) return -1;
    if (!(meta.flags & MIRROR_FLAG_VLAN_METADATA_PRESENT) && meta.vlan_tag_count != 0) return -1;
    stats_add(&arg->stats->agent_packets_received, 1);
    stats_add(&arg->stats->agent_bytes_received, meta.captured_length);
    registry_received(arg, agent, meta.captured_length);
    uint64_t gap = 0;
    enum transport_result transport = transport_registry_check(arg->transport, meta.agent_uuid,
            meta.connection_epoch, meta.sequence_number, receiver_monotonic_ns(), &gap);
    if (transport != TRANSPORT_ACCEPT) {
        registry_duplicate(arg, agent, transport);
        if (transport == TRANSPORT_DUPLICATE) {
            stats_add(&arg->stats->receiver_transport_duplicates_total, 1);
            (*transport_duplicates)++;
        } else {
            stats_add(&arg->stats->receiver_transport_out_of_order_total, 1);
            (*out_of_order)++;
        }
        return 0;
    }
    if (gap) { stats_add(&arg->stats->agent_sequence_gaps, gap); *seq_gaps += gap; }
    registry_transport_accepted(arg, agent, meta.sequence_number, gap);
    if (meta.captured_length > arg->cfg->max_input_frame_size) {
        stats_add(&arg->stats->output_oversized_packets, 1);
        registry_outcome(arg, agent, AGENT_OUTPUT_OVERSIZED, 0);
        log_msg("receiver", "error", "oversized input frame=%u max=%u",
                meta.captured_length, arg->cfg->max_input_frame_size);
        return 0;
    }
    *last_seq = meta.sequence_number;
    const uint8_t *frame = payload + meta_len;
    uint8_t *owned_frame = NULL;
    const uint8_t *send_frame = frame;
    size_t send_len = meta.captured_length;
    if (meta.flags & MIRROR_FLAG_VLAN_METADATA_PRESENT) {
        size_t restored_capacity = send_len + (size_t)meta.vlan_tag_count * 4u;
        owned_frame = malloc(restored_capacity);
        if (!owned_frame) {
            registry_outcome(arg, agent, AGENT_PROCESSING_ERROR, 0);
            return -1;
        }
        uint16_t tpids[MIRROR_MAX_VLAN_TAGS] = {meta.vlan_tpid};
        uint16_t tcis[MIRROR_MAX_VLAN_TAGS] = {meta.vlan_tci};
        for (size_t i = 1; i < meta.vlan_tag_count; i++) {
            tpids[i] = meta.extra_vlan_tpid[i - 1]; tcis[i] = meta.extra_vlan_tci[i - 1];
        }
        if (vlan_restore_tags(frame, meta.captured_length, tpids, tcis, meta.vlan_tag_count,
                              owned_frame, restored_capacity, &send_len) != 0) {
            free(owned_frame);
            registry_outcome(arg, agent, AGENT_PROCESSING_ERROR, 0);
            return -1;
        }
        send_frame = owned_frame;
    } else if (meta.flags & MIRROR_FLAG_CHECKSUM_NOT_READY) {
        owned_frame = malloc(send_len);
        if (!owned_frame) {
            registry_outcome(arg, agent, AGENT_PROCESSING_ERROR, 0);
            return -1;
        }
        memcpy(owned_frame, send_frame, send_len);
        send_frame = owned_frame;
    }

    uint32_t source_mtu = reg->interface_mtu;
    bool aggregate_suspected = (meta.flags & MIRROR_FLAG_GSO_OR_GRO_SUSPECTED) ||
        (source_mtu >= 68 && send_len > (size_t)source_mtu + ETH_HLEN +
                                      MIRROR_MAX_VLAN_TAGS * 4u);
    if (aggregate_suspected && source_mtu >= 68) {
        struct segmented_batch batch = {0};
        enum segment_result segment_result =
            segment_tcp_frame(send_frame, send_len, source_mtu, &batch);
        if (segment_result == SEGMENT_OK) {
            stats_add(&arg->stats->receiver_segmented_aggregates_total, 1);
            stats_add(&arg->stats->receiver_generated_segments_total, batch.count);
            registry_segmentation(arg, agent, batch.count);
            uint32_t segment_flags = (meta.flags |
                MIRROR_FLAG_SOFTWARE_SEGMENTED) &
                ~(MIRROR_FLAG_CHECKSUM_NOT_READY | MIRROR_FLAG_GSO_OR_GRO_SUSPECTED);
            for (size_t i = 0; i < batch.count; i++)
                process_output_frame(arg, reg, agent, &meta, batch.frames[i].data,
                                     batch.frames[i].len, segment_flags,
                                     cross_duplicates);
            segmented_batch_free(&batch);
            free(owned_frame);
            return 0;
        }
        if (segment_result != SEGMENT_NOT_NEEDED)
            stats_add(&arg->stats->receiver_segmentation_failures_total, 1);
    }
    if (send_len > arg->cfg->max_frame_size) {
        if (aggregate_suspected)
            stats_add(&arg->stats->receiver_dedup_unhandled_offload_suspected_total, 1);
        stats_add(&arg->stats->output_oversized_packets, 1);
        registry_outcome(arg, agent, AGENT_OUTPUT_OVERSIZED, 0);
        log_msg("receiver", "error",
                "unhandled oversized frame=%zu output_max=%u source_mtu=%u",
                send_len, arg->cfg->max_frame_size, source_mtu);
        free(owned_frame);
        return 0;
    }
    process_output_frame(arg, reg, agent, &meta, send_frame, send_len,
                         meta.flags, cross_duplicates);
    free(owned_frame);
    return 0;
}

static void set_socket_timeout(int fd, uint32_t seconds) {
    struct timeval tv = { .tv_sec = seconds ? (time_t)seconds : 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int certificate_matches_uuid(SSL *ssl, const uint8_t uuid_bytes[16]) {
    X509 *cert = SSL_get1_peer_certificate(ssl);
    if (!cert) return -1;
    char cn[128] = {0}, uuid[40];
    int n = X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, cn, sizeof(cn));
    X509_free(cert); uuid_to_string(uuid_bytes, uuid, sizeof(uuid));
    return n > 0 && strcasecmp(cn, uuid) == 0 ? 0 : -1;
}

void *receiver_client_thread(void *argp) {
    struct client_thread_arg *arg = argp;
    int fd = arg->fd;
    set_socket_timeout(fd, arg->cfg->registration_timeout_sec);
    SSL *ssl = SSL_new(arg->ctx);
    SSL_set_fd(ssl, fd);
    int accept_rc = SSL_accept(ssl);
    if (accept_rc != 1) {
        stats_add(&arg->stats->tls_handshake_errors, 1);
        log_tls_failure("TLS handshake", ssl, accept_rc);
        SSL_free(ssl); close(fd); stats_add(&arg->stats->active_connections, (uint64_t)-1); free(arg); return NULL;
    }
    bool registered = false;
    uint64_t last_seq = 0, seq_gaps = 0, transport_duplicates = 0;
    uint64_t out_of_order = 0, cross_duplicates = 0;
    struct mirror_register_payload reg = {0};
    struct agent_metric *agent = NULL;
    for (;;) {
        struct mirror_record_header hdr;
        uint8_t *payload = NULL;
        int rr = read_record(ssl, arg->cfg, &hdr, &payload);
        if (rr != 0) {
            if (rr == -2) stats_add(&arg->stats->protocol_errors, 1);
            free(payload);
            break;
        }
        stats_add(&arg->stats->records_received, 1);
        stats_add(&arg->stats->bytes_received, hdr.payload_length);
        if (!registered) {
            if (hdr.record_type != MIRROR_RECORD_REGISTER ||
                mirror_register_from_wire(payload, hdr.payload_length, &reg) != 0) {
                stats_add(&arg->stats->protocol_errors, 1);
                free(payload); break;
            }
            if (reg.interface_mtu < 68 ||
                reg.max_capture_frame_size > arg->cfg->max_input_frame_size) {
                log_msg("receiver", "warn",
                        "registration limits rejected interface_mtu=%u agent_max_capture=%u receiver_max_input=%u",
                        reg.interface_mtu, reg.max_capture_frame_size,
                        arg->cfg->max_input_frame_size);
                stats_add(&arg->stats->protocol_errors, 1);
                free(payload); break;
            }
            if (arg->cfg->require_client_certificate && !arg->cfg->insecure_tls &&
                certificate_matches_uuid(ssl, reg.agent_uuid) != 0) {
                log_msg("receiver", "warn", "client certificate CN does not match registered agent UUID");
                stats_add(&arg->stats->protocol_errors, 1); free(payload); break;
            }
            agent = registry_register(arg, &reg);
            if (!agent) { stats_add(&arg->stats->rejected_connections, 1); free(payload); break; }
            char registered_uuid[40]; uuid_to_string(reg.agent_uuid, registered_uuid, sizeof(registered_uuid));
            log_msg("receiver", "info", "agent_uuid=%s interface_id=%u epoch=%llu hostname=%s interface=%s agent_version=%s kernel=%s registered",
                    registered_uuid, reg.interface_id, (unsigned long long)reg.connection_epoch,
                    reg.hostname, reg.interface_name, reg.agent_version, reg.kernel_release);
            registered = true;
            transport_registry_register(arg->transport, reg.agent_uuid, reg.connection_epoch,
                                        receiver_monotonic_ns());
            stats_add(&arg->stats->active_agents, 1);
            set_socket_timeout(fd, arg->cfg->heartbeat_timeout_sec);
            if (send_ack(ssl) != 0) { free(payload); break; }
        } else if (hdr.record_type == MIRROR_RECORD_PACKET) {
            if (handle_packet(arg, &reg, agent, payload, hdr.payload_length, &last_seq, &seq_gaps,
                              &transport_duplicates, &out_of_order, &cross_duplicates) != 0) {
                stats_add(&arg->stats->protocol_errors, 1);
                free(payload); break;
            }
        } else if (hdr.record_type == MIRROR_RECORD_HEARTBEAT || hdr.record_type == MIRROR_RECORD_STATS) {
        } else {
            stats_add(&arg->stats->protocol_errors, 1);
            free(payload); break;
        }
        free(payload);
    }
    if (registered) {
        stats_add(&arg->stats->active_agents, (uint64_t)-1);
        transport_registry_unregister(arg->transport, reg.agent_uuid, reg.connection_epoch);
    }
    if (agent) { pthread_mutex_lock(&arg->registry->mutex); agent->connected = 0; pthread_mutex_unlock(&arg->registry->mutex); }
    char uuid[40]; uuid_to_string(reg.agent_uuid, uuid, sizeof(uuid));
    log_msg("receiver", "info", "agent_uuid=%s interface_id=%u epoch=%llu disconnected seq_gaps=%llu transport_duplicates=%llu out_of_order=%llu cross_agent_duplicates=%llu",
            uuid, reg.interface_id, (unsigned long long)reg.connection_epoch,
            (unsigned long long)seq_gaps, (unsigned long long)transport_duplicates,
            (unsigned long long)out_of_order, (unsigned long long)cross_duplicates);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);
    stats_add(&arg->stats->active_connections, (uint64_t)-1);
    free(arg);
    return NULL;
}
