#ifndef AGENT_SELF_FILTER_H
#define AGENT_SELF_FILTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <agent/sync.h>

struct self_filter {
    uint32_t local_v4;
    uint32_t remote_v4;
    uint16_t local_port;
    uint16_t remote_port;
    bool enabled;
    uint64_t generation;
    agent_mutex_t mutex;
};

int self_filter_init(struct self_filter *f);
void self_filter_destroy(struct self_filter *f);
void self_filter_update_ipv4(struct self_filter *f, const char *local_ip, uint16_t local_port,
                             const char *remote_ip, uint16_t remote_port);
bool self_filter_match(const struct self_filter *f, const uint8_t *frame, size_t len);
int self_filter_attach_socket(struct self_filter *f, int fd, uint64_t *applied_generation);
int self_filter_expression(struct self_filter *f, char *out, size_t out_len,
                           uint64_t *generation);

#endif
