#include <assert.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

#include <receiver/checksum.h>

static uint16_t get16(const uint8_t *p) {
    uint16_t value;
    memcpy(&value, p, sizeof(value));
    return ntohs(value);
}

static uint32_t add_bytes(uint32_t sum, const uint8_t *data, size_t len) {
    while (len >= 2) {
        sum += ((uint32_t)data[0] << 8) | data[1];
        data += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)data[0] << 8;
    return sum;
}

static uint16_t folded(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)sum;
}

static void test_pcap_syn_regression(void) {
    uint8_t frame[] = {
        0x00,0x0c,0x29,0xdc,0x01,0x97,0x00,0x0c,0x29,0xf0,0x33,0xe0,0x08,0x00,
        0x45,0x00,0x00,0x3c,0xd9,0x99,0x40,0x00,0x40,0x06,0xdd,0x88,0xc0,0xa8,
        0x01,0x24,0xc0,0xa8,0x01,0x25,0xcf,0x30,0x1f,0x40,0xf7,0x50,0x29,0x87,
        0x00,0x00,0x00,0x00,0xa0,0x02,0xfa,0xf0,0x83,0xc8,0x00,0x00,0x02,0x04,
        0x05,0xb4,0x04,0x02,0x08,0x0a,0xfd,0xc9,0x8a,0x39,0x00,0x00,0x00,0x00,
        0x01,0x03,0x03,0x09
    };
    assert(checksum_repair_offload_frame(frame, sizeof(frame)) == CHECKSUM_REPAIR_OK);
    assert(get16(frame + 14 + 20 + 16) == 0x3227);
    assert(folded(add_bytes(0, frame + 14, 20)) == 0xffff);
}

static void test_ipv6_udp(void) {
    uint8_t frame[14 + 40 + 8 + 3] = {0};
    frame[12] = 0x86; frame[13] = 0xdd;
    uint8_t *ip = frame + 14;
    ip[0] = 0x60; ip[4] = 0; ip[5] = 11; ip[6] = 17; ip[7] = 64;
    for (int i = 0; i < 16; i++) {
        ip[8 + i] = (uint8_t)i;
        ip[24 + i] = (uint8_t)(32 + i);
    }
    uint8_t *udp = ip + 40;
    udp[0] = 0x12; udp[1] = 0x34; udp[2] = 0x56; udp[3] = 0x78;
    udp[4] = 0; udp[5] = 11; udp[6] = 0x83; udp[7] = 0xc8;
    memcpy(udp + 8, "udp", 3);
    assert(checksum_repair_offload_frame(frame, sizeof(frame)) == CHECKSUM_REPAIR_OK);

    uint32_t sum = add_bytes(0, ip + 8, 32);
    sum += 11;
    sum += 17;
    sum = add_bytes(sum, udp, 11);
    assert(folded(sum) == 0xffff);
}

static void test_fragment_fail_open(void) {
    uint8_t frame[14 + 28] = {0}, original[sizeof(frame)];
    frame[12] = 0x08; frame[13] = 0x00;
    uint8_t *ip = frame + 14;
    ip[0] = 0x45; ip[2] = 0; ip[3] = 28; ip[6] = 0x20;
    ip[8] = 64; ip[9] = 17;
    memcpy(original, frame, sizeof(frame));
    assert(checksum_repair_offload_frame(frame, sizeof(frame)) ==
           CHECKSUM_REPAIR_UNSUPPORTED);
    assert(memcmp(frame, original, sizeof(frame)) == 0);
}

int main(void) {
    test_pcap_syn_regression();
    test_ipv6_udp();
    test_fragment_fail_open();

    uint8_t short_frame[10] = {0};
    assert(checksum_repair_offload_frame(short_frame, sizeof(short_frame)) ==
           CHECKSUM_REPAIR_MALFORMED);
    return 0;
}
