#include <agent/segment.h>

#include <arpa/inet.h>
#include <assert.h>
#include <linux/if_ether.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t get16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return ntohs(v);
}

static uint32_t get32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return ntohl(v);
}

static void put16(uint8_t *p, uint16_t v) {
    v = htons(v);
    memcpy(p, &v, sizeof(v));
}

static void put32(uint8_t *p, uint32_t v) {
    v = htonl(v);
    memcpy(p, &v, sizeof(v));
}

static uint32_t sum_bytes(uint32_t sum, const uint8_t *p, size_t len) {
    while (len >= 2) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    return sum;
}

static uint16_t fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

static void fill_eth(uint8_t *frame, uint16_t type) {
    for (size_t i = 0; i < 12; i++) frame[i] = (uint8_t)(i + 1);
    put16(frame + 12, type);
}

static uint8_t *make_ipv4(size_t payload_len, size_t *frame_len) {
    const size_t l2 = 14, ihl = 20, thl = 32;
    *frame_len = l2 + ihl + thl + payload_len;
    uint8_t *frame = calloc(1, *frame_len);
    assert(frame);
    fill_eth(frame, ETH_P_IP);
    uint8_t *ip = frame + l2;
    ip[0] = 0x45;
    put16(ip + 2, (uint16_t)(ihl + thl + payload_len));
    put16(ip + 4, 0x1200);
    put16(ip + 6, 0x4000);
    ip[8] = 64;
    ip[9] = 6;
    ip[12] = 192; ip[13] = 0; ip[14] = 2; ip[15] = 1;
    ip[16] = 198; ip[17] = 51; ip[18] = 100; ip[19] = 2;
    uint8_t *tcp = ip + ihl;
    put16(tcp, 12345);
    put16(tcp + 2, 8000);
    put32(tcp + 4, 0x10203040);
    put32(tcp + 8, 0x50607080);
    tcp[12] = 8u << 4;
    tcp[13] = 0x99; /* CWR, ACK, PSH, FIN */
    put16(tcp + 14, 32768);
    for (size_t i = 20; i < thl; i++) tcp[i] = (uint8_t)i;
    for (size_t i = 0; i < payload_len; i++) tcp[thl + i] = (uint8_t)(i * 31u + 7u);
    return frame;
}

static void verify_ipv4_checksum(const uint8_t *frame) {
    const uint8_t *ip = frame + 14;
    size_t ihl = (size_t)(ip[0] & 0x0f) * 4;
    size_t ip_len = get16(ip + 2);
    assert(fold(sum_bytes(0, ip, ihl)) == 0);
    const uint8_t *tcp = ip + ihl;
    size_t tcp_len = ip_len - ihl;
    uint32_t sum = sum_bytes(0, ip + 12, 8);
    sum += 6;
    sum += (uint32_t)tcp_len;
    sum = sum_bytes(sum, tcp, tcp_len);
    assert(fold(sum) == 0);
}

static void test_ipv4_segment(void) {
    size_t frame_len;
    const size_t payload_len = 32768;
    uint8_t *frame = make_ipv4(payload_len, &frame_len);
    struct segmented_batch batch;
    assert(segment_tcp_frame(frame, frame_len, 1500, &batch) == SEGMENT_OK);
    assert(batch.count == 23);
    size_t payload_pos = 0;
    for (size_t i = 0; i < batch.count; i++) {
        const uint8_t *segment = batch.frames[i].data;
        assert(batch.frames[i].len <= 1514);
        const uint8_t *ip = segment + 14;
        const uint8_t *tcp = ip + 20;
        size_t chunk = get16(ip + 2) - 20 - 32;
        assert(get32(tcp + 4) == 0x10203040u + payload_pos);
        assert(memcmp(tcp + 32, frame + 14 + 20 + 32 + payload_pos, chunk) == 0);
        assert((tcp[13] & 0x10) != 0);
        if (i == 0) assert((tcp[13] & 0x80) != 0);
        else assert((tcp[13] & 0x80) == 0);
        if (i + 1 == batch.count) assert((tcp[13] & 0x09) == 0x09);
        else assert((tcp[13] & 0x09) == 0);
        assert(get16(ip + 4) == (uint16_t)(0x1200 + i));
        verify_ipv4_checksum(segment);
        payload_pos += chunk;
    }
    assert(payload_pos == payload_len);
    segmented_batch_free(&batch);
    free(frame);
}

static uint8_t *make_ipv6(size_t payload_len, size_t *frame_len) {
    const size_t l2 = 18, ip_len = 40, ext_len = 8, tcp_len = 20;
    *frame_len = l2 + ip_len + ext_len + tcp_len + payload_len;
    uint8_t *frame = calloc(1, *frame_len);
    assert(frame);
    fill_eth(frame, ETH_P_8021Q);
    put16(frame + 14, 7);
    put16(frame + 16, ETH_P_IPV6);
    uint8_t *ip = frame + l2;
    ip[0] = 0x60;
    put16(ip + 4, (uint16_t)(ext_len + tcp_len + payload_len));
    ip[6] = 60;
    ip[7] = 64;
    for (size_t i = 0; i < 32; i++) ip[8 + i] = (uint8_t)(i + 1);
    uint8_t *ext = ip + ip_len;
    ext[0] = 6;
    ext[1] = 0;
    uint8_t *tcp = ext + ext_len;
    put16(tcp, 443);
    put16(tcp + 2, 50000);
    put32(tcp + 4, 9000);
    tcp[12] = 5u << 4;
    tcp[13] = 0x18;
    put16(tcp + 14, 30000);
    for (size_t i = 0; i < payload_len; i++) tcp[tcp_len + i] = (uint8_t)(i * 13u);
    return frame;
}

static void verify_ipv6_checksum(const uint8_t *frame) {
    const size_t l2 = 18, tcp_offset = 40 + 8;
    const uint8_t *ip = frame + l2;
    const uint8_t *tcp = ip + tcp_offset;
    size_t tcp_len = get16(ip + 4) - 8;
    uint32_t sum = sum_bytes(0, ip + 8, 32);
    sum += (uint32_t)tcp_len;
    sum += 6;
    sum = sum_bytes(sum, tcp, tcp_len);
    assert(fold(sum) == 0);
}

static void test_ipv6_segment(void) {
    size_t frame_len;
    const size_t payload_len = 5000;
    uint8_t *frame = make_ipv6(payload_len, &frame_len);
    struct segmented_batch batch;
    assert(segment_tcp_frame(frame, frame_len, 1500, &batch) == SEGMENT_OK);
    assert(batch.count == 4);
    size_t payload_pos = 0;
    for (size_t i = 0; i < batch.count; i++) {
        const uint8_t *ip = batch.frames[i].data + 18;
        const uint8_t *tcp = ip + 48;
        size_t chunk = get16(ip + 4) - 8 - 20;
        assert(batch.frames[i].len <= 1518);
        assert(get32(tcp + 4) == 9000 + payload_pos);
        assert(memcmp(tcp + 20, frame + 18 + 48 + 20 + payload_pos, chunk) == 0);
        if (i + 1 == batch.count) assert((tcp[13] & 0x08) != 0);
        else assert((tcp[13] & 0x08) == 0);
        verify_ipv6_checksum(batch.frames[i].data);
        payload_pos += chunk;
    }
    assert(payload_pos == payload_len);
    segmented_batch_free(&batch);
    free(frame);
}

static void test_not_needed_and_fragment(void) {
    size_t frame_len;
    uint8_t *frame = make_ipv4(100, &frame_len);
    struct segmented_batch batch;
    assert(segment_tcp_frame(frame, frame_len, 1500, &batch) == SEGMENT_NOT_NEEDED);
    put16(frame + 14 + 6, 0x2000);
    assert(segment_tcp_frame(frame, frame_len, 68, &batch) == SEGMENT_UNSUPPORTED);
    free(frame);
}

int main(void) {
    test_ipv4_segment();
    test_ipv6_segment();
    test_not_needed_and_fragment();
    puts("test_segment: PASS");
    return 0;
}
