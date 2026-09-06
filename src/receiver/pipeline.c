#include <receiver/pipeline.h>

#include <mirror/config.h>
#include <mirror/protocol.h>
#include <mirror/util.h>
#include <receiver/client.h>
#include <receiver/output.h>

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FLOW_BUCKETS 4096u
#define OUTPUT_BATCH_LIMIT 256u
#define HEAP_NONE ((size_t)-1)

enum queue_drop_reason {
    QUEUE_DROP_PACKET_LIMIT,
    QUEUE_DROP_BYTE_LIMIT,
    QUEUE_DROP_ALLOCATION,
    QUEUE_DROP_STOPPING
};

struct flow_key {
    uint8_t ip_version;
    uint8_t address_len;
    uint8_t low_address[16];
    uint8_t high_address[16];
    uint16_t low_port;
    uint16_t high_port;
};

struct pipeline_packet {
    uint8_t *frame;
    uint32_t frame_len;
    uint64_t order_ns;
    uint64_t arrival_ns;
    uint64_t arrival_id;
    struct agent_metric *agent;
    struct pipeline_packet *next_input;
    struct pipeline_packet *next_flow;
    uint32_t tcp_sequence;
    uint32_t tcp_span;
    uint8_t tcp_flags;
    uint8_t direction;
};

struct flow_state {
    struct flow_key key;
    uint64_t last_activity_ns;
    uint64_t deadline_ns;
    uint32_t expected[2];
    bool expected_valid[2];
    struct pipeline_packet *head[2];
    struct pipeline_packet *tail[2];
    uint64_t pending_packets;
    struct flow_state *next;
    struct flow_state *active_next;
    bool active;
    size_t heap_index;
};

struct pipeline_impl {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t worker;
    bool stopping;
    struct pipeline_packet *input_head;
    struct pipeline_packet *input_tail;
    uint64_t input_pending_packets;
    uint64_t input_pending_bytes;
    uint64_t peak_input_pending_packets;
    uint64_t peak_input_pending_bytes;
    uint64_t reserved_packets;
    uint64_t owned_packets;
    uint64_t owned_bytes;
    uint64_t peak_owned_packets;
    uint64_t peak_owned_bytes;
    uint64_t next_arrival_id;
    uint64_t capacity_started_ns;
    uint64_t capacity_time_ns;
    uint64_t capacity_time_max_ns;
    uint64_t reorder_pending_packets;
    uint64_t peak_reorder_pending_packets;
    uint64_t flow_count;
    struct flow_state *flows[FLOW_BUCKETS];
    struct flow_state *active_head;
    struct flow_state *active_tail;
    uint64_t active_count;
    struct flow_state **deadline_heap;
    size_t deadline_count;
    uint64_t last_expire_ns;
    struct pipeline_packet *emit_batch[OUTPUT_BATCH_LIMIT];
    size_t emit_count;
    const struct receiver_config *cfg;
    struct receiver_stats *stats;
    struct agent_registry *registry;
    output_pipeline_send_fn send_fn;
    void *send_arg;
    struct output_sink *batch_sink;
};

static uint64_t pipeline_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void atomic_add(uint64_t *value, uint64_t amount) {
    if (value) __atomic_add_fetch(value, amount, __ATOMIC_RELAXED);
}

static void atomic_peak(uint64_t *peak, uint64_t value) {
    uint64_t old = __atomic_load_n(peak, __ATOMIC_RELAXED);
    while (value > old && !__atomic_compare_exchange_n(peak, &old, value, false,
                                                        __ATOMIC_RELAXED,
                                                        __ATOMIC_RELAXED)) {}
}

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

static int endpoint_compare(const uint8_t *a, uint16_t a_port,
                            const uint8_t *b, uint16_t b_port, size_t address_len) {
    int cmp = memcmp(a, b, address_len);
    if (cmp) return cmp;
    return a_port < b_port ? -1 : a_port > b_port;
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

static int fill_flow_key(struct flow_key *key, struct pipeline_packet *packet,
                         const uint8_t *src, const uint8_t *dst, size_t address_len,
                         uint16_t src_port, uint16_t dst_port,
                         uint8_t ip_version, const uint8_t *tcp, size_t tcp_len) {
    if (tcp_len < 20) return -1;
    size_t tcp_header_len = (size_t)(tcp[12] >> 4) * 4;
    if (tcp_header_len < 20 || tcp_header_len > tcp_len) return -1;
    memset(key, 0, sizeof(*key));
    key->ip_version = ip_version;
    key->address_len = (uint8_t)address_len;
    int cmp = endpoint_compare(src, src_port, dst, dst_port, address_len);
    packet->direction = cmp <= 0 ? 0 : 1;
    const uint8_t *low = cmp <= 0 ? src : dst;
    const uint8_t *high = cmp <= 0 ? dst : src;
    key->low_port = cmp <= 0 ? src_port : dst_port;
    key->high_port = cmp <= 0 ? dst_port : src_port;
    memcpy(key->low_address, low, address_len);
    memcpy(key->high_address, high, address_len);
    packet->tcp_sequence = read_be32(tcp + 4);
    packet->tcp_flags = tcp[13];
    packet->tcp_span = (uint32_t)(tcp_len - tcp_header_len);
    if (packet->tcp_flags & 0x02u) packet->tcp_span++;
    if (packet->tcp_flags & 0x01u) packet->tcp_span++;
    return 0;
}

static int parse_tcp_packet(struct pipeline_packet *packet, struct flow_key *key) {
    size_t l2_len;
    uint16_t ether_type;
    if (parse_l2(packet->frame, packet->frame_len, &l2_len, &ether_type) != 0) return -1;
    const uint8_t *frame = packet->frame;
    if (ether_type == ETH_P_IP) {
        if (packet->frame_len < l2_len + 20) return -1;
        const uint8_t *ip = frame + l2_len;
        size_t ihl = (size_t)(ip[0] & 0x0fu) * 4;
        uint16_t ip_len = read_be16(ip + 2);
        if ((ip[0] >> 4) != 4 || ihl < 20 || ip_len < ihl + 20 ||
            packet->frame_len < l2_len + ip_len || ip[9] != IPPROTO_TCP ||
            (read_be16(ip + 6) & 0x3fffu) != 0)
            return -1;
        const uint8_t *tcp = ip + ihl;
        return fill_flow_key(key, packet, ip + 12, ip + 16, 4,
                             read_be16(tcp), read_be16(tcp + 2), 4,
                             tcp, ip_len - ihl);
    }
    if (ether_type != ETH_P_IPV6 || packet->frame_len < l2_len + 40) return -1;
    const uint8_t *ip = frame + l2_len;
    size_t payload_len = read_be16(ip + 4);
    if ((ip[0] >> 4) != 6 || !payload_len || packet->frame_len < l2_len + 40 + payload_len)
        return -1;
    uint8_t next = ip[6];
    size_t cursor = 40;
    for (unsigned headers = 0; next != IPPROTO_TCP && headers < 8; headers++) {
        if (next == IPPROTO_FRAGMENT || cursor + 2 > 40 + payload_len) return -1;
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
    return fill_flow_key(key, packet, ip + 8, ip + 24, 16,
                         read_be16(tcp), read_be16(tcp + 2), 6,
                         tcp, 40 + payload_len - cursor);
}

static uint64_t flow_hash(const struct flow_key *key) {
    const uint8_t *p = (const uint8_t *)key;
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < sizeof(*key); i++) {
        hash ^= p[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static bool sequence_before(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

static bool packet_before(const struct pipeline_packet *a,
                          const struct pipeline_packet *b) {
    if (!b) return true;
    if (a->order_ns != b->order_ns) return a->order_ns < b->order_ns;
    return a->arrival_id < b->arrival_id;
}

static bool flow_packet_before(const struct pipeline_packet *a,
                               const struct pipeline_packet *b) {
    return packet_before(a, b);
}

static struct flow_state *flow_find_or_create(struct pipeline_impl *impl,
                                              const struct flow_key *key,
                                              uint64_t now) {
    size_t bucket = (size_t)(flow_hash(key) % FLOW_BUCKETS);
    for (struct flow_state *flow = impl->flows[bucket]; flow; flow = flow->next)
        if (memcmp(&flow->key, key, sizeof(*key)) == 0) return flow;
    if (__atomic_load_n(&impl->flow_count, __ATOMIC_RELAXED) >= impl->cfg->reorder_max_flows)
        return NULL;
    struct flow_state *flow = calloc(1, sizeof(*flow));
    if (!flow) return NULL;
    flow->key = *key;
    flow->last_activity_ns = now;
    flow->heap_index = HEAP_NONE;
    flow->next = impl->flows[bucket];
    impl->flows[bucket] = flow;
    atomic_add(&impl->flow_count, 1);
    return flow;
}

static bool deadline_less(const struct flow_state *a, const struct flow_state *b) {
    if (a->deadline_ns != b->deadline_ns) return a->deadline_ns < b->deadline_ns;
    return flow_hash(&a->key) < flow_hash(&b->key);
}

static void deadline_swap(struct pipeline_impl *impl, size_t a, size_t b) {
    struct flow_state *tmp = impl->deadline_heap[a];
    impl->deadline_heap[a] = impl->deadline_heap[b];
    impl->deadline_heap[b] = tmp;
    impl->deadline_heap[a]->heap_index = a;
    impl->deadline_heap[b]->heap_index = b;
}

static void deadline_up(struct pipeline_impl *impl, size_t index) {
    while (index) {
        size_t parent = (index - 1) / 2;
        if (!deadline_less(impl->deadline_heap[index], impl->deadline_heap[parent])) break;
        deadline_swap(impl, index, parent);
        index = parent;
    }
}

static void deadline_down(struct pipeline_impl *impl, size_t index) {
    for (;;) {
        size_t count = __atomic_load_n(&impl->deadline_count, __ATOMIC_RELAXED);
        size_t left = index * 2 + 1;
        size_t right = left + 1;
        size_t smallest = index;
        if (left < count &&
            deadline_less(impl->deadline_heap[left], impl->deadline_heap[smallest]))
            smallest = left;
        if (right < count &&
            deadline_less(impl->deadline_heap[right], impl->deadline_heap[smallest]))
            smallest = right;
        if (smallest == index) break;
        deadline_swap(impl, index, smallest);
        index = smallest;
    }
}

static void deadline_remove(struct pipeline_impl *impl, struct flow_state *flow) {
    if (flow->heap_index == HEAP_NONE) return;
    size_t index = flow->heap_index;
    size_t last = __atomic_sub_fetch(&impl->deadline_count, 1, __ATOMIC_RELAXED);
    flow->heap_index = HEAP_NONE;
    if (index == last) return;
    impl->deadline_heap[index] = impl->deadline_heap[last];
    impl->deadline_heap[index]->heap_index = index;
    deadline_up(impl, index);
    deadline_down(impl, impl->deadline_heap[index]->heap_index);
}

static void deadline_schedule(struct pipeline_impl *impl, struct flow_state *flow,
                              uint64_t deadline_ns) {
    flow->deadline_ns = deadline_ns;
    if (flow->heap_index != HEAP_NONE) {
        size_t index = flow->heap_index;
        deadline_up(impl, index);
        deadline_down(impl, flow->heap_index);
        return;
    }
    if (__atomic_load_n(&impl->deadline_count, __ATOMIC_RELAXED) >=
        impl->cfg->reorder_max_flows) return;
    size_t index = __atomic_fetch_add(&impl->deadline_count, 1, __ATOMIC_RELAXED);
    impl->deadline_heap[index] = flow;
    flow->heap_index = index;
    deadline_up(impl, index);
}

static void active_enqueue(struct pipeline_impl *impl, struct flow_state *flow) {
    if (flow->active) return;
    deadline_remove(impl, flow);
    flow->active = true;
    flow->active_next = NULL;
    if (impl->active_tail) impl->active_tail->active_next = flow;
    else impl->active_head = flow;
    impl->active_tail = flow;
    atomic_add(&impl->active_count, 1);
}

static struct flow_state *active_pop(struct pipeline_impl *impl) {
    struct flow_state *flow = impl->active_head;
    if (!flow) return NULL;
    impl->active_head = flow->active_next;
    if (!impl->active_head) impl->active_tail = NULL;
    flow->active_next = NULL;
    flow->active = false;
    __atomic_sub_fetch(&impl->active_count, 1, __ATOMIC_RELAXED);
    return flow;
}

static void account_agent(struct pipeline_impl *impl, struct agent_metric *agent,
                          bool success, uint32_t bytes) {
    if (!impl->registry || !agent) return;
    pthread_mutex_lock(&impl->registry->mutex);
    if (success) {
        agent->output_packets++;
        agent->output_bytes += bytes;
    } else {
        agent->output_errors++;
    }
    pthread_mutex_unlock(&impl->registry->mutex);
}

static void account_queue_drop(struct pipeline_impl *impl, struct agent_metric *agent,
                               enum queue_drop_reason reason) {
    if (impl->stats) {
        atomic_add(&impl->stats->output_queue_drops, 1);
        if (reason == QUEUE_DROP_PACKET_LIMIT)
            atomic_add(&impl->stats->output_queue_drop_packet_limit, 1);
        else if (reason == QUEUE_DROP_BYTE_LIMIT)
            atomic_add(&impl->stats->output_queue_drop_byte_limit, 1);
        else if (reason == QUEUE_DROP_ALLOCATION)
            atomic_add(&impl->stats->output_queue_drop_allocation, 1);
        else
            atomic_add(&impl->stats->output_queue_drop_stopping, 1);
    }
    if (!impl->registry || !agent) return;
    pthread_mutex_lock(&impl->registry->mutex);
    agent->output_queue_drops++;
    pthread_mutex_unlock(&impl->registry->mutex);
}

static void capacity_start_locked(struct pipeline_impl *impl, uint64_t now) {
    if (!impl->capacity_started_ns) impl->capacity_started_ns = now;
}

static void capacity_end_locked(struct pipeline_impl *impl, uint64_t now) {
    if (!impl->capacity_started_ns) return;
    uint64_t duration = now >= impl->capacity_started_ns ? now - impl->capacity_started_ns : 0;
    impl->capacity_time_ns += duration;
    if (duration > impl->capacity_time_max_ns) impl->capacity_time_max_ns = duration;
    impl->capacity_started_ns = 0;
}

static void complete_packet(struct pipeline_impl *impl, struct pipeline_packet *packet,
                            int rc, uint64_t emitted_ns) {
    if (impl->stats) {
        if (rc == OUTPUT_OK) {
            atomic_add(&impl->stats->output_packets_sent, 1);
            atomic_add(&impl->stats->output_bytes_sent, packet->frame_len);
        } else {
            atomic_add(&impl->stats->output_send_errors, 1);
            if (rc == OUTPUT_SHORT_WRITE) atomic_add(&impl->stats->output_short_writes, 1);
            if (rc == OUTPUT_INTERFACE_DOWN) atomic_add(&impl->stats->output_interface_down, 1);
            if (rc == OUTPUT_OVERSIZED) atomic_add(&impl->stats->output_oversized_packets, 1);
        }
        uint64_t residence = emitted_ns >= packet->arrival_ns ? emitted_ns - packet->arrival_ns : 0;
        atomic_add(&impl->stats->output_pipeline_residence_ns, residence);
        atomic_peak(&impl->stats->output_pipeline_residence_max_ns, residence);
    }
    account_agent(impl, packet->agent, rc == OUTPUT_OK, packet->frame_len);
    if (rc != OUTPUT_OK && impl->stats)
        log_msg("receiver", "error", "AF_PACKET pipeline send failed frame_len=%u result=%d error=%s",
                packet->frame_len, rc, strerror(errno));
}

static void flush_emit_batch(struct pipeline_impl *impl) {
    if (!impl->emit_count) return;
    const uint8_t *frames[OUTPUT_BATCH_LIMIT];
    uint32_t lengths[OUTPUT_BATCH_LIMIT];
    int results[OUTPUT_BATCH_LIMIT];
    uint64_t syscalls = 0;
    for (size_t i = 0; i < impl->emit_count; i++) {
        frames[i] = impl->emit_batch[i]->frame;
        lengths[i] = impl->emit_batch[i]->frame_len;
        results[i] = OUTPUT_ERROR;
    }
    uint64_t started = pipeline_now_ns();
    if (impl->batch_sink) {
        if (output_send_frames(impl->batch_sink, frames, lengths, results,
                               impl->emit_count, &syscalls) != 0)
            for (size_t i = 0; i < impl->emit_count; i++) results[i] = OUTPUT_ERROR;
    } else {
        for (size_t i = 0; i < impl->emit_count; i++) {
            results[i] = impl->send_fn(impl->send_arg, frames[i], lengths[i]);
            syscalls++;
        }
    }
    uint64_t finished = pipeline_now_ns();
    if (impl->stats) {
        atomic_add(&impl->stats->output_pipeline_send_batches, 1);
        atomic_add(&impl->stats->output_pipeline_send_syscalls, syscalls);
        atomic_add(&impl->stats->output_pipeline_send_ns,
                   finished >= started ? finished - started : 0);
    }
    pthread_mutex_lock(&impl->mutex);
    for (size_t i = 0; i < impl->emit_count; i++) {
        impl->owned_packets--;
        impl->owned_bytes -= impl->emit_batch[i]->frame_len;
    }
    if (impl->owned_packets < impl->cfg->output_queue_max_packets &&
        impl->owned_bytes < impl->cfg->output_queue_max_bytes)
        capacity_end_locked(impl, finished);
    pthread_mutex_unlock(&impl->mutex);
    for (size_t i = 0; i < impl->emit_count; i++) {
        complete_packet(impl, impl->emit_batch[i], results[i], finished);
        free(impl->emit_batch[i]->frame);
        free(impl->emit_batch[i]);
    }
    impl->emit_count = 0;
}

static void queue_emit(struct pipeline_impl *impl, struct pipeline_packet *packet) {
    impl->emit_batch[impl->emit_count++] = packet;
    if (impl->emit_count >= impl->cfg->output_batch_max_packets)
        flush_emit_batch(impl);
}

static bool timeout_watermark_safe(struct pipeline_impl *impl, uint64_t deadline_ns) {
    pthread_mutex_lock(&impl->mutex);
    bool safe = impl->reserved_packets == 0 &&
                (!impl->input_head || impl->input_head->arrival_ns > deadline_ns);
    pthread_mutex_unlock(&impl->mutex);
    return safe;
}

static void flow_insert(struct pipeline_impl *impl, struct flow_state *flow,
                        struct pipeline_packet *packet) {
    uint8_t direction = packet->direction;
    if (flow->tail[direction] &&
        !flow_packet_before(packet, flow->tail[direction])) {
        flow->tail[direction]->next_flow = packet;
        flow->tail[direction] = packet;
    } else {
        struct pipeline_packet **link = &flow->head[direction];
        while (*link && !flow_packet_before(packet, *link))
            link = &(*link)->next_flow;
        packet->next_flow = *link;
        *link = packet;
        if (!packet->next_flow) flow->tail[direction] = packet;
    }
    flow->pending_packets++;
    uint64_t pending = __atomic_add_fetch(&impl->reorder_pending_packets, 1, __ATOMIC_RELAXED);
    atomic_peak(&impl->peak_reorder_pending_packets, pending);
}

static bool flow_packet_ready(const struct flow_state *flow, uint8_t direction,
                              const struct pipeline_packet *packet) {
    if (!packet) return false;
    bool syn = (packet->tcp_flags & 0x02u) != 0;
    bool ack = (packet->tcp_flags & 0x10u) != 0;
    if (!flow->expected_valid[direction])
        return syn && (!ack || flow->expected_valid[direction ^ 1u]);
    return syn || packet->tcp_sequence == flow->expected[direction] ||
           sequence_before(packet->tcp_sequence, flow->expected[direction]);
}

static bool ready_packet_before(const struct flow_state *flow,
                                const struct pipeline_packet *candidate,
                                const struct pipeline_packet *current) {
    if (!current) return true;
    if (!flow->expected_valid[0] || !flow->expected_valid[1]) {
        bool candidate_syn = (candidate->tcp_flags & 0x02u) != 0;
        bool current_syn = (current->tcp_flags & 0x02u) != 0;
        if (candidate_syn != current_syn) return candidate_syn;
    }
    return packet_before(candidate, current);
}

static struct pipeline_packet *flow_choose_ready(struct pipeline_impl *impl,
                                                 struct flow_state *flow,
                                                 uint8_t *direction,
                                                 struct pipeline_packet **previous) {
    struct pipeline_packet *best = NULL;
    struct pipeline_packet *best_previous = NULL;
    for (uint8_t d = 0; d < 2; d++) {
        struct pipeline_packet *prev = NULL;
        for (struct pipeline_packet *packet = flow->head[d]; packet;
             prev = packet, packet = packet->next_flow) {
            atomic_add(impl->stats ? &impl->stats->output_reorder_packets_examined : NULL, 1);
            if (flow_packet_ready(flow, d, packet)) {
                if (ready_packet_before(flow, packet, best)) {
                    best = packet;
                    best_previous = prev;
                    *direction = d;
                }
                break;
            }
        }
    }
    *previous = best_previous;
    return best;
}

static bool flow_has_ready(const struct flow_state *flow) {
    for (uint8_t d = 0; d < 2; d++)
        for (const struct pipeline_packet *packet = flow->head[d]; packet;
             packet = packet->next_flow)
            if (flow_packet_ready(flow, d, packet)) return true;
    return false;
}

static struct pipeline_packet *flow_choose_oldest(struct pipeline_impl *impl,
                                                  struct flow_state *flow,
                                                  uint8_t *direction) {
    struct pipeline_packet *best = NULL;
    for (uint8_t d = 0; d < 2; d++) {
        if (flow->head[d]) {
            atomic_add(impl->stats ? &impl->stats->output_reorder_packets_examined : NULL, 1);
            if (packet_before(flow->head[d], best)) {
                best = flow->head[d];
                *direction = d;
            }
        }
    }
    return best;
}

static uint64_t flow_next_deadline(struct pipeline_impl *impl, struct flow_state *flow) {
    uint64_t oldest = UINT64_MAX;
    for (unsigned d = 0; d < 2; d++)
        if (flow->head[d] && flow->head[d]->arrival_ns < oldest)
            oldest = flow->head[d]->arrival_ns;
    return oldest == UINT64_MAX ? UINT64_MAX :
           oldest + (uint64_t)impl->cfg->reorder_window_ms * 1000000ULL;
}

static bool flow_emit_one(struct pipeline_impl *impl, struct flow_state *flow,
                          uint64_t now, bool force, bool *watermark_blocked) {
    uint8_t direction = 0;
    struct pipeline_packet *previous = NULL;
    struct pipeline_packet *packet = flow_choose_ready(impl, flow, &direction, &previous);
    bool timeout = false;
    *watermark_blocked = false;
    if (!packet) {
        uint64_t deadline = flow_next_deadline(impl, flow);
        if (!force && (deadline == UINT64_MAX || now < deadline)) return false;
        if (!force && !timeout_watermark_safe(impl, deadline)) {
            *watermark_blocked = true;
            return false;
        }
        packet = flow_choose_oldest(impl, flow, &direction);
        previous = NULL;
        if (!packet) return false;
        timeout = !force;
        if (timeout && impl->stats) atomic_add(&impl->stats->output_reorder_timeouts, 1);
    }
    if (previous) previous->next_flow = packet->next_flow;
    else flow->head[direction] = packet->next_flow;
    if (flow->tail[direction] == packet) flow->tail[direction] = previous;
    packet->next_flow = NULL;
    flow->pending_packets--;
    __atomic_sub_fetch(&impl->reorder_pending_packets, 1, __ATOMIC_RELAXED);
    uint32_t end = packet->tcp_sequence + packet->tcp_span;
    bool syn = (packet->tcp_flags & 0x02u) != 0;
    if (!flow->expected_valid[direction] || timeout ||
        (syn && packet->tcp_sequence != flow->expected[direction])) {
        flow->expected[direction] = end;
        flow->expected_valid[direction] = true;
    } else if (packet->tcp_span && packet->tcp_sequence == flow->expected[direction]) {
        flow->expected[direction] = end;
    }
    flow->last_activity_ns = now;
    queue_emit(impl, packet);
    return true;
}

static void activate_due_deadlines(struct pipeline_impl *impl, uint64_t now, bool force) {
    while (__atomic_load_n(&impl->deadline_count, __ATOMIC_RELAXED) &&
           (force || impl->deadline_heap[0]->deadline_ns <= now)) {
        struct flow_state *flow = impl->deadline_heap[0];
        deadline_remove(impl, flow);
        active_enqueue(impl, flow);
    }
}

static void run_active_flows(struct pipeline_impl *impl, uint64_t now, bool force,
                             uint64_t output_budget) {
    uint64_t total_emitted = 0;
    while (__atomic_load_n(&impl->active_count, __ATOMIC_RELAXED) &&
           (force || total_emitted < output_budget)) {
        struct flow_state *flow = active_pop(impl);
        if (!flow) break;
        bool blocked = false;
        uint32_t emitted = 0;
        uint64_t started = pipeline_now_ns();
        while (emitted < impl->cfg->scheduler_flow_quantum_packets &&
               (force || total_emitted < output_budget) &&
               flow_emit_one(impl, flow, now, force, &blocked))
            emitted++, total_emitted++;
        if (impl->stats) {
            uint64_t finished = pipeline_now_ns();
            atomic_add(&impl->stats->output_pipeline_reorder_ns,
                       finished >= started ? finished - started : 0);
        }
        if (!flow->pending_packets) continue;
        if (flow_has_ready(flow)) {
            active_enqueue(impl, flow);
        } else {
            uint64_t deadline = flow_next_deadline(impl, flow);
            if (blocked && deadline <= now) deadline = now + 1000000ULL;
            deadline_schedule(impl, flow, deadline);
        }
    }
}

static void process_input(struct pipeline_impl *impl, struct pipeline_packet *packet) {
    uint64_t started = pipeline_now_ns();
    struct flow_key key;
    int parsed = parse_tcp_packet(packet, &key);
    if (impl->stats) {
        atomic_add(&impl->stats->output_pipeline_parsed, 1);
        uint64_t finished = pipeline_now_ns();
        atomic_add(&impl->stats->output_pipeline_parse_ns,
                   finished >= started ? finished - started : 0);
    }
    if (parsed != 0) {
        queue_emit(impl, packet);
        return;
    }
    struct flow_state *flow = flow_find_or_create(impl, &key, packet->arrival_ns);
    if (!flow) {
        if (impl->stats) atomic_add(&impl->stats->output_reorder_bypassed, 1);
        queue_emit(impl, packet);
        return;
    }
    flow_insert(impl, flow, packet);
    flow->last_activity_ns = packet->arrival_ns;
    active_enqueue(impl, flow);
}

static struct pipeline_packet *take_input_quantum(struct pipeline_impl *impl) {
    pthread_mutex_lock(&impl->mutex);
    if (impl->reserved_packets) {
        pthread_mutex_unlock(&impl->mutex);
        return NULL;
    }
    struct pipeline_packet *head = impl->input_head;
    if (!head) {
        pthread_mutex_unlock(&impl->mutex);
        return NULL;
    }
    struct pipeline_packet *tail = NULL;
    struct pipeline_packet *packet = head;
    uint64_t packets = 0, bytes = 0;
    while (packet && packets < impl->cfg->scheduler_input_quantum_packets &&
           (packets == 0 || bytes + packet->frame_len <= impl->cfg->scheduler_input_quantum_bytes)) {
        tail = packet;
        packets++;
        bytes += packet->frame_len;
        packet = packet->next_input;
    }
    impl->input_head = packet;
    if (!packet) impl->input_tail = NULL;
    tail->next_input = NULL;
    impl->input_pending_packets -= packets;
    impl->input_pending_bytes -= bytes;
    pthread_mutex_unlock(&impl->mutex);
    return head;
}

static void expire_empty_flows(struct pipeline_impl *impl, uint64_t now) {
    if (now - impl->last_expire_ns < 1000000000ULL) return;
    impl->last_expire_ns = now;
    uint64_t timeout_ns = (uint64_t)impl->cfg->reorder_flow_timeout_sec * 1000000000ULL;
    for (size_t bucket = 0; bucket < FLOW_BUCKETS; bucket++) {
        struct flow_state **link = &impl->flows[bucket];
        while (*link) {
            struct flow_state *flow = *link;
            if (!flow->pending_packets && !flow->active && flow->heap_index == HEAP_NONE &&
                now - flow->last_activity_ns >= timeout_ns) {
                *link = flow->next;
                free(flow);
                __atomic_sub_fetch(&impl->flow_count, 1, __ATOMIC_RELAXED);
            } else {
                link = &flow->next;
            }
        }
    }
}

static void make_deadline(struct timespec *deadline, uint32_t delay_ms) {
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_nsec += (long)delay_ms * 1000000L;
    deadline->tv_sec += deadline->tv_nsec / 1000000000L;
    deadline->tv_nsec %= 1000000000L;
}

static void *pipeline_worker(void *arg) {
    struct pipeline_impl *impl = arg;
    for (;;) {
        pthread_mutex_lock(&impl->mutex);
        if ((!impl->input_head || impl->reserved_packets) &&
            !impl->active_head && !impl->stopping) {
            uint64_t now = pipeline_now_ns();
            uint64_t delay_ns = 1000000000ULL;
            if (__atomic_load_n(&impl->deadline_count, __ATOMIC_RELAXED)) {
                uint64_t deadline_ns = impl->deadline_heap[0]->deadline_ns;
                delay_ns = deadline_ns > now ? deadline_ns - now : 1000000ULL;
                if (delay_ns > 1000000000ULL) delay_ns = 1000000000ULL;
            }
            uint32_t delay_ms = (uint32_t)((delay_ns + 999999ULL) / 1000000ULL);
            if (!delay_ms) delay_ms = 1;
            struct timespec deadline;
            make_deadline(&deadline, delay_ms);
            pthread_cond_timedwait(&impl->cond, &impl->mutex, &deadline);
        }
        bool stopping = impl->stopping;
        pthread_mutex_unlock(&impl->mutex);
        struct pipeline_packet *input = take_input_quantum(impl);
        uint64_t input_count = 0;
        while (input) {
            struct pipeline_packet *next = input->next_input;
            input->next_input = NULL;
            process_input(impl, input);
            input_count++;
            input = next;
        }
        uint64_t now = pipeline_now_ns();
        activate_due_deadlines(impl, now, stopping);
        uint64_t output_budget = input_count > impl->cfg->scheduler_flow_quantum_packets ?
            input_count : impl->cfg->scheduler_flow_quantum_packets;
        run_active_flows(impl, now, stopping, output_budget);
        flush_emit_batch(impl);
        expire_empty_flows(impl, now);
        if (stopping) {
            pthread_mutex_lock(&impl->mutex);
            bool input_empty = !impl->input_head;
            uint64_t reservations = impl->reserved_packets;
            pthread_mutex_unlock(&impl->mutex);
            if (input_empty && !impl->active_head &&
                !__atomic_load_n(&impl->deadline_count, __ATOMIC_RELAXED) && !reservations)
                break;
        }
    }
    return NULL;
}

static int sink_send(void *arg, const uint8_t *frame, size_t frame_len) {
    return output_send_frame(arg, frame, frame_len);
}

static int pipeline_init_common(struct output_pipeline *pipeline,
                                const struct receiver_config *cfg,
                                output_pipeline_send_fn send_fn, void *send_arg,
                                struct output_sink *batch_sink,
                                struct receiver_stats *stats,
                                struct agent_registry *registry,
                                char *err, size_t err_len) {
    if (!pipeline || !cfg || !send_fn) return -1;
    struct pipeline_impl *impl = calloc(1, sizeof(*impl));
    if (!impl) {
        snprintf(err, err_len, "output pipeline allocation failed");
        return -1;
    }
    impl->deadline_heap = calloc(cfg->reorder_max_flows, sizeof(*impl->deadline_heap));
    if (!impl->deadline_heap) {
        snprintf(err, err_len, "output scheduler deadline allocation failed");
        free(impl);
        return -1;
    }
    impl->cfg = cfg;
    impl->stats = stats;
    impl->registry = registry;
    impl->send_fn = send_fn;
    impl->send_arg = send_arg;
    impl->batch_sink = batch_sink;
    impl->last_expire_ns = pipeline_now_ns();
    if (pthread_mutex_init(&impl->mutex, NULL) != 0) {
        snprintf(err, err_len, "output pipeline synchronization init failed");
        free(impl->deadline_heap);
        free(impl);
        return -1;
    }
    if (pthread_cond_init(&impl->cond, NULL) != 0) {
        snprintf(err, err_len, "output pipeline synchronization init failed");
        pthread_mutex_destroy(&impl->mutex);
        free(impl->deadline_heap);
        free(impl);
        return -1;
    }
    if (pthread_create(&impl->worker, NULL, pipeline_worker, impl) != 0) {
        snprintf(err, err_len, "output pipeline worker creation failed");
        pthread_cond_destroy(&impl->cond);
        pthread_mutex_destroy(&impl->mutex);
        free(impl->deadline_heap);
        free(impl);
        return -1;
    }
    pipeline->impl = impl;
    return 0;
}

int output_pipeline_init(struct output_pipeline *pipeline,
                         const struct receiver_config *cfg,
                         struct output_sink *sink,
                         struct receiver_stats *stats,
                         struct agent_registry *registry,
                         char *err, size_t err_len) {
    return pipeline_init_common(pipeline, cfg, sink_send, sink, sink,
                                stats, registry, err, err_len);
}

int output_pipeline_init_custom(struct output_pipeline *pipeline,
                                const struct receiver_config *cfg,
                                output_pipeline_send_fn send_fn, void *send_arg,
                                char *err, size_t err_len) {
    return pipeline_init_common(pipeline, cfg, send_fn, send_arg, NULL,
                                NULL, NULL, err, err_len);
}

int output_pipeline_init_custom_observed(struct output_pipeline *pipeline,
                                         const struct receiver_config *cfg,
                                         output_pipeline_send_fn send_fn, void *send_arg,
                                         struct receiver_stats *stats,
                                         char *err, size_t err_len) {
    return pipeline_init_common(pipeline, cfg, send_fn, send_arg, NULL,
                                stats, NULL, err, err_len);
}

static void input_insert_ordered(struct pipeline_impl *impl,
                                 struct pipeline_packet *packet) {
    if (!impl->input_tail || impl->input_tail->arrival_id < packet->arrival_id) {
        if (impl->input_tail) impl->input_tail->next_input = packet;
        else impl->input_head = packet;
        impl->input_tail = packet;
        return;
    }
    struct pipeline_packet **link = &impl->input_head;
    while (*link && (*link)->arrival_id < packet->arrival_id)
        link = &(*link)->next_input;
    packet->next_input = *link;
    *link = packet;
}

int output_pipeline_submit(struct output_pipeline *pipeline,
                           const uint8_t *frame, uint32_t frame_len,
                           uint64_t adjusted_timestamp_ns,
                           bool timestamp_valid,
                           struct agent_metric *agent) {
    if (!pipeline || !pipeline->impl || !frame || !frame_len) return -1;
    struct pipeline_impl *impl = pipeline->impl;
    uint64_t arrival_ns = pipeline_now_ns();
    uint64_t arrival_id = 0;
    enum queue_drop_reason reason;
    pthread_mutex_lock(&impl->mutex);
    if (impl->stopping) {
        reason = QUEUE_DROP_STOPPING;
    } else if (impl->owned_packets >= impl->cfg->output_queue_max_packets) {
        reason = QUEUE_DROP_PACKET_LIMIT;
        capacity_start_locked(impl, arrival_ns);
    } else if (frame_len > impl->cfg->output_queue_max_bytes - impl->owned_bytes) {
        reason = QUEUE_DROP_BYTE_LIMIT;
        capacity_start_locked(impl, arrival_ns);
    } else {
        arrival_id = ++impl->next_arrival_id;
        impl->owned_packets++;
        impl->owned_bytes += frame_len;
        impl->reserved_packets++;
        if (impl->owned_packets > impl->peak_owned_packets)
            impl->peak_owned_packets = impl->owned_packets;
        if (impl->owned_bytes > impl->peak_owned_bytes)
            impl->peak_owned_bytes = impl->owned_bytes;
        pthread_mutex_unlock(&impl->mutex);

        struct pipeline_packet *packet = calloc(1, sizeof(*packet));
        if (packet) packet->frame = malloc(frame_len);
        if (!packet || !packet->frame) {
            pthread_mutex_lock(&impl->mutex);
            impl->reserved_packets--;
            impl->owned_packets--;
            impl->owned_bytes -= frame_len;
            pthread_cond_signal(&impl->cond);
            pthread_mutex_unlock(&impl->mutex);
            if (packet) free(packet->frame);
            free(packet);
            account_queue_drop(impl, agent, QUEUE_DROP_ALLOCATION);
            return -1;
        }
        packet->arrival_id = arrival_id;
        packet->arrival_ns = arrival_ns;
        packet->order_ns = timestamp_valid ? adjusted_timestamp_ns : arrival_ns;
        packet->frame_len = frame_len;
        packet->agent = agent;
        memcpy(packet->frame, frame, frame_len);
        pthread_mutex_lock(&impl->mutex);
        impl->reserved_packets--;
        input_insert_ordered(impl, packet);
        impl->input_pending_packets++;
        impl->input_pending_bytes += frame_len;
        if (impl->input_pending_packets > impl->peak_input_pending_packets)
            impl->peak_input_pending_packets = impl->input_pending_packets;
        if (impl->input_pending_bytes > impl->peak_input_pending_bytes)
            impl->peak_input_pending_bytes = impl->input_pending_bytes;
        pthread_cond_signal(&impl->cond);
        pthread_mutex_unlock(&impl->mutex);
        if (impl->stats) atomic_add(&impl->stats->output_pipeline_admitted, 1);
        return 0;
    }
    pthread_mutex_unlock(&impl->mutex);
    account_queue_drop(impl, agent, reason);
    return -1;
}

void output_pipeline_get_snapshot(struct output_pipeline *pipeline,
                                  struct output_pipeline_snapshot *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    if (!pipeline || !pipeline->impl) return;
    struct pipeline_impl *impl = pipeline->impl;
    pthread_mutex_lock(&impl->mutex);
    snapshot->queued_packets = impl->owned_packets;
    snapshot->queued_bytes = impl->owned_bytes;
    snapshot->peak_queued_packets = impl->peak_owned_packets;
    snapshot->peak_queued_bytes = impl->peak_owned_bytes;
    snapshot->input_pending_packets = impl->input_pending_packets;
    snapshot->input_pending_bytes = impl->input_pending_bytes;
    snapshot->peak_input_pending_packets = impl->peak_input_pending_packets;
    snapshot->peak_input_pending_bytes = impl->peak_input_pending_bytes;
    snapshot->reserved_packets = impl->reserved_packets;
    snapshot->capacity_time_ns = impl->capacity_time_ns;
    snapshot->capacity_time_max_ns = impl->capacity_time_max_ns;
    if (impl->capacity_started_ns) {
        uint64_t now = pipeline_now_ns();
        uint64_t current = now - impl->capacity_started_ns;
        snapshot->capacity_time_ns += current;
        if (current > snapshot->capacity_time_max_ns)
            snapshot->capacity_time_max_ns = current;
    }
    pthread_mutex_unlock(&impl->mutex);
    snapshot->reorder_pending_packets =
        __atomic_load_n(&impl->reorder_pending_packets, __ATOMIC_RELAXED);
    snapshot->peak_reorder_pending_packets =
        __atomic_load_n(&impl->peak_reorder_pending_packets, __ATOMIC_RELAXED);
    snapshot->active_flows = __atomic_load_n(&impl->active_count, __ATOMIC_RELAXED);
    snapshot->deadline_flows = __atomic_load_n(&impl->deadline_count, __ATOMIC_RELAXED);
    snapshot->flow_count = __atomic_load_n(&impl->flow_count, __ATOMIC_RELAXED);
}

void output_pipeline_close(struct output_pipeline *pipeline) {
    if (!pipeline || !pipeline->impl) return;
    struct pipeline_impl *impl = pipeline->impl;
    pthread_mutex_lock(&impl->mutex);
    impl->stopping = true;
    pthread_cond_broadcast(&impl->cond);
    pthread_mutex_unlock(&impl->mutex);
    pthread_join(impl->worker, NULL);
    for (size_t bucket = 0; bucket < FLOW_BUCKETS; bucket++) {
        struct flow_state *flow = impl->flows[bucket];
        while (flow) {
            struct flow_state *next = flow->next;
            free(flow);
            flow = next;
        }
    }
    pthread_cond_destroy(&impl->cond);
    pthread_mutex_destroy(&impl->mutex);
    free(impl->deadline_heap);
    free(impl);
    pipeline->impl = NULL;
}
