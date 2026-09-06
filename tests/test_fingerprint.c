#include <assert.h>
#include <string.h>
#include <receiver/fingerprint.h>

static size_t ipv4_tcp(uint8_t *f, int vlan, const char *payload) {
    size_t p = 0; memset(f, 0, 128);
    for (int i = 0; i < 12; i++) f[p++] = (uint8_t)i;
    if (vlan) { f[p++] = 0x81; f[p++] = 0x00; f[p++] = 0; f[p++] = 7; }
    f[p++] = 0x08; f[p++] = 0x00;
    size_t ip = p, n = strlen(payload);
    f[p++] = 0x45; f[p++] = 0; f[p++] = 0; f[p++] = (uint8_t)(40 + n);
    f[p++] = 0x12; f[p++] = 0x34; f[p++] = 0; f[p++] = 0;
    f[p++] = 64; f[p++] = 6; f[p++] = 0xaa; f[p++] = 0xbb;
    f[p++] = 10; f[p++] = 0; f[p++] = 0; f[p++] = 1;
    f[p++] = 10; f[p++] = 0; f[p++] = 0; f[p++] = 2;
    f[p++] = 0x04; f[p++] = 0xd2; f[p++] = 0x01; f[p++] = 0xbb;
    f[p++] = 0; f[p++] = 0; f[p++] = 0; f[p++] = 9;
    f[p++] = 0; f[p++] = 0; f[p++] = 0; f[p++] = 7;
    f[p++] = 0x50; f[p++] = 0x18; f[p++] = 0x20; f[p++] = 0;
    f[p++] = 0xcc; f[p++] = 0xdd; f[p++] = 0; f[p++] = 0;
    memcpy(f + p, payload, n); p += n;
    assert(p - ip == 40 + n); return p;
}

static size_t ipv6_udp_with_hop(uint8_t *f, const char *payload) {
    size_t p = 0, n = strlen(payload); memset(f, 0, 160);
    p = 12; f[p++] = 0x86; f[p++] = 0xdd;
    f[p++] = 0x60; p += 3; f[p++] = 0; f[p++] = (uint8_t)(16 + n);
    f[p++] = 0; f[p++] = 64;
    for (int i = 0; i < 16; i++) f[p++] = (uint8_t)i;
    for (int i = 0; i < 16; i++) f[p++] = (uint8_t)(32 + i);
    f[p++] = 17; f[p++] = 0; p += 6;
    f[p++] = 0x04; f[p++] = 0xd2; f[p++] = 0x16; f[p++] = 0x2e;
    f[p++] = 0; f[p++] = (uint8_t)(8 + n); f[p++] = 0x11; f[p++] = 0x22;
    memcpy(f + p, payload, n); return p + n;
}

static size_t ipv4_datagram(uint8_t *f, uint8_t protocol, const char *payload) {
    size_t p = 0, n = strlen(payload); memset(f, 0, 128);
    p = 12; f[p++] = 0x08; f[p++] = 0x00;
    size_t transport_len = protocol == 17 ? 8 : 4;
    f[p++] = 0x45; f[p++] = 0; f[p++] = 0; f[p++] = (uint8_t)(20 + transport_len + n);
    p += 4; f[p++] = 64; f[p++] = protocol; p += 2;
    f[p++] = 192; f[p++] = 0; f[p++] = 2; f[p++] = 1;
    f[p++] = 192; f[p++] = 0; f[p++] = 2; f[p++] = 2;
    if (protocol == 17) {
        f[p++] = 0x12; f[p++] = 0x34; f[p++] = 0x56; f[p++] = 0x78;
        f[p++] = 0; f[p++] = (uint8_t)(8 + n); f[p++] = 0xaa; f[p++] = 0xbb;
    } else {
        f[p++] = 8; f[p++] = 0; f[p++] = 0xaa; f[p++] = 0xbb;
    }
    memcpy(f + p, payload, n); return p + n;
}

int main(void) {
    uint8_t a[160], b[160]; struct packet_fingerprint fa, fb;
    size_t alen = ipv4_tcp(a, 0, "hello"), blen = ipv4_tcp(b, 1, "hello");
    assert(fingerprint_packet(a, alen, &fa) == FINGERPRINT_OK);
    assert(fingerprint_packet(b, blen, &fb) == FINGERPRINT_OK);
    assert(memcmp(fa.bytes, fb.bytes, 16) == 0);

    memcpy(b, a, alen);
    memcpy(b + 14 + 12, a + 14 + 16, 4); memcpy(b + 14 + 16, a + 14 + 12, 4);
    assert(fingerprint_packet(b, alen, &fb) == FINGERPRINT_OK);
    assert(memcmp(fa.bytes, fb.bytes, 16) != 0);

    memcpy(b, a, alen); b[0] ^= 0xff; b[14 + 8] = 31; b[14 + 10] ^= 0xff;
    b[14 + 20 + 14] ^= 0xff; b[14 + 20 + 16] ^= 0xff;
    assert(fingerprint_packet(b, alen, &fb) == FINGERPRINT_OK);
    assert(memcmp(fa.bytes, fb.bytes, 16) == 0);

    b[14 + 20 + 4 + 3] ^= 1;
    assert(fingerprint_packet(b, alen, &fb) == FINGERPRINT_OK);
    assert(memcmp(fa.bytes, fb.bytes, 16) != 0);

    blen = ipv4_tcp(b, 0, "hello");
    memmove(b + 14 + 44, b + 14 + 40, 5);
    b[14 + 2] = 0; b[14 + 3] = 49; b[14 + 20 + 12] = 0x60;
    b[14 + 40] = 1; b[14 + 41] = 1; b[14 + 42] = 1; b[14 + 43] = 1;
    assert(fingerprint_packet(b, blen + 4, &fb) == FINGERPRINT_OK);
    assert(memcmp(fa.bytes, fb.bytes, 16) == 0);
    blen = ipv4_tcp(b, 0, "HELLO");
    assert(fingerprint_packet(b, blen, &fb) == FINGERPRINT_OK);
    assert(memcmp(fa.bytes, fb.bytes, 16) != 0);

    alen = ipv4_tcp(a, 0, "x"); a[14 + 6] = 0x20;
    assert(fingerprint_packet(a, alen, &fa) == FINGERPRINT_FAIL_OPEN);

    alen = ipv6_udp_with_hop(a, "udp");
    assert(fingerprint_packet(a, alen, &fa) == FINGERPRINT_OK);
    assert(fa.ip_version == 6 && fa.ip_protocol == 17 && fa.payload_length == 3);
    a[14 + 6] = 44;
    assert(fingerprint_packet(a, alen, &fa) == FINGERPRINT_FAIL_OPEN);

    alen = ipv4_datagram(a, 17, "udp"); blen = ipv4_datagram(b, 17, "udp");
    b[14 + 8] = 1; b[14 + 20 + 6] ^= 0xff;
    assert(fingerprint_packet(a, alen, &fa) == FINGERPRINT_OK);
    assert(fingerprint_packet(b, blen, &fb) == FINGERPRINT_OK && memcmp(fa.bytes, fb.bytes, 16) == 0);
    alen = ipv4_datagram(a, 1, "icmp"); blen = ipv4_datagram(b, 1, "icmp");
    b[14 + 20 + 2] ^= 0xff;
    assert(fingerprint_packet(a, alen, &fa) == FINGERPRINT_OK);
    assert(fingerprint_packet(b, blen, &fb) == FINGERPRINT_OK && memcmp(fa.bytes, fb.bytes, 16) == 0);
    return 0;
}
