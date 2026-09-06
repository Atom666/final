#include <mirror/protocol.h>

#include <mirror/net.h>
#include <stdio.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t v) { uint16_t n = htons(v); memcpy(p, &n, 2); }
static void put32(uint8_t *p, uint32_t v) { uint32_t n = htonl(v); memcpy(p, &n, 4); }
static void put64(uint8_t *p, uint64_t v) { uint64_t n = mirror_htonll(v); memcpy(p, &n, 8); }
static uint16_t get16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return ntohs(v); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return ntohl(v); }
static uint64_t get64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return mirror_ntohll(v); }

uint64_t mirror_htonll(uint64_t v) {
#if defined(_WIN32) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    return ((uint64_t)htonl((uint32_t)(v & 0xffffffffULL)) << 32) | htonl((uint32_t)(v >> 32));
#else
    return v;
#endif
}

uint64_t mirror_ntohll(uint64_t v) {
    return mirror_htonll(v);
}

void mirror_record_header_to_wire(const struct mirror_record_header *src, uint8_t out[MIRROR_COMMON_HEADER_LEN]) {
    put32(out, src->magic);
    put16(out + 4, src->version);
    put16(out + 6, src->record_type);
    put32(out + 8, src->header_length);
    put32(out + 12, src->payload_length);
}

int mirror_record_header_from_wire(const uint8_t in[MIRROR_COMMON_HEADER_LEN], struct mirror_record_header *out) {
    out->magic = get32(in);
    out->version = get16(in + 4);
    out->record_type = get16(in + 6);
    out->header_length = get32(in + 8);
    out->payload_length = get32(in + 12);
    return 0;
}

size_t mirror_register_payload_len(void) {
    return 16 + 4 + 4 + 6 + 2 + 4 + 4 + 8 + 64 + 64 + 32 + 64;
}

size_t mirror_packet_meta_len(void) {
    return MIRROR_PACKET_META_LEN;
}

void mirror_register_to_wire(const struct mirror_register_payload *src, uint8_t *out) {
    memcpy(out, src->agent_uuid, 16);
    put32(out + 16, src->interface_id);
    put32(out + 20, src->interface_index);
    memcpy(out + 24, src->interface_mac, 6);
    put16(out + 30, src->reserved);
    put32(out + 32, src->interface_mtu);
    put32(out + 36, src->max_capture_frame_size);
    put64(out + 40, src->connection_epoch);
    memcpy(out + 48, src->hostname, 64);
    memcpy(out + 112, src->interface_name, 64);
    memcpy(out + 176, src->agent_version, 32);
    memcpy(out + 208, src->kernel_release, 64);
}

int mirror_register_from_wire(const uint8_t *in, size_t len, struct mirror_register_payload *out) {
    if (len != mirror_register_payload_len()) return -1;
    memcpy(out->agent_uuid, in, 16);
    out->interface_id = get32(in + 16);
    out->interface_index = get32(in + 20);
    memcpy(out->interface_mac, in + 24, 6);
    out->reserved = get16(in + 30);
    out->interface_mtu = get32(in + 32);
    out->max_capture_frame_size = get32(in + 36);
    out->connection_epoch = get64(in + 40);
    memcpy(out->hostname, in + 48, 64); out->hostname[63] = 0;
    memcpy(out->interface_name, in + 112, 64); out->interface_name[63] = 0;
    memcpy(out->agent_version, in + 176, 32); out->agent_version[31] = 0;
    memcpy(out->kernel_release, in + 208, 64); out->kernel_release[63] = 0;
    return 0;
}

void mirror_packet_meta_to_wire(const struct mirror_packet_meta *src, uint8_t *out) {
    memcpy(out, src->agent_uuid, 16);
    put32(out + 16, src->interface_id);
    put32(out + 20, src->flags);
    put64(out + 24, src->connection_epoch);
    put64(out + 32, src->sequence_number);
    put64(out + 40, src->timestamp_ns);
    put32(out + 48, src->captured_length);
    put32(out + 52, src->original_length);
    put16(out + 56, src->vlan_tci);
    put16(out + 58, src->vlan_tpid);
    out[60] = src->direction;
    out[61] = src->packet_type;
    out[62] = src->vlan_tag_count;
    out[63] = src->reserved;
    for (size_t i = 0; i < MIRROR_MAX_VLAN_TAGS - 1; i++) {
        put16(out + 64 + i * 2, src->extra_vlan_tci[i]);
        put16(out + 70 + i * 2, src->extra_vlan_tpid[i]);
    }
}

int mirror_packet_meta_from_wire(const uint8_t *in, size_t len, struct mirror_packet_meta *out) {
    if (len < mirror_packet_meta_len()) return -1;
    memcpy(out->agent_uuid, in, 16);
    out->interface_id = get32(in + 16);
    out->flags = get32(in + 20);
    out->connection_epoch = get64(in + 24);
    out->sequence_number = get64(in + 32);
    out->timestamp_ns = get64(in + 40);
    out->captured_length = get32(in + 48);
    out->original_length = get32(in + 52);
    out->vlan_tci = get16(in + 56);
    out->vlan_tpid = get16(in + 58);
    out->direction = in[60];
    out->packet_type = in[61];
    out->vlan_tag_count = in[62]; out->reserved = in[63];
    for (size_t i = 0; i < MIRROR_MAX_VLAN_TAGS - 1; i++) {
        out->extra_vlan_tci[i] = get16(in + 64 + i * 2);
        out->extra_vlan_tpid[i] = get16(in + 70 + i * 2);
    }
    return 0;
}

int mirror_validate_header(const struct mirror_record_header *hdr, uint32_t max_record_size, char *err, size_t err_len) {
    if (hdr->magic != MIRROR_MAGIC) {
        snprintf(err, err_len, "invalid magic 0x%08x", hdr->magic);
        return -1;
    }
    if (hdr->version != MIRROR_VERSION) {
        snprintf(err, err_len, "unsupported version %u", hdr->version);
        return -1;
    }
    if (hdr->header_length < MIRROR_COMMON_HEADER_LEN) {
        snprintf(err, err_len, "header too short %u", hdr->header_length);
        return -1;
    }
    if (hdr->record_type < MIRROR_RECORD_REGISTER || hdr->record_type > MIRROR_RECORD_ERROR) {
        snprintf(err, err_len, "unknown record type %u", hdr->record_type);
        return -1;
    }
    uint64_t total = (uint64_t)hdr->header_length + hdr->payload_length;
    if (total > max_record_size) {
        snprintf(err, err_len, "record too large %llu", (unsigned long long)total);
        return -1;
    }
    return 0;
}

int mirror_validate_envelope(const struct mirror_record_header *hdr, uint32_t record_length,
                             uint32_t max_record_size, char *err, size_t err_len) {
    if (record_length < MIRROR_COMMON_HEADER_LEN || record_length > max_record_size) {
        snprintf(err, err_len, "invalid record length %u", record_length); return -1;
    }
    if (mirror_validate_header(hdr, max_record_size, err, err_len) != 0) return -1;
    if ((uint64_t)hdr->header_length + hdr->payload_length != record_length) {
        snprintf(err, err_len, "record length mismatch prefix=%u header=%u payload=%u",
                 record_length, hdr->header_length, hdr->payload_length); return -1;
    }
    return 0;
}
