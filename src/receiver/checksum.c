#include <receiver/checksum.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>

static uint16_t get16(const uint8_t *p) {
    uint16_t value;
    memcpy(&value, p, sizeof(value));
    return ntohs(value);
}

static void put16(uint8_t *p, uint16_t value) {
    uint16_t network = htons(value);
    memcpy(p, &network, sizeof(network));
}

static uint32_t sum_bytes(uint32_t sum, const uint8_t *data, size_t len) {
    while (len >= 2) {
        sum += ((uint32_t)data[0] << 8) | data[1];
        data += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)data[0] << 8;
    return sum;
}

static uint16_t finish_sum(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t ipv4_transport_checksum(const uint8_t *ip, uint8_t protocol,
                                        const uint8_t *transport, size_t len) {
    uint32_t sum = sum_bytes(0, ip + 12, 8);
    sum += protocol;
    sum += (uint16_t)len;
    return finish_sum(sum_bytes(sum, transport, len));
}

static uint16_t ipv6_transport_checksum(const uint8_t *ip, uint8_t protocol,
                                        const uint8_t *transport, size_t len) {
    uint32_t sum = sum_bytes(0, ip + 8, 32);
    sum += (uint16_t)(len >> 16);
    sum += (uint16_t)len;
    sum += protocol;
    return finish_sum(sum_bytes(sum, transport, len));
}

static enum checksum_repair_result repair_transport(uint8_t *ip, uint8_t version,
        uint8_t protocol, uint8_t *transport, size_t transport_len) {
    size_t checksum_offset;
    size_t checksum_len = transport_len;
    if (protocol == IPPROTO_TCP) {
        if (transport_len < 20) return CHECKSUM_REPAIR_MALFORMED;
        size_t header_len = (size_t)(transport[12] >> 4) * 4;
        if (header_len < 20 || header_len > transport_len)
            return CHECKSUM_REPAIR_MALFORMED;
        checksum_offset = 16;
    } else if (protocol == IPPROTO_UDP) {
        if (transport_len < 8) return CHECKSUM_REPAIR_MALFORMED;
        uint16_t udp_len = get16(transport + 4);
        if (udp_len < 8 || udp_len > transport_len)
            return CHECKSUM_REPAIR_MALFORMED;
        checksum_offset = 6;
        checksum_len = udp_len;
    } else if (protocol == IPPROTO_ICMP && version == 4) {
        if (transport_len < 4) return CHECKSUM_REPAIR_MALFORMED;
        put16(transport + 2, 0);
        put16(transport + 2, finish_sum(sum_bytes(0, transport, transport_len)));
        return CHECKSUM_REPAIR_OK;
    } else if (protocol == IPPROTO_ICMPV6 && version == 6) {
        if (transport_len < 4) return CHECKSUM_REPAIR_MALFORMED;
        checksum_offset = 2;
    } else {
        return CHECKSUM_REPAIR_UNSUPPORTED;
    }

    put16(transport + checksum_offset, 0);
    uint16_t checksum = version == 4
        ? ipv4_transport_checksum(ip, protocol, transport, checksum_len)
        : ipv6_transport_checksum(ip, protocol, transport, checksum_len);
    if (protocol == IPPROTO_UDP && checksum == 0) checksum = 0xffff;
    put16(transport + checksum_offset, checksum);
    return CHECKSUM_REPAIR_OK;
}

static enum checksum_repair_result repair_ipv4(uint8_t *ip, size_t available) {
    if (available < 20 || (ip[0] >> 4) != 4) return CHECKSUM_REPAIR_MALFORMED;
    size_t header_len = (size_t)(ip[0] & 0x0f) * 4;
    uint16_t total_len = get16(ip + 2);
    if (header_len < 20 || total_len < header_len || total_len > available)
        return CHECKSUM_REPAIR_MALFORMED;
    if (get16(ip + 6) & 0x3fffu) return CHECKSUM_REPAIR_UNSUPPORTED;

    put16(ip + 10, 0);
    put16(ip + 10, finish_sum(sum_bytes(0, ip, header_len)));
    return repair_transport(ip, 4, ip[9], ip + header_len,
                            total_len - header_len);
}

static enum checksum_repair_result repair_ipv6(uint8_t *ip, size_t available) {
    if (available < 40 || (ip[0] >> 4) != 6) return CHECKSUM_REPAIR_MALFORMED;
    uint16_t payload_len = get16(ip + 4);
    if (!payload_len || (size_t)payload_len + 40 > available)
        return CHECKSUM_REPAIR_MALFORMED;

    size_t total_len = (size_t)payload_len + 40;
    size_t offset = 40;
    uint8_t next = ip[6];
    for (unsigned count = 0; count < 16; count++) {
        if (next == IPPROTO_FRAGMENT) return CHECKSUM_REPAIR_UNSUPPORTED;
        if (next != IPPROTO_HOPOPTS && next != IPPROTO_ROUTING &&
            next != IPPROTO_DSTOPTS && next != IPPROTO_AH) break;
        if (offset + 2 > total_len) return CHECKSUM_REPAIR_MALFORMED;
        size_t ext_len = next == IPPROTO_AH
            ? ((size_t)ip[offset + 1] + 2) * 4
            : ((size_t)ip[offset + 1] + 1) * 8;
        if (ext_len < 8 || offset + ext_len > total_len)
            return CHECKSUM_REPAIR_MALFORMED;
        next = ip[offset];
        offset += ext_len;
    }
    if (next == IPPROTO_HOPOPTS || next == IPPROTO_ROUTING ||
        next == IPPROTO_DSTOPTS || next == IPPROTO_AH ||
        next == IPPROTO_ESP || next == IPPROTO_NONE)
        return CHECKSUM_REPAIR_UNSUPPORTED;
    return repair_transport(ip, 6, next, ip + offset, total_len - offset);
}

enum checksum_repair_result checksum_repair_offload_frame(uint8_t *frame,
                                                           size_t frame_len) {
    if (!frame || frame_len < 14) return CHECKSUM_REPAIR_MALFORMED;
    size_t offset = 14;
    uint16_t ether_type = get16(frame + 12);
    for (unsigned tags = 0; tags < 4 &&
         (ether_type == 0x8100 || ether_type == 0x88a8 ||
          ether_type == 0x9100); tags++) {
        if (offset + 4 > frame_len) return CHECKSUM_REPAIR_MALFORMED;
        ether_type = get16(frame + offset + 2);
        offset += 4;
    }
    if (ether_type == 0x0800) return repair_ipv4(frame + offset, frame_len - offset);
    if (ether_type == 0x86dd) return repair_ipv6(frame + offset, frame_len - offset);
    return CHECKSUM_REPAIR_UNSUPPORTED;
}
