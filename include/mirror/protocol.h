#ifndef MIRROR_PROTOCOL_H
#define MIRROR_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIRROR_MAGIC 0x4d495252u
#define MIRROR_VERSION 1u
#define MIRROR_MAX_FRAME_SIZE_DEFAULT 9216u
#define MIRROR_COMMON_HEADER_LEN 16u
#define MIRROR_PACKET_META_LEN 76u
#define MIRROR_MAX_VLAN_TAGS 4u

enum mirror_record_type {
    MIRROR_RECORD_REGISTER = 1,
    MIRROR_RECORD_REGISTER_ACK = 2,
    MIRROR_RECORD_PACKET = 3,
    MIRROR_RECORD_STATS = 4,
    MIRROR_RECORD_HEARTBEAT = 5,
    MIRROR_RECORD_ERROR = 6
};

enum mirror_packet_flags {
    MIRROR_FLAG_VLAN_METADATA_PRESENT = 1u << 0,
    MIRROR_FLAG_FRAME_TRUNCATED = 1u << 1,
    MIRROR_FLAG_RX_DIRECTION = 1u << 2,
    MIRROR_FLAG_TX_DIRECTION = 1u << 3,
    MIRROR_FLAG_CHECKSUM_NOT_READY = 1u << 4,
    MIRROR_FLAG_GSO_OR_GRO_SUSPECTED = 1u << 5,
    MIRROR_FLAG_SOFTWARE_SEGMENTED = 1u << 6
};

struct mirror_record_header {
    uint32_t magic;
    uint16_t version;
    uint16_t record_type;
    uint32_t header_length;
    uint32_t payload_length;
};

struct mirror_register_payload {
    uint8_t agent_uuid[16];
    uint32_t interface_id;
    uint32_t interface_index;
    uint8_t interface_mac[6];
    uint16_t reserved;
    uint32_t interface_mtu;
    uint32_t max_capture_frame_size;
    uint64_t connection_epoch;
    char hostname[64];
    char interface_name[64];
    char agent_version[32];
    char kernel_release[64];
};

struct mirror_packet_meta {
    uint8_t agent_uuid[16];
    uint32_t interface_id;
    uint32_t flags;
    uint64_t connection_epoch;
    uint64_t sequence_number;
    uint64_t timestamp_ns;
    uint32_t captured_length;
    uint32_t original_length;
    uint16_t vlan_tci;
    uint16_t vlan_tpid;
    uint8_t direction;
    uint8_t packet_type;
    uint8_t vlan_tag_count;
    uint8_t reserved;
    uint16_t extra_vlan_tci[MIRROR_MAX_VLAN_TAGS - 1];
    uint16_t extra_vlan_tpid[MIRROR_MAX_VLAN_TAGS - 1];
};

struct mirror_record_view {
    struct mirror_record_header header;
    const uint8_t *payload;
};

uint64_t mirror_htonll(uint64_t v);
uint64_t mirror_ntohll(uint64_t v);
void mirror_record_header_to_wire(const struct mirror_record_header *src, uint8_t out[MIRROR_COMMON_HEADER_LEN]);
int mirror_record_header_from_wire(const uint8_t in[MIRROR_COMMON_HEADER_LEN], struct mirror_record_header *out);
size_t mirror_register_payload_len(void);
size_t mirror_packet_meta_len(void);
void mirror_register_to_wire(const struct mirror_register_payload *src, uint8_t *out);
int mirror_register_from_wire(const uint8_t *in, size_t len, struct mirror_register_payload *out);
void mirror_packet_meta_to_wire(const struct mirror_packet_meta *src, uint8_t *out);
int mirror_packet_meta_from_wire(const uint8_t *in, size_t len, struct mirror_packet_meta *out);
int mirror_validate_header(const struct mirror_record_header *hdr, uint32_t max_record_size, char *err, size_t err_len);
int mirror_validate_envelope(const struct mirror_record_header *hdr, uint32_t record_length,
                             uint32_t max_record_size, char *err, size_t err_len);

#endif
