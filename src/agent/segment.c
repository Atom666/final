#include <agent/segment.h>
#include <mirror/protocol.h>

#include <mirror/net.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct tcp_layout {
    size_t l2_len;
    size_t ip_header_len;
    size_t extension_len;
    size_t tcp_offset;
    size_t tcp_header_len;
    size_t payload_offset;
    size_t payload_len;
    bool ipv6;
};

static uint16_t read_be16(const uint8_t *p) {
    uint16_t value;
    memcpy(&value, p, sizeof(value));
    return ntohs(value);
}

static uint32_t read_be32(const uint8_t *p) {
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return ntohl(value);
}

static void write_be16(uint8_t *p, uint16_t value) {
    value = htons(value);
    memcpy(p, &value, sizeof(value));
}

static void write_be32(uint8_t *p, uint32_t value) {
    value = htonl(value);
    memcpy(p, &value, sizeof(value));
}

static uint32_t checksum_add(uint32_t sum, const uint8_t *data, size_t len) {
    while (len >= 2) {
        sum += ((uint32_t)data[0] << 8) | data[1];
        data += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)data[0] << 8;
    return sum;
}

static uint16_t checksum_finish(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

static void ipv4_checksums(uint8_t *frame, const struct tcp_layout *layout,
                           size_t tcp_len) {
    uint8_t *ip = frame + layout->l2_len;
    uint8_t *tcp = frame + layout->tcp_offset;
    write_be16(ip + 10, 0);
    write_be16(ip + 10, checksum_finish(checksum_add(0, ip, layout->ip_header_len)));

    write_be16(tcp + 16, 0);
    uint32_t sum = 0;
    sum = checksum_add(sum, ip + 12, 8);
    sum += IPPROTO_TCP;
    sum += (uint32_t)tcp_len;
    sum = checksum_add(sum, tcp, tcp_len);
    write_be16(tcp + 16, checksum_finish(sum));
}

static void ipv6_tcp_checksum(uint8_t *frame, const struct tcp_layout *layout,
                              size_t tcp_len) {
    uint8_t *ip = frame + layout->l2_len;
    uint8_t *tcp = frame + layout->tcp_offset;
    write_be16(tcp + 16, 0);
    uint32_t sum = 0;
    sum = checksum_add(sum, ip + 8, 32);
    sum += (uint32_t)((tcp_len >> 16) & 0xffffu);
    sum += (uint32_t)(tcp_len & 0xffffu);
    sum += IPPROTO_TCP;
    sum = checksum_add(sum, tcp, tcp_len);
    write_be16(tcp + 16, checksum_finish(sum));
}

static int parse_l2(const uint8_t *frame, size_t frame_len,
                    size_t *l2_len, uint16_t *ether_type) {
    if (frame_len < ETH_HLEN) return -1;
    size_t offset = ETH_HLEN;
    uint16_t type = read_be16(frame + 12);
    unsigned tags = 0;
    while (type == ETH_P_8021Q || type == ETH_P_8021AD || type == 0x9100) {
        if (++tags > MIRROR_MAX_VLAN_TAGS || frame_len < offset + 4) return -1;
        type = read_be16(frame + offset + 2);
        offset += 4;
    }
    *l2_len = offset;
    *ether_type = type;
    return 0;
}

static int parse_ipv4(const uint8_t *frame, size_t frame_len,
                      size_t l2_len, struct tcp_layout *layout) {
    if (frame_len < l2_len + 20) return -1;
    const uint8_t *ip = frame + l2_len;
    size_t ihl = (size_t)(ip[0] & 0x0fu) * 4;
    if ((ip[0] >> 4) != 4 || ihl < 20 || frame_len < l2_len + ihl) return -1;
    uint16_t ip_len = read_be16(ip + 2);
    uint16_t fragment = read_be16(ip + 6);
    if (ip_len < ihl || frame_len < l2_len + ip_len || ip[9] != IPPROTO_TCP ||
        (fragment & 0x3fffu) != 0)
        return -1;
    size_t tcp_offset = l2_len + ihl;
    if (ip_len < ihl + 20) return -1;
    const uint8_t *tcp = frame + tcp_offset;
    size_t tcp_header_len = (size_t)(tcp[12] >> 4) * 4;
    if (tcp_header_len < 20 || ip_len < ihl + tcp_header_len) return -1;
    layout->l2_len = l2_len;
    layout->ip_header_len = ihl;
    layout->tcp_offset = tcp_offset;
    layout->tcp_header_len = tcp_header_len;
    layout->payload_offset = tcp_offset + tcp_header_len;
    layout->payload_len = ip_len - ihl - tcp_header_len;
    layout->ipv6 = false;
    return 0;
}

static int parse_ipv6(const uint8_t *frame, size_t frame_len,
                      size_t l2_len, struct tcp_layout *layout) {
    if (frame_len < l2_len + 40) return -1;
    const uint8_t *ip = frame + l2_len;
    if ((ip[0] >> 4) != 6) return -1;
    size_t payload_len = read_be16(ip + 4);
    if (!payload_len || frame_len < l2_len + 40 + payload_len) return -1;
    uint8_t next = ip[6];
    size_t cursor = 40;
    for (unsigned headers = 0; next != IPPROTO_TCP && headers < 8; headers++) {
        if (next == IPPROTO_FRAGMENT) return -1;
        if (cursor + 2 > 40 + payload_len) return -1;
        const uint8_t *ext = ip + cursor;
        size_t ext_len;
        if (next == IPPROTO_HOPOPTS || next == IPPROTO_ROUTING || next == IPPROTO_DSTOPTS)
            ext_len = ((size_t)ext[1] + 1) * 8;
        else if (next == IPPROTO_AH)
            ext_len = ((size_t)ext[1] + 2) * 4;
        else
            return -1;
        if (ext_len < 8 || cursor + ext_len > 40 + payload_len) return -1;
        next = ext[0];
        cursor += ext_len;
    }
    if (next != IPPROTO_TCP || cursor + 20 > 40 + payload_len) return -1;
    const uint8_t *tcp = ip + cursor;
    size_t tcp_header_len = (size_t)(tcp[12] >> 4) * 4;
    if (tcp_header_len < 20 || cursor + tcp_header_len > 40 + payload_len) return -1;
    layout->l2_len = l2_len;
    layout->ip_header_len = 40;
    layout->extension_len = cursor - 40;
    layout->tcp_offset = l2_len + cursor;
    layout->tcp_header_len = tcp_header_len;
    layout->payload_offset = layout->tcp_offset + tcp_header_len;
    layout->payload_len = 40 + payload_len - cursor - tcp_header_len;
    layout->ipv6 = true;
    return 0;
}

void segmented_batch_free(struct segmented_batch *batch) {
    if (!batch) return;
    for (size_t i = 0; i < batch->count; i++) free(batch->frames[i].data);
    free(batch->frames);
    memset(batch, 0, sizeof(*batch));
}

enum segment_result segment_tcp_frame(const uint8_t *frame, size_t frame_len,
                                      uint32_t interface_mtu,
                                      struct segmented_batch *out) {
    if (!frame || !out || interface_mtu < 68) return SEGMENT_UNSUPPORTED;
    memset(out, 0, sizeof(*out));
    size_t l2_len;
    uint16_t ether_type;
    if (parse_l2(frame, frame_len, &l2_len, &ether_type) != 0) return SEGMENT_UNSUPPORTED;

    struct tcp_layout layout = {0};
    int parsed = ether_type == ETH_P_IP ? parse_ipv4(frame, frame_len, l2_len, &layout) :
                 ether_type == ETH_P_IPV6 ? parse_ipv6(frame, frame_len, l2_len, &layout) : -1;
    if (parsed != 0) return SEGMENT_UNSUPPORTED;

    size_t l3_headers = layout.ip_header_len + layout.extension_len + layout.tcp_header_len;
    if (l3_headers + layout.payload_len <= interface_mtu) return SEGMENT_NOT_NEEDED;
    if (l3_headers >= interface_mtu || layout.payload_len == 0) return SEGMENT_UNSUPPORTED;
    const uint8_t *tcp = frame + layout.tcp_offset;
    if (tcp[13] & 0x26u) return SEGMENT_UNSUPPORTED; /* SYN, RST, or URG */

    size_t max_payload = interface_mtu - l3_headers;
    size_t count = (layout.payload_len + max_payload - 1) / max_payload;
    if (count < 2 || count > UINT16_MAX) return SEGMENT_UNSUPPORTED;
    out->frames = calloc(count, sizeof(*out->frames));
    if (!out->frames) return SEGMENT_NO_MEMORY;

    uint32_t initial_seq = read_be32(tcp + 4);
    uint16_t initial_id = layout.ipv6 ? 0 : read_be16(frame + l2_len + 4);
    size_t payload_pos = 0;
    for (size_t i = 0; i < count; i++) {
        size_t chunk = layout.payload_len - payload_pos;
        if (chunk > max_payload) chunk = max_payload;
        size_t segment_len = layout.payload_offset + chunk;
        uint8_t *segment = malloc(segment_len);
        if (!segment) {
            out->count = i;
            segmented_batch_free(out);
            return SEGMENT_NO_MEMORY;
        }
        memcpy(segment, frame, layout.payload_offset);
        memcpy(segment + layout.payload_offset, frame + layout.payload_offset + payload_pos, chunk);
        uint8_t *segment_tcp = segment + layout.tcp_offset;
        write_be32(segment_tcp + 4, initial_seq + (uint32_t)payload_pos);
        if (i + 1 != count) segment_tcp[13] &= (uint8_t)~0x09u; /* FIN and PSH */
        if (i != 0) segment_tcp[13] &= (uint8_t)~0x80u; /* CWR */

        size_t tcp_len = layout.tcp_header_len + chunk;
        if (layout.ipv6) {
            write_be16(segment + l2_len + 4,
                       (uint16_t)(layout.extension_len + tcp_len));
            ipv6_tcp_checksum(segment, &layout, tcp_len);
        } else {
            write_be16(segment + l2_len + 2,
                       (uint16_t)(layout.ip_header_len + tcp_len));
            write_be16(segment + l2_len + 4, (uint16_t)(initial_id + i));
            ipv4_checksums(segment, &layout, tcp_len);
        }
        out->frames[i].data = segment;
        out->frames[i].len = (uint32_t)segment_len;
        out->count = i + 1;
        payload_pos += chunk;
    }
    return SEGMENT_OK;
}
