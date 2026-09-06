#ifndef MIRROR_CONFIG_H
#define MIRROR_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIRROR_UUID_LEN 16
#define MIRROR_MAX_PATH 256
#define MIRROR_MAX_HOST 256
#define MIRROR_MAX_IFNAME 64
#define MIRROR_MAX_CLOCK_OFFSETS 4096

struct agent_clock_offset {
    uint8_t agent_uuid[MIRROR_UUID_LEN];
    int64_t offset_ns;
};

struct agent_config {
    uint8_t agent_uuid[MIRROR_UUID_LEN];
    char capture_iface[MIRROR_MAX_IFNAME];
    uint32_t interface_id;
    char receiver_host[MIRROR_MAX_HOST];
    uint16_t receiver_port;
    char ca_file[MIRROR_MAX_PATH];
    char client_cert_file[MIRROR_MAX_PATH];
    char client_key_file[MIRROR_MAX_PATH];
    char tls_server_name[MIRROR_MAX_HOST];
    char uuid_file[MIRROR_MAX_PATH];
    bool tls_verify_peer;
    bool insecure_tls;
    uint32_t ring_blocks;
    uint32_t block_size;
    uint32_t frame_size;
    uint32_t block_timeout_ms;
    uint32_t max_capture_frame_size;
    uint32_t queue_max_packets;
    uint64_t queue_max_bytes;
    uint32_t batch_max_records;
    uint32_t batch_max_bytes;
    uint32_t batch_linger_us;
    uint32_t transport_write_timeout_sec;
    uint32_t reconnect_initial_sec;
    uint32_t reconnect_max_sec;
    uint32_t heartbeat_interval_sec;
    uint32_t stats_interval_sec;
    uint32_t log_interval_sec;
    char queue_drop_policy[32];
    char offload_policy[32];
    char segmentation_mode[32];
};

struct receiver_config {
    char listen_address[MIRROR_MAX_HOST];
    uint16_t listen_port;
    char server_cert_file[MIRROR_MAX_PATH];
    char server_key_file[MIRROR_MAX_PATH];
    char client_ca_file[MIRROR_MAX_PATH];
    bool require_client_certificate;
    bool insecure_tls;
    uint32_t max_clients;
    uint32_t max_record_size;
    uint32_t max_input_frame_size;
    uint32_t max_frame_size;
    uint32_t registration_timeout_sec;
    uint32_t heartbeat_timeout_sec;
    char output_interface[MIRROR_MAX_IFNAME];
    char output_mode[32];
    char output_driver[32];
    uint32_t required_mtu;
    uint32_t output_queue_max_packets;
    uint64_t output_queue_max_bytes;
    uint32_t reorder_window_ms;
    uint32_t reorder_max_flows;
    uint32_t reorder_flow_timeout_sec;
    uint32_t scheduler_input_quantum_packets;
    uint64_t scheduler_input_quantum_bytes;
    uint32_t scheduler_flow_quantum_packets;
    uint32_t output_batch_max_packets;
    uint32_t log_interval_sec;
    bool dedup_enabled;
    uint32_t dedup_window_ms;
    uint32_t dedup_shards;
    uint32_t dedup_max_entries;
    uint32_t dedup_rejected_max_entries;
    uint32_t dedup_timestamp_tolerance_ms;
    uint32_t transport_max_entries;
    char dedup_mode[32];
    struct agent_clock_offset clock_offsets[MIRROR_MAX_CLOCK_OFFSETS];
    size_t clock_offset_count;
};

void agent_config_defaults(struct agent_config *cfg);
void receiver_config_defaults(struct receiver_config *cfg);
int load_agent_config(const char *path, struct agent_config *cfg, char *err, size_t err_len);
int load_receiver_config(const char *path, struct receiver_config *cfg, char *err, size_t err_len);
int parse_uuid(const char *s, uint8_t out[MIRROR_UUID_LEN]);
void uuid_to_string(const uint8_t uuid[MIRROR_UUID_LEN], char *out, size_t out_len);

#endif
