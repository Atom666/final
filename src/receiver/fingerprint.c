#include <receiver/fingerprint.h>

#include <arpa/inet.h>
#include <string.h>
#include <xxhash.h>

static uint16_t get16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return ntohs(v); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return ntohl(v); }
static void put16(uint8_t **p, uint16_t v) { uint16_t n = htons(v); memcpy(*p, &n, 2); *p += 2; }
static void put32(uint8_t **p, uint32_t v) { uint32_t n = htonl(v); memcpy(*p, &n, 4); *p += 4; }

static void hash128(const void *data, size_t len, uint8_t out[16]) {
    XXH128_canonical_t canonical;
    XXH128_canonicalFromHash(&canonical, XXH3_128bits(data, len));
    memcpy(out, canonical.digest, sizeof(canonical.digest));
}

static void finish(struct packet_fingerprint *out, const uint8_t *key, size_t key_len,
                   const uint8_t *payload, size_t payload_len) {
    uint8_t material[192];
    if (key_len + 16 > sizeof(material)) return;
    hash128(payload, payload_len, out->payload_hash);
    memcpy(material, key, key_len);
    memcpy(material + key_len, out->payload_hash, 16);
    hash128(material, key_len + 16, out->bytes);
    out->payload_length = (uint32_t)payload_len;
}

static enum fingerprint_status fallback(uint16_t ether_type, const uint8_t *l3, size_t l3_len,
                                        struct packet_fingerprint *out) {
    uint8_t key[16], *p = key;
    *p++ = DEDUP_FINGERPRINT_VERSION;
    *p++ = 0xff;
    put16(&p, ether_type);
    put32(&p, (uint32_t)l3_len);
    out->format_version = DEDUP_FINGERPRINT_VERSION;
    out->fallback = 1;
    out->ether_type = ether_type;
    finish(out, key, (size_t)(p - key), l3, l3_len);
    return FINGERPRINT_FALLBACK;
}

static enum fingerprint_status fingerprint_transport(uint8_t version, uint8_t protocol,
        const uint8_t *src, size_t addr_len, const uint8_t *dst,
        const uint8_t *transport, size_t transport_len, uint16_t ether_type,
        uint32_t ip_len, struct packet_fingerprint *out) {
    uint8_t key[160], *p = key;
    const uint8_t *payload = transport;
    size_t payload_len = transport_len;
    *p++ = DEDUP_FINGERPRINT_VERSION;
    *p++ = version;
    *p++ = protocol;
    memcpy(p, src, addr_len); p += addr_len;
    memcpy(p, dst, addr_len); p += addr_len;

    if (protocol == IPPROTO_TCP) {
        if (transport_len < 20) return FINGERPRINT_FALLBACK;
        size_t header_len = (size_t)(transport[12] >> 4) * 4;
        if (header_len < 20 || header_len > transport_len) return FINGERPRINT_FALLBACK;
        put16(&p, get16(transport)); put16(&p, get16(transport + 2));
        put32(&p, get32(transport + 4)); put32(&p, get32(transport + 8));
        *p++ = transport[13] & 0x3f;
        payload = transport + header_len;
        payload_len = transport_len - header_len;
        put32(&p, (uint32_t)payload_len);
    } else if (protocol == IPPROTO_UDP) {
        if (transport_len < 8) return FINGERPRINT_FALLBACK;
        uint16_t udp_len = get16(transport + 4);
        if (udp_len < 8 || udp_len > transport_len) return FINGERPRINT_FALLBACK;
        put16(&p, get16(transport)); put16(&p, get16(transport + 2));
        payload = transport + 8;
        payload_len = udp_len - 8;
        put32(&p, (uint32_t)payload_len);
    } else if (protocol == IPPROTO_ICMP || protocol == IPPROTO_ICMPV6) {
        if (transport_len < 4) return FINGERPRINT_FALLBACK;
        *p++ = transport[0]; *p++ = transport[1];
        payload = transport + 4;
        payload_len = transport_len - 4;
        put32(&p, (uint32_t)payload_len);
    } else {
        put32(&p, (uint32_t)transport_len);
    }

    out->format_version = DEDUP_FINGERPRINT_VERSION;
    out->ether_type = ether_type;
    out->ip_version = version;
    out->ip_protocol = protocol;
    out->ip_packet_length = ip_len;
    finish(out, key, (size_t)(p - key), payload, payload_len);
    return FINGERPRINT_OK;
}

static enum fingerprint_status fingerprint_ipv4(const uint8_t *l3, size_t l3_len,
                                                  uint16_t ether_type,
                                                  struct packet_fingerprint *out) {
    if (l3_len < 20 || (l3[0] >> 4) != 4) return fallback(ether_type, l3, l3_len, out);
    size_t ihl = (size_t)(l3[0] & 0x0f) * 4;
    uint16_t total = get16(l3 + 2);
    if (ihl < 20 || total < ihl || total > l3_len) return fallback(ether_type, l3, l3_len, out);
    if (get16(l3 + 6) & 0x3fff) return FINGERPRINT_FAIL_OPEN;
    enum fingerprint_status status = fingerprint_transport(4, l3[9], l3 + 12, 4, l3 + 16,
                                                            l3 + ihl, total - ihl,
                                                            ether_type, total, out);
    return status == FINGERPRINT_FALLBACK ? fallback(ether_type, l3, total, out) : status;
}

static enum fingerprint_status fingerprint_ipv6(const uint8_t *l3, size_t l3_len,
                                                  uint16_t ether_type,
                                                  struct packet_fingerprint *out) {
    if (l3_len < 40 || (l3[0] >> 4) != 6) return fallback(ether_type, l3, l3_len, out);
    uint32_t total = 40u + get16(l3 + 4);
    if (total > l3_len) return fallback(ether_type, l3, l3_len, out);
    uint8_t next = l3[6];
    size_t offset = 40;
    for (unsigned count = 0; count < 16; count++) {
        if (next == 44) return FINGERPRINT_FAIL_OPEN;
        if (next != 0 && next != 43 && next != 60 && next != 51) break;
        if (offset + 2 > total) return fallback(ether_type, l3, total, out);
        size_t ext_len = next == 51 ? ((size_t)l3[offset + 1] + 2) * 4
                                    : ((size_t)l3[offset + 1] + 1) * 8;
        if (ext_len < 8 || offset + ext_len > total) return fallback(ether_type, l3, total, out);
        next = l3[offset];
        offset += ext_len;
    }
    if (next == 0 || next == 43 || next == 60 || next == 51 || next == 50 || next == 59)
        return fallback(ether_type, l3, total, out);
    enum fingerprint_status status = fingerprint_transport(6, next, l3 + 8, 16, l3 + 24,
                                                            l3 + offset, total - offset,
                                                            ether_type, total, out);
    return status == FINGERPRINT_FALLBACK ? fallback(ether_type, l3, total, out) : status;
}

enum fingerprint_status fingerprint_packet(const uint8_t *frame, size_t frame_len,
                                           struct packet_fingerprint *out) {
    memset(out, 0, sizeof(*out));
    if (!frame || frame_len < 14) return FINGERPRINT_FAIL_OPEN;
    size_t offset = 14;
    uint16_t ether_type = get16(frame + 12);
    for (unsigned tags = 0; tags < 4 &&
         (ether_type == 0x8100 || ether_type == 0x88a8 || ether_type == 0x9100); tags++) {
        if (offset + 4 > frame_len) return FINGERPRINT_FAIL_OPEN;
        ether_type = get16(frame + offset + 2);
        offset += 4;
    }
    const uint8_t *l3 = frame + offset;
    size_t l3_len = frame_len - offset;
    if (ether_type == 0x0800) return fingerprint_ipv4(l3, l3_len, ether_type, out);
    if (ether_type == 0x86dd) return fingerprint_ipv6(l3, l3_len, ether_type, out);
    return fallback(ether_type, l3, l3_len, out);
}
