#include <assert.h>
#include <arpa/inet.h>
#include <string.h>
#include <agent/self_filter.h>

static size_t make_tcp(unsigned char *p, int vlan, const char *src, const char *dst, unsigned sp, unsigned dp) {
    memset(p, 0, 128); size_t ip = vlan ? 18 : 14;
    p[12] = vlan ? 0x81 : 0x08; p[13] = vlan ? 0x00 : 0x00;
    if (vlan) { p[16] = 0x08; p[17] = 0x00; }
    p[ip] = 0x45; p[ip + 9] = 6;
    inet_pton(AF_INET, src, p + ip + 12); inet_pton(AF_INET, dst, p + ip + 16);
    p[ip + 20] = sp >> 8; p[ip + 21] = sp; p[ip + 22] = dp >> 8; p[ip + 23] = dp;
    return ip + 40;
}

int main(void) {
    struct self_filter f; assert(self_filter_init(&f) == 0);
    self_filter_update_ipv4(&f, "10.0.0.2", 50000, "10.0.0.10", 9443);
    char expression[512]; uint64_t generation = 0;
    assert(self_filter_expression(&f, expression, sizeof(expression), &generation) == 1);
    assert(generation == 1);
    assert(strstr(expression, "src host 10.0.0.2") != NULL);
    assert(strstr(expression, "src port 50000") != NULL);
    assert(strstr(expression, "dst port 9443") != NULL);
    unsigned char frame[128]; size_t n = make_tcp(frame, 0, "10.0.0.2", "10.0.0.10", 50000, 9443);
    assert(self_filter_match(&f, frame, n));
    n = make_tcp(frame, 1, "10.0.0.10", "10.0.0.2", 9443, 50000); assert(self_filter_match(&f, frame, n));
    n = make_tcp(frame, 0, "10.0.0.2", "10.0.0.10", 50000, 443); assert(!self_filter_match(&f, frame, n));
    self_filter_destroy(&f); return 0;
}
