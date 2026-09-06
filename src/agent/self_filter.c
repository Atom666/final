#include <agent/self_filter.h>
#include <mirror/net.h>

#ifndef _WIN32
#include <linux/if_ether.h>
#include <linux/filter.h>
#include <sys/socket.h>
#endif
#include <stdio.h>
#include <string.h>

int self_filter_init(struct self_filter *f) {
    memset(f, 0, sizeof(*f));
    return agent_mutex_init(&f->mutex);
}

void self_filter_destroy(struct self_filter *f) { agent_mutex_destroy(&f->mutex); }

void self_filter_update_ipv4(struct self_filter *f, const char *local_ip, uint16_t local_port,
                             const char *remote_ip, uint16_t remote_port) {
    uint32_t local, remote;
    if (inet_pton(AF_INET, local_ip, &local) != 1 || inet_pton(AF_INET, remote_ip, &remote) != 1) return;
    agent_mutex_lock(&f->mutex);
    f->local_v4 = local; f->remote_v4 = remote;
    f->local_port = local_port;
    f->remote_port = remote_port;
    f->enabled = true;
    f->generation++;
    agent_mutex_unlock(&f->mutex);
}

static int parse_l3_offset(const uint8_t *frame, size_t len, size_t *off, uint16_t *ethertype) {
    if (len < 14) return -1;
    *off = 14;
    memcpy(ethertype, frame + 12, 2);
    *ethertype = ntohs(*ethertype);
    if (*ethertype == 0x8100 || *ethertype == 0x88a8) {
        if (len < 18) return -1;
        *off = 18;
        memcpy(ethertype, frame + 16, 2);
        *ethertype = ntohs(*ethertype);
    }
    return 0;
}

bool self_filter_match(const struct self_filter *f, const uint8_t *frame, size_t len) {
    struct self_filter snapshot;
    agent_mutex_lock((agent_mutex_t *)&f->mutex);
    snapshot.local_v4 = f->local_v4; snapshot.remote_v4 = f->remote_v4;
    snapshot.local_port = f->local_port; snapshot.remote_port = f->remote_port; snapshot.enabled = f->enabled;
    agent_mutex_unlock((agent_mutex_t *)&f->mutex);
    f = &snapshot;
    if (!f->enabled) return false;
    size_t off = 0;
    uint16_t et = 0;
    if (parse_l3_offset(frame, len, &off, &et) != 0 || et != ETH_P_IP) return false;
    if (len < off + 20) return false;
    if ((frame[off] >> 4) != 4 || frame[off + 9] != IPPROTO_TCP) return false;
    size_t ihl = (size_t)(frame[off] & 0x0f) * 4;
    if (ihl < 20 || len < off + ihl + 20) return false;
    uint32_t src, dst; uint16_t sp, dp;
    memcpy(&src, frame + off + 12, sizeof(src)); memcpy(&dst, frame + off + 16, sizeof(dst));
    memcpy(&sp, frame + off + ihl, sizeof(sp)); memcpy(&dp, frame + off + ihl + 2, sizeof(dp));
    sp = ntohs(sp); dp = ntohs(dp);
    bool outbound = src == f->local_v4 && dst == f->remote_v4 &&
                    sp == f->local_port && dp == f->remote_port;
    bool inbound = src == f->remote_v4 && dst == f->local_v4 &&
                   sp == f->remote_port && dp == f->local_port;
    return outbound || inbound;
}

int self_filter_attach_socket(struct self_filter *f, int fd, uint64_t *applied_generation) {
#ifdef _WIN32
    (void)f; (void)fd; (void)applied_generation;
    return 0;
#else
    uint32_t local, remote; uint16_t lp, rp; uint64_t gen; bool enabled;
    agent_mutex_lock(&f->mutex);
    local = ntohl(f->local_v4); remote = ntohl(f->remote_v4); lp = f->local_port; rp = f->remote_port;
    gen = f->generation; enabled = f->enabled;
    agent_mutex_unlock(&f->mutex);
    if (!enabled || gen == *applied_generation) return 0;
    struct sock_filter code[] = {
        {40,0,0,12},{21,0,23,2048},{48,0,0,23},{21,0,51,6},{32,0,0,26},{21,0,9,0x01020304},
        {40,0,0,20},{69,47,0,8191},{177,0,0,14},{72,0,0,14},{21,0,44,1234},{32,0,0,30},
        {21,0,42,0x05060708},{72,0,0,16},{21,39,40,9443},{21,0,39,0x05060708},{40,0,0,20},
        {69,37,0,8191},{177,0,0,14},{72,0,0,14},{21,0,34,9443},{32,0,0,30},{21,0,32,0x01020304},
        {72,0,0,16},{21,29,30,1234},{21,29,0,34525},{21,2,0,33024},{21,1,0,34984},{21,0,26,37120},
        {40,0,0,16},{21,0,24,2048},{48,0,0,27},{21,0,22,6},{32,0,0,30},{21,0,9,0x01020304},
        {40,0,0,24},{69,18,0,8191},{177,0,0,18},{72,0,0,18},{21,0,15,1234},{32,0,0,34},
        {21,0,13,0x05060708},{72,0,0,20},{21,10,11,9443},{21,0,10,0x05060708},{40,0,0,24},
        {69,8,0,8191},{177,0,0,18},{72,0,0,18},{21,0,5,9443},{32,0,0,34},{21,0,3,0x01020304},
        {72,0,0,20},{21,0,1,1234},{6,0,0,0},{6,0,0,262144}
    };
    for (size_t i = 0; i < sizeof(code)/sizeof(code[0]); i++) {
        if (code[i].k == 0x01020304) code[i].k = local;
        else if (code[i].k == 0x05060708) code[i].k = remote;
        else if (code[i].k == 1234) code[i].k = lp;
        else if (code[i].k == 9443) code[i].k = rp;
    }
    struct sock_fprog prog = { .len = (unsigned short)(sizeof(code)/sizeof(code[0])), .filter = code };
    if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog)) != 0) return -1;
    *applied_generation = gen;
    return 1;
#endif
}

int self_filter_expression(struct self_filter *f, char *out, size_t out_len,
                           uint64_t *generation) {
    uint32_t local, remote;
    uint16_t lp, rp;
    bool enabled;
    agent_mutex_lock(&f->mutex);
    local = f->local_v4;
    remote = f->remote_v4;
    lp = f->local_port;
    rp = f->remote_port;
    enabled = f->enabled;
    if (generation) *generation = f->generation;
    agent_mutex_unlock(&f->mutex);
    if (!enabled) {
        if (out_len) out[0] = 0;
        return 0;
    }
    char local_ip[INET_ADDRSTRLEN], remote_ip[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &local, local_ip, sizeof(local_ip)) ||
        !inet_ntop(AF_INET, &remote, remote_ip, sizeof(remote_ip))) return -1;
    int n = snprintf(out, out_len,
        "not (tcp and ((src host %s and src port %u and dst host %s and dst port %u) "
        "or (src host %s and src port %u and dst host %s and dst port %u)))",
        local_ip, lp, remote_ip, rp, remote_ip, rp, local_ip, lp);
    return n > 0 && (size_t)n < out_len ? 1 : -1;
}
