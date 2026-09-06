#include <mirror/config.h>
#include <mirror/protocol.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = 0;
    return s;
}

static int parse_bool(const char *v, bool *out) {
    if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcmp(v, "1")) { *out = true; return 0; }
    if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcmp(v, "0")) { *out = false; return 0; }
    return -1;
}

static int parse_u32(const char *v, uint32_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long n = strtoul(v, &end, 0);
    if (errno || !end || *end || n > UINT32_MAX) return -1;
    *out = (uint32_t)n; return 0;
}

static int parse_u64(const char *v, uint64_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long long n = strtoull(v, &end, 0);
    if (errno || !end || *end) return -1;
    *out = (uint64_t)n; return 0;
}

static int parse_i64(const char *v, int64_t *out) {
    char *end = NULL;
    errno = 0;
    long long n = strtoll(v, &end, 0);
    if (errno || !end || *end) return -1;
    *out = (int64_t)n;
    return 0;
}

int parse_uuid(const char *s, uint8_t out[MIRROR_UUID_LEN]) {
    char hex[33];
    size_t j = 0;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '-') continue;
        if (!isxdigit((unsigned char)s[i]) || j >= sizeof(hex) - 1) return -1;
        hex[j++] = s[i];
    }
    if (j != 32) return -1;
    hex[32] = 0;
    for (int i = 0; i < 16; i++) {
        char b[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        out[i] = (uint8_t)strtoul(b, NULL, 16);
    }
    return 0;
}

void uuid_to_string(const uint8_t uuid[MIRROR_UUID_LEN], char *out, size_t out_len) {
    snprintf(out, out_len,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
             uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
}

static void copy_str(char *dst, size_t n, const char *src) {
    snprintf(dst, n, "%s", src);
}

void agent_config_defaults(struct agent_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    copy_str(cfg->capture_iface, sizeof(cfg->capture_iface),
#ifdef _WIN32
             "\\Device\\NPF_{ADAPTER-GUID}"
#else
             "eth0"
#endif
    );
    cfg->interface_id = 1;
    copy_str(cfg->receiver_host, sizeof(cfg->receiver_host), "127.0.0.1");
    cfg->receiver_port = 9443;
    cfg->tls_verify_peer = true;
    copy_str(cfg->uuid_file, sizeof(cfg->uuid_file),
#ifdef _WIN32
             "mirror-agent.uuid"
#else
             "/var/lib/mirror-agent/agent.uuid"
#endif
    );
    cfg->ring_blocks = 64;
    cfg->block_size = 1048576;
    cfg->frame_size = 131072;
    cfg->block_timeout_ms = 64;
    cfg->max_capture_frame_size = 65575;
    cfg->queue_max_packets = 65536;
    cfg->queue_max_bytes = 268435456ULL;
    cfg->batch_max_records = 256;
    cfg->batch_max_bytes = 262144;
    cfg->batch_linger_us = 200;
    cfg->transport_write_timeout_sec = 10;
    cfg->reconnect_initial_sec = 1;
    cfg->reconnect_max_sec = 30;
    cfg->heartbeat_interval_sec = 10;
    cfg->stats_interval_sec = 10;
    cfg->log_interval_sec = 10;
    copy_str(cfg->queue_drop_policy, sizeof(cfg->queue_drop_policy), "drop_newest");
    copy_str(cfg->offload_policy, sizeof(cfg->offload_policy), "observe");
    copy_str(cfg->segmentation_mode, sizeof(cfg->segmentation_mode), "receiver");
}

void receiver_config_defaults(struct receiver_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    copy_str(cfg->listen_address, sizeof(cfg->listen_address), "0.0.0.0");
    cfg->listen_port = 9443;
    cfg->require_client_certificate = true;
    cfg->max_clients = 1000;
    cfg->max_record_size = 1048576;
    cfg->max_input_frame_size = 65575;
    cfg->max_frame_size = 9216;
    cfg->registration_timeout_sec = 10;
    cfg->heartbeat_timeout_sec = 30;
    copy_str(cfg->output_interface, sizeof(cfg->output_interface), "mirror-rx");
    copy_str(cfg->output_mode, sizeof(cfg->output_mode), "shared_veth");
    copy_str(cfg->output_driver, sizeof(cfg->output_driver), "af_packet");
    cfg->required_mtu = 9216;
    cfg->output_queue_max_packets = 262144;
    cfg->output_queue_max_bytes = 536870912ULL;
    cfg->reorder_window_ms = 20;
    cfg->reorder_max_flows = 131072;
    cfg->reorder_flow_timeout_sec = 60;
    cfg->scheduler_input_quantum_packets = 4096;
    cfg->scheduler_input_quantum_bytes = 16777216ULL;
    cfg->scheduler_flow_quantum_packets = 64;
    cfg->output_batch_max_packets = 64;
    cfg->log_interval_sec = 10;
    cfg->dedup_enabled = true;
    cfg->dedup_window_ms = 500;
    cfg->dedup_shards = 64;
    cfg->dedup_max_entries = 1000000;
    cfg->dedup_rejected_max_entries = 65536;
    cfg->dedup_timestamp_tolerance_ms = 500;
    cfg->transport_max_entries = 4096;
    copy_str(cfg->dedup_mode, sizeof(cfg->dedup_mode), "cross_agent");
}

static bool section_allowed(const char *section, const char *allowed) {
    const char *p = allowed;
    while (*p) { const char *end = strchr(p, ','); size_t n = end ? (size_t)(end - p) : strlen(p);
        if (strlen(section) == n && !strncmp(section, p, n)) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}

static int each_config_line(const char *path, const char *allowed_sections,
                            int (*cb)(void *, const char *, const char *, const char *),
                            void *arg, char *err, size_t err_len) {
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, err_len, "open %s: %s", path, strerror(errno));
        return -1;
    }
    char line[1024];
    char section[64] = {0};
    int rc = 0;
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == 0 || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char *end = strchr(s + 1, ']');
            if (!end) { rc = -1; break; }
            *end = 0;
            char *name = trim(s + 1);
            if (!section_allowed(name, allowed_sections)) { rc = -1; break; }
            copy_str(section, sizeof(section), name);
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(s);
        char *v = trim(eq + 1);
        char *comment = strpbrk(v, "#;");
        if (comment) *comment = 0;
        v = trim(v);
        if (!section[0] || cb(arg, section, k, v) != 0) { rc = -1; break; }
    }
    fclose(f);
    if (rc != 0) snprintf(err, err_len, "invalid config line in %s", path);
    return rc;
}

static int agent_kv(void *arg, const char *section, const char *k, const char *v) {
    struct agent_config *c = arg;
    if (strcmp(section, "agent")) return -1;
#define AU32(field) do { if (parse_u32(v, &c->field)) return -1; } while (0)
#define AU64(field) do { if (parse_u64(v, &c->field)) return -1; } while (0)
    if (!strcmp(k, "agent_uuid")) { if (strcmp(v, "auto") && parse_uuid(v, c->agent_uuid)) return -1; }
    else if (!strcmp(k, "capture_iface")) copy_str(c->capture_iface, sizeof(c->capture_iface), v);
    else if (!strcmp(k, "interface_id")) AU32(interface_id);
    else if (!strcmp(k, "receiver_host")) copy_str(c->receiver_host, sizeof(c->receiver_host), v);
    else if (!strcmp(k, "receiver_port")) { uint32_t n; if (parse_u32(v, &n) || n > UINT16_MAX) return -1; c->receiver_port = (uint16_t)n; }
    else if (!strcmp(k, "ca_file")) copy_str(c->ca_file, sizeof(c->ca_file), v);
    else if (!strcmp(k, "client_cert_file")) copy_str(c->client_cert_file, sizeof(c->client_cert_file), v);
    else if (!strcmp(k, "client_key_file")) copy_str(c->client_key_file, sizeof(c->client_key_file), v);
    else if (!strcmp(k, "tls_server_name")) copy_str(c->tls_server_name, sizeof(c->tls_server_name), v);
    else if (!strcmp(k, "uuid_file")) copy_str(c->uuid_file, sizeof(c->uuid_file), v);
    else if (!strcmp(k, "tls_verify_peer")) { if (parse_bool(v, &c->tls_verify_peer)) return -1; }
    else if (!strcmp(k, "insecure_tls")) { if (parse_bool(v, &c->insecure_tls)) return -1; }
    else if (!strcmp(k, "ring_blocks")) AU32(ring_blocks);
    else if (!strcmp(k, "block_size")) AU32(block_size);
    else if (!strcmp(k, "frame_size")) AU32(frame_size);
    else if (!strcmp(k, "block_timeout_ms")) AU32(block_timeout_ms);
    else if (!strcmp(k, "max_capture_frame_size")) AU32(max_capture_frame_size);
    else if (!strcmp(k, "queue_max_packets")) AU32(queue_max_packets);
    else if (!strcmp(k, "queue_max_bytes")) AU64(queue_max_bytes);
    else if (!strcmp(k, "batch_max_records")) AU32(batch_max_records);
    else if (!strcmp(k, "batch_max_bytes")) AU32(batch_max_bytes);
    else if (!strcmp(k, "batch_linger_us")) AU32(batch_linger_us);
    else if (!strcmp(k, "transport_write_timeout_sec")) AU32(transport_write_timeout_sec);
    else if (!strcmp(k, "queue_drop_policy")) copy_str(c->queue_drop_policy, sizeof(c->queue_drop_policy), v);
    else if (!strcmp(k, "offload_policy")) copy_str(c->offload_policy, sizeof(c->offload_policy), v);
    else if (!strcmp(k, "segmentation_mode")) copy_str(c->segmentation_mode, sizeof(c->segmentation_mode), v);
    else if (!strcmp(k, "reconnect_initial_sec")) AU32(reconnect_initial_sec);
    else if (!strcmp(k, "reconnect_max_sec")) AU32(reconnect_max_sec);
    else if (!strcmp(k, "heartbeat_interval_sec")) AU32(heartbeat_interval_sec);
    else if (!strcmp(k, "stats_interval_sec")) AU32(stats_interval_sec);
    else if (!strcmp(k, "log_interval_sec")) AU32(log_interval_sec);
    else return -1;
    return 0;
#undef AU32
#undef AU64
}

static int receiver_kv(void *arg, const char *section, const char *k, const char *v) {
    struct receiver_config *c = arg;
#define RU32(field) do { if (parse_u32(v, &c->field)) return -1; } while (0)
    if (!strcmp(section, "agent_clock_offsets")) {
        if (c->clock_offset_count >= MIRROR_MAX_CLOCK_OFFSETS) return -1;
        uint8_t uuid[MIRROR_UUID_LEN]; int64_t offset;
        if (parse_uuid(k, uuid) != 0 || parse_i64(v, &offset) != 0) return -1;
        for (size_t i = 0; i < c->clock_offset_count; i++)
            if (memcmp(c->clock_offsets[i].agent_uuid, uuid, MIRROR_UUID_LEN) == 0) return -1;
        memcpy(c->clock_offsets[c->clock_offset_count].agent_uuid, uuid, MIRROR_UUID_LEN);
        c->clock_offsets[c->clock_offset_count++].offset_ns = offset;
        return 0;
    }
    if (!strcmp(section, "dedup")) {
        if (!strcmp(k, "enabled")) { if (parse_bool(v, &c->dedup_enabled)) return -1; }
        else if (!strcmp(k, "window_ms")) RU32(dedup_window_ms);
        else if (!strcmp(k, "shards")) RU32(dedup_shards);
        else if (!strcmp(k, "max_entries")) RU32(dedup_max_entries);
        else if (!strcmp(k, "rejected_max_entries")) RU32(dedup_rejected_max_entries);
        else if (!strcmp(k, "timestamp_tolerance_ms")) RU32(dedup_timestamp_tolerance_ms);
        else if (!strcmp(k, "transport_max_entries")) RU32(transport_max_entries);
        else if (!strcmp(k, "mode")) copy_str(c->dedup_mode, sizeof(c->dedup_mode), v);
        else return -1;
        return 0;
    }
    if (!strcmp(section, "receiver") && !strcmp(k, "listen_address")) copy_str(c->listen_address, sizeof(c->listen_address), v);
    else if (!strcmp(section, "receiver") && !strcmp(k, "listen_port")) { uint32_t n; if (parse_u32(v, &n) || n > UINT16_MAX) return -1; c->listen_port = (uint16_t)n; }
    else if (!strcmp(section, "receiver") && !strcmp(k, "server_cert_file")) copy_str(c->server_cert_file, sizeof(c->server_cert_file), v);
    else if (!strcmp(section, "receiver") && !strcmp(k, "server_key_file")) copy_str(c->server_key_file, sizeof(c->server_key_file), v);
    else if (!strcmp(section, "receiver") && !strcmp(k, "client_ca_file")) copy_str(c->client_ca_file, sizeof(c->client_ca_file), v);
    else if (!strcmp(section, "receiver") && !strcmp(k, "require_client_certificate")) { if (parse_bool(v, &c->require_client_certificate)) return -1; }
    else if (!strcmp(section, "receiver") && !strcmp(k, "insecure_tls")) { if (parse_bool(v, &c->insecure_tls)) return -1; }
    else if (!strcmp(section, "receiver") && !strcmp(k, "max_clients")) RU32(max_clients);
    else if (!strcmp(section, "receiver") && !strcmp(k, "max_record_size")) RU32(max_record_size);
    else if (!strcmp(section, "receiver") && !strcmp(k, "max_input_frame_size")) RU32(max_input_frame_size);
    else if (!strcmp(section, "receiver") && !strcmp(k, "max_frame_size")) RU32(max_frame_size);
    else if (!strcmp(section, "receiver") && !strcmp(k, "registration_timeout_sec")) RU32(registration_timeout_sec);
    else if (!strcmp(section, "receiver") && !strcmp(k, "heartbeat_timeout_sec")) RU32(heartbeat_timeout_sec);
    else if (!strcmp(section, "output") && !strcmp(k, "mode")) copy_str(c->output_driver, sizeof(c->output_driver), v);
    else if (!strcmp(section, "output") && !strcmp(k, "output_mode")) copy_str(c->output_mode, sizeof(c->output_mode), v);
    else if (!strcmp(section, "output") && !strcmp(k, "interface")) copy_str(c->output_interface, sizeof(c->output_interface), v);
    else if (!strcmp(section, "output") && !strcmp(k, "required_mtu")) RU32(required_mtu);
    else if (!strcmp(section, "output") && !strcmp(k, "queue_max_packets")) RU32(output_queue_max_packets);
    else if (!strcmp(section, "output") && !strcmp(k, "queue_max_bytes")) { if (parse_u64(v, &c->output_queue_max_bytes)) return -1; }
    else if (!strcmp(section, "output") && !strcmp(k, "reorder_window_ms")) RU32(reorder_window_ms);
    else if (!strcmp(section, "output") && !strcmp(k, "reorder_max_flows")) RU32(reorder_max_flows);
    else if (!strcmp(section, "output") && !strcmp(k, "reorder_flow_timeout_sec")) RU32(reorder_flow_timeout_sec);
    else if (!strcmp(section, "output") && !strcmp(k, "scheduler_input_quantum_packets")) RU32(scheduler_input_quantum_packets);
    else if (!strcmp(section, "output") && !strcmp(k, "scheduler_input_quantum_bytes")) { if (parse_u64(v, &c->scheduler_input_quantum_bytes)) return -1; }
    else if (!strcmp(section, "output") && !strcmp(k, "scheduler_flow_quantum_packets")) RU32(scheduler_flow_quantum_packets);
    else if (!strcmp(section, "output") && !strcmp(k, "batch_max_packets")) RU32(output_batch_max_packets);
    else if ((!strcmp(section, "receiver") || !strcmp(section, "output")) && !strcmp(k, "log_interval_sec")) RU32(log_interval_sec);
    else return -1;
    return 0;
#undef RU32
}

int load_agent_config(const char *path, struct agent_config *cfg, char *err, size_t err_len) {
    agent_config_defaults(cfg);
    if (each_config_line(path, "agent", agent_kv, cfg, err, err_len) != 0) return -1;
    uint64_t largest_record = 4ULL + MIRROR_COMMON_HEADER_LEN +
                              MIRROR_PACKET_META_LEN + cfg->max_capture_frame_size;
    if (!cfg->receiver_port || !cfg->queue_max_packets || !cfg->queue_max_bytes ||
        !cfg->batch_max_records || cfg->batch_max_records > cfg->queue_max_packets ||
        cfg->batch_max_bytes < largest_record || cfg->batch_linger_us > 1000000 ||
        !cfg->transport_write_timeout_sec || cfg->transport_write_timeout_sec > 300 ||
        !cfg->ring_blocks || !cfg->block_size || !cfg->frame_size ||
        cfg->max_capture_frame_size < 64 || cfg->max_capture_frame_size > 262144 ||
        (!cfg->insecure_tls && (cfg->tls_verify_peer && !cfg->tls_server_name[0])) ||
        (!cfg->insecure_tls && (!cfg->client_cert_file[0] || !cfg->client_key_file[0])) ||
        !cfg->heartbeat_interval_sec || !cfg->stats_interval_sec || !cfg->log_interval_sec ||
        cfg->reconnect_initial_sec > cfg->reconnect_max_sec || strcmp(cfg->queue_drop_policy, "drop_newest") ||
        strcmp(cfg->offload_policy, "observe") ||
        (strcmp(cfg->segmentation_mode, "receiver") &&
         strcmp(cfg->segmentation_mode, "agent"))) {
        snprintf(err, err_len, "invalid or unsupported agent configuration"); return -1;
    }
    return 0;
}

int load_receiver_config(const char *path, struct receiver_config *cfg, char *err, size_t err_len) {
    receiver_config_defaults(cfg);
    if (each_config_line(path, "receiver,output,dedup,agent_clock_offsets", receiver_kv, cfg, err, err_len) != 0) return -1;
    if (strcmp(cfg->dedup_mode, "cross_agent")) {
        snprintf(err, err_len, "unsupported dedup mode '%s' (tcp_stream is reserved but not implemented)", cfg->dedup_mode);
        return -1;
    }
    if (!cfg->listen_port || !cfg->max_clients || cfg->max_record_size < MIRROR_COMMON_HEADER_LEN ||
        cfg->max_input_frame_size < cfg->max_frame_size ||
        cfg->max_input_frame_size > 262144 ||
        cfg->max_record_size < MIRROR_COMMON_HEADER_LEN + MIRROR_PACKET_META_LEN + cfg->max_input_frame_size ||
        cfg->max_frame_size < 64 || cfg->max_frame_size > 65531 || !cfg->registration_timeout_sec || !cfg->heartbeat_timeout_sec ||
        !cfg->server_cert_file[0] || !cfg->server_key_file[0] ||
        (!cfg->insecure_tls && cfg->require_client_certificate && !cfg->client_ca_file[0]) ||
        !cfg->log_interval_sec || !cfg->dedup_window_ms || !cfg->dedup_shards || !cfg->dedup_max_entries ||
        !cfg->dedup_rejected_max_entries ||
        (cfg->dedup_shards > cfg->dedup_max_entries ||
         cfg->dedup_shards > cfg->dedup_rejected_max_entries) ||
        !cfg->dedup_timestamp_tolerance_ms ||
        !cfg->transport_max_entries || !cfg->output_queue_max_packets ||
        !cfg->output_queue_max_bytes || !cfg->reorder_window_ms ||
        !cfg->reorder_max_flows || !cfg->reorder_flow_timeout_sec ||
        !cfg->scheduler_input_quantum_packets || !cfg->scheduler_input_quantum_bytes ||
        !cfg->scheduler_flow_quantum_packets || !cfg->output_batch_max_packets ||
        cfg->output_batch_max_packets > 256 ||
        strcmp(cfg->output_driver, "af_packet") || strcmp(cfg->output_mode, "shared_veth")) {
        snprintf(err, err_len, "invalid or unsupported receiver configuration"); return -1;
    }
    return 0;
}
